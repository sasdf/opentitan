// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * CW340 FPGA & QEMU Empirical Errata Confirmation Test for `rstmgr` (P22).
 *
 * Exercises and confirms all 5 silicon / spec / architectural behaviors in
 * `/root/knowledge/errata/rstmgr.md`:
 * - [rstmgr.sv:1187-1257]: `ALERT_INFO_CTRL.INDEX` and `CPU_INFO_CTRL.INDEX`
 *   (clocked on `rst_por_ni` with `.de(1'b0)`) retain their pre-reset values
 *   across non-POR `SW_RESET` reboots, while `.EN` is cleared to `0` by
 *   `dump_capture_halt = rst_hw_req`.
 * - [rstmgr_crash_info.sv:13-52]: Out-of-bounds `ALERT_INFO` reads (`INDEX
 * = 9..15`) return `0x00000000` (`CrashStoreSlot = 9`, `SlotCntWidth = 4`),
 * whereas out-of-bounds `CPU_INFO` reads (`INDEX = 8..15`) wrap around and
 * alias `slots[0..7]` because `CrashStoreSlot = 8` (`SlotCntWidth = 3 <
 * IdxWidth`) ties off `slot_sel_i[3]` (`rstmgr_crash_info.sv:15-52`).
 * - [rstmgr.sv:1193]: `RESET_REQ = kMultiBitBool4True (0x6)` can be
 *   cancelled in-flight by writing `kMultiBitBool4False (0x9)` before the
 *   200 kHz AON clock (`clk_slow_i`, 5 us period) synchronizer samples it.
 * - [top_earlgrey.sv:1698,1706-1707]: Pulsing `SW_RST_CTRL_N[3]` (`USB`) clears
 *   `USBDEV.WAKE_EVENTS` (`src_q = 0`) and leaves it stuck at `0` because
 *   `dst_qs_o` in `USB_AON` did not change (`prim_reg_cdc_arb.sv:99-100`);
 *   pulsing `SW_RST_CTRL_N[4]` (`USB_AON`) restores `USBDEV.WAKE_EVENTS`.
 * - [rstmgr_reg_pkg.sv:247-276]: `ALERT_INFO` and `CPU_INFO` (`RSTMGR_PERMIT =
 * 4'b1111`) silently ignore 32-bit `sw` writes (`d_error = 0`) while faulting
 * (`mcause = 7`) on sub-word `sb`/`sh` writes, and offsets `0x70..0x7F` (`AW =
 * 7`) fault with `addrmiss = 1` (`mcause = 5 / 7`).
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/dif/dif_pinmux.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"
#include "sw/device/silicon_creator/lib/drivers/retention_sram.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "rstmgr_regs.h"
#include "usbdev_regs.h"

OTTF_DEFINE_TEST_CONFIG();

#define CHECK_EQ(a, b, ...) CHECK((a) == (b), __VA_ARGS__)

