// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_alert_handler.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/irq.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"
#include "sw/device/silicon_creator/lib/drivers/retention_sram.h"

#include "alert_handler_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "i2c_regs.h"
#include "rstmgr_regs.h"
#include "spi_device_regs.h"
#include "spi_host_regs.h"
#include "uart_regs.h"
#include "usbdev_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kAlertHandlerBase = TOP_EARLGREY_ALERT_HANDLER_BASE_ADDR,
  kRstmgrBase = TOP_EARLGREY_RSTMGR_AON_BASE_ADDR,
  kSpiDeviceBase = TOP_EARLGREY_SPI_DEVICE_BASE_ADDR,
  kSpiHost0Base = TOP_EARLGREY_SPI_HOST0_BASE_ADDR,
  kSpiHost1Base = TOP_EARLGREY_SPI_HOST1_BASE_ADDR,
  kUsbdevBase = TOP_EARLGREY_USBDEV_BASE_ADDR,
  kI2c0Base = TOP_EARLGREY_I2C0_BASE_ADDR,
  kUart0Base = TOP_EARLGREY_UART0_BASE_ADDR,
  kRetMagic = 0x5a5aa5a5u,
};

static int g_failures = 0;

static void write_shadowed(uint32_t addr, uint32_t val) {
  abs_mmio_write32(addr, val);
  abs_mmio_write32(addr, val);
}

#define EXPECT_EQ(actual, expected, msg)                                    \
  do {                                                                      \
    uint32_t _act = (uint32_t)(actual);                                     \
    uint32_t _exp = (uint32_t)(expected);                                   \
    if (_act != _exp) {                                                     \
      LOG_ERROR("MISMATCH: %s (actual=0x%08x, expected=0x%08x)", msg, _act, \
                _exp);                                                      \
      g_failures++;                                                         \
    } else {                                                                \
      LOG_INFO("OK: %s (0x%08x)", msg, _act);                               \
    }                                                                       \
  } while (0)

