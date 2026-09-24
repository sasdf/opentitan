// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// CW340 FPGA hardware behavior verification for ADC Controller (adc_ctrl) on
// Earlgrey trunk-v2:
//
// 1. Oneshot completion (hw/ip/adc_ctrl/rtl/adc_ctrl_fsm.sv:257-260) asserts
//    oneshot_done_o and transitions fsm_state_d = PWRDN while ADC_EN_CTL
//    remains hardware read-only (hwaccess: "hro", adc_ctrl.hjson:106-126).
//    Because trigger_q remains 1'b1 (adc_ctrl_fsm.sv:62-73), trigger_l2h
//    ((trigger_q == 1'b0) && (cfg_adc_enable_i == 1'b1)) cannot fire again
//    until ADC_EN_CTL.adc_enable is cleared (0->1) or ADC_FSM_RST is toggled.
// 2. ADC_CHN_VAL_0/1.adc_chn_value_intr (adc_ctrl_core.sv:85-91) is updated
//    whenever chn_val_intr_we asserts (oneshot_done in oneshot mode, or
//    |match_pulse in normal/low-power mode), overwriting prior captured values
//    when the debounced match set changes (!stay_match,
//    adc_ctrl_fsm.sv:171-175, 355-367) even while ADC_INTR_STATUS.MATCH is
//    still set.
// 3. ADC_FSM_RST.rst_en (adc_ctrl_fsm.sv:66-73, 144-149, 181-182) is a
//    level-sensitive hold forcing trigger_q <= 1'b0 and fsm_state_q <= PWRDN;
//    clearing rst_en (1 -> 0) while ADC_EN_CTL.adc_enable == 1 creates a
//    rising edge on trigger_l2h and immediately launches a new conversion.
// 4. Heterogeneous ADC_CTRL_PERMIT byte-enable masks (adc_ctrl_reg_top.sv /
//    adc_ctrl_reg_pkg.sv) reject sub-word writes (sb/sh) to 4'b1111 registers
//    such as ADC_PD_CTL and ADC_CHN0/1_FILTER_CTL_0..7 with a synchronous
//    Store Access Fault (mcause = 7) without mutating the register.
// 5. Zero sample count underflow (adc_ctrl_fsm.sv:188-189:
//    lp_sample_cnt_thresh = cfg_lp_sample_cnt_i - 1'b1 -> 0xFF,
//    np_sample_cnt_thresh = cfg_np_sample_cnt_i - 1'b1 -> 0xFFFF) and
//    in-flight NP_SAMPLE_CNT reduction bypass when np_sample_cnt_q >
//    np_sample_cnt_thresh (adc_ctrl_fsm.sv:362-370).
// 6. ADC_INTR_CTL gates the write-enable (.de) of ADC_INTR_STATUS itself
//    (adc_ctrl_intr.sv:106-116) whereas ADC_CHN_VAL_0/1.adc_chn_value_intr
//    (adc_ctrl_core.sv:85-91) and FILTER_STATUS (adc_ctrl_core.sv:139-143)
//    latch unconditionally without checking ADC_INTR_CTL; additionally,
//    ADC_FSM_RST resets internal chn0_val_o/chn1_val_o to 0 while forcing
//    chn0_val_we_o/chn1_val_we_o = 0 (adc_ctrl_fsm.sv:144-149), leaving the
//    ADC_CHN_VAL_0/1 CSRs un-cleared across ADC_FSM_RST.

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_adc_ctrl.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top/adc_ctrl_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kAdcCtrlBase = TOP_EARLGREY_ADC_CTRL_BASE_ADDR,
  // Sync delay for 200 kHz AON clock domain (~5 us per cycle; 40 us = 8 AON
  // clock edges).
  kAonSyncDelayUs = 40,
  kPollTimeoutUs = 50000,
};

static volatile bool g_fault_seen = false;
static volatile uint32_t g_fault_mcause = 0;

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
  g_fault_seen = true;
  g_fault_mcause = mcause;
  advance_mepc_over_faulting_insn();
}

static inline void sync_aon(void) { busy_spin_micros(kAonSyncDelayUs); }

static void adc_ctrl_full_reset_and_clean(void) {
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET, 0u);
  sync_aon();
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_FSM_RST_REG_OFFSET, 1u);
  sync_aon();
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_FSM_RST_REG_OFFSET, 0u);
  sync_aon();

  for (uint32_t i = 0; i < ADC_CTRL_PARAM_NUM_ADC_FILTER; ++i) {
    abs_mmio_write32(
        kAdcCtrlBase + ADC_CTRL_ADC_CHN0_FILTER_CTL_0_REG_OFFSET + i * 4u, 0u);
    abs_mmio_write32(
        kAdcCtrlBase + ADC_CTRL_ADC_CHN1_FILTER_CTL_0_REG_OFFSET + i * 4u, 0u);
  }
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_WAKEUP_CTL_REG_OFFSET, 0u);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_INTR_CTL_REG_OFFSET, 0u);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_INTR_ENABLE_REG_OFFSET, 0u);
  sync_aon();

  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_FILTER_STATUS_REG_OFFSET, 0x1FFu);
  sync_aon();
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET, 0x3FFu);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_INTR_STATE_REG_OFFSET, 0x1u);
}

