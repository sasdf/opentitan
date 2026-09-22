// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"
#include "sw/device/silicon_creator/lib/drivers/retention_sram.h"

#include "alert_handler_regs.h"
#include "aon_timer_regs.h"
#include "clkmgr_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "i2c_regs.h"
#include "rstmgr_regs.h"
#include "uart_regs.h"
#include "usbdev_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kRstmgrBase = TOP_EARLGREY_RSTMGR_AON_BASE_ADDR,
  kAlertBase = TOP_EARLGREY_ALERT_HANDLER_BASE_ADDR,
  kAonTimerBase = TOP_EARLGREY_AON_TIMER_AON_BASE_ADDR,
  kClkmgrBase = TOP_EARLGREY_CLKMGR_AON_BASE_ADDR,
  kUart0Base = TOP_EARLGREY_UART0_BASE_ADDR,
  kI2c0Base = TOP_EARLGREY_I2C0_BASE_ADDR,
  kUsbdevBase = TOP_EARLGREY_USBDEV_BASE_ADDR,
  kRetSramMagicCancelTest = 0x52535431u,
  kRetSramMagicExpectedSwReset = 0x52535432u,
};

static volatile uint32_t g_expected_fault_count = 0;
static volatile uint32_t g_last_mcause = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  g_last_mcause = ibex_mcause_read();
  g_expected_fault_count++;
}

