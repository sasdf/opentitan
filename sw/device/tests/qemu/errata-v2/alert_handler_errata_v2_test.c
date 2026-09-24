// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top/alert_handler_regs.h"
#include "hw/top/uart_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

#define CHECK_EQ(a, b, ...) CHECK((a) == (b), __VA_ARGS__)

enum {
  kAlertBase = TOP_EARLGREY_ALERT_HANDLER_BASE_ADDR,
  kUart1Base = TOP_EARLGREY_UART1_BASE_ADDR,
  kUart1AlertId = kTopEarlgreyAlertIdUart1FatalFault,
  kLocAlertShadowUpdateErr = 5u,
  kRiscvLoadAccessFault = 5,
  kRiscvStoreAccessFault = 7,
  kUnmappedOffsetV2 = ALERT_HANDLER_CLASSD_STATE_REG_OFFSET + 4u,
};

static volatile uint32_t g_fault_count = 0;
static volatile uint32_t g_last_mcause = 0;

void ottf_exception_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = 0;
  uint32_t mepc = 0;
  CSR_READ(CSR_REG_MCAUSE, &mcause);
  CSR_READ(CSR_REG_MEPC, &mepc);
  if (mcause == kRiscvLoadAccessFault || mcause == kRiscvStoreAccessFault) {
    g_fault_count++;
    g_last_mcause = mcause;
    uint16_t insn16 = *(const volatile uint16_t *)mepc;
    uint32_t step = ((insn16 & 0x3u) != 0x3u) ? 2u : 4u;
    CSR_WRITE(CSR_REG_MEPC, mepc + step);
    return;
  }
  ottf_generic_fault_print(exc_info, "Unhandled exception", mcause);
  abort();
}

static void write_shadowed(uint32_t addr, uint32_t val) {
  abs_mmio_write32(addr, val);
  abs_mmio_write32(addr, val);
}

static void test_alert_handler_class_en_gated_by_esc_enables(void) {
  LOG_INFO(
      "Verifying [alert_handler_reg_wrap.sv:193-213]: CLASSx_CTRL_SHADOWED.EN "
      "gated by |EN_E0..E3");

  uint32_t loc_en5_addr = kAlertBase +
                          ALERT_HANDLER_LOC_ALERT_EN_SHADOWED_0_REG_OFFSET +
                          4u * kLocAlertShadowUpdateErr;
  uint32_t loc_class5_addr =
      kAlertBase + ALERT_HANDLER_LOC_ALERT_CLASS_SHADOWED_0_REG_OFFSET +
      4u * kLocAlertShadowUpdateErr;
  uint32_t loc_cause5_addr = kAlertBase +
                             ALERT_HANDLER_LOC_ALERT_CAUSE_0_REG_OFFSET +
                             4u * kLocAlertShadowUpdateErr;

  // Map local alert 5 (shadow_reg_update_error) to Class B (1) and enable it.
  write_shadowed(loc_class5_addr, 1u);
  write_shadowed(loc_en5_addr, 1u);

  // Configure Class B with EN = 1 (bit 0) but all EN_E0..EN_E3 = 0 (0x0001).
  write_shadowed(kAlertBase + ALERT_HANDLER_CLASSB_CTRL_SHADOWED_REG_OFFSET,
                 0x0001u);
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_INTR_STATE_REG_OFFSET,
                   1u << ALERT_HANDLER_INTR_STATE_CLASSB_BIT);

  // Trigger a shadow register update error on CLASSA_ACCUM_THRESH_SHADOWED.
  abs_mmio_write32(
      kAlertBase + ALERT_HANDLER_CLASSA_ACCUM_THRESH_SHADOWED_REG_OFFSET,
      0x0011u);
  abs_mmio_write32(
      kAlertBase + ALERT_HANDLER_CLASSA_ACCUM_THRESH_SHADOWED_REG_OFFSET,
      0x0022u);

  // Verify LOC_ALERT_CAUSE[5] latched 1 AND INTR_STATE.CLASSB latched 1,
  // while CLASSB_ACCUM_CNT remained 0 because `class_en = en & (|en_e0..3)` ==
  // 0!
  CHECK_EQ(
      abs_mmio_read32(loc_cause5_addr), 1u,
      "[alert_handler_reg_wrap.sv:193-213] Expected LOC_ALERT_CAUSE[5] == 1");
  CHECK_EQ(
      (abs_mmio_read32(kAlertBase + ALERT_HANDLER_INTR_STATE_REG_OFFSET) >>
       ALERT_HANDLER_INTR_STATE_CLASSB_BIT) &
          1u,
      1u,
      "[alert_handler_reg_wrap.sv:73] Expected INTR_STATE.CLASSB == 1 even "
      "when class_en == 0");
  CHECK_EQ(
      abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSB_ACCUM_CNT_REG_OFFSET),
      0u,
      "[alert_handler_reg_wrap.sv:193-213] Expected CLASSB_ACCUM_CNT == 0 when "
      "EN=1 but EN_E0..3 == 0");

  // Also verify: disabling LOC_ALERT_EN_SHADOWED[5] does NOT mask
  // LOC_ALERT_CAUSE[5] on readback.
  write_shadowed(loc_en5_addr, 0u);
  CHECK_EQ(
      abs_mmio_read32(loc_cause5_addr), 1u,
      "Expected LOC_ALERT_CAUSE[5] to still read 1 when LOC_ALERT_EN == 0");
  abs_mmio_write32(loc_cause5_addr, 1u);
  CHECK_EQ(abs_mmio_read32(loc_cause5_addr), 0u,
           "Expected LOC_ALERT_CAUSE[5] to clear on W1C");
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_INTR_STATE_REG_OFFSET,
                   1u << ALERT_HANDLER_INTR_STATE_CLASSB_BIT);
}

