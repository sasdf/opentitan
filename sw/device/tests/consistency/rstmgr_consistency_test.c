// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file rstmgr_errata_v2_test.c
 * @brief Physical CW340 FPGA verification of Earlgrey v2 (`trunk-v2`) RSTMGR
 *        hardware and DIF behaviors across `rstmgr.sv`, `rstmgr_crash_info.sv`,
 *        `rstmgr_reg_pkg.sv`, `rstmgr_reg_top.sv`, and `dif_rstmgr.c`.
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/dif/dif_rstmgr.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top/rstmgr_regs.h"
#include "hw/top/usbdev_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG(.ignore_alerts = true);

enum {
  kRstmgrBase = TOP_EARLGREY_RSTMGR_BASE_ADDR,
  kUsbdevBase = TOP_EARLGREY_USBDEV_BASE_ADDR,
};

static volatile uint32_t g_fault_count = 0;
static volatile uint32_t g_last_mcause = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = 0;
  CSR_READ(CSR_REG_MCAUSE, &mcause);
  g_last_mcause = mcause;
  g_fault_count++;
}

/**
 * [rstmgr.sv:1196]:
 * Writing RESET_REQ = kMultiBitBool4True (0x6) and immediately writing
 * kMultiBitBool4False (0x9) before the 200 kHz AON clock (5 us) samples
 * sw_rst_req_o cancels the pending software reset in-flight.
 */
static void test_v1_003_reset_req_in_flight_cancel(void) {
  LOG_INFO("Testing [rstmgr.sv:1196] RESET_REQ in-flight cancel...");
  // Synchronize to a 200 kHz AON clock (clk_slow_i, 5 us period) rising edge
  // via PWSTMGR_CFG_CDC_SYNC (0x40400018) so that the back-to-back RESET_REQ
  // writes (0x6 -> 0x9) always execute at the start of a 5 us AON clock period
  // regardless of binary code alignment.
  const uint32_t kPwrmgrCfgCdcSync = 0x40400018u;
  abs_mmio_write32(kPwrmgrCfgCdcSync, 1u);
  while (abs_mmio_read32(kPwrmgrCfgCdcSync) != 0u) {
  }
  abs_mmio_write32(kRstmgrBase + RSTMGR_RESET_REQ_REG_OFFSET,
                   kMultiBitBool4True);
  abs_mmio_write32(kRstmgrBase + RSTMGR_RESET_REQ_REG_OFFSET,
                   kMultiBitBool4False);
  busy_spin_micros(50);
  uint32_t req = abs_mmio_read32(kRstmgrBase + RSTMGR_RESET_REQ_REG_OFFSET);
  CHECK(req == kMultiBitBool4False,
        "Expected RESET_REQ == 0x9 after in-flight cancellation");
}

/**
 * [rstmgr_reg_pkg.sv:256]:
 * 1. Read-only CSRs ALERT_INFO (0x24) and CPU_INFO (0x34) have RSTMGR_PERMIT =
 *    4'b1111, so 32-bit SW writes are silently ignored (no fault), whereas
 *    sub-word SB/SH writes fault with d_error = 1 (mcause = 7).
 * 2. Unmapped offsets 0x70..0x7F within BlockAw = 7 (0x80) assert addrmiss = 1
 *    (d_error = 1, mcause = 5 on LW and mcause = 7 on SW).
 */
