// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "clkmgr_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kClkmgrBase = TOP_EARLGREY_CLKMGR_AON_BASE_ADDR,
};

static void write_shadowed(uint32_t offset, uint32_t val) {
  abs_mmio_write32(kClkmgrBase + offset, val);
  abs_mmio_write32(kClkmgrBase + offset, val);
}

bool test_main(void) {
  // 1. Verify ALERT_TEST single-shot pulse for recov_fault.
  CHECK_STATUS_OK(ottf_alerts_expect_alert_start(
      kTopEarlgreyAlertIdClkmgrAonRecovFault));
  abs_mmio_write32(kClkmgrBase + CLKMGR_ALERT_TEST_REG_OFFSET,
                   1u << CLKMGR_ALERT_TEST_RECOV_FAULT_BIT);
  CHECK_STATUS_OK(ottf_alerts_expect_alert_finish(
      kTopEarlgreyAlertIdClkmgrAonRecovFault));

  // Ignore further recov_fault alerts during intentional measurement fault
  // injection so continuous clk_aon measurement pulses do not storm the ISR.
  CHECK_STATUS_OK(
      ottf_alerts_ignore_alert(kTopEarlgreyAlertIdClkmgrAonRecovFault));

  // 2. Verify reset values of per-clock MEAS_CTRL_SHADOWED registers.
  // IO & MAIN have 10-bit HI/LO (20 bits total);
  // IO_DIV2 & USB have 9-bit HI/LO (18 bits total);
  // IO_DIV4 has 8-bit HI/LO (16 bits total).
  uint32_t io_meas =
      abs_mmio_read32(kClkmgrBase + CLKMGR_IO_MEAS_CTRL_SHADOWED_REG_OFFSET);
  uint32_t io_div2_meas = abs_mmio_read32(
      kClkmgrBase + CLKMGR_IO_DIV2_MEAS_CTRL_SHADOWED_REG_OFFSET);
  uint32_t io_div4_meas = abs_mmio_read32(
      kClkmgrBase + CLKMGR_IO_DIV4_MEAS_CTRL_SHADOWED_REG_OFFSET);
  uint32_t main_meas =
      abs_mmio_read32(kClkmgrBase + CLKMGR_MAIN_MEAS_CTRL_SHADOWED_REG_OFFSET);
  uint32_t usb_meas =
      abs_mmio_read32(kClkmgrBase + CLKMGR_USB_MEAS_CTRL_SHADOWED_REG_OFFSET);

  CHECK(io_meas == 0x000759ea,
        "IO_MEAS_CTRL_SHADOWED reset mismatch: got 0x%08x, expected 0x000759ea",
        io_meas);
  CHECK(io_div2_meas == 0x0001ccfa,
        "IO_DIV2_MEAS_CTRL_SHADOWED reset mismatch: got 0x%08x, expected "
        "0x0001ccfa",
        io_div2_meas);
  CHECK(io_div4_meas == 0x00006e82,
        "IO_DIV4_MEAS_CTRL_SHADOWED reset mismatch: got 0x%08x, expected "
        "0x00006e82",
        io_div4_meas);
  CHECK(main_meas == 0x0007a9fe,
        "MAIN_MEAS_CTRL_SHADOWED reset mismatch: got 0x%08x, expected "
        "0x0007a9fe",
        main_meas);
  CHECK(usb_meas == 0x0001ccfa,
        "USB_MEAS_CTRL_SHADOWED reset mismatch: got 0x%08x, expected 0x0001ccfa",
        usb_meas);

  // 3. Verify bitmasks of MEAS_CTRL_SHADOWED registers via two-step shadow write.
  write_shadowed(CLKMGR_IO_MEAS_CTRL_SHADOWED_REG_OFFSET, 0xffffffff);
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_IO_MEAS_CTRL_SHADOWED_REG_OFFSET) ==
        0x000fffff);
  write_shadowed(CLKMGR_IO_MEAS_CTRL_SHADOWED_REG_OFFSET, io_meas);

  write_shadowed(CLKMGR_IO_DIV2_MEAS_CTRL_SHADOWED_REG_OFFSET, 0xffffffff);
  CHECK(abs_mmio_read32(kClkmgrBase +
                        CLKMGR_IO_DIV2_MEAS_CTRL_SHADOWED_REG_OFFSET) ==
        0x0003ffff);
  write_shadowed(CLKMGR_IO_DIV2_MEAS_CTRL_SHADOWED_REG_OFFSET, io_div2_meas);

  write_shadowed(CLKMGR_IO_DIV4_MEAS_CTRL_SHADOWED_REG_OFFSET, 0xffffffff);
  CHECK(abs_mmio_read32(kClkmgrBase +
                        CLKMGR_IO_DIV4_MEAS_CTRL_SHADOWED_REG_OFFSET) ==
        0x0000ffff);
  write_shadowed(CLKMGR_IO_DIV4_MEAS_CTRL_SHADOWED_REG_OFFSET, io_div4_meas);

  write_shadowed(CLKMGR_MAIN_MEAS_CTRL_SHADOWED_REG_OFFSET, 0xffffffff);
  CHECK(abs_mmio_read32(kClkmgrBase +
                        CLKMGR_MAIN_MEAS_CTRL_SHADOWED_REG_OFFSET) ==
        0x000fffff);
  write_shadowed(CLKMGR_MAIN_MEAS_CTRL_SHADOWED_REG_OFFSET, main_meas);

  write_shadowed(CLKMGR_USB_MEAS_CTRL_SHADOWED_REG_OFFSET, 0xffffffff);
  CHECK(abs_mmio_read32(kClkmgrBase +
                        CLKMGR_USB_MEAS_CTRL_SHADOWED_REG_OFFSET) ==
        0x0003ffff);
  write_shadowed(CLKMGR_USB_MEAS_CTRL_SHADOWED_REG_OFFSET, usb_meas);

  // 4. Verify JITTER_ENABLE and JITTER_REGWEN behavior.
  // In clkmgr_reg_top.sv, JITTER_REGWEN is W0C (reset 1) but does NOT gate
  // JITTER_ENABLE writes, and ANY write to JITTER_ENABLE sets it to MuBi4True
  // (0x6) regardless of the written data.
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_JITTER_REGWEN_REG_OFFSET) == 1);
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_JITTER_ENABLE_REG_OFFSET) ==
        kMultiBitBool4False);

  // Clear JITTER_REGWEN (W0C).
  abs_mmio_write32(kClkmgrBase + CLKMGR_JITTER_REGWEN_REG_OFFSET, 0);
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_JITTER_REGWEN_REG_OFFSET) == 0);

  // Write kMultiBitBool4False (0x9) to JITTER_ENABLE; RTL hardwires .wd to
  // MuBi4True (0x6) and ignores JITTER_REGWEN.
  abs_mmio_write32(kClkmgrBase + CLKMGR_JITTER_ENABLE_REG_OFFSET,
                   kMultiBitBool4False);
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_JITTER_ENABLE_REG_OFFSET) ==
        kMultiBitBool4True);

  // Writing 0x0 or 0x9 again keeps JITTER_ENABLE at MuBi4True (0x6).
  abs_mmio_write32(kClkmgrBase + CLKMGR_JITTER_ENABLE_REG_OFFSET, 0x0);
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_JITTER_ENABLE_REG_OFFSET) ==
        kMultiBitBool4True);

  // 5. Verify EXTCLK_CTRL and EXTCLK_CTRL_REGWEN without switching AST off
  // the internal oscillator (keep SEL != MuBi4True).
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_EXTCLK_CTRL_REG_OFFSET) == 0x99);
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_EXTCLK_STATUS_REG_OFFSET) ==
        kMultiBitBool4False);
  abs_mmio_write32(kClkmgrBase + CLKMGR_EXTCLK_CTRL_REG_OFFSET, 0x69);
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_EXTCLK_CTRL_REG_OFFSET) == 0x69);
  abs_mmio_write32(kClkmgrBase + CLKMGR_EXTCLK_CTRL_REG_OFFSET, 0x99);
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_EXTCLK_CTRL_REG_OFFSET) == 0x99);

  // 6. Verify measurement error detection with mubi4_test_true_loose (e.g. 0x0)
  // and continuous RECOV_ERR_CODE re-assertion while measurement remains enabled.
  // Set IO_DIV4_MEAS_CTRL_SHADOWED to an out-of-range window (LO=5, HI=10 ->
  // 0x050a, whereas actual IO_DIV4 count is ~120).
  write_shadowed(CLKMGR_IO_DIV4_MEAS_CTRL_SHADOWED_REG_OFFSET, 0x0000050a);
  // Enable measurement using 0x0 (which satisfies mubi4_test_true_loose != 0x9).
  abs_mmio_write32(kClkmgrBase + CLKMGR_IO_DIV4_MEAS_CTRL_EN_REG_OFFSET, 0x0);
  busy_spin_micros(30);

  uint32_t recov_err =
      abs_mmio_read32(kClkmgrBase + CLKMGR_RECOV_ERR_CODE_REG_OFFSET);
  CHECK((recov_err & (1u << CLKMGR_RECOV_ERR_CODE_IO_DIV4_MEASURE_ERR_BIT)) !=
            0,
        "Expected IO_DIV4_MEASURE_ERR to assert when MEAS_CTRL_EN=0x0, got "
        "0x%08x",
        recov_err);

  // Attempt to clear IO_DIV4_MEASURE_ERR while measurement is still enabled
  // and out-of-range: hardware must re-assert IO_DIV4_MEASURE_ERR on the next
  // clk_aon cycle.
  abs_mmio_write32(kClkmgrBase + CLKMGR_RECOV_ERR_CODE_REG_OFFSET,
                   1u << CLKMGR_RECOV_ERR_CODE_IO_DIV4_MEASURE_ERR_BIT);
  busy_spin_micros(30);
  recov_err = abs_mmio_read32(kClkmgrBase + CLKMGR_RECOV_ERR_CODE_REG_OFFSET);
  CHECK((recov_err & (1u << CLKMGR_RECOV_ERR_CODE_IO_DIV4_MEASURE_ERR_BIT)) !=
            0,
        "Expected IO_DIV4_MEASURE_ERR to re-assert while MEAS_CTRL_EN is active");

  // Now disable IO_DIV4_MEAS_CTRL_EN (0x9) and restore threshold, then clear
  // RECOV_ERR_CODE and verify it stays 0.
  abs_mmio_write32(kClkmgrBase + CLKMGR_IO_DIV4_MEAS_CTRL_EN_REG_OFFSET,
                   kMultiBitBool4False);
  write_shadowed(CLKMGR_IO_DIV4_MEAS_CTRL_SHADOWED_REG_OFFSET, io_div4_meas);
  busy_spin_micros(30);
  abs_mmio_write32(kClkmgrBase + CLKMGR_RECOV_ERR_CODE_REG_OFFSET, 0x7ff);
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_RECOV_ERR_CODE_REG_OFFSET) == 0);

  return true;
}