static void test_alert_handler_timeout_irq_and_clr_shadowed(void) {
  LOG_INFO(
      "Verifying [alert_handler.sv:256] & "
      "[alert_handler_reg_top.sv:12925-12974]: TimeoutSt gated by irq[k] & "
      "CLASSA_CLR_SHADOWED");

  uint32_t intr_en_orig =
      abs_mmio_read32(kAlertBase + ALERT_HANDLER_INTR_ENABLE_REG_OFFSET);

  uint32_t alert_en_addr = kAlertBase +
                           ALERT_HANDLER_ALERT_EN_SHADOWED_0_REG_OFFSET +
                           4u * kUart1AlertId;
  uint32_t alert_class_addr = kAlertBase +
                              ALERT_HANDLER_ALERT_CLASS_SHADOWED_0_REG_OFFSET +
                              4u * kUart1AlertId;
  uint32_t alert_cause_addr =
      kAlertBase + ALERT_HANDLER_ALERT_CAUSE_0_REG_OFFSET + 4u * kUart1AlertId;

  // Configure UART1 fatal_fault alert into Class A with ACCUM_THRESH = 16,
  // TIMEOUT_CYC = 100000, EN=1, LOCK=1, EN_E0..3=1 (0x393f), and
  // INTR_ENABLE.CLASSA = 0.
  write_shadowed(
      kAlertBase + ALERT_HANDLER_CLASSA_ACCUM_THRESH_SHADOWED_REG_OFFSET,
      0x0010u);
  write_shadowed(
      kAlertBase + ALERT_HANDLER_CLASSA_TIMEOUT_CYC_SHADOWED_REG_OFFSET,
      100000u);
  write_shadowed(kAlertBase + ALERT_HANDLER_CLASSA_CTRL_SHADOWED_REG_OFFSET,
                 0x393fu);
  write_shadowed(alert_class_addr, 0u);  // Class A
  write_shadowed(alert_en_addr, 1u);

  abs_mmio_write32(
      kAlertBase + ALERT_HANDLER_INTR_ENABLE_REG_OFFSET,
      intr_en_orig & ~(1u << ALERT_HANDLER_INTR_ENABLE_CLASSA_BIT));
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_INTR_STATE_REG_OFFSET,
                   1u << ALERT_HANDLER_INTR_STATE_CLASSA_BIT);

  // Trigger UART1.ALERT_TEST = 1.
  abs_mmio_write32(kUart1Base + UART_ALERT_TEST_REG_OFFSET, 1u);

  // 1. Even though INTR_STATE.CLASSA == 1 and CLASSA_ACCUM_CNT == 1,
  // CLASSA_STATE remains Idle (0) because INTR_ENABLE.CLASSA == 0 (`irq[0] ==
  // 0`).
  CHECK_EQ(abs_mmio_read32(alert_cause_addr), 1u, "Expected ALERT_CAUSE == 1");
  CHECK_EQ(
      abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSA_ACCUM_CNT_REG_OFFSET),
      1u, "Expected CLASSA_ACCUM_CNT == 1");
  CHECK_EQ(abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSA_STATE_REG_OFFSET),
           0u,
           "[alert_handler.sv:256] Expected CLASSA_STATE == Idle (0) when "
           "INTR_ENABLE.CLASSA == 0");

  // 2. Setting INTR_ENABLE.CLASSA = 1 asserts `irq[0] = 1` and starts
  // `TimeoutSt` (`CLASSA_STATE == 1`).
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_INTR_ENABLE_REG_OFFSET,
                   intr_en_orig | (1u << ALERT_HANDLER_INTR_ENABLE_CLASSA_BIT));
  CHECK_EQ(abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSA_STATE_REG_OFFSET),
           1u,
           "[alert_handler.sv:256] Expected CLASSA_STATE == Timeout (1) "
           "when INTR_ENABLE.CLASSA == 1");

  // 3. Committing 1 to CLASSA_CLR_SHADOWED while in `TimeoutSt` clears
  // CLASSA_ACCUM_CNT to 0 AND retains `qs == 1` on readback (`SwAccessRW`),
  // but does NOT exit `TimeoutSt` (`CLASSA_STATE` stays `1` while `irq[0] ==
  // 1`).
  write_shadowed(kAlertBase + ALERT_HANDLER_CLASSA_CLR_SHADOWED_REG_OFFSET, 1u);
  CHECK_EQ(abs_mmio_read32(kAlertBase +
                           ALERT_HANDLER_CLASSA_CLR_SHADOWED_REG_OFFSET),
           1u,
           "[alert_handler_reg_top.sv:12925-12974] Expected "
           "CLASSA_CLR_SHADOWED to read back as 1 after committing 1");
  CHECK_EQ(
      abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSA_ACCUM_CNT_REG_OFFSET),
      0u,
      "[alert_handler_reg_top.sv:12925-12974] Expected CLASSA_ACCUM_CNT "
      "cleared to 0 by CLASSA_CLR_SHADOWED");
  CHECK_EQ(abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSA_STATE_REG_OFFSET),
           1u,
           "[alert_handler_reg_top.sv:12925-12974] Expected CLASSA_STATE to "
           "remain Timeout (1) after CLASSA_CLR_SHADOWED while irq[0] == 1");

  // 4. Clearing INTR_ENABLE.CLASSA to 0 deasserts `irq[0]` and aborts
  // `TimeoutSt` back to `IdleSt` (`0`) even while `INTR_STATE.CLASSA` is still
  // `1`.
  abs_mmio_write32(
      kAlertBase + ALERT_HANDLER_INTR_ENABLE_REG_OFFSET,
      intr_en_orig & ~(1u << ALERT_HANDLER_INTR_ENABLE_CLASSA_BIT));
  CHECK_EQ(abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSA_STATE_REG_OFFSET),
           0u,
           "[alert_handler.sv:256] Expected CLASSA_STATE to return to Idle "
           "(0) when INTR_ENABLE.CLASSA cleared");

  // Clean up.
  write_shadowed(kAlertBase + ALERT_HANDLER_CLASSA_CLR_SHADOWED_REG_OFFSET, 0u);
  abs_mmio_write32(alert_cause_addr, 1u);
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_INTR_STATE_REG_OFFSET,
                   1u << ALERT_HANDLER_INTR_STATE_CLASSA_BIT);
}

