// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file entropy_src_errata_v2_test.c
 * @brief CW340 FPGA Earlgrey v2 (`trunk-v2`) Errata Confirmation & Discovery
 * Suite for `entropy_src` (P09).
 *
 * Empirically verifies on physical CW340 FPGA silicon (`trunk-v2`):
 * 1. [entropy_src_core.sv:530-533, 626-762, 1945-2001]:
 *    - `RECOV_ALERT_STS.ES_THRESH_CFG_ALERT` (`entropy_src_core.sv:1945-1947`)
 *      and `*_FIELD_ALERT` are continuous combinational level signals (`de=1,
 *      d=1`) that override `rw0c` clears while `ALERT_THRESHOLD` is
 *      non-complementary (`0x12345678`) or an invalid `mubi4` value remains in
 *      a configuration CSR.
 *    - `REGWEN` uses `mubi4_test_false_loose(MODULE_ENABLE)` (`!= 0x6`), so
 *      writing `0x5` to `MODULE_ENABLE` keeps `REGWEN == 1`.
 * 2. [entropy_src_core.sv:1815-1887] vs [entropy_src.hjson:802-898]
 *    (NEW_IN_V2 + Refactored v1 Watermark Reset):
 *    - `u_entropy_src_ht_watermark_reg` (`entropy_src_core.sv:1873-1887`) uses
 *      a single shared 16-bit register (`event_cntr_q`) across all 9
 *      `HT_WATERMARK_NUM` selections and only clears on `health_test_clr`
 *      (`es_enable_pulse`). Writing `HT_WATERMARK_NUM = ADAPTP_LO` (`3`) while
 *      disabled leaves `HT_WATERMARK == 0x0000` until `MODULE_ENABLE = True`
 *      pulses `health_test_clr` (which sets `HT_WATERMARK = 0xFFFF`). When
 *      subsequently disabled and reconfigured to `HT_WATERMARK_NUM = ADAPTP_HI`
 *      (`2`), `dif_entropy_src_get_health_test_stats()` returns
 *      `watermark_num = 2` (`ADAPTP_HI`) paired with the stale `0xFFFF` low
 *      watermark from `ADAPTP_LO`.
 *    - Furthermore, for `REPCNT_HI` (`0`) and `REPCNTS_HI` (`1`),
 *      `ht_watermark_event_pre` is hardcoded to `1'b1` (`continuous`), so one
 *      cycle after `MODULE_ENABLE = True` (`es_delayed_enable == 1`),
 *      `HT_WATERMARK` immediately latches `0x0001` (`repcnt_event_cnt = 1`)
 *      even in `FW_OV_ENTROPY_INSERT = True` mode with zero RNG symbols tested.
 * 3. [entropy_src_core.sv:692-713] & [entropy_src_reg_top.sv:1123-1148] vs
 *    [entropy_src.hjson:608-625, 1412-1418] (NEW_IN_V2):
 *    - `THRESHOLD_ONEWAY` evaluates `mubi4_test_true_loose` (`!= 0x9`), so
 *      writing invalid `0x0` immediately activates one-way clamping across all
 *      health-test threshold registers (`REPCNT_THRESHOLD` refuses upward
 *      writes `0x3000 > 0x2000` while accepting downward writes
 *      `0x1800 < 0x2000`).
 *    - Because `u_threshold_oneway` uses `SwAccessW1S` with `Mubi = 1'b1`
 *      (`mubi4_or_hi`), once `THRESHOLD_ONEWAY == 0x0`, writing
 *      `kMultiBitBool4False` (`0x9`) computes `mubi4_or_hi(0x0, 0x9) == 0x0`
 *      and cannot clear `THRESHOLD_ONEWAY_FIELD_ALERT`. Furthermore,
 *      `THRESHOLD_ONEWAY` lacks `REGWEN` protection and remains writable when
 *      `SW_REGUPD == 0` (`REGWEN == 0`), requiring software to write
 *      `kMultiBitBool4True` (`0x6`) to clear `THRESHOLD_ONEWAY_FIELD_ALERT`.
 * 4. [entropy_src_core.sv:1033-1034, 2477-2478, 2851-2864] &
 *    [entropy_src_main_sm.sv:61-71, 220-237, 259-265]:
 *    - `FWInsertStart` (`0x0c3`) pulses `sha3_start_o = 1` on leaving `Idle`,
 *      absorbing `FW_OV_WR_DATA` words (`0x11111111, 0x22222222`) written prior
 *      to `FW_OV_SHA3_START = True` into Seed 1 (`0x1d9845a5`).
 *    - Clearing `FW_OV_SHA3_START` with 1 unprocessed 32-bit word
 *      (`0xdeadbeef`) in `u_prim_packer_fifo_precon` (`FW_OV_WR_FIFO_FULL ==
 * 0`) does not assert `ES_FW_OV_DISABLE_ALERT`.
 *    - Post-hash `MAIN_SM_STATE` immediately returns from `Idle` (`0x0f5`) to
 *      `FWInsertStart` (`0x0c3`, `DEBUG_STATUS.MAIN_SM_IDLE == 0`), and
 *      `ES_BUS_CMP_ALERT` compares `[63:0]` of consecutive seeds and asserts on
 *      the 12th `ENTROPY_DATA` read (`sfifo_esfinal_pop`).
 * 5. [entropy_src_core.sv:2811, 2818-2821, 2882-2899] &
 *    [entropy_src_reg_pkg.sv:791-842]:
 *    - Reading an empty `ENTROPY_DATA` returns `0` with `ERR_CODE == 0` on
 *      reads `1..11` (`swread_idx_q = 0..10`) and latches
 *      `SFIFO_ESFINAL_ERR | FIFO_READ_ERR` (`0x20000008`) only on the 12th
 *      read (`swread_idx_q == 11`).
 *    - `ENTROPY_SRC_PERMIT` rejects 8-bit `sb` writes to `CONF` (`4'b1111`,
 *      `mcause = 7`) while accepting 8-bit `sb` writes to byte 0 of
 *      `MODULE_ENABLE` (`4'b0001`).
 */

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/dif/dif_entropy_src.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/entropy_testutils.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top/entropy_src_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kEsBase = TOP_EARLGREY_ENTROPY_SRC_BASE_ADDR,
};

