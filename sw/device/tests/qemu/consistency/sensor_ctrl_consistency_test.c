// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sensor_ctrl_regs.h"

OTTF_DEFINE_TEST_CONFIG(.ignore_alerts = true);

static volatile bool g_fault_seen = false;
static volatile uint32_t g_fault_mcause = 0u;

void ottf_exception_handler(uint32_t *exc_info) {
  (void)exc_info;
  g_fault_seen = true;
  g_fault_mcause = ibex_mcause_read();
}

enum {
  kSensorCtrlBase = TOP_EARLGREY_SENSOR_CTRL_AON_BASE_ADDR,
  kManualPadAttrMask =
      (1u << SENSOR_CTRL_MANUAL_PAD_ATTR_0_PULL_EN_0_BIT) |
      (1u << SENSOR_CTRL_MANUAL_PAD_ATTR_0_PULL_SELECT_0_BIT) |
      (1u << SENSOR_CTRL_MANUAL_PAD_ATTR_0_INPUT_DISABLE_0_BIT),
};

bool test_main(void) {
  LOG_INFO("1. Checking SENSOR_CTRL reset defaults and STATUS...");
  uint32_t status =
      abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_STATUS_REG_OFFSET);
  CHECK(bitfield_bit32_read(status, SENSOR_CTRL_STATUS_AST_INIT_DONE_BIT));
  CHECK(abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_CFG_REGWEN_REG_OFFSET) ==
        1u);
  for (uint32_t i = 0; i < SENSOR_CTRL_PARAM_NUM_ALERT_EVENTS; ++i) {
    uint32_t alert_en = abs_mmio_read32(
        kSensorCtrlBase + SENSOR_CTRL_ALERT_EN_0_REG_OFFSET + i * 4u);
    CHECK(alert_en == kMultiBitBool4True || alert_en == kMultiBitBool4False,
          "ALERT_EN_%u expected 0x6 or 0x9, got 0x%x", i, alert_en);
  }

  LOG_INFO("2. Testing FATAL_ALERT SwAccessRO (software writes ignored)...");
  CHECK(abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_FATAL_ALERT_REG_OFFSET) ==
        0u);
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_FATAL_ALERT_REG_OFFSET,
                   0x7ffu);
  uint32_t fatal_alert_val =
      abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_FATAL_ALERT_REG_OFFSET);
  CHECK(fatal_alert_val == 0u);

  LOG_INFO(
      "3. Testing MANUAL_PAD_ATTR_0..3 RW and MANUAL_PAD_ATTR_REGWEN_0..3 "
      "W0C...");
  for (uint32_t i = 0; i < SENSOR_CTRL_PARAM_NUM_ATTR_PADS; ++i) {
    uint32_t attr_off =
        kSensorCtrlBase + SENSOR_CTRL_MANUAL_PAD_ATTR_0_REG_OFFSET + i * 4u;
    uint32_t regwen_off = kSensorCtrlBase +
                          SENSOR_CTRL_MANUAL_PAD_ATTR_REGWEN_0_REG_OFFSET +
                          i * 4u;

    CHECK(abs_mmio_read32(regwen_off) == 1u);
    abs_mmio_write32(attr_off, 0xffffffffu);
    uint32_t readback = abs_mmio_read32(attr_off);
    CHECK(readback == kManualPadAttrMask,
          "MANUAL_PAD_ATTR_%u expected 0x%x, got 0x%x", i, kManualPadAttrMask,
          readback);

    // Writing 1 to RW0C REGWEN must keep it enabled.
    abs_mmio_write32(regwen_off, 1u);
    CHECK(abs_mmio_read32(regwen_off) == 1u);
    abs_mmio_write32(attr_off,
                     (1u << SENSOR_CTRL_MANUAL_PAD_ATTR_0_PULL_EN_0_BIT));
    CHECK(abs_mmio_read32(attr_off) ==
          (1u << SENSOR_CTRL_MANUAL_PAD_ATTR_0_PULL_EN_0_BIT));

    // Lock MANUAL_PAD_ATTR_REGWEN_i by writing 0 (RW0C) and verify writes to
    // MANUAL_PAD_ATTR_i are ignored.
    if (i == 3u) {
      abs_mmio_write32(regwen_off, 0u);
      CHECK(abs_mmio_read32(regwen_off) == 0u);
      abs_mmio_write32(regwen_off, 1u);
      CHECK(abs_mmio_read32(regwen_off) == 0u);
      abs_mmio_write32(attr_off, kManualPadAttrMask);
      CHECK(abs_mmio_read32(attr_off) ==
            (1u << SENSOR_CTRL_MANUAL_PAD_ATTR_0_PULL_EN_0_BIT));
    } else {
      abs_mmio_write32(attr_off, 0u);
      CHECK(abs_mmio_read32(attr_off) == 0u);
    }
  }

  LOG_INFO(
      "4. Testing AST_ALERT latch when ALERT_TRIG pulsed while ALERT_EN == "
      "False...");
  // Disable ALERT_EN_0 (write kMultiBitBool4False = 0x9).
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_ALERT_EN_0_REG_OFFSET,
                   kMultiBitBool4False);
  // Clear any prior RECOV_ALERT bits.
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET,
                   0x7ffu);
  CHECK(abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET) ==
        0u);

  // Pulse ALERT_TRIG bit 0 (1 then 0) while ALERT_EN_0 is False.
  // In ast_alert.sv, set_p_alert latches p_alert = 1; because ALERT_EN_0 is
  // False, event_clr[0] is 0 so p_alert stays latched even after ALERT_TRIG=0.
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_ALERT_TRIG_REG_OFFSET, 1u);
  busy_spin_micros(5);
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_ALERT_TRIG_REG_OFFSET, 0u);
  busy_spin_micros(5);

  // While ALERT_EN_0 is still False, RECOV_ALERT bit 0 must still be 0.
  CHECK((abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET) &
         1u) == 0u);

  // Re-enable ALERT_EN_0 (write kMultiBitBool4True = 0x6).
  // The latched AST alert immediately fires recov_event[0], setting RECOV_ALERT
  // bit 0 and sending event_clr[0] back to ast_alert.sv.
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_ALERT_EN_0_REG_OFFSET,
                   kMultiBitBool4True);
  busy_spin_micros(5);
  uint32_t recov_val =
      abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET);
  CHECK((recov_val & 1u) == 1u,
        "Expected RECOV_ALERT bit 0 to latch from unacked AST_ALERT after "
        "re-enabling ALERT_EN_0, got 0x%x",
        recov_val);

  // Clear RECOV_ALERT bit 0 (RW1C) and verify it stays 0 now that ast_alert was
  // acked.
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET, 1u);
  busy_spin_micros(5);
  CHECK((abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET) &
         1u) == 0u);

  LOG_INFO(
      "5. Testing continuous ALERT_TRIG=1 re-latching RECOV_ALERT on RW1C and "
      "mubi4_test_true_loose...");
  // While ALERT_EN_0 is True (0x6), hold ALERT_TRIG bit 0 = 1.
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_ALERT_TRIG_REG_OFFSET, 1u);
  busy_spin_micros(5);
  CHECK((abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET) &
         1u) == 1u);
  // Attempting RW1C clear on RECOV_ALERT while ALERT_TRIG bit 0 is still 1 must
  // re-latch bit 0.
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET, 1u);
  busy_spin_micros(5);
  CHECK((abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET) &
         1u) == 1u);
  // Clear ALERT_TRIG bit 0 to 0, then RW1C clear RECOV_ALERT bit 0 -> must now
  // stay 0.
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_ALERT_TRIG_REG_OFFSET, 0u);
  busy_spin_micros(5);
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET, 1u);
  busy_spin_micros(5);
  CHECK((abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET) &
         1u) == 0u);

  // Test mubi4_test_true_loose on ALERT_EN_0: a non-False corrupt nibble (e.g.
  // 0x5) must still enable the alert.
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_ALERT_EN_0_REG_OFFSET, 0x5u);
  CHECK(abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_ALERT_EN_0_REG_OFFSET) ==
        0x5u);
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_ALERT_TRIG_REG_OFFSET, 1u);
  busy_spin_micros(5);
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_ALERT_TRIG_REG_OFFSET, 0u);
  busy_spin_micros(5);
  CHECK((abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET) &
         1u) == 1u);
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET, 1u);
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_ALERT_EN_0_REG_OFFSET,
                   kMultiBitBool4True);

  LOG_INFO("6. Testing INTR_TEST (WO) and INTR_STATE (RW1C)...");
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_INTR_ENABLE_REG_OFFSET, 0u);
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_INTR_STATE_REG_OFFSET, 0x3u);
  CHECK(abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_INTR_STATE_REG_OFFSET) ==
        0u);
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_INTR_TEST_REG_OFFSET, 0x3u);
  CHECK(abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_INTR_TEST_REG_OFFSET) ==
        0u);
  CHECK(abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_ALERT_TEST_REG_OFFSET) ==
        0u);
  CHECK(abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_INTR_STATE_REG_OFFSET) ==
        0x3u);
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_INTR_STATE_REG_OFFSET, 0x1u);
  CHECK(abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_INTR_STATE_REG_OFFSET) ==
        0x2u);
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_INTR_STATE_REG_OFFSET, 0x2u);
  CHECK(abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_INTR_STATE_REG_OFFSET) ==
        0u);

  LOG_INFO("7. Testing CFG_REGWEN RW0C lock on ALERT_EN and FATAL_ALERT_EN...");
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_CFG_REGWEN_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_CFG_REGWEN_REG_OFFSET) ==
        0u);
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_ALERT_EN_0_REG_OFFSET,
                   kMultiBitBool4False);
  CHECK(abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_ALERT_EN_0_REG_OFFSET) ==
        kMultiBitBool4True);
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_FATAL_ALERT_EN_REG_OFFSET,
                   0x7ffu);
  CHECK(abs_mmio_read32(kSensorCtrlBase +
                        SENSOR_CTRL_FATAL_ALERT_EN_REG_OFFSET) == 0u);

  LOG_INFO(
      "8. Testing SENSOR_CTRL_PERMIT sub-word wr_err and addrmiss TL-UL "
      "faults...");
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_ALERT_TRIG_REG_OFFSET, 0u);
  g_fault_seen = false;
  abs_mmio_write8(kSensorCtrlBase + SENSOR_CTRL_ALERT_TRIG_REG_OFFSET, 1u);
  CHECK(g_fault_seen && g_fault_mcause == kIbexExcStoreAccessFault);
  CHECK(abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_ALERT_TRIG_REG_OFFSET) ==
        0u);

  g_fault_seen = false;
  abs_mmio_write8(kSensorCtrlBase + SENSOR_CTRL_INTR_ENABLE_REG_OFFSET + 1u,
                  1u);
  CHECK(g_fault_seen && g_fault_mcause == kIbexExcStoreAccessFault);

  g_fault_seen = false;
  (void)abs_mmio_read32(kSensorCtrlBase + 0x74u);
  CHECK(g_fault_seen && g_fault_mcause == kIbexExcLoadAccessFault);

  g_fault_seen = false;
  abs_mmio_write32(kSensorCtrlBase + 0x74u, 0x1u);
  CHECK(g_fault_seen && g_fault_mcause == kIbexExcStoreAccessFault);

  return true;
}
