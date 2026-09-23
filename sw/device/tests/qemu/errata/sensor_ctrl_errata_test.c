// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file sensor_ctrl_errata_test.c
 * @brief CW340 FPGA & QEMU Errata Confirmation Test for `sensor_ctrl` (P28).
 *
 * Empirically confirms on both physical CW340 FPGA silicon and QEMU:
 * - [ast_alert.sv:44-75] (SPEC_DOC_ERRATA):
 *   1) `ALERT_TRIG` drives `ast_alert.sv:44-75`, which latches `p_alert <=
 * 1'b1` on a `1 -> 0` pulse while `ALERT_EN_0 == kMultiBitBool4False` (`0x9`),
 *      so re-enabling `ALERT_EN_0 == kMultiBitBool4True` (`0x6`) later
 *      immediately fires `RECOV_ALERT` bit 0 even though `ALERT_TRIG == 0`
 *      (contrary to `sensor_ctrl.hjson:174` "0 No alerts triggered").
 *   2) While `ALERT_TRIG[0] == 1`, `clr_p_alert = !set_p_alert && p_alert_ack`
 *      stays `0`, so `RW1C` clearing `RECOV_ALERT[0]` immediately re-latches
 *      `RECOV_ALERT[0] = 1` until `ALERT_TRIG[0]` is cleared to `0`.
 *   3) `CFG_REGWEN` (`rw0c`) locks both `FATAL_ALERT_EN` and all 11
 *      `ALERT_EN_0..10` registers (`sensor_ctrl_reg_top.sv:655-1280`).
 * - [sensor_ctrl_reg_pkg.sv:254-284] (INTENDED_SECURITY_HARDENING):
 *   `SENSOR_CTRL_PERMIT` assigns a 2-byte minimum write mask (`4'b0011`) to the
 *   11/12-bit multiregs (`ALERT_TRIG`, `FATAL_ALERT_EN`, `RECOV_ALERT`, and
 *   read-only `FATAL_ALERT`), causing 1-byte `sb` stores to fault with
 *   `d_error = 1` (`mcause = 7`), while unmapped offsets `0x74..0x7f` fault via
 *   `addrmiss` (`mcause = 5 / 7`).
 */

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sensor_ctrl_regs.h"

OTTF_DEFINE_TEST_CONFIG(.ignore_alerts = true);

static volatile bool g_fault_seen = false;
static volatile uint32_t g_fault_mcause = 0u;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
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
  LOG_INFO(
      "Verifying [ast_alert.sv:44-75] (SPEC_DOC_ERRATA): ast_alert.sv "
      "persistent ALERT_TRIG latch across 1->0 pulse & continuous re-latch...");

  // Disable ALERT_EN_0 (kMultiBitBool4False = 0x9) and clear RECOV_ALERT.
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_ALERT_EN_0_REG_OFFSET,
                   kMultiBitBool4False);
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET,
                   0x7ffu);
  CHECK(abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET) ==
        0u);

  // Pulse ALERT_TRIG[0] = 1 then 0 while ALERT_EN_0 is False.
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_ALERT_TRIG_REG_OFFSET, 1u);
  busy_spin_micros(5);
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_ALERT_TRIG_REG_OFFSET, 0u);
  busy_spin_micros(5);
  CHECK((abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET) &
         1u) == 0u);

  // Re-enable ALERT_EN_0 (kMultiBitBool4True = 0x6): latched p_alert in
  // ast_alert.sv immediately fires RECOV_ALERT[0] = 1 even with ALERT_TRIG=0!
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_ALERT_EN_0_REG_OFFSET,
                   kMultiBitBool4True);
  busy_spin_micros(5);
  CHECK((abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET) &
         1u) == 1u);

  // RW1C clear RECOV_ALERT[0] now that event_clr[0] acknowledged ast_alert.sv.
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET, 1u);
  busy_spin_micros(5);
  CHECK((abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET) &
         1u) == 0u);

  // Continuous ALERT_TRIG[0] = 1 prevents RW1C clear on RECOV_ALERT[0] from
  // clearing p_alert (re-latches bit 0) until ALERT_TRIG[0] is cleared to 0.
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_ALERT_TRIG_REG_OFFSET, 1u);
  busy_spin_micros(5);
  CHECK((abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET) &
         1u) == 1u);
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET, 1u);
  busy_spin_micros(5);
  CHECK((abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET) &
         1u) == 1u);
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_ALERT_TRIG_REG_OFFSET, 0u);
  busy_spin_micros(5);
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET, 1u);
  busy_spin_micros(5);
  CHECK((abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET) &
         1u) == 0u);

  // Verify CFG_REGWEN (rw0c) locks both ALERT_EN_0..10 and FATAL_ALERT_EN.
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
      "Verifying [sensor_ctrl_reg_pkg.sv:254-284] "
      "(INTENDED_SECURITY_HARDENING): "
      "SENSOR_CTRL_PERMIT 4'b0011 8-bit sb faults & 0x74..0x7f addrmiss...");

  // 32-bit write to RO FATAL_ALERT (0x4c) succeeds with d_error=0 and is
  // ignored, whereas 8-bit sb write to FATAL_ALERT (PERMIT = 4'b0011) raises
  // Store Access Fault (mcause = 7)!
  g_fault_seen = false;
  abs_mmio_write32(kSensorCtrlBase + SENSOR_CTRL_FATAL_ALERT_REG_OFFSET,
                   0x7ffu);
  CHECK(!g_fault_seen &&
        abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_FATAL_ALERT_REG_OFFSET) ==
            0u);

  g_fault_seen = false;
  abs_mmio_write8(kSensorCtrlBase + SENSOR_CTRL_FATAL_ALERT_REG_OFFSET, 1u);
  CHECK(g_fault_seen && g_fault_mcause == kIbexExcStoreAccessFault);

  g_fault_seen = false;
  abs_mmio_write8(kSensorCtrlBase + SENSOR_CTRL_ALERT_TRIG_REG_OFFSET, 1u);
  CHECK(g_fault_seen && g_fault_mcause == kIbexExcStoreAccessFault);

  g_fault_seen = false;
  abs_mmio_write8(kSensorCtrlBase + SENSOR_CTRL_RECOV_ALERT_REG_OFFSET, 1u);
  CHECK(g_fault_seen && g_fault_mcause == kIbexExcStoreAccessFault);

  // Unmapped offsets 0x74..0x7c raise addrmiss Load/Store Access Faults.
  g_fault_seen = false;
  (void)abs_mmio_read32(kSensorCtrlBase + 0x74u);
  CHECK(g_fault_seen && g_fault_mcause == kIbexExcLoadAccessFault);

  g_fault_seen = false;
  abs_mmio_write32(kSensorCtrlBase + 0x7cu, 0x1u);
  CHECK(g_fault_seen && g_fault_mcause == kIbexExcStoreAccessFault);

  LOG_INFO("All [ast_alert.sv:44-75..002] checks confirmed!");
  return true;
}
