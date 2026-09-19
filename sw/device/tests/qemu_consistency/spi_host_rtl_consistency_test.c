// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_alert_handler.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "spi_host_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kSpiHost0Base = TOP_EARLGREY_SPI_HOST0_BASE_ADDR,
  kSpiHost1Base = TOP_EARLGREY_SPI_HOST1_BASE_ADDR,
  kAlertHandlerBase = TOP_EARLGREY_ALERT_HANDLER_BASE_ADDR,
};

static dif_alert_handler_t alert_handler;
static uint32_t g_failures = 0;

#define EXPECT_CHECK(cond, ...)     \
  do {                              \
    if (!(cond)) {                  \
      LOG_ERROR(__VA_ARGS__);       \
      ++g_failures;                 \
    }                               \
  } while (0)

/**
 * Check 1: SPI_HOST0 and SPI_HOST1 ALERT_TEST must pulse alert_tx_o 1 -> 0
 * rather than latching it high forever (`spi_host.sv:68-87` vs
 * `ot_spi_host.c:790-798, 1421-1425`).
 */
static void test_alert_test_pulse(void) {
  const dif_alert_handler_alert_t kAlerts[2] = {
      kTopEarlgreyAlertIdSpiHost0FatalFault,
      kTopEarlgreyAlertIdSpiHost1FatalFault,
  };
  const uint32_t kBases[2] = {kSpiHost0Base, kSpiHost1Base};

  for (size_t i = 0; i < 2; ++i) {
    CHECK_DIF_OK(dif_alert_handler_configure_alert(
        &alert_handler, kAlerts[i], kDifAlertHandlerClassA,
        /*enabled=*/kDifToggleEnabled, /*locked=*/kDifToggleDisabled));
  }

  for (size_t i = 0; i < 2; ++i) {
    abs_mmio_write32(kBases[i] + SPI_HOST_ALERT_TEST_REG_OFFSET,
                     1u << SPI_HOST_ALERT_TEST_FATAL_FAULT_BIT);
    busy_spin_micros(10);

    bool is_cause = false;
    CHECK_DIF_OK(
        dif_alert_handler_alert_is_cause(&alert_handler, kAlerts[i], &is_cause));
    EXPECT_CHECK(is_cause, "SPI_HOST%u ALERT_TEST should set ALERT_CAUSE", i);

    CHECK_DIF_OK(dif_alert_handler_alert_acknowledge(&alert_handler, kAlerts[i]));
    busy_spin_micros(10);

    is_cause = false;
    CHECK_DIF_OK(
        dif_alert_handler_alert_is_cause(&alert_handler, kAlerts[i], &is_cause));
    EXPECT_CHECK(
        !is_cause,
        "SPI_HOST%u ALERT_CAUSE should remain 0 after clearing (ALERT_TEST "
        "is a single-cycle pulse in spi_host.sv:68-87)",
        i);
  }
}

/**
 * Check 2: `CONFIGOPTS` bit 28 is reserved/unimplemented (`spi_host.hjson:320-385`),
 * so writing `0xffffffff` must read back `0xefffffff`.
 */
static void test_configopts_reserved_mask(uint32_t base) {
  abs_mmio_write32(base + SPI_HOST_CONFIGOPTS_REG_OFFSET, 0xffffffffu);
  uint32_t configopts = abs_mmio_read32(base + SPI_HOST_CONFIGOPTS_REG_OFFSET);
  EXPECT_CHECK(configopts == 0xefffffffu,
               "CONFIGOPTS bit 28 is reserved/unimplemented (mask 0xefffffff), "
               "got 0x%08x",
               configopts);
  abs_mmio_write32(base + SPI_HOST_CONFIGOPTS_REG_OFFSET, 0u);
}

/**
 * Check 3: `CONTROL.SW_RST` (`spi_host.sv:285, 425, 433, 461`) is a level reset
 * wired directly to `u_tx_fifo.clr_i` and `u_cmd_queue.clr_i`. While
 * `CONTROL.SW_RST == 1`, writes to `TXDATA` must not accumulate in the TX FIFO
 * (`STATUS.TXQD` must stay 0 and `STATUS.TXEMPTY` must stay 1).
 */
