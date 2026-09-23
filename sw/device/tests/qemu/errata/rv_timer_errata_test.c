// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file rv_timer_errata_test.c
 * @brief CW340 FPGA & QEMU Hardware Errata Confirmation Test for `rv_timer`
 * (`P16`).
 *
 * Empirically confirms the taped-out OpenTitan Earlgrey `rv_timer` silicon
 * errata, specification errata, and security hardening behaviors on physical
 * CW340 FPGA silicon and QEMU (`sim_qemu`):
 *
 * - `[timer_core.sv:32-39]` (`TRUE_SILICON_ERRATA` — `MEDIUM` / `MEDIUM
 * (65%)`): `CFG0` (`0x10c`) has no hardware `regwen` or `!CTRL.active0` lock
 *   (`rv_timer_reg_top.sv:525`), and `timer_core.sv:32-39` resets `tick_count`
 *   on exact equality (`tick_count == prescaler`) while driving `tick` on
 *   `active & (tick_count >= prescaler)`. Lowering `CFG0.prescale` below the
 *   in-flight `tick_count` while `CTRL.active0 == 1` misses the equality reset
 *   and asserts `tick = 1` on every single clock cycle until `tick_count` wraps
 *   `12'hfff -> 12'h000` (`(4096 - tick_count) * step` runaway burst).
 *
 * - `[timer_core.sv:39-46]` (`SPEC_DOC_ERRATA` — `LOW` / `HIGH (95%)`):
 *   Setting `CFG0.step = 0` halts `mtime` progression (`mtime_d = mtime + 0`),
 *   but the combinational comparator `intr[0] = active & (mtime >=
 * mtimecmp[0])`
 *   (`timer_core.sv:45`) remains continuously active and asserts `INTR_STATE0`
 *   whenever `mtime >= mtimecmp` (and re-asserts after `W1C` until `mtime <
 *   mtimecmp` or `CTRL.active0 = 0`).
 *
 * - `[rv_timer_reg_top.sv:118-119,477-492]` (`BENIGN_RTL_IMPL_DETAIL` / `E2`):
 *   The `{ skipto: "0x100" }` unmapped offset hole (`0x008..0x0fc`) between
 *   `CTRL` (`0x004`) and `INTR_ENABLE0` (`0x100`) asserts `addrmiss = 1` ->
 *   `d_error = 1` (`mcause = 5` Load Access Fault / `mcause = 7` Store Access
 *   Fault).
 *
 * - `[rv_timer_reg_pkg.sv:138-149]` (`INTENDED_SECURITY_HARDENING` — `INFO`):
 *   `RV_TIMER_PERMIT[5] = 4'b0111` (`rv_timer_reg_pkg.sv:144`,
 *   `rv_timer_reg_top.sv:495-507`) requires bytes 0, 1, and 2 simultaneously on
 *   `CFG0` (`0x10c`), causing every sub-word store (`sh` to `CFG0.prescale` or
 *   `sb` to `CFG0.step`) to fault with `mcause = 7` (`Store Access Fault`) and
 *   drop the write.
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "rv_timer_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kTimerBase = TOP_EARLGREY_RV_TIMER_BASE_ADDR,
};

static volatile uint32_t g_load_fault_count = 0;
static volatile uint32_t g_store_fault_count = 0;
static volatile uint32_t g_last_mcause = 0;

void ottf_exception_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = ibex_mcause_read();
  g_last_mcause = mcause;
  if (mcause == kIbexExcLoadAccessFault) {
    g_load_fault_count++;
    return;
  }
  if (mcause == kIbexExcStoreAccessFault) {
    g_store_fault_count++;
    return;
  }
  CHECK(false, "Unexpected exception mcause=0x%08x", mcause);
}