static volatile bool g_load_store_fault = false;
static volatile uint32_t g_last_mcause = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  g_load_store_fault = true;
  g_last_mcause = ibex_mcause_read();
  uint32_t mepc = ibex_mepc_read();
  uint16_t inst = *(volatile uint16_t *)mepc;
  uint32_t step = ((inst & 0x3u) == 0x3u) ? 4u : 2u;
  CSR_WRITE(CSR_REG_MEPC, mepc + step);
}

bool test_main(void) {
  LOG_INFO(
      "=== OpenTitan Earlgrey v2 (trunk-v2) ENTROPY_SRC Errata Suite (P09) "
      "===");
  CHECK_STATUS_OK(entropy_testutils_stop_all());

  dif_entropy_src_t entropy_src;
  CHECK_DIF_OK(
      dif_entropy_src_init(mmio_region_from_addr(kEsBase), &entropy_src));

  // ---------------------------------------------------------------------------
  // 1. [entropy_src_core.sv:530-533, 626-762, 1945-2001]:
  //    - ES_THRESH_CFG_ALERT level signal overrides rw0c clear while
  //      ALERT_THRESHOLD is non-complementary (0x12345678).
  //    - REGWEN stays 1 when MODULE_ENABLE is written with 0x5 (loose false).
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [entropy_src_core.sv:530-533, 1945-2001]: sticky "
      "ES_THRESH_CFG_ALERT & loose-false REGWEN");
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdEntropySrcRecovAlert));
  abs_mmio_write32(kEsBase + ENTROPY_SRC_ALERT_THRESHOLD_REG_OFFSET,
                   0x12345678u);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET, 0u);
  uint32_t recov_sts =
      abs_mmio_read32(kEsBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET);
  CHECK(
      bitfield_bit32_read(recov_sts,
                          ENTROPY_SRC_RECOV_ALERT_STS_ES_THRESH_CFG_ALERT_BIT),
      "[entropy_src_core.sv:1945-1947] Expected ES_THRESH_CFG_ALERT to remain "
      "1 after rw0c clear while ALERT_THRESHOLD=0x12345678 (got 0x%x)",
      recov_sts);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_ALERT_THRESHOLD_REG_OFFSET,
                   ENTROPY_SRC_ALERT_THRESHOLD_REG_RESVAL);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET, 0u);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdEntropySrcRecovAlert));

  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdEntropySrcRecovAlert));
  abs_mmio_write32(kEsBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET, 0x5u);
  uint32_t regwen = abs_mmio_read32(kEsBase + ENTROPY_SRC_REGWEN_REG_OFFSET);
  CHECK(regwen == 1u,
        "[entropy_src_core.sv:530-533] Expected REGWEN == 1 when "
        "MODULE_ENABLE=0x5 (mubi4_test_false_loose), got 0x%x",
        regwen);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   kMultiBitBool4False);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET, 0u);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdEntropySrcRecovAlert));
  LOG_INFO(
      "[entropy_src_core.sv:530-533, 1945-2001] CONFIRMED: ES_THRESH_CFG_ALERT "
      "sticky & REGWEN=1 on MODULE_ENABLE=0x5");

  // ---------------------------------------------------------------------------
  // 2. [entropy_src_core.sv:1815-1887] vs [entropy_src.hjson:802-898]
  //    (NEW_IN_V2 + Refactored v1 Watermark Behavior):
  //    - Configure HT_WATERMARK_NUM = ADAPTP_LO (3) while MODULE_ENABLE=False:
  //      HT_WATERMARK remains 0x0000 until MODULE_ENABLE=True pulses
  //      health_test_clr (which initializes HT_WATERMARK to 0xFFFF).
  //    - Disable MODULE_ENABLE=False and change HT_WATERMARK_NUM = ADAPTP_HI
  //      (2): dif_entropy_src_get_health_test_stats() reports watermark_num=2
  //      (ADAPTP_HI) paired with stale watermark=0xFFFF from ADAPTP_LO.
  //    - Configure HT_WATERMARK_NUM = REPCNT_HI (0) and enable MODULE_ENABLE in
  //      FW_OV_ENTROPY_INSERT mode (0 RNG symbols): HT_WATERMARK immediately
  //      latches 0x0001 on es_delayed_enable because ht_watermark_event_pre=1
  //      and repcnt_event_cnt resets to 1.
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [entropy_src_core.sv:1815-1887] (NEW_IN_V2): HT_WATERMARK "
      "shared flop retention & REPCNT_HI continuous latch of 1");
  uint32_t conf_fw_ov_sha3 =
      (kMultiBitBool4True << ENTROPY_SRC_CONF_FIPS_ENABLE_OFFSET) |
      (kMultiBitBool4True << ENTROPY_SRC_CONF_FIPS_FLAG_OFFSET) |
      (kMultiBitBool4False << ENTROPY_SRC_CONF_RNG_FIPS_OFFSET) |
      (kMultiBitBool4False << ENTROPY_SRC_CONF_RNG_BIT_ENABLE_OFFSET) |
      (kMultiBitBool4False << ENTROPY_SRC_CONF_THRESHOLD_SCOPE_OFFSET) |
      (kMultiBitBool4True << ENTROPY_SRC_CONF_ENTROPY_DATA_REG_ENABLE_OFFSET);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_CONF_REG_OFFSET, conf_fw_ov_sha3);
  uint32_t es_ctrl_sw_cond =
      (kMultiBitBool4True << ENTROPY_SRC_ENTROPY_CONTROL_ES_ROUTE_OFFSET) |
      (kMultiBitBool4False << ENTROPY_SRC_ENTROPY_CONTROL_ES_TYPE_OFFSET);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_ENTROPY_CONTROL_REG_OFFSET,
                   es_ctrl_sw_cond);
  uint32_t fw_ov_insert =
      (kMultiBitBool4True << ENTROPY_SRC_FW_OV_CONTROL_FW_OV_MODE_OFFSET) |
      (kMultiBitBool4True
       << ENTROPY_SRC_FW_OV_CONTROL_FW_OV_ENTROPY_INSERT_OFFSET);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_CONTROL_REG_OFFSET,
                   fw_ov_insert);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   kMultiBitBool4False);

  // Clear any residual REPCNT_HI watermark (e.g. ~0x000e) recorded by mask_rom
  // during boot by pulsing MODULE_ENABLE with HT_WATERMARK_NUM = BUCKET_HI (4)
  // in FW_OV_ENTROPY_INSERT mode (where health_test_done_pulse never fires, so
  // HT_WATERMARK deterministically clears to 0x0000).
  CHECK_DIF_OK(dif_entropy_src_watermark_configure(
      &entropy_src, kDifEntropySrcWatermarkNumBucketHi));
  abs_mmio_write32(kEsBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   kMultiBitBool4True);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   kMultiBitBool4False);

  CHECK_DIF_OK(dif_entropy_src_watermark_configure(
      &entropy_src, kDifEntropySrcWatermarkNumAdaptpLo));
  uint32_t wm_before_en =
      abs_mmio_read32(kEsBase + ENTROPY_SRC_HT_WATERMARK_REG_OFFSET);
  CHECK(wm_before_en == 0x0000u,
        "[entropy_src_core.sv:1873-1887] Expected HT_WATERMARK == 0x0000 "
        "before MODULE_ENABLE=True even with HT_WATERMARK_NUM=ADAPTP_LO, got "
        "0x%04x",
        wm_before_en);

  abs_mmio_write32(kEsBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   kMultiBitBool4True);
  uint32_t wm_adaptp_lo =
      abs_mmio_read32(kEsBase + ENTROPY_SRC_HT_WATERMARK_REG_OFFSET);
  CHECK(wm_adaptp_lo == 0xffffu,
        "[entropy_src_core.sv:1873-1887] Expected HT_WATERMARK == 0xFFFF "
        "after MODULE_ENABLE=True with HT_WATERMARK_NUM=ADAPTP_LO, got 0x%04x",
        wm_adaptp_lo);

  abs_mmio_write32(kEsBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   kMultiBitBool4False);
  CHECK_DIF_OK(dif_entropy_src_watermark_configure(
      &entropy_src, kDifEntropySrcWatermarkNumAdaptpHi));
  dif_entropy_src_health_test_stats_t stats;
  CHECK_DIF_OK(dif_entropy_src_get_health_test_stats(&entropy_src, &stats));
  CHECK(stats.watermark_num == kDifEntropySrcWatermarkNumAdaptpHi &&
            stats.watermark == 0xffffu,
        "[entropy_src_core.sv:1873-1887] Expected shared HT_WATERMARK to "
        "retain stale 0xFFFF from ADAPTP_LO after switching HT_WATERMARK_NUM "
        "to ADAPTP_HI while disabled (got num=%u, wm=0x%04x)",
        stats.watermark_num, stats.watermark);

  CHECK_DIF_OK(dif_entropy_src_watermark_configure(
      &entropy_src, kDifEntropySrcWatermarkNumRepcntHi));
  abs_mmio_write32(kEsBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   kMultiBitBool4True);
  uint32_t wm_repcnt_hi =
      abs_mmio_read32(kEsBase + ENTROPY_SRC_HT_WATERMARK_REG_OFFSET);
  CHECK(wm_repcnt_hi == 0x0001u,
        "[entropy_src_core.sv:1816-1820] Expected HT_WATERMARK == 0x0001 for "
        "REPCNT_HI immediately after MODULE_ENABLE=True with 0 RNG symbols "
        "tested, got 0x%04x",
        wm_repcnt_hi);
  LOG_INFO(
      "[entropy_src_core.sv:1815-1887] CONFIRMED: wm_before_en=0x%04x, "
      "wm_adaptp_lo=0x%04x, stale_adaptp_hi=0x%04x, wm_repcnt_hi=0x%04x",
      wm_before_en, wm_adaptp_lo, stats.watermark, wm_repcnt_hi);

  // ---------------------------------------------------------------------------
  // 3. [entropy_src_core.sv:1033-1034, 2477-2478, 2851-2864] &
  //    [entropy_src_main_sm.sv:61-71, 220-237, 259-265]:
  //    - Conditioned FW_OV_ENTROPY_INSERT enters FWInsertStart (0x0c3) with
  //      MAIN_SM_IDLE == 0, and pulses sha3_start_o immediately in Idle so
  //      words written in FWInsertStart (0x11111111, 0x22222222) BEFORE
  //      FW_OV_SHA3_START=True are absorbed into Seed 1 (word 0 == 0x1d9845a5).
  //    - Clearing FW_OV_SHA3_START with 1 odd 32-bit word (0xdeadbeef) in
  //      pfifo_precon (FW_OV_WR_FIFO_FULL == 0) does NOT assert
  //      ES_FW_OV_DISABLE_ALERT.
  //    - Popping an identical 2nd seed on the 12th ENTROPY_DATA read asserts
  //      ES_BUS_CMP_ALERT.
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [entropy_src_core.sv:1033-1034, 2477-2478, 2851-2864 / "
      "entropy_src_main_sm.sv:61-71]: FWInsertStart (0x0c3), pre-SHA3_START "
      "absorption, & ES_BUS_CMP_ALERT");
  abs_mmio_write32(kEsBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET, 0xfu);

  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_WR_DATA_REG_OFFSET, 0x11111111u);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_WR_DATA_REG_OFFSET, 0x22222222u);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   kMultiBitBool4True);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_WR_DATA_REG_OFFSET, 0xdeadbeefu);
  CHECK(abs_mmio_read32(kEsBase + ENTROPY_SRC_FW_OV_WR_FIFO_FULL_REG_OFFSET) ==
            0u,
        "Expected FW_OV_WR_FIFO_FULL == 0 after 1 odd 32-bit word");
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   kMultiBitBool4False);

  recov_sts = abs_mmio_read32(kEsBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET);
  CHECK(!bitfield_bit32_read(
            recov_sts, ENTROPY_SRC_RECOV_ALERT_STS_ES_FW_OV_DISABLE_ALERT_BIT),
        "[entropy_src_core.sv:2477-2478] Expected ES_FW_OV_DISABLE_ALERT == 0 "
        "when FW_OV_WR_FIFO_FULL == 0 despite 1 unprocessed 32-bit word in "
        "pfifo_precon");

  while (!bitfield_bit32_read(
      abs_mmio_read32(kEsBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET),
      ENTROPY_SRC_INTR_STATE_ES_ENTROPY_VALID_BIT)) {
  }

  uint32_t sm_state =
      abs_mmio_read32(kEsBase + ENTROPY_SRC_MAIN_SM_STATE_REG_OFFSET);
  uint32_t debug_status =
      abs_mmio_read32(kEsBase + ENTROPY_SRC_DEBUG_STATUS_REG_OFFSET);
  CHECK(sm_state == 0x0c3u &&
            !bitfield_bit32_read(debug_status,
                                 ENTROPY_SRC_DEBUG_STATUS_MAIN_SM_IDLE_BIT),
        "[entropy_src_main_sm.sv:61-71] Expected MAIN_SM_STATE == 0x0c3 "
        "(FWInsertStart) and MAIN_SM_IDLE == 0 after SHA3 completion");

  uint32_t seed1_w0 =
      abs_mmio_read32(kEsBase + ENTROPY_SRC_ENTROPY_DATA_REG_OFFSET);
  for (int i = 1; i < 12; ++i) {
    (void)abs_mmio_read32(kEsBase + ENTROPY_SRC_ENTROPY_DATA_REG_OFFSET);
  }
  abs_mmio_write32(kEsBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET, 0xfu);
  CHECK(
      seed1_w0 == 0x1d9845a5u,
      "[entropy_src_main_sm.sv:65-71] Expected words (0x11111111, 0x22222222) "
      "written in FWInsertStart prior to FW_OV_SHA3_START=True to be absorbed "
      "into SHA3-384 digest (0x1d9845a5), got 0x%08x",
      seed1_w0);

  // Seed 2: Pair 0xdeadbeef in pfifo_precon with 0xcafebabe and drain Seed 2.
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_WR_DATA_REG_OFFSET, 0xcafebabeu);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   kMultiBitBool4True);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   kMultiBitBool4False);
  while (!bitfield_bit32_read(
      abs_mmio_read32(kEsBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET),
      ENTROPY_SRC_INTR_STATE_ES_ENTROPY_VALID_BIT)) {
  }
  for (int i = 0; i < 12; ++i) {
    (void)abs_mmio_read32(kEsBase + ENTROPY_SRC_ENTROPY_DATA_REG_OFFSET);
  }
  abs_mmio_write32(kEsBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET, 0xfu);

  // Seed 3: Empty SHA3-384 sponge #1
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   kMultiBitBool4True);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   kMultiBitBool4False);
  while (!bitfield_bit32_read(
      abs_mmio_read32(kEsBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET),
      ENTROPY_SRC_INTR_STATE_ES_ENTROPY_VALID_BIT)) {
  }
  for (int i = 0; i < 12; ++i) {
    (void)abs_mmio_read32(kEsBase + ENTROPY_SRC_ENTROPY_DATA_REG_OFFSET);
  }
  abs_mmio_write32(kEsBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET, 0xfu);

  // Seed 4: Empty SHA3-384 sponge #2 (identical to Seed 3) -> ES_BUS_CMP_ALERT
  // fires on the 12th ENTROPY_DATA read!
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   kMultiBitBool4True);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   kMultiBitBool4False);
  while (!bitfield_bit32_read(
      abs_mmio_read32(kEsBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET),
      ENTROPY_SRC_INTR_STATE_ES_ENTROPY_VALID_BIT)) {
  }
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdEntropySrcRecovAlert));
  for (int i = 0; i < 12; ++i) {
    (void)abs_mmio_read32(kEsBase + ENTROPY_SRC_ENTROPY_DATA_REG_OFFSET);
  }
  recov_sts = abs_mmio_read32(kEsBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET);
  CHECK(bitfield_bit32_read(recov_sts,
                            ENTROPY_SRC_RECOV_ALERT_STS_ES_BUS_CMP_ALERT_BIT),
        "[entropy_src_core.sv:2851-2864] Expected ES_BUS_CMP_ALERT set on 12th "
        "read of identical seed (got 0x%x)",
        recov_sts);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET, 0u);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET, 0xfu);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdEntropySrcRecovAlert));
  LOG_INFO(
      "[entropy_src_core.sv:1033-1034, 2851-2864 / "
      "entropy_src_main_sm.sv:61-71] "
      "CONFIRMED: seed1_w0=0x%08x, MAIN_SM_STATE=0x%03x, ES_BUS_CMP_ALERT=1",
      seed1_w0, sm_state);

  // ---------------------------------------------------------------------------
  // 4. [entropy_src_core.sv:2811, 2818-2821]:
  //    Reading an empty ENTROPY_DATA returns 0 with ERR_CODE == 0 on
  //    reads 1..11 (swread_idx_q = 0..10) and only latches SFIFO_ESFINAL_ERR |
  //    FIFO_READ_ERR (0x20000008) on the 12th read (swread_idx_q == 11).
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [entropy_src_core.sv:2811]: ENTROPY_DATA empty underflow only "
      "fires on 12th read");
  CHECK_STATUS_OK(
      ottf_alerts_ignore_alert(kTopEarlgreyAlertIdEntropySrcFatalAlert));
  for (int i = 0; i < 11; ++i) {
    (void)abs_mmio_read32(kEsBase + ENTROPY_SRC_ENTROPY_DATA_REG_OFFSET);
  }
  uint32_t err_code_11 =
      abs_mmio_read32(kEsBase + ENTROPY_SRC_ERR_CODE_REG_OFFSET);
  CHECK(err_code_11 == 0u,
        "[entropy_src_core.sv:2811] Expected ERR_CODE == 0 after 11 empty "
        "ENTROPY_DATA reads, got 0x%08x",
        err_code_11);

  (void)abs_mmio_read32(kEsBase + ENTROPY_SRC_ENTROPY_DATA_REG_OFFSET);
  uint32_t err_code_12 =
      abs_mmio_read32(kEsBase + ENTROPY_SRC_ERR_CODE_REG_OFFSET);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   kMultiBitBool4False);
  CHECK(bitfield_bit32_read(err_code_12,
                            ENTROPY_SRC_ERR_CODE_SFIFO_ESFINAL_ERR_BIT) &&
            bitfield_bit32_read(err_code_12,
                                ENTROPY_SRC_ERR_CODE_FIFO_READ_ERR_BIT),
        "[entropy_src_core.sv:2811] Expected SFIFO_ESFINAL_ERR | FIFO_READ_ERR "
        "(0x20000008) on 12th empty read, got 0x%08x",
        err_code_12);
  LOG_INFO(
      "[entropy_src_core.sv:2811] CONFIRMED: ERR_CODE after 11 reads=0x%08x, "
      "after 12th read=0x%08x",
      err_code_11, err_code_12);

  // ---------------------------------------------------------------------------
  // 5. [entropy_src_core.sv:692-713, 1967-1986] &
  //    [entropy_src_reg_top.sv:1123-1148] vs [entropy_src.hjson:608-625,
  //    1412-1418] (NEW_IN_V2):
  //    - threshold_oneway_pfa drives
  //    RECOV_ALERT_STS.THRESHOLD_ONEWAY_FIELD_ALERT
  //      (bit 4), but is OMITTED from `assign recov_alert_state = ...` in
  //      entropy_src_core.sv:1967-1986! Therefore, writing 0x0 to
  //      THRESHOLD_ONEWAY sets THRESHOLD_ONEWAY_FIELD_ALERT == 1 in
  //      RECOV_ALERT_STS WITHOUT firing recov_alert_o (verified by OTTF alert
  //      catcher not triggering an unexpected alert!).
  //    - Furthermore, threshold_oneway_pfe uses mubi4_test_true_loose(0x0) ==
  //    1,
  //      immediately enabling one-way clamping (0x3000 > 0x2000 ignored,
  //      0x1800 < 0x2000 accepted), and SwAccessW1S (mubi4_or_hi) prevents
  //      restoring kMultiBitBool4False (0x9), while missing REGWEN gating
  //      allows writing kMultiBitBool4True (0x6) even when REGWEN == 0.
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [entropy_src_core.sv:692-713, 1967-1986 / "
      "entropy_src_reg_top.sv:1123-1148] (NEW_IN_V2): THRESHOLD_ONEWAY "
      "missing recov_alert_state wire, loose-true activation, SwAccessW1S "
      "trap, & missing REGWEN");
  abs_mmio_write32(kEsBase + ENTROPY_SRC_REPCNT_THRESHOLD_REG_OFFSET, 0x1000u);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_REPCNT_THRESHOLD_REG_OFFSET, 0x2000u);
  CHECK(abs_mmio_read32(kEsBase + ENTROPY_SRC_REPCNT_THRESHOLD_REG_OFFSET) ==
            0x2000u,
        "Expected REPCNT_THRESHOLD == 0x2000 while THRESHOLD_ONEWAY=False");

  // Do NOT call
  // ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdEntropySrcRecovAlert):
  // because threshold_oneway_pfa is omitted from recov_alert_state in
  // entropy_src_core.sv:1967-1986, recov_alert_o NEVER fires (if it did, OTTF's
  // alert catcher ISR would fail the test!).
  abs_mmio_write32(kEsBase + ENTROPY_SRC_THRESHOLD_ONEWAY_REG_OFFSET, 0x0u);
  uint32_t oneway_val =
      abs_mmio_read32(kEsBase + ENTROPY_SRC_THRESHOLD_ONEWAY_REG_OFFSET);
  CHECK(oneway_val == 0x0u,
        "[entropy_src_reg_top.sv:1124-1129] Expected THRESHOLD_ONEWAY == 0x0 "
        "after writing 0x0 (mubi4_or_hi(0x9, 0x0)), got 0x%x",
        oneway_val);

  // Even though THRESHOLD_ONEWAY == 0x0 (not kMultiBitBool4True), one-way
  // clamping is active because threshold_oneway_pfe uses mubi4_test_true_loose.
  abs_mmio_write32(kEsBase + ENTROPY_SRC_REPCNT_THRESHOLD_REG_OFFSET, 0x3000u);
  uint32_t repcnt_thresh_up =
      abs_mmio_read32(kEsBase + ENTROPY_SRC_REPCNT_THRESHOLD_REG_OFFSET);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_REPCNT_THRESHOLD_REG_OFFSET, 0x1800u);
  uint32_t repcnt_thresh_down =
      abs_mmio_read32(kEsBase + ENTROPY_SRC_REPCNT_THRESHOLD_REG_OFFSET);
  CHECK(repcnt_thresh_up == 0x2000u && repcnt_thresh_down == 0x1800u,
        "[entropy_src_core.sv:696] Expected one-way clamping active when "
        "THRESHOLD_ONEWAY=0x0 (up=0x%04x, down=0x%04x)",
        repcnt_thresh_up, repcnt_thresh_down);

  // Attempting to restore THRESHOLD_ONEWAY to kMultiBitBool4False (0x9) fails
  // because SwAccessW1S evaluates mubi4_or_hi(0x0, 0x9) == 0x0.
  abs_mmio_write32(kEsBase + ENTROPY_SRC_THRESHOLD_ONEWAY_REG_OFFSET,
                   kMultiBitBool4False);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET, 0u);
  oneway_val =
      abs_mmio_read32(kEsBase + ENTROPY_SRC_THRESHOLD_ONEWAY_REG_OFFSET);
  recov_sts = abs_mmio_read32(kEsBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET);
  CHECK(oneway_val == 0x0u &&
            bitfield_bit32_read(
                recov_sts,
                ENTROPY_SRC_RECOV_ALERT_STS_THRESHOLD_ONEWAY_FIELD_ALERT_BIT),
        "[entropy_src_reg_top.sv:1124-1129] Expected writing "
        "kMultiBitBool4False (0x9) to leave THRESHOLD_ONEWAY=0x0 and "
        "THRESHOLD_ONEWAY_FIELD_ALERT=1 without firing recov_alert_o (got "
        "oneway=0x%x, recov=0x%x)",
        oneway_val, recov_sts);

  // Clear SW_REGUPD to 0 so REGWEN == 0, then verify THRESHOLD_ONEWAY is still
  // writable (missing REGWEN protection) and writing kMultiBitBool4True (0x6)
  // clears the alert status bit.
  abs_mmio_write32(kEsBase + ENTROPY_SRC_SW_REGUPD_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kEsBase + ENTROPY_SRC_REGWEN_REG_OFFSET) == 0u,
        "Expected REGWEN == 0 after SW_REGUPD = 0");
  CHECK_DIF_OK(
      dif_entropy_src_health_test_threshold_oneway_enable(&entropy_src));
  oneway_val =
      abs_mmio_read32(kEsBase + ENTROPY_SRC_THRESHOLD_ONEWAY_REG_OFFSET);
  abs_mmio_write32(kEsBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET, 0u);
  recov_sts = abs_mmio_read32(kEsBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET);
  CHECK(oneway_val == kMultiBitBool4True &&
            !bitfield_bit32_read(
                recov_sts,
                ENTROPY_SRC_RECOV_ALERT_STS_THRESHOLD_ONEWAY_FIELD_ALERT_BIT),
        "[entropy_src_reg_top.sv:1124-1148] Expected THRESHOLD_ONEWAY to "
        "accept kMultiBitBool4True (0x6) while REGWEN==0 and clear "
        "THRESHOLD_ONEWAY_FIELD_ALERT (got oneway=0x%x, recov=0x%x)",
        oneway_val, recov_sts);
  LOG_INFO(
      "[entropy_src_core.sv:692-713, 1967-1986 / "
      "entropy_src_reg_top.sv:1123-1148] CONFIRMED: THRESHOLD_ONEWAY missing "
      "recov_alert_state wire, loose-true, SwAccessW1S trap, & writable when "
      "REGWEN=0");

  // ---------------------------------------------------------------------------
  // 6. [entropy_src_reg_pkg.sv:791-842] (SEC_CM: BUS.INTEGRITY):
  //    ENTROPY_SRC_PERMIT rejects 8-bit sb writes to CONF (4'b1111, mcause=7)
  //    while accepting 8-bit sb writes to byte 0 of MODULE_ENABLE (4'b0001).
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [entropy_src_reg_pkg.sv:791-842]: ENTROPY_SRC_PERMIT "
      "sub-word write protection");
  g_load_store_fault = false;
  g_last_mcause = 0;
  abs_mmio_write8(kEsBase + ENTROPY_SRC_CONF_REG_OFFSET, 0x66u);
  CHECK(g_load_store_fault && g_last_mcause == 7u,
        "[entropy_src_reg_pkg.sv:791-842] Expected sb to CONF (PERMIT=4'b1111) "
        "to fault with mcause=7");

  g_load_store_fault = false;
  abs_mmio_write8(kEsBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                  kMultiBitBool4False);
  CHECK(!g_load_store_fault,
        "[entropy_src_reg_pkg.sv:791-842] Expected sb to MODULE_ENABLE+0 "
        "(PERMIT=4'b0001) to succeed");
  LOG_INFO(
      "[entropy_src_reg_pkg.sv:791-842] CONFIRMED: ENTROPY_SRC_PERMIT "
      "enforced");

  LOG_INFO("=== ALL ENTROPY_SRC V2 ERRATA CHECKS PASSED ===");
  return true;
}