static uint32_t pack_filter(bool en, bool out_of_range, uint32_t min_v,
                            uint32_t max_v) {
  uint32_t reg = 0;
  reg = bitfield_field32_write(
      reg, ADC_CTRL_ADC_CHN0_FILTER_CTL_0_MIN_V_0_FIELD, min_v & 0x3FFu);
  reg = bitfield_bit32_write(reg, ADC_CTRL_ADC_CHN0_FILTER_CTL_0_COND_0_BIT,
                             out_of_range);
  reg = bitfield_field32_write(
      reg, ADC_CTRL_ADC_CHN0_FILTER_CTL_0_MAX_V_0_FIELD, max_v & 0x3FFu);
  reg = bitfield_bit32_write(reg, ADC_CTRL_ADC_CHN0_FILTER_CTL_0_EN_0_BIT, en);
  return reg;
}

static bool wait_for_intr_status_mask(uint32_t mask, uint32_t timeout_us) {
  ibex_timeout_t timeout = ibex_timeout_init(timeout_us);
  while (!ibex_timeout_check(&timeout)) {
    uint32_t st =
        abs_mmio_read32(kAdcCtrlBase + ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET);
    if ((st & mask) == mask) {
      return true;
    }
  }
  return false;
}

static void test_oneshot_sticky_enable_and_retrigger(void) {
  LOG_INFO("Test 1: Oneshot completion sticky ADC_ENABLE and re-trigger");
  adc_ctrl_full_reset_and_clean();

  // Configure fast power-up time (pwrup_time = 1, wakeup_time = 2, lp_mode =
  // 0).
  uint32_t pd_ctl =
      bitfield_field32_write(0u, ADC_CTRL_ADC_PD_CTL_PWRUP_TIME_FIELD, 1u);
  pd_ctl =
      bitfield_field32_write(pd_ctl, ADC_CTRL_ADC_PD_CTL_WAKEUP_TIME_FIELD, 2u);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_PD_CTL_REG_OFFSET, pd_ctl);
  sync_aon();

  // Enable ONESHOT interrupt source and top-level interrupt enable.
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_INTR_CTL_REG_OFFSET,
                   1u << ADC_CTRL_ADC_INTR_CTL_ONESHOT_EN_BIT);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_INTR_ENABLE_REG_OFFSET,
                   1u << ADC_CTRL_INTR_ENABLE_MATCH_PENDING_BIT);

  // Trigger oneshot conversion: adc_enable = 1, oneshot_mode = 1.
  uint32_t en_oneshot = (1u << ADC_CTRL_ADC_EN_CTL_ADC_ENABLE_BIT) |
                        (1u << ADC_CTRL_ADC_EN_CTL_ONESHOT_MODE_BIT);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET, en_oneshot);

  CHECK(wait_for_intr_status_mask(1u << ADC_CTRL_ADC_INTR_STATUS_ONESHOT_BIT,
                                  kPollTimeoutUs),
        "Oneshot conversion did not complete in time");

  // Wait 2 AON cycles so ONEST_DONE transitions to PWRDN.
  sync_aon();

  uint32_t fsm_state =
      abs_mmio_read32(kAdcCtrlBase + ADC_CTRL_ADC_FSM_STATE_REG_OFFSET) & 0x1Fu;
  CHECK(fsm_state == ADC_CTRL_ADC_FSM_STATE_STATE_VALUE_PWRDN,
        "Expected FSM state PWRDN (0) after oneshot, got %u", fsm_state);

  uint32_t en_ctl_after =
      abs_mmio_read32(kAdcCtrlBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET);
  CHECK(en_ctl_after == en_oneshot,
        "Expected ADC_EN_CTL to remain 0x%x after oneshot, got 0x%x",
        en_oneshot, en_ctl_after);

  // Clear ADC_INTR_STATUS.ONESHOT and INTR_STATE.match_pending (rw1c).
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET,
                   1u << ADC_CTRL_ADC_INTR_STATUS_ONESHOT_BIT);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_INTR_STATE_REG_OFFSET,
                   1u << ADC_CTRL_INTR_STATE_MATCH_PENDING_BIT);
  CHECK(
      abs_mmio_read32(kAdcCtrlBase + ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET) == 0u,
      "ADC_INTR_STATUS should be cleared");

  // Re-write ADC_EN_CTL = 0x3 without clearing adc_enable first.
  // Because trigger_q is still 1'b1, trigger_l2h stays 0 and no new conversion
  // occurs.
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET, en_oneshot);
  busy_spin_micros(500);
  CHECK(
      abs_mmio_read32(kAdcCtrlBase + ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET) == 0u,
      "Expected no re-trigger when writing 0x3 while trigger_q == 1");

  // Now toggle adc_enable 0 -> 1 across AON sync: trigger_l2h fires and a
  // second oneshot conversion completes.
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET,
                   1u << ADC_CTRL_ADC_EN_CTL_ONESHOT_MODE_BIT);
  sync_aon();
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET, en_oneshot);
  CHECK(wait_for_intr_status_mask(1u << ADC_CTRL_ADC_INTR_STATUS_ONESHOT_BIT,
                                  kPollTimeoutUs),
        "Expected second oneshot conversion after 0->1 toggle of adc_enable");
}

