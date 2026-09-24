// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_rv_timer.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top/rv_timer_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kTimerBase = TOP_EARLGREY_RV_TIMER_BASE_ADDR,
};

static volatile uint32_t g_load_fault_count = 0;
static volatile uint32_t g_store_fault_count = 0;
static volatile uint32_t g_last_mcause = 0;

static void advance_mepc_over_faulting_insn(void) {
  uint32_t mepc = 0;
  CSR_READ(CSR_REG_MEPC, &mepc);
  uint16_t insn_half = *(const volatile uint16_t *)mepc;
  uint32_t step = ((insn_half & 0x3u) == 0x3u) ? 4u : 2u;
  CSR_WRITE(CSR_REG_MEPC, mepc + step);
}

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = 0;
  CSR_READ(CSR_REG_MCAUSE, &mcause);
  g_last_mcause = mcause;
  if (mcause == kIbexExcLoadAccessFault) {
    g_load_fault_count++;
  } else if (mcause == kIbexExcStoreAccessFault) {
    g_store_fault_count++;
  }
  advance_mepc_over_faulting_insn();
}

static void rv_timer_reset_clean(void) {
  abs_mmio_write32(kTimerBase + RV_TIMER_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kTimerBase + RV_TIMER_INTR_ENABLE0_REG_OFFSET, 0u);
  abs_mmio_write32(kTimerBase + RV_TIMER_CFG0_REG_OFFSET,
                   1u << RV_TIMER_CFG0_STEP_OFFSET);
  abs_mmio_write32(kTimerBase + RV_TIMER_COMPARE_UPPER0_0_REG_OFFSET,
                   0xffffffffu);
  abs_mmio_write32(kTimerBase + RV_TIMER_COMPARE_LOWER0_0_REG_OFFSET,
                   0xffffffffu);
  abs_mmio_write32(kTimerBase + RV_TIMER_TIMER_V_LOWER0_REG_OFFSET, 0u);
  abs_mmio_write32(kTimerBase + RV_TIMER_TIMER_V_UPPER0_REG_OFFSET, 0u);
  abs_mmio_write32(kTimerBase + RV_TIMER_INTR_STATE0_REG_OFFSET, 1u);
}

// -----------------------------------------------------------------------------
// 1. [rv_timer_reg_top.sv:587, timer_core.sv:31-43]:
//    CFG0 is writable while CTRL.active0 == 1, and lowering CFG0.prescale
//    below the in-flight tick_count misses (tick_count == prescaler) while
//    asserting tick = active & (tick_count >= prescaler) on every clock cycle
//    until tick_count wraps all 12 bits (4095 -> 0).
// -----------------------------------------------------------------------------
static void test_cfg0_ungated_and_live_prescale_runaway(void) {
  rv_timer_reset_clean();

  // Start timer with prescale = 4000, step = 1, mtime = 0.
  abs_mmio_write32(kTimerBase + RV_TIMER_CFG0_REG_OFFSET,
                   (1u << RV_TIMER_CFG0_STEP_OFFSET) | 4000u);
  abs_mmio_write32(kTimerBase + RV_TIMER_CTRL_REG_OFFSET, 1u);

  busy_spin_micros(20);
  CHECK(abs_mmio_read32(kTimerBase + RV_TIMER_TIMER_V_LOWER0_REG_OFFSET) == 0u);

  // Lower CFG0.prescale to 10 (< tick_count) and set step = 10 while active.
  const uint32_t kFastCfg0 = (10u << RV_TIMER_CFG0_STEP_OFFSET) | 10u;
  abs_mmio_write32(kTimerBase + RV_TIMER_CFG0_REG_OFFSET, kFastCfg0);
  CHECK(abs_mmio_read32(kTimerBase + RV_TIMER_CFG0_REG_OFFSET) == kFastCfg0);
  busy_spin_micros(200);
  uint32_t mid_wrap_mtime =
      abs_mmio_read32(kTimerBase + RV_TIMER_TIMER_V_LOWER0_REG_OFFSET);
  CHECK(mid_wrap_mtime > 8000u,
        "Expected mid-wrap every-cycle runaway (>8000 at 200 us), got %u",
        mid_wrap_mtime);

  busy_spin_micros(600);
  uint32_t runaway_mtime =
      abs_mmio_read32(kTimerBase + RV_TIMER_TIMER_V_LOWER0_REG_OFFSET);
  CHECK(runaway_mtime > 30000u,
        "Expected full 12-bit wrap runaway burst (>30000 at 800 us), got %u",
        runaway_mtime);

  // Compare against deactivating CTRL.active0 = 0 before writing CFG0:
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
  LOG_INFO("Test 1 (CFG0 ungated & live-prescaler 12-bit wrap runaway) PASSED");
}