static void test_alert_handler_permit_subword_and_addrmiss(void) {
  LOG_INFO(
      "Verifying [alert_handler_reg_top.sv:16096-16454]: ALERT_HANDLER_PERMIT "
      "sub-word wr_err & >= 0x588 addrmiss (NAlerts=66, NumRegs=354)");

  // 1. 1-byte write to CLASSA_TIMEOUT_CYC_SHADOWED (PERMIT = 4'b1111) traps
  // with Store Access Fault (mcause = 7).
  g_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write8(
      kAlertBase + ALERT_HANDLER_CLASSA_TIMEOUT_CYC_SHADOWED_REG_OFFSET, 0xAAu);
  CHECK_EQ(g_fault_count, 1u,
           "[alert_handler_reg_top.sv:16096-16454] Expected sb to "
           "CLASSA_TIMEOUT_CYC_SHADOWED to trap");
  CHECK_EQ(g_last_mcause, (uint32_t)kRiscvStoreAccessFault,
           "Expected mcause=7");

  // 2. 1-byte write to CLASSA_ACCUM_THRESH_SHADOWED (PERMIT = 4'b0011) traps
  // with Store Access Fault (mcause = 7), whereas 2-byte halfword write
  // succeeds.
  g_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write8(
      kAlertBase + ALERT_HANDLER_CLASSA_ACCUM_THRESH_SHADOWED_REG_OFFSET,
      0x55u);
  CHECK_EQ(g_fault_count, 1u,
           "[alert_handler_reg_top.sv:16096-16454] Expected sb to "
           "CLASSA_ACCUM_THRESH_SHADOWED to trap");
  CHECK_EQ(g_last_mcause, (uint32_t)kRiscvStoreAccessFault,
           "Expected mcause=7");

  g_fault_count = 0;
  *(volatile uint16_t
        *)(uintptr_t)(kAlertBase +
                      ALERT_HANDLER_CLASSA_ACCUM_THRESH_SHADOWED_REG_OFFSET) =
      0x0033u;
  *(volatile uint16_t
        *)(uintptr_t)(kAlertBase +
                      ALERT_HANDLER_CLASSA_ACCUM_THRESH_SHADOWED_REG_OFFSET) =
      0x0033u;
  CHECK_EQ(
      g_fault_count, 0u,
      "2-byte sh write to CLASSA_ACCUM_THRESH_SHADOWED (4'b0011) must succeed");
  CHECK_EQ(
      abs_mmio_read32(kAlertBase +
                      ALERT_HANDLER_CLASSA_ACCUM_THRESH_SHADOWED_REG_OFFSET),
      0x0033u, "Expected committed CLASSA_ACCUM_THRESH_SHADOWED == 0x0033");

  // 3. On trunk-v2 (NAlerts=66, NumRegs=354), CLASSD_STATE is at 0x584
  // (mapped, no fault), while 0x588 (kUnmappedOffsetV2) traps with Load/Store
  // Access Fault.
  g_fault_count = 0;
  (void)abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSD_STATE_REG_OFFSET);
  CHECK_EQ(g_fault_count, 0u,
           "Expected read at mapped CLASSD_STATE (0x584) to succeed");

  g_fault_count = 0;
  g_last_mcause = 0;
  (void)abs_mmio_read32(kAlertBase + kUnmappedOffsetV2);
  CHECK_EQ(g_fault_count, 1u,
           "[alert_handler_reg_top.sv:16096-16454] Expected read at unmapped "
           "offset 0x588 to trap");
  CHECK_EQ(g_last_mcause, (uint32_t)kRiscvLoadAccessFault, "Expected mcause=5");

  g_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write32(kAlertBase + kUnmappedOffsetV2, 0xDEADBEEFu);
  CHECK_EQ(g_fault_count, 1u,
           "[alert_handler_reg_top.sv:16096-16454] Expected write at unmapped "
           "offset 0x588 to trap");
  CHECK_EQ(g_last_mcause, (uint32_t)kRiscvStoreAccessFault,
           "Expected mcause=7");
}

bool test_main(void) {
  LOG_INFO("Starting alert_handler v2 CW340 FPGA errata confirmation test");

  test_alert_handler_class_en_gated_by_esc_enables();
  test_alert_handler_timeout_irq_and_clr_shadowed();
  test_alert_handler_permit_subword_and_addrmiss();

  LOG_INFO("All alert_handler v2 errata checks PASSED on CW340 FPGA!");
  return true;
}
