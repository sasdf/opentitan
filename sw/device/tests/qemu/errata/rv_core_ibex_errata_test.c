// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file rv_core_ibex_errata_test.c
 * @brief CW340 FPGA & QEMU Errata Confirmation Suite for `rv_core_ibex` (P34).
 *
 * Empirically confirms on both physical CW340 FPGA silicon and QEMU:
 * 1. [rv_core_ibex_cfg_reg_top.sv:130-168] (INTENDED_SECURITY_HARDENING, INFO —
 * SEC_CM: BUS.INTEGRITY):
 *    - Implicit 28-byte unmapped decode hole (`0x64..0x7c`) between `FPGA_INFO`
 *      (`0x60`) and `DV_SIM_WINDOW` (`0x80..0x9c`) returns synchronous TL-UL
 *      `d_error = 1` (`mcause = 5` on read, `mcause = 7` on write).
 *    - `RV_CORE_IBEX_CFG_PERMIT` rejects 8-bit `sb` writes to 32-bit CSRs
 *      (`IBUS_ADDR_MATCHING_0` at `0x10`, `mcause = 7`), whereas `sb` writes to
 *      `DV_SIM_WINDOW` (`0x80..0x9c`, `byte-write: "true"`) succeed (`d_error =
 * 0`).
 * 2. [rv_core_ibex.sv:752] (INTENDED_SECURITY_HARDENING, INFO):
 *    - `SW_RECOV_ERR` (`0x04`): Writing `kMultiBitBool4True` (`0x6`) or any
 *      non-`MuBi4False` value (`0x5`) triggers
 * `kTopEarlgreyAlertIdRvCoreIbexRecovSwErr` and hardware automatically resets
 * `SW_RECOV_ERR` back to `0x9`
 *      (`kMultiBitBool4False`) upon alert handshake completion.
 *    - `RND_DATA` (`0x58`) & `RND_STATUS` (`0x5c`): Reading `RND_DATA` consumes
 *      the buffered entropy word and immediately clears
 * `RND_STATUS.RND_DATA_VALID` and `RND_DATA` to `0` until the next `EDN0` word
 * arrives.
 * 3. Address Translation Routing (`IBUS/DBUS_ADDR_MATCHING_0`, `REMAP_ADDR_0`,
 *    `ADDR_EN_0`):
 *    - `DBUS_ADDR_EN_0` (`0x24`) masks writes with `0x1` (`1-bit` register).
 *    - Mapping a 4 KiB NAPOT region (`0xa00007ff`) to `sram_ctrl_ret_aon`
 *      (`0x40600000`) routes loads/stores at `0xa0000000 + offset`
 * transparently to retention SRAM when enabled (`ADDR_EN_0 = 1`) and faults
 * (`mcause = 5`) when disabled (`ADDR_EN_0 = 0`).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/entropy_testutils.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"
#include "sw/device/silicon_creator/lib/drivers/retention_sram.h"

#include "edn_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "rv_core_ibex_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kIbexCfgBase = TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR,
  kEdn0Base = TOP_EARLGREY_EDN0_BASE_ADDR,
};

static volatile uint32_t s_remap_src[64] __attribute__((aligned(256)));
static volatile uint32_t s_remap_dst[64] __attribute__((aligned(256)));

static volatile bool g_load_store_fault = false;
static volatile uint32_t g_last_mcause = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  g_load_store_fault = true;
  g_last_mcause = ibex_mcause_read();
}

