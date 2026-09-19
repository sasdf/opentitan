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

/**
 * Check 5 (Wave 2): Effective TX FIFO capacity is `TxDepth + 1 = 73` words
 * (`72` words in `u_tx_fifo` + `1` word greedily popped into
 * `spi_host_byte_select` (`u_select`) even when `CONTROL.SPIEN == 0`).
 * See `spi_host_data_fifos.sv:71-73, 124` and `spi_host_core.sv:78-96`.
 * - After 72 writes to `TXDATA`: `STATUS.TXQD == 72`, `STATUS.TXFULL == 0`.
 * - After 73rd write to `TXDATA`: `STATUS.TXQD == 73`, `STATUS.TXFULL == 1`,
 *   `ERROR_STATUS.OVERFLOW == 0`.
 * - After 74th write to `TXDATA`: `ERROR_STATUS.OVERFLOW == 1`.
 */
static void test_tx_fifo_plus_byte_select_depth_73(uint32_t base) {
  abs_mmio_write32(base + SPI_HOST_CONTROL_REG_OFFSET,
                   (1u << SPI_HOST_CONTROL_SW_RST_BIT) | 0x7fu);
  abs_mmio_write32(base + SPI_HOST_CONTROL_REG_OFFSET, 0x7fu);
  abs_mmio_write32(base + SPI_HOST_ERROR_STATUS_REG_OFFSET, 0x3fu);

  for (uint32_t i = 0; i < 72; ++i) {
    abs_mmio_write32(base + SPI_HOST_TXDATA_REG_OFFSET, 0x1000u + i);
  }

  uint32_t status72 = abs_mmio_read32(base + SPI_HOST_STATUS_REG_OFFSET);
  uint32_t txqd72 = bitfield_field32_read(status72, SPI_HOST_STATUS_TXQD_FIELD);
  bool txfull72 = bitfield_bit32_read(status72, SPI_HOST_STATUS_TXFULL_BIT);
  EXPECT_CHECK(txqd72 == 72u && !txfull72,
               "After 72 TXDATA writes, TXQD must be 72 and TXFULL must be 0 "
               "(effective capacity is TxDepth+1 = 73), got STATUS=0x%08x "
               "(TXQD=%u, TXFULL=%u)",
               status72, txqd72, txfull72);

  // 73rd write fills u_tx_fifo (72) + u_select (1) = 73 words without overflow.
  abs_mmio_write32(base + SPI_HOST_TXDATA_REG_OFFSET, 0x1048u);
  uint32_t status73 = abs_mmio_read32(base + SPI_HOST_STATUS_REG_OFFSET);
  uint32_t txqd73 = bitfield_field32_read(status73, SPI_HOST_STATUS_TXQD_FIELD);
  bool txfull73 = bitfield_bit32_read(status73, SPI_HOST_STATUS_TXFULL_BIT);
  uint32_t err73 = abs_mmio_read32(base + SPI_HOST_ERROR_STATUS_REG_OFFSET);
  EXPECT_CHECK(txqd73 == 73u && txfull73 &&
                   !bitfield_bit32_read(err73, SPI_HOST_ERROR_STATUS_OVERFLOW_BIT),
               "After 73rd TXDATA write, TXQD must be 73, TXFULL=1, OVERFLOW=0, "
               "got STATUS=0x%08x (TXQD=%u, TXFULL=%u), ERROR_STATUS=0x%08x",
               status73, txqd73, txfull73, err73);

  // 74th write overflows the TX FIFO + byte_select pipeline.
  abs_mmio_write32(base + SPI_HOST_TXDATA_REG_OFFSET, 0x1049u);
  uint32_t err74 = abs_mmio_read32(base + SPI_HOST_ERROR_STATUS_REG_OFFSET);
  EXPECT_CHECK(bitfield_bit32_read(err74, SPI_HOST_ERROR_STATUS_OVERFLOW_BIT),
               "74th TXDATA write must set ERROR_STATUS.OVERFLOW=1, got 0x%08x",
               err74);

  // Clean up via SW_RST and clear ERROR_STATUS.
  abs_mmio_write32(base + SPI_HOST_CONTROL_REG_OFFSET,
                   (1u << SPI_HOST_CONTROL_SW_RST_BIT) | 0x7fu);
  abs_mmio_write32(base + SPI_HOST_CONTROL_REG_OFFSET, 0x7fu);
  abs_mmio_write32(base + SPI_HOST_ERROR_STATUS_REG_OFFSET, 0x3fu);
}

