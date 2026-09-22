// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/arch/boot_stage.h"
#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"
#include "sw/device/silicon_creator/lib/drivers/retention_sram.h"

#include "aon_timer_regs.h"
#include "clkmgr_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "pwrmgr_regs.h"
#include "rstmgr_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kClkmgrBase = TOP_EARLGREY_CLKMGR_AON_BASE_ADDR,
  kPwrmgrBase = TOP_EARLGREY_PWRMGR_AON_BASE_ADDR,
  kRstmgrBase = TOP_EARLGREY_RSTMGR_AON_BASE_ADDR,
  kAonTimerBase = TOP_EARLGREY_AON_TIMER_AON_BASE_ADDR,
  kAonTimerWkupBit = 1u << PWRMGR_PARAM_AON_TIMER_AON_WKUP_REQ_IDX,
  kPhaseDeepSleep = 0x434c4b31u,
  kPhaseWarmReset = 0x434c4b32u,
  kCustomIoDiv4Meas = 0x00001085u,
};

static void write_shadowed(uint32_t offset, uint32_t val) {
  abs_mmio_write32(kClkmgrBase + offset, val);
  abs_mmio_write32(kClkmgrBase + offset, val);
}

static void pwrmgr_cdc_sync(void) {
  abs_mmio_write32(kPwrmgrBase + PWRMGR_CFG_CDC_SYNC_REG_OFFSET,
                   1u << PWRMGR_CFG_CDC_SYNC_SYNC_BIT);
  while (abs_mmio_read32(kPwrmgrBase + PWRMGR_CFG_CDC_SYNC_REG_OFFSET) != 0u) {
  }
}

static volatile uint32_t g_access_fault_count = 0;
static volatile uint32_t g_last_mcause = 0;
static volatile bool g_expect_access_fault = false;

void ottf_exception_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = ibex_mcause_read();
  uint32_t mepc = ibex_mepc_read();
  if (g_expect_access_fault && (mcause == 5u || mcause == 7u)) {
    g_access_fault_count++;
    g_last_mcause = mcause;
    uint16_t insn16 = *(const uint16_t *)mepc;
    uint32_t step = ((insn16 & 0x3u) == 0x3u) ? 4u : 2u;
    asm volatile("csrw mepc, %0" : : "r"(mepc + step));
    return;
  }
  CHECK(false, "Unexpected exception mcause=0x%x mepc=0x%x", mcause, mepc);
}