// -----------------------------------------------------------------------------
// 2. [timer_core.sv:39-46, rv_timer.hjson:47]:
//    CFG0.step == 0 freezes mtime increments (mtime_d = mtime + 0), yet the
//    combinational comparator intr[0] = active & (mtime >= mtimecmp[0])
//    immediately asserts INTR_STATE0 whenever mtime >= mtimecmp.
// -----------------------------------------------------------------------------
static void test_step_zero_combinational_intr(void) {
  rv_timer_reset_clean();

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

  // Lower COMPARE_LOWER0_0 to 100 while step == 0 and active0 == 1:
  abs_mmio_write32(kTimerBase + RV_TIMER_COMPARE_LOWER0_0_REG_OFFSET, 100u);
  busy_spin_micros(5);
  CHECK(abs_mmio_read32(kTimerBase + RV_TIMER_INTR_STATE0_REG_OFFSET) == 1u);

  // W1C cannot clear INTR_STATE0 while active0 == 1 and mtime >= mtimecmp:
  abs_mmio_write32(kTimerBase + RV_TIMER_INTR_STATE0_REG_OFFSET, 1u);
  busy_spin_micros(5);
  CHECK(abs_mmio_read32(kTimerBase + RV_TIMER_INTR_STATE0_REG_OFFSET) == 1u);

  // Raising COMPARE_LOWER0_0 to 101 (> mtime=100) clears INTR_STATE0:
  abs_mmio_write32(kTimerBase + RV_TIMER_COMPARE_LOWER0_0_REG_OFFSET, 101u);
  CHECK(abs_mmio_read32(kTimerBase + RV_TIMER_INTR_STATE0_REG_OFFSET) == 0u);

  rv_timer_reset_clean();
  LOG_INFO(
      "Test 2 (CFG0.step == 0 combinational mtime >= mtimecmp IRQ) PASSED");
}

// -----------------------------------------------------------------------------
// 3. [rv_timer_reg_top.sv:540, rv_timer.hjson:129]:
//    Unmapped skipto offset hole (0x008..0x0fc) asserts addrmiss = 1
//    (tl_o.d_error = 1), raising Load/Store Access Faults (mcause = 5/7).
// -----------------------------------------------------------------------------
static void test_unmapped_skipto_hole_bus_error(void) {
  rv_timer_reset_clean();

  g_load_fault_count = 0;
  g_store_fault_count = 0;

  (void)abs_mmio_read32(kTimerBase + 0x008u);
  CHECK(g_load_fault_count == 1u && g_last_mcause == kIbexExcLoadAccessFault);

  (void)abs_mmio_read32(kTimerBase + 0x0fcu);
  CHECK(g_load_fault_count == 2u && g_last_mcause == kIbexExcLoadAccessFault);

  abs_mmio_write32(kTimerBase + 0x008u, 0x1u);
  CHECK(g_store_fault_count == 1u && g_last_mcause == kIbexExcStoreAccessFault);

  LOG_INFO("Test 3 (Unmapped 0x008..0x0fc skipto hole addrmiss fault) PASSED");
}