static void test_sw_rst_level_behavior(uint32_t base) {
  // Hold SW_RST = 1 (bit 30), RX_WATERMARK = 0x7f.
  abs_mmio_write32(base + SPI_HOST_CONTROL_REG_OFFSET,
                   (1u << SPI_HOST_CONTROL_SW_RST_BIT) | 0x7fu);

  // Write TXDATA while SW_RST == 1.
  abs_mmio_write32(base + SPI_HOST_TXDATA_REG_OFFSET, 0xdeadbeefu);

  uint32_t status = abs_mmio_read32(base + SPI_HOST_STATUS_REG_OFFSET);
  uint32_t txqd =
      bitfield_field32_read(status, SPI_HOST_STATUS_TXQD_FIELD);
  bool txempty = bitfield_bit32_read(status, SPI_HOST_STATUS_TXEMPTY_BIT);
  EXPECT_CHECK(
      txqd == 0u && txempty,
      "While CONTROL.SW_RST == 1, u_tx_fifo is held in synchronous reset "
      "(TXQD must be 0 and TXEMPTY must be 1), got STATUS=0x%08x (TXQD=%u)",
      status, txqd);

  // Deassert SW_RST = 0.
  abs_mmio_write32(base + SPI_HOST_CONTROL_REG_OFFSET, 0x7fu);
}

/**
 * Check 4: `COMMAND` writes when `CONTROL.SPIEN == 0` (`spi_host.sv:212-215,
 * 252, 272-286, 432-445` vs `ot_spi_host.c:1452-1457`).
 * In RTL, `command_valid = |cmd_qes` feeds `u_cmd_queue` (`CmdDepth = 4`) and
 * error logic (`CMDINVAL`, `CSIDINVAL`, `CMDBUSY`) even while `CONTROL.SPIEN == 0`
 * (`u_spi_core` simply waits to pop `u_cmd_queue` until `SPIEN` is set to 1).
 */
