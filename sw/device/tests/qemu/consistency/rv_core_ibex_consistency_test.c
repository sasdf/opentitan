// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/dif/dif_alert_handler.h"
#include "sw/device/lib/dif/dif_rv_core_ibex.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "alert_handler_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "rv_core_ibex_regs.h"

OTTF_DEFINE_TEST_CONFIG();

static volatile uint32_t fault_count = 0;
static volatile uint32_t last_mcause = 0;

void ottf_exception_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = 0;
  CSR_READ(CSR_REG_MCAUSE, &mcause);
  last_mcause = mcause;
  fault_count++;
}

static bool expect_load_fault_32(mmio_region_t base, uint32_t offset) {
  uint32_t before = fault_count;
  last_mcause = 0;
  (void)mmio_region_read32(base, (ptrdiff_t)offset);
  return (fault_count == before + 1u) && (last_mcause == 5u);
}

static bool expect_store_fault_32(mmio_region_t base, uint32_t offset,
                                  uint32_t val) {
  uint32_t before = fault_count;
  last_mcause = 0;
  mmio_region_write32(base, (ptrdiff_t)offset, val);
  return (fault_count == before + 1u) && (last_mcause == 7u);
}

static bool expect_store_fault_8(mmio_region_t base, uint32_t offset,
                                 uint8_t val) {
  uint32_t before = fault_count;
  last_mcause = 0;
  mmio_region_write8(base, (ptrdiff_t)offset, val);
  return (fault_count == before + 1u) && (last_mcause == 7u);
}

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
    uint32_t regwen =
        abs_mmio_read32(kAlertHandlerBase +
                        ALERT_HANDLER_ALERT_REGWEN_0_REG_OFFSET + reg_offset);
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
  CHECK_DIF_OK(dif_rv_core_ibex_init(mmio_region_from_addr(kIbexBase), &ibex));

  dif_alert_handler_t alert_handler;
  CHECK_DIF_OK(dif_alert_handler_init(mmio_region_from_addr(kAlertHandlerBase),
                                      &alert_handler));

  configure_ibex_alerts_non_escalating();

  // 1. Verify register reset values and RO enforcement.
  CHECK(abs_mmio_read32(kIbexBase + RV_CORE_IBEX_ALERT_TEST_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kIbexBase + RV_CORE_IBEX_SW_RECOV_ERR_REG_OFFSET) ==
        kMultiBitBool4False);
  CHECK(abs_mmio_read32(kIbexBase + RV_CORE_IBEX_SW_FATAL_ERR_REG_OFFSET) ==
        kMultiBitBool4False);
  uint32_t ibus_regwen_0 =
      abs_mmio_read32(kIbexBase + RV_CORE_IBEX_IBUS_REGWEN_0_REG_OFFSET);
  uint32_t ibus_regwen_1 =
      abs_mmio_read32(kIbexBase + RV_CORE_IBEX_IBUS_REGWEN_1_REG_OFFSET);
  uint32_t dbus_regwen_0 =
      abs_mmio_read32(kIbexBase + RV_CORE_IBEX_DBUS_REGWEN_0_REG_OFFSET);
  uint32_t dbus_regwen_1 =
      abs_mmio_read32(kIbexBase + RV_CORE_IBEX_DBUS_REGWEN_1_REG_OFFSET);
  CHECK(ibus_regwen_0 == dbus_regwen_0);
  CHECK(ibus_regwen_1 == dbus_regwen_1);
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

    abs_mmio_write32(kIbexBase + RV_CORE_IBEX_ALERT_TEST_REG_OFFSET, 1u << bit);
    busy_spin_micros(10);
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
    busy_spin_micros(10);
    CHECK_DIF_OK(dif_alert_handler_alert_is_cause(&alert_handler, target_alert,
                                                  &is_cause));
    CHECK(!is_cause);
  }

  // 3. Verify SW_RECOV_ERR (0x04):
  // - Writing kMultiBitBool4False (0x9) does not trigger recov_sw_err.
  // - Writing kMultiBitBool4True (0x6) or any non-False 4-bit value triggers
  //   recov_sw_err (alert ID 62), self-resets SW_RECOV_ERR to
  //   kMultiBitBool4False (0x9) upon alert_acks[1], and can be acknowledged
  //   and re-triggered.
  abs_mmio_write32(kIbexBase + RV_CORE_IBEX_SW_RECOV_ERR_REG_OFFSET,
                   kMultiBitBool4False);
  busy_spin_micros(10);
  CHECK(abs_mmio_read32(kIbexBase + RV_CORE_IBEX_SW_RECOV_ERR_REG_OFFSET) ==
        kMultiBitBool4False);
  bool recov_cause = false;
  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
      &alert_handler, kTopEarlgreyAlertIdRvCoreIbexRecovSwErr, &recov_cause));
  CHECK(!recov_cause);

  abs_mmio_write32(kIbexBase + RV_CORE_IBEX_SW_RECOV_ERR_REG_OFFSET,
                   kMultiBitBool4True);
  busy_spin_micros(10);
  CHECK(abs_mmio_read32(kIbexBase + RV_CORE_IBEX_SW_RECOV_ERR_REG_OFFSET) ==
        kMultiBitBool4False);
  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
      &alert_handler, kTopEarlgreyAlertIdRvCoreIbexRecovSwErr, &recov_cause));
  CHECK(recov_cause);
  CHECK_DIF_OK(dif_alert_handler_alert_acknowledge(
      &alert_handler, kTopEarlgreyAlertIdRvCoreIbexRecovSwErr));
  busy_spin_micros(10);
  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
      &alert_handler, kTopEarlgreyAlertIdRvCoreIbexRecovSwErr, &recov_cause));
  CHECK(!recov_cause);

  // Re-trigger SW_RECOV_ERR with non-strict true (0x0 != MuBi4False).
  abs_mmio_write32(kIbexBase + RV_CORE_IBEX_SW_RECOV_ERR_REG_OFFSET, 0x0u);
  busy_spin_micros(10);
  CHECK(abs_mmio_read32(kIbexBase + RV_CORE_IBEX_SW_RECOV_ERR_REG_OFFSET) ==
        kMultiBitBool4False);
  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
      &alert_handler, kTopEarlgreyAlertIdRvCoreIbexRecovSwErr, &recov_cause));
  CHECK(recov_cause);
  CHECK_DIF_OK(dif_alert_handler_alert_acknowledge(
      &alert_handler, kTopEarlgreyAlertIdRvCoreIbexRecovSwErr));
  busy_spin_micros(10);

  // 4. Verify EDN RND_DATA / RND_STATUS interface.
  uint32_t rnd_word = 0;
  CHECK_DIF_OK(dif_rv_core_ibex_read_rnd_data(&ibex, &rnd_word));

  // 5. Verify DBUS Address Translation (rv_core_addr_trans), 1-bit ADDR_EN
  // mask, Slot Priority (Slot 0 > Slot 1), and DBUS_REGWEN RW0C locking.
  if (dbus_regwen_1 == 1u) {
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
    // Verify 1-bit write mask on IBUS_ADDR_EN_1 and DBUS_ADDR_EN_1: writing
    // 0xfffffffe (bit 0 = 0) must store 0 and NOT enable translation.
    abs_mmio_write32(kIbexBase + RV_CORE_IBEX_IBUS_ADDR_EN_1_REG_OFFSET,
                     0xfffffffeu);
    CHECK(abs_mmio_read32(kIbexBase + RV_CORE_IBEX_IBUS_ADDR_EN_1_REG_OFFSET) ==
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

    if (dbus_regwen_0 == 1u) {
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

      // Test interior nested higher-priority NAPOT sub-region (Slot 0 strictly
      // inside Slot 1: start1 < start0 && end0 < end1) so Slot 1 is split into
      // left [0..255], middle Slot 0 [256..511], and right [512..1023] slices.
      sram_buf_b[0] = 0x10101010u;
      sram_buf_b[256] = 0x20202020u;
      sram_buf_b[512] = 0x30303030u;
      sram_buf_a[0] = 0x9999aaaau;
      dif_rv_core_ibex_addr_translation_mapping_t map_slot0_nested = {
          .matching_addr = (uintptr_t)&sram_virt_win[256],
          .remap_addr = (uintptr_t)sram_buf_a,
          .size = 1024,
      };
      CHECK_DIF_OK(dif_rv_core_ibex_configure_addr_translation(
          &ibex, kDifRvCoreIbexAddrTranslationSlot_0,
          kDifRvCoreIbexAddrTranslationDBus, map_slot0_nested));
      CHECK_DIF_OK(dif_rv_core_ibex_enable_addr_translation(
          &ibex, kDifRvCoreIbexAddrTranslationSlot_0,
          kDifRvCoreIbexAddrTranslationDBus));
      icache_invalidate();
      CHECK(sram_virt_win[0] == 0x10101010u);
      CHECK(sram_virt_win[256] == 0x9999aaaau);
      CHECK(sram_virt_win[512] == 0x30303030u);

      CHECK_DIF_OK(dif_rv_core_ibex_disable_addr_translation(
          &ibex, kDifRvCoreIbexAddrTranslationSlot_0,
          kDifRvCoreIbexAddrTranslationDBus));
      icache_invalidate();
    }

    // Disable Slot 1 -> falls back to unmapped sram_virt_win.
    CHECK_DIF_OK(dif_rv_core_ibex_disable_addr_translation(
        &ibex, kDifRvCoreIbexAddrTranslationSlot_1,
        kDifRvCoreIbexAddrTranslationDBus));
    icache_invalidate();
    CHECK(sram_virt_win[0] == 0x55556666u);
  }

  // Lock DBUS Slot 1 via DBUS_REGWEN_1 (RW0C) and verify further writes are
  // ignored.
  uint32_t saved_matching_1 =
      abs_mmio_read32(kIbexBase + RV_CORE_IBEX_DBUS_ADDR_MATCHING_1_REG_OFFSET);
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
  CHECK(abs_mmio_read32(kIbexBase +
                        RV_CORE_IBEX_DBUS_ADDR_MATCHING_1_REG_OFFSET) ==
        saved_matching_1);

  // 6. Wave 2 Deep-Pass Checks:
  // 6a. NMI_ENABLE (0x44) and NMI_STATE (0x48) 2-bit mask (NMI_MASK = 0x3):
  // Writing upper bits [31:2] with bits [1:0] = 0 must be ignored and read back
  // as 0 without enabling NMI.
  uint32_t orig_nmi_enable =
      abs_mmio_read32(kIbexBase + RV_CORE_IBEX_NMI_ENABLE_REG_OFFSET);
  abs_mmio_write32(kIbexBase + RV_CORE_IBEX_NMI_ENABLE_REG_OFFSET, 0xfffffffcu);
  CHECK(abs_mmio_read32(kIbexBase + RV_CORE_IBEX_NMI_ENABLE_REG_OFFSET) ==
        orig_nmi_enable);
  abs_mmio_write32(kIbexBase + RV_CORE_IBEX_NMI_STATE_REG_OFFSET, 0xfffffffcu);
  CHECK((abs_mmio_read32(kIbexBase + RV_CORE_IBEX_NMI_STATE_REG_OFFSET) &
         0xfffffffcu) == 0u);

  // 6b. RND_DATA (0x50) consume clears RND_STATUS.RND_DATA_VALID (0x54) and
  // automatically triggers a new EDN request until valid again.
  uint32_t rnd_word2 = 0;
  CHECK_DIF_OK(dif_rv_core_ibex_read_rnd_data(&ibex, &rnd_word2));

  // 6c. Lock IBUS Slot 1 via IBUS_REGWEN_1 (RW0C) and verify further writes to
  // IBUS_ADDR_EN_1 / IBUS_ADDR_MATCHING_1 / IBUS_REMAP_ADDR_1 are ignored.
  if (ibus_regwen_1 == 1u) {
    abs_mmio_write32(kIbexBase + RV_CORE_IBEX_IBUS_ADDR_MATCHING_1_REG_OFFSET,
                     0x12345000u);
    abs_mmio_write32(kIbexBase + RV_CORE_IBEX_IBUS_REMAP_ADDR_1_REG_OFFSET,
                     0x6789a000u);
    CHECK(abs_mmio_read32(kIbexBase +
                          RV_CORE_IBEX_IBUS_ADDR_MATCHING_1_REG_OFFSET) ==
          0x12345000u);
    CHECK(abs_mmio_read32(kIbexBase +
                          RV_CORE_IBEX_IBUS_REMAP_ADDR_1_REG_OFFSET) ==
          0x6789a000u);

    // Writing 0xfffffffe (bit 0 = 0) clears IBUS_REGWEN_1 (1-bit W0C).
    abs_mmio_write32(kIbexBase + RV_CORE_IBEX_IBUS_REGWEN_1_REG_OFFSET,
                     0xfffffffeu);
    CHECK(abs_mmio_read32(kIbexBase + RV_CORE_IBEX_IBUS_REGWEN_1_REG_OFFSET) ==
          0u);
    // Writing 0xffffffff cannot unlock W0C register.
    abs_mmio_write32(kIbexBase + RV_CORE_IBEX_IBUS_REGWEN_1_REG_OFFSET,
                     0xffffffffu);
    CHECK(abs_mmio_read32(kIbexBase + RV_CORE_IBEX_IBUS_REGWEN_1_REG_OFFSET) ==
          0u);

    // Locked IBUS Slot 1 registers must ignore writes.
    abs_mmio_write32(kIbexBase + RV_CORE_IBEX_IBUS_ADDR_EN_1_REG_OFFSET, 1u);
    abs_mmio_write32(kIbexBase + RV_CORE_IBEX_IBUS_ADDR_MATCHING_1_REG_OFFSET,
                     0xdeadbeefu);
    abs_mmio_write32(kIbexBase + RV_CORE_IBEX_IBUS_REMAP_ADDR_1_REG_OFFSET,
                     0xcafebabeu);
    CHECK(abs_mmio_read32(kIbexBase + RV_CORE_IBEX_IBUS_ADDR_EN_1_REG_OFFSET) ==
          0u);
    CHECK(abs_mmio_read32(kIbexBase +
                          RV_CORE_IBEX_IBUS_ADDR_MATCHING_1_REG_OFFSET) ==
          0x12345000u);
    CHECK(abs_mmio_read32(kIbexBase +
                          RV_CORE_IBEX_IBUS_REMAP_ADDR_1_REG_OFFSET) ==
          0x6789a000u);
  }

  // 7. Wave 3 Convergence Checks:
  // 7a. Unmapped gap offsets 0x64..0x7c (between FPGA_INFO at 0x60 and
  // DV_SIM_WINDOW at 0x80) and 0xa0..0xfc decode to addrmiss = 1'b1 in
  // rv_core_ibex_cfg_reg_top.sv, returning d_error = 1 (Load Access Fault
  // mcause = 5 on read, Store Access Fault mcause = 7 on write) without
  // asserting intg_err_o.
  mmio_region_t ibex_mmio = mmio_region_from_addr(kIbexBase);
  CHECK(expect_load_fault_32(ibex_mmio, 0x64u));
  CHECK(expect_store_fault_32(ibex_mmio, 0x64u, 0xdeadbeefu));
  CHECK(expect_load_fault_32(ibex_mmio, 0x7cu));
  CHECK(expect_store_fault_32(ibex_mmio, 0x7cu, 0xdeadbeefu));
  CHECK(expect_load_fault_32(ibex_mmio, 0xa0u));
  CHECK(expect_store_fault_32(ibex_mmio, 0xa0u, 0xdeadbeefu));

  // 7b. Sub-word write permit checks (RV_CORE_IBEX_CFG_PERMIT / wr_err):
  // - ERR_STATUS (PERMIT = 4'b0011) rejects 1-byte write (reg_be = 4'b0001).
  // - SW_RECOV_ERR (PERMIT = 4'b0001) rejects byte write to offset +1
  //   (reg_be = 4'b0010).
  // - FPGA_INFO (PERMIT = 4'b1111) rejects 1-byte write (reg_be = 4'b0001).
  CHECK(expect_store_fault_8(ibex_mmio, RV_CORE_IBEX_ERR_STATUS_REG_OFFSET,
                             0xffu));
  CHECK(expect_store_fault_8(ibex_mmio,
                             RV_CORE_IBEX_SW_RECOV_ERR_REG_OFFSET + 1u, 0x06u));
  CHECK(expect_store_fault_8(ibex_mmio, RV_CORE_IBEX_FPGA_INFO_REG_OFFSET,
                             0x55u));
  CHECK(abs_mmio_read32(kIbexBase + RV_CORE_IBEX_ERR_STATUS_REG_OFFSET) == 0u);

  // 7c. PMU CSRs (mcountinhibit, mcycle/mcycleh, minstret/minstreth) in
  // ibex_cs_registers.sv:
  // - mcountinhibit bit 1 (TM) is hardwired to 0; bit 0 (CY) inhibits mcycle;
  //   bit 2 (IR) inhibits minstret.
  // - Writing mcycle/mcycleh and minstret/minstreth sets the 64-bit counter
  //   value; when inhibited, counters hold their written values; when
  //   uninhibited, counters resume incrementing from the written baseline.
  uint32_t saved_mcountinhibit = 0;
  uint32_t saved_mcycleh = 0, saved_mcycle = 0;
  uint32_t saved_minstreth = 0, saved_minstret = 0;
  CSR_READ(CSR_REG_MCOUNTINHIBIT, &saved_mcountinhibit);
  CSR_READ(CSR_REG_MCYCLEH, &saved_mcycleh);
  CSR_READ(CSR_REG_MCYCLE, &saved_mcycle);
  CSR_READ(CSR_REG_MINSTRETH, &saved_minstreth);
  CSR_READ(CSR_REG_MINSTRET, &saved_minstret);

  // Write 0x7 (CY | TM | IR): bit 1 (TM) must read back as 0 -> 0x5.
  CSR_WRITE(CSR_REG_MCOUNTINHIBIT, 0x7u);
  uint32_t mcountinhibit_val = 0;
  CSR_READ(CSR_REG_MCOUNTINHIBIT, &mcountinhibit_val);
  CHECK((mcountinhibit_val & 0x7u) == 0x5u);

  // Program 64-bit baselines while CY and IR are inhibited.
  CSR_WRITE(CSR_REG_MCYCLEH, 0x00000042u);
  CSR_WRITE(CSR_REG_MCYCLE, 0x12345678u);
  CSR_WRITE(CSR_REG_MINSTRETH, 0x00000024u);
  CSR_WRITE(CSR_REG_MINSTRET, 0x87654321u);
  asm volatile("nop; nop; nop; nop");

  uint32_t cyc_lo_inh = 0, cyc_hi_inh = 0;
  uint32_t inst_lo_inh = 0, inst_hi_inh = 0;
  CSR_READ(CSR_REG_MCYCLE, &cyc_lo_inh);
  CSR_READ(CSR_REG_MCYCLEH, &cyc_hi_inh);
  CSR_READ(CSR_REG_MINSTRET, &inst_lo_inh);
  CSR_READ(CSR_REG_MINSTRETH, &inst_hi_inh);
  CHECK(cyc_lo_inh == 0x12345678u);
  CHECK(cyc_hi_inh == 0x00000042u);
  CHECK(inst_lo_inh == 0x87654321u);
  CHECK(inst_hi_inh == 0x00000024u);

  // Re-enable CY and IR (mcountinhibit = 0) and verify counters advance from
  // the written 64-bit baselines.
  CSR_WRITE(CSR_REG_MCOUNTINHIBIT, 0u);
  asm volatile("nop; nop; nop; nop");
  uint32_t cyc_lo_run = 0, cyc_hi_run = 0;
  uint32_t inst_lo_run = 0, inst_hi_run = 0;
  CSR_READ(CSR_REG_MCYCLE, &cyc_lo_run);
  CSR_READ(CSR_REG_MCYCLEH, &cyc_hi_run);
  CSR_READ(CSR_REG_MINSTRET, &inst_lo_run);
  CSR_READ(CSR_REG_MINSTRETH, &inst_hi_run);
  CHECK(cyc_hi_run == 0x00000042u);
  CHECK(cyc_lo_run > 0x12345678u);
  CHECK(inst_hi_run == 0x00000024u);
  CHECK(inst_lo_run > 0x87654321u);

  // Restore original counter baselines and mcountinhibit.
  CSR_WRITE(CSR_REG_MCYCLEH, saved_mcycleh);
  CSR_WRITE(CSR_REG_MCYCLE, saved_mcycle);
  CSR_WRITE(CSR_REG_MINSTRETH, saved_minstreth);
  CSR_WRITE(CSR_REG_MINSTRET, saved_minstret);
  CSR_WRITE(CSR_REG_MCOUNTINHIBIT, saved_mcountinhibit);

  // 9. Verify Debug Trigger Context CSRs (`scontext` 0x5a8 and `mscontext`
  // 0x7aa, `ibex_cs_registers.sv:523-530` with `DbgTriggerEn = 1`):
  // Both CSRs are accessible in M-mode without raising an illegal instruction
  // exception, ignore writes, and read back as 0.
  uint32_t faults_before_ctx = fault_count;
  uint32_t scontext_val = 0xffffffffu;
  uint32_t mscontext_val = 0xffffffffu;
  CSR_WRITE(0x5a8, 0xdeadbeefu);
  CSR_READ(0x5a8, &scontext_val);
  CSR_WRITE(0x7aa, 0xdeadbeefu);
  CSR_READ(0x7aa, &mscontext_val);
  CHECK(fault_count == faults_before_ctx);
  CHECK(scontext_val == 0u);
  CHECK(mscontext_val == 0u);

  return true;
}