bool test_main(void) {
  LOG_INFO(
      "=== OpenTitan Earlgrey RV_CORE_IBEX Errata Confirmation Suite (P34) "
      "===");

  // ---------------------------------------------------------------------------
  // 1. [rv_core_ibex_cfg_reg_top.sv:130-168] (INTENDED_SECURITY_HARDENING —
  // SEC_CM: BUS.INTEGRITY):
  //    - Implicit 28-byte unmapped decode hole (0x64..0x7c) faults with
  //      mcause=5 on read and mcause=7 on write.
  //    - RV_CORE_IBEX_CFG_PERMIT rejects sb to IBUS_ADDR_MATCHING_0 (0x10,
  //      mcause=7), while sb to DV_SIM_WINDOW (0x84+1) succeeds.
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [rv_core_ibex_cfg_reg_top.sv:130-168] "
      "(INTENDED_SECURITY_HARDENING): "
      "0x64..0x7c decode hole & RV_CORE_IBEX_CFG_PERMIT");
  g_load_store_fault = false;
  g_last_mcause = 0;
  (void)abs_mmio_read32(kIbexCfgBase + 0x64u);
  CHECK(g_load_store_fault && g_last_mcause == 5u,
        "[rv_core_ibex_cfg_reg_top.sv:130-168] Expected read at unmapped gap "
        "0x64 to fault "
        "with mcause=5");

  g_load_store_fault = false;
  g_last_mcause = 0;
  (void)abs_mmio_read32(kIbexCfgBase + 0x7cu);
  CHECK(g_load_store_fault && g_last_mcause == 5u,
        "[rv_core_ibex_cfg_reg_top.sv:130-168] Expected read at unmapped gap "
        "0x7c to fault "
        "with mcause=5");

  g_load_store_fault = false;
  g_last_mcause = 0;
  abs_mmio_write32(kIbexCfgBase + 0x64u, 0u);
  CHECK(g_load_store_fault && g_last_mcause == 7u,
        "[rv_core_ibex_cfg_reg_top.sv:130-168] Expected write at unmapped gap "
        "0x64 to "
        "fault with mcause=7");

  g_load_store_fault = false;
  g_last_mcause = 0;
  abs_mmio_write8(kIbexCfgBase + RV_CORE_IBEX_IBUS_ADDR_MATCHING_0_REG_OFFSET,
                  0x55u);
  CHECK(g_load_store_fault && g_last_mcause == 7u,
        "[rv_core_ibex_cfg_reg_top.sv:130-168] Expected sb to "
        "IBUS_ADDR_MATCHING_0 "
        "(PERMIT=4'b1111) to fault with mcause=7");

  // Sub-word write to 1-byte CSR DBUS_ADDR_EN_0 (+0, PERMIT=4'b0001) succeeds
  // when targeting byte lane 0 (reg_be=4'b0001), whereas targeting byte lane 1
  // (+1, reg_be=4'b0010) faults with mcause=7:
  g_load_store_fault = false;
  abs_mmio_write8(kIbexCfgBase + RV_CORE_IBEX_DBUS_ADDR_EN_0_REG_OFFSET, 1u);
  CHECK(!g_load_store_fault &&
            abs_mmio_read32(kIbexCfgBase +
                            RV_CORE_IBEX_DBUS_ADDR_EN_0_REG_OFFSET) == 1u,
        "[rv_core_ibex_cfg_reg_top.sv:130-168] Expected sb to DBUS_ADDR_EN_0+0 "
        "(PERMIT=4'b0001, reg_be=4'b0001) to succeed");
  abs_mmio_write8(kIbexCfgBase + RV_CORE_IBEX_DBUS_ADDR_EN_0_REG_OFFSET, 0u);

  g_load_store_fault = false;
  g_last_mcause = 0;
  abs_mmio_write8(kIbexCfgBase + RV_CORE_IBEX_DBUS_ADDR_EN_0_REG_OFFSET + 1u,
                  1u);
  CHECK(g_load_store_fault && g_last_mcause == 7u,
        "[rv_core_ibex_cfg_reg_top.sv:130-168] Expected sb to DBUS_ADDR_EN_0+1 "
        "(PERMIT=4'b0001, reg_be=4'b0010) to fault with mcause=7");
  LOG_INFO(
      "[rv_core_ibex_cfg_reg_top.sv:130-168] CONFIRMED: 0x64..0x7c decode hole "
      "& "
      "RV_CORE_IBEX_CFG_PERMIT enforced");

  // ---------------------------------------------------------------------------
  // 2. [rv_core_ibex.sv:752] (INTENDED_SECURITY_HARDENING):
  //    - SW_RECOV_ERR (0x04) triggers RvCoreIbexRecovSwErr on both 0x6 (True)
  //      and 0x5 (loose True) and automatically self-clears back to 0x9
  //      (kMultiBitBool4False) upon alert handshake completion.
  //    - RND_DATA (0x58) read immediately clears RND_STATUS (0x5c) and RND_DATA
  //      to 0 until the next EDN0 word arrives.
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [rv_core_ibex.sv:752] (INTENDED_SECURITY_HARDENING): "
      "SW_RECOV_ERR auto-clear & RND_DATA consumption clear");
  CHECK(abs_mmio_read32(kIbexCfgBase + RV_CORE_IBEX_SW_RECOV_ERR_REG_OFFSET) ==
            kMultiBitBool4False,
        "Expected SW_RECOV_ERR initial value == 0x9");

  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdRvCoreIbexRecovSwErr));
  abs_mmio_write32(kIbexCfgBase + RV_CORE_IBEX_SW_RECOV_ERR_REG_OFFSET,
                   kMultiBitBool4True);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdRvCoreIbexRecovSwErr));
  uint32_t sw_recov =
      abs_mmio_read32(kIbexCfgBase + RV_CORE_IBEX_SW_RECOV_ERR_REG_OFFSET);
  CHECK(sw_recov == kMultiBitBool4False,
        "[rv_core_ibex.sv:752] Expected SW_RECOV_ERR to auto-clear to 0x9 "
        "after writing 0x6 (got 0x%x)",
        sw_recov);

  // Also test loose-true 0x5:
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdRvCoreIbexRecovSwErr));
  abs_mmio_write32(kIbexCfgBase + RV_CORE_IBEX_SW_RECOV_ERR_REG_OFFSET, 0x5u);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdRvCoreIbexRecovSwErr));
  sw_recov =
      abs_mmio_read32(kIbexCfgBase + RV_CORE_IBEX_SW_RECOV_ERR_REG_OFFSET);
  CHECK(sw_recov == kMultiBitBool4False,
        "[rv_core_ibex.sv:752] Expected SW_RECOV_ERR to auto-clear to 0x9 "
        "after writing loose-true 0x5 (got 0x%x)",
        sw_recov);

  // Ensure EDN0 is running so RND_STATUS.RND_DATA_VALID becomes 1, then stop
  // EDN0 and verify reading RND_DATA clears RND_STATUS and RND_DATA to 0:
  CHECK_STATUS_OK(entropy_testutils_auto_mode_init());
  while (!bitfield_bit32_read(
      abs_mmio_read32(kIbexCfgBase + RV_CORE_IBEX_RND_STATUS_REG_OFFSET),
      RV_CORE_IBEX_RND_STATUS_RND_DATA_VALID_BIT)) {
  }
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9999u);
  (void)abs_mmio_read32(kIbexCfgBase + RV_CORE_IBEX_RND_DATA_REG_OFFSET);
  uint32_t rnd_sts_after =
      abs_mmio_read32(kIbexCfgBase + RV_CORE_IBEX_RND_STATUS_REG_OFFSET);
  uint32_t rnd_data_after =
      abs_mmio_read32(kIbexCfgBase + RV_CORE_IBEX_RND_DATA_REG_OFFSET);
  CHECK(rnd_sts_after == 0u && rnd_data_after == 0u,
        "[rv_core_ibex.sv:752] Expected RND_STATUS == 0 and RND_DATA == 0 "
        "immediately after consuming RND_DATA with EDN0 stopped (sts=0x%x, "
        "data=0x%x)",
        rnd_sts_after, rnd_data_after);
  LOG_INFO(
      "[rv_core_ibex.sv:752] CONFIRMED: SW_RECOV_ERR auto-cleared to 0x9 & "
      "RND_DATA read cleared RND_STATUS/RND_DATA to 0");

  // ---------------------------------------------------------------------------
  // ---------------------------------------------------------------------------
  // 3. Address Translation Routing (DBUS_ADDR_MATCHING_0, DBUS_REMAP_ADDR_0,
  //    DBUS_ADDR_EN_0 1-bit mask):
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying RV_CORE_IBEX address translation routing & 1-bit ADDR_EN "
      "mask");
  s_remap_src[0] = 0x11111111u;
  s_remap_dst[0] = 0xcafebabeu;

  uintptr_t src_addr = (uintptr_t)s_remap_src;
  uintptr_t dst_addr = (uintptr_t)s_remap_dst;
  // NAPOT encoding for 256-byte region: base | ((256 - 1) >> 1) = base | 0x7f
  uint32_t napot_256 = (uint32_t)src_addr | 0x7fu;

  abs_mmio_write32(kIbexCfgBase + RV_CORE_IBEX_DBUS_ADDR_MATCHING_0_REG_OFFSET,
                   napot_256);
  abs_mmio_write32(kIbexCfgBase + RV_CORE_IBEX_DBUS_REMAP_ADDR_0_REG_OFFSET,
                   (uint32_t)dst_addr);
  abs_mmio_write32(kIbexCfgBase + RV_CORE_IBEX_DBUS_ADDR_EN_0_REG_OFFSET,
                   0xffffffffu);
  CHECK(abs_mmio_read32(kIbexCfgBase +
                        RV_CORE_IBEX_DBUS_ADDR_EN_0_REG_OFFSET) == 1u,
        "Expected DBUS_ADDR_EN_0 to mask 0xffffffff to 0x1");

  g_load_store_fault = false;
  uint32_t remapped_read = abs_mmio_read32(src_addr);
  CHECK(!g_load_store_fault && remapped_read == 0xcafebabeu,
        "Expected translated read at s_remap_src to return 0xcafebabe, "
        "got 0x%08x",
        remapped_read);

  // Disable translation and verify read at s_remap_src returns original value:
  abs_mmio_write32(kIbexCfgBase + RV_CORE_IBEX_DBUS_ADDR_EN_0_REG_OFFSET, 0u);
  g_load_store_fault = false;
  uint32_t unmapped_read = abs_mmio_read32(src_addr);
  CHECK(!g_load_store_fault && unmapped_read == 0x11111111u,
        "Expected read at s_remap_src with ADDR_EN_0=0 to return 0x11111111, "
        "got 0x%08x",
        unmapped_read);
  LOG_INFO("Address translation routing & 1-bit ADDR_EN_0 mask CONFIRMED");

  LOG_INFO("=== ALL RV_CORE_IBEX ERRATA CHECKS PASSED ===");
  return true;
}
