// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file sensor_ctrl_errata_v2_test.c
 * @brief Physical CW340 FPGA verification test for Earlgrey v2 (`trunk-v2`)
 *        `sensor_ctrl` errata:
 *   - [ERRATA-SENSOR_CTRL-001] (CONFIRMED_PRESENT_ON_V2):
 *     `ALERT_TRIG` latches persistently inside `ast_alert.sv` flip-flops across
 *     `1 -> 0` pulses when `ALERT_EN_i == kMultiBitBool4False` (firing later
 *     as soon as `ALERT_EN_i` is enabled), and `RECOV_ALERT` (`rw1c`) cannot
 *     clear while `ALERT_TRIG_i == 1`. Also verifies `CFG_REGWEN` locks both
 *     `FATAL_ALERT_EN` and `ALERT_EN_0..10`.
 *   - [ERRATA-SENSOR_CTRL-002] (CONFIRMED_PRESENT_ON_V2):
 *     `SENSOR_CTRL_PERMIT = 4'b0011` rejects 8-bit `sb` writes to `ALERT_TRIG`,
 *     `FATAL_ALERT_EN`, `RECOV_ALERT`, and read-only `FATAL_ALERT` with a
 *     synchronous Store Access Fault (`mcause = 7`), whereas 16-bit `sh` and
 *     32-bit `sw` writes succeed, and unmapped offsets `0x74..0x7c` raise
 *     `addrmiss` bus faults (`mcause = 5` / `7`).
 *   - [ERRATA-SENSOR_CTRL-V2-001] (NEW_IN_V2):
 *     `MANUAL_PAD_ATTR_0..3` (`0x64..0x70`) is documented in
 * `sensor_ctrl.hjson` as having WARL pad-attribute behavior only supported on
 * `chip_earlgrey_asic`, and is omitted from `dif_sensor_ctrl`, yet
 * `sensor_ctrl.sv:354-388` implements unconditional flip-flops for all 3
 * defined bits (`0x8c`: `pull_en`, `pull_select`, `input_disable`) without any
 * WARL pad-mask input on CW340 FPGA.
 *   - [ERRATA-SENSOR_CTRL-V2-002] (NEW_IN_V2):
 *     `ALERT_EN_0..10` (`0x18..0x40`) uses `mubi4_test_true_loose` (`!= 0x9`)
 *     in `sensor_ctrl.sv:213`, so writing `0x0` (or any non-`0x9` value such as
 *     `0x5` or `0xF`) to `ALERT_EN_i` leaves the alert channel ENABLED in
 *     hardware even though `ALERT_EN_i` reads back `0x0`.
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/dif/dif_sensor_ctrl.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top/sensor_ctrl_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG(.ignore_alerts = true);

enum {
  kSensorCtrlBase = TOP_EARLGREY_SENSOR_CTRL_BASE_ADDR,
};

static volatile uint32_t g_fault_count = 0;
static volatile uint32_t g_last_mcause = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = ibex_mcause_read();
  g_last_mcause = mcause;
  g_fault_count++;

  uint32_t mepc;
  CSR_READ(CSR_REG_MEPC, &mepc);
  uint16_t insn_first_half = *(const volatile uint16_t *)mepc;
  uint32_t step = ((insn_first_half & 0x3u) != 0x3u) ? 2u : 4u;
  CSR_WRITE(CSR_REG_MEPC, mepc + step);
}

static inline void mmio_write8(uint32_t addr, uint8_t val) {
  asm volatile("sb %0, 0(%1)" : : "r"(val), "r"(addr) : "memory");
}

static inline void mmio_write16(uint32_t addr, uint16_t val) {
  asm volatile("sh %0, 0(%1)" : : "r"(val), "r"(addr) : "memory");
}