// -----------------------------------------------------------------------------
// 4. [rv_timer_reg_pkg.sv:146-157, rv_timer_reg_top.sv:557-569]:
//    RV_TIMER_PERMIT[5] = 4'b0111 on CFG0 (0x10c) rejects all sub-word stores
//    (sh to CFG0.prescale or sb to CFG0.step) with Store Access Fault
//    (mcause=7).
// -----------------------------------------------------------------------------
static void test_cfg0_3byte_permit_subword_fault(void) {
  rv_timer_reset_clean();

  g_store_fault_count = 0;
  g_last_mcause = 0;

  abs_mmio_write8(kTimerBase + RV_TIMER_CTRL_REG_OFFSET, 0u);
  CHECK(g_store_fault_count == 0u);

  const uint32_t kExpectedCfg0 = (3u << RV_TIMER_CFG0_STEP_OFFSET) | 0x123u;
  abs_mmio_write32(kTimerBase + RV_TIMER_CFG0_REG_OFFSET, kExpectedCfg0);
  CHECK(abs_mmio_read32(kTimerBase + RV_TIMER_CFG0_REG_OFFSET) ==
        kExpectedCfg0);

  *((volatile uint16_t *)(kTimerBase + RV_TIMER_CFG0_REG_OFFSET)) = 0x0456u;
  CHECK(g_store_fault_count == 1u && g_last_mcause == kIbexExcStoreAccessFault);
  CHECK(abs_mmio_read32(kTimerBase + RV_TIMER_CFG0_REG_OFFSET) ==
        kExpectedCfg0);

  abs_mmio_write8(kTimerBase + RV_TIMER_CFG0_REG_OFFSET + 2u, 0x09u);
  CHECK(g_store_fault_count == 2u && g_last_mcause == kIbexExcStoreAccessFault);
  CHECK(abs_mmio_read32(kTimerBase + RV_TIMER_CFG0_REG_OFFSET) ==
        kExpectedCfg0);

  rv_timer_reset_clean();
  LOG_INFO("Test 4 (RV_TIMER_PERMIT[5]=4'b0111 CFG0 sub-word fault) PASSED");
}

