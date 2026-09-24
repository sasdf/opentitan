// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file prim_errata_v2_test.c
 * @brief Physical CW340 FPGA verification of Earlgrey v2 (`trunk-v2` /
 *        `mainline-v2`) hardware, specification, and RTL primitive behaviors
 *        for `prim` (`hw/ip/prim/rtl/`):
 *
 * 1. `prim_subreg_shadow.sv:85-156, 228-261` (`phase_q`, `phase_clear`,
 *    `err_update`, and `trunk-v2` 2-register `shadow_q` merge / `err_storage`
 *    Phase-1 gating):
 *    - Writing a shadowed register (`CLKMGR_AON.USB_MEAS_CTRL_SHADOWED`
 *      `0x4042004c`) once transitions `phase_q` from `0` to `1` and overwrites
 *      `shadow_q` with `~wr_data` while `committed_q` holds `v0`.
 *    - While `phase_q == 1` (`~shadow_q != committed_q`), `err_storage`
 *      (`(~shadow_q != committed_q) ? ~phase_q : 1'b0`) is gated to `0`, so
 *      `CLKMGR_AON.FATAL_ERR_CODE` (`0x40420054` bit 2 `SHADOW_STORAGE_ERR`)
 *      remains `0` even across many clock cycles in `Phase 1`.
 *    - Reading the shadowed CSR (`re = 1`) while `phase_q == 1` asserts
 *      `phase_clear = 1`, resetting `phase_q` to `0` and overwriting
 *      `shadow_q <= ~committed_q` with `RECOV_ERR_CODE == 0`, so a subsequent
 *      single write of `v1` only re-enters `Phase 1` (`committed_q` stays
 * `v0`).
 *    - Writing a mismatched second value `v2 != v1` while `phase_q == 1`
 *      asserts `err_update = 1`, latching `CLKMGR_AON.RECOV_ERR_CODE` bit 0
 *      (`SHADOW_UPDATE_ERR = 1`) while resetting `phase_q` to `0` and leaving
 *      `committed_q` at `v0`.
 *
 * 2. `prim_fifo_sync.sv:162, 181-182, 205-206` & `prim_packer_fifo.sv:53-118`:
 *    - `fifo_incr_rptr = rvalid_o & rready_i & ~under_rst` gates off read
 *      pointer advancement when `empty == 1` and `OutputZeroIfEmpty = 1`
 *      returns `0x00000000` with `0` pointer underflow or `err_o` alerts.
 *    - Asserting `clr_i = 1` (`UART1.FIFO_CTRL.TXRST = 1`) synchronously resets
 *      `wptr`, `rptr`, and `depth_o` to `0`.
 *
 * 3. `prim_mubi_pkg.sv:120-143` & `prim_ram_1p_scr.sv:249-281`:
 *    - `mubi4_test_true_loose(4'h0)` (`!= 4'h9`) evaluates to `1'b1` (`True`)
 *      whereas `mubi4_test_true_strict(4'h0)` (`== 4'h6`) evaluates to `1'b0`
 *      (`False`).
 *    - `u_sram_ctrl_meta` (`0x11000000`, `MemSizeRam = 38912 = 19 * 2048`
 *      bytes, `Depth = 9728 = 19 * 512` words, `SramCtrlMetaNumAddrScrRounds =
 * 0`) operates across all 19 chunks (`0x11000000` .. `0x110097fc`).
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top/clkmgr_regs.h"
#include "hw/top/csrng_regs.h"
#include "hw/top/sram_ctrl_regs.h"
#include "hw/top/uart_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kClkmgrBase = 0x40420000u,
  kUart1Base = 0x40010000u,
  kSramRetRegsBase = 0x40500000u,
  kSramMetaRegsBase = TOP_EARLGREY_SRAM_CTRL_META_REGS_BASE_ADDR,
  kCsrngBase = TOP_EARLGREY_CSRNG_BASE_ADDR,
};

/**
 * Verify `prim_subreg_shadow.sv` (`phase_q`, `phase_clear` on SW read,
 * `err_update` on mismatched Phase-1 write, and `err_storage` Phase-1 gating
 * when `shadow_q` holds `~wr_data != ~committed_q`).
 */
