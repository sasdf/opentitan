// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sysrst_ctrl_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kSysrstBase = TOP_EARLGREY_SYSRST_CTRL_AON_BASE_ADDR,
};

static volatile bool g_expect_access_fault = false;
static volatile uint32_t g_access_fault_count = 0;
static volatile uint32_t g_last_mcause = 0;

void ottf_exception_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = 0;
  CSR_READ(CSR_REG_MCAUSE, &mcause);
  if (g_expect_access_fault && (mcause == 5u || mcause == 7u)) {
    g_access_fault_count++;
    g_last_mcause = mcause;
    uint32_t mepc = 0;
    CSR_READ(CSR_REG_MEPC, &mepc);
    uint16_t inst16 = *(const uint16_t *)mepc;
    uint32_t step = ((inst16 & 0x3u) != 0x3u) ? 2u : 4u;
    CSR_WRITE(CSR_REG_MEPC, mepc + step);
    return;
  }
  ottf_generic_fault_print(exc_info, "FAULT", mcause);
  abort();
}

#define EXPECT_RTL(cond, ...)                  \
  do {                                         \
    if (!(cond)) {                             \
      LOG_ERROR("RTL_MISMATCH: " __VA_ARGS__); \
      all_ok = false;                          \
    }                                          \
  } while (0)

static void check_rw_mask(uint32_t offset, uint32_t expected_mask,
                          uint32_t restore_val, const char *name,
                          bool *all_ok_ptr) {
  bool all_ok = *all_ok_ptr;
  abs_mmio_write32(kSysrstBase + offset, 0xffffffffu);
  busy_spin_micros(20);
  uint32_t got = abs_mmio_read32(kSysrstBase + offset);
  EXPECT_RTL(got == expected_mask,
             "%s write 0xffffffff read back 0x%08x, expected 0x%08x", name, got,
             expected_mask);
  abs_mmio_write32(kSysrstBase + offset, restore_val);
  busy_spin_micros(20);
  *all_ok_ptr = all_ok;
}

