// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file spi_device_errata_test.c
 * @brief CW340 FPGA & QEMU Errata Confirmation Test for `spi_device` (`P07`).
 *
 * Empirically confirms on physical CW340 FPGA silicon and QEMU:
 * 1. [spid_status.sv:165-239] (SPEC_DOC_ERRATA / CDC Hazard, HIGH):
 *    `spi_device.hjson:708-709, 715-716` states `FLASH_STATUS` clears when
 *    `CSb` is high and instructs SW to read back `FLASH_STATUS` to confirm.
 *    In `spid_status.sv:165-239, 308-314`, `sys_status_o` never updates while
 *    `STATUS.CSB == 1` (`SCK` idle) and remains `0x00000000` until an external
 *    SPI transaction clocks `>= 8` `SCK` edges and deasserts `CSb` (`0 -> 1`).
 * 2. [spid_addr_4b.sv:52-94] (TRUE_SILICON_ERRATA / CDC Hazard, HIGH):
 *    In `spid_addr_4b.sv:52-68, 92-117`, writing `ADDR_MODE` (even writing `0`
 *    when `sys_cfg_addr_4b_en` in the SPI domain is already `0`)
 * unconditionally sets `ADDR_MODE.PENDING` (`bit 31`) to `1` (`0x80000000`),
 * which remains stuck at `1` while `CSb` is high (`SCK` idle) because
 * `cmd_sync_pulse_i` requires `8` `SCK` rising edges.
 * 3. [spi_device_pkg.sv:498-547] (SPEC_DOC_ERRATA, MEDIUM):
 *    `spi_device.hjson:1573-1591` falsely describes `ingress_buffer` as `640 B`
 *    starting with `256 B SFDP buffer`, whereas `spi_device_reg_top.sv:131-137`
 *    maps `egress_buffer` to `[0x1000..0x1d3f]` (`848` words) and
 *    `ingress_buffer` to `[0x1e00..0x1fbf]` (`112` words = `448 B`), routing
 *    the unmapped SRAM aperture holes `[0x1d40..0x1dff]` and `[0x1fc0..0x1fff]`
 *    to `addrmiss -> d_error = 1` (`mcause = 5` / `mcause = 7`).
 * 4. [spi_device.sv:1655-1715] (INTENDED_SECURITY_HARDENING, LOW):
 *    `spi_device.sv:1655-1715` configures `u_tlul2sram_egress` with
 *    `.ByteAccess(0), .ErrOnRead(1)` (`sb`/`sh` raises `mcause = 7`; `lw`
 *    raises `mcause = 5`) and `u_tlul2sram_ingress` with
 *    `.ByteAccess(0), .ErrOnWrite(1)` (`sw` raises `mcause = 7`).
 * 5. [spi_device_reg_pkg.sv:837-911] (INTENDED_SECURITY_HARDENING, INFO —
 * SEC_CM: BUS.INTEGRITY): `SPI_DEVICE_PERMIT` (`spi_device_reg_pkg.sv:837-911`)
 * enforces narrow byte-enable masks (`4'b1111` on `CFG` and `TPM_ACCESS_0`,
 * `4'b0001` on `TPM_ACCESS_1` and `TPM_RID`), raising `mcause = 7` on
 * out-of-mask sub-word writes.
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "spi_device_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kSpiDeviceBase = TOP_EARLGREY_SPI_DEVICE_BASE_ADDR,
};

static volatile bool g_expect_access_fault = false;
static volatile uint32_t g_access_fault_count = 0;
static volatile uint32_t g_last_mcause = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  if (g_expect_access_fault) {
    g_last_mcause = ibex_mcause_read();
    ++g_access_fault_count;
    return;
  }
  ottf_generic_fault_print(exc_info, "Unhandled Load/Store Fault",
                           ibex_mcause_read());
  abort();
}

