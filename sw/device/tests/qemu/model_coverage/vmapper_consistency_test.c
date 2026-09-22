// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_rv_core_ibex.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "rv_core_ibex_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kIbexBase = TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR,
  kPageSize = 4096,
};

alignas(4096) static volatile uint32_t sram_buf_a[kPageSize / sizeof(uint32_t)];
alignas(4096) static volatile uint32_t sram_buf_b[kPageSize / sizeof(uint32_t)];
alignas(4096) static volatile uint32_t
    sram_virt_win[kPageSize / sizeof(uint32_t)];

bool test_main(void) {
  dif_rv_core_ibex_t ibex;
  CHECK_DIF_OK(dif_rv_core_ibex_init(mmio_region_from_addr(kIbexBase), &ibex));

  uint32_t dbus_regwen_0 =
      abs_mmio_read32(kIbexBase + RV_CORE_IBEX_DBUS_REGWEN_0_REG_OFFSET);
  uint32_t dbus_regwen_1 =
      abs_mmio_read32(kIbexBase + RV_CORE_IBEX_DBUS_REGWEN_1_REG_OFFSET);
  CHECK(dbus_regwen_0 == 1u);
  CHECK(dbus_regwen_1 == 1u);

  sram_buf_b[0] = 0x10101010u;
  sram_buf_b[256] = 0x20202020u;
  sram_buf_b[512] = 0x30303030u;
  sram_buf_a[0] = 0x9999aaaau;
  sram_virt_win[0] = 0x55556666u;
  sram_virt_win[256] = 0x66667777u;
  sram_virt_win[512] = 0x77778888u;

  // Configure Slot 1 (lower priority, prio = 1) as a 4 KiB NAPOT region
  // covering [sram_virt_win, sram_virt_win + 4095] -> sram_buf_b.
  dif_rv_core_ibex_addr_translation_mapping_t map_slot1 = {
      .matching_addr = (uintptr_t)sram_virt_win,
      .remap_addr = (uintptr_t)sram_buf_b,
      .size = kPageSize,
  };
  CHECK_DIF_OK(dif_rv_core_ibex_configure_addr_translation(
      &ibex, kDifRvCoreIbexAddrTranslationSlot_1,
      kDifRvCoreIbexAddrTranslationDBus, map_slot1));
  CHECK_DIF_OK(dif_rv_core_ibex_enable_addr_translation(
      &ibex, kDifRvCoreIbexAddrTranslationSlot_1,
      kDifRvCoreIbexAddrTranslationDBus));

  // Configure Slot 0 (higher priority, prio = 0) as an interior 1 KiB NAPOT
  // region covering [sram_virt_win + 1024, sram_virt_win + 2047] -> sram_buf_a.
  // Because start1 < start0 and end0 < end1, Slot 1 is split into left
  // [0..1023], interior Slot 0 [1024..2047], and right [2048..4095].
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

  // Configure Slot 0 (higher priority, prio = 0) at the SAME base address
  // [sram_virt_win, sram_virt_win + 1023] -> sram_buf_a (size = 1024 < 4096).
  // Because start0 == start1 and end0 < end1, Slot 0 overrides [0..1023] and
  // shifts Slot 1's active window to [1024..4095] -> sram_buf_b + 1024 (case
  // b1).
  dif_rv_core_ibex_addr_translation_mapping_t map_slot0_prefix = {
      .matching_addr = (uintptr_t)&sram_virt_win[0],
      .remap_addr = (uintptr_t)sram_buf_a,
      .size = 1024,
  };
  CHECK_DIF_OK(dif_rv_core_ibex_configure_addr_translation(
      &ibex, kDifRvCoreIbexAddrTranslationSlot_0,
      kDifRvCoreIbexAddrTranslationDBus, map_slot0_prefix));
  icache_invalidate();
  CHECK(sram_virt_win[0] == 0x9999aaaau);
  CHECK(sram_virt_win[256] == 0x20202020u);
  CHECK(sram_virt_win[512] == 0x30303030u);

  // Disable Slot 0 -> window falls back to full Slot 1 (sram_buf_b[0..1023]).
  CHECK_DIF_OK(dif_rv_core_ibex_disable_addr_translation(
      &ibex, kDifRvCoreIbexAddrTranslationSlot_0,
      kDifRvCoreIbexAddrTranslationDBus));
  icache_invalidate();
  CHECK(sram_virt_win[0] == 0x10101010u);
  CHECK(sram_virt_win[256] == 0x20202020u);
  CHECK(sram_virt_win[512] == 0x30303030u);

  // Disable Slot 1 -> all slices fall back to physical sram_virt_win.
  CHECK_DIF_OK(dif_rv_core_ibex_disable_addr_translation(
      &ibex, kDifRvCoreIbexAddrTranslationSlot_1,
      kDifRvCoreIbexAddrTranslationDBus));
  icache_invalidate();
  CHECK(sram_virt_win[0] == 0x55556666u);
  CHECK(sram_virt_win[256] == 0x66667777u);
  CHECK(sram_virt_win[512] == 0x77778888u);

  return true;
}