static void test_chn_val_intr_overwrite_and_oneshot_latch(void) {
  LOG_INFO("Test 2: ADC_CHN_VAL_INTR overwrite on match set change");
  adc_ctrl_full_reset_and_clean();

  uint32_t pd_ctl =
      bitfield_field32_write(0u, ADC_CTRL_ADC_PD_CTL_PWRUP_TIME_FIELD, 1u);
  pd_ctl =
      bitfield_field32_write(pd_ctl, ADC_CTRL_ADC_PD_CTL_WAKEUP_TIME_FIELD, 2u);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_PD_CTL_REG_OFFSET, pd_ctl);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_SAMPLE_CTL_REG_OFFSET, 2u);
  sync_aon();

  // Enable Filter 0 as catch-all [0, 0x3FF] on both channels.
  uint32_t catch_all = pack_filter(true, false, 0u, 0x3FFu);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_CHN0_FILTER_CTL_0_REG_OFFSET,
                   catch_all);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_CHN1_FILTER_CTL_0_REG_OFFSET,
                   catch_all);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_INTR_CTL_REG_OFFSET, 0xFFu);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_INTR_ENABLE_REG_OFFSET, 1u);
  sync_aon();

  // Start normal continuous scanning.
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET,
                   1u << ADC_CTRL_ADC_EN_CTL_ADC_ENABLE_BIT);
  CHECK(wait_for_intr_status_mask(0x01u, kPollTimeoutUs),
        "Filter 0 debounced match did not fire");

  uint32_t intr_st_first =
      abs_mmio_read32(kAdcCtrlBase + ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET);
  CHECK((intr_st_first & 0x01u) == 0x01u,
        "Expected filter 0 match bit set in ADC_INTR_STATUS");

  // While filter 0 match interrupt is STILL pending (unacknowledged in
  // ADC_INTR_STATUS and INTR_STATE), force Filter 1 to an impossible range
  // first and then enable Filter 1 as catch-all so the active match vector
  // transitions from 0x01 -> 0x03 (!stay_match), resetting np_sample_cnt_q and
  // firing a second adc_ctrl_done pulse that overwrites adc_chn_value_intr.
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_CHN0_FILTER_CTL_1_REG_OFFSET,
                   catch_all);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_CHN1_FILTER_CTL_1_REG_OFFSET,
                   catch_all);
  sync_aon();

  CHECK(wait_for_intr_status_mask(0x03u, kPollTimeoutUs),
        "Filter 1 match did not accumulate in ADC_INTR_STATUS");

  uint32_t chn0_val =
      abs_mmio_read32(kAdcCtrlBase + ADC_CTRL_ADC_CHN_VAL_0_REG_OFFSET);
  uint32_t chn0_cur = bitfield_field32_read(
      chn0_val, ADC_CTRL_ADC_CHN_VAL_0_ADC_CHN_VALUE_0_FIELD);
  uint32_t chn0_intr = bitfield_field32_read(
      chn0_val, ADC_CTRL_ADC_CHN_VAL_0_ADC_CHN_VALUE_INTR_0_FIELD);
  CHECK(chn0_intr == chn0_cur,
        "Expected chn0 adc_chn_value_intr (%u) to match latest sample (%u)",
        chn0_intr, chn0_cur);

  uint32_t chn1_val =
      abs_mmio_read32(kAdcCtrlBase + ADC_CTRL_ADC_CHN_VAL_1_REG_OFFSET);
  uint32_t chn1_cur = bitfield_field32_read(
      chn1_val, ADC_CTRL_ADC_CHN_VAL_1_ADC_CHN_VALUE_1_FIELD);
  uint32_t chn1_intr = bitfield_field32_read(
      chn1_val, ADC_CTRL_ADC_CHN_VAL_1_ADC_CHN_VALUE_INTR_1_FIELD);
  CHECK(chn1_intr == chn1_cur,
        "Expected chn1 adc_chn_value_intr (%u) to match latest sample (%u)",
        chn1_intr, chn1_cur);

  // Also verify oneshot_done latches adc_chn_value_intr == adc_chn_value on
  // both ADC_CHN_VAL_0 and ADC_CHN_VAL_1:
  adc_ctrl_full_reset_and_clean();
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_PD_CTL_REG_OFFSET, pd_ctl);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_INTR_CTL_REG_OFFSET,
                   1u << ADC_CTRL_ADC_INTR_CTL_ONESHOT_EN_BIT);
  sync_aon();
  uint32_t en_oneshot = (1u << ADC_CTRL_ADC_EN_CTL_ADC_ENABLE_BIT) |
                        (1u << ADC_CTRL_ADC_EN_CTL_ONESHOT_MODE_BIT);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET, en_oneshot);
  CHECK(wait_for_intr_status_mask(1u << ADC_CTRL_ADC_INTR_STATUS_ONESHOT_BIT,
                                  kPollTimeoutUs),
        "Oneshot conversion in Test 2 did not complete");
  sync_aon();
  chn0_val = abs_mmio_read32(kAdcCtrlBase + ADC_CTRL_ADC_CHN_VAL_0_REG_OFFSET);
  chn1_val = abs_mmio_read32(kAdcCtrlBase + ADC_CTRL_ADC_CHN_VAL_1_REG_OFFSET);
  CHECK(bitfield_field32_read(
            chn0_val, ADC_CTRL_ADC_CHN_VAL_0_ADC_CHN_VALUE_INTR_0_FIELD) ==
            bitfield_field32_read(chn0_val,
                                  ADC_CTRL_ADC_CHN_VAL_0_ADC_CHN_VALUE_0_FIELD),
        "Expected oneshot_done to latch ADC_CHN_VAL_0.adc_chn_value_intr");
  CHECK(bitfield_field32_read(
            chn1_val, ADC_CTRL_ADC_CHN_VAL_1_ADC_CHN_VALUE_INTR_1_FIELD) ==
            bitfield_field32_read(chn1_val,
                                  ADC_CTRL_ADC_CHN_VAL_1_ADC_CHN_VALUE_1_FIELD),
        "Expected oneshot_done to latch ADC_CHN_VAL_1.adc_chn_value_intr");
}