static void test_v1_005_permit_subword_and_addrmiss_faults(void) {
  LOG_INFO("Testing [rstmgr_reg_pkg.sv:256] RSTMGR_PERMIT & addrmiss...");

  // 1. 32-bit write to RO ALERT_INFO (0x24) and CPU_INFO (0x34): NO fault.
  g_fault_count = 0;
  abs_mmio_write32(kRstmgrBase + RSTMGR_ALERT_INFO_REG_OFFSET, 0xdeadbeefu);
  abs_mmio_write32(kRstmgrBase + RSTMGR_CPU_INFO_REG_OFFSET, 0xdeadbeefu);
  CHECK(g_fault_count == 0u,
        "Expected 32-bit writes to RO ALERT_INFO/CPU_INFO to be silently "
        "ignored without bus fault");

  // 2. 8-bit write (SB) to RO ALERT_INFO (0x24) and CPU_INFO (0x34): faults!
  g_fault_count = 0;
  abs_mmio_write8(kRstmgrBase + RSTMGR_ALERT_INFO_REG_OFFSET, 0xaau);
  CHECK(g_fault_count == 1u && g_last_mcause == 7u,
        "Expected SB to ALERT_INFO to fault with mcause=7");

  g_fault_count = 0;
  abs_mmio_write8(kRstmgrBase + RSTMGR_CPU_INFO_REG_OFFSET, 0x55u);
  CHECK(g_fault_count == 1u && g_last_mcause == 7u,
        "Expected SB to CPU_INFO to fault with mcause=7");

  // 2b. 16-bit write (SH) to RO ALERT_INFO (0x24) and CPU_INFO (0x34): faults!
  g_fault_count = 0;
  *(volatile uint16_t *)(kRstmgrBase + RSTMGR_ALERT_INFO_REG_OFFSET) = 0x1234u;
  CHECK(g_fault_count == 1u && g_last_mcause == 7u,
        "Expected SH to ALERT_INFO to fault with mcause=7");

  g_fault_count = 0;
  *(volatile uint16_t *)(kRstmgrBase + RSTMGR_CPU_INFO_REG_OFFSET) = 0x5678u;
  CHECK(g_fault_count == 1u && g_last_mcause == 7u,
        "Expected SH to CPU_INFO to fault with mcause=7");

  // 2c. 8-bit write (SB) to ALERT_INFO_CTRL (0x1c, PERMIT=4'b0001): succeeds!
  g_fault_count = 0;
  abs_mmio_write8(kRstmgrBase + RSTMGR_ALERT_INFO_CTRL_REG_OFFSET, 0x20u);
  CHECK(g_fault_count == 0u,
        "Expected SB to ALERT_INFO_CTRL (PERMIT=4'b0001) to succeed");

  // [prim_reg_cdc_arb.sv:94-95]: Verify SW_RST_CTRL_N[3] (USB, 0x54) vs
  // SW_RST_CTRL_N[4] (USB_AON, 0x58) on USBDEV.WAKE_EVENTS (0x94).
  // 1. Pulse both SW_RST_CTRL_N[3] and SW_RST_CTRL_N[4] to start clean, then
  //    write USBDEV.WAKE_CONTROL.suspend_req = 1 and wait 50 us so the AON
  //    domain sets dst_qs_o = 1 and transfers WAKE_EVENTS.module_active = 1.
  abs_mmio_write32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_3_REG_OFFSET, 0u);
  abs_mmio_write32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_4_REG_OFFSET, 0u);
  busy_spin_micros(20);
  abs_mmio_write32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_3_REG_OFFSET, 1u);
  abs_mmio_write32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_4_REG_OFFSET, 1u);
  busy_spin_micros(20);

  abs_mmio_write32(kUsbdevBase + USBDEV_WAKE_CONTROL_REG_OFFSET,
                   1u << USBDEV_WAKE_CONTROL_SUSPEND_REQ_BIT);
  busy_spin_micros(50);
  uint32_t wake_ev_init =
      abs_mmio_read32(kUsbdevBase + USBDEV_WAKE_EVENTS_REG_OFFSET);
  CHECK((wake_ev_init & (1u << USBDEV_WAKE_EVENTS_MODULE_ACTIVE_BIT)) != 0u,
        "Expected USBDEV.WAKE_EVENTS.module_active == 1 after SUSPEND_REQ, "
        "got 0x%x",
        wake_ev_init);

  // 2. Pulse ONLY SW_RST_CTRL_N[3] (USB, 0x54) without pulsing SW_RST_CTRL_N[4]
  //    (USB_AON, 0x58): src_q (WAKE_EVENTS) resets to 0, while dst_qs_o in AON
  //    remains 1 (== dst_ds_i), so re-issuing SUSPEND_REQ fails to re-latch
  //    WAKE_EVENTS!
  abs_mmio_write32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_3_REG_OFFSET, 0u);
  busy_spin_micros(20);
  abs_mmio_write32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_3_REG_OFFSET, 1u);
  busy_spin_micros(20);
  uint32_t wake_ev_usb_only =
      abs_mmio_read32(kUsbdevBase + USBDEV_WAKE_EVENTS_REG_OFFSET);
  CHECK(wake_ev_usb_only == 0u,
        "Expected USBDEV.WAKE_EVENTS == 0 after pulsing SW_RST_CTRL_N[3] "
        "alone, got 0x%x",
        wake_ev_usb_only);
  abs_mmio_write32(kUsbdevBase + USBDEV_WAKE_CONTROL_REG_OFFSET,
                   1u << USBDEV_WAKE_CONTROL_SUSPEND_REQ_BIT);
  busy_spin_micros(50);
  wake_ev_usb_only =
      abs_mmio_read32(kUsbdevBase + USBDEV_WAKE_EVENTS_REG_OFFSET);
  CHECK(wake_ev_usb_only == 0u,
        "Expected USBDEV.WAKE_EVENTS to remain stuck at 0 when SW_RST_CTRL_N[4]"
        " (USB_AON) was not pulsed (dst_qs_o == dst_ds_i), got 0x%x",
        wake_ev_usb_only);

  // 3. Now pulse BOTH SW_RST_CTRL_N[3] and SW_RST_CTRL_N[4] (USB_AON, 0x58):
  //    dst_qs_o resets to 0, so SUSPEND_REQ causes (dst_qs_o != dst_ds_i) and
  //    re-latches WAKE_EVENTS.module_active == 1!
  abs_mmio_write32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_3_REG_OFFSET, 0u);
  abs_mmio_write32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_4_REG_OFFSET, 0u);
  busy_spin_micros(20);
  abs_mmio_write32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_3_REG_OFFSET, 1u);
  abs_mmio_write32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_4_REG_OFFSET, 1u);
  busy_spin_micros(20);
  abs_mmio_write32(kUsbdevBase + USBDEV_WAKE_CONTROL_REG_OFFSET,
                   1u << USBDEV_WAKE_CONTROL_SUSPEND_REQ_BIT);
  busy_spin_micros(50);
  uint32_t wake_ev_both =
      abs_mmio_read32(kUsbdevBase + USBDEV_WAKE_EVENTS_REG_OFFSET);
  CHECK((wake_ev_both & (1u << USBDEV_WAKE_EVENTS_MODULE_ACTIVE_BIT)) != 0u,
        "Expected USBDEV.WAKE_EVENTS.module_active == 1 after pulsing both "
        "SW_RST_CTRL_N[3..4], got 0x%x",
        wake_ev_both);

  // Clean up USBDEV state by pulsing both SW_RST_CTRL_N[3..4].
  abs_mmio_write32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_3_REG_OFFSET, 0u);
  abs_mmio_write32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_4_REG_OFFSET, 0u);
  busy_spin_micros(20);
  abs_mmio_write32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_3_REG_OFFSET, 1u);
  abs_mmio_write32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_4_REG_OFFSET, 1u);
  CHECK(
      abs_mmio_read32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_3_REG_OFFSET) == 1u &&
          abs_mmio_read32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_4_REG_OFFSET) ==
              1u,
      "Expected SW_RST_CTRL_N[3..4] == 1 after pulse");

  // 3. Unmapped offset 0x70 within BlockAw=7 (0x80) window: addrmiss ->
  // d_error.
  g_fault_count = 0;
  (void)abs_mmio_read32(kRstmgrBase + 0x70u);
  CHECK(g_fault_count == 1u && g_last_mcause == 5u,
        "Expected LW from unmapped 0x70 to fault with mcause=5");

  g_fault_count = 0;
  abs_mmio_write32(kRstmgrBase + 0x70u, 0x12345678u);
  CHECK(g_fault_count == 1u && g_last_mcause == 7u,
        "Expected SW to unmapped 0x70 to fault with mcause=7");
}