enum {
  kRstmgrBase = TOP_EARLGREY_RSTMGR_AON_BASE_ADDR,
  kUsbdevBase = TOP_EARLGREY_USBDEV_BASE_ADDR,
  kPinmuxBase = TOP_EARLGREY_PINMUX_AON_BASE_ADDR,
  kRiscvLoadAccessFault = 5,
  kRiscvStoreAccessFault = 7,
  kTestAlertIndex = 5u,
  kTestCpuIndex = 6u,
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

static void test_rstmgr_reset_req_in_flight_cancel(void) {
  LOG_INFO(
      "Testing [rstmgr.sv:1193]: RESET_REQ in-flight cancellation before "
      "clk_slow_i");

  // Write kMultiBitBool4True (0x6) to RESET_REQ and immediately cancel with
  // kMultiBitBool4False (0x9) before the 200 kHz AON clock (5 us period)
  // synchronizer samples it.
  abs_mmio_write32(kRstmgrBase + RSTMGR_RESET_REQ_REG_OFFSET,
                   kMultiBitBool4True);
  abs_mmio_write32(kRstmgrBase + RSTMGR_RESET_REQ_REG_OFFSET,
                   kMultiBitBool4False);

  // Spin 30 us (> 5 AON clock cycles) and verify no system reset occurred.
  busy_spin_micros(30);
  uint32_t reset_req =
      abs_mmio_read32(kRstmgrBase + RSTMGR_RESET_REQ_REG_OFFSET);
  CHECK_EQ(reset_req, (uint32_t)kMultiBitBool4False,
           "[rstmgr.sv:1193] Expected RESET_REQ == kMultiBitBool4False (0x9)");
}

static void test_rstmgr_usb_vs_usb_aon_sw_reset_cdc_quirk(void) {
  LOG_INFO(
      "Testing [top_earlgrey.sv:1698,1706-1707]: SW_RST_CTRL_N[3] (USB) vs "
      "SW_RST_CTRL_N[4] "
      "(USB_AON) CDC quirk");

  // Connect UsbdevSense to ConstantOne so VBUS is sensed.
  dif_pinmux_t pinmux;
  CHECK_DIF_OK(dif_pinmux_init(mmio_region_from_addr(kPinmuxBase), &pinmux));
  CHECK_DIF_OK(dif_pinmux_input_select(
      &pinmux, kTopEarlgreyPinmuxPeripheralInUsbdevSense,
      kTopEarlgreyPinmuxInselConstantOne));
  busy_spin_micros(20);

  // Activate AON wake monitoring so USBDEV.WAKE_EVENTS latches MODULE_ACTIVE
  // = 1.
  abs_mmio_write32(kUsbdevBase + USBDEV_WAKE_CONTROL_REG_OFFSET,
                   1u << USBDEV_WAKE_CONTROL_SUSPEND_REQ_BIT);
  busy_spin_micros(60);
  uint32_t wake_events =
      abs_mmio_read32(kUsbdevBase + USBDEV_WAKE_EVENTS_REG_OFFSET);
  CHECK((wake_events & (1u << USBDEV_WAKE_EVENTS_MODULE_ACTIVE_BIT)) != 0u,
        "Expected USBDEV.WAKE_EVENTS.MODULE_ACTIVE == 1 before USB reset");

  // 1. Pulse SW_RST_CTRL_N_3 (USB) alone (0 -> 1): resets
  // u_wake_events_cdc.src_q to 0 while dst_qs_o (on rst_usb_aon_n) remains
  // unchanged, leaving USBDEV.WAKE_EVENTS stuck at 0.
  abs_mmio_write32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_3_REG_OFFSET, 0u);
  busy_spin_micros(10);
  abs_mmio_write32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_3_REG_OFFSET, 1u);
  busy_spin_micros(60);
  wake_events = abs_mmio_read32(kUsbdevBase + USBDEV_WAKE_EVENTS_REG_OFFSET);
  CHECK_EQ(wake_events, 0u,
           "[top_earlgrey.sv:1698,1706-1707] Expected USBDEV.WAKE_EVENTS == 0 "
           "after pulsing "
           "only SW_RST_CTRL_N_3 (USB)");

  // 2. Pulse SW_RST_CTRL_N_4 (USB_AON) (0 -> 1): resets dst_qs_o to 0,
  // triggering `dst_qs_o != dst_ds_i` in prim_reg_cdc_arb.sv and restoring
  // USBDEV.WAKE_EVENTS (MODULE_ACTIVE == 1).
  abs_mmio_write32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_4_REG_OFFSET, 0u);
  busy_spin_micros(10);
  abs_mmio_write32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_4_REG_OFFSET, 1u);
  busy_spin_micros(60);
  wake_events = abs_mmio_read32(kUsbdevBase + USBDEV_WAKE_EVENTS_REG_OFFSET);
  CHECK((wake_events & (1u << USBDEV_WAKE_EVENTS_MODULE_ACTIVE_BIT)) != 0u,
        "[top_earlgrey.sv:1698,1706-1707] Expected "
        "USBDEV.WAKE_EVENTS.MODULE_ACTIVE "
        "restored after pulsing SW_RST_CTRL_N_4 (USB_AON)");

  // Acknowledge wake to restore idle state.
  abs_mmio_write32(kUsbdevBase + USBDEV_WAKE_CONTROL_REG_OFFSET,
                   1u << USBDEV_WAKE_CONTROL_WAKE_ACK_BIT);
  busy_spin_micros(40);
}