bool test_main(void) {
  irq_global_ctrl(false);

  dif_alert_handler_t alert_handler;
  CHECK_DIF_OK(dif_alert_handler_init(mmio_region_from_addr(kAlertHandlerBase),
                                      &alert_handler));

  retention_sram_t *ret_sram = retention_sram_get();
  uint32_t reset_reasons = ret_sram->creator.reset_reasons;
  /*
   * Check if we just rebooted from the escalation reset (E3) triggered at the
   * end of the first pass. Mask ROM copies RSTMGR.RESET_INFO into
   * retention_sram->creator.reset_reasons and clears RSTMGR.RESET_INFO.
   */
  if (ret_sram->owner.reserved[0] == kRetMagic ||
      (reset_reasons & (1u << 6)) != 0u) {
    g_failures = (int)ret_sram->owner.reserved[1];
    ret_sram->owner.reserved[0] = 0u;
    ret_sram->owner.reserved[1] = 0u;

    uint32_t dump[9];
    for (uint32_t i = 0; i < 9; ++i) {
      abs_mmio_write32(kRstmgrBase + RSTMGR_ALERT_INFO_CTRL_REG_OFFSET,
                       (i << RSTMGR_ALERT_INFO_CTRL_INDEX_OFFSET) |
                           (1u << RSTMGR_ALERT_INFO_CTRL_EN_BIT));
      dump[i] = abs_mmio_read32(kRstmgrBase + RSTMGR_ALERT_INFO_REG_OFFSET);
      LOG_INFO("ALERT_INFO[%u] = 0x%08x", i, dump[i]);
    }

    uint32_t post_e2_marker = ret_sram->owner.reserved[2];
    ret_sram->owner.reserved[2] = 0u;
    EXPECT_EQ(post_e2_marker, 0u,
              "ALERT_HANDLER esc_tx[2] -> LC_CTRL (EscScrapState1) halts CPU "
              "via lc_cpu_en/lc_escalate_en in Phase 0");

    return g_failures == 0;
  }

  // ---------------------------------------------------------------------------
  // 1. RSTMGR ALERT_TEST (alerts 23 & 24 in top_earlgrey)
  // ---------------------------------------------------------------------------
  CHECK_DIF_OK(dif_alert_handler_alert_acknowledge(
      &alert_handler, kTopEarlgreyAlertIdRstmgrAonFatalFault));
  CHECK_DIF_OK(dif_alert_handler_alert_acknowledge(
      &alert_handler, kTopEarlgreyAlertIdRstmgrAonFatalCnstyFault));

  abs_mmio_write32(kRstmgrBase + RSTMGR_ALERT_TEST_REG_OFFSET,
                   (1u << RSTMGR_ALERT_TEST_FATAL_FAULT_BIT) |
                       (1u << RSTMGR_ALERT_TEST_FATAL_CNSTY_FAULT_BIT));
  busy_spin_micros(10);

  bool is_cause = false;
  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
      &alert_handler, kTopEarlgreyAlertIdRstmgrAonFatalFault, &is_cause));
  EXPECT_EQ(is_cause, 1u, "RSTMGR ALERT_TEST bit 0 sets ALERT_CAUSE_23");

  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
      &alert_handler, kTopEarlgreyAlertIdRstmgrAonFatalCnstyFault, &is_cause));
  EXPECT_EQ(is_cause, 1u, "RSTMGR ALERT_TEST bit 1 sets ALERT_CAUSE_24");

  CHECK_DIF_OK(dif_alert_handler_alert_acknowledge(
      &alert_handler, kTopEarlgreyAlertIdRstmgrAonFatalFault));
  CHECK_DIF_OK(dif_alert_handler_alert_acknowledge(
      &alert_handler, kTopEarlgreyAlertIdRstmgrAonFatalCnstyFault));
  busy_spin_micros(10);

  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
      &alert_handler, kTopEarlgreyAlertIdRstmgrAonFatalFault, &is_cause));
  EXPECT_EQ(is_cause, 0u,
            "RSTMGR ALERT_CAUSE_23 stays 0 after ack (1-cycle pulse)");

  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
      &alert_handler, kTopEarlgreyAlertIdRstmgrAonFatalCnstyFault, &is_cause));
  EXPECT_EQ(is_cause, 0u,
            "RSTMGR ALERT_CAUSE_24 stays 0 after ack (1-cycle pulse)");

  // ---------------------------------------------------------------------------
  // 2. RSTMGR SW_RST_CTRL_N[0..7] peripheral reset routing
  // ---------------------------------------------------------------------------
  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_CFG_REG_OFFSET, 0x0100000cu);
  abs_mmio_write32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_0_REG_OFFSET, 0u);
  abs_mmio_write32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_0_REG_OFFSET, 1u);
  EXPECT_EQ(abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_CFG_REG_OFFSET), 0x0u,
            "SPI_DEVICE.CFG reset via SW_RST_CTRL_N_0");

  abs_mmio_write32(kSpiHost0Base + SPI_HOST_CONFIGOPTS_REG_OFFSET, 0x0f0f0000u);
  abs_mmio_write32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_1_REG_OFFSET, 0u);
  abs_mmio_write32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_1_REG_OFFSET, 1u);
  EXPECT_EQ(abs_mmio_read32(kSpiHost0Base + SPI_HOST_CONFIGOPTS_REG_OFFSET),
            0x0u, "SPI_HOST0.CONFIGOPTS reset via SW_RST_CTRL_N_1");

  abs_mmio_write32(kSpiHost1Base + SPI_HOST_CONFIGOPTS_REG_OFFSET, 0x0f0f0000u);
  abs_mmio_write32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_2_REG_OFFSET, 0u);
  abs_mmio_write32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_2_REG_OFFSET, 1u);
  EXPECT_EQ(abs_mmio_read32(kSpiHost1Base + SPI_HOST_CONFIGOPTS_REG_OFFSET),
            0x0u, "SPI_HOST1.CONFIGOPTS reset via SW_RST_CTRL_N_2");

  abs_mmio_write32(kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET, 0x00150000u);
  abs_mmio_write32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_3_REG_OFFSET, 0u);
  abs_mmio_write32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_3_REG_OFFSET, 1u);
  EXPECT_EQ(abs_mmio_read32(kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET), 0x0u,
            "USBDEV.USBCTRL reset via SW_RST_CTRL_N_3");

  abs_mmio_write32(kI2c0Base + I2C_TIMING0_REG_OFFSET, 0x00100010u);
  abs_mmio_write32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_5_REG_OFFSET, 0u);
  abs_mmio_write32(kRstmgrBase + RSTMGR_SW_RST_CTRL_N_5_REG_OFFSET, 1u);
  EXPECT_EQ(abs_mmio_read32(kI2c0Base + I2C_TIMING0_REG_OFFSET), 0x0u,
            "I2C0.TIMING0 reset via SW_RST_CTRL_N_5");

  // ---------------------------------------------------------------------------
  // 3. ALERT_HANDLER Escalation Signal 2 (esc_tx[2]) -> LC_CTRL
  // ---------------------------------------------------------------------------
  ret_sram->owner.reserved[0] = kRetMagic;
  ret_sram->owner.reserved[1] = (uint32_t)g_failures;
  ret_sram->owner.reserved[2] = 0u;

  abs_mmio_write32(kRstmgrBase + RSTMGR_ALERT_INFO_CTRL_REG_OFFSET,
                   1u << RSTMGR_ALERT_INFO_CTRL_EN_BIT);

  write_shadowed(
      kAlertHandlerBase + ALERT_HANDLER_CLASSB_ACCUM_THRESH_SHADOWED_REG_OFFSET,
      0u);
  write_shadowed(
      kAlertHandlerBase + ALERT_HANDLER_CLASSB_PHASE0_CYC_SHADOWED_REG_OFFSET,
      1200u);
  write_shadowed(
      kAlertHandlerBase + ALERT_HANDLER_CLASSB_PHASE1_CYC_SHADOWED_REG_OFFSET,
      10u);
  write_shadowed(
      kAlertHandlerBase + ALERT_HANDLER_CLASSB_PHASE2_CYC_SHADOWED_REG_OFFSET,
      10u);
  write_shadowed(
      kAlertHandlerBase + ALERT_HANDLER_CLASSB_PHASE3_CYC_SHADOWED_REG_OFFSET,
      10u);

  uint32_t classb_ctrl =
      (1u << ALERT_HANDLER_CLASSB_CTRL_SHADOWED_EN_BIT) |
      (1u << ALERT_HANDLER_CLASSB_CTRL_SHADOWED_EN_E2_BIT) |
      (0u << ALERT_HANDLER_CLASSB_CTRL_SHADOWED_MAP_E2_OFFSET) |
      (1u << ALERT_HANDLER_CLASSB_CTRL_SHADOWED_EN_E3_BIT) |
      (1u << ALERT_HANDLER_CLASSB_CTRL_SHADOWED_MAP_E3_OFFSET);
  write_shadowed(
      kAlertHandlerBase + ALERT_HANDLER_CLASSB_CTRL_SHADOWED_REG_OFFSET,
      classb_ctrl);

  // Map Alert 1 (UART1 fatal_fault, not locked by ROM) to Class B (value 1) and
  // enable.
  write_shadowed(
      kAlertHandlerBase + ALERT_HANDLER_ALERT_CLASS_SHADOWED_1_REG_OFFSET, 1u);
  write_shadowed(
      kAlertHandlerBase + ALERT_HANDLER_ALERT_EN_SHADOWED_1_REG_OFFSET, 1u);

  // Trigger UART1 ALERT_TEST (Alert 1) -> Class B Phase 0 (E2) -> Phase 1 (E3)
  abs_mmio_write32(TOP_EARLGREY_UART1_BASE_ADDR + UART_ALERT_TEST_REG_OFFSET,
                   1u);
  busy_spin_micros(50);

  // Attempt to write ret_sram->owner.reserved[2] during Phase 0 (while E2 is
  // active).
  ret_sram->owner.reserved[2] = 0xdeadbeefu;

  busy_spin_micros(3000);
  LOG_ERROR("MISMATCH: Escalation Reset (E3) did not fire");
  return false;
}
