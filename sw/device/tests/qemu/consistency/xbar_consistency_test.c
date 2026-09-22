// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_lc_ctrl.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "pattgen_regs.h"
#include "rv_core_ibex_regs.h"
#include "spi_host_regs.h"
#include "sysrst_ctrl_regs.h"

OTTF_DEFINE_TEST_CONFIG();

static volatile uint32_t fault_count = 0;
static volatile uint32_t last_mcause = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = 0;
  CSR_READ(CSR_REG_MCAUSE, &mcause);
  last_mcause = mcause;
  fault_count++;
}

static void expect_read_ok(uint32_t addr, const char *desc) {
  uint32_t before = fault_count;
  volatile uint32_t val = *(volatile uint32_t *)addr;
  (void)val;
  CHECK(
      fault_count == before,
      "Expected read at 0x%08x (%s) to succeed without fault, got mcause=0x%x",
      addr, desc, last_mcause);
}

static void expect_read_fault(uint32_t addr, const char *desc) {
  uint32_t before = fault_count;
  last_mcause = 0;
  volatile uint32_t val = *(volatile uint32_t *)addr;
  (void)val;
  CHECK(fault_count == before + 1u,
        "Expected read at 0x%08x (%s) to trigger Load Access Fault, but no "
        "fault occurred",
        addr, desc);
  CHECK(last_mcause == 5u,
        "Expected mcause=5 (Load Access Fault) at 0x%08x (%s), got 0x%x", addr,
        desc, last_mcause);
}

static void test_otp_prim_crossbar_decode(void) {
  // In tl_peri_pkg.sv:
  // - ADDR_SPACE_OTP_CTRL__CORE = 0x40130000, ADDR_MASK = 0x00000fff
  // - ADDR_SPACE_OTP_CTRL__PRIM = 0x40138000, ADDR_MASK = 0x0000001f
  // (CSR0..CSR7) In otp_ctrl.sv:778-792, OTP_CTRL__PRIM is gated by
  // tlul_lc_gate with lc_dft_en[0] (enabled in TEST_UNLOCKED*, DEV, RMA;
  // disabled in PROD/PROD_END).
  const uint32_t otp_prim_base = TOP_EARLGREY_OTP_CTRL_PRIM_BASE_ADDR;
  CHECK(otp_prim_base == 0x40138000u,
        "Expected TOP_EARLGREY_OTP_CTRL_PRIM_BASE_ADDR=0x40138000, got 0x%08x",
        otp_prim_base);

  dif_lc_ctrl_t lc_ctrl;
  CHECK_DIF_OK(dif_lc_ctrl_init(
      mmio_region_from_addr(TOP_EARLGREY_LC_CTRL_BASE_ADDR), &lc_ctrl));
  dif_lc_ctrl_state_t lc_state;
  CHECK_DIF_OK(dif_lc_ctrl_get_state(&lc_ctrl, &lc_state));
  bool lc_dft_en =
      (lc_state != kDifLcCtrlStateProd && lc_state != kDifLcCtrlStateProdEnd);

  if (lc_dft_en) {
    // Valid accesses inside OTP_CTRL__PRIM (0x40138000..0x4013801c) when
    // lc_dft_en=On.
    expect_read_ok(otp_prim_base + 0x00u, "OTP_CTRL__PRIM CSR0");
    expect_read_ok(otp_prim_base + 0x1cu, "OTP_CTRL__PRIM CSR7");
  } else {
    // When lc_dft_en=Off (PROD/PROD_END), tlul_lc_gate rejects OTP_CTRL__PRIM
    // with d_error=1.
    expect_read_fault(otp_prim_base + 0x00u,
                      "OTP_CTRL__PRIM CSR0 gated by lc_dft_en=Off");
    expect_read_fault(otp_prim_base + 0x1cu,
                      "OTP_CTRL__PRIM CSR7 gated by lc_dft_en=Off");
  }

  // Out-of-bounds offset 0x20 (outside ADDR_MASK_OTP_CTRL__PRIM = 0x1f).
  expect_read_fault(otp_prim_base + 0x20u, "OTP_CTRL__PRIM + 0x20");

  // 0x40132000 is outside ADDR_MASK_OTP_CTRL__CORE (0x00000fff) and must fault
  // in xbar_peri.
  expect_read_fault(0x40132000u, "Unmapped xbar_peri hole at 0x40132000");
}