static void test_rstmgr_permit_subword_and_addrmiss_faults(void) {
  LOG_INFO(
      "Testing [rstmgr_reg_pkg.sv:247-276]: RSTMGR_PERMIT (4'b1111 on "
      "ALERT_INFO/CPU_INFO) & 0x70..0x7F addrmiss");

  // 1. 32-bit sw writes to read-only ALERT_INFO (0x24) and CPU_INFO (0x34) are
  // silently ignored without faulting.
  g_fault_count = 0;
  abs_mmio_write32(kRstmgrBase + RSTMGR_ALERT_INFO_REG_OFFSET, 0xDEADBEEFu);
  abs_mmio_write32(kRstmgrBase + RSTMGR_CPU_INFO_REG_OFFSET, 0xCAFEBABEu);
  CHECK_EQ(g_fault_count, 0u,
           "32-bit writes to read-only ALERT_INFO / CPU_INFO must not fault");

  // 2. Sub-word sb/sh writes to ALERT_INFO (4'b1111) and CPU_INFO (4'b1111)
  // trap with synchronous Store Access Fault (mcause = 7).
  g_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write8(kRstmgrBase + RSTMGR_ALERT_INFO_REG_OFFSET, 0xAAu);
  CHECK_EQ(g_fault_count, 1u,
           "[rstmgr_reg_pkg.sv:247-276] Expected sb to ALERT_INFO to trap");
  CHECK_EQ(g_last_mcause, (uint32_t)kRiscvStoreAccessFault,
           "Expected mcause=7");

  g_fault_count = 0;
  g_last_mcause = 0;
  *(volatile uint16_t *)(uintptr_t)(kRstmgrBase + RSTMGR_CPU_INFO_REG_OFFSET) =
      0xBBCCu;
  CHECK_EQ(g_fault_count, 1u,
           "[rstmgr_reg_pkg.sv:247-276] Expected sh to CPU_INFO to trap");
  CHECK_EQ(g_last_mcause, (uint32_t)kRiscvStoreAccessFault,
           "Expected mcause=7");

  // 3. Sub-word sb write to byte 0 of 1-byte CSR ALERT_INFO_CTRL (4'b0001)
  // succeeds, whereas sb to byte 1 traps with Store Access Fault (mcause = 7).
  g_fault_count = 0;
  abs_mmio_write8(kRstmgrBase + RSTMGR_ALERT_INFO_CTRL_REG_OFFSET, 0x00u);
  CHECK_EQ(g_fault_count, 0u,
           "Byte 0 sb to ALERT_INFO_CTRL (4'b0001) must not fault");

  g_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write8(kRstmgrBase + RSTMGR_ALERT_INFO_CTRL_REG_OFFSET + 1u, 0x00u);
  CHECK_EQ(g_fault_count, 1u,
           "[rstmgr_reg_pkg.sv:247-276] Expected byte 1 sb to ALERT_INFO_CTRL "
           "to trap");
  CHECK_EQ(g_last_mcause, (uint32_t)kRiscvStoreAccessFault,
           "Expected mcause=7");

  // 4. Unmapped tail aperture 0x70..0x7F (AW = 7) traps on both load & store.
  g_fault_count = 0;
  g_last_mcause = 0;
  (void)abs_mmio_read32(kRstmgrBase + 0x70u);
  CHECK_EQ(g_fault_count, 1u,
           "[rstmgr_reg_pkg.sv:247-276] Expected read at unmapped offset 0x70 "
           "to trap");
  CHECK_EQ(g_last_mcause, (uint32_t)kRiscvLoadAccessFault, "Expected mcause=5");

  g_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write32(kRstmgrBase + 0x70u, 0x12345678u);
  CHECK_EQ(g_fault_count, 1u,
           "[rstmgr_reg_pkg.sv:247-276] Expected write at unmapped offset 0x70 "
           "to trap");
  CHECK_EQ(g_last_mcause, (uint32_t)kRiscvStoreAccessFault,
           "Expected mcause=7");
}

