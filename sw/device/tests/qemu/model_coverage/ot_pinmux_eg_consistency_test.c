// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "pinmux_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kBase = TOP_EARLGREY_PINMUX_AON_BASE_ADDR,
};

bool test_main(void) {
  // 1. Verify write-only ALERT_TEST register reads back as 0.
  CHECK(abs_mmio_read32(kBase + PINMUX_ALERT_TEST_REG_OFFSET) == 0x0u);
  abs_mmio_write32(kBase + PINMUX_ALERT_TEST_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kBase + PINMUX_ALERT_TEST_REG_OFFSET) == 0x0u);

  // 2. Verify bit-width masking on MIO_PERIPH_INSEL_0 (0x3f), MIO_OUTSEL_0
  // (0x7f), and WKUP_DETECTOR_PADSEL_0 (0x3f).
  abs_mmio_write32(kBase + PINMUX_MIO_PERIPH_INSEL_0_REG_OFFSET, 0xffffffffu);
  CHECK(abs_mmio_read32(kBase + PINMUX_MIO_PERIPH_INSEL_0_REG_OFFSET) == 0x3fu);
  abs_mmio_write32(kBase + PINMUX_MIO_PERIPH_INSEL_0_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kBase + PINMUX_MIO_PERIPH_INSEL_0_REG_OFFSET) == 0x0u);

  abs_mmio_write32(kBase + PINMUX_MIO_OUTSEL_0_REG_OFFSET, 0xffffffffu);
  CHECK(abs_mmio_read32(kBase + PINMUX_MIO_OUTSEL_0_REG_OFFSET) == 0x7fu);
  abs_mmio_write32(kBase + PINMUX_MIO_OUTSEL_0_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kBase + PINMUX_MIO_OUTSEL_0_REG_OFFSET) == 0x0u);

  abs_mmio_write32(kBase + PINMUX_WKUP_DETECTOR_PADSEL_0_REG_OFFSET,
                   0xffffffffu);
  CHECK(abs_mmio_read32(kBase + PINMUX_WKUP_DETECTOR_PADSEL_0_REG_OFFSET) ==
        0x3fu);
  abs_mmio_write32(kBase + PINMUX_WKUP_DETECTOR_PADSEL_0_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kBase + PINMUX_WKUP_DETECTOR_PADSEL_0_REG_OFFSET) ==
        0x0u);

  // 3. Verify MIO_PAD_SLEEP_STATUS_1 RW0C read/write behavior.
  abs_mmio_write32(kBase + PINMUX_MIO_PAD_SLEEP_STATUS_1_REG_OFFSET,
                   0xffffffffu);
  CHECK(abs_mmio_read32(kBase + PINMUX_MIO_PAD_SLEEP_STATUS_1_REG_OFFSET) ==
        0x0u);
  abs_mmio_write32(kBase + PINMUX_MIO_PAD_SLEEP_STATUS_1_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kBase + PINMUX_MIO_PAD_SLEEP_STATUS_1_REG_OFFSET) ==
        0x0u);

  // 4. Verify Wakeup Detector TimedLow mode (mode = 4) with constant 0 input
  // (PADSEL_0 = 0).
  abs_mmio_write32(kBase + PINMUX_WKUP_DETECTOR_EN_0_REG_OFFSET, 0x0u);
  busy_spin_micros(20);
  abs_mmio_write32(kBase + PINMUX_WKUP_CAUSE_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kBase + PINMUX_WKUP_CAUSE_REG_OFFSET) == 0x0u);

  abs_mmio_write32(kBase + PINMUX_WKUP_DETECTOR_0_REG_OFFSET, 0x4u);
  abs_mmio_write32(kBase + PINMUX_WKUP_DETECTOR_CNT_TH_0_REG_OFFSET, 0x0u);
  abs_mmio_write32(kBase + PINMUX_WKUP_DETECTOR_PADSEL_0_REG_OFFSET, 0x0u);
  abs_mmio_write32(kBase + PINMUX_WKUP_DETECTOR_EN_0_REG_OFFSET, 0x1u);
  CHECK(abs_mmio_read32(kBase + PINMUX_WKUP_DETECTOR_EN_0_REG_OFFSET) == 0x1u);

  busy_spin_micros(30);
  CHECK((abs_mmio_read32(kBase + PINMUX_WKUP_CAUSE_REG_OFFSET) & 0x1u) == 0x1u);

  abs_mmio_write32(kBase + PINMUX_WKUP_DETECTOR_EN_0_REG_OFFSET, 0x0u);
  busy_spin_micros(20);
  abs_mmio_write32(kBase + PINMUX_WKUP_CAUSE_REG_OFFSET, 0x0u);
  CHECK((abs_mmio_read32(kBase + PINMUX_WKUP_CAUSE_REG_OFFSET) & 0x1u) == 0x0u);

  // 5. Verify RW0C locking across all 7 REGWEN register families.
  abs_mmio_write32(kBase + PINMUX_MIO_PERIPH_INSEL_REGWEN_0_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kBase + PINMUX_MIO_PERIPH_INSEL_REGWEN_0_REG_OFFSET) ==
        0x0u);
  abs_mmio_write32(kBase + PINMUX_MIO_PERIPH_INSEL_0_REG_OFFSET, 0x15u);
  CHECK(abs_mmio_read32(kBase + PINMUX_MIO_PERIPH_INSEL_0_REG_OFFSET) == 0x0u);

  abs_mmio_write32(kBase + PINMUX_MIO_OUTSEL_REGWEN_0_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kBase + PINMUX_MIO_OUTSEL_REGWEN_0_REG_OFFSET) == 0x0u);
  abs_mmio_write32(kBase + PINMUX_MIO_OUTSEL_0_REG_OFFSET, 0x2au);
  CHECK(abs_mmio_read32(kBase + PINMUX_MIO_OUTSEL_0_REG_OFFSET) == 0x0u);

  uint32_t mio_attr_init =
      abs_mmio_read32(kBase + PINMUX_MIO_PAD_ATTR_0_REG_OFFSET);
  abs_mmio_write32(kBase + PINMUX_MIO_PAD_ATTR_REGWEN_0_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kBase + PINMUX_MIO_PAD_ATTR_REGWEN_0_REG_OFFSET) ==
        0x0u);
  abs_mmio_write32(kBase + PINMUX_MIO_PAD_ATTR_0_REG_OFFSET,
                   mio_attr_init ^ 0x1u);
  CHECK(abs_mmio_read32(kBase + PINMUX_MIO_PAD_ATTR_0_REG_OFFSET) ==
        mio_attr_init);

  uint32_t dio_attr_init =
      abs_mmio_read32(kBase + PINMUX_DIO_PAD_ATTR_15_REG_OFFSET);
  abs_mmio_write32(kBase + PINMUX_DIO_PAD_ATTR_REGWEN_15_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kBase + PINMUX_DIO_PAD_ATTR_REGWEN_15_REG_OFFSET) ==
        0x0u);
  abs_mmio_write32(kBase + PINMUX_DIO_PAD_ATTR_15_REG_OFFSET,
                   dio_attr_init ^ 0x1u);
  CHECK(abs_mmio_read32(kBase + PINMUX_DIO_PAD_ATTR_15_REG_OFFSET) ==
        dio_attr_init);

  abs_mmio_write32(kBase + PINMUX_MIO_PAD_SLEEP_REGWEN_0_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kBase + PINMUX_MIO_PAD_SLEEP_REGWEN_0_REG_OFFSET) ==
        0x0u);
  abs_mmio_write32(kBase + PINMUX_MIO_PAD_SLEEP_EN_0_REG_OFFSET, 0x1u);
  CHECK(abs_mmio_read32(kBase + PINMUX_MIO_PAD_SLEEP_EN_0_REG_OFFSET) == 0x0u);
  abs_mmio_write32(kBase + PINMUX_MIO_PAD_SLEEP_MODE_0_REG_OFFSET, 0x3u);
  CHECK(abs_mmio_read32(kBase + PINMUX_MIO_PAD_SLEEP_MODE_0_REG_OFFSET) ==
        0x2u);

  abs_mmio_write32(kBase + PINMUX_DIO_PAD_SLEEP_REGWEN_15_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kBase + PINMUX_DIO_PAD_SLEEP_REGWEN_15_REG_OFFSET) ==
        0x0u);
  abs_mmio_write32(kBase + PINMUX_DIO_PAD_SLEEP_EN_15_REG_OFFSET, 0x1u);
  CHECK(abs_mmio_read32(kBase + PINMUX_DIO_PAD_SLEEP_EN_15_REG_OFFSET) == 0x0u);
  abs_mmio_write32(kBase + PINMUX_DIO_PAD_SLEEP_MODE_15_REG_OFFSET, 0x3u);
  CHECK(abs_mmio_read32(kBase + PINMUX_DIO_PAD_SLEEP_MODE_15_REG_OFFSET) ==
        0x2u);

  abs_mmio_write32(kBase + PINMUX_WKUP_DETECTOR_REGWEN_0_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kBase + PINMUX_WKUP_DETECTOR_REGWEN_0_REG_OFFSET) ==
        0x0u);
  abs_mmio_write32(kBase + PINMUX_WKUP_DETECTOR_EN_0_REG_OFFSET, 0x1u);
  CHECK(abs_mmio_read32(kBase + PINMUX_WKUP_DETECTOR_EN_0_REG_OFFSET) == 0x0u);
  abs_mmio_write32(kBase + PINMUX_WKUP_DETECTOR_0_REG_OFFSET, 0x1u);
  CHECK(abs_mmio_read32(kBase + PINMUX_WKUP_DETECTOR_0_REG_OFFSET) == 0x4u);
  abs_mmio_write32(kBase + PINMUX_WKUP_DETECTOR_CNT_TH_0_REG_OFFSET, 0x55u);
  CHECK(abs_mmio_read32(kBase + PINMUX_WKUP_DETECTOR_CNT_TH_0_REG_OFFSET) ==
        0x0u);
  abs_mmio_write32(kBase + PINMUX_WKUP_DETECTOR_PADSEL_0_REG_OFFSET, 0x15u);
  CHECK(abs_mmio_read32(kBase + PINMUX_WKUP_DETECTOR_PADSEL_0_REG_OFFSET) ==
        0x0u);

  LOG_INFO("ot_pinmux_eg_consistency_test passed");
  return true;
}