static void test_fsm_rst_level_hold_and_release_retrigger(void) {
  LOG_INFO("Test 3: ADC_FSM_RST level-sensitive hold and release re-trigger");
  adc_ctrl_full_reset_and_clean();

  uint32_t pd_ctl =
      bitfield_field32_write(0u, ADC_CTRL_ADC_PD_CTL_PWRUP_TIME_FIELD, 1u);
  pd_ctl =
      bitfield_field32_write(pd_ctl, ADC_CTRL_ADC_PD_CTL_WAKEUP_TIME_FIELD, 2u);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_PD_CTL_REG_OFFSET, pd_ctl);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_INTR_CTL_REG_OFFSET,
                   1u << ADC_CTRL_ADC_INTR_CTL_ONESHOT_EN_BIT);
  sync_aon();

  // Complete an initial oneshot conversion so trigger_q == 1 and FSM == PWRDN.
  uint32_t en_oneshot = (1u << ADC_CTRL_ADC_EN_CTL_ADC_ENABLE_BIT) |
                        (1u << ADC_CTRL_ADC_EN_CTL_ONESHOT_MODE_BIT);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET, en_oneshot);
  CHECK(wait_for_intr_status_mask(1u << ADC_CTRL_ADC_INTR_STATUS_ONESHOT_BIT,
                                  kPollTimeoutUs),
        "Initial oneshot did not complete");
  sync_aon();

  // Clear oneshot status.
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET,
                   1u << ADC_CTRL_ADC_INTR_STATUS_ONESHOT_BIT);
  CHECK(
      abs_mmio_read32(kAdcCtrlBase + ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET) == 0u,
      "ADC_INTR_STATUS not cleared");

  // Assert ADC_FSM_RST = 1 while ADC_EN_CTL = 0x3.
  // Because rst_en is level-sensitive (not self-clearing), it holds
  // trigger_q <= 0 and fsm_state_q <= PWRDN for as long as rst_en == 1.
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_FSM_RST_REG_OFFSET, 1u);
  sync_aon();
  busy_spin_micros(300);

  CHECK(abs_mmio_read32(kAdcCtrlBase + ADC_CTRL_ADC_FSM_RST_REG_OFFSET) == 1u,
        "ADC_FSM_RST must not self-clear");
  CHECK((abs_mmio_read32(kAdcCtrlBase + ADC_CTRL_ADC_FSM_STATE_REG_OFFSET) &
         0x1Fu) == ADC_CTRL_ADC_FSM_STATE_STATE_VALUE_PWRDN,
        "FSM must be held in PWRDN while ADC_FSM_RST == 1");
  CHECK(
      abs_mmio_read32(kAdcCtrlBase + ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET) == 0u,
      "No conversion may occur while ADC_FSM_RST == 1");

  // Now release ADC_FSM_RST = 0 WITHOUT touching ADC_EN_CTL (still 0x3).
  // Because trigger_q was forced to 0 while cfg_adc_enable_i remained 1,
  // releasing ADC_FSM_RST immediately asserts trigger_l2h and launches a new
  // oneshot conversion!
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_FSM_RST_REG_OFFSET, 0u);
  CHECK(wait_for_intr_status_mask(1u << ADC_CTRL_ADC_INTR_STATUS_ONESHOT_BIT,
                                  kPollTimeoutUs),
        "Releasing ADC_FSM_RST while adc_enable==1 must trigger oneshot");
}

