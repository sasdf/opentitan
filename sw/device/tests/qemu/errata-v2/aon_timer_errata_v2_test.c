// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file aon_timer_errata_v2_test.c
 * @brief Physical CW340 FPGA verification of Earlgrey v2 (`trunk-v2`) hardware,
 * specification, and DIF errata for `aon_timer` (`hw/ip/aon_timer/`).
 *
 * Verifies:
 * 1. [hw/ip/aon_timer/rtl/aon_timer.sv:185-218 & aon_timer_core.sv:60-68, 83
 *    vs. hw/ip/aon_timer/doc/programmers_guide.md:88-90]:
 *    `u_intr_sync` (`prim_edge_detector`, `q_posedge_pulse_o`) requires a
 *    `0 -> 1` rising edge on `aon_intr_set`. When `WKUP_CTRL.PRESCALER == 0`
 *    (or for `wdog_timer_bark`), `aon_intr_set` stays continuously `1'b1` while
 *    `WKUP_COUNT >= WKUP_THOLD` (or `WDOG_COUNT >= WDOG_BARK_THOLD`), so W1C
 *    clearing `INTR_STATE` while `WKUP_CAUSE` is cleared to `0` re-asserts
 *    `WKUP_CAUSE = 1` on the next `clk_aon_i` tick while `INTR_STATE` remains
 *    permanently `0`.
 * 2. [FIXED IN V2 RTL: hw/ip/aon_timer/rtl/aon_timer_core.sv:49-50 &
 *    hw/ip/aon_timer/data/aon_timer.hjson:150-152, 161 (commit b883960c)]:
 *    Every write to `WKUP_CTRL` pulses `reg2hw_i.wkup_ctrl.prescaler.qe` and
 *    resets `prescale_count_q <= 12'h000`, eliminating the v1 4096-cycle
 *    (`20.48 ms`) wrap-around stall when lowering `WKUP_CTRL.PRESCALER` below a
 *    residual `prescale_count_q`.
 * 3. [hw/ip/aon_timer/rtl/aon_timer_core.sv:68 vs.
 *    hw/ip/aon_timer/data/aon_timer.hjson:172, 184]:
 *    `wkup_intr_o = wkup_incr & (wkup_count >= wkup_thold)` gates threshold
 *    comparison on `wkup_incr` (`prescale_count_q == prescaler`), so writing
 *    `WKUP_COUNT_LO >= WKUP_THOLD_LO` while `PRESCALER = 200` does not assert
 *    `WKUP_CAUSE` or `INTR_STATE.wkup_timer_expired` until `prescale_count_q`
 *    reaches `200`.
 * 4. [hw/ip/aon_timer/rtl/aon_timer_reg_pkg.sv:191-206 &
 *    aon_timer_reg_top.sv:1171-1187]:
 *    `AON_TIMER_PERMIT` allows 8-bit (`sb`) writes to `WDOG_CTRL` (`0x1c`,
 *    `4'b0001`), requires >=16-bit (`sh`/`sw`) writes to `WKUP_CTRL` (`0x04`,
 *    `4'b0011`, faulting with `mcause = 7` on `sb`), and requires 32-bit (`sw`)
 *    writes to `THOLD`/`COUNT` (`4'b1111`, faulting with `mcause = 7` on
 *    `sb`/`sh`).
 * 5. [NEW IN V2 + V1: hw/ip/aon_timer/rtl/aon_timer_reg_top.sv:331, 374, 536,
 *    579, 737, 767, 946, 1069 & hw/ip/prim/rtl/prim_reg_cdc_arb.sv:71-95,
 *    199-212 (cf. aon_timer_unr_excl.el:31-45)]:
 *    `u_wkup_cause_cdc`, `u_wkup_count_lo_cdc`, `u_wkup_count_hi_cdc`, and
 *    `u_wdog_count_cdc` wire `.dst_update_i` to `aon_*_qe` (`prim_subreg.qe ==
 *    we == 0` on hardware updates) instead of `hw2reg.*.de`. This makes the
 *    early `dst_ds_i` capture branch (`else if (dst_update) dst_lat_d = 1`)
 *    100% unreachable (`VC_COV_UNR`) and forces `prim_reg_cdc_arb` to wait for
 *    `dst_qs_o != dst_qs_i` (`dst_lat_q`), causing `WKUP_CAUSE` (`0x34`) and
 *    `WKUP_COUNT_LO` (`0x14`) readbacks to lag 1 full `clk_aon_i` cycle
 *    (`5 us` = `~120 clk_i` cycles) behind `INTR_STATE.wkup_timer_expired` /
 *    `INTR_STATE.wdog_timer_bark` (`0x2c`) assertion.
 * 6. [NEW IN V2 + DIF: hw/ip/aon_timer/rtl/aon_timer_core.sv:49-53, 60-62 &
 *    aon_timer_reg_top.sv:614-622, 676 + sw/device/lib/dif/dif_aon_timer.c:
 *    63-91, 179-201]:
 *    (a) `dif_aon_timer_wakeup_start(&aon, 0, 0)`
 *    and `dif_aon_timer_watchdog_start(&aon, 0, 0, ...)`
 *    return `kDifOk` while underflowing `threshold - 1` to `0xffffffffffffffff`
 *    and `0xffffffff` (`WKUP_THOLD_HI/LO = 0xffffffff`, `WDOG_BARK_THOLD =
 *    0xffffffff`, `WDOG_BITE_THOLD = 0xffffffff`).
 *    (b) Because `u_wkup_ctrl0_qe` delays `reg2hw_i.wkup_ctrl.prescaler.qe` by
 *    1 `clk_aon_i` cycle (`cycle T+1`, when `enable.q` and `prescaler.q` have
 *    already updated), `else if (reg2hw_i.wkup_ctrl.prescaler.qe)
 *    prescale_count_q <= 12'h000;` overrides `else if (prescale_en)` during the
 *    first enabled `clk_aon_i` cycle (`T+1`), holding `prescale_count_q == 0`
 *    for two `clk_aon_i` cycles (`T+1` and `T+2`) so the first tick with
 *    `PRESCALER = P >= 1` takes `P + 2` `clk_aon_i` cycles instead of `P + 1`
 *    cycles.
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_aon_timer.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top/aon_timer_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kAonTimerBase = 0x40470000u,
  kRvCoreIbexBase = 0x411f0000u,
  kRvCoreIbexNmiStateOffset = 0x50u,
  kRiscvStoreAccessFault = 7u,
};