static void test_w5_permit_subword_wr_err_and_addrmiss(void) {
  // 1. Valid 8-bit store to byte 0 of CLK_HINTS (CLKMGR_PERMIT[7] = 4'b0001,
  // reg_be = 4'b0001): (4'b0001 & ~4'b0001) == 0 -> succeeds without fault.
  g_access_fault_count = 0;
  g_expect_access_fault = true;
  abs_mmio_write8(kClkmgrBase + CLKMGR_CLK_HINTS_REG_OFFSET, 0x05u);
  g_expect_access_fault = false;
  CHECK(g_access_fault_count == 0u,
        "W5-1: 8-bit write to byte 0 of CLK_HINTS (PERMIT=4'b0001) must "
        "succeed without fault");
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_CLK_HINTS_REG_OFFSET) == 0x05u,
        "W5-1: CLK_HINTS should be 0x05 after valid byte-0 write, got 0x%08x",
        abs_mmio_read32(kClkmgrBase + CLKMGR_CLK_HINTS_REG_OFFSET));

  // 2. Illegal 8-bit store to byte 1 of CLK_HINTS (CLKMGR_PERMIT[7] = 4'b0001,
  // reg_be = 4'b0010): (4'b0001 & ~4'b0010) != 0 -> wr_err = 1 (mcause = 7)
  // and register value remains 0x05.
  g_access_fault_count = 0;
  g_last_mcause = 0;
  g_expect_access_fault = true;
  abs_mmio_write8(kClkmgrBase + CLKMGR_CLK_HINTS_REG_OFFSET + 1u, 0x0fu);
  g_expect_access_fault = false;
  CHECK(g_access_fault_count == 1u && g_last_mcause == 7u,
        "W5-2: 8-bit write to byte 1 of CLK_HINTS (PERMIT=4'b0001) must fault "
        "with Store Access Fault (mcause=7), got count=%u mcause=%u",
        g_access_fault_count, g_last_mcause);
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_CLK_HINTS_REG_OFFSET) == 0x05u,
        "W5-2: CLK_HINTS must remain 0x05 after rejected sub-word write");
  abs_mmio_write32(kClkmgrBase + CLKMGR_CLK_HINTS_REG_OFFSET, 0x0fu);

  // 3. Illegal 16-bit store to IO_MEAS_CTRL_SHADOWED (CLKMGR_PERMIT[11] =
  // 4'b0111, reg_be = 4'b0011): (4'b0111 & ~4'b0011) != 0 -> wr_err = 1.
  g_access_fault_count = 0;
  g_last_mcause = 0;
  g_expect_access_fault = true;
  *(volatile uint16_t *)(kClkmgrBase +
                         CLKMGR_IO_MEAS_CTRL_SHADOWED_REG_OFFSET) = 0x1234u;
  g_expect_access_fault = false;
  CHECK(g_access_fault_count == 1u && g_last_mcause == 7u,
        "W5-3: 16-bit write to IO_MEAS_CTRL_SHADOWED (PERMIT=4'b0111) must "
        "fault with Store Access Fault (mcause=7), got count=%u mcause=%u",
        g_access_fault_count, g_last_mcause);

  // 4. Illegal 8-bit store to byte 0 of RECOV_ERR_CODE (CLKMGR_PERMIT[20] =
  // 4'b0011, reg_be = 4'b0001): (4'b0011 & ~4'b0001) != 0 -> wr_err = 1.
  g_access_fault_count = 0;
  g_last_mcause = 0;
  g_expect_access_fault = true;
  abs_mmio_write8(kClkmgrBase + CLKMGR_RECOV_ERR_CODE_REG_OFFSET, 0x01u);
  g_expect_access_fault = false;
  CHECK(g_access_fault_count == 1u && g_last_mcause == 7u,
        "W5-4: 8-bit write to RECOV_ERR_CODE (PERMIT=4'b0011) must fault with "
        "Store Access Fault (mcause=7), got count=%u mcause=%u",
        g_access_fault_count, g_last_mcause);

  // 5. Out-of-bounds read at offset 0x58 (past FATAL_ERR_CODE at 0x54) must
  // fault with Load Access Fault (mcause = 5).
  g_access_fault_count = 0;
  g_last_mcause = 0;
  g_expect_access_fault = true;
  (void)abs_mmio_read32(kClkmgrBase + 0x58u);
  g_expect_access_fault = false;
  CHECK(g_access_fault_count == 1u && g_last_mcause == 5u,
        "W5-5: 32-bit read at out-of-bounds offset 0x58 must fault with Load "
        "Access Fault (mcause=5), got count=%u mcause=%u",
        g_access_fault_count, g_last_mcause);

  // 6. Out-of-bounds write at offset 0x58 must fault with Store Access Fault
  // (mcause = 7).
  g_access_fault_count = 0;
  g_last_mcause = 0;
  g_expect_access_fault = true;
  abs_mmio_write32(kClkmgrBase + 0x58u, 0xdeadbeefu);
  g_expect_access_fault = false;
  CHECK(g_access_fault_count == 1u && g_last_mcause == 7u,
        "W5-6: 32-bit write at out-of-bounds offset 0x58 must fault with Store "
        "Access Fault (mcause=7), got count=%u mcause=%u",
        g_access_fault_count, g_last_mcause);
}