static void rv_timer_reset_clean(void) {
  abs_mmio_write32(kTimerBase + RV_TIMER_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kTimerBase + RV_TIMER_INTR_ENABLE0_REG_OFFSET, 0u);
  abs_mmio_write32(kTimerBase + RV_TIMER_CFG0_REG_OFFSET,
                   (1u << RV_TIMER_CFG0_STEP_OFFSET) | 0u);
  abs_mmio_write32(kTimerBase + RV_TIMER_TIMER_V_LOWER0_REG_OFFSET, 0u);
  abs_mmio_write32(kTimerBase + RV_TIMER_TIMER_V_UPPER0_REG_OFFSET, 0u);
  abs_mmio_write32(kTimerBase + RV_TIMER_COMPARE_UPPER0_0_REG_OFFSET,
                   0xffffffffu);
  abs_mmio_write32(kTimerBase + RV_TIMER_COMPARE_LOWER0_0_REG_OFFSET,
                   0xffffffffu);
  abs_mmio_write32(kTimerBase + RV_TIMER_INTR_STATE0_REG_OFFSET, 1u);
}

static void test_cfg0_ungated_and_live_prescale_runaway(void) {
  LOG_INFO(
      "Verifying [timer_core.sv:32-39] (TRUE_SILICON_ERRATA): CFG0 "
      "ungated write while active & tick_count > new_prescaler 12-bit "
      "wrap runaway...");
  rv_timer_reset_clean();

  // 1. Start timer with large prescale = 4000 (0xfa0), step = 1, mtime = 0.
  abs_mmio_write32(kTimerBase + RV_TIMER_CFG0_REG_OFFSET,
                   (1u << RV_TIMER_CFG0_STEP_OFFSET) | 4000u);
  abs_mmio_write32(kTimerBase + RV_TIMER_CTRL_REG_OFFSET, 1u);

  // Spin 20 us (~480 peripheral clock cycles at 24 MHz). Because 480 < 4000,
  // tick_count is ~480 while mtime is still 0.
  busy_spin_micros(20);
  CHECK(abs_mmio_read32(kTimerBase + RV_TIMER_TIMER_V_LOWER0_REG_OFFSET) == 0u);

  // 2. Lower CFG0.prescale to 10 (< tick_count!) and set step = 10 while
  // CTRL.active0 == 1. Because tick_count (~125 at 6 MHz) > 10,
  // timer_core.sv:32-39 misses (tick_count == prescaler) and asserts tick=1 on
  // EVERY 6 MHz clock cycle until tick_count wraps 4095 -> 0!
  const uint32_t kFastCfg0 = (10u << RV_TIMER_CFG0_STEP_OFFSET) | 10u;
  abs_mmio_write32(kTimerBase + RV_TIMER_CFG0_REG_OFFSET, kFastCfg0);
  CHECK(abs_mmio_read32(kTimerBase + RV_TIMER_CFG0_REG_OFFSET) == kFastCfg0);
  busy_spin_micros(200);
  uint32_t mid_wrap_mtime =
      abs_mmio_read32(kTimerBase + RV_TIMER_TIMER_V_LOWER0_REG_OFFSET);
  CHECK(mid_wrap_mtime > 8000u,
        "Expected mid-wrap every-cycle runaway (>8000 at 200 us), got %u",
        mid_wrap_mtime);

  // Wait another 600 us (800 us total = 4800 cycles at 6 MHz > 4096) so
  // tick_count finishes wrapping 4095 -> 0 (~40,460 total increment).
  busy_spin_micros(600);
  uint32_t runaway_mtime =
      abs_mmio_read32(kTimerBase + RV_TIMER_TIMER_V_LOWER0_REG_OFFSET);
  CHECK(runaway_mtime > 30000u,
        "Expected full 12-bit wrap runaway burst (>30000 at 800 us), got %u",
        runaway_mtime);

  // 3. Compare against the documented workaround: deactivate CTRL.active0 = 0
  // (resetting tick_count <= 0) before writing CFG0 = kFastCfg0, then re-enable
  // CTRL.active0 = 1 for the exact same 800 us window (~4,360 increment).
  rv_timer_reset_clean();
  abs_mmio_write32(kTimerBase + RV_TIMER_CFG0_REG_OFFSET, kFastCfg0);
  abs_mmio_write32(kTimerBase + RV_TIMER_CTRL_REG_OFFSET, 1u);
  busy_spin_micros(800);
  uint32_t clean_mtime =
      abs_mmio_read32(kTimerBase + RV_TIMER_TIMER_V_LOWER0_REG_OFFSET);
  CHECK(clean_mtime > 2000u && clean_mtime < 10000u,
        "Expected clean mtime (<10000) when CTRL.active0=0 before CFG0 write, "
        "got %u",
        clean_mtime);
  CHECK(runaway_mtime > clean_mtime * 6u);

  rv_timer_reset_clean();
  LOG_INFO(
      "[timer_core.sv:32-39] confirmed: live prescale reduction caused "
      "mid_wrap=%u, full_wrap=%u vs clean mtime=%u.",
      mid_wrap_mtime, runaway_mtime, clean_mtime);
}