bool test_main(void) {
  uint32_t failures = 0;
  retention_sram_t *ret_sram = retention_sram_get();
  uint32_t reset_info = ret_sram->creator.reset_reasons;
  uint32_t phase = ret_sram->creator.reserved[0];

  // Disable alert_handler CPU interrupts so OTTF alert catcher ISR does not
  // abort when we test RSTMGR/UART0 ALERT_TEST pulses into ALERT_CAUSE.
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_INTR_ENABLE_REG_OFFSET, 0u);

  LOG_INFO("rstmgr_rtl_consistency_test: reset_info=0x%x phase=0x%x",
           reset_info, phase);

  if (phase == kRetSramMagicCancelTest) {
    // If we arrived here after a reset while phase == kRetSramMagicCancelTest,
    // it means writing 0x6 to RESET_REQ and immediately clearing it to 0x9
    // caused an unexpected system reset (QEMU failed to cancel sw_reset_timer).
    LOG_ERROR(
        "RTL_MISMATCH: RESET_REQ=0x6 immediately cleared to 0x9 caused an "
        "unexpected system reset (reset_info=0x%x)",
        reset_info);
    failures++;
    // Proceed to set up and run the intentional SW_RESET phase so we also
    // check post-reset register preservation and ALERT_INFO indexing.
    abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET, 0u);
    abs_mmio_write32(kRstmgrBase + RSTMGR_RESET_INFO_REG_OFFSET, reset_info);
    abs_mmio_write32(kUart0Base + UART_ALERT_TEST_REG_OFFSET, 0x1u);
    abs_mmio_write32(kRstmgrBase + RSTMGR_ALERT_INFO_CTRL_REG_OFFSET,
                     (15u << RSTMGR_ALERT_INFO_CTRL_INDEX_OFFSET) |
                         (1u << RSTMGR_ALERT_INFO_CTRL_EN_BIT));
    abs_mmio_write32(kRstmgrBase + RSTMGR_CPU_INFO_CTRL_REG_OFFSET,
                     (5u << RSTMGR_CPU_INFO_CTRL_INDEX_OFFSET) |
                         (1u << RSTMGR_CPU_INFO_CTRL_EN_BIT));
    abs_mmio_write32(kRstmgrBase + RSTMGR_ALERT_REGWEN_REG_OFFSET, 0u);
    abs_mmio_write32(kRstmgrBase + RSTMGR_CPU_REGWEN_REG_OFFSET, 0u);
    ret_sram->creator.reserved[0] = kRetSramMagicExpectedSwReset;
    ret_sram->creator.reserved[1] = failures;
    abs_mmio_write32(kRstmgrBase + RSTMGR_RESET_REQ_REG_OFFSET,
                     kMultiBitBool4True);
    wait_for_interrupt();
  }

  if (phase != kRetSramMagicExpectedSwReset) {
    // =========================================================================
    // Phase 1: Initial Boot (POR) Checks
    // =========================================================================

    // 1. Check default register values.
    uint32_t reset_req =
        abs_mmio_read32(kRstmgrBase + RSTMGR_RESET_REQ_REG_OFFSET);
    if (reset_req != kMultiBitBool4False) {
      LOG_ERROR("RTL_MISMATCH: POR RESET_REQ expected 0x9, got 0x%x",
                reset_req);
      failures++;
    }

    uint32_t alert_attr =
        abs_mmio_read32(kRstmgrBase + RSTMGR_ALERT_INFO_ATTR_REG_OFFSET);
    uint32_t cpu_attr =
        abs_mmio_read32(kRstmgrBase + RSTMGR_CPU_INFO_ATTR_REG_OFFSET);
    if (alert_attr != 9u || cpu_attr != 8u) {
      LOG_ERROR(
          "RTL_MISMATCH: INFO_ATTR expected alert=9 cpu=8, got alert=%u cpu=%u",
          alert_attr, cpu_attr);
      failures++;
    }

    // 2. Test ALERT_TEST transient pulse semantics (rstmgr.sv:213-216).
    // Clear ALERT_CAUSE_23 (rstmgr fatal_fault) and ALERT_CAUSE_24
    // (rstmgr fatal_cnsty_fault) first.
    const uint32_t kCause23Addr =
        kAlertBase + ALERT_HANDLER_ALERT_CAUSE_23_REG_OFFSET;
    const uint32_t kCause24Addr =
        kAlertBase + ALERT_HANDLER_ALERT_CAUSE_24_REG_OFFSET;
    abs_mmio_write32(kCause23Addr, 1u);
    abs_mmio_write32(kCause24Addr, 1u);

    // Write 1 to ALERT_TEST.FATAL_FAULT -> should pulse alert 23 once.
    abs_mmio_write32(kRstmgrBase + RSTMGR_ALERT_TEST_REG_OFFSET,
                     1u << RSTMGR_ALERT_TEST_FATAL_FAULT_BIT);
    uint32_t cause23_first = abs_mmio_read32(kCause23Addr);
    if (cause23_first != 1u) {
      LOG_ERROR(
          "RTL_MISMATCH: first write to ALERT_TEST.FATAL_FAULT did not set "
          "ALERT_CAUSE_23 (got 0x%x)",
          cause23_first);
      failures++;
    }

    // Clear ALERT_CAUSE_23 in alert_handler. Because ALERT_TEST is a 1-cycle
    // pulse (prim_subreg_ext q & qe), ALERT_CAUSE_23 must stay 0 after
    // clearing, AND a second write of 1 to ALERT_TEST (without writing 0 in
    // between) must fire a new alert pulse setting ALERT_CAUSE_23 = 1 again.
    abs_mmio_write32(kCause23Addr, 1u);
    uint32_t cause23_cleared = abs_mmio_read32(kCause23Addr);
    if (cause23_cleared != 0u) {
      LOG_ERROR(
          "RTL_MISMATCH: ALERT_CAUSE_23 remained asserted (0x%x) after clear; "
          "ALERT_TEST is sticky instead of a transient pulse",
          cause23_cleared);
      failures++;
    }

    abs_mmio_write32(kRstmgrBase + RSTMGR_ALERT_TEST_REG_OFFSET,
                     1u << RSTMGR_ALERT_TEST_FATAL_FAULT_BIT);
    uint32_t cause23_second = abs_mmio_read32(kCause23Addr);
    if (cause23_second != 1u) {
      LOG_ERROR(
          "RTL_MISMATCH: second write of 1 to ALERT_TEST.FATAL_FAULT (without "
          "intervening 0 write) failed to set ALERT_CAUSE_23 (got 0x%x)",
          cause23_second);
      failures++;
    }
    abs_mmio_write32(kCause23Addr, 1u);

    // 3. Test SW_RST_CTRL_N active-low reset hold & USB (slot 3) vs USB_AON
    // (slot 4) reset separation.
    abs_mmio_write32(kClkmgrBase + CLKMGR_CLK_ENABLES_REG_OFFSET, 0xfu);

    // 3a. Write to I2C0.TIMING0, pulse SW_RST_CTRL_N_5 = 0 then 1, and verify
    // I2C0.TIMING0 is reset to 0. (Do not issue TL-UL MMIO to I2C0 while
    // SW_RST_CTRL_N_5 = 0, as u_reg_if is also held in reset and does not
    // return d_valid until SW_RST_CTRL_N_5 is released.)
    abs_mmio_write32(kI2c0Base + I2C_TIMING0_REG_OFFSET, 0x01230456u);
    CHECK(abs_mmio_read32(kI2c0Base + I2C_TIMING0_REG_OFFSET) == 0x01230456u);
    abs_mmio_write32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_5_REG_OFFSET, 0u);
    busy_spin_micros(10);
    abs_mmio_write32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_5_REG_OFFSET, 1u);
    busy_spin_micros(10);
    uint32_t i2c0_timing0 = abs_mmio_read32(kI2c0Base + I2C_TIMING0_REG_OFFSET);
    if (i2c0_timing0 != 0u) {
      LOG_ERROR(
          "RTL_MISMATCH: SW_RST_CTRL_N_5 failed to reset I2C0.TIMING0 "
          "(expected 0x0, got 0x%x)",
          i2c0_timing0);
      failures++;
    }

    // 3b. Test USB (SW_RST_CTRL_N_3) vs USB_AON (SW_RST_CTRL_N_4).
    // Activate USBDEV AON wake detection so USBDEV.WAKE_EVENTS.MODULE_ACTIVE=1.
    abs_mmio_write32(kUsbdevBase + USBDEV_WAKE_CONTROL_REG_OFFSET,
                     1u << USBDEV_WAKE_CONTROL_SUSPEND_REQ_BIT);
    busy_spin_micros(20);
    uint32_t wake_events_init =
        abs_mmio_read32(kUsbdevBase + USBDEV_WAKE_EVENTS_REG_OFFSET);
    if ((wake_events_init & (1u << USBDEV_WAKE_EVENTS_MODULE_ACTIVE_BIT)) ==
        0u) {
      LOG_ERROR("RTL_MISMATCH: USBDEV.WAKE_EVENTS.MODULE_ACTIVE not set (0x%x)",
                wake_events_init);
      failures++;
    }

    // Pulse SW_RST_CTRL_N_3 (USB non-AON reset: rst_usb_n).
    // In usbdev_reg_top.sv (u_wake_events_cdc), rst_src_ni (rst_usb_n) resets
    // src_q (the software readback register) to 0x0, while dst_qs_o (on
    // rst_dst_ni = rst_usb_aon_n) remains 0x301 (equal to dst_ds_i from
    // pinmux_aon). Because dst_qs_o == dst_ds_i, prim_reg_cdc_arb does not
    // emit a new dst_update pulse until SW_RST_CTRL_N_4 (rst_usb_aon_n) resets
    // dst_qs_o to 0x0, which then causes dst_qs_o != dst_ds_i upon release and
    // resynchronizes MODULE_ACTIVE=1 (0x301) back into WAKE_EVENTS!
    abs_mmio_write32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_3_REG_OFFSET, 0u);
    busy_spin_micros(10);
    abs_mmio_write32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_3_REG_OFFSET, 1u);
    busy_spin_micros(10);

    // Now pulse SW_RST_CTRL_N_4 (USB_AON reset: rst_usb_aon_n).
    // On RTL/FPGA, releasing SW_RST_CTRL_N_4 triggers u_wake_events_cdc to
    // resynchronize the active wake state from pinmux_aon (MODULE_ACTIVE=1).
    // In QEMU, SW_RST_CTRL_N_4 is unimplemented, so WAKE_EVENTS remains 0x0.
    abs_mmio_write32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_4_REG_OFFSET, 0u);
    busy_spin_micros(10);
    abs_mmio_write32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_4_REG_OFFSET, 1u);
    busy_spin_micros(30);
    uint32_t wake_events_after_usb_aon_rst =
        abs_mmio_read32(kUsbdevBase + USBDEV_WAKE_EVENTS_REG_OFFSET);
    if ((wake_events_after_usb_aon_rst &
         (1u << USBDEV_WAKE_EVENTS_MODULE_ACTIVE_BIT)) == 0u) {
      LOG_ERROR(
          "RTL_MISMATCH: SW_RST_CTRL_N_3 + SW_RST_CTRL_N_4 (USB_AON reset) "
          "failed to resynchronize USBDEV.WAKE_EVENTS.MODULE_ACTIVE (got 0x%x, "
          "expected MODULE_ACTIVE=1)",
          wake_events_after_usb_aon_rst);
      failures++;
    }

    // Acknowledge wake in WAKE_CONTROL to return AON wake detector to idle.
    abs_mmio_write32(kUsbdevBase + USBDEV_WAKE_CONTROL_REG_OFFSET,
                     1u << USBDEV_WAKE_CONTROL_WAKE_ACK_BIT);
    busy_spin_micros(30);

    // 4. Test RESET_REQ non-MuBi4True write and immediate MuBi4True ->
    // MuBi4False cancellation before AON clock sync (~5 us period on 200 kHz
    // clk_aon).
    abs_mmio_write32(kRstmgrBase + RSTMGR_RESET_REQ_REG_OFFSET, 0x5u);
    uint32_t req_invalid =
        abs_mmio_read32(kRstmgrBase + RSTMGR_RESET_REQ_REG_OFFSET);
    if (req_invalid != 0x5u) {
      LOG_ERROR("RTL_MISMATCH: RESET_REQ write 0x5 read back 0x%x",
                req_invalid);
      failures++;
    }
    busy_spin_micros(20);

    // Align right after a 200 kHz clk_aon rising edge using AON_TIMER so the
    // back-to-back 0x6 -> 0x9 writes complete >4.5 us before the next clk_aon
    // edge.
    abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET,
                     1u << AON_TIMER_WKUP_CTRL_ENABLE_BIT);
    uint32_t aon_cnt0 =
        abs_mmio_read32(kAonTimerBase + AON_TIMER_WKUP_COUNT_LO_REG_OFFSET);
    while (abs_mmio_read32(kAonTimerBase +
                           AON_TIMER_WKUP_COUNT_LO_REG_OFFSET) == aon_cnt0) {
    }
    busy_spin_micros(1);
    ret_sram->creator.reserved[0] = kRetSramMagicCancelTest;
    ret_sram->creator.reserved[1] = failures;
    abs_mmio_write32(kRstmgrBase + RSTMGR_RESET_REQ_REG_OFFSET,
                     kMultiBitBool4True);
    abs_mmio_write32(kRstmgrBase + RSTMGR_RESET_REQ_REG_OFFSET,
                     kMultiBitBool4False);
    abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET, 0u);
    busy_spin_micros(20);

    // If we reach here without resetting, the cancellation succeeded!
    // 5. Now configure ALERT_INFO_CTRL (INDEX=15, EN=1) and CPU_INFO_CTRL
    // (INDEX=5, EN=1), trigger UART0 alert_test (so alert_cause[0] at bit 211
    // -> alert_info_dump[6] bit 19 is non-zero), clear POR bit in RESET_INFO,
    // lock ALERT_REGWEN/CPU_REGWEN, and trigger an intentional SW_RESET.
    abs_mmio_write32(kRstmgrBase + RSTMGR_RESET_INFO_REG_OFFSET, reset_info);
    abs_mmio_write32(kUart0Base + UART_ALERT_TEST_REG_OFFSET, 0x1u);
    abs_mmio_write32(kRstmgrBase + RSTMGR_ALERT_INFO_CTRL_REG_OFFSET,
                     (15u << RSTMGR_ALERT_INFO_CTRL_INDEX_OFFSET) |
                         (1u << RSTMGR_ALERT_INFO_CTRL_EN_BIT));
    abs_mmio_write32(kRstmgrBase + RSTMGR_CPU_INFO_CTRL_REG_OFFSET,
                     (5u << RSTMGR_CPU_INFO_CTRL_INDEX_OFFSET) |
                         (1u << RSTMGR_CPU_INFO_CTRL_EN_BIT));
    abs_mmio_write32(kRstmgrBase + RSTMGR_ALERT_REGWEN_REG_OFFSET, 0u);
    abs_mmio_write32(kRstmgrBase + RSTMGR_CPU_REGWEN_REG_OFFSET, 0u);

    ret_sram->creator.reserved[0] = kRetSramMagicExpectedSwReset;
    ret_sram->creator.reserved[1] = failures;
    abs_mmio_write32(kRstmgrBase + RSTMGR_RESET_REQ_REG_OFFSET,
                     kMultiBitBool4True);
    wait_for_interrupt();
  }

  // ===========================================================================
  // Phase 2: Post-SW_RESET Verification
  // ===========================================================================
  failures = ret_sram->creator.reserved[1];
  ret_sram->creator.reserved[0] = 0u;
  ret_sram->creator.reserved[1] = 0u;

  if ((reset_info & (1u << RSTMGR_RESET_INFO_SW_RESET_BIT)) == 0u) {
    LOG_ERROR("RTL_MISMATCH: expected SW_RESET bit in RESET_INFO, got 0x%x",
              reset_info);
    failures++;
  }

  uint32_t post_reset_req =
      abs_mmio_read32(kRstmgrBase + RSTMGR_RESET_REQ_REG_OFFSET);
  if (post_reset_req != kMultiBitBool4False) {
    LOG_ERROR("RTL_MISMATCH: post-SW_RESET RESET_REQ expected 0x9, got 0x%x",
              post_reset_req);
    failures++;
  }

  // ALERT_REGWEN and CPU_REGWEN are clocked by clk_i / reset by rst_ni, so they
  // must reset back to 1 on SW_RESET.
  uint32_t alert_regwen =
      abs_mmio_read32(kRstmgrBase + RSTMGR_ALERT_REGWEN_REG_OFFSET);
  uint32_t cpu_regwen =
      abs_mmio_read32(kRstmgrBase + RSTMGR_CPU_REGWEN_REG_OFFSET);
  if (alert_regwen != 1u || cpu_regwen != 1u) {
    LOG_ERROR(
        "RTL_MISMATCH: post-SW_RESET REGWEN expected 1/1, got alert=%u cpu=%u",
        alert_regwen, cpu_regwen);
    failures++;
  }

  // ALERT_INFO_CTRL and CPU_INFO_CTRL are reset by rst_por_ni (NOT rst_ni).
  // On SW_RESET (rst_hw_req = 1), hardware clears EN (de=1, d=0), while INDEX
  // (de=0) is preserved across non-POR resets!
  uint32_t alert_ctrl =
      abs_mmio_read32(kRstmgrBase + RSTMGR_ALERT_INFO_CTRL_REG_OFFSET);
  uint32_t cpu_ctrl =
      abs_mmio_read32(kRstmgrBase + RSTMGR_CPU_INFO_CTRL_REG_OFFSET);
  if (alert_ctrl != (15u << RSTMGR_ALERT_INFO_CTRL_INDEX_OFFSET)) {
    LOG_ERROR(
        "RTL_MISMATCH: post-SW_RESET ALERT_INFO_CTRL expected 0xf0 (INDEX=15 "
        "preserved, EN=0 cleared), got 0x%x",
        alert_ctrl);
    failures++;
  }
  if (cpu_ctrl != (5u << RSTMGR_CPU_INFO_CTRL_INDEX_OFFSET)) {
    LOG_ERROR(
        "RTL_MISMATCH: post-SW_RESET CPU_INFO_CTRL expected 0x50 (INDEX=5 "
        "preserved, EN=0 cleared), got 0x%x",
        cpu_ctrl);
    failures++;
  }

  // Verify ALERT_INFO slot 6 vs out-of-bounds slot 15 (rstmgr_crash_info.sv).
  // Slot 6 contains alert_cause[0] (bit 211 -> word 6 bit 19 = 0x00080000).
  abs_mmio_write32(kRstmgrBase + RSTMGR_ALERT_INFO_CTRL_REG_OFFSET,
                   6u << RSTMGR_ALERT_INFO_CTRL_INDEX_OFFSET);
  uint32_t alert_slot6 =
      abs_mmio_read32(kRstmgrBase + RSTMGR_ALERT_INFO_REG_OFFSET);
  if ((alert_slot6 & (1u << 19)) == 0u) {
    LOG_ERROR(
        "RTL_MISMATCH: ALERT_INFO[6] expected bit 19 (alert_cause[0]) set, got "
        "0x%x",
        alert_slot6);
    failures++;
  }

  // Out-of-bounds slot 15 (INDEX=15 >= 9): in RTL (rstmgr_crash_info.sv:39-45),
  // slots[15:9] = '0, so reading ALERT_INFO with INDEX=15 returns 0x00000000
  // (whereas QEMU's `% 9u` wraps 15 -> 6 and returns alert_slot6!).
  abs_mmio_write32(kRstmgrBase + RSTMGR_ALERT_INFO_CTRL_REG_OFFSET,
                   15u << RSTMGR_ALERT_INFO_CTRL_INDEX_OFFSET);
  uint32_t alert_slot15 =
      abs_mmio_read32(kRstmgrBase + RSTMGR_ALERT_INFO_REG_OFFSET);
  if (alert_slot15 != 0u) {
    LOG_ERROR(
        "RTL_MISMATCH: ALERT_INFO[15] (out-of-bounds INDEX=15) expected 0x0, "
        "got 0x%x (wrapped to slot 6!)",
        alert_slot15);
    failures++;
  }

  // Wave 2 check: In rstmgr_crash_info.sv, SlotCntWidth =
  // $clog2(CrashStoreSlot). Unlike ALERT_INFO (CrashStoreSlot=9 ->
  // SlotCntWidth=$clog2(9)=4, so slots[15:9] are 0), CPU_INFO has
  // CrashStoreSlot=8 -> SlotCntWidth=$clog2(8)=3! Line 44: `assign slot_o =
  // slots[slot_sel_i[SlotCntWidth-1:0]];` Line 50: `assign unused_idx =
  // slot_sel_i[IdxWidth-1:SlotCntWidth];` Therefore, for CPU_INFO, bit 3 of
  // INDEX is tied off as unused, and INDEX=8..15 aliases slots[0..7] (INDEX &
  // 7)! Slot 4 is current_pc (always non-zero in flash), so INDEX=12 (4 | 8)
  // must equal INDEX=4.
  abs_mmio_write32(kRstmgrBase + RSTMGR_CPU_INFO_CTRL_REG_OFFSET,
                   4u << RSTMGR_CPU_INFO_CTRL_INDEX_OFFSET);
  uint32_t cpu_slot4 =
      abs_mmio_read32(kRstmgrBase + RSTMGR_CPU_INFO_REG_OFFSET);
  abs_mmio_write32(kRstmgrBase + RSTMGR_CPU_INFO_CTRL_REG_OFFSET,
                   12u << RSTMGR_CPU_INFO_CTRL_INDEX_OFFSET);
  uint32_t cpu_slot12 =
      abs_mmio_read32(kRstmgrBase + RSTMGR_CPU_INFO_REG_OFFSET);
  if (cpu_slot4 == 0u || cpu_slot12 != cpu_slot4) {
    LOG_ERROR(
        "RTL_MISMATCH: CPU_INFO[12] (3-bit SlotCntWidth=$clog2(8)=3) expected "
        "to alias CPU_INFO[4]=0x%x, got 0x%x",
        cpu_slot4, cpu_slot12);
    failures++;
  }

  // Wave 5 check: RSTMGR_PERMIT sub-word wr_err, sub-word read, and AW=7
  // addrmiss (0x70).
  // 1) Valid 1-byte write to byte 0 of ALERT_INFO_CTRL (PERMIT = 4'b0001) sets
  // INDEX=6 without fault, and 1-byte read at ALERT_INFO + 2 returns byte 2 of
  // slot 6 (0x08).
  g_expected_fault_count = 0;
  abs_mmio_write8(kRstmgrBase + RSTMGR_ALERT_INFO_CTRL_REG_OFFSET,
                  6u << RSTMGR_ALERT_INFO_CTRL_INDEX_OFFSET);
  uint8_t alert_slot6_b2 =
      abs_mmio_read8(kRstmgrBase + RSTMGR_ALERT_INFO_REG_OFFSET + 2u);
  if (g_expected_fault_count != 0u ||
      alert_slot6_b2 != (uint8_t)((alert_slot6 >> 16) & 0xffu)) {
    LOG_ERROR(
        "RTL_MISMATCH: valid byte-0 write / byte-2 read failed (faults=%u "
        "b2=0x%x expected=0x%x)",
        g_expected_fault_count, alert_slot6_b2,
        (uint8_t)((alert_slot6 >> 16) & 0xffu));
    failures++;
  }

  // 2) Unpermitted 1-byte write to byte 1 of ALERT_INFO_CTRL (offset 0x11,
  // reg_be = 4'b0010, PERMIT = 4'b0001) must raise Store Access Fault
  // (mcause = 7) and preserve ALERT_INFO_CTRL == 0x60.
  g_expected_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write8(kRstmgrBase + RSTMGR_ALERT_INFO_CTRL_REG_OFFSET + 1u, 0x00u);
  uint32_t ctrl_after_bad_sb =
      abs_mmio_read32(kRstmgrBase + RSTMGR_ALERT_INFO_CTRL_REG_OFFSET);
  if (g_expected_fault_count != 1u || g_last_mcause != 7u ||
      ctrl_after_bad_sb != (6u << RSTMGR_ALERT_INFO_CTRL_INDEX_OFFSET)) {
    LOG_ERROR(
        "RTL_MISMATCH: sub-word write to ALERT_INFO_CTRL+1 expected mcause=7 "
        "and preserved 0x60 (got faults=%u mcause=%u ctrl=0x%x)",
        g_expected_fault_count, g_last_mcause, ctrl_after_bad_sb);
    failures++;
  }

  // 3) Unpermitted 1-byte write to ALERT_INFO (offset 0x18, reg_be = 4'b0001,
  // PERMIT = 4'b1111) must raise Store Access Fault (mcause = 7).
  g_expected_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write8(kRstmgrBase + RSTMGR_ALERT_INFO_REG_OFFSET, 0x00u);
  if (g_expected_fault_count != 1u || g_last_mcause != 7u) {
    LOG_ERROR(
        "RTL_MISMATCH: 1-byte write to ALERT_INFO (PERMIT=4'b1111) expected "
        "mcause=7 (got faults=%u mcause=%u)",
        g_expected_fault_count, g_last_mcause);
    failures++;
  }

  // 4) Unmapped offset 0x70 within AW=7 (0x80) aperture must raise Load/Store
  // Access Fault (mcause = 5 / 7) with ERR_CODE remaining 0.
  g_expected_fault_count = 0;
  g_last_mcause = 0;
  (void)abs_mmio_read32(kRstmgrBase + 0x70u);
  if (g_expected_fault_count != 1u || g_last_mcause != 5u) {
    LOG_ERROR(
        "RTL_MISMATCH: read from addrmiss 0x70 expected mcause=5 (got "
        "faults=%u mcause=%u)",
        g_expected_fault_count, g_last_mcause);
    failures++;
  }
  g_expected_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write32(kRstmgrBase + 0x70u, 0u);
  if (g_expected_fault_count != 1u || g_last_mcause != 7u) {
    LOG_ERROR(
        "RTL_MISMATCH: write to addrmiss 0x70 expected mcause=7 (got "
        "faults=%u mcause=%u)",
        g_expected_fault_count, g_last_mcause);
    failures++;
  }
  uint32_t err_code = abs_mmio_read32(kRstmgrBase + RSTMGR_ERR_CODE_REG_OFFSET);
  if (err_code != 0u) {
    LOG_ERROR(
        "RTL_MISMATCH: ERR_CODE expected 0 after wr_err/addrmiss, got 0x%x",
        err_code);
    failures++;
  }

  CHECK(failures == 0u, "rstmgr_rtl_consistency_test failed with %u mismatches",
        failures);
  return true;
}
