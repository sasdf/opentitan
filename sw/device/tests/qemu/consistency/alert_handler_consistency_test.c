// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/dif/dif_alert_handler.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "alert_handler_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "uart_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kAlertBase = TOP_EARLGREY_ALERT_HANDLER_BASE_ADDR,
  kUart1Base = TOP_EARLGREY_UART1_BASE_ADDR,
  kUart1AlertId = kTopEarlgreyAlertIdUart1FatalFault,
  kLocAlertShadowUpdateErr = 5u,
};

#define EXPECT_RTL(cond, ...)                  \
  do {                                         \
    if (!(cond)) {                             \
      LOG_ERROR("RTL_MISMATCH: " __VA_ARGS__); \
      all_ok = false;                          \
    }                                          \
  } while (0)

static volatile uint32_t g_fault_count;
static volatile uint32_t g_last_mcause;

void ottf_exception_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = 0;
  uint32_t mepc = 0;
  asm volatile("csrr %0, mcause" : "=r"(mcause));
  asm volatile("csrr %0, mepc" : "=r"(mepc));
  g_fault_count++;
  g_last_mcause = mcause;
  uint16_t insn16 = *(const uint16_t *)mepc;
  mepc += ((insn16 & 0x3u) != 0x3u) ? 2u : 4u;
  asm volatile("csrw mepc, %0" : : "r"(mepc));
}

static void write_shadowed(uint32_t addr, uint32_t val) {
  abs_mmio_write32(addr, val);
  abs_mmio_write32(addr, val);
}

