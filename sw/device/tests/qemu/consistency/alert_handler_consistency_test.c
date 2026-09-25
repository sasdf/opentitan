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

OTTF_DEFINE_TEST_CONFIG(.ignore_alerts = true);

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
  CHECK_EQ(
      abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSB_STATE_REG_OFFSET), 0u,
      "[alert_handler_reg_wrap.sv:193-213] Expected CLASSB_STATE == 0 when "
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
  CHECK_EQ((abs_mmio_read32(kAlertBase + ALERT_HANDLER_INTR_STATE_REG_OFFSET) >>
            ALERT_HANDLER_INTR_STATE_CLASSA_BIT) &
               1u,
           1u,
           "[alert_handler.sv:256] Expected INTR_STATE.CLASSA == 1 at Step 1");
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
  CHECK_EQ((abs_mmio_read32(kAlertBase + ALERT_HANDLER_INTR_STATE_REG_OFFSET) >>
            ALERT_HANDLER_INTR_STATE_CLASSA_BIT) &
               1u,
           1u,
           "[alert_handler.sv:256] Expected INTR_STATE.CLASSA to remain 1 when "
           "INTR_ENABLE.CLASSA cleared");
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

static void test_alert_handler_reset_values_and_intr_test(void) {
  LOG_INFO(
      "Verifying [alert_handler_reg_pkg.sv:10-172, 1320-1796]: All 354 v2 "
      "register reset defaults and INTR_TEST (WO) / INTR_STATE (W1C)");

  // Scalar interrupt and ping timer registers (0x000..0x014).
  CHECK_EQ(abs_mmio_read32(kAlertBase + ALERT_HANDLER_INTR_STATE_REG_OFFSET),
           0x0u, "INTR_STATE reset default");
  CHECK_EQ(abs_mmio_read32(kAlertBase + ALERT_HANDLER_INTR_ENABLE_REG_OFFSET),
           0x0u, "INTR_ENABLE reset default");
  CHECK_EQ(abs_mmio_read32(kAlertBase + ALERT_HANDLER_INTR_TEST_REG_OFFSET),
           0x0u, "INTR_TEST WO readback 0");
  CHECK_EQ(
      abs_mmio_read32(kAlertBase + ALERT_HANDLER_PING_TIMER_REGWEN_REG_OFFSET),
      0x1u, "PING_TIMER_REGWEN reset default");
  CHECK_EQ(abs_mmio_read32(kAlertBase +
                           ALERT_HANDLER_PING_TIMEOUT_CYC_SHADOWED_REG_OFFSET),
           0x100u, "PING_TIMEOUT_CYC_SHADOWED reset default (256)");
  CHECK_EQ(abs_mmio_read32(kAlertBase +
                           ALERT_HANDLER_PING_TIMER_EN_SHADOWED_REG_OFFSET),
           0x0u, "PING_TIMER_EN_SHADOWED reset default");

  // 66 peripheral alert multiregs (NAlerts = 66 on Earlgrey v2).
  // Verify ALERT_REGWEN_i == 1 and ALERT_CAUSE_i == 0, and verify 2-phase
  // shadow write/readback on all 66 ALERT_EN_SHADOWED_i and
  // ALERT_CLASS_SHADOWED_i registers.
  for (uint32_t i = 0; i < 66u; ++i) {
    uint32_t regwen_addr =
        kAlertBase + ALERT_HANDLER_ALERT_REGWEN_0_REG_OFFSET + 4u * i;
    uint32_t en_addr =
        kAlertBase + ALERT_HANDLER_ALERT_EN_SHADOWED_0_REG_OFFSET + 4u * i;
    uint32_t cls_addr =
        kAlertBase + ALERT_HANDLER_ALERT_CLASS_SHADOWED_0_REG_OFFSET + 4u * i;
    uint32_t cause_addr =
        kAlertBase + ALERT_HANDLER_ALERT_CAUSE_0_REG_OFFSET + 4u * i;

    CHECK_EQ(abs_mmio_read32(regwen_addr), 0x1u,
             "ALERT_REGWEN_%u reset default", i);
    write_shadowed(en_addr, 0x0u);
    CHECK_EQ(abs_mmio_read32(en_addr), 0x0u, "ALERT_EN_SHADOWED_%u == 0", i);
    write_shadowed(cls_addr, i & 0x3u);
    CHECK_EQ(abs_mmio_read32(cls_addr), i & 0x3u,
             "ALERT_CLASS_SHADOWED_%u == %u", i, i & 0x3u);
    write_shadowed(cls_addr, 0x0u);
    CHECK_EQ(abs_mmio_read32(cause_addr), 0x0u, "ALERT_CAUSE_%u reset default",
             i);
  }

  // 7 local alert multiregs (N_LOC_ALERT = 7).
  for (uint32_t i = 0; i < 7u; ++i) {
    uint32_t regwen_addr =
        kAlertBase + ALERT_HANDLER_LOC_ALERT_REGWEN_0_REG_OFFSET + 4u * i;
    uint32_t en_addr =
        kAlertBase + ALERT_HANDLER_LOC_ALERT_EN_SHADOWED_0_REG_OFFSET + 4u * i;
    uint32_t cls_addr = kAlertBase +
                        ALERT_HANDLER_LOC_ALERT_CLASS_SHADOWED_0_REG_OFFSET +
                        4u * i;
    uint32_t cause_addr =
        kAlertBase + ALERT_HANDLER_LOC_ALERT_CAUSE_0_REG_OFFSET + 4u * i;

    CHECK_EQ(abs_mmio_read32(regwen_addr), 0x1u,
             "LOC_ALERT_REGWEN_%u reset default", i);
    write_shadowed(en_addr, 0x0u);
    CHECK_EQ(abs_mmio_read32(en_addr), 0x0u, "LOC_ALERT_EN_SHADOWED_%u == 0",
             i);
    write_shadowed(cls_addr, i & 0x3u);
    CHECK_EQ(abs_mmio_read32(cls_addr), i & 0x3u,
             "LOC_ALERT_CLASS_SHADOWED_%u == %u", i, i & 0x3u);
    write_shadowed(cls_addr, 0x0u);
    CHECK_EQ(abs_mmio_read32(cause_addr), 0x0u,
             "LOC_ALERT_CAUSE_%u reset default", i);
  }

  // 4 escalation classes CLASS{A..D} (4 x 14 = 56 registers, stride 0x38).
  for (uint32_t c = 0; c < 4u; ++c) {
    uint32_t base = kAlertBase + ALERT_HANDLER_CLASSA_REGWEN_REG_OFFSET +
                    c * (ALERT_HANDLER_CLASSB_REGWEN_REG_OFFSET -
                         ALERT_HANDLER_CLASSA_REGWEN_REG_OFFSET);
    CHECK_EQ(abs_mmio_read32(base + 0x00u), 0x1u, "CLASS%u_REGWEN reset", c);
    write_shadowed(base + 0x04u, 0x393cu);
    CHECK_EQ(abs_mmio_read32(base + 0x04u), 0x393cu,
             "CLASS%u_CTRL_SHADOWED (0x393c)", c);
    CHECK_EQ(abs_mmio_read32(base + 0x08u), 0x1u, "CLASS%u_CLR_REGWEN reset",
             c);
    CHECK_EQ(abs_mmio_read32(base + 0x0cu), 0x0u, "CLASS%u_CLR_SHADOWED reset",
             c);
    CHECK_EQ(abs_mmio_read32(base + 0x10u), 0x0u, "CLASS%u_ACCUM_CNT reset", c);
    write_shadowed(base + 0x14u, 0x0u);
    CHECK_EQ(abs_mmio_read32(base + 0x14u), 0x0u,
             "CLASS%u_ACCUM_THRESH_SHADOWED", c);
    write_shadowed(base + 0x18u, 0x0u);
    CHECK_EQ(abs_mmio_read32(base + 0x18u), 0x0u,
             "CLASS%u_TIMEOUT_CYC_SHADOWED", c);
    CHECK_EQ(abs_mmio_read32(base + 0x1cu), 0x0u,
             "CLASS%u_CRASHDUMP_TRIGGER_SHADOWED reset", c);
    write_shadowed(base + 0x20u, 0x0u);
    CHECK_EQ(abs_mmio_read32(base + 0x20u), 0x0u, "CLASS%u_PHASE0_CYC_SHADOWED",
             c);
    write_shadowed(base + 0x24u, 0x0u);
    CHECK_EQ(abs_mmio_read32(base + 0x24u), 0x0u, "CLASS%u_PHASE1_CYC_SHADOWED",
             c);
    write_shadowed(base + 0x28u, 0x0u);
    CHECK_EQ(abs_mmio_read32(base + 0x28u), 0x0u, "CLASS%u_PHASE2_CYC_SHADOWED",
             c);
    write_shadowed(base + 0x2cu, 0x0u);
    CHECK_EQ(abs_mmio_read32(base + 0x2cu), 0x0u, "CLASS%u_PHASE3_CYC_SHADOWED",
             c);
    CHECK_EQ(abs_mmio_read32(base + 0x30u), 0x0u, "CLASS%u_ESC_CNT reset", c);
    CHECK_EQ(abs_mmio_read32(base + 0x34u), 0x0u, "CLASS%u_STATE reset", c);
  }

  // Verify INTR_TEST (WO) sets INTR_STATE bits [3:0] and INTR_STATE (W1C)
  // clears them.
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_INTR_TEST_REG_OFFSET, 0x0fu);
  CHECK_EQ(abs_mmio_read32(kAlertBase + ALERT_HANDLER_INTR_TEST_REG_OFFSET),
           0x0u, "INTR_TEST must read back 0");
  CHECK_EQ(abs_mmio_read32(kAlertBase + ALERT_HANDLER_INTR_STATE_REG_OFFSET),
           0x0fu, "INTR_STATE must latch 0xf from INTR_TEST");
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_INTR_STATE_REG_OFFSET, 0x0fu);
  CHECK_EQ(abs_mmio_read32(kAlertBase + ALERT_HANDLER_INTR_STATE_REG_OFFSET),
           0x0u, "INTR_STATE must clear to 0 on W1C");
}