static void verify_post_sw_reset_regwen_and_crash_info(void) {
  LOG_INFO(
      "Verifying [rstmgr.sv:1187-1257] & [rstmgr_crash_info.sv:13-52] after "
      "SW_RESET");

  // 1. [rstmgr.sv:1187-1257]: Verify ALERT_INFO_CTRL.INDEX (5) and
  // CPU_INFO_CTRL.INDEX (6) persisted across non-POR SW_RESET (clocked on
  // rst_por_ni with .de(1'b0)), while .EN was cleared to 0 by rst_hw_req.
  uint32_t alert_ctrl =
      abs_mmio_read32(kRstmgrBase + RSTMGR_ALERT_INFO_CTRL_REG_OFFSET);
  uint32_t cpu_ctrl =
      abs_mmio_read32(kRstmgrBase + RSTMGR_CPU_INFO_CTRL_REG_OFFSET);
  uint32_t alert_idx = (alert_ctrl >> RSTMGR_ALERT_INFO_CTRL_INDEX_OFFSET) &
                       RSTMGR_ALERT_INFO_CTRL_INDEX_MASK;
  uint32_t alert_en = (alert_ctrl >> RSTMGR_ALERT_INFO_CTRL_EN_BIT) & 1u;
  uint32_t cpu_idx = (cpu_ctrl >> RSTMGR_CPU_INFO_CTRL_INDEX_OFFSET) &
                     RSTMGR_CPU_INFO_CTRL_INDEX_MASK;
  uint32_t cpu_en = (cpu_ctrl >> RSTMGR_CPU_INFO_CTRL_EN_BIT) & 1u;

  CHECK_EQ(alert_idx, kTestAlertIndex,
           "[rstmgr.sv:1187-1257] Expected ALERT_INFO_CTRL.INDEX == %u to "
           "persist across SW_RESET",
           kTestAlertIndex);
  CHECK_EQ(alert_en, 0u,
           "[rstmgr.sv:1187-1257] Expected ALERT_INFO_CTRL.EN cleared to 0 on "
           "SW_RESET");
  CHECK_EQ(
      cpu_idx, kTestCpuIndex,
      "[rstmgr.sv:1187-1257] Expected CPU_INFO_CTRL.INDEX == %u to persist "
      "across SW_RESET",
      kTestCpuIndex);
  CHECK_EQ(cpu_en, 0u,
           "[rstmgr.sv:1187-1257] Expected CPU_INFO_CTRL.EN cleared to 0 on "
           "SW_RESET");

  // 2. [rstmgr_crash_info.sv:13-46]: ALERT_INFO_ATTR == 9, and out-of-bounds
  // INDEX = 9..15 returns 0x00000000.
  uint32_t alert_attr =
      abs_mmio_read32(kRstmgrBase + RSTMGR_ALERT_INFO_ATTR_REG_OFFSET);
  CHECK_EQ(alert_attr, 9u, "Expected ALERT_INFO_ATTR == 9");
  for (uint32_t idx = 9u; idx < 16u; ++idx) {
    abs_mmio_write32(kRstmgrBase + RSTMGR_ALERT_INFO_CTRL_REG_OFFSET,
                     idx << RSTMGR_ALERT_INFO_CTRL_INDEX_OFFSET);
    uint32_t val = abs_mmio_read32(kRstmgrBase + RSTMGR_ALERT_INFO_REG_OFFSET);
    CHECK_EQ(val, 0u,
             "[rstmgr_crash_info.sv:13-46] Expected ALERT_INFO[INDEX=%u] == 0, "
             "got 0x%08x",
             idx, val);
  }

  // 3. [rstmgr_crash_info.sv:15-52]: CPU_INFO_ATTR == 8 (`SlotCntWidth =
  // $clog2(8) = 3`), so out-of-bounds INDEX = 8..15 aliases slots[0..7] (`INDEX
  // & 0x7`)!
  uint32_t cpu_attr =
      abs_mmio_read32(kRstmgrBase + RSTMGR_CPU_INFO_ATTR_REG_OFFSET);
  CHECK_EQ(cpu_attr, 8u, "Expected CPU_INFO_ATTR == 8");

  uint32_t cpu_slots[8];
  uint32_t nonzero_slots = 0;
  for (uint32_t i = 0; i < 8u; ++i) {
    abs_mmio_write32(kRstmgrBase + RSTMGR_CPU_INFO_CTRL_REG_OFFSET,
                     i << RSTMGR_CPU_INFO_CTRL_INDEX_OFFSET);
    cpu_slots[i] = abs_mmio_read32(kRstmgrBase + RSTMGR_CPU_INFO_REG_OFFSET);
    if (cpu_slots[i] != 0u) {
      nonzero_slots++;
    }
  }
  CHECK(nonzero_slots > 0u,
        "Expected captured CPU_INFO crash dump to contain non-zero PC/addr "
        "slots");

  for (uint32_t i = 0; i < 8u; ++i) {
    uint32_t oob_idx = 8u + i;
    abs_mmio_write32(kRstmgrBase + RSTMGR_CPU_INFO_CTRL_REG_OFFSET,
                     oob_idx << RSTMGR_CPU_INFO_CTRL_INDEX_OFFSET);
    uint32_t oob_val =
        abs_mmio_read32(kRstmgrBase + RSTMGR_CPU_INFO_REG_OFFSET);
    CHECK_EQ(
        oob_val, cpu_slots[i],
        "[rstmgr_crash_info.sv:15-52] Expected CPU_INFO[INDEX=%u] (0x%08x) to "
        "alias CPU_INFO[INDEX=%u] (0x%08x)",
        oob_idx, oob_val, i, cpu_slots[i]);
  }
}

