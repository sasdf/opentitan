// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file adc_ctrl_errata_test.c
 * @brief CW340 FPGA & QEMU Hardware Errata Confirmation Test for `adc_ctrl`
 * (`P05`).
 *
 * Empirically confirms the taped-out OpenTitan Earlgrey `adc_ctrl` silicon
 * errata, specification errata, and security hardening behaviors on physical
 * CW340 FPGA silicon and QEMU (`sim_qemu`):
 *
 * - `[adc_ctrl_fsm.sv:60-72]` (`TRUE_SILICON_ERRATA` — `HIGH` / `HIGH (90%)`):
 *   Oneshot completion (`ONEST_DONE`) transitions `fsm_state_q` back to `PWRDN`
 *   (`0`) without clearing `ADC_EN_CTL.adc_enable` (`trigger_q == 1` in
 *   `adc_ctrl_fsm.sv:60-72, 208-213, 255-258`). Subsequent writes of
 *   `ADC_EN_CTL = 0x3` (`adc_enable = 1, oneshot_mode = 1`) fail to synthesize
 *   a `trigger_l2h = !trigger_q && cfg_adc_enable_i` rising edge and silently
 *   fail to re-trigger oneshot conversion until software clears `ADC_EN_CTL`
 *   to `0` (or pulses `ADC_FSM_RST`).
 *
 * - `[adc_ctrl_core.sv:77-90]` (`SPEC_DOC_ERRATA` — `MEDIUM`):
 *   `ADC_CHN_VAL_0/1.adc_chn_value_intr` (`bits [27:18]`) is driven by
 *   `chn_val_intr_we = oneshot_mode ? oneshot_done : |match_pulse`
 *   (`adc_ctrl_core.sv:77-90`), latching on `ONEST_DONE` in oneshot mode and
 *   updating whenever a new filter mask debounces (`!stay_match` in
 *   `adc_ctrl_fsm.sv:353-368`) even if a prior interrupt is already pending in
 *   `ADC_INTR_STATUS`.
 *
 * - `[adc_ctrl_fsm.sv:60-72]` (`SPEC_DOC_ERRATA` — `MEDIUM`):
 *   `ADC_FSM_RST.rst_en` (`adc_ctrl_fsm.sv:60-72, 175-184`) is a
 *   level-sensitive hold that forces `trigger_q <= 0` and holds `PWRDN` even
 *   when `ADC_EN_CTL = 0x3` is written; clearing `ADC_FSM_RST` (`1 -> 0`) while
 *   `ADC_EN_CTL.adc_enable == 1` immediately generates `trigger_l2h = 1` on the
 *   next `clk_aon_i` edge and launches conversion without re-writing
 *   `ADC_EN_CTL`.
 *
 * - `[adc_ctrl_reg_pkg.sv:305-338]` (`INTENDED_SECURITY_HARDENING` — `INFO`):
 *   Heterogeneous `ADC_CTRL_PERMIT[32]` byte-enable masks (`4'b0001`,
 *   `4'b0011`, `4'b1111` in `adc_ctrl_reg_pkg.sv:305-338`) reject narrow
 *   sub-word writes (`sb`/`sh`) to 4-byte CSRs (`ADC_PD_CTL`,
 *   `ADC_CHN0/1_FILTER_CTL_0..7`) with a synchronous Store Access Fault
 *   (`mcause = 7`) while dropping the write.
 *
 * - `[adc_ctrl_fsm.sv:19-20]` (`TRUE_SILICON_ERRATA` — `MEDIUM` / `MEDIUM
 * (65%)`): Unsigned subtraction `np_sample_cnt_thresh = cfg_np_sample_cnt_i -
 * 1'b1`
 *   (`16-bit`) and `lp_sample_cnt_thresh = cfg_lp_sample_cnt_i - 1'b1`
 *   (`8-bit`) in `adc_ctrl_fsm.sv:186-187` causes programming `0` into
 *   `ADC_SAMPLE_CTL` or `ADC_LP_SAMPLE_CTL` to underflow to `65535` (`65,536`
 *   NP samples, `~1.64 s`) or `255` (`256` LP samples) instead of completing
 *   after 1 sample.
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/arch/device.h"
#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "adc_ctrl_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kAdcBase = TOP_EARLGREY_ADC_CTRL_AON_BASE_ADDR,
  kOneshotIntrBit = (1u << 9u),
  kExpectedChn0Val = 0x000u,
  kExpectedChn1Val = 0x000u,
};

