// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/dif/dif_alert_handler.h"
#include "sw/device/lib/dif/dif_rv_core_ibex.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "alert_handler_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "rv_core_ibex_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kIbexBase = TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR,
  kAlertHandlerBase = TOP_EARLGREY_ALERT_HANDLER_BASE_ADDR,
  kPageSize = 4096,
};

alignas(4096) static volatile uint32_t sram_buf_a[kPageSize / sizeof(uint32_t)];
alignas(4096) static volatile uint32_t sram_buf_b[kPageSize / sizeof(uint32_t)];
alignas(4096) static volatile uint32_t
    sram_virt_win[kPageSize / sizeof(uint32_t)];

static void configure_ibex_alerts_non_escalating(void) {
  for (uint32_t alert_id = kTopEarlgreyAlertIdRvCoreIbexFatalSwErr;
       alert_id <= kTopEarlgreyAlertIdRvCoreIbexRecovHwErr; ++alert_id) {
    uint32_t reg_offset = alert_id * sizeof(uint32_t);
    uint32_t regwen = abs_mmio_read32(
        kAlertHandlerBase + ALERT_HANDLER_ALERT_REGWEN_0_REG_OFFSET +
        reg_offset);
    if (regwen & 0x1u) {
      // Route alert to Class A (which OTTF leaves disabled) while keeping
      // ALERT_EN_SHADOWED = 1 so ALERT_CAUSE still latches without triggering
      // OTTF's Class D fault ISR.
      abs_mmio_write32_shadowed(
          kAlertHandlerBase + ALERT_HANDLER_ALERT_CLASS_SHADOWED_0_REG_OFFSET +
              reg_offset,
          0u);
      abs_mmio_write32_shadowed(
          kAlertHandlerBase + ALERT_HANDLER_ALERT_EN_SHADOWED_0_REG_OFFSET +
              reg_offset,
          1u);
    }
  }
}