bool test_main(void) {
  retention_sram_t *ret_sram = retention_sram_get();
  uint32_t phase = ret_sram->creator.reserved[0];
  uint32_t reset_info = ret_sram->creator.reset_reasons;

  if (phase == kPhaseDeepSleep) {
    CHECK_STATUS_OK(
        ottf_alerts_ignore_alert(kTopEarlgreyAlertIdClkmgrAonRecovFault));
    // Clean up AON timer and PWRMGR wakeup state after deep-sleep exit.
    abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET, 0u);
    abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET, 0u);
    abs_mmio_write32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET, 0x3u);
    abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REG_OFFSET, 0u);
    pwrmgr_cdc_sync();
    abs_mmio_write32(kPwrmgrBase + PWRMGR_INTR_STATE_REG_OFFSET, 1u);

    CHECK((reset_info & (1u << RSTMGR_RESET_INFO_LOW_POWER_EXIT_BIT)) != 0u,
          "Expected LOW_POWER_EXIT reset_info bit, got 0x%08x", reset_info);

    // In top_earlgrey.sv (u_clkmgr_aon), all clkmgr resets belong to
    // DomainAonSel, which is NOT asserted during deep-sleep wakeup
    // (low_power_exit). All CLKMGR CSRs and measurement shadow registers must
    // preserve their pre-sleep state.
    CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_EXTCLK_CTRL_REGWEN_REG_OFFSET) ==
              0u,
          "EXTCLK_CTRL_REGWEN not preserved across deep sleep");
    CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_EXTCLK_CTRL_REG_OFFSET) == 0x69u,
          "EXTCLK_CTRL not preserved across deep sleep: got 0x%08x",
          abs_mmio_read32(kClkmgrBase + CLKMGR_EXTCLK_CTRL_REG_OFFSET));
    CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_JITTER_REGWEN_REG_OFFSET) == 0u,
          "JITTER_REGWEN not preserved across deep sleep");
    CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_JITTER_ENABLE_REG_OFFSET) ==
              kMultiBitBool4True,
          "JITTER_ENABLE not preserved across deep sleep: got 0x%08x",
          abs_mmio_read32(kClkmgrBase + CLKMGR_JITTER_ENABLE_REG_OFFSET));
    CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_CLK_ENABLES_REG_OFFSET) == 0x7u,
          "CLK_ENABLES not preserved across deep sleep: got 0x%08x",
          abs_mmio_read32(kClkmgrBase + CLKMGR_CLK_ENABLES_REG_OFFSET));
    CHECK(abs_mmio_read32(kClkmgrBase +
                          CLKMGR_MEASURE_CTRL_REGWEN_REG_OFFSET) == 1u,
          "MEASURE_CTRL_REGWEN must be re-enabled (1) by calib_rdy=MuBi4False "
          "during deep sleep (clkmgr.sv:555-558)");
    CHECK(
        abs_mmio_read32(kClkmgrBase + CLKMGR_IO_DIV4_MEAS_CTRL_EN_REG_OFFSET) ==
            kMultiBitBool4False,
        "IO_DIV4_MEAS_CTRL_EN must be cleared to MuBi4False by "
        "calib_rdy=MuBi4False during deep sleep (clkmgr_meas_chk.sv:81-85)");
    CHECK(abs_mmio_read32(kClkmgrBase +
                          CLKMGR_IO_DIV4_MEAS_CTRL_SHADOWED_REG_OFFSET) ==
              kCustomIoDiv4Meas,
          "IO_DIV4_MEAS_CTRL_SHADOWED not preserved across deep sleep: got "
          "0x%08x",
          abs_mmio_read32(kClkmgrBase +
                          CLKMGR_IO_DIV4_MEAS_CTRL_SHADOWED_REG_OFFSET));
    CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_RECOV_ERR_CODE_REG_OFFSET) ==
              (1u << CLKMGR_RECOV_ERR_CODE_SHADOW_UPDATE_ERR_BIT),
          "RECOV_ERR_CODE not preserved across deep sleep: got 0x%08x",
          abs_mmio_read32(kClkmgrBase + CLKMGR_RECOV_ERR_CODE_REG_OFFSET));

    // Now trigger a non-sleep warm reset via RSTMGR.RESET_REQ, which resets
    // DomainAonSel and must restore all CLKMGR CSRs to their reset defaults.
    if (kBootStage != kBootStageOwner) {
      abs_mmio_write32(kRstmgrBase + RSTMGR_RESET_INFO_REG_OFFSET, reset_info);
    }
    ret_sram->creator.reserved[0] = kPhaseWarmReset;
    abs_mmio_write32(kRstmgrBase + RSTMGR_RESET_REQ_REG_OFFSET,
                     kMultiBitBool4True);
    wait_for_interrupt();
  }

  if (phase == kPhaseWarmReset) {
    ret_sram->creator.reserved[0] = 0u;
    CHECK((reset_info & (1u << RSTMGR_RESET_INFO_SW_RESET_BIT)) != 0u,
          "Expected SW_RESET reset_info bit, got 0x%08x", reset_info);

    CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_EXTCLK_CTRL_REGWEN_REG_OFFSET) ==
              1u,
          "EXTCLK_CTRL_REGWEN did not reset on SW_RESET");
    CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_EXTCLK_CTRL_REG_OFFSET) == 0x99u,
          "EXTCLK_CTRL did not reset on SW_RESET");
    CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_JITTER_REGWEN_REG_OFFSET) == 1u,
          "JITTER_REGWEN did not reset on SW_RESET");
    CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_JITTER_ENABLE_REG_OFFSET) ==
              kMultiBitBool4False,
          "JITTER_ENABLE did not reset on SW_RESET");
    CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_CLK_ENABLES_REG_OFFSET) == 0xfu,
          "CLK_ENABLES did not reset on SW_RESET");
    CHECK(abs_mmio_read32(kClkmgrBase +
                          CLKMGR_MEASURE_CTRL_REGWEN_REG_OFFSET) == 1u,
          "MEASURE_CTRL_REGWEN did not reset on SW_RESET");
    CHECK(abs_mmio_read32(kClkmgrBase +
                          CLKMGR_IO_DIV4_MEAS_CTRL_SHADOWED_REG_OFFSET) ==
              0x00006e82u,
          "IO_DIV4_MEAS_CTRL_SHADOWED did not reset on SW_RESET");
    CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_RECOV_ERR_CODE_REG_OFFSET) == 0u,
          "RECOV_ERR_CODE did not reset on SW_RESET");
    return true;
  }

  test_w5_permit_subword_wr_err_and_addrmiss();

  // 1. Verify ALERT_TEST single-shot pulse for recov_fault.
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdClkmgrAonRecovFault));
  abs_mmio_write32(kClkmgrBase + CLKMGR_ALERT_TEST_REG_OFFSET,
                   1u << CLKMGR_ALERT_TEST_RECOV_FAULT_BIT);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdClkmgrAonRecovFault));

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
  CHECK(
      usb_meas == 0x0001ccfa,
      "USB_MEAS_CTRL_SHADOWED reset mismatch: got 0x%08x, expected 0x0001ccfa",
      usb_meas);

  // 3. Verify bitmasks of MEAS_CTRL_SHADOWED registers via two-step shadow
  // write.
  write_shadowed(CLKMGR_IO_MEAS_CTRL_SHADOWED_REG_OFFSET, 0xffffffff);
  CHECK(abs_mmio_read32(kClkmgrBase +
                        CLKMGR_IO_MEAS_CTRL_SHADOWED_REG_OFFSET) == 0x000fffff);
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
  CHECK(
      abs_mmio_read32(kClkmgrBase + CLKMGR_USB_MEAS_CTRL_SHADOWED_REG_OFFSET) ==
      0x0003ffff);
  write_shadowed(CLKMGR_USB_MEAS_CTRL_SHADOWED_REG_OFFSET, usb_meas);

  // 3b. Verify shadow phase clear on read and SHADOW_UPDATE_ERR on mismatched
  // second write.
  abs_mmio_write32(kClkmgrBase + CLKMGR_IO_DIV4_MEAS_CTRL_SHADOWED_REG_OFFSET,
                   0x00001234u);
  // Reading before second write returns committed value and clears phase.
  CHECK(abs_mmio_read32(kClkmgrBase +
                        CLKMGR_IO_DIV4_MEAS_CTRL_SHADOWED_REG_OFFSET) ==
        io_div4_meas);
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_RECOV_ERR_CODE_REG_OFFSET) == 0u);
  // Mismatched two-step write triggers SHADOW_UPDATE_ERR.
  abs_mmio_write32(kClkmgrBase + CLKMGR_IO_DIV4_MEAS_CTRL_SHADOWED_REG_OFFSET,
                   0x00001234u);
  abs_mmio_write32(kClkmgrBase + CLKMGR_IO_DIV4_MEAS_CTRL_SHADOWED_REG_OFFSET,
                   0x00005678u);
  busy_spin_micros(10);
  CHECK((abs_mmio_read32(kClkmgrBase + CLKMGR_RECOV_ERR_CODE_REG_OFFSET) &
         (1u << CLKMGR_RECOV_ERR_CODE_SHADOW_UPDATE_ERR_BIT)) != 0u);
  abs_mmio_write32(kClkmgrBase + CLKMGR_RECOV_ERR_CODE_REG_OFFSET,
                   1u << CLKMGR_RECOV_ERR_CODE_SHADOW_UPDATE_ERR_BIT);
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_RECOV_ERR_CODE_REG_OFFSET) == 0u);

  // 4. Verify JITTER_ENABLE and JITTER_REGWEN behavior.
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
  // and continuous RECOV_ERR_CODE re-assertion while measurement remains
  // enabled.
  write_shadowed(CLKMGR_IO_DIV4_MEAS_CTRL_SHADOWED_REG_OFFSET, 0x0000050a);
  abs_mmio_write32(kClkmgrBase + CLKMGR_IO_DIV4_MEAS_CTRL_EN_REG_OFFSET, 0x0);
  busy_spin_micros(30);

  uint32_t recov_err =
      abs_mmio_read32(kClkmgrBase + CLKMGR_RECOV_ERR_CODE_REG_OFFSET);
  CHECK(
      (recov_err & (1u << CLKMGR_RECOV_ERR_CODE_IO_DIV4_MEASURE_ERR_BIT)) != 0,
      "Expected IO_DIV4_MEASURE_ERR to assert when MEAS_CTRL_EN=0x0, got "
      "0x%08x",
      recov_err);

  abs_mmio_write32(kClkmgrBase + CLKMGR_RECOV_ERR_CODE_REG_OFFSET,
                   1u << CLKMGR_RECOV_ERR_CODE_IO_DIV4_MEASURE_ERR_BIT);
  busy_spin_micros(30);
  recov_err = abs_mmio_read32(kClkmgrBase + CLKMGR_RECOV_ERR_CODE_REG_OFFSET);
  CHECK(
      (recov_err & (1u << CLKMGR_RECOV_ERR_CODE_IO_DIV4_MEASURE_ERR_BIT)) != 0,
      "Expected IO_DIV4_MEASURE_ERR to re-assert while MEAS_CTRL_EN is active");

  // Disable IO_DIV4_MEAS_CTRL_EN (0x9) and set a custom in-range threshold
  // (kCustomIoDiv4Meas), then clear RECOV_ERR_CODE and re-enable
  // IO_DIV4_MEAS_CTRL_EN (0x6) with the valid window.
  abs_mmio_write32(kClkmgrBase + CLKMGR_IO_DIV4_MEAS_CTRL_EN_REG_OFFSET,
                   kMultiBitBool4False);
  write_shadowed(CLKMGR_IO_DIV4_MEAS_CTRL_SHADOWED_REG_OFFSET,
                 kCustomIoDiv4Meas);
  busy_spin_micros(30);
  abs_mmio_write32(kClkmgrBase + CLKMGR_RECOV_ERR_CODE_REG_OFFSET, 0x7ff);
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_RECOV_ERR_CODE_REG_OFFSET) == 0);
  abs_mmio_write32(kClkmgrBase + CLKMGR_IO_DIV4_MEAS_CTRL_EN_REG_OFFSET,
                   kMultiBitBool4True);
  busy_spin_micros(30);
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_RECOV_ERR_CODE_REG_OFFSET) == 0);

  // 7. Set up CLKMGR state to test DomainAonSel preservation across deep sleep:
  // Trigger a sticky SHADOW_UPDATE_ERR in RECOV_ERR_CODE, then lock
  // MEASURE_CTRL_REGWEN=0 and EXTCLK_CTRL_REGWEN=0, and set CLK_HINTS=0x5.
  abs_mmio_write32(kClkmgrBase + CLKMGR_IO_DIV4_MEAS_CTRL_SHADOWED_REG_OFFSET,
                   0x00001111u);
  abs_mmio_write32(kClkmgrBase + CLKMGR_IO_DIV4_MEAS_CTRL_SHADOWED_REG_OFFSET,
                   0x00002222u);
  busy_spin_micros(10);
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_RECOV_ERR_CODE_REG_OFFSET) ==
        (1u << CLKMGR_RECOV_ERR_CODE_SHADOW_UPDATE_ERR_BIT));

  abs_mmio_write32(kClkmgrBase + CLKMGR_MEASURE_CTRL_REGWEN_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_MEASURE_CTRL_REGWEN_REG_OFFSET) ==
        0u);
  // Writes while MEASURE_CTRL_REGWEN=0 must be ignored.
  write_shadowed(CLKMGR_IO_DIV4_MEAS_CTRL_SHADOWED_REG_OFFSET, io_div4_meas);
  CHECK(abs_mmio_read32(kClkmgrBase +
                        CLKMGR_IO_DIV4_MEAS_CTRL_SHADOWED_REG_OFFSET) ==
        kCustomIoDiv4Meas);

  abs_mmio_write32(kClkmgrBase + CLKMGR_EXTCLK_CTRL_REG_OFFSET, 0x69u);
  abs_mmio_write32(kClkmgrBase + CLKMGR_EXTCLK_CTRL_REGWEN_REG_OFFSET, 0u);
  abs_mmio_write32(kClkmgrBase + CLKMGR_EXTCLK_CTRL_REG_OFFSET, 0x99u);
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_EXTCLK_CTRL_REG_OFFSET) == 0x69u);

  abs_mmio_write32(kClkmgrBase + CLKMGR_CLK_ENABLES_REG_OFFSET, 0x7u);
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_CLK_ENABLES_REG_OFFSET) == 0x7u);

  // Enter deep sleep (MAIN_PD_N = 0, LOW_POWER_HINT = 1) with AON_TIMER wakeup.
  ret_sram->creator.reserved[0] = kPhaseDeepSleep;
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CAUSE_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET, 0x3u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_COUNT_HI_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_COUNT_LO_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_THOLD_HI_REG_OFFSET, 0u);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_THOLD_LO_REG_OFFSET, 20u);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_WAKEUP_EN_REG_OFFSET, kAonTimerWkupBit);
  abs_mmio_write32(kPwrmgrBase + PWRMGR_CONTROL_REG_OFFSET,
                   1u << PWRMGR_CONTROL_LOW_POWER_HINT_BIT);
  pwrmgr_cdc_sync();
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WKUP_CTRL_REG_OFFSET,
                   1u << AON_TIMER_WKUP_CTRL_ENABLE_BIT);
  wait_for_interrupt();

  return false;
}