static void test_alert_handler_phase_transitions_autolock_and_regwen(void) {
  LOG_INFO(
      "Verifying [alert_handler_esc_timer.sv:144-235] & "
      "[alert_handler_reg_wrap.sv:136-147, 271-276]: Phase0..2 transitions, "
      "unlocked CLASSC_CLR_SHADOWED, LOCK=1 autolock, and RW0C REGWEN locks");

  uint32_t loc_en5_addr = kAlertBase +
                          ALERT_HANDLER_LOC_ALERT_EN_SHADOWED_0_REG_OFFSET +
                          4u * kLocAlertShadowUpdateErr;
  uint32_t loc_class5_addr =
      kAlertBase + ALERT_HANDLER_LOC_ALERT_CLASS_SHADOWED_0_REG_OFFSET +
      4u * kLocAlertShadowUpdateErr;
  uint32_t loc_cause5_addr = kAlertBase +
                             ALERT_HANDLER_LOC_ALERT_CAUSE_0_REG_OFFSET +
                             4u * kLocAlertShadowUpdateErr;

  // Map local alert 5 (shadow_reg_update_error) to Class C (2) and enable it.
  write_shadowed(loc_class5_addr, 2u);
  write_shadowed(loc_en5_addr, 1u);

  // Configure Class C with ACCUM_THRESH = 0, PHASE0_CYC = 10, PHASE1_CYC = 10,
  // PHASE2_CYC = 0xFFFFFFFF (holds in Phase2St = 6), CTRL = 0x39c5 (EN=1,
  // LOCK=0, EN_E0=1, MAP_E0=3 so no escalation output wire asserts in Phases
  // 0..2).
  write_shadowed(
      kAlertBase + ALERT_HANDLER_CLASSC_ACCUM_THRESH_SHADOWED_REG_OFFSET, 0u);
  write_shadowed(
      kAlertBase + ALERT_HANDLER_CLASSC_PHASE0_CYC_SHADOWED_REG_OFFSET, 10u);
  write_shadowed(
      kAlertBase + ALERT_HANDLER_CLASSC_PHASE1_CYC_SHADOWED_REG_OFFSET, 10u);
  write_shadowed(
      kAlertBase + ALERT_HANDLER_CLASSC_PHASE2_CYC_SHADOWED_REG_OFFSET,
      0xFFFFFFFFu);
  write_shadowed(kAlertBase + ALERT_HANDLER_CLASSC_CTRL_SHADOWED_REG_OFFSET,
                 0x39c5u);

  // Trigger local alert 5 via shadow mismatch on CLASSD_ACCUM_THRESH_SHADOWED.
  abs_mmio_write32(
      kAlertBase + ALERT_HANDLER_CLASSD_ACCUM_THRESH_SHADOWED_REG_OFFSET,
      0x1111u);
  abs_mmio_write32(
      kAlertBase + ALERT_HANDLER_CLASSD_ACCUM_THRESH_SHADOWED_REG_OFFSET,
      0x2222u);

  // Wait for Class C to advance through Phase0St (4) -> Phase1St (5) ->
  // Phase2St (6).
  uint32_t st = 0;
  for (uint32_t spin = 0; spin < 200000u; ++spin) {
    st = abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSC_STATE_REG_OFFSET);
    if (st == 6u) {
      break;
    }
  }
  CHECK_EQ(st, 6u,
           "[alert_handler_esc_timer.sv:186-235] Expected CLASSC_STATE to "
           "reach Phase2St (6)");
  CHECK_EQ(
      abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSC_ACCUM_CNT_REG_OFFSET),
      1u, "Expected CLASSC_ACCUM_CNT == 1");
  CHECK_EQ(
      abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSC_CLR_REGWEN_REG_OFFSET),
      1u, "Expected CLASSC_CLR_REGWEN == 1 when LOCK == 0");

  // Because LOCK == 0, committing 1 to CLASSC_CLR_SHADOWED clears
  // CLASSC_ACCUM_CNT to 0 and returns CLASSC_STATE from Phase2St (6) to
  // IdleSt (0).
  write_shadowed(kAlertBase + ALERT_HANDLER_CLASSC_CLR_SHADOWED_REG_OFFSET, 1u);
  CHECK_EQ(
      abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSC_ACCUM_CNT_REG_OFFSET),
      0u, "Expected CLASSC_ACCUM_CNT cleared to 0 by CLASSC_CLR_SHADOWED");
  CHECK_EQ(abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSC_STATE_REG_OFFSET),
           0u,
           "[alert_handler_esc_timer.sv:220-235] Expected CLASSC_STATE to "
           "return to IdleSt (0) on CLASSC_CLR_SHADOWED");
  write_shadowed(kAlertBase + ALERT_HANDLER_CLASSC_CLR_SHADOWED_REG_OFFSET, 0u);
  abs_mmio_write32(loc_cause5_addr, 1u);
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_INTR_STATE_REG_OFFSET,
                   1u << ALERT_HANDLER_INTR_STATE_CLASSC_BIT);

  // Now configure Class C with PHASE0_CYC = 0xFFFFFFFF and LOCK = 1 (0x39c7).
  write_shadowed(
      kAlertBase + ALERT_HANDLER_CLASSC_PHASE0_CYC_SHADOWED_REG_OFFSET,
      0xFFFFFFFFu);
  write_shadowed(kAlertBase + ALERT_HANDLER_CLASSC_CTRL_SHADOWED_REG_OFFSET,
                 0x39c7u);

  // Trigger local alert 5 again -> Class C enters Phase0St (4) and hardware
  // automatically clears CLASSC_CLR_REGWEN to 0!
  abs_mmio_write32(
      kAlertBase + ALERT_HANDLER_CLASSD_ACCUM_THRESH_SHADOWED_REG_OFFSET,
      0x3333u);
  abs_mmio_write32(
      kAlertBase + ALERT_HANDLER_CLASSD_ACCUM_THRESH_SHADOWED_REG_OFFSET,
      0x4444u);

  CHECK_EQ(abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSC_STATE_REG_OFFSET),
           4u,
           "[alert_handler_esc_timer.sv:186-202] Expected CLASSC_STATE == "
           "Phase0St (4)");
  CHECK_EQ(
      abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSC_CLR_REGWEN_REG_OFFSET),
      0u,
      "[alert_handler_reg_wrap.sv:136-147] Expected CLASSC_CLR_REGWEN "
      "autolocked to 0 upon entering Phase0St with LOCK=1");

  // Attempting to commit 1 to CLASSC_CLR_SHADOWED while CLASSC_CLR_REGWEN == 0
  // is ignored; CLASSC_CLR_SHADOWED remains 0 and CLASSC_STATE stays 4.
  write_shadowed(kAlertBase + ALERT_HANDLER_CLASSC_CLR_SHADOWED_REG_OFFSET, 1u);
  CHECK_EQ(abs_mmio_read32(kAlertBase +
                           ALERT_HANDLER_CLASSC_CLR_SHADOWED_REG_OFFSET),
           0u, "Expected CLASSC_CLR_SHADOWED write ignored when locked");
  CHECK_EQ(abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSC_STATE_REG_OFFSET),
           4u, "Expected CLASSC_STATE to remain Phase0St (4) when locked");

  // Disable local alert 5 and clear causes.
  write_shadowed(loc_en5_addr, 0u);
  abs_mmio_write32(loc_cause5_addr, 1u);
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_INTR_STATE_REG_OFFSET,
                   1u << ALERT_HANDLER_INTR_STATE_CLASSC_BIT);

  // Verify RW0C lock behavior on PING_TIMER_REGWEN, ALERT_REGWEN_65 (v2 alert
  // 65), LOC_ALERT_REGWEN_6, and CLASSD_REGWEN.
  write_shadowed(
      kAlertBase + ALERT_HANDLER_PING_TIMEOUT_CYC_SHADOWED_REG_OFFSET, 0x0200u);
  CHECK_EQ(abs_mmio_read32(kAlertBase +
                           ALERT_HANDLER_PING_TIMEOUT_CYC_SHADOWED_REG_OFFSET),
           0x0200u, "Expected PING_TIMEOUT_CYC_SHADOWED == 0x200");
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_PING_TIMER_REGWEN_REG_OFFSET, 1u);
  CHECK_EQ(
      abs_mmio_read32(kAlertBase + ALERT_HANDLER_PING_TIMER_REGWEN_REG_OFFSET),
      1u, "RW0C write 1 keeps PING_TIMER_REGWEN == 1");
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_PING_TIMER_REGWEN_REG_OFFSET, 0u);
  CHECK_EQ(
      abs_mmio_read32(kAlertBase + ALERT_HANDLER_PING_TIMER_REGWEN_REG_OFFSET),
      0u, "RW0C write 0 clears PING_TIMER_REGWEN to 0");
  write_shadowed(
      kAlertBase + ALERT_HANDLER_PING_TIMEOUT_CYC_SHADOWED_REG_OFFSET, 0x0300u);
  CHECK_EQ(abs_mmio_read32(kAlertBase +
                           ALERT_HANDLER_PING_TIMEOUT_CYC_SHADOWED_REG_OFFSET),
           0x0200u,
           "Expected PING_TIMEOUT_CYC_SHADOWED locked by PING_TIMER_REGWEN==0");

  uint32_t alert_regwen65 =
      kAlertBase + ALERT_HANDLER_ALERT_REGWEN_0_REG_OFFSET + 4u * 65u;
  uint32_t alert_en65 =
      kAlertBase + ALERT_HANDLER_ALERT_EN_SHADOWED_0_REG_OFFSET + 4u * 65u;
  uint32_t alert_class65 =
      kAlertBase + ALERT_HANDLER_ALERT_CLASS_SHADOWED_0_REG_OFFSET + 4u * 65u;
  write_shadowed(alert_en65, 1u);
  write_shadowed(alert_class65, 3u);
  abs_mmio_write32(alert_regwen65, 0u);
  CHECK_EQ(abs_mmio_read32(alert_regwen65), 0u, "ALERT_REGWEN_65 RW0C -> 0");
  write_shadowed(alert_en65, 0u);
  write_shadowed(alert_class65, 0u);
  CHECK_EQ(abs_mmio_read32(alert_en65), 1u,
           "ALERT_EN_SHADOWED_65 locked by ALERT_REGWEN_65==0");
  CHECK_EQ(abs_mmio_read32(alert_class65), 3u,
           "ALERT_CLASS_SHADOWED_65 locked by ALERT_REGWEN_65==0");

  uint32_t loc_regwen6 =
      kAlertBase + ALERT_HANDLER_LOC_ALERT_REGWEN_0_REG_OFFSET + 4u * 6u;
  uint32_t loc_en6 =
      kAlertBase + ALERT_HANDLER_LOC_ALERT_EN_SHADOWED_0_REG_OFFSET + 4u * 6u;
  uint32_t loc_class6 = kAlertBase +
                        ALERT_HANDLER_LOC_ALERT_CLASS_SHADOWED_0_REG_OFFSET +
                        4u * 6u;
  write_shadowed(loc_en6, 1u);
  write_shadowed(loc_class6, 3u);
  abs_mmio_write32(loc_regwen6, 0u);
  CHECK_EQ(abs_mmio_read32(loc_regwen6), 0u, "LOC_ALERT_REGWEN_6 RW0C -> 0");
  write_shadowed(loc_en6, 0u);
  write_shadowed(loc_class6, 0u);
  CHECK_EQ(abs_mmio_read32(loc_en6), 1u,
           "LOC_ALERT_EN_SHADOWED_6 locked by LOC_ALERT_REGWEN_6==0");
  CHECK_EQ(abs_mmio_read32(loc_class6), 3u,
           "LOC_ALERT_CLASS_SHADOWED_6 locked by LOC_ALERT_REGWEN_6==0");

  write_shadowed(
      kAlertBase + ALERT_HANDLER_CLASSD_CRASHDUMP_TRIGGER_SHADOWED_REG_OFFSET,
      2u);
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_CLASSD_REGWEN_REG_OFFSET, 0u);
  CHECK_EQ(abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSD_REGWEN_REG_OFFSET),
           0u, "CLASSD_REGWEN RW0C -> 0");
  write_shadowed(
      kAlertBase + ALERT_HANDLER_CLASSD_CRASHDUMP_TRIGGER_SHADOWED_REG_OFFSET,
      1u);
  CHECK_EQ(abs_mmio_read32(
               kAlertBase +
               ALERT_HANDLER_CLASSD_CRASHDUMP_TRIGGER_SHADOWED_REG_OFFSET),
           2u, "CLASSD_CRASHDUMP_TRIGGER_SHADOWED locked by CLASSD_REGWEN==0");
}