static void test_v1_001_ast_alert_latch_and_ack(void) {
  LOG_INFO("Testing [ERRATA-SENSOR_CTRL-001] on trunk-v2...");

  // Ensure channel 0 is recoverable (FATAL_ALERT_EN[0] = 0) and initially
  // disabled (ALERT_EN_0 = kMultiBitBool4False = 0x9).
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_FATAL_ALERT_EN_REG_OFFSET, 0u);
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_ALERT_EN_0_REG_OFFSET,
                   kMultiBitBool4False);
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_ALERT_TRIG_REG_OFFSET, 0u);
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET,
                   0x7ffu);
  busy_spin_micros(10);

  // 1. Pulse ALERT_TRIG[0] = 1 -> 0 while ALERT_EN_0 == kMultiBitBool4False.
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_ALERT_TRIG_REG_OFFSET, 1u);
  busy_spin_micros(10);
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_ALERT_TRIG_REG_OFFSET, 0u);
  busy_spin_micros(10);

  // While ALERT_EN_0 is disabled, RECOV_ALERT[0] stays 0 even though p_alert is
  // now secretly latched inside ast_alert.sv!
  uint32_t recov_while_disabled =
      abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET);
  CHECK((recov_while_disabled & 1u) == 0u,
        "Expected RECOV_ALERT[0] == 0 while ALERT_EN_0 is False");

  // Now enable ALERT_EN_0 = kMultiBitBool4True (0x6) while ALERT_TRIG == 0:
  // the hidden latched p_alert in ast_alert.sv immediately sets RECOV_ALERT[0]!
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_ALERT_EN_0_REG_OFFSET,
                   kMultiBitBool4True);
  busy_spin_micros(10);
  uint32_t recov_after_en =
      abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET);
  CHECK((recov_after_en & 1u) == 1u,
        "Expected hidden ast_alert latch to set RECOV_ALERT[0] once ALERT_EN_0 "
        "is enabled");

  // Clear RECOV_ALERT[0] (rw1c) while ALERT_TRIG[0] == 0: now it clears
  // cleanly.
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET, 1u);
  busy_spin_micros(10);
  CHECK((abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET) &
         1u) == 0u,
        "Expected RECOV_ALERT[0] to clear when ALERT_TRIG[0] == 0");

  // 2. Now hold ALERT_TRIG[0] = 1 while ALERT_EN_0 == kMultiBitBool4True:
  // writing W1C to RECOV_ALERT[0] cannot clear it while set_p_alert is active!
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_ALERT_TRIG_REG_OFFSET, 1u);
  busy_spin_micros(10);
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET, 1u);
  busy_spin_micros(10);
  CHECK(
      (abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET) &
       1u) == 1u,
      "Expected RECOV_ALERT[0] to re-latch immediately while ALERT_TRIG[0]=1");

  // Clear ALERT_TRIG[0] = 0, then W1C RECOV_ALERT[0] = 1 -> clears to 0.
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_ALERT_TRIG_REG_OFFSET, 0u);
  busy_spin_micros(10);
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET, 1u);
  busy_spin_micros(10);
  CHECK((abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET) &
         1u) == 0u,
        "Expected RECOV_ALERT[0] == 0 after clearing ALERT_TRIG[0] and W1C");
}

static void test_v1_002_permit_subword_and_addrmiss(void) {
  LOG_INFO("Testing [ERRATA-SENSOR_CTRL-002] on trunk-v2...");

  // 1. 8-bit sb writes to PERMIT = 4'b0011 registers (ALERT_TRIG,
  // FATAL_ALERT_EN, RECOV_ALERT, FATAL_ALERT) must fault with mcause = 7!
  const uint32_t kPermit0011Offsets[] = {
      SENSOR_CTRL_ALERT_TRIG_REG_OFFSET,
      SENSOR_CTRL_FATAL_ALERT_EN_REG_OFFSET,
      SENSOR_CTRL_RECOV_ALERT_REG_OFFSET,
      SENSOR_CTRL_FATAL_ALERT_REG_OFFSET,
  };
  for (size_t i = 0; i < 4u; ++i) {
    g_fault_count = 0;
    g_last_mcause = 0;
    mmio_write8(kSensorCtrlBase + kPermit0011Offsets[i], 0u);
    CHECK(g_fault_count == 1u && g_last_mcause == 7u,
          "Expected sb to offset 0x%x (PERMIT=4'b0011) to fault with mcause=7",
          kPermit0011Offsets[i]);
  }

  // 2. 16-bit sh write to RECOV_ALERT (PERMIT = 4'b0011) and 8-bit sb write to
  // ALERT_EN_0 (PERMIT = 4'b0001) must succeed without fault!
  g_fault_count = 0;
  mmio_write16(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET, 0u);
  mmio_write8(kSensorCtrlBase + SENSOR_CTRL_ALERT_EN_0_REG_OFFSET,
              kMultiBitBool4True);
  CHECK(g_fault_count == 0u,
        "Expected sh to RECOV_ALERT and sb to ALERT_EN_0 to succeed");

  // 3. Unmapped offsets 0x74..0x7c within BlockAw = 7 (128B) aperture assert
  // addrmiss = 1 (mcause = 5 on lw, mcause = 7 on sw).
  g_fault_count = 0;
  g_last_mcause = 0;
  (void)abs_mmio_read32(kSensorCtrlBase + 0x74u);
  CHECK(g_fault_count == 1u && g_last_mcause == 5u,
        "Expected lw at unmapped offset 0x74 to fault with mcause=5");

  g_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write32(kSensorCtrlBase + 0x74u, 0u);
  CHECK(g_fault_count == 1u && g_last_mcause == 7u,
        "Expected sw at unmapped offset 0x74 to fault with mcause=7");
}