static void test_peripheral_reg_top_bounds(void) {
  // 1. PATTGEN (0x400e0000): valid registers 0x00..0x28
  // (PATTGEN_SIZE_REG_OFFSET). Offset 0x2c triggers addrmiss=1 -> d_error=1 in
  // pattgen_reg_top.sv.
  expect_read_ok(TOP_EARLGREY_PATTGEN_BASE_ADDR + PATTGEN_SIZE_REG_OFFSET,
                 "PATTGEN.SIZE (0x28)");
  expect_read_fault(
      TOP_EARLGREY_PATTGEN_BASE_ADDR + PATTGEN_SIZE_REG_OFFSET + 4u,
      "PATTGEN addrmiss (0x2c)");
  expect_read_fault(TOP_EARLGREY_PATTGEN_BASE_ADDR + 0x40u,
                    "PATTGEN xbar_peri boundary (0x40)");

  // 2. SPI_HOST0 (0x40300000) & SPI_HOST1 (0x40310000): valid registers
  // 0x00..0x30 (SPI_HOST_EVENT_ENABLE_REG_OFFSET).
  // Offset 0x34 triggers addrmiss=1 -> d_error=1 in spi_host_reg_top.sv.
  expect_read_ok(
      TOP_EARLGREY_SPI_HOST0_BASE_ADDR + SPI_HOST_EVENT_ENABLE_REG_OFFSET,
      "SPI_HOST0.EVENT_ENABLE (0x30)");
  expect_read_fault(
      TOP_EARLGREY_SPI_HOST0_BASE_ADDR + SPI_HOST_EVENT_ENABLE_REG_OFFSET + 4u,
      "SPI_HOST0 addrmiss (0x34)");
  expect_read_ok(
      TOP_EARLGREY_SPI_HOST1_BASE_ADDR + SPI_HOST_EVENT_ENABLE_REG_OFFSET,
      "SPI_HOST1.EVENT_ENABLE (0x30)");
  expect_read_fault(
      TOP_EARLGREY_SPI_HOST1_BASE_ADDR + SPI_HOST_EVENT_ENABLE_REG_OFFSET + 4u,
      "SPI_HOST1 addrmiss (0x34)");

  // 3. SYSRST_CTRL_AON (0x40430000): valid registers 0x00..0x70
  // (SYSRST_CTRL_KEY_INTR_STATUS_REG_OFFSET).
  // Offset 0x74 triggers addrmiss=1 -> d_error=1 in sysrst_ctrl_reg_top.sv.
  expect_read_ok(TOP_EARLGREY_SYSRST_CTRL_AON_BASE_ADDR +
                     SYSRST_CTRL_KEY_INTR_STATUS_REG_OFFSET,
                 "SYSRST_CTRL_AON.KEY_INTR_STATUS (0x70)");
  expect_read_fault(TOP_EARLGREY_SYSRST_CTRL_AON_BASE_ADDR +
                        SYSRST_CTRL_KEY_INTR_STATUS_REG_OFFSET + 4u,
                    "SYSRST_CTRL_AON addrmiss (0x74)");
}

static alignas(4096) volatile uint32_t remap_target_buf[1024];

static void test_vmapper_dbus_remap(void) {
  mmio_region_t ibex_base =
      mmio_region_from_addr(TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR);

  // 0x42000000..0x42000fff is inside ROM PMP TOR region 11 [0x40000000,
  // 0x42010000) (LRW) and unmapped in xbar_main when DBUS remap slot 0 is
  // disabled.
  const uint32_t vaddr_base = 0x42000000u;
  const uint32_t paddr_base = (uint32_t)(uintptr_t)remap_target_buf;
  CHECK((paddr_base & 0xfffu) == 0u,
        "Expected 4KiB-aligned target buffer, got 0x%08x", paddr_base);

  uint32_t dbus_regwen_0 =
      mmio_region_read32(ibex_base, RV_CORE_IBEX_DBUS_REGWEN_0_REG_OFFSET);

  // When DBUS_ADDR_EN_0 = 0, reading 0x42000000 must fault in xbar_main.
  mmio_region_write32(ibex_base, RV_CORE_IBEX_DBUS_ADDR_EN_0_REG_OFFSET, 0u);
  expect_read_fault(vaddr_base, "Unmapped 0x42000000 with DBUS_ADDR_EN_0=0");

  // Attempt to enable 4 KiB NAPOT remap window: match_addr = vaddr_base |
  // 0x7ff.
  remap_target_buf[0] = 0xcafebabeu;
  mmio_region_write32(ibex_base, RV_CORE_IBEX_DBUS_ADDR_MATCHING_0_REG_OFFSET,
                      vaddr_base | 0x7ffu);
  mmio_region_write32(ibex_base, RV_CORE_IBEX_DBUS_REMAP_ADDR_0_REG_OFFSET,
                      paddr_base);
  mmio_region_write32(ibex_base, RV_CORE_IBEX_DBUS_ADDR_EN_0_REG_OFFSET, 1u);

  if (dbus_regwen_0 == 0u) {
    // Under sival_rom_ext, ROM_EXT locks DBUS_REGWEN_0=0 before jumping to
    // owner code. Verify that the writes were ignored by hardware and that
    // 0x42000000 still faults.
    CHECK(mmio_region_read32(ibex_base,
                             RV_CORE_IBEX_DBUS_ADDR_EN_0_REG_OFFSET) == 0u,
          "Expected DBUS_ADDR_EN_0 write to be blocked when DBUS_REGWEN_0=0");
    expect_read_fault(vaddr_base,
                      "Unmapped 0x42000000 when DBUS_REGWEN_0=0 blocks remap");
    return;
  }

  uint32_t val = *(volatile uint32_t *)vaddr_base;
  CHECK(val == 0xcafebabeu,
        "Expected remapped read at 0x%08x to return 0xcafebabe, got 0x%08x",
        vaddr_base, val);

  *(volatile uint32_t *)(vaddr_base + 16u) = 0x12345678u;
  CHECK(remap_target_buf[4] == 0x12345678u,
        "Expected remapped write at 0x%08x to update target SRAM buffer, got "
        "0x%08x",
        vaddr_base + 16u, remap_target_buf[4]);

  // Disable DBUS_ADDR_EN_0 and verify 0x42000000 faults again.
  mmio_region_write32(ibex_base, RV_CORE_IBEX_DBUS_ADDR_EN_0_REG_OFFSET, 0u);
  expect_read_fault(vaddr_base,
                    "Unmapped 0x42000000 after clearing DBUS_ADDR_EN_0");
}

bool test_main(void) {
  test_otp_prim_crossbar_decode();
  test_peripheral_reg_top_bounds();
  test_vmapper_dbus_remap();

  LOG_INFO("xbar_consistency_test passed!");
  return true;
}