static void test_subword_write_permit_faults(void) {
  LOG_INFO("Test 4: Sub-word write permit mask Store Access Faults (mcause=7)");
  adc_ctrl_full_reset_and_clean();

  // 1. ADC_PD_CTL has permit mask 4'b1111 (full 32-bit word required):
  //    both sb and sh fault with mcause = 7 without mutating ADC_PD_CTL.
  uint32_t orig_pd =
      abs_mmio_read32(kAdcCtrlBase + ADC_CTRL_ADC_PD_CTL_REG_OFFSET);
  g_fault_seen = false;
  g_fault_mcause = 0;
  abs_mmio_write8(kAdcCtrlBase + ADC_CTRL_ADC_PD_CTL_REG_OFFSET, 0x01u);
  CHECK(g_fault_seen && g_fault_mcause == 7u,
        "Expected Store Access Fault (mcause=7) on byte write to ADC_PD_CTL");
  CHECK(
      abs_mmio_read32(kAdcCtrlBase + ADC_CTRL_ADC_PD_CTL_REG_OFFSET) == orig_pd,
      "Rejected byte write must not mutate ADC_PD_CTL");

  g_fault_seen = false;
  g_fault_mcause = 0;
  *(volatile uint16_t *)(kAdcCtrlBase + ADC_CTRL_ADC_PD_CTL_REG_OFFSET) =
      0x0101u;
  CHECK(g_fault_seen && g_fault_mcause == 7u,
        "Expected Store Access Fault (mcause=7) on halfword write to "
        "ADC_PD_CTL");
  CHECK(
      abs_mmio_read32(kAdcCtrlBase + ADC_CTRL_ADC_PD_CTL_REG_OFFSET) == orig_pd,
      "Rejected halfword write must not mutate ADC_PD_CTL");

  // 2. ADC_CHN0_FILTER_CTL_0 has permit mask 4'b1111 (full 32-bit word):
  //    both sb and sh fault with mcause = 7 without mutating the register.
  uint32_t filter_val = pack_filter(true, false, 0x12u, 0x34u);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_CHN0_FILTER_CTL_0_REG_OFFSET,
                   filter_val);
  g_fault_seen = false;
  g_fault_mcause = 0;
  abs_mmio_write8(kAdcCtrlBase + ADC_CTRL_ADC_CHN0_FILTER_CTL_0_REG_OFFSET,
                  0x00u);
  CHECK(g_fault_seen && g_fault_mcause == 7u,
        "Expected Store Access Fault (mcause=7) on byte write to "
        "ADC_CHN0_FILTER_CTL_0");
  CHECK(
      abs_mmio_read32(kAdcCtrlBase +
                      ADC_CTRL_ADC_CHN0_FILTER_CTL_0_REG_OFFSET) == filter_val,
      "Rejected byte write must not mutate ADC_CHN0_FILTER_CTL_0");

  g_fault_seen = false;
  g_fault_mcause = 0;
  *(volatile uint16_t *)(kAdcCtrlBase +
                         ADC_CTRL_ADC_CHN0_FILTER_CTL_0_REG_OFFSET) = 0x0000u;
  CHECK(g_fault_seen && g_fault_mcause == 7u,
        "Expected Store Access Fault (mcause=7) on halfword write to "
        "ADC_CHN0_FILTER_CTL_0");
  CHECK(
      abs_mmio_read32(kAdcCtrlBase +
                      ADC_CTRL_ADC_CHN0_FILTER_CTL_0_REG_OFFSET) == filter_val,
      "Rejected halfword write must not mutate ADC_CHN0_FILTER_CTL_0");

  // 3. ADC_SAMPLE_CTL (0x18) has permit mask 4'b0011: sb faults with mcause=7
  //    whereas sh succeeds without fault!
  g_fault_seen = false;
  g_fault_mcause = 0;
  abs_mmio_write8(kAdcCtrlBase + ADC_CTRL_ADC_SAMPLE_CTL_REG_OFFSET, 0x04u);
  CHECK(g_fault_seen && g_fault_mcause == 7u,
        "Expected Store Access Fault (mcause=7) on byte write to "
        "ADC_SAMPLE_CTL (permit 4'b0011)");
  g_fault_seen = false;
  *(volatile uint16_t *)(kAdcCtrlBase + ADC_CTRL_ADC_SAMPLE_CTL_REG_OFFSET) =
      0x0123u;
  CHECK(!g_fault_seen &&
            abs_mmio_read32(kAdcCtrlBase +
                            ADC_CTRL_ADC_SAMPLE_CTL_REG_OFFSET) == 0x0123u,
        "Halfword write to ADC_SAMPLE_CTL (permit 4'b0011) must succeed");

  // 4. ADC_LP_SAMPLE_CTL (0x14) and ADC_EN_CTL (0x0c) have permit mask 4'b0001:
  //    byte write at offset +0 succeeds without fault!
  g_fault_seen = false;
  abs_mmio_write8(kAdcCtrlBase + ADC_CTRL_ADC_LP_SAMPLE_CTL_REG_OFFSET, 0x05u);
  CHECK(!g_fault_seen,
        "Byte write to ADC_LP_SAMPLE_CTL (permit 4'b0001) must succeed");
  CHECK(abs_mmio_read32(kAdcCtrlBase + ADC_CTRL_ADC_LP_SAMPLE_CTL_REG_OFFSET) ==
            0x05u,
        "ADC_LP_SAMPLE_CTL should update to 5");

  g_fault_seen = false;
  abs_mmio_write8(kAdcCtrlBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET, 0x00u);
  CHECK(!g_fault_seen &&
            abs_mmio_read32(kAdcCtrlBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET) ==
                0x00u,
        "Byte write to ADC_EN_CTL (permit 4'b0001) must succeed");
}

