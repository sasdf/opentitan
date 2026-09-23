// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file spi_host_errata_test.c
 * @brief CW340 FPGA & QEMU Errata Confirmation Suite for `spi_host` (P30).
 *
 * Empirically confirms on both physical CW340 FPGA silicon and QEMU:
 * 1. [spi_host_data_fifos.sv:71-73,122-125] ([E1-01] SPEC_DOC_ERRATA, MEDIUM):
 *    - 73-word effective `TXDATA` queue depth (`TxDepth + 1 = 73` due to greedy
 *      pop into `u_select` / `spi_host_byte_select`):
 *      * 72 words -> `STATUS.TXQD == 72`, `STATUS.TXFULL == 0`, `OVERFLOW ==
 * 0`.
 *      * 73rd word -> `STATUS.TXQD == 73`, `STATUS.TXFULL == 1`, `OVERFLOW ==
 * 0`.
 *      * 74th word -> `ERROR_STATUS.OVERFLOW == 1`.
 *    - Combinational `STATUS.TXSTALL == 1` with `STATUS.ACTIVE == 0` and
 *      `STATUS.CMDQD == 1` in `Idle` when a `WrOnly` (`DIRECTION = 2`) command
 *      is queued with `CONTROL.SPIEN = 1`, `CONFIGOPTS = 0`, and an empty TX
 *      FIFO.
 * 2. [spi_host.sv:212-215,252,272-286,425-461] ([E2-01] SPEC_DOC_ERRATA, LOW):
 *    - Level-sensitive `CONTROL.SW_RST = 1` continuously discards `TXDATA` and
 *      `COMMAND` writes (`STATUS.TXQD == 0`, `STATUS.CMDQD == 0`).
 *    - When `CONTROL.SW_RST = 0` and `CONTROL.SPIEN = 0`, `COMMAND` writes are
 *      still validated and queued into `u_cmd_queue` (`STATUS.CMDQD` reaches 4,
 *      `STATUS.READY` becomes 0), and a 5th invalid command sets
 *      `ERROR_STATUS.CMD_BUSY = 1` while masking `CMDINVAL` (`~command_busy`).
 * 3. [spi_host_reg_pkg.sv:329-366] ([E2-02] INTENDED_SECURITY_HARDENING, INFO —
 * SEC_CM: BUS.INTEGRITY):
 *    - `SPI_HOST_PERMIT` rejects `sb` writes to `CONTROL` (`4'b1111`,
 * `mcause=7`) and `COMMAND` (`4'b0011`, `mcause=7`), whereas `sb` writes to
 * `TXDATA`
 *      (`0x28`, `.ByteAccess(1)`) succeed (`TXQD == 1`).
 *    - Writes to `RXDATA` (`0x24`) fault with `mcause = 7`, and reads from
 *      `TXDATA` (`0x28`) fault with `mcause = 5`, while reads from `COMMAND`
 *      (`0x20`, `wo`) return `0` without fault.
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "spi_host_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kSpiHostBase = TOP_EARLGREY_SPI_HOST0_BASE_ADDR,
};

static volatile bool g_load_store_fault = false;
static volatile uint32_t g_last_mcause = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  g_load_store_fault = true;
  g_last_mcause = ibex_mcause_read();
}

static void spi_host_sw_reset_and_clear(void) {
  abs_mmio_write32(kSpiHostBase + SPI_HOST_CONTROL_REG_OFFSET,
                   1u << SPI_HOST_CONTROL_SW_RST_BIT);
  abs_mmio_write32(kSpiHostBase + SPI_HOST_CONTROL_REG_OFFSET, 0u);
  abs_mmio_write32(kSpiHostBase + SPI_HOST_ERROR_STATUS_REG_OFFSET, 0x3fu);
  abs_mmio_write32(kSpiHostBase + SPI_HOST_INTR_STATE_REG_OFFSET, 0x3u);
}