/**
 * Check 6 (Wave 2): When `CONTROL.SPIEN == 1`, `CONFIGOPTS == 0`
 * (`config_changed == 0`, `CPHA == 0`), and `TXFIFO` is empty, issuing a
 * `WrOnly` (`DIRECTION = 2`) command causes `spi_host_fsm.sv` (`Idle ->
 * WaitLead`) to combinationally assert `byte_starting_cpha0 = 1`,
 * `wr_en_internal = 1`, `tx_stall_o = 1`, and `stall = 1` while `state_q` is
 * still `Idle`:
 * - `command_ready_o = command_ready_int & ~stall = 0`, so `u_cmd_queue` does
 *   NOT pop the command (`STATUS.CMDQD == 1`).
 * - `state_q` remains `Idle`, so `STATUS.ACTIVE == 0` while `STATUS.TXSTALL == 1`.
 */
static void test_wronly_txstall_in_idle_cmdqd_and_active(uint32_t base) {
  abs_mmio_write32(base + SPI_HOST_CONTROL_REG_OFFSET,
                   (1u << SPI_HOST_CONTROL_SW_RST_BIT) | 0x7fu);
  abs_mmio_write32(base + SPI_HOST_CONFIGOPTS_REG_OFFSET, 0u);
  abs_mmio_write32(base + SPI_HOST_CSID_REG_OFFSET, 0u);
  abs_mmio_write32(base + SPI_HOST_ERROR_STATUS_REG_OFFSET, 0x3fu);
  abs_mmio_write32(base + SPI_HOST_CONTROL_REG_OFFSET,
                   (1u << SPI_HOST_CONTROL_SPIEN_BIT) | 0x7fu);

  uint32_t wronly_cmd =
      bitfield_field32_write(0u, SPI_HOST_COMMAND_DIRECTION_FIELD, 2u) |
      bitfield_field32_write(0u, SPI_HOST_COMMAND_SPEED_FIELD, 0u) |
      bitfield_field32_write(0u, SPI_HOST_COMMAND_LEN_FIELD, 0u);
  abs_mmio_write32(base + SPI_HOST_COMMAND_REG_OFFSET, wronly_cmd);

  uint32_t status = abs_mmio_read32(base + SPI_HOST_STATUS_REG_OFFSET);
  uint32_t cmdqd = bitfield_field32_read(status, SPI_HOST_STATUS_CMDQD_FIELD);
  bool active = bitfield_bit32_read(status, SPI_HOST_STATUS_ACTIVE_BIT);
  bool txstall = bitfield_bit32_read(status, SPI_HOST_STATUS_TXSTALL_BIT);
  EXPECT_CHECK(
      cmdqd == 1u && !active && txstall,
      "WrOnly command with empty TXFIFO and CONFIGOPTS=0 must stall in Idle "
      "(CMDQD=1, ACTIVE=0, TXSTALL=1), got STATUS=0x%08x (CMDQD=%u, ACTIVE=%u, "
      "TXSTALL=%u)",
      status, cmdqd, active, txstall);

  // Clean up via SW_RST.
  abs_mmio_write32(base + SPI_HOST_CONTROL_REG_OFFSET,
                   (1u << SPI_HOST_CONTROL_SW_RST_BIT) | 0x7fu);
  abs_mmio_write32(base + SPI_HOST_CONTROL_REG_OFFSET, 0x7fu);
}