static void test_sample_cnt_zero_underflow_and_inflight_reduction(void) {
  LOG_INFO("Test 5: LP/NP_SAMPLE_CNT = 0 underflow and in-flight reduction");
  adc_ctrl_full_reset_and_clean();

  uint32_t pd_ctl =
      bitfield_field32_write(0u, ADC_CTRL_ADC_PD_CTL_PWRUP_TIME_FIELD, 1u);
  pd_ctl =
      bitfield_field32_write(pd_ctl, ADC_CTRL_ADC_PD_CTL_WAKEUP_TIME_FIELD, 1u);
  pd_ctl = bitfield_bit32_write(pd_ctl, ADC_CTRL_ADC_PD_CTL_LP_MODE_BIT, true);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_PD_CTL_REG_OFFSET, pd_ctl);

  // Program LP_SAMPLE_CNT = 0 (underflows lp_sample_cnt_thresh to 8'hFF = 255,
  // requiring 256 LP matches instead of 0 or 1).
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_LP_SAMPLE_CTL_REG_OFFSET, 0u);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_SAMPLE_CTL_REG_OFFSET, 1u);

  uint32_t catch_all = pack_filter(true, false, 0u, 0x3FFu);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_CHN0_FILTER_CTL_0_REG_OFFSET,
                   catch_all);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_CHN1_FILTER_CTL_0_REG_OFFSET,
                   catch_all);
  sync_aon();

  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET,
                   1u << ADC_CTRL_ADC_EN_CTL_ADC_ENABLE_BIT);
  // Wait ~1.5 ms (~300 AON cycles, enough for ~10 LP samples, far less than 256
  // LP samples).
  busy_spin_micros(1500);

  uint32_t filter_st =
      abs_mmio_read32(kAdcCtrlBase + ADC_CTRL_FILTER_STATUS_REG_OFFSET);
  CHECK((filter_st & (1u << ADC_CTRL_FILTER_STATUS_TRANS_BIT)) == 0u,
        "Expected LP_SAMPLE_CNT=0 to underflow to 255 and not transition to NP "
        "after ~10 samples, got FILTER_STATUS=0x%x",
        filter_st);

  // Control comparison: LP_SAMPLE_CNT = 1 transitions LP -> NP (TRANS = 1)
  // within the same 1500 us window, while NP_SAMPLE_CNT = 0 (16'h0000 ->
  // 16'hFFFF) prevents FILTER_STATUS.MATCH from firing after 1500 us!
  adc_ctrl_full_reset_and_clean();
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_PD_CTL_REG_OFFSET, pd_ctl);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_LP_SAMPLE_CTL_REG_OFFSET, 1u);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_SAMPLE_CTL_REG_OFFSET, 0u);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_CHN0_FILTER_CTL_0_REG_OFFSET,
                   catch_all);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_CHN1_FILTER_CTL_0_REG_OFFSET,
                   catch_all);
  sync_aon();
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET,
                   1u << ADC_CTRL_ADC_EN_CTL_ADC_ENABLE_BIT);
  busy_spin_micros(1500);
  filter_st = abs_mmio_read32(kAdcCtrlBase + ADC_CTRL_FILTER_STATUS_REG_OFFSET);
  CHECK((filter_st & (1u << ADC_CTRL_FILTER_STATUS_TRANS_BIT)) != 0u &&
            (filter_st & 0xFFu) == 0u,
        "Expected LP_SAMPLE_CNT=1 to set TRANS=1 while NP_SAMPLE_CNT=0 "
        "(underflow to 65535) keeps MATCH=0 after 1500us, got 0x%x",
        filter_st);

  // Also verify in-flight NP_SAMPLE_CNT reduction bypass:
  // Start in NP mode with NP_SAMPLE_CNT = 20000, wait ~2 ms so np_sample_cnt_q
  // increments past 5, then reduce NP_SAMPLE_CNT to 2 (thresh = 1). Because
  // np_sample_cnt_q > 1, np_sample_cnt_q == np_sample_cnt_thresh is false and
  // np_sample_cnt_en stays 0, so FILTER_STATUS.MATCH stays 0!
  adc_ctrl_full_reset_and_clean();
  pd_ctl = bitfield_bit32_write(pd_ctl, ADC_CTRL_ADC_PD_CTL_LP_MODE_BIT, false);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_PD_CTL_REG_OFFSET, pd_ctl);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_SAMPLE_CTL_REG_OFFSET, 20000u);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_CHN0_FILTER_CTL_0_REG_OFFSET,
                   catch_all);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_CHN1_FILTER_CTL_0_REG_OFFSET,
                   catch_all);
  sync_aon();

  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET,
                   1u << ADC_CTRL_ADC_EN_CTL_ADC_ENABLE_BIT);
  busy_spin_micros(2000);
  // Reduce NP_SAMPLE_CNT in-flight to 2 (threshold = 1) while np_sample_cnt_q >
  // 1.
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_SAMPLE_CTL_REG_OFFSET, 2u);
  sync_aon();
  busy_spin_micros(1000);

  filter_st = abs_mmio_read32(kAdcCtrlBase + ADC_CTRL_FILTER_STATUS_REG_OFFSET);
  CHECK(filter_st == 0u,
        "Expected in-flight reduction of NP_SAMPLE_CNT below np_sample_cnt_q "
        "to bypass NP_DONE (FILTER_STATUS=0x%x)",
        filter_st);
}