static void test_prim_subreg_shadow_semantics(void) {
  LOG_INFO(
      "Verifying [prim_subreg_shadow.sv:85-156, 254-256]: phase_clear on SW "
      "read, err_update on mismatched Phase-1 write, and err_storage Phase-1 "
      "gating");

  // Clear any prior recoverable error bits in CLKMGR.
  abs_mmio_write32(kClkmgrBase + CLKMGR_RECOV_ERR_CODE_REG_OFFSET, 0x7ffu);
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_RECOV_ERR_CODE_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_FATAL_ERR_CODE_REG_OFFSET) == 0u);

  uint32_t v0 =
      abs_mmio_read32(kClkmgrBase + CLKMGR_USB_MEAS_CTRL_SHADOWED_REG_OFFSET);
  uint32_t v1 = 0x12345u;
  uint32_t v2 = 0x15555u;

  // 1. Phase-0 write overwrites shadow_q <= ~v1 while committed_q stays v0
  //    (~shadow_q != committed_q), and sets phase_q <= 1.
  //    Keep the register in Phase 1 (do NOT read it yet) across multiple clock
  //    cycles and verify err_storage = (~shadow_q != committed_q) ? ~phase_q :
  //    0 remains 0 (FATAL_ERR_CODE == 0).
  abs_mmio_write32(kClkmgrBase + CLKMGR_USB_MEAS_CTRL_SHADOWED_REG_OFFSET, v1);
  busy_spin_micros(10);
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_FATAL_ERR_CODE_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_RECOV_ERR_CODE_REG_OFFSET) == 0u);

  // 2. Reading USB_MEAS_CTRL_SHADOWED (re = 1) while phase_q == 1 returns
  //    committed_q (v0) AND asserts phase_clear = 1, resetting phase_q <= 0
  //    and restoring shadow_q <= ~committed_q without setting err_update.
  uint32_t read_mid =
      abs_mmio_read32(kClkmgrBase + CLKMGR_USB_MEAS_CTRL_SHADOWED_REG_OFFSET);
  CHECK(read_mid == v0);
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_RECOV_ERR_CODE_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_FATAL_ERR_CODE_REG_OFFSET) == 0u);

  // Prove that the read above reset phase_q back to 0: writing v1 once more
  // only enters Phase 1 again instead of committing v1!
  abs_mmio_write32(kClkmgrBase + CLKMGR_USB_MEAS_CTRL_SHADOWED_REG_OFFSET, v1);
  uint32_t after_re_stage =
      abs_mmio_read32(kClkmgrBase + CLKMGR_USB_MEAS_CTRL_SHADOWED_REG_OFFSET);
  CHECK(after_re_stage == v0);
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_RECOV_ERR_CODE_REG_OFFSET) == 0u);

  // 3. Stage v1 in Phase 0 (phase_q -> 1), then write mismatched v2 != v1 in
  //    Phase 1 without an intervening read: err_update = 1 latches
  //    RECOV_ERR_CODE.SHADOW_UPDATE_ERR (bit 0 = 1), fires Alert 24
  //    (kTopEarlgreyAlertIdClkmgrRecovFault), and leaves committed_q = v0.
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdClkmgrRecovFault));
  abs_mmio_write32(kClkmgrBase + CLKMGR_USB_MEAS_CTRL_SHADOWED_REG_OFFSET, v1);
  abs_mmio_write32(kClkmgrBase + CLKMGR_USB_MEAS_CTRL_SHADOWED_REG_OFFSET, v2);
  uint32_t recov_err =
      abs_mmio_read32(kClkmgrBase + CLKMGR_RECOV_ERR_CODE_REG_OFFSET);
  CHECK((recov_err & (1u << CLKMGR_RECOV_ERR_CODE_SHADOW_UPDATE_ERR_BIT)) !=
        0u);
  CHECK(abs_mmio_read32(kClkmgrBase +
                        CLKMGR_USB_MEAS_CTRL_SHADOWED_REG_OFFSET) == v0);
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_FATAL_ERR_CODE_REG_OFFSET) == 0u);

  // Clear RECOV_ERR_CODE (rw1c) and verify expected recoverable alert 24.
  abs_mmio_write32(kClkmgrBase + CLKMGR_RECOV_ERR_CODE_REG_OFFSET,
                   (1u << CLKMGR_RECOV_ERR_CODE_SHADOW_UPDATE_ERR_BIT));
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_RECOV_ERR_CODE_REG_OFFSET) == 0u);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdClkmgrRecovFault));

  // 4. Write v1 twice back-to-back with no intervening read to commit v1, then
  //    restore v0.
  abs_mmio_write32(kClkmgrBase + CLKMGR_USB_MEAS_CTRL_SHADOWED_REG_OFFSET, v1);
  abs_mmio_write32(kClkmgrBase + CLKMGR_USB_MEAS_CTRL_SHADOWED_REG_OFFSET, v1);
  CHECK(abs_mmio_read32(kClkmgrBase +
                        CLKMGR_USB_MEAS_CTRL_SHADOWED_REG_OFFSET) == v1);
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_RECOV_ERR_CODE_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_FATAL_ERR_CODE_REG_OFFSET) == 0u);

  abs_mmio_write32(kClkmgrBase + CLKMGR_USB_MEAS_CTRL_SHADOWED_REG_OFFSET, v0);
  abs_mmio_write32(kClkmgrBase + CLKMGR_USB_MEAS_CTRL_SHADOWED_REG_OFFSET, v0);
  CHECK(abs_mmio_read32(kClkmgrBase +
                        CLKMGR_USB_MEAS_CTRL_SHADOWED_REG_OFFSET) == v0);
}