static void test_step_zero_combinational_intr(void) {
  LOG_INFO(
      "Verifying [timer_core.sv:39-46] (SPEC_DOC_ERRATA): CFG0.step==0 "
      "halts mtime but combinational (mtime >= mtimecmp) still asserts "
      "INTR_STATE0...");
  rv_timer_reset_clean();

  // Set CFG0.step = 0, prescale = 0, mtime = 100, mtimecmp = 200, active0 = 1.
  abs_mmio_write32(kTimerBase + RV_TIMER_CFG0_REG_OFFSET, 0u);
  abs_mmio_write32(kTimerBase + RV_TIMER_TIMER_V_LOWER0_REG_OFFSET, 100u);
  abs_mmio_write32(kTimerBase + RV_TIMER_COMPARE_UPPER0_0_REG_OFFSET, 0u);
  abs_mmio_write32(kTimerBase + RV_TIMER_COMPARE_LOWER0_0_REG_OFFSET, 200u);
  abs_mmio_write32(kTimerBase + RV_TIMER_INTR_STATE0_REG_OFFSET, 1u);
  abs_mmio_write32(kTimerBase + RV_TIMER_CTRL_REG_OFFSET, 1u);

  busy_spin_micros(50);
  CHECK(abs_mmio_read32(kTimerBase + RV_TIMER_TIMER_V_LOWER0_REG_OFFSET) ==
        100u);
  CHECK(abs_mmio_read32(kTimerBase + RV_TIMER_INTR_STATE0_REG_OFFSET) == 0u);

  // Lower COMPARE_LOWER0_0 to 100 while step == 0 and active0 == 1.
  // Combinational intr[0] = active & (mtime >= mtimecmp[0]) immediately asserts
  // INTR_STATE0!
  abs_mmio_write32(kTimerBase + RV_TIMER_COMPARE_LOWER0_0_REG_OFFSET, 100u);
  busy_spin_micros(5);
  CHECK(abs_mmio_read32(kTimerBase + RV_TIMER_INTR_STATE0_REG_OFFSET) == 1u);

  // Clearing INTR_STATE0 via W1C while active0 == 1 and mtime >= mtimecmp
  // re-asserts INTR_STATE0 because intr[0] is level-high.
  abs_mmio_write32(kTimerBase + RV_TIMER_INTR_STATE0_REG_OFFSET, 1u);
  busy_spin_micros(5);
  CHECK(abs_mmio_read32(kTimerBase + RV_TIMER_INTR_STATE0_REG_OFFSET) == 1u);

  // Raising COMPARE_LOWER0_0 to 101 (> mtime=100) clears INTR_STATE0 via
  // mtimecmp_update.
  abs_mmio_write32(kTimerBase + RV_TIMER_COMPARE_LOWER0_0_REG_OFFSET, 101u);
  CHECK(abs_mmio_read32(kTimerBase + RV_TIMER_INTR_STATE0_REG_OFFSET) == 0u);

  rv_timer_reset_clean();
  LOG_INFO(
      "[timer_core.sv:39-46] confirmed: step==0 freezes mtime=100 while "
      "mtime>=mtimecmp continuously asserts INTR_STATE0.");
}