// -----------------------------------------------------------------------------
// 5. NEW_IN_V2 [programmers_guide.md:50-68, dif_rv_timer.c:153-180,
//    rv_timer.sv:75,82-83]:
//    In trunk-v2, programmers_guide.md:50-68 added a 3-write mtime update
//    sequence (using non-existent RV_TIMER_TIMER_V_LOWER0_0_REG_OFFSET /
//    RV_TIMER_TIMER_V_UPPER0_0_REG_OFFSET macro names) and noted that mtimecmp
//    must be written after mtime to clear pending interrupts because
//    mtimecmp_update (rv_timer.sv:75) only pulses on COMPARE_LOWER0_0 /
//    COMPARE_UPPER0_0 writes. However, dif_rv_timer_counter_write()
//    (dif_rv_timer.c:153-180) neither uses the 3-write sequence nor writes
//    COMPARE_* or INTR_STATE0, so rewinding mtime below mtimecmp via
//    dif_rv_timer_counter_write() after an expiration leaves INTR_STATE0 == 1
//    stuck asserted until COMPARE_LOWER0_0 / COMPARE_UPPER0_0 is written!
// -----------------------------------------------------------------------------
static void test_v2_mtime_rewind_sticky_intr_and_dif_counter_write(void) {
  rv_timer_reset_clean();

  dif_rv_timer_t timer;
  CHECK_DIF_OK(dif_rv_timer_init(mmio_region_from_addr(kTimerBase), &timer));

  // Configure step = 0, mtime = 500, mtimecmp = 400, active0 = 1 -> expired!
  abs_mmio_write32(kTimerBase + RV_TIMER_CFG0_REG_OFFSET, 0u);
  abs_mmio_write32(kTimerBase + RV_TIMER_TIMER_V_UPPER0_REG_OFFSET, 0u);
  abs_mmio_write32(kTimerBase + RV_TIMER_TIMER_V_LOWER0_REG_OFFSET, 500u);
  abs_mmio_write32(kTimerBase + RV_TIMER_COMPARE_UPPER0_0_REG_OFFSET, 0u);
  abs_mmio_write32(kTimerBase + RV_TIMER_COMPARE_LOWER0_0_REG_OFFSET, 400u);
  abs_mmio_write32(kTimerBase + RV_TIMER_CTRL_REG_OFFSET, 1u);
  busy_spin_micros(5);
  CHECK(abs_mmio_read32(kTimerBase + RV_TIMER_INTR_STATE0_REG_OFFSET) == 1u);

  // Rewind mtime to 10 (< mtimecmp = 400) via dif_rv_timer_counter_write():
  CHECK_DIF_OK(dif_rv_timer_counter_write(&timer, 0, 10ull));
  uint64_t now = 0;
  CHECK_DIF_OK(dif_rv_timer_counter_read(&timer, 0, &now));
  CHECK(now == 10ull, "Expected mtime == 10 after dif_rv_timer_counter_write");

  // Even though mtime (10) < mtimecmp (400), INTR_STATE0 remains stuck at 1!
  CHECK(abs_mmio_read32(kTimerBase + RV_TIMER_INTR_STATE0_REG_OFFSET) == 1u,
        "[dif_rv_timer.c:153-180, rv_timer.sv:75,82-83] Expected INTR_STATE0 "
        "to remain sticky 1 after dif_rv_timer_counter_write(10 < 400)");

  // Executing the programmers_guide.md:58-60 3-write mtime sequence
  // (LOWER0 = 0, UPPER0 = 0, LOWER0 = 20) also leaves INTR_STATE0 == 1 until
  // COMPARE_LOWER0_0 is re-written with its existing value (400), which pulses
  // mtimecmp_update[0][0] = 1 and clears INTR_STATE0 to 0!
  abs_mmio_write32(kTimerBase + RV_TIMER_TIMER_V_LOWER0_REG_OFFSET, 0u);
  abs_mmio_write32(kTimerBase + RV_TIMER_TIMER_V_UPPER0_REG_OFFSET, 0u);
  abs_mmio_write32(kTimerBase + RV_TIMER_TIMER_V_LOWER0_REG_OFFSET, 20u);
  CHECK(abs_mmio_read32(kTimerBase + RV_TIMER_INTR_STATE0_REG_OFFSET) == 1u);

  abs_mmio_write32(kTimerBase + RV_TIMER_COMPARE_LOWER0_0_REG_OFFSET, 400u);
  CHECK(abs_mmio_read32(kTimerBase + RV_TIMER_INTR_STATE0_REG_OFFSET) == 0u,
        "Expected dummy write of existing mtimecmp (400) to COMPARE_LOWER0_0 "
        "to pulse mtimecmp_update and clear INTR_STATE0 to 0");

  rv_timer_reset_clean();
  LOG_INFO(
      "Test 5 (dif_rv_timer_counter_write & programmers_guide mtime update vs "
      "mtimecmp_update clear) PASSED");
}

bool test_main(void) {
  LOG_INFO("=== OpenTitan Earlgrey v2 (trunk-v2) RV_TIMER Errata Suite ===");
  test_cfg0_ungated_and_live_prescale_runaway();
  test_step_zero_combinational_intr();
  test_unmapped_skipto_hole_bus_error();
  test_cfg0_3byte_permit_subword_fault();
  test_v2_mtime_rewind_sticky_intr_and_dif_counter_write();
  LOG_INFO("=== ALL RV_TIMER ERRATA-V2 CHECKS PASSED ===");
  return true;
}