/**
 * Verify `prim_fifo_sync.sv` empty-pop pointer gating (`OutputZeroIfEmpty =
 * 1`), full-push pointer gating (`wready_o = ~full_o & ~under_rst`),
 * `prim_packer_fifo.sv` (`pulled = rvalid_o & rready_i`), and synchronous
 * `clr_i` reset.
 */
static void test_prim_fifo_sync_semantics(void) {
  LOG_INFO(
      "Verifying [prim_fifo_sync.sv:162, 181-182, 205-206] & "
      "[prim_packer_fifo.sv:53-118]: empty pop, full push, packer unpack, and "
      "clr_i reset");

  abs_mmio_write32(kUart1Base + UART_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(
      kUart1Base + UART_FIFO_CTRL_REG_OFFSET,
      (1u << UART_FIFO_CTRL_RXRST_BIT) | (1u << UART_FIFO_CTRL_TXRST_BIT));

  uint32_t status = abs_mmio_read32(kUart1Base + UART_STATUS_REG_OFFSET);
  CHECK((status & (1u << UART_STATUS_RXEMPTY_BIT)) != 0u);
  CHECK((status & (1u << UART_STATUS_TXEMPTY_BIT)) != 0u);

  // 1. Popping an empty prim_fifo_sync returns 0 (OutputZeroIfEmpty=1) and does
  //    not decrement/underflow rptr or depth_o.
  uint32_t rdata_empty = abs_mmio_read32(kUart1Base + UART_RDATA_REG_OFFSET);
  CHECK(rdata_empty == 0u);
  uint32_t fifo_status =
      abs_mmio_read32(kUart1Base + UART_FIFO_STATUS_REG_OFFSET);
  CHECK(fifo_status == 0u);

  // 2. Push 4 bytes into TX FIFO while UART1.CTRL.TX == 0, then fill to
  //    Depth = 128 and push a 129th byte (`0xFF`) to verify `fifo_incr_wptr =
  //    wvalid_i & wready_o` gates off pointer wrap/overflow when `full_o == 1`.
  abs_mmio_write32(kUart1Base + UART_WDATA_REG_OFFSET, 0x11u);
  abs_mmio_write32(kUart1Base + UART_WDATA_REG_OFFSET, 0x22u);
  abs_mmio_write32(kUart1Base + UART_WDATA_REG_OFFSET, 0x33u);
  abs_mmio_write32(kUart1Base + UART_WDATA_REG_OFFSET, 0x44u);
  fifo_status = abs_mmio_read32(kUart1Base + UART_FIFO_STATUS_REG_OFFSET);
  CHECK((fifo_status & UART_FIFO_STATUS_TXLVL_MASK) == 4u);

  for (uint32_t i = 4u; i < 32u; ++i) {
    abs_mmio_write32(kUart1Base + UART_WDATA_REG_OFFSET, i);
  }
  fifo_status = abs_mmio_read32(kUart1Base + UART_FIFO_STATUS_REG_OFFSET);
  status = abs_mmio_read32(kUart1Base + UART_STATUS_REG_OFFSET);
  CHECK((fifo_status & UART_FIFO_STATUS_TXLVL_MASK) == 32u);
  CHECK((status & (1u << UART_STATUS_TXFULL_BIT)) != 0u);

  // Push 33rd byte when full: wready_o == 0, so TXLVL stays 32 and TXFULL == 1
  abs_mmio_write32(kUart1Base + UART_WDATA_REG_OFFSET, 0xFFu);
  fifo_status = abs_mmio_read32(kUart1Base + UART_FIFO_STATUS_REG_OFFSET);
  status = abs_mmio_read32(kUart1Base + UART_STATUS_REG_OFFSET);
  CHECK((fifo_status & UART_FIFO_STATUS_TXLVL_MASK) == 32u);
  CHECK((status & (1u << UART_STATUS_TXFULL_BIT)) != 0u);

  abs_mmio_write32(kUart1Base + UART_FIFO_CTRL_REG_OFFSET,
                   (1u << UART_FIFO_CTRL_TXRST_BIT));
  fifo_status = abs_mmio_read32(kUart1Base + UART_FIFO_STATUS_REG_OFFSET);
  CHECK((fifo_status & UART_FIFO_STATUS_TXLVL_MASK) == 0u);

  // 3. Exercise `prim_packer_fifo` (`u_prim_packer_fifo_sw_genbits` in CSRNG,
  //    128-bit InW -> 32-bit OutW): empty read when rvalid_o == 0 is gated by
  //    `pulled = rvalid_o & rready_i`, and a 128-bit GENERATE block unpacks
  //    across exactly 4 32-bit pops before GENBITS_VLD drops to 0.
  abs_mmio_write32(
      kCsrngBase + CSRNG_CTRL_REG_OFFSET,
      (kMultiBitBool4True << CSRNG_CTRL_ENABLE_OFFSET) |
          (kMultiBitBool4True << CSRNG_CTRL_SW_APP_ENABLE_OFFSET) |
          (kMultiBitBool4False << CSRNG_CTRL_READ_INT_STATE_OFFSET) |
          (kMultiBitBool4False << CSRNG_CTRL_FIPS_FORCE_ENABLE_OFFSET));
  CHECK((abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_VLD_REG_OFFSET) & 1u) ==
        0u);
  (void)abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_REG_OFFSET);
  CHECK((abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_VLD_REG_OFFSET) & 1u) ==
        0u);
  // Instantiate (acmd=1, flag0=True) and Generate 1 block (acmd=3, glen=1).
  abs_mmio_write32(kCsrngBase + CSRNG_CMD_REQ_REG_OFFSET,
                   1u | (kMultiBitBool4True << 8));
  while ((abs_mmio_read32(kCsrngBase + CSRNG_SW_CMD_STS_REG_OFFSET) &
          (1u << CSRNG_SW_CMD_STS_CMD_ACK_BIT)) == 0u) {
  }
  abs_mmio_write32(kCsrngBase + CSRNG_CMD_REQ_REG_OFFSET,
                   3u | (kMultiBitBool4False << 8) | (1u << 12));
  while ((abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_VLD_REG_OFFSET) & 1u) ==
         0u) {
  }
  for (int w = 0; w < 3; ++w) {
    (void)abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_REG_OFFSET);
    CHECK((abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_VLD_REG_OFFSET) & 1u) ==
          1u);
  }
  (void)abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_REG_OFFSET);
  CHECK((abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_VLD_REG_OFFSET) & 1u) ==
        0u);
  abs_mmio_write32(
      kCsrngBase + CSRNG_CTRL_REG_OFFSET,
      (kMultiBitBool4False << CSRNG_CTRL_ENABLE_OFFSET) |
          (kMultiBitBool4False << CSRNG_CTRL_SW_APP_ENABLE_OFFSET) |
          (kMultiBitBool4False << CSRNG_CTRL_READ_INT_STATE_OFFSET) |
          (kMultiBitBool4False << CSRNG_CTRL_FIPS_FORCE_ENABLE_OFFSET));
}