bool test_main(void) {
  LOG_INFO(
      "=== OpenTitan Earlgrey SPI_HOST Errata Confirmation Suite (P30) ===");
  CHECK_STATUS_OK(
      ottf_alerts_ignore_alert(kTopEarlgreyAlertIdSpiHost0FatalFault));

  spi_host_sw_reset_and_clear();

  // ---------------------------------------------------------------------------
  // 1. [spi_host_data_fifos.sv:71-73,122-125] ([E1-01] SPEC_DOC_ERRATA,
  // MEDIUM):
  //    - 73-word effective TXDATA queue depth (TxDepth=72 + 1 word in
  //    u_select).
  //    - Combinational STATUS.TXSTALL=1 with STATUS.ACTIVE=0 and STATUS.CMDQD=1
  //      in Idle when a WrOnly command is issued with an empty TX FIFO.
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [spi_host_data_fifos.sv:71-73,122-125] (SPEC_DOC_ERRATA): "
      "73-word TXDATA depth "
      "& combinational Idle TXSTALL");
  for (uint32_t i = 0; i < 72; ++i) {
    abs_mmio_write32(kSpiHostBase + SPI_HOST_TXDATA_REG_OFFSET, 0x1000u + i);
  }
  uint32_t status = abs_mmio_read32(kSpiHostBase + SPI_HOST_STATUS_REG_OFFSET);
  uint32_t err_sts =
      abs_mmio_read32(kSpiHostBase + SPI_HOST_ERROR_STATUS_REG_OFFSET);
  uint32_t txqd = bitfield_field32_read(status, SPI_HOST_STATUS_TXQD_FIELD);
  bool txfull = bitfield_bit32_read(status, SPI_HOST_STATUS_TXFULL_BIT);
  CHECK(txqd == 72u && !txfull && err_sts == 0u,
        "[spi_host_data_fifos.sv:71-73,122-125] After 72 words expected "
        "TXQD=72, TXFULL=0, "
        "ERROR_STATUS=0 (got TXQD=%u, TXFULL=%d, err=0x%x)",
        txqd, txfull, err_sts);

  // 73rd word succeeds and sets TXQD = 73, TXFULL = 1, OVERFLOW = 0!
  abs_mmio_write32(kSpiHostBase + SPI_HOST_TXDATA_REG_OFFSET, 0x1048u);
  status = abs_mmio_read32(kSpiHostBase + SPI_HOST_STATUS_REG_OFFSET);
  err_sts = abs_mmio_read32(kSpiHostBase + SPI_HOST_ERROR_STATUS_REG_OFFSET);
  txqd = bitfield_field32_read(status, SPI_HOST_STATUS_TXQD_FIELD);
  txfull = bitfield_bit32_read(status, SPI_HOST_STATUS_TXFULL_BIT);
  CHECK(txqd == 73u && txfull && err_sts == 0u,
        "[spi_host_data_fifos.sv:71-73,122-125] After 73rd word expected "
        "TXQD=73, TXFULL=1, "
        "ERROR_STATUS=0 (got TXQD=%u, TXFULL=%d, err=0x%x)",
        txqd, txfull, err_sts);

  // 74th word triggers ERROR_STATUS.OVERFLOW (bit 0)!
  abs_mmio_write32(kSpiHostBase + SPI_HOST_TXDATA_REG_OFFSET, 0x1049u);
  err_sts = abs_mmio_read32(kSpiHostBase + SPI_HOST_ERROR_STATUS_REG_OFFSET);
  CHECK(
      bitfield_bit32_read(err_sts, SPI_HOST_ERROR_STATUS_OVERFLOW_BIT),
      "[spi_host_data_fifos.sv:71-73,122-125] Expected 74th TXDATA word to set "
      "ERROR_STATUS.OVERFLOW=1, got 0x%x",
      err_sts);

  // Reset FIFOs, set CONFIGOPTS=0, CSID=0, CONTROL.SPIEN=1, and issue a WrOnly
  // command (DIRECTION=2, LEN=0) with an empty TX FIFO:
  spi_host_sw_reset_and_clear();
  abs_mmio_write32(kSpiHostBase + SPI_HOST_CONFIGOPTS_REG_OFFSET, 0u);
  abs_mmio_write32(kSpiHostBase + SPI_HOST_CSID_REG_OFFSET, 0u);
  abs_mmio_write32(kSpiHostBase + SPI_HOST_CONTROL_REG_OFFSET,
                   1u << SPI_HOST_CONTROL_SPIEN_BIT);
  uint32_t cmd_wronly = (2u << SPI_HOST_COMMAND_DIRECTION_OFFSET) |
                        (0u << SPI_HOST_COMMAND_SPEED_OFFSET) |
                        (0u << SPI_HOST_COMMAND_LEN_OFFSET);
  abs_mmio_write32(kSpiHostBase + SPI_HOST_COMMAND_REG_OFFSET, cmd_wronly);
  status = abs_mmio_read32(kSpiHostBase + SPI_HOST_STATUS_REG_OFFSET);
  bool txstall = bitfield_bit32_read(status, SPI_HOST_STATUS_TXSTALL_BIT);
  bool active = bitfield_bit32_read(status, SPI_HOST_STATUS_ACTIVE_BIT);
  uint32_t cmdqd = bitfield_field32_read(status, SPI_HOST_STATUS_CMDQD_FIELD);
  CHECK(txstall && !active && cmdqd == 1u,
        "[spi_host_data_fifos.sv:71-73,122-125] Expected combinational "
        "TXSTALL=1, ACTIVE=0, "
        "CMDQD=1 in Idle on empty-TX WrOnly command (status=0x%08x)",
        status);
  LOG_INFO(
      "[spi_host_data_fifos.sv:71-73,122-125] CONFIRMED: TXQD=73 before "
      "OVERFLOW, and Idle "
      "TXSTALL=1 with ACTIVE=0 & CMDQD=1");

  // ---------------------------------------------------------------------------
  // 2. [spi_host.sv:212-215,252,272-286,425-461] ([E2-01] SPEC_DOC_ERRATA,
  // LOW):
  //    - Level-sensitive CONTROL.SW_RST = 1 discards TXDATA and COMMAND writes.
  //    - When CONTROL.SPIEN = 0, COMMAND writes are queued (CMDQD = 4,
  //      READY = 0) and validated, and a 5th invalid command sets CMD_BUSY = 1
  //      while masking CMDINVAL (~command_busy).
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [spi_host.sv:212-215,252,272-286,425-461] (SPEC_DOC_ERRATA): "
      "level-sensitive "
      "SW_RST & SPIEN=0 COMMAND queueing/error masking");
  abs_mmio_write32(kSpiHostBase + SPI_HOST_CONTROL_REG_OFFSET,
                   1u << SPI_HOST_CONTROL_SW_RST_BIT);
  abs_mmio_write32(kSpiHostBase + SPI_HOST_TXDATA_REG_OFFSET, 0xdeadbeefu);
  uint32_t cmd_rdonly = (1u << SPI_HOST_COMMAND_DIRECTION_OFFSET);
  abs_mmio_write32(kSpiHostBase + SPI_HOST_COMMAND_REG_OFFSET, cmd_rdonly);
  status = abs_mmio_read32(kSpiHostBase + SPI_HOST_STATUS_REG_OFFSET);
  CHECK(bitfield_field32_read(status, SPI_HOST_STATUS_TXQD_FIELD) == 0u &&
            bitfield_field32_read(status, SPI_HOST_STATUS_CMDQD_FIELD) == 0u,
        "[spi_host.sv:212-215,252,272-286,425-461] Expected TXDATA & COMMAND "
        "writes while SW_RST=1 "
        "to be discarded (status=0x%08x)",
        status);

  // Deassert SW_RST (leaving SPIEN = 0):
  abs_mmio_write32(kSpiHostBase + SPI_HOST_CONTROL_REG_OFFSET, 0u);
  abs_mmio_write32(kSpiHostBase + SPI_HOST_ERROR_STATUS_REG_OFFSET, 0x3fu);

  // Queue 4 valid commands while SPIEN == 0:
  for (int i = 0; i < 4; ++i) {
    abs_mmio_write32(kSpiHostBase + SPI_HOST_COMMAND_REG_OFFSET, cmd_rdonly);
  }
  status = abs_mmio_read32(kSpiHostBase + SPI_HOST_STATUS_REG_OFFSET);
  CHECK(bitfield_field32_read(status, SPI_HOST_STATUS_CMDQD_FIELD) == 4u &&
            !bitfield_bit32_read(status, SPI_HOST_STATUS_READY_BIT),
        "[spi_host.sv:212-215,252,272-286,425-461] Expected CMDQD=4 and "
        "READY=0 when 4 commands "
        "are queued with SPIEN=0 (status=0x%08x)",
        status);

  // Write a 5th command that is ALSO invalid (SPEED=2 Dual + DIRECTION=3 RdWr)
  // while CMDQD == 4 (command_busy == 1): CMD_BUSY asserts, while CMDINVAL is
  // masked by ~command_busy!
  uint32_t cmd_inval_dual_rdwr = (3u << SPI_HOST_COMMAND_DIRECTION_OFFSET) |
                                 (2u << SPI_HOST_COMMAND_SPEED_OFFSET);
  abs_mmio_write32(kSpiHostBase + SPI_HOST_COMMAND_REG_OFFSET,
                   cmd_inval_dual_rdwr);
  err_sts = abs_mmio_read32(kSpiHostBase + SPI_HOST_ERROR_STATUS_REG_OFFSET);
  CHECK(bitfield_bit32_read(err_sts, SPI_HOST_ERROR_STATUS_CMDBUSY_BIT) &&
            !bitfield_bit32_read(err_sts, SPI_HOST_ERROR_STATUS_CMDINVAL_BIT),
        "[spi_host.sv:212-215,252,272-286,425-461] Expected 5th invalid "
        "command to set CMD_BUSY=1 "
        "and mask CMDINVAL=0 (got ERROR_STATUS=0x%x)",
        err_sts);
  LOG_INFO(
      "[spi_host.sv:212-215,252,272-286,425-461] CONFIRMED: SW_RST level "
      "discard, SPIEN=0 CMDQD=4, "
      "and CMD_BUSY masking CMDINVAL");

  // ---------------------------------------------------------------------------
  // 3. [spi_host_reg_pkg.sv:329-366] ([E2-02] INTENDED_SECURITY_HARDENING —
  // SEC_CM: BUS.INTEGRITY):
  //    - SPI_HOST_PERMIT rejects sb to CONTROL (4'b1111) and COMMAND (4'b0011)
  //      with mcause=7, whereas sb to TXDATA (0x28) succeeds (TXQD=1).
  //    - Write to RXDATA (0x24) faults (mcause=7) and read from TXDATA (0x28)
  //      faults (mcause=5), while read from COMMAND (0x20, wo) returns 0.
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [spi_host_reg_pkg.sv:329-366] (INTENDED_SECURITY_HARDENING): "
      "SPI_HOST_PERMIT & RXDATA/TXDATA window faults");
  spi_host_sw_reset_and_clear();

  g_load_store_fault = false;
  g_last_mcause = 0;
  abs_mmio_write8(kSpiHostBase + SPI_HOST_CONTROL_REG_OFFSET, 0u);
  CHECK(g_load_store_fault && g_last_mcause == 7u,
        "[spi_host_reg_pkg.sv:329-366] Expected sb to CONTROL (PERMIT=4'b1111) "
        "to "
        "fault with mcause=7");

  g_load_store_fault = false;
  g_last_mcause = 0;
  abs_mmio_write8(kSpiHostBase + SPI_HOST_COMMAND_REG_OFFSET, 0u);
  CHECK(g_load_store_fault && g_last_mcause == 7u,
        "[spi_host_reg_pkg.sv:329-366] Expected sb to COMMAND (PERMIT=4'b0011) "
        "to "
        "fault with mcause=7");

  g_load_store_fault = false;
  g_last_mcause = 0;
  abs_mmio_write32(kSpiHostBase + SPI_HOST_RXDATA_REG_OFFSET, 0u);
  CHECK(g_load_store_fault && g_last_mcause == 7u,
        "[spi_host_reg_pkg.sv:329-366] Expected write to RXDATA (0x24) to "
        "fault with "
        "mcause=7");

  g_load_store_fault = false;
  g_last_mcause = 0;
  (void)abs_mmio_read32(kSpiHostBase + SPI_HOST_TXDATA_REG_OFFSET);
  CHECK(g_load_store_fault && g_last_mcause == 5u,
        "[spi_host_reg_pkg.sv:329-366] Expected read from TXDATA (0x28) to "
        "fault with "
        "mcause=5");

  g_load_store_fault = false;
  CHECK(abs_mmio_read32(kSpiHostBase + SPI_HOST_COMMAND_REG_OFFSET) == 0u &&
            !g_load_store_fault,
        "Expected read from WO CSR COMMAND (0x20) to return 0 without fault");

  g_load_store_fault = false;
  abs_mmio_write8(kSpiHostBase + SPI_HOST_TXDATA_REG_OFFSET, 0xa5u);
  status = abs_mmio_read32(kSpiHostBase + SPI_HOST_STATUS_REG_OFFSET);
  CHECK(!g_load_store_fault &&
            bitfield_field32_read(status, SPI_HOST_STATUS_TXQD_FIELD) == 1u,
        "[spi_host_reg_pkg.sv:329-366] Expected sb to TXDATA (0x28) to succeed "
        "and "
        "increment TXQD to 1 (status=0x%08x)",
        status);
  spi_host_sw_reset_and_clear();
  LOG_INFO(
      "[spi_host_reg_pkg.sv:329-366] CONFIRMED: SPI_HOST_PERMIT & "
      "RXDATA/TXDATA window "
      "access checks enforced");

  LOG_INFO("=== ALL SPI_HOST ERRATA CHECKS PASSED ===");
  return true;
}