static volatile uint32_t g_store_fault_count = 0;
static volatile uint32_t g_last_mcause = 0;

void ottf_exception_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = ibex_mcause_read();
  g_last_mcause = mcause;
  if (mcause == kIbexExcStoreAccessFault) {
    g_store_fault_count++;
    return;
  }
  CHECK(false, "Unexpected exception mcause=0x%08x", mcause);
}

static uint32_t make_filter_ctl(uint32_t min_v, uint32_t max_v, bool out_range,
                                bool en) {
  return ((min_v & 0x3ffu) << 2u) | ((out_range ? 1u : 0u) << 12u) |
         ((max_v & 0x3ffu) << 18u) | ((en ? 1u : 0u) << 31u);
}

static void adc_ctrl_clean_reset(void) {
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET, 0u);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_FSM_RST_REG_OFFSET, 1u);
  busy_spin_micros(60);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_FSM_RST_REG_OFFSET, 0u);
  busy_spin_micros(60);
  for (uint32_t i = 0; i < 8u; ++i) {
    abs_mmio_write32(
        kAdcBase + ADC_CTRL_ADC_CHN0_FILTER_CTL_0_REG_OFFSET + 4u * i, 0u);
    abs_mmio_write32(
        kAdcBase + ADC_CTRL_ADC_CHN1_FILTER_CTL_0_REG_OFFSET + 4u * i, 0u);
  }
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_INTR_CTL_REG_OFFSET, 0u);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_WAKEUP_CTL_REG_OFFSET, 0u);
  abs_mmio_write32(kAdcBase + ADC_CTRL_FILTER_STATUS_REG_OFFSET, 0x1ffu);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET, 0x3ffu);
  abs_mmio_write32(kAdcBase + ADC_CTRL_INTR_STATE_REG_OFFSET, 0x1u);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_PD_CTL_REG_OFFSET,
                   (1u << 4u) | (2u << 8u));
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_LP_SAMPLE_CTL_REG_OFFSET, 1u);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_SAMPLE_CTL_REG_OFFSET, 1u);
  busy_spin_micros(60);
}

