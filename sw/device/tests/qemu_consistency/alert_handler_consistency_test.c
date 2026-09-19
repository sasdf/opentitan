// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/dif/dif_alert_handler.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "alert_handler_regs.h"
#include "uart_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kAlertBase = TOP_EARLGREY_ALERT_HANDLER_BASE_ADDR,
  kUart1Base = TOP_EARLGREY_UART1_BASE_ADDR,
  kUart1AlertId = kTopEarlgreyAlertIdUart1FatalFault,
};

#define EXPECT_RTL(cond, ...)                   \
  do {                                          \
    if (!(cond)) {                              \
      LOG_ERROR("RTL_MISMATCH: " __VA_ARGS__);  \
      all_ok = false;                           \
    }                                           \
  } while (0)

static void write_shadowed(uint32_t addr, uint32_t val) {
  abs_mmio_write32(addr, val);
  abs_mmio_write32(addr, val);
}

bool test_main(void) {
  bool all_ok = true;

  // 1. WO register INTR_TEST (0x08) must read back as 0.
  EXPECT_RTL(abs_mmio_read32(kAlertBase + ALERT_HANDLER_INTR_TEST_REG_OFFSET) == 0u,
             "INTR_TEST is WO and must read back as 0");

  // 2. RO registers CLASSA_ACCUM_CNT, CLASSA_ESC_CNT, CLASSA_STATE ignore software writes.
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_CLASSA_ACCUM_CNT_REG_OFFSET, 0x1234u);
  EXPECT_RTL(abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSA_ACCUM_CNT_REG_OFFSET) == 0u,
             "CLASSA_ACCUM_CNT is RO and must remain 0");
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_CLASSA_ESC_CNT_REG_OFFSET, 0x12345678u);
  EXPECT_RTL(abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSA_ESC_CNT_REG_OFFSET) == 0u,
             "CLASSA_ESC_CNT is RO and must remain 0");
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_CLASSA_STATE_REG_OFFSET, 0x7u);
  EXPECT_RTL(abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSA_STATE_REG_OFFSET) == 0u,
             "CLASSA_STATE is RO and must remain Idle (0)");

  // 3. Two-step shadow register write semantics on CLASSA_ACCUM_THRESH_SHADOWED.
  // Single write followed by read must not commit and must reset shadow phase.
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_CLASSA_ACCUM_THRESH_SHADOWED_REG_OFFSET,
                   0x0010u);
  EXPECT_RTL(abs_mmio_read32(kAlertBase +
                             ALERT_HANDLER_CLASSA_ACCUM_THRESH_SHADOWED_REG_OFFSET) == 0u,
             "CLASSA_ACCUM_THRESH_SHADOWED must not update on single write");
  // Two consecutive matching writes commit value.
  write_shadowed(kAlertBase + ALERT_HANDLER_CLASSA_ACCUM_THRESH_SHADOWED_REG_OFFSET,
                 0x0010u);
  EXPECT_RTL(abs_mmio_read32(kAlertBase +
                             ALERT_HANDLER_CLASSA_ACCUM_THRESH_SHADOWED_REG_OFFSET) ==
                 0x0010u,
             "CLASSA_ACCUM_THRESH_SHADOWED must commit on two matching writes");

  // 4. Configure UART1 fatal_fault alert (Alert ID 1) into Class A with threshold = 10,
  // timeout = 0, and trigger via UART1.ALERT_TEST.
  uint32_t alert_en_addr =
      kAlertBase + ALERT_HANDLER_ALERT_EN_SHADOWED_0_REG_OFFSET + 4u * kUart1AlertId;
  uint32_t alert_class_addr =
      kAlertBase + ALERT_HANDLER_ALERT_CLASS_SHADOWED_0_REG_OFFSET + 4u * kUart1AlertId;
  uint32_t alert_cause_addr =
      kAlertBase + ALERT_HANDLER_ALERT_CAUSE_0_REG_OFFSET + 4u * kUart1AlertId;
  uint32_t alert_regwen_addr =
      kAlertBase + ALERT_HANDLER_ALERT_REGWEN_0_REG_OFFSET + 4u * kUart1AlertId;

  write_shadowed(alert_class_addr, 0u);  // Class A
  write_shadowed(alert_en_addr, 1u);     // Enabled
  write_shadowed(kAlertBase + ALERT_HANDLER_CLASSA_TIMEOUT_CYC_SHADOWED_REG_OFFSET, 0u);
  write_shadowed(kAlertBase + ALERT_HANDLER_CLASSA_CTRL_SHADOWED_REG_OFFSET,
                 0x393du);  // EN=1, LOCK=0, EN_E0..3=1

  // Clear any prior Class A interrupt state.
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_INTR_STATE_REG_OFFSET,
                   1u << ALERT_HANDLER_INTR_STATE_CLASSA_BIT);

  // Trigger UART1 alert via UART1.ALERT_TEST.
  abs_mmio_write32(kUart1Base + UART_ALERT_TEST_REG_OFFSET, 1u);

  // Verify ALERT_CAUSE[1] == 1, CLASSA_ACCUM_CNT == 1, INTR_STATE.CLASSA == 1.
  EXPECT_RTL(abs_mmio_read32(alert_cause_addr) == 1u,
             "ALERT_CAUSE[%u] must be 1 after UART1 ALERT_TEST", kUart1AlertId);
  EXPECT_RTL(abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSA_ACCUM_CNT_REG_OFFSET) == 1u,
             "CLASSA_ACCUM_CNT must be 1 after single alert trigger");
  EXPECT_RTL((abs_mmio_read32(kAlertBase + ALERT_HANDLER_INTR_STATE_REG_OFFSET) &
              (1u << ALERT_HANDLER_INTR_STATE_CLASSA_BIT)) != 0u,
             "INTR_STATE.CLASSA must be set when Class A alert triggers");

  // Clear ALERT_CAUSE[1] (W1C), INTR_STATE.CLASSA (W1C), and CLASSA_ACCUM_CNT via CLASSA_CLR_SHADOWED.
  abs_mmio_write32(alert_cause_addr, 1u);
  EXPECT_RTL(abs_mmio_read32(alert_cause_addr) == 0u,
             "ALERT_CAUSE[%u] must clear to 0 on W1C write", kUart1AlertId);
  abs_mmio_write32(kAlertBase + ALERT_HANDLER_INTR_STATE_REG_OFFSET,
                   1u << ALERT_HANDLER_INTR_STATE_CLASSA_BIT);
  write_shadowed(kAlertBase + ALERT_HANDLER_CLASSA_CLR_SHADOWED_REG_OFFSET, 1u);
  EXPECT_RTL(abs_mmio_read32(kAlertBase + ALERT_HANDLER_CLASSA_ACCUM_CNT_REG_OFFSET) == 0u,
             "CLASSA_ACCUM_CNT must clear to 0 after writing CLASSA_CLR_SHADOWED");

  // Restore UART1 alert back to Class D (3) for OTTF and lock ALERT_REGWEN[1] (rw0c) test.
  write_shadowed(alert_class_addr, 3u);
  abs_mmio_write32(alert_regwen_addr, 0u);
  abs_mmio_write32(alert_regwen_addr, 1u);
  EXPECT_RTL(abs_mmio_read32(alert_regwen_addr) == 0u,
             "ALERT_REGWEN[%u] is rw0c and must remain 0 once cleared",
             kUart1AlertId);

  return all_ok;
}