bool test_main(void) {
  bool all_ok = true;

  // 1. WO register INTR_TEST (0x08) must read back as 0, and INTR_ENABLE (0x04)
  // masks to 0xf.
  EXPECT_RTL(
      abs_mmio_read32(kAlertBase + ALERT_HANDLER_INTR_TEST_REG_OFFSET) == 0u,
      "INTR_TEST is WO and must read back as 0");
  uint32_t intr_en_orig =
      abs_mmio_read32(kAlertBase + ALERT_HANDLER_INTR_ENABLE_REG_OFFSET);
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_INTR_ENABLE_REG_OFFSET,
                   0xfffffff0u | intr_en_orig);
  EXPECT_RTL(
      abs_mmio_read32(kAlertBase + ALERT_HANDLER_INTR_ENABLE_REG_OFFSET) ==
          (intr_en_orig & 0xfu),
      "INTR_ENABLE upper bits [31:4] must read as 0");

  // 2. RO registers CLASSA_ACCUM_CNT, CLASSA_ESC_CNT, CLASSA_STATE ignore
  // software writes.
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_CLASSA_ACCUM_CNT_REG_OFFSET,
                   0x1234u);
  EXPECT_RTL(abs_mmio_read32(kAlertBase +
                             ALERT_HANDLER_CLASSA_ACCUM_CNT_REG_OFFSET) == 0u,
             "CLASSA_ACCUM_CNT is RO and must remain 0");
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_CLASSA_ESC_CNT_REG_OFFSET,
                   0x12345678u);
  EXPECT_RTL(abs_mmio_read32(kAlertBase +
                             ALERT_HANDLER_CLASSA_ESC_CNT_REG_OFFSET) == 0u,
             "CLASSA_ESC_CNT is RO and must remain 0");
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_CLASSA_STATE_REG_OFFSET, 0x7u);
  EXPECT_RTL(
      abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSA_STATE_REG_OFFSET) == 0u,
      "CLASSA_STATE is RO and must remain Idle (0)");

  // 3. Two-step shadow register write semantics &
  // LOCAL_ALERT_SHADOW_REG_UPDATE_ERROR (loc_alert 5).
  uint32_t loc_en5_addr = kAlertBase +
                          ALERT_HANDLER_LOC_ALERT_EN_SHADOWED_0_REG_OFFSET +
                          4u * kLocAlertShadowUpdateErr;
  uint32_t loc_class5_addr =
      kAlertBase + ALERT_HANDLER_LOC_ALERT_CLASS_SHADOWED_0_REG_OFFSET +
      4u * kLocAlertShadowUpdateErr;
  uint32_t loc_cause5_addr = kAlertBase +
                             ALERT_HANDLER_LOC_ALERT_CAUSE_0_REG_OFFSET +
                             4u * kLocAlertShadowUpdateErr;

  write_shadowed(loc_class5_addr, 1u);  // Class B
  write_shadowed(loc_en5_addr, 1u);     // Enable local alert 5

  // Class B: EN=1, LOCK=0, EN_E0..3=0 (0x0001) -> class_en must be false when
  // all EN_E0..3 are 0.
  write_shadowed(kAlertBase + ALERT_HANDLER_CLASSB_CTRL_SHADOWED_REG_OFFSET,
                 0x0001u);
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_INTR_STATE_REG_OFFSET,
                   1u << ALERT_HANDLER_INTR_STATE_CLASSB_BIT);

  // Trigger shadow update error on CLASSA_ACCUM_THRESH_SHADOWED (write 0x11
  // then 0x22).
  abs_mmio_write32(
      kAlertBase + ALERT_HANDLER_CLASSA_ACCUM_THRESH_SHADOWED_REG_OFFSET,
      0x0011u);
  abs_mmio_write32(
      kAlertBase + ALERT_HANDLER_CLASSA_ACCUM_THRESH_SHADOWED_REG_OFFSET,
      0x0022u);

  EXPECT_RTL(abs_mmio_read32(loc_cause5_addr) == 1u,
             "Mismatched shadow write must set LOC_ALERT_CAUSE[5] "
             "(shadow_reg_update_error)");
  EXPECT_RTL(
      abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSB_ACCUM_CNT_REG_OFFSET) ==
          0u,
      "CLASSB_ACCUM_CNT must not increment when EN=1 but EN_E0..3 are all 0");

  // Disable LOC_ALERT_EN_SHADOWED[5] BEFORE reading/clearing
  // LOC_ALERT_CAUSE[5]: In RTL, LOC_ALERT_CAUSE[5] remains readable as 1 even
  // after LOC_ALERT_EN_SHADOWED[5] is 0.
  write_shadowed(loc_en5_addr, 0u);
  EXPECT_RTL(abs_mmio_read32(loc_cause5_addr) == 1u,
             "LOC_ALERT_CAUSE[5] must still read back as 1 when "
             "LOC_ALERT_EN_SHADOWED[5] == 0");
  abs_mmio_write32(loc_cause5_addr, 1u);
  EXPECT_RTL(abs_mmio_read32(loc_cause5_addr) == 0u,
             "LOC_ALERT_CAUSE[5] must clear to 0 on W1C write");
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_INTR_STATE_REG_OFFSET,
                   1u << ALERT_HANDLER_INTR_STATE_CLASSB_BIT);

  // 4. Commit CLASSA_ACCUM_THRESH_SHADOWED = 0x0010 via two matching writes.
  write_shadowed(
      kAlertBase + ALERT_HANDLER_CLASSA_ACCUM_THRESH_SHADOWED_REG_OFFSET,
      0x0010u);
  EXPECT_RTL(
      abs_mmio_read32(kAlertBase +
                      ALERT_HANDLER_CLASSA_ACCUM_THRESH_SHADOWED_REG_OFFSET) ==
          0x0010u,
      "CLASSA_ACCUM_THRESH_SHADOWED must commit on two matching writes");

  // 5. Configure UART1 fatal_fault alert (Alert ID 1) into Class A with:
  //    - LOCK = 1 (bit 1) in CLASSA_CTRL_SHADOWED (0x393f)
  //    - TIMEOUT_CYC_SHADOWED = 100000
  //    - INTR_ENABLE.CLASSA = 0 initially
  uint32_t alert_en_addr = kAlertBase +
                           ALERT_HANDLER_ALERT_EN_SHADOWED_0_REG_OFFSET +
                           4u * kUart1AlertId;
  uint32_t alert_class_addr = kAlertBase +
                              ALERT_HANDLER_ALERT_CLASS_SHADOWED_0_REG_OFFSET +
                              4u * kUart1AlertId;
  uint32_t alert_cause_addr =
      kAlertBase + ALERT_HANDLER_ALERT_CAUSE_0_REG_OFFSET + 4u * kUart1AlertId;
  uint32_t alert_regwen_addr =
      kAlertBase + ALERT_HANDLER_ALERT_REGWEN_0_REG_OFFSET + 4u * kUart1AlertId;

  write_shadowed(alert_class_addr, 0u);  // Class A
  write_shadowed(alert_en_addr, 1u);     // Enabled
  write_shadowed(
      kAlertBase + ALERT_HANDLER_CLASSA_TIMEOUT_CYC_SHADOWED_REG_OFFSET,
      100000u);
  write_shadowed(kAlertBase + ALERT_HANDLER_CLASSA_CTRL_SHADOWED_REG_OFFSET,
                 0x393fu);  // EN=1, LOCK=1, EN_E0..3=1

  abs_mmio_write32(
      kAlertBase + ALERT_HANDLER_INTR_ENABLE_REG_OFFSET,
      intr_en_orig & ~(1u << ALERT_HANDLER_INTR_ENABLE_CLASSA_BIT));
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_INTR_STATE_REG_OFFSET,
                   1u << ALERT_HANDLER_INTR_STATE_CLASSA_BIT);

  // Trigger UART1 alert via UART1.ALERT_TEST.
  abs_mmio_write32(kUart1Base + UART_ALERT_TEST_REG_OFFSET, 1u);

  // Verify ALERT_CAUSE[1] == 1, CLASSA_ACCUM_CNT == 1, INTR_STATE.CLASSA == 1,
  // and CLASSA_STATE == Idle (0) because INTR_ENABLE.CLASSA == 0!
  EXPECT_RTL(abs_mmio_read32(alert_cause_addr) == 1u,
             "ALERT_CAUSE[%u] must be 1 after UART1 ALERT_TEST", kUart1AlertId);
  EXPECT_RTL(abs_mmio_read32(kAlertBase +
                             ALERT_HANDLER_CLASSA_ACCUM_CNT_REG_OFFSET) == 1u,
             "CLASSA_ACCUM_CNT must be 1 after single alert trigger");
  EXPECT_RTL(
      abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSA_STATE_REG_OFFSET) == 0u,
      "CLASSA_STATE must remain Idle (0) when INTR_ENABLE.CLASSA == 0");

  // Enabling INTR_ENABLE.CLASSA while INTR_STATE.CLASSA == 1 asserts irq[0] and
  // enters Timeout (1).
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_INTR_ENABLE_REG_OFFSET,
                   intr_en_orig | (1u << ALERT_HANDLER_INTR_ENABLE_CLASSA_BIT));
  EXPECT_RTL(
      abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSA_STATE_REG_OFFSET) == 1u,
      "CLASSA_STATE must enter Timeout (1) when INTR_ENABLE.CLASSA is set");

  // Disabling INTR_ENABLE.CLASSA deasserts irq[0] and returns CLASSA_STATE to
  // Idle (0).
  abs_mmio_write32(
      kAlertBase + ALERT_HANDLER_INTR_ENABLE_REG_OFFSET,
      intr_en_orig & ~(1u << ALERT_HANDLER_INTR_ENABLE_CLASSA_BIT));
  EXPECT_RTL(
      abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSA_STATE_REG_OFFSET) == 0u,
      "CLASSA_STATE must return to Idle (0) when INTR_ENABLE.CLASSA is "
      "cleared");

  // Even with CLASSA_CTRL_SHADOWED.LOCK == 1, before escalation (Phase0) has
  // triggered, CLASSA_CLR_REGWEN is still 1 so software can clear
  // CLASSA_ACCUM_CNT via CLASSA_CLR_SHADOWED.
  EXPECT_RTL(abs_mmio_read32(kAlertBase +
                             ALERT_HANDLER_CLASSA_CLR_REGWEN_REG_OFFSET) == 1u,
             "CLASSA_CLR_REGWEN must remain 1 before escalation triggers");
  write_shadowed(kAlertBase + ALERT_HANDLER_CLASSA_CLR_SHADOWED_REG_OFFSET, 1u);
  EXPECT_RTL(abs_mmio_read32(kAlertBase +
                             ALERT_HANDLER_CLASSA_ACCUM_CNT_REG_OFFSET) == 0u,
             "CLASSA_ACCUM_CNT must clear via CLASSA_CLR_SHADOWED before "
             "escalation triggers");

  // Clear ALERT_CAUSE[1] and INTR_STATE.CLASSA, restore UART1 alert to Class D
  // (3), and verify ALERT_REGWEN[1] locks ALERT_CLASS_SHADOWED[1].
  abs_mmio_write32(alert_cause_addr, 1u);
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_INTR_STATE_REG_OFFSET,
                   1u << ALERT_HANDLER_INTR_STATE_CLASSA_BIT);
  write_shadowed(alert_class_addr, 3u);
  abs_mmio_write32(alert_regwen_addr, 0u);
  abs_mmio_write32(alert_regwen_addr, 1u);
  EXPECT_RTL(abs_mmio_read32(alert_regwen_addr) == 0u,
             "ALERT_REGWEN[%u] is rw0c and must remain 0 once cleared",
             kUart1AlertId);
  write_shadowed(alert_class_addr, 0u);
  EXPECT_RTL(
      abs_mmio_read32(alert_class_addr) == 3u,
      "ALERT_CLASS_SHADOWED[%u] must ignore writes when ALERT_REGWEN[%u] == 0",
      kUart1AlertId, kUart1AlertId);

  // 6. Verify REGWEN protection on shadowed registers (CLASSC_REGWEN &
  // PING_TIMER_REGWEN).
  write_shadowed(
      kAlertBase + ALERT_HANDLER_CLASSC_ACCUM_THRESH_SHADOWED_REG_OFFSET,
      0x0055u);
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_CLASSC_REGWEN_REG_OFFSET, 0u);
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_CLASSC_REGWEN_REG_OFFSET, 1u);
  EXPECT_RTL(abs_mmio_read32(kAlertBase +
                             ALERT_HANDLER_CLASSC_REGWEN_REG_OFFSET) == 0u,
             "CLASSC_REGWEN is rw0c and must remain 0 once cleared");
  write_shadowed(
      kAlertBase + ALERT_HANDLER_CLASSC_ACCUM_THRESH_SHADOWED_REG_OFFSET,
      0x00aau);
  EXPECT_RTL(
      abs_mmio_read32(kAlertBase +
                      ALERT_HANDLER_CLASSC_ACCUM_THRESH_SHADOWED_REG_OFFSET) ==
          0x0055u,
      "CLASSC_ACCUM_THRESH_SHADOWED must ignore writes when CLASSC_REGWEN == "
      "0");

  uint32_t ping_timeout_before = abs_mmio_read32(
      kAlertBase + ALERT_HANDLER_PING_TIMEOUT_CYC_SHADOWED_REG_OFFSET);
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_PING_TIMER_REGWEN_REG_OFFSET, 0u);
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_PING_TIMER_REGWEN_REG_OFFSET, 1u);
  EXPECT_RTL(abs_mmio_read32(kAlertBase +
                             ALERT_HANDLER_PING_TIMER_REGWEN_REG_OFFSET) == 0u,
             "PING_TIMER_REGWEN is rw0c and must remain 0 once cleared");
  write_shadowed(
      kAlertBase + ALERT_HANDLER_PING_TIMEOUT_CYC_SHADOWED_REG_OFFSET,
      ping_timeout_before ^ 0x00ffu);
  EXPECT_RTL(
      abs_mmio_read32(kAlertBase +
                      ALERT_HANDLER_PING_TIMEOUT_CYC_SHADOWED_REG_OFFSET) ==
          ping_timeout_before,
      "PING_TIMEOUT_CYC_SHADOWED must ignore writes when PING_TIMER_REGWEN == "
      "0");

  // 7. Wave 2 Deep-Pass Checks:
  //    (a) CLASSA_CLR_SHADOWED is SwAccessRW (.de=0) and retains committed 1 on
  //    readback.
  EXPECT_RTL(
      abs_mmio_read32(kAlertBase +
                      ALERT_HANDLER_CLASSA_CLR_SHADOWED_REG_OFFSET) == 1u,
      "CLASSA_CLR_SHADOWED is SwAccessRW and must read back as 1 after "
      "committing 1");
  write_shadowed(kAlertBase + ALERT_HANDLER_CLASSA_CLR_SHADOWED_REG_OFFSET, 0u);
  EXPECT_RTL(
      abs_mmio_read32(kAlertBase +
                      ALERT_HANDLER_CLASSA_CLR_SHADOWED_REG_OFFSET) == 0u,
      "CLASSA_CLR_SHADOWED must read back as 0 after committing 0");

  //    (b) Enabling CLASSA_CTRL_SHADOWED or setting CLASSA_TIMEOUT_CYC_SHADOWED
  //    > 0 while
  //        irq[0] (INTR_STATE.CLASSA & INTR_ENABLE.CLASSA) is active
  //        transitions Idle -> Timeout (1).
  write_shadowed(
      kAlertBase + ALERT_HANDLER_CLASSA_PHASE0_CYC_SHADOWED_REG_OFFSET,
      100000u);
  write_shadowed(kAlertBase + ALERT_HANDLER_CLASSA_CTRL_SHADOWED_REG_OFFSET,
                 0x3910u);  // EN=0, LOCK=0, EN_E2=1 (Phase2), EN_E0..1,3=0
  write_shadowed(
      kAlertBase + ALERT_HANDLER_CLASSA_TIMEOUT_CYC_SHADOWED_REG_OFFSET, 0u);
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_INTR_TEST_REG_OFFSET,
                   1u << ALERT_HANDLER_INTR_TEST_CLASSA_BIT);
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_INTR_ENABLE_REG_OFFSET,
                   intr_en_orig | (1u << ALERT_HANDLER_INTR_ENABLE_CLASSA_BIT));
  EXPECT_RTL(
      abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSA_STATE_REG_OFFSET) == 0u,
      "CLASSA_STATE must be Idle (0) when EN=0 and TIMEOUT_CYC=0");

  // Enable CLASSA_CTRL_SHADOWED (EN=1) while TIMEOUT_CYC == 0 -> still Idle
  // (0).
  write_shadowed(kAlertBase + ALERT_HANDLER_CLASSA_CTRL_SHADOWED_REG_OFFSET,
                 0x3911u);  // EN=1, LOCK=0, EN_E2=1 (Phase2), EN_E0..1,3=0
  EXPECT_RTL(
      abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSA_STATE_REG_OFFSET) == 0u,
      "CLASSA_STATE must remain Idle (0) when TIMEOUT_CYC == 0");

  // Set CLASSA_TIMEOUT_CYC_SHADOWED = 100000 while irq[0] == 1 and EN == 1 ->
  // enters Timeout (1)!
  write_shadowed(
      kAlertBase + ALERT_HANDLER_CLASSA_TIMEOUT_CYC_SHADOWED_REG_OFFSET,
      100000u);
  EXPECT_RTL(
      abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSA_STATE_REG_OFFSET) == 1u,
      "CLASSA_STATE must enter Timeout (1) when TIMEOUT_CYC_SHADOWED > 0 is "
      "committed while irq active");

  //    (c) Writing CLASSA_CLR_SHADOWED = 1 while in Timeout (1) does NOT exit
  //    Timeout (1)
  //        as long as irq[0] (timeout_en_i) remains 1 (CheckClr_A in
  //        alert_handler_esc_timer.sv).
  write_shadowed(kAlertBase + ALERT_HANDLER_CLASSA_CLR_SHADOWED_REG_OFFSET, 1u);
  EXPECT_RTL(
      abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSA_STATE_REG_OFFSET) == 1u,
      "CLASSA_STATE must remain in Timeout (1) after CLASSA_CLR_SHADOWED write "
      "while irq[0] is active");
  write_shadowed(kAlertBase + ALERT_HANDLER_CLASSA_CLR_SHADOWED_REG_OFFSET, 0u);

  //    (d) Lowering CLASSA_ACCUM_THRESH_SHADOWED to 0 when ACCUM_CNT == 1
  //    without a new alert trigger
  //        (class_trig_i == 0) must NOT trigger Phase0 on INTR_ENABLE toggle
  //        (accu_trig_o is gated by trig_gated).
  abs_mmio_write32(
      kAlertBase + ALERT_HANDLER_INTR_ENABLE_REG_OFFSET,
      intr_en_orig & ~(1u << ALERT_HANDLER_INTR_ENABLE_CLASSA_BIT));
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_INTR_STATE_REG_OFFSET,
                   1u << ALERT_HANDLER_INTR_STATE_CLASSA_BIT);
  write_shadowed(
      kAlertBase + ALERT_HANDLER_CLASSA_ACCUM_THRESH_SHADOWED_REG_OFFSET, 10u);
  // Trigger local alert 5 into Class A once so CLASSA_ACCUM_CNT becomes 1.
  write_shadowed(loc_class5_addr, 0u);  // Class A
  write_shadowed(loc_en5_addr, 1u);
  abs_mmio_write32(
      kAlertBase + ALERT_HANDLER_CLASSA_ACCUM_THRESH_SHADOWED_REG_OFFSET, 10u);
  abs_mmio_write32(
      kAlertBase + ALERT_HANDLER_CLASSA_ACCUM_THRESH_SHADOWED_REG_OFFSET, 11u);
  write_shadowed(loc_en5_addr, 0u);
  abs_mmio_write32(loc_cause5_addr, 1u);
  EXPECT_RTL(abs_mmio_read32(kAlertBase +
                             ALERT_HANDLER_CLASSA_ACCUM_CNT_REG_OFFSET) == 1u,
             "CLASSA_ACCUM_CNT must be 1 after single local alert");
  // Now lower CLASSA_ACCUM_THRESH_SHADOWED to 0 (so ACCUM_CNT=1 > THRESH=0)
  // WITHOUT firing any new alert:
  write_shadowed(
      kAlertBase + ALERT_HANDLER_CLASSA_ACCUM_THRESH_SHADOWED_REG_OFFSET, 0u);
  // Enable INTR_ENABLE.CLASSA while INTR_STATE.CLASSA == 1 -> must enter
  // Timeout (1), NOT Phase0 (4)!
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_INTR_ENABLE_REG_OFFSET,
                   intr_en_orig | (1u << ALERT_HANDLER_INTR_ENABLE_CLASSA_BIT));
  EXPECT_RTL(
      abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSA_STATE_REG_OFFSET) == 1u,
      "CLASSA_STATE must enter Timeout (1), NOT Phase0 (4), when ACCUM_CNT > "
      "THRESH without new class_trig_i");

  // Clean up Class A state.
  abs_mmio_write32(
      kAlertBase + ALERT_HANDLER_INTR_ENABLE_REG_OFFSET,
      intr_en_orig & ~(1u << ALERT_HANDLER_INTR_ENABLE_CLASSA_BIT));
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_INTR_STATE_REG_OFFSET,
                   1u << ALERT_HANDLER_INTR_STATE_CLASSA_BIT);
  write_shadowed(kAlertBase + ALERT_HANDLER_CLASSA_CLR_SHADOWED_REG_OFFSET, 1u);
  write_shadowed(kAlertBase + ALERT_HANDLER_CLASSA_CLR_SHADOWED_REG_OFFSET, 0u);

  // 8. Wave 5 Check: ALERT_HANDLER_PERMIT sub-word write wr_err and addrmiss
  // (0x578)
  write_shadowed(
      kAlertBase + ALERT_HANDLER_CLASSA_TIMEOUT_CYC_SHADOWED_REG_OFFSET,
      0x12345678u);
  g_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write8(
      kAlertBase + ALERT_HANDLER_CLASSA_TIMEOUT_CYC_SHADOWED_REG_OFFSET, 0xaau);
  EXPECT_RTL(g_fault_count == 1u && g_last_mcause == 7u,
             "1-byte write to CLASSA_TIMEOUT_CYC_SHADOWED (PERMIT=4'b1111) "
             "must fault (count=%u, mcause=%u)",
             g_fault_count, g_last_mcause);

  g_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write8(
      kAlertBase + ALERT_HANDLER_CLASSA_ACCUM_THRESH_SHADOWED_REG_OFFSET,
      0x55u);
  EXPECT_RTL(g_fault_count == 1u && g_last_mcause == 7u,
             "1-byte write to CLASSA_ACCUM_THRESH_SHADOWED (PERMIT=4'b0011) "
             "must fault (count=%u, mcause=%u)",
             g_fault_count, g_last_mcause);

  g_fault_count = 0;
  g_last_mcause = 0;
  *(volatile uint16_t
        *)(kAlertBase + ALERT_HANDLER_CLASSA_ACCUM_THRESH_SHADOWED_REG_OFFSET) =
      0x0033u;
  *(volatile uint16_t
        *)(kAlertBase + ALERT_HANDLER_CLASSA_ACCUM_THRESH_SHADOWED_REG_OFFSET) =
      0x0033u;
  EXPECT_RTL(
      g_fault_count == 0u &&
          abs_mmio_read32(
              kAlertBase +
              ALERT_HANDLER_CLASSA_ACCUM_THRESH_SHADOWED_REG_OFFSET) == 0x0033u,
      "2-byte write to CLASSA_ACCUM_THRESH_SHADOWED (PERMIT=4'b0011) must "
      "succeed");

  g_fault_count = 0;
  g_last_mcause = 0;
  (void)abs_mmio_read32(kAlertBase + 0x578u);
  EXPECT_RTL(g_fault_count == 1u && g_last_mcause == 5u,
             "Out-of-bounds read at 0x578 (addrmiss) must fault with mcause=5 "
             "(count=%u, mcause=%u)",
             g_fault_count, g_last_mcause);

  g_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write32(kAlertBase + 0x578u, 0xdeadbeefu);
  EXPECT_RTL(g_fault_count == 1u && g_last_mcause == 7u,
             "Out-of-bounds write at 0x578 (addrmiss) must fault with mcause=7 "
             "(count=%u, mcause=%u)",
             g_fault_count, g_last_mcause);

  return all_ok;
}