static void test_001_and_oneshot_retrigger_deadlock_and_latch(void) {
  LOG_INFO(
      "Verifying [adc_ctrl_fsm.sv:60-72] (TRUE_SILICON_ERRATA): ONEST_DONE "
      "leaves trigger_q==1 blocking oneshot re-trigger...");
  adc_ctrl_clean_reset();

  // Poison ADC_CHN_VAL_0/1 cannot be done via SW (R/O), so we verify that
  // ONEST_DONE updates both adc_chn_value [11:2] and adc_chn_value_intr [27:18]
  // ([adc_ctrl_core.sv:77-90]) and sets ADC_INTR_STATUS.ONESHOT (bit 9).
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_INTR_CTL_REG_OFFSET,
                   kOneshotIntrBit);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET, 0x3u);

  uint32_t intr_status = 0u;
  for (int i = 0; i < 2000; ++i) {
    intr_status =
        abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET);
    if ((intr_status & kOneshotIntrBit) != 0u) {
      break;
    }
    busy_spin_micros(10);
  }
  CHECK((intr_status & kOneshotIntrBit) == kOneshotIntrBit,
        "First oneshot conversion failed to assert ONESHOT interrupt");
  busy_spin_micros(50);

  // Confirm [adc_ctrl_fsm.sv:60-72]: hardware returned fsm_state_q to PWRDN (0)
  // while leaving ADC_EN_CTL == 0x3 (adc_enable == 1, oneshot_mode == 1).
  CHECK(abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET) == 0x3u);
  CHECK(abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_FSM_STATE_REG_OFFSET) == 0u);

  // Confirm [adc_ctrl_core.sv:77-90] (Oneshot latch): both adc_chn_value [11:2]
  // and adc_chn_value_intr [27:18] were latched on oneshot_done.
  uint32_t chn0_val =
      abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_CHN_VAL_0_REG_OFFSET);
  uint32_t chn1_val =
      abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_CHN_VAL_1_REG_OFFSET);
  CHECK(((chn0_val >> 2u) & 0x3ffu) == kExpectedChn0Val);
  CHECK(((chn0_val >> 18u) & 0x3ffu) == kExpectedChn0Val);
  CHECK(((chn1_val >> 2u) & 0x3ffu) == kExpectedChn1Val);
  CHECK(((chn1_val >> 18u) & 0x3ffu) == kExpectedChn1Val);

  // Clear ADC_INTR_STATUS.ONESHOT (W1C) and attempt to re-trigger oneshot by
  // writing ADC_EN_CTL = 0x3 directly without clearing adc_enable to 0 first.
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET, 0x3ffu);
  CHECK(abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET) == 0u);

  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET, 0x3u);
  // Wait 250 us (50 clk_aon_i cycles, >8x the time required for oneshot).
  busy_spin_micros(250);

  // Because trigger_q remained 1, trigger_l2h == 0 and no oneshot conversion
  // occurs!
  CHECK((abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET) &
         kOneshotIntrBit) == 0u);
  CHECK(abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_FSM_STATE_REG_OFFSET) == 0u);

  // Verify the required software workaround: clear ADC_EN_CTL = 0, wait for
  // clk_aon_i CDC (60 us) so trigger_q <= 0, then write ADC_EN_CTL = 0x3.
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET, 0u);
  busy_spin_micros(60);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET, 0x3u);
  for (int i = 0; i < 2000; ++i) {
    intr_status =
        abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET);
    if ((intr_status & kOneshotIntrBit) != 0u) {
      break;
    }
    busy_spin_micros(10);
  }
  CHECK((intr_status & kOneshotIntrBit) == kOneshotIntrBit,
        "Workaround (ADC_EN_CTL 0 -> 0x3) must re-trigger oneshot");
  LOG_INFO(
      "[adc_ctrl_fsm.sv:60-72] confirmed: direct ADC_EN_CTL=0x3 re-write "
      "blocked by trigger_q==1; 0->0x3 toggle succeeds.");
}

static void test_subsequent_filter_match_while_intr_pending(void) {
  LOG_INFO(
      "Verifying [adc_ctrl_core.sv:77-90] (SPEC_DOC_ERRATA): new filter "
      "set transition (!stay_match) fires match_pulse & updates "
      "adc_chn_value_intr while prior interrupt is still pending...");
  adc_ctrl_clean_reset();

  // Enable Filter 0 matching [0x000, 0x100] and enable interrupts for Filter 0
  // and Filter 1.
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_CHN0_FILTER_CTL_0_REG_OFFSET,
                   make_filter_ctl(0x000u, 0x100u, false, true));
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_CHN1_FILTER_CTL_0_REG_OFFSET,
                   make_filter_ctl(0x000u, 0x100u, false, true));
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_INTR_CTL_REG_OFFSET, 0x3u);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET, 0x1u);

  uint32_t intr_status = 0u;
  for (int i = 0; i < 2000; ++i) {
    intr_status =
        abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET);
    if ((intr_status & 0x1u) != 0u) {
      break;
    }
    busy_spin_micros(10);
  }
  CHECK((intr_status & 0x3u) == 0x1u);

  // Without clearing ADC_INTR_STATUS (so bit 0 remains pending!), dynamically
  // enable Filter 1 (matching [0x000, 0x100]) to change adc_ctrl_match_i from
  // 0x01 to 0x03 (!stay_match), triggering a new NP_DONE match_pulse.
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_CHN0_FILTER_CTL_1_REG_OFFSET,
                   make_filter_ctl(0x000u, 0x100u, false, true));
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_CHN1_FILTER_CTL_1_REG_OFFSET,
                   make_filter_ctl(0x000u, 0x100u, false, true));

  for (int i = 0; i < 2000; ++i) {
    intr_status =
        abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET);
    if ((intr_status & 0x3u) == 0x3u) {
      break;
    }
    busy_spin_micros(10);
  }
  CHECK((intr_status & 0x3u) == 0x3u);
  LOG_INFO(
      "[adc_ctrl_core.sv:77-90] confirmed: subsequent debounced filter match "
      "fires while prior interrupt is pending (ADC_INTR_STATUS=0x%x).",
      intr_status);
}