bool test_main(void) {
  bool all_ok = true;

  // Clear all control and status registers before testing.
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_ULP_CTL_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INTR_CTL_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COM_PRE_SEL_CTL_0_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COM_SEL_CTL_0_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COM_OUT_CTL_0_REG_OFFSET, 0u);
  busy_spin_micros(30);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_ULP_STATUS_REG_OFFSET, 1u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COMBO_INTR_STATUS_REG_OFFSET,
                   0xfu);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INTR_STATUS_REG_OFFSET,
                   0x3fffu);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_WKUP_STATUS_REG_OFFSET, 1u);
  busy_spin_micros(30);

  // 1. Verify register field bitmasks on RW registers.
  check_rw_mask(SYSRST_CTRL_ULP_AC_DEBOUNCE_CTL_REG_OFFSET, 0x0000ffffu, 8000u,
                "ULP_AC_DEBOUNCE_CTL", &all_ok);
  check_rw_mask(SYSRST_CTRL_ULP_LID_DEBOUNCE_CTL_REG_OFFSET, 0x0000ffffu, 8000u,
                "ULP_LID_DEBOUNCE_CTL", &all_ok);
  check_rw_mask(SYSRST_CTRL_ULP_PWRB_DEBOUNCE_CTL_REG_OFFSET, 0x0000ffffu,
                8000u, "ULP_PWRB_DEBOUNCE_CTL", &all_ok);
  check_rw_mask(SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET, 0x00000fffu, 0u,
                "KEY_INVERT_CTL", &all_ok);
  check_rw_mask(SYSRST_CTRL_PIN_ALLOWED_CTL_REG_OFFSET, 0x0000ffffu, 0x82u,
                "PIN_ALLOWED_CTL", &all_ok);
  check_rw_mask(SYSRST_CTRL_PIN_OUT_CTL_REG_OFFSET, 0x000000ffu, 0x82u,
                "PIN_OUT_CTL", &all_ok);
  check_rw_mask(SYSRST_CTRL_PIN_OUT_VALUE_REG_OFFSET, 0x000000ffu, 0u,
                "PIN_OUT_VALUE", &all_ok);
  check_rw_mask(SYSRST_CTRL_KEY_INTR_CTL_REG_OFFSET, 0x00003fffu, 0u,
                "KEY_INTR_CTL", &all_ok);
  check_rw_mask(SYSRST_CTRL_KEY_INTR_DEBOUNCE_CTL_REG_OFFSET, 0x0000ffffu,
                2000u, "KEY_INTR_DEBOUNCE_CTL", &all_ok);
  check_rw_mask(SYSRST_CTRL_AUTO_BLOCK_DEBOUNCE_CTL_REG_OFFSET, 0x0001ffffu,
                2000u, "AUTO_BLOCK_DEBOUNCE_CTL", &all_ok);
  check_rw_mask(SYSRST_CTRL_AUTO_BLOCK_OUT_CTL_REG_OFFSET, 0x00000077u, 0u,
                "AUTO_BLOCK_OUT_CTL", &all_ok);
  check_rw_mask(SYSRST_CTRL_COM_PRE_SEL_CTL_0_REG_OFFSET, 0x0000001fu, 0u,
                "COM_PRE_SEL_CTL_0", &all_ok);
  check_rw_mask(SYSRST_CTRL_COM_SEL_CTL_0_REG_OFFSET, 0x0000001fu, 0u,
                "COM_SEL_CTL_0", &all_ok);
  check_rw_mask(SYSRST_CTRL_COM_OUT_CTL_0_REG_OFFSET, 0x0000000fu, 0u,
                "COM_OUT_CTL_0", &all_ok);

  // Clear any events generated during mask checks.
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_ULP_STATUS_REG_OFFSET, 1u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COMBO_INTR_STATUS_REG_OFFSET,
                   0xfu);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INTR_STATUS_REG_OFFSET,
                   0x3fffu);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_WKUP_STATUS_REG_OFFSET, 1u);
  busy_spin_micros(30);

  // 2. KEY_INVERT_CTL input inversion occurs upstream of
  // u_prim_flop_2sync_input (sysrst_ctrl.sv:106-142). Toggling
  // KEY_INVERT_CTL.key0_in must produce key0_in_L2H and key0_in_H2L transitions
  // in sysrst_ctrl_keyintr.
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INTR_DEBOUNCE_CTL_REG_OFFSET,
                   0u);
  uint32_t key0_intr_mask = (1u << SYSRST_CTRL_KEY_INTR_CTL_KEY0_IN_H2L_BIT) |
                            (1u << SYSRST_CTRL_KEY_INTR_CTL_KEY0_IN_L2H_BIT);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INTR_CTL_REG_OFFSET,
                   key0_intr_mask);
  busy_spin_micros(30);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INTR_STATUS_REG_OFFSET,
                   0x3fffu);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_WKUP_STATUS_REG_OFFSET, 1u);
  busy_spin_micros(30);

  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET,
                   1u << SYSRST_CTRL_KEY_INVERT_CTL_KEY0_IN_BIT);
  busy_spin_micros(50);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET, 0u);
  busy_spin_micros(50);

  uint32_t key_sts =
      abs_mmio_read32(kSysrstBase + SYSRST_CTRL_KEY_INTR_STATUS_REG_OFFSET);
  uint32_t expected_key_sts =
      (1u << SYSRST_CTRL_KEY_INTR_STATUS_KEY0_IN_H2L_BIT) |
      (1u << SYSRST_CTRL_KEY_INTR_STATUS_KEY0_IN_L2H_BIT);
  EXPECT_RTL((key_sts & expected_key_sts) == expected_key_sts,
             "Toggling KEY_INVERT_CTL.key0_in did not set KEY_INTR_STATUS "
             "(got 0x%08x, expected 0x%08x)",
             key_sts, expected_key_sts);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INTR_CTL_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INTR_STATUS_REG_OFFSET,
                   0x3fffu);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_WKUP_STATUS_REG_OFFSET, 1u);
  busy_spin_micros(30);

  // 3. ULP_AC_DEBOUNCE_CTL debounce timing and Sticky=1 single-pulse behavior
  // (sysrst_ctrl_ulp.sv / sysrst_ctrl_detect.sv).
  uint32_t pin_in =
      abs_mmio_read32(kSysrstBase + SYSRST_CTRL_PIN_IN_VALUE_REG_OFFSET);
  uint32_t raw_ac = (pin_in >> SYSRST_CTRL_PIN_IN_VALUE_AC_PRESENT_BIT) & 1u;
  uint32_t inv_ac =
      raw_ac ? 0u : (1u << SYSRST_CTRL_KEY_INVERT_CTL_AC_PRESENT_BIT);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET, inv_ac);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_ULP_AC_DEBOUNCE_CTL_REG_OFFSET,
                   200u);
  busy_spin_micros(30);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_ULP_STATUS_REG_OFFSET, 1u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_WKUP_STATUS_REG_OFFSET, 1u);
  busy_spin_micros(30);

  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_ULP_CTL_REG_OFFSET, 1u);
  busy_spin_micros(50);
  uint32_t ulp_early =
      abs_mmio_read32(kSysrstBase + SYSRST_CTRL_ULP_STATUS_REG_OFFSET);
  EXPECT_RTL(ulp_early == 0u,
             "ULP_STATUS asserted before ULP_AC_DEBOUNCE_CTL (200 cycles = "
             "~1ms) elapsed (got 0x%08x)",
             ulp_early);

  busy_spin_micros(1200);
  uint32_t ulp_late =
      abs_mmio_read32(kSysrstBase + SYSRST_CTRL_ULP_STATUS_REG_OFFSET);
  EXPECT_RTL(ulp_late == 1u,
             "ULP_STATUS did not assert after ULP_AC_DEBOUNCE_CTL elapsed "
             "(got 0x%08x)",
             ulp_late);

  // Clear ULP_STATUS and WKUP_STATUS while ULP_CTL remains enabled (StableSt,
  // Sticky=1). Writing another CSR while ULP_CTL stays 1 must NOT re-pulse
  // ulp_wakeup_pulse_o.
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_ULP_STATUS_REG_OFFSET, 1u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_WKUP_STATUS_REG_OFFSET, 1u);
  busy_spin_micros(30);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_ULP_CTL_REG_OFFSET, 1u);
  busy_spin_micros(30);
  uint32_t ulp_sticky =
      abs_mmio_read32(kSysrstBase + SYSRST_CTRL_ULP_STATUS_REG_OFFSET);
  EXPECT_RTL(ulp_sticky == 0u,
             "ULP_STATUS re-asserted while ULP detector remained in Sticky "
             "StableSt without disabling ULP_CTL (got 0x%08x)",
             ulp_sticky);

  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_ULP_CTL_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_ULP_STATUS_REG_OFFSET, 1u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_WKUP_STATUS_REG_OFFSET, 1u);
  busy_spin_micros(30);

  // 4. Combo precondition gating via COM_PRE_SEL_CTL_0 (sysrst_ctrl_combo.sv):
  // Ensure key0_int and key1_int start at 1 (unpressed).
  uint32_t raw_key0 = (pin_in >> SYSRST_CTRL_PIN_IN_VALUE_KEY0_IN_BIT) & 1u;
  uint32_t raw_key1 = (pin_in >> SYSRST_CTRL_PIN_IN_VALUE_KEY1_IN_BIT) & 1u;
  uint32_t inv_both_unpressed =
      (raw_key0 ? 0u : (1u << SYSRST_CTRL_KEY_INVERT_CTL_KEY0_IN_BIT)) |
      (raw_key1 ? 0u : (1u << SYSRST_CTRL_KEY_INVERT_CTL_KEY1_IN_BIT));
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET,
                   inv_both_unpressed);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INTR_DEBOUNCE_CTL_REG_OFFSET,
                   0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COM_PRE_DET_CTL_0_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COM_DET_CTL_0_REG_OFFSET, 0u);
  // Require key1_in_sel (bit 1) as precondition, and key0_in_sel (bit 0) as
  // combo trigger.
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COM_PRE_SEL_CTL_0_REG_OFFSET,
                   1u << 1u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COM_SEL_CTL_0_REG_OFFSET,
                   1u << 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COM_OUT_CTL_0_REG_OFFSET,
                   1u << 1u);
  busy_spin_micros(30);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COMBO_INTR_STATUS_REG_OFFSET,
                   0xfu);
  busy_spin_micros(30);

  // Press key0_int (set to 0) while key1_int is still 1 (precondition NOT met).
  uint32_t inv_key0_pressed_only =
      inv_both_unpressed ^ (1u << SYSRST_CTRL_KEY_INVERT_CTL_KEY0_IN_BIT);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET,
                   inv_key0_pressed_only);
  busy_spin_micros(50);
  uint32_t combo_sts_no_pre =
      abs_mmio_read32(kSysrstBase + SYSRST_CTRL_COMBO_INTR_STATUS_REG_OFFSET);
  EXPECT_RTL(combo_sts_no_pre == 0u,
             "COMBO_INTR_STATUS fired when COM_PRE_SEL_CTL_0 precondition was "
             "not satisfied (got 0x%08x)",
             combo_sts_no_pre);

  // Release key0_int, then press key1_int (satisfy precondition), then press
  // key0_int.
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET,
                   inv_both_unpressed);
  busy_spin_micros(30);
  uint32_t inv_key1_pressed =
      inv_both_unpressed ^ (1u << SYSRST_CTRL_KEY_INVERT_CTL_KEY1_IN_BIT);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET,
                   inv_key1_pressed);
  busy_spin_micros(50);
  uint32_t inv_both_pressed =
      inv_key1_pressed ^ (1u << SYSRST_CTRL_KEY_INVERT_CTL_KEY0_IN_BIT);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET,
                   inv_both_pressed);
  busy_spin_micros(50);
  uint32_t combo_sts_with_pre =
      abs_mmio_read32(kSysrstBase + SYSRST_CTRL_COMBO_INTR_STATUS_REG_OFFSET);
  EXPECT_RTL((combo_sts_with_pre & 1u) == 1u,
             "COMBO_INTR_STATUS did not fire after COM_PRE_SEL_CTL_0 "
             "precondition was satisfied (got 0x%08x)",
             combo_sts_with_pre);

  // Clean up combo registers before locking REGWEN.
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COM_PRE_SEL_CTL_0_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COM_SEL_CTL_0_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COM_OUT_CTL_0_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COMBO_INTR_STATUS_REG_OFFSET,
                   0xfu);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_WKUP_STATUS_REG_OFFSET, 1u);
  busy_spin_micros(30);

  // 5. REGWEN (rw0c) locks EC_RST_CTL and PIN_ALLOWED_CTL, but MUST NOT lock
  // PIN_OUT_CTL or PIN_OUT_VALUE (sysrst_ctrl.hjson:473-573,
  // sysrst_ctrl_reg_top.sv:693, 745).
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_REGWEN_REG_OFFSET, 0u);
  EXPECT_RTL(abs_mmio_read32(kSysrstBase + SYSRST_CTRL_REGWEN_REG_OFFSET) == 0u,
             "REGWEN did not clear on write 0");

  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_EC_RST_CTL_REG_OFFSET, 0x1234u);
  busy_spin_micros(20);
  EXPECT_RTL(
      abs_mmio_read32(kSysrstBase + SYSRST_CTRL_EC_RST_CTL_REG_OFFSET) == 2000u,
      "EC_RST_CTL modified while REGWEN == 0");

  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_PIN_OUT_CTL_REG_OFFSET, 0u);
  busy_spin_micros(20);
  uint32_t pin_out_ctl =
      abs_mmio_read32(kSysrstBase + SYSRST_CTRL_PIN_OUT_CTL_REG_OFFSET);
  EXPECT_RTL(pin_out_ctl == 0u,
             "PIN_OUT_CTL must remain writable when REGWEN == 0 (got 0x%08x, "
             "expected 0x0)",
             pin_out_ctl);

  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_PIN_OUT_VALUE_REG_OFFSET, 0xa5u);
  busy_spin_micros(20);
  uint32_t pin_out_val =
      abs_mmio_read32(kSysrstBase + SYSRST_CTRL_PIN_OUT_VALUE_REG_OFFSET);
  EXPECT_RTL(pin_out_val == 0xa5u,
             "PIN_OUT_VALUE must remain writable when REGWEN == 0 (got 0x%08x, "
             "expected 0xa5)",
             pin_out_val);

  // Restore PIN_OUT_CTL and PIN_OUT_VALUE.
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_PIN_OUT_CTL_REG_OFFSET, 0x82u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_PIN_OUT_VALUE_REG_OFFSET, 0u);
  busy_spin_micros(20);

  // 6. ULP_CTL is NOT gated by REGWEN (sysrst_ctrl.hjson:266-278).
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_ULP_CTL_REG_OFFSET, 1u);
  busy_spin_micros(20);
  EXPECT_RTL(
      abs_mmio_read32(kSysrstBase + SYSRST_CTRL_ULP_CTL_REG_OFFSET) == 1u,
      "ULP_CTL must remain writable when REGWEN == 0");
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_ULP_CTL_REG_OFFSET, 0u);
  busy_spin_micros(20);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_ULP_STATUS_REG_OFFSET, 1u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_WKUP_STATUS_REG_OFFSET, 1u);
  busy_spin_micros(20);

  // 7. Status-type INTR_STATE / INTR_TEST behavior (prim_intr_hw
  // IntrT="Status") and WO readback for INTR_TEST / ALERT_TEST.
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_INTR_TEST_REG_OFFSET, 1u);
  EXPECT_RTL(
      abs_mmio_read32(kSysrstBase + SYSRST_CTRL_INTR_TEST_REG_OFFSET) == 0u,
      "INTR_TEST (WO) readback must be 0");
  EXPECT_RTL(
      abs_mmio_read32(kSysrstBase + SYSRST_CTRL_ALERT_TEST_REG_OFFSET) == 0u,
      "ALERT_TEST (WO) readback must be 0");
  EXPECT_RTL(
      abs_mmio_read32(kSysrstBase + SYSRST_CTRL_INTR_STATE_REG_OFFSET) == 1u,
      "INTR_STATE must assert when INTR_TEST=1");
  // Writing 1 to INTR_STATE must NOT clear it (Status-type RO interrupt).
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_INTR_STATE_REG_OFFSET, 1u);
  EXPECT_RTL(
      abs_mmio_read32(kSysrstBase + SYSRST_CTRL_INTR_STATE_REG_OFFSET) == 1u,
      "INTR_STATE (Status-type RO) must not clear on write 1");
  // Writing 0 to INTR_TEST clears test_q and deasserts INTR_STATE.
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_INTR_TEST_REG_OFFSET, 0u);
  EXPECT_RTL(
      abs_mmio_read32(kSysrstBase + SYSRST_CTRL_INTR_STATE_REG_OFFSET) == 0u,
      "INTR_STATE must deassert when INTR_TEST=0 and all status registers are "
      "0");

  // 8. Wave 5: SYSRST_CTRL_PERMIT sub-word write wr_err and addrmiss checks.
  g_access_fault_count = 0;
  abs_mmio_write8(kSysrstBase + SYSRST_CTRL_PIN_OUT_VALUE_REG_OFFSET, 0x55u);
  busy_spin_micros(20);
  EXPECT_RTL(g_access_fault_count == 0u &&
                 abs_mmio_read32(kSysrstBase +
                                 SYSRST_CTRL_PIN_OUT_VALUE_REG_OFFSET) == 0x55u,
             "8-bit write to PIN_OUT_VALUE+0 (PERMIT=4'b0001) must succeed");

  g_access_fault_count = 0;
  g_last_mcause = 0;
  g_expect_access_fault = true;
  abs_mmio_write8(kSysrstBase + SYSRST_CTRL_PIN_OUT_VALUE_REG_OFFSET + 1u,
                  0xaau);
  g_expect_access_fault = false;
  EXPECT_RTL(g_access_fault_count == 1u && g_last_mcause == 7u &&
                 abs_mmio_read32(kSysrstBase +
                                 SYSRST_CTRL_PIN_OUT_VALUE_REG_OFFSET) == 0x55u,
             "8-bit write to PIN_OUT_VALUE+1 (PERMIT=4'b0001, reg_be=4'b0010) "
             "must raise Store Access Fault (mcause=7) and preserve 0x55");
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_PIN_OUT_VALUE_REG_OFFSET, 0u);

  g_access_fault_count = 0;
  g_last_mcause = 0;
  g_expect_access_fault = true;
  abs_mmio_write8(kSysrstBase + SYSRST_CTRL_EC_RST_CTL_REG_OFFSET, 0x10u);
  g_expect_access_fault = false;
  EXPECT_RTL(g_access_fault_count == 1u && g_last_mcause == 7u,
             "8-bit write to EC_RST_CTL (PERMIT=4'b0011) must raise Store "
             "Access Fault (mcause=7)");

  g_access_fault_count = 0;
  g_last_mcause = 0;
  g_expect_access_fault = true;
  abs_mmio_write8(kSysrstBase + SYSRST_CTRL_COM_DET_CTL_0_REG_OFFSET, 0x10u);
  g_expect_access_fault = false;
  EXPECT_RTL(g_access_fault_count == 1u && g_last_mcause == 7u,
             "8-bit write to COM_DET_CTL_0 (PERMIT=4'b1111) must raise Store "
             "Access Fault (mcause=7)");

  g_access_fault_count = 0;
  g_last_mcause = 0;
  g_expect_access_fault = true;
  (void)abs_mmio_read32(kSysrstBase + 0xacu);
  g_expect_access_fault = false;
  EXPECT_RTL(g_access_fault_count == 1u && g_last_mcause == 5u,
             "32-bit read at unmapped offset 0xac (addrmiss) must raise Load "
             "Access Fault (mcause=5)");

  g_access_fault_count = 0;
  g_last_mcause = 0;
  g_expect_access_fault = true;
  abs_mmio_write32(kSysrstBase + 0xacu, 0x12345678u);
  g_expect_access_fault = false;
  EXPECT_RTL(g_access_fault_count == 1u && g_last_mcause == 7u,
             "32-bit write at unmapped offset 0xac (addrmiss) must raise Store "
             "Access Fault (mcause=7)");

  return all_ok;
}
