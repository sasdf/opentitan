// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sensor_ctrl_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kBase = TOP_EARLGREY_SENSOR_CTRL_AON_BASE_ADDR,
};

bool test_main(void) {
  // 1. Verify read-only STATUS and FATAL_ALERT registers and write-ignore
  // behavior.
  uint32_t status = abs_mmio_read32(kBase + SENSOR_CTRL_STATUS_REG_OFFSET);
  CHECK(status == 0x7u);

  uint32_t fatal_alert =
      abs_mmio_read32(kBase + SENSOR_CTRL_FATAL_ALERT_REG_OFFSET);
  CHECK(fatal_alert == 0x0u);

  abs_mmio_write32(kBase + SENSOR_CTRL_STATUS_REG_OFFSET, 0x00000000u);
  CHECK(abs_mmio_read32(kBase + SENSOR_CTRL_STATUS_REG_OFFSET) == 0x7u);
  abs_mmio_write32(kBase + SENSOR_CTRL_STATUS_REG_OFFSET, 0xffffffffu);
  CHECK(abs_mmio_read32(kBase + SENSOR_CTRL_STATUS_REG_OFFSET) == 0x7u);

  abs_mmio_write32(kBase + SENSOR_CTRL_FATAL_ALERT_REG_OFFSET, 0x00000000u);
  CHECK(abs_mmio_read32(kBase + SENSOR_CTRL_FATAL_ALERT_REG_OFFSET) == 0x0u);
  abs_mmio_write32(kBase + SENSOR_CTRL_FATAL_ALERT_REG_OFFSET, 0xffffffffu);
  CHECK(abs_mmio_read32(kBase + SENSOR_CTRL_FATAL_ALERT_REG_OFFSET) == 0x0u);

  // 2. Verify write-only INTR_TEST and ALERT_TEST readback as 0.
  CHECK(abs_mmio_read32(kBase + SENSOR_CTRL_INTR_TEST_REG_OFFSET) == 0x0u);
  CHECK(abs_mmio_read32(kBase + SENSOR_CTRL_ALERT_TEST_REG_OFFSET) == 0x0u);

  // 3. Verify INTR_ENABLE, INTR_TEST, and INTR_STATE RW1C behavior.
  abs_mmio_write32(kBase + SENSOR_CTRL_INTR_ENABLE_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kBase + SENSOR_CTRL_INTR_ENABLE_REG_OFFSET) == 0x0u);

  // Clear any pending boot status-change interrupts first.
  abs_mmio_write32(kBase + SENSOR_CTRL_INTR_STATE_REG_OFFSET, 0x3u);
  CHECK(abs_mmio_read32(kBase + SENSOR_CTRL_INTR_STATE_REG_OFFSET) == 0x0u);

  abs_mmio_write32(kBase + SENSOR_CTRL_INTR_TEST_REG_OFFSET, 0x3u);
  CHECK(abs_mmio_read32(kBase + SENSOR_CTRL_INTR_STATE_REG_OFFSET) == 0x3u);
  abs_mmio_write32(kBase + SENSOR_CTRL_INTR_STATE_REG_OFFSET, 0x1u);
  CHECK(abs_mmio_read32(kBase + SENSOR_CTRL_INTR_STATE_REG_OFFSET) == 0x2u);
  abs_mmio_write32(kBase + SENSOR_CTRL_INTR_STATE_REG_OFFSET, 0x2u);
  CHECK(abs_mmio_read32(kBase + SENSOR_CTRL_INTR_STATE_REG_OFFSET) == 0x0u);

  // 4. Verify MANUAL_PAD_ATTR_0 read/write and MANUAL_PAD_ATTR_REGWEN_0 RW0C
  // lock.
  CHECK(abs_mmio_read32(
            kBase + SENSOR_CTRL_MANUAL_PAD_ATTR_REGWEN_0_REG_OFFSET) == 0x1u);
  abs_mmio_write32(kBase + SENSOR_CTRL_MANUAL_PAD_ATTR_0_REG_OFFSET, 0x8cu);
  CHECK(abs_mmio_read32(kBase + SENSOR_CTRL_MANUAL_PAD_ATTR_0_REG_OFFSET) ==
        0x8cu);
  abs_mmio_write32(kBase + SENSOR_CTRL_MANUAL_PAD_ATTR_0_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kBase + SENSOR_CTRL_MANUAL_PAD_ATTR_0_REG_OFFSET) ==
        0x0u);

  abs_mmio_write32(kBase + SENSOR_CTRL_MANUAL_PAD_ATTR_REGWEN_0_REG_OFFSET,
                   0x0u);
  CHECK(abs_mmio_read32(
            kBase + SENSOR_CTRL_MANUAL_PAD_ATTR_REGWEN_0_REG_OFFSET) == 0x0u);
  abs_mmio_write32(kBase + SENSOR_CTRL_MANUAL_PAD_ATTR_REGWEN_0_REG_OFFSET,
                   0x1u);
  CHECK(abs_mmio_read32(
            kBase + SENSOR_CTRL_MANUAL_PAD_ATTR_REGWEN_0_REG_OFFSET) == 0x0u);

  // Writes to locked MANUAL_PAD_ATTR_0 must be ignored.
  abs_mmio_write32(kBase + SENSOR_CTRL_MANUAL_PAD_ATTR_0_REG_OFFSET, 0x8cu);
  CHECK(abs_mmio_read32(kBase + SENSOR_CTRL_MANUAL_PAD_ATTR_0_REG_OFFSET) ==
        0x0u);

  // 5. Verify CFG_REGWEN RW0C lock gating ALERT_EN_0 and FATAL_ALERT_EN.
  uint32_t alert_en_0_init =
      abs_mmio_read32(kBase + SENSOR_CTRL_ALERT_EN_0_REG_OFFSET);
  uint32_t fatal_alert_en_init =
      abs_mmio_read32(kBase + SENSOR_CTRL_FATAL_ALERT_EN_REG_OFFSET);

  abs_mmio_write32(kBase + SENSOR_CTRL_CFG_REGWEN_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kBase + SENSOR_CTRL_CFG_REGWEN_REG_OFFSET) == 0x0u);
  abs_mmio_write32(kBase + SENSOR_CTRL_CFG_REGWEN_REG_OFFSET, 0x1u);
  CHECK(abs_mmio_read32(kBase + SENSOR_CTRL_CFG_REGWEN_REG_OFFSET) == 0x0u);

  abs_mmio_write32(kBase + SENSOR_CTRL_ALERT_EN_0_REG_OFFSET,
                   alert_en_0_init ^ 0xfu);
  CHECK(abs_mmio_read32(kBase + SENSOR_CTRL_ALERT_EN_0_REG_OFFSET) ==
        alert_en_0_init);

  abs_mmio_write32(kBase + SENSOR_CTRL_FATAL_ALERT_EN_REG_OFFSET,
                   fatal_alert_en_init ^ 0x7ffu);
  CHECK(abs_mmio_read32(kBase + SENSOR_CTRL_FATAL_ALERT_EN_REG_OFFSET) ==
        fatal_alert_en_init);

  LOG_INFO("ot_sensor_eg_consistency_test passed");
  return true;
}