static void test_alert_handler_permit_subword_and_addrmiss(void) {
  LOG_INFO(
      "Verifying [alert_handler_reg_pkg.sv:1441-1796]: ALERT_HANDLER_PERMIT "
      "3 tiers (4'b0001, 4'b0011, 4'b1111) & >= 0x588 addrmiss (NAlerts=66)");

  // Tier 1 (PERMIT = 4'b0001): 1-byte sb at offset +0 succeeds, while 1-byte
  // sb at offset +1 traps with Store Access Fault (mcause = 7).
  g_fault_count = 0;
  abs_mmio_write8(kAlertBase + ALERT_HANDLER_INTR_ENABLE_REG_OFFSET, 0x00u);
  abs_mmio_write8(
      kAlertBase + ALERT_HANDLER_CLASSA_CRASHDUMP_TRIGGER_SHADOWED_REG_OFFSET,
      0x01u);
  abs_mmio_write8(
      kAlertBase + ALERT_HANDLER_CLASSA_CRASHDUMP_TRIGGER_SHADOWED_REG_OFFSET,
      0x01u);
  CHECK_EQ(g_fault_count, 0u,
           "1-byte sb at offset +0 on 4'b0001 registers must succeed");
  CHECK_EQ(abs_mmio_read32(
               kAlertBase +
               ALERT_HANDLER_CLASSA_CRASHDUMP_TRIGGER_SHADOWED_REG_OFFSET),
           0x01u, "Expected CLASSA_CRASHDUMP_TRIGGER_SHADOWED == 1");

  g_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write8(kAlertBase + ALERT_HANDLER_INTR_ENABLE_REG_OFFSET + 1u,
                  0xAAu);
  CHECK_EQ(g_fault_count, 1u,
           "1-byte sb at offset +1 on 4'b0001 register must trap");
  CHECK_EQ(g_last_mcause, (uint32_t)kRiscvStoreAccessFault,
           "Expected mcause=7");

  // Tier 2 (PERMIT = 4'b0011): 1-byte sb at offset +0 traps with Store Access
  // Fault (mcause = 7), whereas 2-byte sh at offset +0 succeeds.
  const uint32_t kTier2Offsets[] = {
      ALERT_HANDLER_PING_TIMEOUT_CYC_SHADOWED_REG_OFFSET,
      ALERT_HANDLER_CLASSA_CTRL_SHADOWED_REG_OFFSET,
      ALERT_HANDLER_CLASSA_ACCUM_CNT_REG_OFFSET,
      ALERT_HANDLER_CLASSA_ACCUM_THRESH_SHADOWED_REG_OFFSET,
  };
  for (size_t i = 0; i < sizeof(kTier2Offsets) / sizeof(kTier2Offsets[0]);
       ++i) {
    g_fault_count = 0;
    g_last_mcause = 0;
    abs_mmio_write8(kAlertBase + kTier2Offsets[i], 0x55u);
    CHECK_EQ(g_fault_count, 1u, "Expected sb to 4'b0011 register 0x%x to trap",
             kTier2Offsets[i]);
    CHECK_EQ(g_last_mcause, (uint32_t)kRiscvStoreAccessFault,
             "Expected mcause=7");
  }

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

  // Tier 3 (PERMIT = 4'b1111): Both 1-byte sb and 2-byte sh trap with Store
  // Access Fault (mcause = 7), while 4-byte sw succeeds.
  const uint32_t kTier3Offsets[] = {
      ALERT_HANDLER_CLASSA_TIMEOUT_CYC_SHADOWED_REG_OFFSET,
      ALERT_HANDLER_CLASSA_PHASE0_CYC_SHADOWED_REG_OFFSET,
      ALERT_HANDLER_CLASSA_PHASE1_CYC_SHADOWED_REG_OFFSET,
      ALERT_HANDLER_CLASSA_PHASE2_CYC_SHADOWED_REG_OFFSET,
      ALERT_HANDLER_CLASSA_PHASE3_CYC_SHADOWED_REG_OFFSET,
      ALERT_HANDLER_CLASSA_ESC_CNT_REG_OFFSET,
  };
  for (size_t i = 0; i < sizeof(kTier3Offsets) / sizeof(kTier3Offsets[0]);
       ++i) {
    g_fault_count = 0;
    g_last_mcause = 0;
    abs_mmio_write8(kAlertBase + kTier3Offsets[i], 0xAAu);
    CHECK_EQ(g_fault_count, 1u, "Expected sb to 4'b1111 register 0x%x to trap",
             kTier3Offsets[i]);
    CHECK_EQ(g_last_mcause, (uint32_t)kRiscvStoreAccessFault,
             "Expected mcause=7");

    g_fault_count = 0;
    g_last_mcause = 0;
    *(volatile uint16_t *)(uintptr_t)(kAlertBase + kTier3Offsets[i]) = 0x1234u;
    CHECK_EQ(g_fault_count, 1u, "Expected sh to 4'b1111 register 0x%x to trap",
             kTier3Offsets[i]);
    CHECK_EQ(g_last_mcause, (uint32_t)kRiscvStoreAccessFault,
             "Expected mcause=7");
  }

  // On trunk-v2 (NAlerts=66, NumRegs=354), CLASSD_STATE is at 0x584
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
           "[alert_handler_reg_top.sv:16081-16439] Expected read at unmapped "
           "offset 0x588 to trap");
  CHECK_EQ(g_last_mcause, (uint32_t)kRiscvLoadAccessFault, "Expected mcause=5");

  g_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write32(kAlertBase + kUnmappedOffsetV2, 0xDEADBEEFu);
  CHECK_EQ(g_fault_count, 1u,
           "[alert_handler_reg_top.sv:16081-16439] Expected write at unmapped "
           "offset 0x588 to trap");
  CHECK_EQ(g_last_mcause, (uint32_t)kRiscvStoreAccessFault,
           "Expected mcause=7");
}

bool test_main(void) {
  LOG_INFO("Starting alert_handler v2 RTL alignment consistency test");

  test_alert_handler_reset_values_and_intr_test();
  test_alert_handler_class_en_gated_by_esc_enables();
  test_alert_handler_timeout_irq_and_clr_shadowed();
  test_alert_handler_phase_transitions_autolock_and_regwen();
  test_alert_handler_permit_subword_and_addrmiss();

  LOG_INFO("All alert_handler v2 consistency checks OK!");
  return true;
}