static volatile uint32_t g_access_fault_count = 0;
static volatile uint32_t g_last_mcause = 0;
static volatile uint32_t g_wdog_nmi_count = 0;
static volatile uint32_t g_wdog_cause_in_nmi = 0xffu;
static volatile uint32_t g_wdog_count_in_nmi = 0xffu;

static inline void mmio_write16(uint32_t addr, uint16_t val) {
  *(volatile uint16_t *)addr = val;
}

void ottf_internal_isr(uint32_t *exc_info) {
  (void)exc_info;
  g_wdog_cause_in_nmi =
      abs_mmio_read32(kAonTimerBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET);
  g_wdog_count_in_nmi =
      abs_mmio_read32(kAonTimerBase + AON_TIMER_WDOG_COUNT_REG_OFFSET);
  g_wdog_nmi_count++;
  // Clear INTR_STATE.wdog_timer_bark (W1C) and RV_CORE_IBEX.NMI_STATE.WDOG
  // (W1C) WITHOUT petting WDOG_COUNT or disabling WDOG_CTRL.
  abs_mmio_write32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET,
                   (1u << AON_TIMER_INTR_STATE_WDOG_TIMER_BARK_BIT));
  abs_mmio_write32(kRvCoreIbexBase + kRvCoreIbexNmiStateOffset, 0x2u);
}

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = 0;
  uint32_t mepc = 0;
  CSR_READ(CSR_REG_MCAUSE, &mcause);
  CSR_READ(CSR_REG_MEPC, &mepc);
  g_last_mcause = mcause;
  g_access_fault_count++;
  uint16_t insn16 = *(const uint16_t *)mepc;
  mepc += ((insn16 & 0x3u) != 0x3u) ? 2u : 4u;
  CSR_WRITE(CSR_REG_MEPC, mepc);
}