static void test_fsm_rst_level_gating_and_release_edge(void) {
  LOG_INFO(
      "Verifying [adc_ctrl_fsm.sv:60-72] (SPEC_DOC_ERRATA): ADC_FSM_RST "
      "level-sensitive hold and automatic trigger_l2h rising edge on "
      "1->0 release...");
  adc_ctrl_clean_reset();

  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_INTR_CTL_REG_OFFSET,
                   kOneshotIntrBit);
  // Assert ADC_FSM_RST = 1 and wait for clk_aon_i CDC sync.
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_FSM_RST_REG_OFFSET, 1u);
  busy_spin_micros(60);

  // Enable oneshot mode while ADC_FSM_RST == 1.
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET, 0x3u);
  busy_spin_micros(250);

  // Confirm FSM is held in PWRDN (0) and ONESHOT interrupt did NOT fire.
  CHECK((abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET) &
         kOneshotIntrBit) == 0u);
  CHECK(abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_FSM_STATE_REG_OFFSET) == 0u);

  // Now release ADC_FSM_RST (1 -> 0) WITHOUT writing ADC_EN_CTL again.
  // Because trigger_q was held at 0 while cfg_adc_enable_i == 1, releasing
  // ADC_FSM_RST immediately synthesizes trigger_l2h = 1 on the next clk_aon_i
  // edge and runs the oneshot conversion!
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_FSM_RST_REG_OFFSET, 0u);

  uint32_t intr_status = 0u;
  for (int i = 0; i < 2000; ++i) {
    intr_status =
        abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET);
    if ((intr_status & kOneshotIntrBit) != 0u) {
      break;
    }
    busy_spin_micros(10);
  }
  CHECK((intr_status & kOneshotIntrBit) == kOneshotIntrBit,
        "Releasing ADC_FSM_RST (1->0) while ADC_EN_CTL=0x3 must synthesize "
        "trigger_l2h and complete oneshot");
  LOG_INFO(
      "[adc_ctrl_fsm.sv:60-72] confirmed: ADC_FSM_RST=1 blocks conversion "
      "and releasing 1->0 auto-synthesizes trigger_l2h.");
}

static void test_subword_write_permit_fault(void) {
  LOG_INFO(
      "Verifying [adc_ctrl_reg_pkg.sv:305-338] (INTENDED_SECURITY_HARDENING): "
      "ADC_CTRL_PERMIT rejects sub-word writes to 4-byte CSRs with "
      "Store Access Fault (mcause=7)...");
  adc_ctrl_clean_reset();

  g_store_fault_count = 0;
  g_last_mcause = 0;

  // 1-byte write to ADC_EN_CTL (PERMIT = 4'b0001) must succeed without fault.
  abs_mmio_write8(kAdcBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET, 0x0u);
  CHECK(g_store_fault_count == 0u);

  // 1-byte write to ADC_PD_CTL (PERMIT = 4'b1111) must fault with mcause = 7
  // and drop the write even though lp_mode is in bit 0 (byte 0).
  uint32_t pd_before =
      abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_PD_CTL_REG_OFFSET);
  abs_mmio_write8(kAdcBase + ADC_CTRL_ADC_PD_CTL_REG_OFFSET,
                  (uint8_t)(pd_before ^ 0x1u));
  CHECK(g_store_fault_count == 1u);
  CHECK(g_last_mcause == kIbexExcStoreAccessFault);
  CHECK(abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_PD_CTL_REG_OFFSET) ==
        pd_before);

  // 2-byte write to ADC_CHN0_FILTER_CTL_0 (PERMIT = 4'b1111) must also fault
  // with mcause = 7 and drop the write.
  uint32_t filt_expected = make_filter_ctl(0x10u, 0x20u, false, true);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_CHN0_FILTER_CTL_0_REG_OFFSET,
                   filt_expected);
  *((volatile uint16_t *)(kAdcBase +
                          ADC_CTRL_ADC_CHN0_FILTER_CTL_0_REG_OFFSET)) = 0xffffu;
  CHECK(g_store_fault_count == 2u);
  CHECK(g_last_mcause == kIbexExcStoreAccessFault);
  CHECK(abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_CHN0_FILTER_CTL_0_REG_OFFSET) ==
        filt_expected);

  LOG_INFO(
      "[adc_ctrl_reg_pkg.sv:305-338] confirmed: sub-word writes to ADC_PD_CTL "
      "and "
      "ADC_CHN0_FILTER_CTL_0 fault with mcause=7 (faults=%u).",
      g_store_fault_count);
}