bool test_main(void) {
  uint32_t reset_reasons = retention_sram_get()->creator.reset_reasons;
  LOG_INFO("rstmgr_errata_test boot: reset_reasons = 0x%08x", reset_reasons);

  if ((reset_reasons & (1u << RSTMGR_RESET_INFO_SW_RESET_BIT)) == 0u) {
    // Boot 1 (POR): test [rstmgr.sv:1193], [top_earlgrey.sv:1698,1706-1707],
    // and [rstmgr_reg_pkg.sv:247-276], then arm ALERT_INFO_CTRL / CPU_INFO_CTRL
    // with non-zero INDEX and trigger SW_RESET.
    test_rstmgr_reset_req_in_flight_cancel();
    test_rstmgr_usb_vs_usb_aon_sw_reset_cdc_quirk();
    test_rstmgr_permit_subword_and_addrmiss_faults();

    LOG_INFO(
        "Arming ALERT_INFO_CTRL (INDEX=%u, EN=1) & CPU_INFO_CTRL (INDEX=%u, "
        "EN=1) and requesting SW_RESET",
        kTestAlertIndex, kTestCpuIndex);
    abs_mmio_write32(kRstmgrBase + RSTMGR_ALERT_INFO_CTRL_REG_OFFSET,
                     (kTestAlertIndex << RSTMGR_ALERT_INFO_CTRL_INDEX_OFFSET) |
                         (1u << RSTMGR_ALERT_INFO_CTRL_EN_BIT));
    abs_mmio_write32(kRstmgrBase + RSTMGR_CPU_INFO_CTRL_REG_OFFSET,
                     (kTestCpuIndex << RSTMGR_CPU_INFO_CTRL_INDEX_OFFSET) |
                         (1u << RSTMGR_CPU_INFO_CTRL_EN_BIT));
    abs_mmio_write32(kRstmgrBase + RSTMGR_RESET_REQ_REG_OFFSET,
                     kMultiBitBool4True);
    busy_spin_micros(200);
    CHECK(false, "Expected SW_RESET to fire within 200 us");
  }

  // Boot 2 (SW_RESET): verify [rstmgr.sv:1187-1257] and
  // [rstmgr_crash_info.sv:13-52].
  verify_post_sw_reset_regwen_and_crash_info();

  LOG_INFO("All rstmgr errata checks PASSED");
  return true;
}