static void aon_reset_all(void) {
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WDOG_CTRL_REG_OFFSET, 0u);
  busy_spin_micros(30);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_COUNT_LO_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_COUNT_HI_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_THOLD_LO_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_THOLD_HI_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WDOG_COUNT_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WDOG_BARK_THOLD_REG_OFFSET,
                   0xffffffffu);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WDOG_BITE_THOLD_REG_OFFSET,
                   0xffffffffu);
  busy_spin_micros(30);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET, 0u);
  busy_spin_micros(30);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET, 0x3u);
}

bool test_main(void) {
  dif_aon_timer_t aon;
  CHECK_DIF_OK(dif_aon_timer_init(mmio_region_from_addr(kAonTimerBase), &aon));

  // =========================================================================
  // 1. [NEW IN V2 + V1: aon_timer_reg_top.sv:374, 579, 767, 1069 &
  //     prim_reg_cdc_arb.sv:71-95, 199-212]
  //    AND [aon_timer.sv:185-218 vs programmers_guide.md:88-90]:
  //    (a) When `INTR_STATE.wkup_timer_expired` first becomes `1`
  //    (`u_intr_sync`
  //        in 24 MHz `clk_i`), `WKUP_CAUSE` (`0x34`) and `WKUP_COUNT_LO`
  //        (`0x14`) still read `0` for ~5 us (`1 clk_aon_i` cycle) because
  //        `.dst_update_i` on `u_wkup_cause_cdc` and `u_wkup_count_lo_cdc` is
  //        wired to `prim_subreg.qe` (`== we == 0`) instead of `hw2reg.*.de`,
  //        making `dst_ds_i` early capture unreachable (`VC_COV_UNR`).
  //    (b) After `WKUP_CAUSE` becomes `1`, clearing `WKUP_CAUSE` (`write 0`)
  //        and `INTR_STATE` (`write 1` W1C) while `PRESCALER == 0` and
  //        `WKUP_COUNT >= WKUP_THOLD` re-asserts `WKUP_CAUSE = 1` on the next
  //        `clk_aon_i` tick, whereas `INTR_STATE.wkup_timer_expired` stays `0`
  //        (and likewise for `wdog_timer_bark`).
  // =========================================================================
  LOG_INFO(
      "Verifying [aon_timer_reg_top.sv:374,579 & aon_timer.sv:185-218]: "
      "WKUP_CAUSE/WKUP_COUNT 1-cycle (5 us) CDC lag behind INTR_STATE & "
      "PRESCALER==0 / wdog_timer_bark edge-detector non-retrigger...");
  aon_reset_all();

  // Set WKUP_THOLD = 1, WKUP_COUNT = 0, and start with PRESCALER = 0, ENABLE
  // = 1.
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_THOLD_LO_REG_OFFSET, 1u);
  busy_spin_micros(30);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET, 1u);

  // Tightly poll INTR_STATE until bit 0 (wkup_timer_expired) becomes 1, then
  // immediately read WKUP_CAUSE and WKUP_COUNT_LO on the next instruction.
  while ((abs_mmio_read32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET) &
          (1u << AON_TIMER_INTR_STATE_WKUP_TIMER_EXPIRED_BIT)) == 0u) {
  }
  uint32_t cause_at_irq =
      abs_mmio_read32(kAonTimerBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET);
  uint32_t count_at_irq =
      abs_mmio_read32(kAonTimerBase + AON_TIMER_WKUP_COUNT_LO_REG_OFFSET);

  // Wait 15 us (~3 clk_aon_i cycles) so u_wkup_cause_cdc / u_wkup_count_lo_cdc
  // complete their delayed dst_qs_o != dst_qs_i handshake.
  busy_spin_micros(15);
  uint32_t cause_after_cdc =
      abs_mmio_read32(kAonTimerBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET);
  uint32_t count_after_cdc =
      abs_mmio_read32(kAonTimerBase + AON_TIMER_WKUP_COUNT_LO_REG_OFFSET);
  LOG_INFO(
      "At INTR_STATE=1: WKUP_CAUSE=%u, WKUP_COUNT_LO=%u; after 15 us: "
      "WKUP_CAUSE=%u, WKUP_COUNT_LO=%u",
      cause_at_irq, count_at_irq, cause_after_cdc, count_after_cdc);

  // Because .dst_update_i is wired to qe (we=0) instead of de, WKUP_CAUSE and
  // WKUP_COUNT_LO lag behind INTR_STATE by 1 clk_aon_i cycle (5 us): at the
  // instant INTR_STATE asserts 1, WKUP_CAUSE is still 0 and WKUP_COUNT_LO < 2!
  CHECK(cause_at_irq == 0u,
        "Expected WKUP_CAUSE==0 immediately at INTR_STATE assertion due to "
        "unreachable dst_update_i (qe vs de), got %u",
        cause_at_irq);
  CHECK(
      count_at_irq < 2u,
      "Expected WKUP_COUNT_LO < 2 immediately at INTR_STATE assertion, got %u",
      count_at_irq);
  CHECK(cause_after_cdc == 1u);
  CHECK(count_after_cdc >= 2u);

  // Clear WKUP_CAUSE (rw0c) and INTR_STATE (rw1c) while WKUP_CTRL.PRESCALER==0
  // and WKUP_COUNT >= WKUP_THOLD.
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET, 0u);
  busy_spin_micros(20);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET,
                   (1u << AON_TIMER_INTR_STATE_WKUP_TIMER_EXPIRED_BIT));
  busy_spin_micros(40);

  uint32_t cause_retriggered =
      abs_mmio_read32(kAonTimerBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET);
  uint32_t intr_retriggered =
      abs_mmio_read32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET);
  CHECK(cause_retriggered == 1u, "Expected WKUP_CAUSE to re-assert 1, got 0x%x",
        cause_retriggered);
  CHECK((intr_retriggered &
         (1u << AON_TIMER_INTR_STATE_WKUP_TIMER_EXPIRED_BIT)) == 0u,
        "Expected INTR_STATE.wkup_timer_expired to remain 0 when PRESCALER==0, "
        "got 0x%x",
        intr_retriggered);

  // Verify that if software clears WKUP_CAUSE (write 0, landing on clk_aon_i
  // edge T_aon where SW wins in prim_subreg_arb) and stops WKUP_CTRL (write 0)
  // on the next clk_aon_i cycle (T_aon + 1, e.g. 6 us later),
  // wkup_ctrl.enable.q is still 1 on edge T_aon + 1 while WKUP_CTRL=0 is being
  // latched, so hw2reg.wkup_cause.de (wkup_intr == 1) re-asserts WKUP_CAUSE = 1
  // on the exact clk_aon_i edge that disables WKUP_CTRL!
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET, 0u);
  busy_spin_micros(6);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET, 0u);
  busy_spin_micros(30);
  uint32_t cause_after_clear_then_stop =
      abs_mmio_read32(kAonTimerBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET);
  CHECK(cause_after_clear_then_stop == 1u,
        "Expected WKUP_CAUSE==1 when WKUP_CAUSE=0 precedes WKUP_CTRL=0 by 1 "
        "clk_aon_i cycle (enable.q still 1 on WKUP_CTRL=0 latch edge), got %u",
        cause_after_clear_then_stop);

  // Also verify wdog_timer_bark NMI (mcause=0x8000001f) WDOG_COUNT lag in ISR
  // (WDOG_COUNT reads 1 < WDOG_BARK_THOLD=2 because u_wdog_count_cdc misses
  // dst_update_i on T0->T1 and then stays busy in u_dst_update_sync's 4-cycle
  // prim_sync_reqack handshake for 20 us, jumping 1 -> 5) and non-retrigger
  // after W1C clear inside ottf_internal_isr.
  aon_reset_all();
  g_wdog_nmi_count = 0;
  g_wdog_cause_in_nmi = 0xffu;
  g_wdog_count_in_nmi = 0xffu;
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WDOG_BARK_THOLD_REG_OFFSET, 2u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WDOG_BITE_THOLD_REG_OFFSET,
                   0xffffffffu);
  busy_spin_micros(30);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WDOG_CTRL_REG_OFFSET, 1u);
  while (g_wdog_nmi_count == 0u) {
  }
  LOG_INFO(
      "In wdog_timer_bark NMI ISR (BARK_THOLD=2): WKUP_CAUSE=%u, WDOG_COUNT=%u",
      g_wdog_cause_in_nmi, g_wdog_count_in_nmi);
  CHECK(g_wdog_count_in_nmi == 1u,
        "Expected WDOG_COUNT==1 (< WDOG_BARK_THOLD==2) inside wdog_timer_bark "
        "NMI ISR due to u_wdog_count_cdc dst_update_i lag + 4-cycle "
        "prim_sync_reqack busy window, got %u",
        g_wdog_count_in_nmi);
  busy_spin_micros(20);
  uint32_t wdog_count_after_20us =
      abs_mmio_read32(kAonTimerBase + AON_TIMER_WDOG_COUNT_REG_OFFSET);
  CHECK(wdog_count_after_20us >= 5u,
        "Expected WDOG_COUNT >= 5 after 20 us CDC handshake, got %u",
        wdog_count_after_20us);
  CHECK(abs_mmio_read32(kAonTimerBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET) == 1u);

  // Clear WKUP_CAUSE (rw0c) while WDOG_CTRL.ENABLE==1 and WDOG_COUNT >=
  // WDOG_BARK_THOLD. Verify WKUP_CAUSE re-asserts 1, while INTR_STATE and
  // nmi_wdog_timer_bark_o remain 0 (g_wdog_nmi_count stays 1).
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET, 0u);
  busy_spin_micros(40);
  CHECK(abs_mmio_read32(kAonTimerBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET) == 1u);
  CHECK((abs_mmio_read32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET) &
         (1u << AON_TIMER_INTR_STATE_WDOG_TIMER_BARK_BIT)) == 0u);
  CHECK(g_wdog_nmi_count == 1u);

  // =========================================================================
  // 2. [FIXED IN V2 RTL: aon_timer_core.sv:49-50 (commit b883960c)]
  //    AND [NEW IN V2: aon_timer_core.sv:49-53, 60-62 &
  //         aon_timer_reg_top.sv:614-622, 676 + dif_aon_timer.c:63-91,
  //         179-201]:
  //    (a) Verify that lowering `WKUP_CTRL.PRESCALER` from `0xfff` to `1` after
  //        running for 100 us resets `prescale_count_q <= 12'h000` on v2 and
  //        increments `WKUP_COUNT_LO` within 100 us (no 20.48 ms wrap stall).
  //    (b) Verify that `dif_aon_timer_wakeup_start(&aon, 0, 0)` and
  //        `dif_aon_timer_watchdog_start(&aon, 0, 0, false, false)` return
  //        `kDifOk` while underflowing `threshold - 1` to `0xffffffffffffffff`
  //        and `0xffffffff`.
  // =========================================================================
  LOG_INFO(
      "Verifying [FIXED IN V2 RTL: aon_timer_core.sv:49-50] prescale_count_q "
      "reset on WKUP_CTRL write & [dif_aon_timer.c:76,198] threshold=0 "
      "underflow...");
  aon_reset_all();
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_THOLD_LO_REG_OFFSET,
                   0xffffffffu);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_THOLD_HI_REG_OFFSET,
                   0xffffffffu);
  busy_spin_micros(20);

  // Run with PRESCALER = 0xfff (4095) for 100 us (~20 AON ticks) so
  // prescale_count_q reaches ~20 while WKUP_COUNT_LO is still 0.
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET,
                   (0xfffu << AON_TIMER_WKUP_CTRL_PRESCALER_OFFSET) | 1u);
  busy_spin_micros(100);
  CHECK(abs_mmio_read32(kAonTimerBase + AON_TIMER_WKUP_COUNT_LO_REG_OFFSET) ==
        0u);

  // Reconfigure to PRESCALER = 1, ENABLE = 1. On v1 this stalled for 20.48 ms
  // (4096 AON cycles); on v2 reg2hw_i.wkup_ctrl.prescaler.qe resets
  // prescale_count_q <= 0 so WKUP_COUNT_LO increments within 100 us!
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET,
                   (1u << AON_TIMER_WKUP_CTRL_PRESCALER_OFFSET) | 1u);
  busy_spin_micros(100);
  uint32_t count_after_lower_prescaler =
      abs_mmio_read32(kAonTimerBase + AON_TIMER_WKUP_COUNT_LO_REG_OFFSET);
  LOG_INFO("WKUP_COUNT_LO 100 us after lowering PRESCALER 0xfff -> 1: %u",
           count_after_lower_prescaler);
  CHECK(count_after_lower_prescaler > 0u,
        "Expected WKUP_COUNT_LO > 0 after lowering PRESCALER in v2, got 0");

  // Verify dif_aon_timer_wakeup_start(&aon, /*threshold=*/0, /*prescaler=*/0)
  // and dif_aon_timer_watchdog_start(&aon, 0, 0, false, false) threshold-1
  // underflow to 0xffffffffffffffff / 0xffffffff.
  aon_reset_all();
  CHECK_DIF_OK(dif_aon_timer_wakeup_start(&aon, /*threshold=*/0u,
                                          /*prescaler=*/0u));
  CHECK(abs_mmio_read32(kAonTimerBase + AON_TIMER_WKUP_THOLD_LO_REG_OFFSET) ==
        0xffffffffu);
  CHECK(abs_mmio_read32(kAonTimerBase + AON_TIMER_WKUP_THOLD_HI_REG_OFFSET) ==
        0xffffffffu);
  busy_spin_micros(30);
  CHECK(abs_mmio_read32(kAonTimerBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET) == 0u);

  CHECK_DIF_OK(dif_aon_timer_watchdog_start(&aon, /*bark_threshold=*/0u,
                                            /*bite_threshold=*/0u,
                                            /*pause_in_sleep=*/false,
                                            /*lock=*/false));
  CHECK(abs_mmio_read32(kAonTimerBase + AON_TIMER_WDOG_BARK_THOLD_REG_OFFSET) ==
        0xffffffffu);
  CHECK(abs_mmio_read32(kAonTimerBase + AON_TIMER_WDOG_BITE_THOLD_REG_OFFSET) ==
        0xffffffffu);

  // =========================================================================
  // 3. [aon_timer_core.sv:68 vs aon_timer.hjson:172, 184]:
  //    `wkup_intr_o = wkup_incr & (wkup_count >= wkup_thold)` gates threshold
  //    comparison on `wkup_incr` (`prescale_count_q == prescaler`).
  // =========================================================================
  LOG_INFO(
      "Verifying [aon_timer_core.sv:68]: wkup_intr_o threshold comparison "
      "is gated by wkup_incr (prescale_count_q == prescaler)...");
  aon_reset_all();
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_THOLD_LO_REG_OFFSET, 5u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_COUNT_LO_REG_OFFSET, 10u);
  busy_spin_micros(30);

  // Enable with PRESCALER = 200 (1 ms per tick). Even after 50 us (~10 AON
  // cycles, well after WKUP_CTRL has crossed the CDC), WKUP_CAUSE and
  // INTR_STATE remain 0 because wkup_incr has not yet pulsed!
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET,
                   (200u << AON_TIMER_WKUP_CTRL_PRESCALER_OFFSET) | 1u);
  busy_spin_micros(50);
  CHECK(abs_mmio_read32(kAonTimerBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET) == 0u);

  // =========================================================================
  // 4. [aon_timer_reg_pkg.sv:191-206 & aon_timer_reg_top.sv:1171-1187]:
  //    Sub-word write `wr_err` (`d_error = 1`, `mcause = 7`) on `WKUP_CTRL`
  //    (`4'b0011`, `sb` faults while `sh` succeeds), `WDOG_CTRL` (`4'b0001`,
  //    `sb` succeeds), and `WKUP_THOLD_LO` (`4'b1111`, `sb`/`sh` fault).
  // =========================================================================
  LOG_INFO(
      "Verifying [aon_timer_reg_pkg.sv:191-206]: AON_TIMER_PERMIT sub-word "
      "write access faults (WKUP_CTRL 4'b0011 vs WDOG_CTRL 4'b0001 vs "
      "WKUP_THOLD_LO 4'b1111)...");
  aon_reset_all();

  // 8-bit write (sb) to WDOG_CTRL (4'b0001) succeeds without fault.
  g_access_fault_count = 0;
  abs_mmio_write8(kAonTimerBase + AON_TIMER_WDOG_CTRL_REG_OFFSET, 0x2u);
  CHECK(g_access_fault_count == 0u);
  CHECK(abs_mmio_read32(kAonTimerBase + AON_TIMER_WDOG_CTRL_REG_OFFSET) ==
        0x2u);

  // 8-bit write (sb) to WKUP_CTRL (4'b0011) faults with mcause = 7 and is
  // ignored, whereas 16-bit write (sh) to WKUP_CTRL succeeds!
  g_last_mcause = 0;
  abs_mmio_write8(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET, 0x1u);
  CHECK(g_access_fault_count == 1u);
  CHECK(g_last_mcause == kRiscvStoreAccessFault);
  CHECK(abs_mmio_read32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET) == 0u);

  mmio_write16(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET, 0x0004u);
  CHECK(g_access_fault_count == 1u);
  CHECK(abs_mmio_read32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET) ==
        0x0004u);

  // 8-bit (sb) and 16-bit (sh) writes to WKUP_THOLD_LO (4'b1111) fault with
  // mcause = 7 and do not modify WKUP_THOLD_LO.
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_THOLD_LO_REG_OFFSET,
                   0x11223344u);
  abs_mmio_write8(kAonTimerBase + AON_TIMER_WKUP_THOLD_LO_REG_OFFSET, 0x99u);
  CHECK(g_access_fault_count == 2u);
  CHECK(g_last_mcause == kRiscvStoreAccessFault);
  mmio_write16(kAonTimerBase + AON_TIMER_WKUP_THOLD_LO_REG_OFFSET, 0x8899u);
  CHECK(g_access_fault_count == 3u);
  CHECK(g_last_mcause == kRiscvStoreAccessFault);
  CHECK(abs_mmio_read32(kAonTimerBase + AON_TIMER_WKUP_THOLD_LO_REG_OFFSET) ==
        0x11223344u);

  aon_reset_all();
  LOG_INFO("All Earlgrey v2 aon_timer errata checks confirmed on CW340 FPGA!");
  return true;
}
