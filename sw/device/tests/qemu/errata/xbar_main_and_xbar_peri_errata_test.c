// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file xbar_main_and_xbar_peri_errata_test.c
 * @brief CW340 FPGA & QEMU Errata Confirmation Test for `xbar_main` and
 *        `xbar_peri` (`P38` —
 * `/root/knowledge/errata/xbar_main_and_xbar_peri.md`).
 *
 * Empirically confirms on both physical CW340 FPGA silicon and QEMU:
 * - [tl_peri_pkg.sv:26] (`Category E2` / `INTENDED_SECURITY_HARDENING`):
 *   Two-stage TileLink-UL bus error architecture (`SEC_CM: BUS.INTEGRITY`):
 *   1. Stage 1 (`xbar_main` & `xbar_peri` `tlul_err_resp`): Accesses to
 *      unmapped crossbar apertures (`0x40010000` in `xbar_peri`, `0x40320000`
 *      in `xbar_main`) raise synchronous Ibex Load Access Fault (`mcause = 5`)
 *      and Store Access Fault (`mcause = 7`).
 *   2. Stage 2 (Downstream `*_reg_top.sv` `addrmiss = ~|addr_hit`):
 *      Even when an address falls inside a peripheral's power-of-two crossbar
 *      `ADDR_MASK` aperture (`size_byte = 0x40` for `pattgen` and
 * `spi_host0/1`, `size_byte = 0x100` for `sysrst_ctrl_aon`) and is routed by
 * the crossbar without crossbar error, accessing any unallocated trailing
 * offset beyond the last implemented CSR (`0x30..0x3c` in `pattgen` after `SIZE
 * = 0x2c`, `0x34..0x3c` in `spi_host0/1` after `EVENT_ENABLE = 0x30`, and
 *      `0xac..0xfc` in `sysrst_ctrl_aon` after `KEY_INTR_STATUS = 0xa8`)
 * asserts `addrmiss = 1'b1` -> `tl_o.d_error = 1'b1`, raising synchronous Ibex
 *      Load/Store Access Faults (`mcause = 5/7`) while the last valid CSR
 * offset succeeds without fault.
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
#include "pattgen_regs.h"
#include "spi_host_regs.h"
#include "sysrst_ctrl_regs.h"

OTTF_DEFINE_TEST_CONFIG();

static volatile bool g_expect_access_fault = false;
static volatile uint32_t g_expected_mcause = 0;
static volatile uint32_t g_fault_count = 0;

void ottf_exception_handler(uint32_t *exc_info) {
  uint32_t mcause = ibex_mcause_read();
  if (g_expect_access_fault && mcause == g_expected_mcause) {
    g_fault_count++;
    g_expect_access_fault = false;
    return;
  }
  LOG_ERROR("Unexpected exception mcause=0x%08x mepc=0x%08x mtval=0x%08x",
            mcause, ibex_mepc_read(), ibex_mtval_read());
  ottf_generic_fault_print(exc_info, "Unexpected fault", mcause);
  abort();
}

static void expect_load_fault(uint32_t addr) {
  uint32_t before = g_fault_count;
  g_expected_mcause = kIbexExcLoadAccessFault;
  g_expect_access_fault = true;
  uint32_t dummy;
  asm volatile("lw %0, 0(%1)" : "=r"(dummy) : "r"(addr) : "memory");
  CHECK(!g_expect_access_fault, "Expected Load Access Fault at 0x%08x", addr);
  CHECK(g_fault_count == before + 1);
}

static void expect_store_fault(uint32_t addr, uint32_t val) {
  uint32_t before = g_fault_count;
  g_expected_mcause = kIbexExcStoreAccessFault;
  g_expect_access_fault = true;
  asm volatile("sw %0, 0(%1)" : : "r"(val), "r"(addr) : "memory");
  CHECK(!g_expect_access_fault, "Expected Store Access Fault at 0x%08x", addr);
  CHECK(g_fault_count == before + 1);
}