static void test_command_queue_and_errors_while_spien_disabled(uint32_t base) {
  // Ensure clean state via SW_RST, then keep SPIEN = 0, SW_RST = 0.
  abs_mmio_write32(base + SPI_HOST_CONTROL_REG_OFFSET,
                   (1u << SPI_HOST_CONTROL_SW_RST_BIT) | 0x7fu);
  abs_mmio_write32(base + SPI_HOST_CONTROL_REG_OFFSET, 0x7fu);
  abs_mmio_write32(base + SPI_HOST_ERROR_STATUS_REG_OFFSET, 0x3fu);
  abs_mmio_write32(base + SPI_HOST_CSID_REG_OFFSET, 0u);

  // 4a. Invalid SPEED = 3 (reserved) with SPIEN == 0 must set ERROR_STATUS.CMDINVAL.
  uint32_t invalid_speed_cmd =
      bitfield_field32_write(0u, SPI_HOST_COMMAND_SPEED_FIELD, 3u) |
      bitfield_field32_write(0u, SPI_HOST_COMMAND_DIRECTION_FIELD, 0u);
  abs_mmio_write32(base + SPI_HOST_COMMAND_REG_OFFSET, invalid_speed_cmd);

  uint32_t err_status =
      abs_mmio_read32(base + SPI_HOST_ERROR_STATUS_REG_OFFSET);
  EXPECT_CHECK(
      bitfield_bit32_read(err_status, SPI_HOST_ERROR_STATUS_CMDINVAL_BIT),
      "COMMAND with SPEED=3 while SPIEN==0 must set ERROR_STATUS.CMDINVAL, "
      "got ERROR_STATUS=0x%08x",
      err_status);

  // Reset queue & clear ERROR_STATUS.
  abs_mmio_write32(base + SPI_HOST_CONTROL_REG_OFFSET,
                   (1u << SPI_HOST_CONTROL_SW_RST_BIT) | 0x7fu);
  abs_mmio_write32(base + SPI_HOST_CONTROL_REG_OFFSET, 0x7fu);
  abs_mmio_write32(base + SPI_HOST_ERROR_STATUS_REG_OFFSET, 0x3fu);

  // 4b. Invalid CSID = 1 (NumCS = 1) with SPIEN == 0 must set ERROR_STATUS.CSIDINVAL.
  abs_mmio_write32(base + SPI_HOST_CSID_REG_OFFSET, 1u);
  uint32_t dummy_cmd =
      bitfield_field32_write(0u, SPI_HOST_COMMAND_DIRECTION_FIELD, 0u) |
      bitfield_field32_write(0u, SPI_HOST_COMMAND_SPEED_FIELD, 0u) |
      bitfield_field32_write(0u, SPI_HOST_COMMAND_LEN_FIELD, 3u);
  abs_mmio_write32(base + SPI_HOST_COMMAND_REG_OFFSET, dummy_cmd);

  err_status = abs_mmio_read32(base + SPI_HOST_ERROR_STATUS_REG_OFFSET);
  EXPECT_CHECK(
      bitfield_bit32_read(err_status, SPI_HOST_ERROR_STATUS_CSIDINVAL_BIT),
      "COMMAND with CSID=1 (NumCS=1) while SPIEN==0 must set "
      "ERROR_STATUS.CSIDINVAL, got ERROR_STATUS=0x%08x",
      err_status);

  // Reset queue, clear ERROR_STATUS, restore CSID = 0.
  abs_mmio_write32(base + SPI_HOST_CSID_REG_OFFSET, 0u);
  abs_mmio_write32(base + SPI_HOST_CONTROL_REG_OFFSET,
                   (1u << SPI_HOST_CONTROL_SW_RST_BIT) | 0x7fu);
  abs_mmio_write32(base + SPI_HOST_CONTROL_REG_OFFSET, 0x7fu);
  abs_mmio_write32(base + SPI_HOST_ERROR_STATUS_REG_OFFSET, 0x3fu);

  // 4c. Enqueue 4 valid commands while SPIEN == 0: CMDQD must increment 1..4
  // and STATUS.READY must drop to 0 when CMDQD == 4 (CmdDepth = 4).
  for (uint32_t i = 1; i <= 4; ++i) {
    abs_mmio_write32(base + SPI_HOST_COMMAND_REG_OFFSET, dummy_cmd);
    uint32_t status = abs_mmio_read32(base + SPI_HOST_STATUS_REG_OFFSET);
    uint32_t cmdqd =
        bitfield_field32_read(status, SPI_HOST_STATUS_CMDQD_FIELD);
    bool ready = bitfield_bit32_read(status, SPI_HOST_STATUS_READY_BIT);
    EXPECT_CHECK(
        cmdqd == i,
        "After %u COMMAND writes with SPIEN==0, STATUS.CMDQD must be %u, "
        "got STATUS=0x%08x (CMDQD=%u)",
        i, i, status, cmdqd);
    if (i < 4) {
      EXPECT_CHECK(ready, "STATUS.READY must remain 1 when CMDQD=%u < 4", i);
    } else {
      EXPECT_CHECK(!ready,
                   "STATUS.READY must drop to 0 when CMDQD == 4 (CmdDepth=4)");
    }
  }

  // 4d. Writing a 5th command when CMDQD == 4 (STATUS.READY == 0) must set
  // ERROR_STATUS.CMDBUSY = 1.
  abs_mmio_write32(base + SPI_HOST_COMMAND_REG_OFFSET, dummy_cmd);
  err_status = abs_mmio_read32(base + SPI_HOST_ERROR_STATUS_REG_OFFSET);
  EXPECT_CHECK(
      bitfield_bit32_read(err_status, SPI_HOST_ERROR_STATUS_CMDBUSY_BIT),
      "Writing 5th COMMAND when u_cmd_queue is full (CMDQD==4) must set "
      "ERROR_STATUS.CMDBUSY, got ERROR_STATUS=0x%08x",
      err_status);

  // Clean up via SW_RST and clear ERROR_STATUS.
  abs_mmio_write32(base + SPI_HOST_CONTROL_REG_OFFSET,
                   (1u << SPI_HOST_CONTROL_SW_RST_BIT) | 0x7fu);
  abs_mmio_write32(base + SPI_HOST_CONTROL_REG_OFFSET, 0x7fu);
  abs_mmio_write32(base + SPI_HOST_ERROR_STATUS_REG_OFFSET, 0x3fu);
}

bool test_main(void) {
  CHECK_DIF_OK(dif_alert_handler_init(
      mmio_region_from_addr(kAlertHandlerBase), &alert_handler));

  LOG_INFO("Running spi_host_rtl_consistency_test...");
  test_alert_test_pulse();
  test_configopts_reserved_mask(kSpiHost0Base);
  test_sw_rst_level_behavior(kSpiHost0Base);
  test_command_queue_and_errors_while_spien_disabled(kSpiHost0Base);
  LOG_INFO("spi_host_rtl_consistency_test finished with %u failure(s).",
           g_failures);
  return g_failures == 0;
}