static void test_unmapped_skipto_hole_bus_error(void) {
  LOG_INFO(
      "Verifying [rv_timer_reg_top.sv:118-119,477-492] "
      "(BENIGN_RTL_IMPL_DETAIL): "
      "unmapped skipto hole (0x008..0x0fc) raises synchronous Load/Store "
      "Access Faults (mcause=5/7)...");
  rv_timer_reset_clean();

  g_load_fault_count = 0;
  g_store_fault_count = 0;

  (void)abs_mmio_read32(kTimerBase + 0x008u);
  CHECK(g_load_fault_count == 1u);
  CHECK(g_last_mcause == kIbexExcLoadAccessFault);

  (void)abs_mmio_read32(kTimerBase + 0x0fcu);
  CHECK(g_load_fault_count == 2u);
  CHECK(g_last_mcause == kIbexExcLoadAccessFault);

  abs_mmio_write32(kTimerBase + 0x008u, 0x1u);
  CHECK(g_store_fault_count == 1u);
  CHECK(g_last_mcause == kIbexExcStoreAccessFault);

  LOG_INFO(
      "[rv_timer_reg_top.sv:118-119,477-492] confirmed: unmapped hole "
      "0x008..0x0fc "
      "faulted (load_faults=%u, store_faults=%u).",
      g_load_fault_count, g_store_fault_count);
}

static void test_cfg0_3byte_permit_subword_fault(void) {
  LOG_INFO(
      "Verifying [rv_timer_reg_pkg.sv:138-149] (INTENDED_SECURITY_HARDENING): "
      "RV_TIMER_PERMIT[5]=4'b0111 rejects all sub-word writes (sh/sb) to "
      "CFG0 with mcause=7...");
  rv_timer_reset_clean();

  g_store_fault_count = 0;
  g_last_mcause = 0;

  // 1-byte write to CTRL (0x004, PERMIT = 4'b0001) must succeed.
  abs_mmio_write8(kTimerBase + RV_TIMER_CTRL_REG_OFFSET, 0u);
  CHECK(g_store_fault_count == 0u);

  const uint32_t kExpectedCfg0 = (3u << RV_TIMER_CFG0_STEP_OFFSET) | 0x123u;
  abs_mmio_write32(kTimerBase + RV_TIMER_CFG0_REG_OFFSET, kExpectedCfg0);
  CHECK(abs_mmio_read32(kTimerBase + RV_TIMER_CFG0_REG_OFFSET) ==
        kExpectedCfg0);

  // 16-bit write (sh) to CFG0.prescale (bytes 0..1) faults with mcause=7
  // because PERMIT=4'b0111 requires byte 2 simultaneously!
  *((volatile uint16_t *)(kTimerBase + RV_TIMER_CFG0_REG_OFFSET)) = 0x0456u;
  CHECK(g_store_fault_count == 1u);
  CHECK(g_last_mcause == kIbexExcStoreAccessFault);
  CHECK(abs_mmio_read32(kTimerBase + RV_TIMER_CFG0_REG_OFFSET) ==
        kExpectedCfg0);

  // 8-bit write (sb) to CFG0.step (byte 2 at CFG0 + 2) also faults with
  // mcause=7 and drops the write.
  abs_mmio_write8(kTimerBase + RV_TIMER_CFG0_REG_OFFSET + 2u, 0x09u);
  CHECK(g_store_fault_count == 2u);
  CHECK(g_last_mcause == kIbexExcStoreAccessFault);
  CHECK(abs_mmio_read32(kTimerBase + RV_TIMER_CFG0_REG_OFFSET) ==
        kExpectedCfg0);

  rv_timer_reset_clean();
  LOG_INFO(
      "[rv_timer_reg_pkg.sv:138-149] confirmed: sub-word sh/sb writes to CFG0 "
      "faulted with mcause=7 (faults=%u).",
      g_store_fault_count);
}

bool test_main(void) {
  LOG_INFO("Starting rv_timer hardware errata confirmation suite...");
  test_cfg0_ungated_and_live_prescale_runaway();
  test_step_zero_combinational_intr();
  test_unmapped_skipto_hole_bus_error();
  test_cfg0_3byte_permit_subword_fault();
  LOG_INFO("All rv_timer errata checks confirmed on hardware!");
  return true;
}