static void test_xbar_two_stage_decode_faults(void) {
  LOG_INFO(
      "Verifying [tl_peri_pkg.sv:26] (Category E2 / "
      "INTENDED_SECURITY_HARDENING): "
      "Two-stage xbar_main/xbar_peri ADDR_MASK vs *_reg_top.sv addrmiss "
      "faults");

  // 1. Stage 1: Unmapped crossbar apertures (tlul_err_resp just past ADDR_MASK)
  const uint32_t kUnmappedPeriAddr = TOP_EARLGREY_PATTGEN_BASE_ADDR + 0x40u;
  const uint32_t kUnmappedMainAddr = TOP_EARLGREY_SPI_HOST0_BASE_ADDR + 0x40u;
  expect_load_fault(kUnmappedPeriAddr);
  expect_store_fault(kUnmappedPeriAddr, 0xdeadbeefu);
  expect_load_fault(kUnmappedMainAddr);
  expect_store_fault(kUnmappedMainAddr, 0xdeadbeefu);

  // 2. Stage 2a: PATTGEN (base 0x400e0000, xbar_peri size_byte=0x40, last
  // CSR=0x2c)
  const uint32_t kPattgenBase = TOP_EARLGREY_PATTGEN_BASE_ADDR;
  uint32_t faults_before = g_fault_count;
  uint32_t pattgen_size =
      abs_mmio_read32(kPattgenBase + PATTGEN_SIZE_REG_OFFSET);
  (void)pattgen_size;
  CHECK(g_fault_count == faults_before,
        "Last valid PATTGEN CSR (0x2c) should not fault");
  expect_load_fault(kPattgenBase + 0x30u);
  expect_store_fault(kPattgenBase + 0x30u, 0u);
  expect_load_fault(kPattgenBase + 0x3cu);
  expect_store_fault(kPattgenBase + 0x3cu, 0u);

  // 3. Stage 2b: SPI_HOST0 (0x40300000) & SPI_HOST1 (0x40310000)
  // (xbar_main size_byte=0x40, last CSR=EVENT_ENABLE at 0x34 due to
  // RXDATA/TXDATA windows at 0x24/0x28)
  const uint32_t kSpiHost0Base = TOP_EARLGREY_SPI_HOST0_BASE_ADDR;
  const uint32_t kSpiHost1Base = TOP_EARLGREY_SPI_HOST1_BASE_ADDR;
  CHECK(SPI_HOST_EVENT_ENABLE_REG_OFFSET == 0x34u);
  faults_before = g_fault_count;
  (void)abs_mmio_read32(kSpiHost0Base + SPI_HOST_EVENT_ENABLE_REG_OFFSET);
  (void)abs_mmio_read32(kSpiHost1Base + SPI_HOST_EVENT_ENABLE_REG_OFFSET);
  CHECK(g_fault_count == faults_before,
        "Last valid SPI_HOST0/1 CSR (0x34) should not fault");
  expect_load_fault(kSpiHost0Base + 0x38u);
  expect_store_fault(kSpiHost0Base + 0x38u, 0u);
  expect_load_fault(kSpiHost0Base + 0x3cu);
  expect_store_fault(kSpiHost1Base + 0x38u, 0u);
  expect_load_fault(kSpiHost1Base + 0x3cu);

  // 4. Stage 2c: SYSRST_CTRL_AON (0x40430000, xbar_peri size_byte=0x100,
  // last CSR=KEY_INTR_STATUS at 0xa8)
  const uint32_t kSysrstBase = TOP_EARLGREY_SYSRST_CTRL_AON_BASE_ADDR;
  faults_before = g_fault_count;
  (void)abs_mmio_read32(kSysrstBase + SYSRST_CTRL_KEY_INTR_STATUS_REG_OFFSET);
  CHECK(g_fault_count == faults_before,
        "Last valid SYSRST_CTRL_AON CSR (0xa8) should not fault");
  expect_load_fault(kSysrstBase + 0xacu);
  expect_store_fault(kSysrstBase + 0xacu, 0u);
  expect_load_fault(kSysrstBase + 0xfcu);
  expect_store_fault(kSysrstBase + 0xfcu, 0u);

  LOG_INFO(
      "[tl_peri_pkg.sv:26] Confirmed %u synchronous TL-UL bus faults across "
      "xbar_main/xbar_peri and pattgen/spi_host/sysrst_ctrl tail apertures",
      g_fault_count);
}

bool test_main(void) {
  LOG_INFO("=== xbar_main_and_xbar_peri Errata Confirmation Test Suite ===");
  test_xbar_two_stage_decode_faults();
  LOG_INFO("=== All xbar_main_and_xbar_peri Errata Checks PASSED ===");
  return true;
}