static void test_intr_ctl_gates_status_while_chn_val_and_fsm_rst_behave(void) {
  LOG_INFO(
      "Test 6: ADC_INTR_CTL gates ADC_INTR_STATUS while ADC_CHN_VAL latches "
      "and survives ADC_FSM_RST");
  adc_ctrl_full_reset_and_clean();

  uint32_t pd_ctl =
      bitfield_field32_write(0u, ADC_CTRL_ADC_PD_CTL_PWRUP_TIME_FIELD, 1u);
  pd_ctl =
      bitfield_field32_write(pd_ctl, ADC_CTRL_ADC_PD_CTL_WAKEUP_TIME_FIELD, 1u);
  pd_ctl = bitfield_bit32_write(pd_ctl, ADC_CTRL_ADC_PD_CTL_LP_MODE_BIT, true);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_PD_CTL_REG_OFFSET, pd_ctl);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_LP_SAMPLE_CTL_REG_OFFSET, 1u);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_SAMPLE_CTL_REG_OFFSET, 1u);

  // Leave ADC_INTR_CTL = 0 (all interrupt sources disabled: MATCH_EN=0,
  // TRANS_EN=0, ONESHOT_EN=0).
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_INTR_CTL_REG_OFFSET, 0u);

  // Part A: Run oneshot mode with ADC_INTR_CTL.ONESHOT_EN == 0.
  // Despite adc_ctrl.hjson:100 stating oneshot sets ADC_INTR_STATUS.ONESHOT on
  // completion, adc_ctrl_intr.sv:115 gates oneshot.de with
  // cfg_oneshot_done_en_i so ADC_INTR_STATUS remains 0 even after oneshot
  // completes and latches ADC_CHN_VAL_0/1!
  uint32_t en_oneshot = (1u << ADC_CTRL_ADC_EN_CTL_ADC_ENABLE_BIT) |
                        (1u << ADC_CTRL_ADC_EN_CTL_ONESHOT_MODE_BIT);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET, en_oneshot);
  busy_spin_micros(1000);

  CHECK((abs_mmio_read32(kAdcCtrlBase + ADC_CTRL_ADC_FSM_STATE_REG_OFFSET) &
         0x1Fu) == ADC_CTRL_ADC_FSM_STATE_STATE_VALUE_PWRDN,
        "Oneshot must have completed and returned to PWRDN");
  CHECK(
      abs_mmio_read32(kAdcCtrlBase + ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET) == 0u,
      "ADC_INTR_STATUS.ONESHOT must remain 0 when ADC_INTR_CTL.ONESHOT_EN==0");
  uint32_t chn0_part_a =
      abs_mmio_read32(kAdcCtrlBase + ADC_CTRL_ADC_CHN_VAL_0_REG_OFFSET);
  uint32_t chn1_part_a =
      abs_mmio_read32(kAdcCtrlBase + ADC_CTRL_ADC_CHN_VAL_1_REG_OFFSET);
  CHECK(bitfield_field32_read(
            chn0_part_a, ADC_CTRL_ADC_CHN_VAL_0_ADC_CHN_VALUE_INTR_0_FIELD) ==
            bitfield_field32_read(chn0_part_a,
                                  ADC_CTRL_ADC_CHN_VAL_0_ADC_CHN_VALUE_0_FIELD),
        "Expected ADC_CHN_VAL_0.adc_chn_value_intr latched in Part A");
  CHECK(bitfield_field32_read(
            chn1_part_a, ADC_CTRL_ADC_CHN_VAL_1_ADC_CHN_VALUE_INTR_1_FIELD) ==
            bitfield_field32_read(chn1_part_a,
                                  ADC_CTRL_ADC_CHN_VAL_1_ADC_CHN_VALUE_1_FIELD),
        "Expected ADC_CHN_VAL_1.adc_chn_value_intr latched in Part A");

  // Part B: Run LP->NP filter match with ADC_INTR_CTL == 0.
  // FILTER_STATUS latches both TRANS and MATCH, and ADC_CHN_VAL_0/1 latches
  // adc_chn_value_intr, while ADC_INTR_STATUS remains 0!
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET, 0u);
  sync_aon();
  uint32_t catch_all = pack_filter(true, false, 0u, 0x3FFu);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_CHN0_FILTER_CTL_0_REG_OFFSET,
                   catch_all);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_CHN1_FILTER_CTL_0_REG_OFFSET,
                   catch_all);
  sync_aon();

  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET,
                   1u << ADC_CTRL_ADC_EN_CTL_ADC_ENABLE_BIT);
  ibex_timeout_t t = ibex_timeout_init(kPollTimeoutUs);
  while (!ibex_timeout_check(&t)) {
    uint32_t fst =
        abs_mmio_read32(kAdcCtrlBase + ADC_CTRL_FILTER_STATUS_REG_OFFSET);
    if ((fst & 0x101u) == 0x101u) {
      break;
    }
  }
  uint32_t fst =
      abs_mmio_read32(kAdcCtrlBase + ADC_CTRL_FILTER_STATUS_REG_OFFSET);
  CHECK((fst & 0x101u) == 0x101u,
        "Expected FILTER_STATUS to latch TRANS (bit 8) and MATCH[0] (bit 0), "
        "got 0x%x",
        fst);
  CHECK(
      abs_mmio_read32(kAdcCtrlBase + ADC_CTRL_ADC_INTR_STATUS_REG_OFFSET) == 0u,
      "Expected ADC_INTR_STATUS to remain 0 when ADC_INTR_CTL == 0");

  // Part C: Assert ADC_FSM_RST = 1 after stopping ADC_EN_CTL.
  // In adc_ctrl_fsm.sv:144-149, cfg_fsm_rst_i clears chn0_val_o <= 0 alongside
  // chn0_val_we_o <= 0, so adc_chn_val_o[0].adc_chn_value.de is 0 and the
  // register-file flops ADC_CHN_VAL_0/1 retain their sampled values!
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_EN_CTL_REG_OFFSET, 0u);
  sync_aon();
  uint32_t chn0_before_rst =
      abs_mmio_read32(kAdcCtrlBase + ADC_CTRL_ADC_CHN_VAL_0_REG_OFFSET);
  uint32_t chn1_before_rst =
      abs_mmio_read32(kAdcCtrlBase + ADC_CTRL_ADC_CHN_VAL_1_REG_OFFSET);
  CHECK(
      bitfield_field32_read(
          chn0_before_rst, ADC_CTRL_ADC_CHN_VAL_0_ADC_CHN_VALUE_INTR_0_FIELD) ==
          bitfield_field32_read(chn0_before_rst,
                                ADC_CTRL_ADC_CHN_VAL_0_ADC_CHN_VALUE_0_FIELD),
      "Expected ADC_CHN_VAL_0.adc_chn_value_intr latched in Part B");
  CHECK(
      bitfield_field32_read(
          chn1_before_rst, ADC_CTRL_ADC_CHN_VAL_1_ADC_CHN_VALUE_INTR_1_FIELD) ==
          bitfield_field32_read(chn1_before_rst,
                                ADC_CTRL_ADC_CHN_VAL_1_ADC_CHN_VALUE_1_FIELD),
      "Expected ADC_CHN_VAL_1.adc_chn_value_intr latched in Part B");

  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_FSM_RST_REG_OFFSET, 1u);
  sync_aon();
  uint32_t chn0_after_rst =
      abs_mmio_read32(kAdcCtrlBase + ADC_CTRL_ADC_CHN_VAL_0_REG_OFFSET);
  uint32_t chn1_after_rst =
      abs_mmio_read32(kAdcCtrlBase + ADC_CTRL_ADC_CHN_VAL_1_REG_OFFSET);
  CHECK(chn0_after_rst == chn0_before_rst && chn1_after_rst == chn1_before_rst,
        "Expected ADC_CHN_VAL_0 (0x%x->0x%x) and ADC_CHN_VAL_1 (0x%x->0x%x) "
        "to be preserved across ADC_FSM_RST",
        chn0_before_rst, chn0_after_rst, chn1_before_rst, chn1_after_rst);
  abs_mmio_write32(kAdcCtrlBase + ADC_CTRL_ADC_FSM_RST_REG_OFFSET, 0u);
  sync_aon();
}

bool test_main(void) {
  test_oneshot_sticky_enable_and_retrigger();
  test_chn_val_intr_overwrite_and_oneshot_latch();
  test_fsm_rst_level_hold_and_release_retrigger();
  test_subword_write_permit_faults();
  test_sample_cnt_zero_underflow_and_inflight_reduction();
  test_intr_ctl_gates_status_while_chn_val_and_fsm_rst_behave();
  adc_ctrl_full_reset_and_clean();
  LOG_INFO("All adc_ctrl trunk-v2 errata tests passed on CW340 FPGA!");
  return true;
}