static void test_v2_001_manual_pad_attr_no_warl_on_fpga(void) {
  LOG_INFO("Testing [ERRATA-SENSOR_CTRL-V2-001] (NEW_IN_V2) on trunk-v2...");

  // sensor_ctrl.hjson states MANUAL_PAD_ATTR_0..3 (0x64..0x70) has WARL
  // behavior and is only supported on chip_earlgrey_asic, yet sensor_ctrl.sv
  // implements unconditional flip-flops for bits {7, 3, 2} (mask 0x8c) on all
  // targets (including CW340 FPGA).
  for (uint32_t k = 0; k < 4u; ++k) {
    uint32_t attr_off = SENSOR_CTRL_MANUAL_PAD_ATTR_0_REG_OFFSET + 4u * k;
    uint32_t regwen_off =
        SENSOR_CTRL_MANUAL_PAD_ATTR_REGWEN_0_REG_OFFSET + 4u * k;

    abs_mmio_write32(kSensorCtrlBase + attr_off, 0xffu);
    uint32_t rb = abs_mmio_read32(kSensorCtrlBase + attr_off);
    CHECK(rb == 0x8cu,
          "Expected MANUAL_PAD_ATTR_%u to store all 3 defined bits (0x8c), got "
          "0x%x",
          k, rb);

    // Lock MANUAL_PAD_ATTR_REGWEN_0 (k == 0) and verify writes are blocked.
    if (k == 0u) {
      abs_mmio_write32(kSensorCtrlBase + regwen_off, 0u);
      abs_mmio_write32(kSensorCtrlBase + attr_off, 0u);
      CHECK(abs_mmio_read32(kSensorCtrlBase + attr_off) == 0x8cu,
            "Expected MANUAL_PAD_ATTR_0 to remain 0x8c after locking REGWEN_0");
    } else {
      abs_mmio_write32(kSensorCtrlBase + attr_off, 0u);
    }
  }
}

static void test_v2_002_alert_en_loose_mubi4_zero_stays_enabled(void) {
  LOG_INFO("Testing [ERRATA-SENSOR_CTRL-V2-002] (NEW_IN_V2) on trunk-v2...");

  // Write 0x0 (non-MuBi4 encoding, often written by naive software intending
  // to disable) to ALERT_EN_1 (0x1c).
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_ALERT_EN_1_REG_OFFSET, 0x0u);
  uint32_t alert_en_1 =
      abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_ALERT_EN_1_REG_OFFSET);
  CHECK(alert_en_1 == 0x0u, "Expected ALERT_EN_1 to read back 0x0");

  // Pulse ALERT_TRIG[1] = 1 -> 0: because sensor_ctrl.sv:213 uses
  // mubi4_test_true_loose (val != 0x9), ALERT_EN_1 == 0x0 is treated as
  // ENABLED (1'b1) and immediately sets RECOV_ALERT[1]!
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET,
                   0x7ffu);
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_ALERT_TRIG_REG_OFFSET,
                   (1u << 1));
  busy_spin_micros(10);
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_ALERT_TRIG_REG_OFFSET, 0u);
  busy_spin_micros(10);

  uint32_t recov =
      abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET);
  CHECK(
      (recov & (1u << 1)) != 0u,
      "Expected ALERT_EN_1 == 0x0 to remain ENABLED via mubi4_test_true_loose "
      "and fire RECOV_ALERT[1]");

  // Clear RECOV_ALERT[1] and restore ALERT_EN_1 = kMultiBitBool4True.
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET,
                   (1u << 1));
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_ALERT_EN_1_REG_OFFSET,
                   kMultiBitBool4True);

  // Finally verify CFG_REGWEN locks both FATAL_ALERT_EN and ALERT_EN_0..10.
  dif_sensor_ctrl_t sensor_ctrl;
  CHECK_DIF_OK(dif_sensor_ctrl_init(mmio_region_from_addr(kSensorCtrlBase),
                                    &sensor_ctrl));
  CHECK_DIF_OK(dif_sensor_ctrl_lock_cfg(&sensor_ctrl));
  CHECK(dif_sensor_ctrl_set_alert_en(&sensor_ctrl, 0, kDifToggleDisabled) ==
            kDifLocked,
        "Expected dif_sensor_ctrl_set_alert_en to return kDifLocked");
  CHECK(dif_sensor_ctrl_set_alert_fatal(&sensor_ctrl, 0, kDifToggleEnabled) ==
            kDifLocked,
        "Expected dif_sensor_ctrl_set_alert_fatal to return kDifLocked");
}

bool test_main(void) {
  LOG_INFO(
      "=== SENSOR_CTRL Earlgrey v2 (trunk-v2) Errata Verification Test ===");
  test_v1_001_ast_alert_latch_and_ack();
  test_v1_002_permit_subword_and_addrmiss();
  test_v2_001_manual_pad_attr_no_warl_on_fpga();
  test_v2_002_alert_en_loose_mubi4_zero_stays_enabled();
  LOG_INFO(
      "=== ALL SENSOR_CTRL Earlgrey v2 Errata Checks PASSED on CW340 FPGA! "
      "===");
  return true;
}