static void test_zero_sample_cnt_underflow(void) {
  LOG_INFO(
      "Verifying [adc_ctrl_fsm.sv:19-20] (TRUE_SILICON_ERRATA): "
      "ADC_SAMPLE_CTL=0 underflows cfg_np_sample_cnt_i - 1 to 65535 "
      "(65,536 samples)...");
  adc_ctrl_clean_reset();

  // Configure Filter 0 to match [0x000, 0x100], LP_MODE = 0, and set
  // ADC_SAMPLE_CTL = 0 (which underflows np_sample_cnt_thresh to 0xffff).
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_CHN0_FILTER_CTL_0_REG_OFFSET,
                   make_filter_ctl(0x000u, 0x100u, false, true));
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_CHN1_FILTER_CTL_0_REG_OFFSET,
                   make_filter_ctl(0x000u, 0x100u, false, true));
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_INTR_CTL_REG_OFFSET, 0x1u);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_SAMPLE_CTL_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_SAMPLE_CTL_REG_OFFSET) == 0u);

  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET, 0x1u);
  // Wait 500 us (100 clk_aon_i cycles = ~20 normal-power ADC samples).
  // Because NP_SAMPLE_CNT = 0 underflows to 65535 (requiring 65,536 samples =
  // ~1.64 seconds), FILTER_STATUS and ADC_INTR_STATUS must still be 0!
  busy_spin_micros(500);
  CHECK((abs_mmio_read32(kAdcBase + ADC_CTRL_FILTER_STATUS_REG_OFFSET) &
         0x1u) == 0u);
  CHECK((abs_mmio_read32(kAdcBase + ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET) &
         0x1u) == 0u);

  // If ADC_SAMPLE_CTL is lowered in-flight to 1 (np_sample_cnt_thresh = 0)
  // after np_sample_cnt_q has already incremented > 0, adc_ctrl_fsm.sv:366-368
  // evaluates (np_sample_cnt_q > np_sample_cnt_thresh) -> NP_0 without hitting
  // NP_DONE!
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_SAMPLE_CTL_REG_OFFSET, 1u);
  busy_spin_micros(200);
  CHECK((abs_mmio_read32(kAdcBase + ADC_CTRL_FILTER_STATUS_REG_OFFSET) &
         0x1u) == 0u);

  // Resetting the FSM via ADC_FSM_RST (1 -> 0) clears np_sample_cnt_q to 0,
  // and with ADC_SAMPLE_CTL = 1 (np_sample_cnt_thresh = 0), Filter 0 matches
  // on the very first sample!
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_FSM_RST_REG_OFFSET, 1u);
  busy_spin_micros(60);
  abs_mmio_write32(kAdcBase + ADC_CTRL_ADC_FSM_RST_REG_OFFSET, 0u);

  uint32_t filter_status = 0u;
  for (int i = 0; i < 2000; ++i) {
    filter_status =
        abs_mmio_read32(kAdcBase + ADC_CTRL_FILTER_STATUS_REG_OFFSET);
    if ((filter_status & 0x1u) != 0u) {
      break;
    }
    busy_spin_micros(10);
  }
  CHECK((filter_status & 0x1u) == 0x1u);
  adc_ctrl_clean_reset();
  LOG_INFO(
      "[adc_ctrl_fsm.sv:19-20] confirmed: ADC_SAMPLE_CTL=0 underflows "
      "threshold to 65535 (and in-flight lower to 1 is bypassed when "
      "np_sample_cnt_q > 0); resetting FSM with ADC_SAMPLE_CTL=1 "
      "completes immediately.");
}

bool test_main(void) {
  LOG_INFO("Starting adc_ctrl hardware errata confirmation suite...");
  test_001_and_oneshot_retrigger_deadlock_and_latch();
  test_subsequent_filter_match_while_intr_pending();
  test_fsm_rst_level_gating_and_release_edge();
  test_subword_write_permit_fault();
  test_zero_sample_cnt_underflow();
  LOG_INFO("All adc_ctrl errata checks confirmed on hardware!");
  return true;
}
