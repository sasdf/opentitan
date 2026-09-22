// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sysrst_ctrl_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kSysrstBase = TOP_EARLGREY_SYSRST_CTRL_AON_BASE_ADDR,
};

bool test_main(void) {
  // Clean initial state.
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_ULP_CTL_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INTR_CTL_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COM_PRE_SEL_CTL_0_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COM_SEL_CTL_0_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COM_OUT_CTL_0_REG_OFFSET, 0u);
  busy_spin_micros(60);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_ULP_STATUS_REG_OFFSET, 1u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COMBO_INTR_STATUS_REG_OFFSET,
                   0xfu);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INTR_STATUS_REG_OFFSET,
                   0x3fffu);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_WKUP_STATUS_REG_OFFSET, 1u);
  busy_spin_micros(60);

  uint32_t pin_in =
      abs_mmio_read32(kSysrstBase + SYSRST_CTRL_PIN_IN_VALUE_REG_OFFSET);
  uint32_t raw_ac = (pin_in >> SYSRST_CTRL_PIN_IN_VALUE_AC_PRESENT_BIT) & 1u;
  uint32_t raw_key0 = (pin_in >> SYSRST_CTRL_PIN_IN_VALUE_KEY0_IN_BIT) & 1u;

  // 1. ULP ac_present high-level detection with zero debounce
  // (ULP_AC_DEBOUNCE_CTL == 0, ot_sysrst_ctrl.c:389-393).
  uint32_t inv_ac_high =
      raw_ac ? 0u : (1u << SYSRST_CTRL_KEY_INVERT_CTL_AC_PRESENT_BIT);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_ULP_AC_DEBOUNCE_CTL_REG_OFFSET,
                   0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET,
                   inv_ac_high);
  busy_spin_micros(60);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_ULP_STATUS_REG_OFFSET, 1u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_WKUP_STATUS_REG_OFFSET, 1u);
  busy_spin_micros(60);
  CHECK(abs_mmio_read32(kSysrstBase + SYSRST_CTRL_ULP_STATUS_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kSysrstBase + SYSRST_CTRL_WKUP_STATUS_REG_OFFSET) ==
        0u);

  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_ULP_CTL_REG_OFFSET, 1u);
  busy_spin_micros(60);
  CHECK(abs_mmio_read32(kSysrstBase + SYSRST_CTRL_ULP_STATUS_REG_OFFSET) == 1u,
        "Expected ULP_STATUS == 1 when ULP_AC_DEBOUNCE_CTL == 0");
  CHECK(abs_mmio_read32(kSysrstBase + SYSRST_CTRL_WKUP_STATUS_REG_OFFSET) == 1u,
        "Expected WKUP_STATUS == 1 when ULP_AC_DEBOUNCE_CTL == 0");

  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_ULP_CTL_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET, 0u);
  busy_spin_micros(60);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_ULP_STATUS_REG_OFFSET, 1u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_WKUP_STATUS_REG_OFFSET, 1u);
  busy_spin_micros(60);

  // 2. Combo EC_RST trigger, zero-write while stretched
  // (ot_sysrst_ctrl.c:636-640), and zero-stretch release (EC_RST_CTL == 0,
  // ot_sysrst_ctrl.c:456-458).
  uint32_t inv_key0_high =
      raw_key0 ? 0u : (1u << SYSRST_CTRL_KEY_INVERT_CTL_KEY0_IN_BIT);
  uint32_t inv_key0_low =
      inv_key0_high ^ (1u << SYSRST_CTRL_KEY_INVERT_CTL_KEY0_IN_BIT);

  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET,
                   inv_key0_high);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INTR_DEBOUNCE_CTL_REG_OFFSET,
                   0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COM_PRE_SEL_CTL_0_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COM_PRE_DET_CTL_0_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COM_SEL_CTL_0_REG_OFFSET, 1u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COM_DET_CTL_0_REG_OFFSET, 0u);
  // COM_OUT_CTL_0: bit 1 = interrupt, bit 2 = ec_rst.
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COM_OUT_CTL_0_REG_OFFSET,
                   (1u << 2u) | (1u << 1u));
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_EC_RST_CTL_REG_OFFSET, 200u);
  busy_spin_micros(60);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COMBO_INTR_STATUS_REG_OFFSET,
                   0xfu);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_WKUP_STATUS_REG_OFFSET, 1u);
  busy_spin_micros(60);

  // Trigger combo 0 (key0 pressed -> ec_rst_l_hw = false), then write
  // EC_RST_CTL = 0 while ec_rst_l_hw is active and release key0.
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET,
                   inv_key0_low);
  busy_spin_micros(60);
  CHECK(
      (abs_mmio_read32(kSysrstBase + SYSRST_CTRL_COMBO_INTR_STATUS_REG_OFFSET) &
       1u) == 1u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_EC_RST_CTL_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kSysrstBase + SYSRST_CTRL_EC_RST_CTL_REG_OFFSET) == 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET,
                   inv_key0_high);
  busy_spin_micros(60);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COMBO_INTR_STATUS_REG_OFFSET,
                   0xfu);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_WKUP_STATUS_REG_OFFSET, 1u);
  busy_spin_micros(60);
  CHECK(abs_mmio_read32(kSysrstBase +
                        SYSRST_CTRL_COMBO_INTR_STATUS_REG_OFFSET) == 0u);

  // Now trigger and release combo 0 while EC_RST_CTL == 0 (stretch == 0).
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET,
                   inv_key0_low);
  busy_spin_micros(60);
  CHECK(
      (abs_mmio_read32(kSysrstBase + SYSRST_CTRL_COMBO_INTR_STATUS_REG_OFFSET) &
       1u) == 1u);
  CHECK(abs_mmio_read32(kSysrstBase + SYSRST_CTRL_WKUP_STATUS_REG_OFFSET) ==
        1u);

  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET,
                   inv_key0_high);
  busy_spin_micros(60);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COMBO_INTR_STATUS_REG_OFFSET,
                   0xfu);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_WKUP_STATUS_REG_OFFSET, 1u);
  busy_spin_micros(60);
  CHECK(abs_mmio_read32(kSysrstBase +
                        SYSRST_CTRL_COMBO_INTR_STATUS_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kSysrstBase + SYSRST_CTRL_WKUP_STATUS_REG_OFFSET) ==
        0u);

  // Clean up.
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COM_SEL_CTL_0_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_COM_OUT_CTL_0_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_KEY_INVERT_CTL_REG_OFFSET, 0u);
  abs_mmio_write32(kSysrstBase + SYSRST_CTRL_EC_RST_CTL_REG_OFFSET, 2000u);

  return true;
}