bool test_main(void) {
  LOG_INFO(
      "=== OpenTitan Earlgrey SPI_DEVICE Errata Confirmation Suite (P07) ===");

  // Confirm SPI CSb is high (SPI bus idle).
  uint32_t status =
      abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_STATUS_REG_OFFSET);
  CHECK(((status >> SPI_DEVICE_STATUS_CSB_BIT) & 1u) == 1u,
        "Expected SPI_DEVICE STATUS.CSB == 1 (idle), got 0x%08x", status);

  // ---------------------------------------------------------------------------
  // 1. [spid_status.sv:165-239] (SPEC_DOC_ERRATA, HIGH):
  //    While CSb is high (SCK idle), writing FLASH_STATUS queues to
  //    u_sw_status_update_sync (clocked by external SCK), and reading back
  //    FLASH_STATUS returns sys_status_o (0x00000000) rather than updating
  //    immediately while CSb is high.
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [spid_status.sv:165-239] (SPEC_DOC_ERRATA): FLASH_STATUS "
      "readback stays 0 while CSb==1 (SCK idle)");
  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_FLASH_STATUS_REG_OFFSET,
                   0x00a5a504u);
  uint32_t flash_status =
      abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_FLASH_STATUS_REG_OFFSET);
  CHECK(flash_status == 0x00000000u,
        "[spid_status.sv:165-239] Expected FLASH_STATUS readback == 0x00000000 "
        "while CSb is high (SCK idle), got 0x%08x",
        flash_status);
  // Clear u_sw_status_update_sync via CONTROL.FLASH_STATUS_FIFO_CLR.
  uint32_t ctrl =
      abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_CONTROL_REG_OFFSET);
  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_CONTROL_REG_OFFSET,
                   ctrl | (1u << SPI_DEVICE_CONTROL_FLASH_STATUS_FIFO_CLR_BIT));
  LOG_INFO(
      "[spid_status.sv:165-239] CONFIRMED: FLASH_STATUS readback=0x%08x while "
      "CSb==1",
      flash_status);

  // ---------------------------------------------------------------------------
  // 2. [spid_addr_4b.sv:52-94] (TRUE_SILICON_ERRATA / CDC Hazard, HIGH):
  //    Writing ADDR_MODE sets PENDING (bit 31) to 1 even when writing 0 (the
  //    value already active in the SPI domain), and PENDING remains stuck at 1
  //    until 8 SCK edges occur on the external SPI bus.
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [spid_addr_4b.sv:52-94] (TRUE_SILICON_ERRATA): "
      "ADDR_MODE.PENDING asserts unconditionally & stays 1 without 8 SCK "
      "edges");
  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_ADDR_MODE_REG_OFFSET, 1u);
  uint32_t addr_mode_1 =
      abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_ADDR_MODE_REG_OFFSET);
  CHECK(addr_mode_1 == 0x80000001u,
        "[spid_addr_4b.sv:52-94] Expected ADDR_MODE == 0x80000001 after "
        "writing 1, got 0x%08x",
        addr_mode_1);

  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_ADDR_MODE_REG_OFFSET, 0u);
  uint32_t addr_mode_0 =
      abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_ADDR_MODE_REG_OFFSET);
  CHECK(addr_mode_0 == 0x80000000u,
        "[spid_addr_4b.sv:52-94] Expected ADDR_MODE == 0x80000000 (PENDING=1 "
        "despite writing 0 matching SPI domain), got 0x%08x",
        addr_mode_0);
  LOG_INFO(
      "[spid_addr_4b.sv:52-94] CONFIRMED: ADDR_MODE=0x%08x (PENDING=1) on "
      "identical-value write",
      addr_mode_0);

  // ---------------------------------------------------------------------------
  // 3. [spi_device_pkg.sv:498-547] (SPEC_DOC_ERRATA, MEDIUM):
  //    ingress_buffer maps [0x1e00..0x1fbf] (448 B) and egress_buffer maps
  //    [0x1000..0x1d3f] (3392 B); unmapped holes [0x1d40..0x1dff] and
  //    [0x1fc0..0x1fff] steer to addrmiss -> d_error = 1 (mcause = 7 / 5).
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [spi_device_pkg.sv:498-547] (SPEC_DOC_ERRATA): ingress_buffer "
      "layout & unmapped SRAM holes [0x1d40..0x1dff], [0x1fc0..0x1fff]");
  g_access_fault_count = 0;
  (void)abs_mmio_read32(kSpiDeviceBase + 0x1e00u);  // Payload FIFO (256 B)
  (void)abs_mmio_read32(kSpiDeviceBase + 0x1f00u);  // CmdFIFO (64 B)
  (void)abs_mmio_read32(kSpiDeviceBase + 0x1f40u);  // AddrFIFO (64 B)
  CHECK(g_access_fault_count == 0u,
        "[spi_device_pkg.sv:498-547] Expected valid ingress_buffer reads at "
        "0x1e00, 0x1f00, 0x1f40 to succeed without fault");

  g_access_fault_count = 0;
  g_last_mcause = 0;
  g_expect_access_fault = true;
  abs_mmio_write32(kSpiDeviceBase + 0x1d40u, 0xdeadbeefu);
  g_expect_access_fault = false;
  CHECK(g_access_fault_count == 1u && g_last_mcause == 7u,
        "[spi_device_pkg.sv:498-547] Expected write to unmapped hole 0x1d40 to "
        "fault with mcause=7 (got count=%u, mcause=%u)",
        g_access_fault_count, g_last_mcause);

  g_access_fault_count = 0;
  g_last_mcause = 0;
  g_expect_access_fault = true;
  (void)abs_mmio_read32(kSpiDeviceBase + 0x1fc0u);
  g_expect_access_fault = false;
  CHECK(
      g_access_fault_count == 1u && g_last_mcause == 5u,
      "[spi_device_pkg.sv:498-547] Expected read from unmapped hole 0x1fc0 to "
      "fault with mcause=5 (got count=%u, mcause=%u)",
      g_access_fault_count, g_last_mcause);
  LOG_INFO(
      "[spi_device_pkg.sv:498-547] CONFIRMED: holes 0x1d40 (mcause=7) & 0x1fc0 "
      "(mcause=5) fault");

  // ---------------------------------------------------------------------------
  // 4. [spi_device.sv:1655-1715] (INTENDED_SECURITY_HARDENING, LOW):
  //    u_tlul2sram_egress (.ByteAccess(0), .ErrOnRead(1)) and
  //    u_tlul2sram_ingress (.ByteAccess(0), .ErrOnWrite(1)) fault on sub-word
  //    or wrong-direction accesses.
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [spi_device.sv:1655-1715] (INTENDED_SECURITY_HARDENING): "
      "Egress/Ingress SRAM .ByteAccess(0) & directional faults");
  g_access_fault_count = 0;
  g_last_mcause = 0;
  g_expect_access_fault = true;
  abs_mmio_write8(kSpiDeviceBase + SPI_DEVICE_EGRESS_BUFFER_REG_OFFSET, 0x5au);
  g_expect_access_fault = false;
  CHECK(g_access_fault_count == 1u && g_last_mcause == 7u,
        "[spi_device.sv:1655-1715] Expected 8-bit write (sb) to egress_buffer "
        "to fault with mcause=7 (.ByteAccess(0))");

  g_access_fault_count = 0;
  g_last_mcause = 0;
  g_expect_access_fault = true;
  (void)abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_EGRESS_BUFFER_REG_OFFSET);
  g_expect_access_fault = false;
  CHECK(
      g_access_fault_count == 1u && g_last_mcause == 5u,
      "[spi_device.sv:1655-1715] Expected 32-bit read (lw) from egress_buffer "
      "to fault with mcause=5 (.ErrOnRead(1))");

  g_access_fault_count = 0;
  g_last_mcause = 0;
  g_expect_access_fault = true;
  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_INGRESS_BUFFER_REG_OFFSET,
                   0x12345678u);
  g_expect_access_fault = false;
  CHECK(
      g_access_fault_count == 1u && g_last_mcause == 7u,
      "[spi_device.sv:1655-1715] Expected 32-bit write (sw) to ingress_buffer "
      "to fault with mcause=7 (.ErrOnWrite(1))");
  LOG_INFO(
      "[spi_device.sv:1655-1715] CONFIRMED: .ByteAccess(0), .ErrOnRead(1), "
      ".ErrOnWrite(1) enforced");

  // ---------------------------------------------------------------------------
  // 5. [spi_device_reg_pkg.sv:837-911] (INTENDED_SECURITY_HARDENING, INFO —
  // SEC_CM: BUS.INTEGRITY):
  //    SPI_DEVICE_PERMIT enforces 4'b1111 on CFG and TPM_ACCESS_0, and 4'b0001
  //    on TPM_ACCESS_1 and TPM_RID.
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [spi_device_reg_pkg.sv:837-911] "
      "(INTENDED_SECURITY_HARDENING): "
      "SPI_DEVICE_PERMIT sub-word write protection");
  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_CFG_REG_OFFSET, 0x00000004u);
  g_access_fault_count = 0;
  g_last_mcause = 0;
  g_expect_access_fault = true;
  abs_mmio_write8(kSpiDeviceBase + SPI_DEVICE_CFG_REG_OFFSET, 0x03u);
  g_expect_access_fault = false;
  CHECK(g_access_fault_count == 1u && g_last_mcause == 7u &&
            abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_CFG_REG_OFFSET) ==
                0x00000004u,
        "[spi_device_reg_pkg.sv:837-911] Expected sb to CFG (PERMIT=4'b1111) "
        "to fault "
        "with mcause=7 and preserve CFG=0x4");
  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_CFG_REG_OFFSET, 0u);

  g_access_fault_count = 0;
  abs_mmio_write8(kSpiDeviceBase + SPI_DEVICE_TPM_ACCESS_1_REG_OFFSET, 0x5au);
  CHECK(g_access_fault_count == 0u &&
            abs_mmio_read32(kSpiDeviceBase +
                            SPI_DEVICE_TPM_ACCESS_1_REG_OFFSET) == 0x5au,
        "[spi_device_reg_pkg.sv:837-911] Expected sb to TPM_ACCESS_1+0 "
        "(PERMIT=4'b0001) to succeed");
  g_access_fault_count = 0;
  g_last_mcause = 0;
  g_expect_access_fault = true;
  abs_mmio_write8(kSpiDeviceBase + SPI_DEVICE_TPM_ACCESS_1_REG_OFFSET + 1u,
                  0xa5u);
  g_expect_access_fault = false;
  CHECK(g_access_fault_count == 1u && g_last_mcause == 7u,
        "[spi_device_reg_pkg.sv:837-911] Expected sb to TPM_ACCESS_1+1 "
        "(PERMIT=4'b0001) to fault with mcause=7");
  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_TPM_ACCESS_1_REG_OFFSET, 0u);
  LOG_INFO(
      "[spi_device_reg_pkg.sv:837-911] CONFIRMED: SPI_DEVICE_PERMIT enforced");

  LOG_INFO("=== ALL SPI_DEVICE ERRATA CHECKS PASSED ===");
  return true;
}