/**
 * Post-SW_RESET verification of:
 * - [rstmgr.sv:1224]: ALERT_INFO_CTRL.INDEX (5) and CPU_INFO_CTRL.INDEX (3)
 *   persist across SW_RESET (clocked on rst_por_ni with .de(1'b0)), while EN
 *   is cleared to 0.
 * - [rstmgr_crash_info.sv:45]: ALERT_INFO out-of-bounds INDEX = 9..15 returns
 *   0x00000000 (SlotCntWidth=4), whereas CPU_INFO out-of-bounds INDEX = 8..15
 *   aliases slots[0..7] (SlotCntWidth=3).
 * - [rstmgr_reg_top.sv:430,551]: Locking CPU_REGWEN = 0 (or ALERT_REGWEN = 0)
 *   gates CPU_INFO_CTRL.INDEX (and ALERT_INFO_CTRL.INDEX) writes in hardware,
 *   yet dif_rstmgr_cpu_info_dump_read() and dif_rstmgr_alert_info_dump_read()
 *   fail to check cpu_capture_is_locked() / alert_capture_is_locked(),
 *   returning kDifOk while filling all 8 (or 9) output segments with identical
 *   copies of the frozen slot[INDEX]!
 */
static void verify_post_sw_reset_errata(void) {
  LOG_INFO(
      "Verifying post-SW_RESET [rstmgr.sv:1224], [rstmgr_crash_info.sv:45], "
      "and [rstmgr_reg_top.sv:430,551]...");

  // 1. [rstmgr.sv:1224]: Check that EN cleared to 0 while INDEX survived!
  uint32_t alert_ctrl =
      abs_mmio_read32(kRstmgrBase + RSTMGR_ALERT_INFO_CTRL_REG_OFFSET);
  uint32_t cpu_ctrl =
      abs_mmio_read32(kRstmgrBase + RSTMGR_CPU_INFO_CTRL_REG_OFFSET);
  LOG_INFO("  Post-SW_RESET ALERT_INFO_CTRL=0x%x, CPU_INFO_CTRL=0x%x",
           alert_ctrl, cpu_ctrl);

  CHECK(alert_ctrl == 0x50u,
        "Expected ALERT_INFO_CTRL == 0x50 (EN=0, INDEX=5) after SW_RESET, got "
        "0x%x",
        alert_ctrl);
  CHECK(cpu_ctrl == 0x30u,
        "Expected CPU_INFO_CTRL == 0x30 (EN=0, INDEX=3) after SW_RESET, got "
        "0x%x",
        cpu_ctrl);
  CHECK(bitfield_bit32_read(alert_ctrl, RSTMGR_ALERT_INFO_CTRL_EN_BIT) == 0u,
        "Expected ALERT_INFO_CTRL.EN == 0 after SW_RESET");
  CHECK(bitfield_field32_read(alert_ctrl, RSTMGR_ALERT_INFO_CTRL_INDEX_FIELD) ==
            5u,
        "Expected ALERT_INFO_CTRL.INDEX == 5 to survive SW_RESET");
  CHECK(bitfield_bit32_read(cpu_ctrl, RSTMGR_CPU_INFO_CTRL_EN_BIT) == 0u,
        "Expected CPU_INFO_CTRL.EN == 0 after SW_RESET");
  CHECK(bitfield_field32_read(cpu_ctrl, RSTMGR_CPU_INFO_CTRL_INDEX_FIELD) == 3u,
        "Expected CPU_INFO_CTRL.INDEX == 3 to survive SW_RESET");

  // 2. [rstmgr_crash_info.sv:45]: ALERT_INFO (CNT_AVAIL = 9) returns 0 for
  // INDEX 9..15.
  uint32_t alert_cnt =
      abs_mmio_read32(kRstmgrBase + RSTMGR_ALERT_INFO_ATTR_REG_OFFSET);
  CHECK(alert_cnt == 9u, "Expected ALERT_INFO_ATTR.CNT_AVAIL == 9");
  for (uint32_t idx = 9u; idx < 16u; ++idx) {
    abs_mmio_write32(kRstmgrBase + RSTMGR_ALERT_INFO_CTRL_REG_OFFSET, idx << 4);
    uint32_t val = abs_mmio_read32(kRstmgrBase + RSTMGR_ALERT_INFO_REG_OFFSET);
    CHECK(val == 0u, "Expected ALERT_INFO[INDEX=%u] == 0", idx);
  }

  // 3. [rstmgr_crash_info.sv:45]: CPU_INFO (CNT_AVAIL = 8) aliases slots[0..7]
  // for INDEX 8..15.
  uint32_t cpu_cnt =
      abs_mmio_read32(kRstmgrBase + RSTMGR_CPU_INFO_ATTR_REG_OFFSET);
  CHECK(cpu_cnt == 8u, "Expected CPU_INFO_ATTR.CNT_AVAIL == 8");
  uint32_t cpu_slots[8];
  uint32_t any_nonzero = 0u;
  for (uint32_t idx = 0u; idx < 8u; ++idx) {
    abs_mmio_write32(kRstmgrBase + RSTMGR_CPU_INFO_CTRL_REG_OFFSET, idx << 4);
    cpu_slots[idx] = abs_mmio_read32(kRstmgrBase + RSTMGR_CPU_INFO_REG_OFFSET);
    any_nonzero |= cpu_slots[idx];
  }
  CHECK(any_nonzero != 0u,
        "Expected captured CPU_INFO crash dump to contain non-zero PC/state");

  for (uint32_t idx = 8u; idx < 16u; ++idx) {
    abs_mmio_write32(kRstmgrBase + RSTMGR_CPU_INFO_CTRL_REG_OFFSET, idx << 4);
    uint32_t aliased =
        abs_mmio_read32(kRstmgrBase + RSTMGR_CPU_INFO_REG_OFFSET);
    CHECK(
        aliased == cpu_slots[idx & 0x7u],
        "Expected CPU_INFO[INDEX=%u] (0x%x) to alias CPU_INFO[INDEX=%u] (0x%x)",
        idx, aliased, idx & 0x7u, cpu_slots[idx & 0x7u]);
  }
  LOG_INFO(
      "  [rstmgr_crash_info.sv:45] CPU_INFO[0]=0x%x == CPU_INFO[8]=0x%x "
      "(modulo-8 alias verified)",
      cpu_slots[0], cpu_slots[0]);

  // 4. [rstmgr_reg_top.sv:430,551]:
  // Find a slot k_diff (1..7) where cpu_slots[k_diff] != cpu_slots[0].
  uint32_t k_diff = 0u;
  for (uint32_t k = 1u; k < 8u; ++k) {
    if (cpu_slots[k] != cpu_slots[0]) {
      k_diff = k;
      break;
    }
  }
  CHECK(k_diff != 0u, "Expected at least one CPU_INFO slot != slot[0]");

  // Set CPU_INFO_CTRL.INDEX = k_diff, then lock CPU_REGWEN = 0.
  abs_mmio_write32(kRstmgrBase + RSTMGR_CPU_INFO_CTRL_REG_OFFSET, k_diff << 4);
  abs_mmio_write32(kRstmgrBase + RSTMGR_CPU_REGWEN_REG_OFFSET, 0u);

  dif_rstmgr_t rstmgr;
  CHECK_DIF_OK(dif_rstmgr_init(mmio_region_from_addr(kRstmgrBase), &rstmgr));

  // Verify dif_rstmgr_cpu_info_set_enabled returns kDifLocked when
  // CPU_REGWEN==0:
  CHECK(
      dif_rstmgr_cpu_info_set_enabled(&rstmgr, kDifToggleEnabled) == kDifLocked,
      "Expected dif_rstmgr_cpu_info_set_enabled to return kDifLocked");

  // Yet dif_rstmgr_cpu_info_dump_read() returns kDifOk and silently fills all 8
  // entries of dump[0..7] with the single frozen word cpu_slots[k_diff]!
  dif_rstmgr_cpu_info_dump_segment_t dump[DIF_RSTMGR_CPU_INFO_MAX_SIZE] = {0};
  size_t segments_read = 0;
  CHECK_DIF_OK(dif_rstmgr_cpu_info_dump_read(
      &rstmgr, dump, DIF_RSTMGR_CPU_INFO_MAX_SIZE, &segments_read));
  CHECK(segments_read == 8u, "Expected segments_read == 8");
  for (size_t i = 0; i < 8u; ++i) {
    CHECK(dump[i] == cpu_slots[k_diff],
          "Expected dump[%u] (0x%x) to repeat frozen cpu_slots[%u] (0x%x)",
          (uint32_t)i, dump[i], k_diff, cpu_slots[k_diff]);
  }
  CHECK(dump[0] != cpu_slots[0],
        "Expected dump[0] (0x%x) != true cpu_slots[0] (0x%x) due to frozen "
        "CPU_INFO_CTRL.INDEX",
        dump[0], cpu_slots[0]);

  // Also verify ALERT_REGWEN=0 (0x18) locks ALERT_INFO_CTRL.INDEX at 5 while
  // dif_rstmgr_alert_info_dump_read() returns kDifOk (segments_read == 9) and
  // repeats alert_slot_5 across all 9 entries:
  abs_mmio_write32(kRstmgrBase + RSTMGR_ALERT_INFO_CTRL_REG_OFFSET, 5u << 4);
  uint32_t alert_slot_5 =
      abs_mmio_read32(kRstmgrBase + RSTMGR_ALERT_INFO_REG_OFFSET);
  abs_mmio_write32(kRstmgrBase + RSTMGR_ALERT_REGWEN_REG_OFFSET, 0u);
  CHECK(dif_rstmgr_alert_info_set_enabled(&rstmgr, kDifToggleEnabled) ==
            kDifLocked,
        "Expected dif_rstmgr_alert_info_set_enabled to return kDifLocked");
  dif_rstmgr_alert_info_dump_segment_t
      alert_dump[DIF_RSTMGR_ALERT_INFO_MAX_SIZE] = {0};
  size_t alert_segments_read = 0;
  CHECK_DIF_OK(dif_rstmgr_alert_info_dump_read(&rstmgr, alert_dump,
                                               DIF_RSTMGR_ALERT_INFO_MAX_SIZE,
                                               &alert_segments_read));
  CHECK(alert_segments_read == 9u, "Expected alert_segments_read == 9");
  for (size_t i = 0; i < 9u; ++i) {
    CHECK(alert_dump[i] == alert_slot_5,
          "Expected alert_dump[%u] (0x%08x) to repeat frozen alert_slot_5 "
          "(0x%08x)",
          (uint32_t)i, alert_dump[i], alert_slot_5);
  }
  CHECK(abs_mmio_read32(kRstmgrBase + RSTMGR_ALERT_INFO_CTRL_REG_OFFSET) ==
            (5u << 4),
        "Expected ALERT_INFO_CTRL.INDEX frozen at 5 after dump_read");
  LOG_INFO(
      "  [rstmgr_reg_top.sv:430,551] Locked CPU_REGWEN=0 at INDEX=%u -> "
      "dif_rstmgr_cpu_info_dump_read returned kDifOk with all 8 words == 0x%x "
      "(true slot[0]=0x%x)",
      k_diff, dump[0], cpu_slots[0]);
}