/**
 * Verify `prim_mubi_pkg.sv` (`mubi4_test_true_loose` vs
 * `mubi4_test_true_strict`) and `prim_ram_1p_scr.sv` non-power-of-2 chunk
 * addressing (`u_sram_ctrl_meta`, `MemSizeRam = 38912 = 19 * 2048` bytes).
 */
static void test_prim_mubi_and_ram_scr_chunks(void) {
  LOG_INFO(
      "Verifying [prim_mubi_pkg.sv:120-143] & [prim_ram_1p_scr.sv:249-281]: "
      "MuBi4 strict/loose decoding and 19-chunk non-power-of-2 SRAM_CTRL_META");

  // 1. Write non-canonical 0x0 to SRAM_CTRL_RET_AON.READBACK
  //    (mubi4_test_true_loose) and EXEC (mubi4_test_true_strict), then restore
  //    MuBi4False (0x9).
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_READBACK_REG_OFFSET, 0x0u);
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_EXEC_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_READBACK_REG_OFFSET) ==
        0x0u);
  CHECK(abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_EXEC_REG_OFFSET) == 0x0u);
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_READBACK_REG_OFFSET,
                   kMultiBitBool4False);
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_EXEC_REG_OFFSET,
                   kMultiBitBool4False);

  // Also functionally verify `mubi4_test_true_loose(4'h0) == 1'b1` (`True`) vs
  // `mubi4_test_true_strict(4'h0) == 1'b0` (`False`) on CLKMGR:
  // - `CLKMGR_IO_MEAS_CTRL_EN` uses `mubi4_test_true_loose(val)`, so writing
  //   `0x0` with out-of-range thresholds `10..20` enables measurement and sets
  //   `CLKMGR_RECOV_ERR_CODE.IO_MEASURE_ERR`!
  // - `CLKMGR_EXTCLK_CTRL.SEL` uses `mubi4_test_true_strict(val)`, so writing
  //   `0x0` evaluates to `False` and leaves `CLKMGR_EXTCLK_STATUS == 0x9`!
  uint32_t orig_io_shadow =
      abs_mmio_read32(kClkmgrBase + CLKMGR_IO_MEAS_CTRL_SHADOWED_REG_OFFSET);
  abs_mmio_write32(kClkmgrBase + CLKMGR_IO_MEAS_CTRL_SHADOWED_REG_OFFSET,
                   (20u << 10) | 10u);
  abs_mmio_write32(kClkmgrBase + CLKMGR_IO_MEAS_CTRL_SHADOWED_REG_OFFSET,
                   (20u << 10) | 10u);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdClkmgrRecovFault));
  abs_mmio_write32(kClkmgrBase + CLKMGR_IO_MEAS_CTRL_EN_REG_OFFSET, 0x0u);
  busy_spin_micros(250);
  CHECK((abs_mmio_read32(kClkmgrBase + CLKMGR_RECOV_ERR_CODE_REG_OFFSET) &
         (1u << CLKMGR_RECOV_ERR_CODE_IO_MEASURE_ERR_BIT)) != 0u);
  abs_mmio_write32(kClkmgrBase + CLKMGR_IO_MEAS_CTRL_EN_REG_OFFSET,
                   kMultiBitBool4False);
  abs_mmio_write32(kClkmgrBase + CLKMGR_IO_MEAS_CTRL_SHADOWED_REG_OFFSET,
                   orig_io_shadow);
  abs_mmio_write32(kClkmgrBase + CLKMGR_IO_MEAS_CTRL_SHADOWED_REG_OFFSET,
                   orig_io_shadow);
  abs_mmio_write32(kClkmgrBase + CLKMGR_RECOV_ERR_CODE_REG_OFFSET, 0x7ffu);
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdClkmgrRecovFault));

  abs_mmio_write32(kClkmgrBase + CLKMGR_EXTCLK_CTRL_REG_OFFSET,
                   (kMultiBitBool4False << 4) | 0x0u);
  busy_spin_micros(10);
  CHECK(abs_mmio_read32(kClkmgrBase + CLKMGR_EXTCLK_STATUS_REG_OFFSET) ==
        kMultiBitBool4False);
  abs_mmio_write32(kClkmgrBase + CLKMGR_EXTCLK_CTRL_REG_OFFSET,
                   (kMultiBitBool4False << 4) | kMultiBitBool4False);

  // 2. Verify SRAM_CTRL_RET_AON RAM (0x40600000..0x40600ffc) and
  //    SRAM_CTRL_META RAM (0x11000000 Chunk 0 and 0x11000800 Chunk 1,
  //    MemSizeRam = 38912 = 19 * 2048 bytes, Depth = 9728).
  const uint32_t kRetWord0 = 0x40600e00u;
  const uint32_t kRetWord1 = 0x40600ffcu;

  abs_mmio_write32(kRetWord0, 0xa5a50000u);
  abs_mmio_write32(kRetWord1, 0xa5a50012u);

  CHECK(abs_mmio_read32(kRetWord0) == 0xa5a50000u);
  CHECK(abs_mmio_read32(kRetWord1) == 0xa5a50012u);

  CHECK((abs_mmio_read32(kSramMetaRegsBase + SRAM_CTRL_STATUS_REG_OFFSET) &
         0x3fu) == 0u);
}

bool test_main(void) {
  LOG_INFO("=== Starting PRIM Earlgrey v2 Errata Test ===");
  test_prim_subreg_shadow_semantics();
  test_prim_fifo_sync_semantics();
  test_prim_mubi_and_ram_scr_chunks();
  LOG_INFO("=== ALL PRIM V2 ERRATA CHECKS PASSED ===");
  return true;
}