bool test_main(void) {
  dif_rv_core_ibex_t ibex;
  CHECK_DIF_OK(
      dif_rv_core_ibex_init(mmio_region_from_addr(kIbexBase), &ibex));

  dif_alert_handler_t alert_handler;
  CHECK_DIF_OK(dif_alert_handler_init(
      mmio_region_from_addr(kAlertHandlerBase), &alert_handler));

  configure_ibex_alerts_non_escalating();

  // 1. Verify register reset values and RO enforcement.
  CHECK(abs_mmio_read32(kIbexBase + RV_CORE_IBEX_ALERT_TEST_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kIbexBase + RV_CORE_IBEX_SW_RECOV_ERR_REG_OFFSET) ==
        kMultiBitBool4False);
  CHECK(abs_mmio_read32(kIbexBase + RV_CORE_IBEX_SW_FATAL_ERR_REG_OFFSET) ==
        kMultiBitBool4False);
  CHECK(abs_mmio_read32(kIbexBase + RV_CORE_IBEX_IBUS_REGWEN_0_REG_OFFSET) ==
        1u);
  CHECK(abs_mmio_read32(kIbexBase + RV_CORE_IBEX_IBUS_REGWEN_1_REG_OFFSET) ==
        1u);
  CHECK(abs_mmio_read32(kIbexBase + RV_CORE_IBEX_DBUS_REGWEN_0_REG_OFFSET) ==
        1u);
  CHECK(abs_mmio_read32(kIbexBase + RV_CORE_IBEX_DBUS_REGWEN_1_REG_OFFSET) ==
        1u);
  CHECK(abs_mmio_read32(kIbexBase + RV_CORE_IBEX_ERR_STATUS_REG_OFFSET) == 0u);

  uint32_t fpga_info =
      abs_mmio_read32(kIbexBase + RV_CORE_IBEX_FPGA_INFO_REG_OFFSET);
  abs_mmio_write32(kIbexBase + RV_CORE_IBEX_FPGA_INFO_REG_OFFSET,
                   fpga_info ^ 0xdeadbeefu);
  CHECK(abs_mmio_read32(kIbexBase + RV_CORE_IBEX_FPGA_INFO_REG_OFFSET) ==
        fpga_info);

  // 2. Verify ALERT_TEST (0x00) pulses each of the 4 alerts (IDs 61..64)
  // without latching, and ALERT_CAUSE can be cleanly cleared via RW1C.
  for (uint32_t bit = 0; bit < 4; ++bit) {
    dif_alert_handler_alert_t target_alert =
        (dif_alert_handler_alert_t)(kTopEarlgreyAlertIdRvCoreIbexFatalSwErr +
                                    bit);
    CHECK_DIF_OK(
        dif_alert_handler_alert_acknowledge(&alert_handler, target_alert));

    abs_mmio_write32(kIbexBase + RV_CORE_IBEX_ALERT_TEST_REG_OFFSET,
                     1u << bit);
    CHECK(abs_mmio_read32(kIbexBase + RV_CORE_IBEX_ALERT_TEST_REG_OFFSET) ==
          0u);

    bool is_cause = false;
    CHECK_DIF_OK(dif_alert_handler_alert_is_cause(&alert_handler, target_alert,
                                                  &is_cause));
    CHECK(is_cause);

    // Ensure none of the other 3 ibex alerts fired.
    for (uint32_t other = 0; other < 4; ++other) {
      if (other == bit) {
        continue;
      }
      bool other_cause = false;
      CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
          &alert_handler,
          (dif_alert_handler_alert_t)(kTopEarlgreyAlertIdRvCoreIbexFatalSwErr +
                                      other),
          &other_cause));
      CHECK(!other_cause);
    }

    CHECK_DIF_OK(
        dif_alert_handler_alert_acknowledge(&alert_handler, target_alert));
    CHECK_DIF_OK(dif_alert_handler_alert_is_cause(&alert_handler, target_alert,
                                                  &is_cause));
    CHECK(!is_cause);
  }

  // 3. Verify SW_RECOV_ERR (0x04):
  // - Writing kMultiBitBool4False (0x9) does not trigger recov_sw_err.
  // - Writing kMultiBitBool4True (0x6) or any non-False 4-bit value triggers
  //   recov_sw_err (alert ID 62), self-resets SW_RECOV_ERR to
  //   kMultiBitBool4False (0x9), and can be acknowledged and re-triggered.
  abs_mmio_write32(kIbexBase + RV_CORE_IBEX_SW_RECOV_ERR_REG_OFFSET,
                   kMultiBitBool4False);
  CHECK(abs_mmio_read32(kIbexBase + RV_CORE_IBEX_SW_RECOV_ERR_REG_OFFSET) ==
        kMultiBitBool4False);
  bool recov_cause = false;
  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
      &alert_handler, kTopEarlgreyAlertIdRvCoreIbexRecovSwErr, &recov_cause));
  CHECK(!recov_cause);

  abs_mmio_write32(kIbexBase + RV_CORE_IBEX_SW_RECOV_ERR_REG_OFFSET,
                   kMultiBitBool4True);
  CHECK(abs_mmio_read32(kIbexBase + RV_CORE_IBEX_SW_RECOV_ERR_REG_OFFSET) ==
        kMultiBitBool4False);
  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
      &alert_handler, kTopEarlgreyAlertIdRvCoreIbexRecovSwErr, &recov_cause));
  CHECK(recov_cause);
  CHECK_DIF_OK(dif_alert_handler_alert_acknowledge(
      &alert_handler, kTopEarlgreyAlertIdRvCoreIbexRecovSwErr));
  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
      &alert_handler, kTopEarlgreyAlertIdRvCoreIbexRecovSwErr, &recov_cause));
  CHECK(!recov_cause);

  // Re-trigger SW_RECOV_ERR with non-strict true (0x0 != MuBi4False).
  abs_mmio_write32(kIbexBase + RV_CORE_IBEX_SW_RECOV_ERR_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kIbexBase + RV_CORE_IBEX_SW_RECOV_ERR_REG_OFFSET) ==
        kMultiBitBool4False);
  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
      &alert_handler, kTopEarlgreyAlertIdRvCoreIbexRecovSwErr, &recov_cause));
  CHECK(recov_cause);
  CHECK_DIF_OK(dif_alert_handler_alert_acknowledge(
      &alert_handler, kTopEarlgreyAlertIdRvCoreIbexRecovSwErr));

  // 4. Verify EDN RND_DATA / RND_STATUS interface.
  uint32_t rnd_word = 0;
  CHECK_DIF_OK(dif_rv_core_ibex_read_rnd_data(&ibex, &rnd_word));

  // 5. Verify DBUS Address Translation (rv_core_addr_trans), 1-bit ADDR_EN
  // mask, Slot Priority (Slot 0 > Slot 1), and DBUS_REGWEN RW0C locking.
  sram_buf_a[0] = 0x11112222u;
  sram_buf_b[0] = 0x33334444u;
  sram_virt_win[0] = 0x55556666u;
  CHECK(sram_virt_win[0] == 0x55556666u);

  dif_rv_core_ibex_addr_translation_mapping_t map_slot0 = {
      .matching_addr = (uintptr_t)sram_virt_win,
      .remap_addr = (uintptr_t)sram_buf_a,
      .size = kPageSize,
  };
  dif_rv_core_ibex_addr_translation_mapping_t map_slot1 = {
      .matching_addr = (uintptr_t)sram_virt_win,
      .remap_addr = (uintptr_t)sram_buf_b,
      .size = kPageSize,
  };

  CHECK_DIF_OK(dif_rv_core_ibex_configure_addr_translation(
      &ibex, kDifRvCoreIbexAddrTranslationSlot_1,
      kDifRvCoreIbexAddrTranslationDBus, map_slot1));
  // Verify 1-bit write mask on IBUS_ADDR_EN_0 and DBUS_ADDR_EN_1: writing
  // 0xfffffffe (bit 0 = 0) must store 0 and NOT enable translation.
  abs_mmio_write32(kIbexBase + RV_CORE_IBEX_IBUS_ADDR_EN_0_REG_OFFSET,
                   0xfffffffeu);
  CHECK(abs_mmio_read32(kIbexBase + RV_CORE_IBEX_IBUS_ADDR_EN_0_REG_OFFSET) ==
        0u);
  abs_mmio_write32(kIbexBase + RV_CORE_IBEX_DBUS_ADDR_EN_1_REG_OFFSET,
                   0xfffffffeu);
  CHECK(abs_mmio_read32(kIbexBase + RV_CORE_IBEX_DBUS_ADDR_EN_1_REG_OFFSET) ==
        0u);
  icache_invalidate();
  CHECK(sram_virt_win[0] == 0x55556666u);

  // Writing 0xffffffff masks to 0x1 and enables Slot 1 translation.
  abs_mmio_write32(kIbexBase + RV_CORE_IBEX_DBUS_ADDR_EN_1_REG_OFFSET,
                   0xffffffffu);
  CHECK(abs_mmio_read32(kIbexBase + RV_CORE_IBEX_DBUS_ADDR_EN_1_REG_OFFSET) ==
        1u);
  icache_invalidate();
  CHECK(sram_virt_win[0] == 0x33334444u);

  // Enable Slot 0 mapping the same virtual range to sram_buf_a; Slot 0 must
  // take priority over Slot 1.
  CHECK_DIF_OK(dif_rv_core_ibex_configure_addr_translation(
      &ibex, kDifRvCoreIbexAddrTranslationSlot_0,
      kDifRvCoreIbexAddrTranslationDBus, map_slot0));
  CHECK_DIF_OK(dif_rv_core_ibex_enable_addr_translation(
      &ibex, kDifRvCoreIbexAddrTranslationSlot_0,
      kDifRvCoreIbexAddrTranslationDBus));
  icache_invalidate();
  CHECK(sram_virt_win[0] == 0x11112222u);

  // Disable Slot 0 -> falls back to Slot 1 (sram_buf_b).
  CHECK_DIF_OK(dif_rv_core_ibex_disable_addr_translation(
      &ibex, kDifRvCoreIbexAddrTranslationSlot_0,
      kDifRvCoreIbexAddrTranslationDBus));
  icache_invalidate();
  CHECK(sram_virt_win[0] == 0x33334444u);

  // Disable Slot 1 -> falls back to unmapped sram_virt_win.
  CHECK_DIF_OK(dif_rv_core_ibex_disable_addr_translation(
      &ibex, kDifRvCoreIbexAddrTranslationSlot_1,
      kDifRvCoreIbexAddrTranslationDBus));
  icache_invalidate();
  CHECK(sram_virt_win[0] == 0x55556666u);

  // Lock DBUS Slot 1 via DBUS_REGWEN_1 (RW0C) and verify further writes are
  // ignored.
  uint32_t saved_matching_1 = abs_mmio_read32(
      kIbexBase + RV_CORE_IBEX_DBUS_ADDR_MATCHING_1_REG_OFFSET);
  CHECK_DIF_OK(dif_rv_core_ibex_lock_addr_translation(
      &ibex, kDifRvCoreIbexAddrTranslationSlot_1,
      kDifRvCoreIbexAddrTranslationDBus));
  CHECK(abs_mmio_read32(kIbexBase + RV_CORE_IBEX_DBUS_REGWEN_1_REG_OFFSET) ==
        0u);
  // Writing 1 to RW0C REGWEN cannot unlock it.
  abs_mmio_write32(kIbexBase + RV_CORE_IBEX_DBUS_REGWEN_1_REG_OFFSET, 1u);
  CHECK(abs_mmio_read32(kIbexBase + RV_CORE_IBEX_DBUS_REGWEN_1_REG_OFFSET) ==
        0u);
  // Locked registers ignore writes.
  abs_mmio_write32(kIbexBase + RV_CORE_IBEX_DBUS_ADDR_EN_1_REG_OFFSET, 1u);
  abs_mmio_write32(kIbexBase + RV_CORE_IBEX_DBUS_ADDR_MATCHING_1_REG_OFFSET,
                   0xdeadbeefu);
  CHECK(abs_mmio_read32(kIbexBase + RV_CORE_IBEX_DBUS_ADDR_EN_1_REG_OFFSET) ==
        0u);
  CHECK(abs_mmio_read32(
            kIbexBase + RV_CORE_IBEX_DBUS_ADDR_MATCHING_1_REG_OFFSET) ==
        saved_matching_1);

  return true;
}