bool test_main(void) {
  LOG_INFO("=== RSTMGR Earlgrey v2 (trunk-v2) Errata Verification Test ===");
  uint32_t reset_info =
      abs_mmio_read32(kRstmgrBase + RSTMGR_RESET_INFO_REG_OFFSET) |
      abs_mmio_read32(TOP_EARLGREY_SRAM_CTRL_RET_RAM_BASE_ADDR + 4u);
  LOG_INFO("  Boot RESET_INFO = 0x%x", reset_info);

  if ((reset_info & (1u << RSTMGR_RESET_INFO_SW_RESET_BIT)) == 0u) {
    test_v1_003_reset_req_in_flight_cancel();
    test_v1_005_permit_subword_and_addrmiss_faults();

    // Arm ALERT_INFO_CTRL (EN=1, INDEX=5) and CPU_INFO_CTRL (EN=1, INDEX=3)
    // and trigger SW_RESET.
    abs_mmio_write32(kRstmgrBase + RSTMGR_ALERT_INFO_CTRL_REG_OFFSET,
                     (5u << 4) | 1u);
    abs_mmio_write32(kRstmgrBase + RSTMGR_CPU_INFO_CTRL_REG_OFFSET,
                     (3u << 4) | 1u);
    LOG_INFO(
        "Triggering SW_RESET to capture crash dump and test warm reset "
        "persistence...");
    abs_mmio_write32(kRstmgrBase + RSTMGR_RESET_REQ_REG_OFFSET,
                     kMultiBitBool4True);
    wait_for_interrupt();
    CHECK(false, "Unreachable: SW_RESET did not fire");
  }

  // Clear SW_RESET bit in RESET_INFO (rw1c) and retention SRAM, then verify
  // post-reset errata.
  abs_mmio_write32(kRstmgrBase + RSTMGR_RESET_INFO_REG_OFFSET, reset_info);
  abs_mmio_write32(TOP_EARLGREY_SRAM_CTRL_RET_RAM_BASE_ADDR + 4u, 0u);
  verify_post_sw_reset_errata();
  LOG_INFO(
      "=== ALL RSTMGR Earlgrey v2 Errata Checks PASSED on CW340 FPGA! ===");
  return true;
}