/**
 * Check 7 (Wave 2): Reading `STATUS` (`SwAccessRO`) has no side effect on
 * `u_fsm` timing. With `CONFIGOPTS.CLKDIV = 0xffff` (131,072 core cycles per
 * SPI clock period) and a 64-cycle dummy command (`DIRECTION = 0, LEN = 63`),
 * `STATUS.ACTIVE` must remain `1` across 3 consecutive `STATUS` reads
 * (`ot_spi_host.c:1299-1305` erroneously deletes `s->fsm_delay` on every
 * `STATUS` read, retiring the command on the 2nd read).
 */
static void test_status_read_does_not_cancel_clkdiv_active(uint32_t base) {
  abs_mmio_write32(base + SPI_HOST_CONTROL_REG_OFFSET,
                   (1u << SPI_HOST_CONTROL_SW_RST_BIT) | 0x7fu);
  abs_mmio_write32(base + SPI_HOST_CONFIGOPTS_REG_OFFSET, 0xffffu);
  abs_mmio_write32(base + SPI_HOST_CSID_REG_OFFSET, 0u);
  abs_mmio_write32(base + SPI_HOST_ERROR_STATUS_REG_OFFSET, 0x3fu);
  abs_mmio_write32(base + SPI_HOST_CONTROL_REG_OFFSET,
                   (1u << SPI_HOST_CONTROL_SPIEN_BIT) | 0x7fu);

  uint32_t slow_dummy_cmd =
      bitfield_field32_write(0u, SPI_HOST_COMMAND_DIRECTION_FIELD, 0u) |
      bitfield_field32_write(0u, SPI_HOST_COMMAND_SPEED_FIELD, 0u) |
      bitfield_field32_write(0u, SPI_HOST_COMMAND_LEN_FIELD, 63u);
  abs_mmio_write32(base + SPI_HOST_COMMAND_REG_OFFSET, slow_dummy_cmd);

  uint32_t s1 = abs_mmio_read32(base + SPI_HOST_STATUS_REG_OFFSET);
  uint32_t s2 = abs_mmio_read32(base + SPI_HOST_STATUS_REG_OFFSET);
  uint32_t s3 = abs_mmio_read32(base + SPI_HOST_STATUS_REG_OFFSET);
  bool active3 = bitfield_bit32_read(s3, SPI_HOST_STATUS_ACTIVE_BIT);
  EXPECT_CHECK(
      active3,
      "STATUS.ACTIVE must remain 1 across 3 immediate STATUS reads when "
      "CONFIGOPTS.CLKDIV=0xffff (s1=0x%08x, s2=0x%08x, s3=0x%08x)",
      s1, s2, s3);

  // Clean up via SW_RST and restore CONFIGOPTS=0.
  abs_mmio_write32(base + SPI_HOST_CONTROL_REG_OFFSET,
                   (1u << SPI_HOST_CONTROL_SW_RST_BIT) | 0x7fu);
  abs_mmio_write32(base + SPI_HOST_CONFIGOPTS_REG_OFFSET, 0u);
  abs_mmio_write32(base + SPI_HOST_CONTROL_REG_OFFSET, 0x7fu);
}

bool test_main(void) {
  CHECK_DIF_OK(dif_alert_handler_init(
      mmio_region_from_addr(kAlertHandlerBase), &alert_handler));

  LOG_INFO("Running spi_host_rtl_consistency_test...");
  test_alert_test_pulse();
  test_configopts_reserved_mask(kSpiHost0Base);
  test_sw_rst_level_behavior(kSpiHost0Base);
  test_command_queue_and_errors_while_spien_disabled(kSpiHost0Base);
  test_tx_fifo_plus_byte_select_depth_73(kSpiHost0Base);
  test_wronly_txstall_in_idle_cmdqd_and_active(kSpiHost0Base);
  test_status_read_does_not_cancel_clkdiv_active(kSpiHost0Base);
  LOG_INFO("spi_host_rtl_consistency_test finished with %u failure(s).",
           g_failures);
  return g_failures == 0;
}

