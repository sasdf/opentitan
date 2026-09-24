// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file pattgen_errata_v2_test.c
 * @brief Earlgrey v2 (`trunk-v2`) CW340 FPGA Hardware, Spec & DIF Errata
 * Verification Suite for P32 `pattgen` (+ `hmac`).
 *
 * Verifies:
 *   1. `hw/ip/pattgen/rtl/pattgen_chan.sv:60-62, 172-181` &
 * `pattgen_reg_pkg.sv:188` (removed from `top_earlgrey` in `trunk-v2`, while
 * `hw/ip/pattgen` and `dif_pattgen` remain in the tree):
 *      - Unmapped v1 `pattgen` base address (`0x400e0000u`) on `trunk-v2`
 *        `xbar_peri` raises synchronous TL-UL Load/Store Access Faults
 *        (`mcause = 5 / 7`).
 *      - `dif_pattgen_configure_channel()` and
 * `dif_pattgen_channel_set_enabled()` in `sw/device/lib/dif/dif_pattgen.c`
 * retain the exact 32-bit `SIZE` register packing and `ENABLE_CHx` lock check
 * (`kDifLocked`).
 *   2. `hw/ip/hmac/rtl/hmac.sv:26-44` &
 * `hw/ip/keymgr_dpe/rtl/keymgr_dpe.sv:112-119` (`TODO(#31026)`): `hmac`
 * (`hw/ip/hmac/rtl/hmac.sv:26-44`) and `keymgr_dpe`
 *      (`hw/ip/keymgr_dpe/rtl/keymgr_dpe.sv:112-119`) declare the `keymgr_key`
 *      sideload interface in `hmac.hjson:95-100` and `interfaces.md:16`, but
 *      permanently tie `hmac_key_o = '0` and wire `keymgr_key_i` to
 *      `unused_key` with no `CFG.SIDELOAD` bit (`CFG[31:15]` read as `0`).
 *   3. `hw/ip/hmac/rtl/hmac.sv:215-228, 339` &
 * `sw/device/lib/dif/dif_hmac.c:103-132`: `hmac.sv:221-225` applies
 * `conv_endian32(reg2hw.key[31-i].q, key_swap)` ONLY during the single clock
 * cycle when `KEY_i` is written (`regext` `qe == 1`). Toggling `CFG.KEY_SWAP`
 * AFTER writing `KEY_0..KEY_7` (or calling `dif_hmac_mode_hmac_start()`, which
 * writes `KEY_7..0` before `CFG` without updating `KEY_SWAP`) fails to re-swap
 * the latched `secret_key`.
 *   4. `hw/ip/prim/rtl/prim_sha2.sv:161-166` & `hw/ip/hmac/rtl/hmac.sv:252-284,
 * 648-655`:
 *      - `hmac.hjson:488` claims `DIGEST_0..15` are writable whenever
 *        `STATUS.hmac_idle == 1`, but `prim_sha2.sv:163` gates `digest_we_i` by
 *        `!sha_en_i` (`CFG.SHA_EN == 0`) and `hmac.sv:252-265` gates
 *        `digest_sw_we` by `digest_size != SHA2_None`, silently dropping all
 *        `DIGEST_0..15` writes when `STATUS.hmac_idle == 1` and `CFG.SHA_EN ==
 * 1`.
 *      - Meanwhile, `MSG_LENGTH_LOWER` (`hmac.sv:648-655`) is gated only by
 *        `!cfg_block` (accepting writes even when `CFG.SHA_EN == 1`) and stores
 *        bits `[2:0]` verbatim (`0x205` reads back `0x205`), contradicting
 *        `hmac.hjson:510` ("Lower 3 bits [2:0] are ignored").
 *      - When `CFG.SHA_EN == 0` and `CFG.DIGEST_SIZE == SHA2_512`, writing
 *        `DIGEST_0..15` updates 64-bit `digest_q[0..7]`, but reading
 *        `DIGEST_0..15` before `CMD.HASH_CONTINUE` multiplexes on
 *        `digest_size_started_q` (`hmac.sv:268-284`), returning the odd words
 *        (`DIGEST_1,3..15`) duplicated in both `DIGEST_0..7` and `DIGEST_8..15`
 *        until `CMD.HASH_CONTINUE` latches `digest_size_started_q = SHA2_512`.
 *   5. `hw/ip/hmac/rtl/hmac.sv:530-532, 598-618` &
 * `hw/ip/hmac/rtl/hmac_reg_pkg.sv:522-580`: Reading `HMAC_MSG_FIFO`
 * (`0x41111000`) triggers `tlul_adapter_sram`
 *      (`ErrOnRead = 1`, `rvalid_i = 1'b0` in `hmac.sv:611-618`) and raises a
 *      synchronous Load Access Fault (`mcause = 5`), and narrow sub-word writes
 *      violating `HMAC_PERMIT` (`1-byte` write to `HMAC_KEY_0` or byte `2` of
 *      `HMAC_CFG`) raise a synchronous Store Access Fault (`mcause = 7`).
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_hmac.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/irq.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top/hmac_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kHmacBase = TOP_EARLGREY_HMAC_BASE_ADDR,
  // v1 Earlgrey pattgen base address & CSR offsets (removed from top_earlgrey
  // in trunk-v2, and hw/ip/pattgen/data/BUILD deleted pattgen_c_regs while
  // dif_pattgen.c/h include non-existent hw/top/pattgen_regs.h and deleted
  // sw/device/lib/dif/autogen/dif_pattgen_autogen.h):
  kV1PattgenBase = 0x400e0000u,
  kV1PattgenCtrlOffset = 0x10u,
  kV1PattgenSizeOffset = 0x2cu,
};

static volatile bool g_saw_bus_fault = false;
static volatile uint32_t g_last_mcause = 0;

void ottf_external_isr(uint32_t *exc_info) {
  (void)exc_info;
  irq_external_ctrl(false);
}

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  g_last_mcause = ibex_mcause_read();
  g_saw_bus_fault = true;
}

/**
 * Helper: wait for `INTR_STATE.HMAC_DONE` and clear it.
 */
static void hmac_wait_done(void) {
  while ((abs_mmio_read32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET) &
          (1u << HMAC_INTR_STATE_HMAC_DONE_BIT)) == 0u) {
  }
  abs_mmio_write32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET,
                   (1u << HMAC_INTR_STATE_HMAC_DONE_BIT));
}

/**
 * Test 1: `top_earlgrey` removal of `pattgen` (`0x400e0000`) & orphaned
 * `dif_pattgen.{c,h}`
 * (`MODULE_REPLACED_IN_V2` on `top_earlgrey`; `dif_pattgen.{c,h}` orphaned by
 * deletion of `//hw/ip/pattgen/data:pattgen_c_regs` and
 * `sw/device/lib/dif/autogen/dif_pattgen_autogen.{c,h}` while referencing
 * non-existent `hw/top/pattgen_regs.h` & `dif_pattgen_autogen.h`).
 */
static void test_pattgen_v2_top_removal_and_unmapped_fault(void) {
  LOG_INFO(
      "Testing top_earlgrey pattgen removal & orphaned dif_pattgen: v1 pattgen "
      "MMIO unmapped fault on trunk-v2 xbar_peri");

  // Verify v1 pattgen base address (0x400e0000) is unmapped on trunk-v2
  // xbar_peri and raises a synchronous Load Access Fault (mcause = 5) and
  // Store Access Fault (mcause = 7).
  g_saw_bus_fault = false;
  g_last_mcause = 0;
  (void)abs_mmio_read32(kV1PattgenBase + kV1PattgenCtrlOffset);
  CHECK(g_saw_bus_fault && g_last_mcause == 5u,
        "Expected Load Access Fault (mcause=5) at unmapped v1 pattgen base");

  g_saw_bus_fault = false;
  g_last_mcause = 0;
  abs_mmio_write32(kV1PattgenBase + kV1PattgenSizeOffset, 0x12345678u);
  CHECK(g_saw_bus_fault && g_last_mcause == 7u,
        "Expected Store Access Fault (mcause=7) at unmapped v1 pattgen base");
}

/**
 * Test 2: `hmac.sv:36-44` & `keymgr_dpe.sv:112-119` (`TODO(#31026)`)
 * `hmac.hjson:95-100` and `interfaces.md:16` declare the `keymgr_key` sideload
 * interface (`keymgr_dpe_pkg::hw_key_req_t`), but `keymgr_dpe.sv:112-119` ties
 * `hmac_key_o = '0` and `hmac.sv:36-44` buffers `keymgr_key_i` into
 * `unused_key` with no `CFG.SIDELOAD` bit (`CFG[31:15]` read back `0`).
 */
static void test_hmac_unwired_keymgr_sideload(void) {
  LOG_INFO(
      "Testing hmac.sv:36-44: Unwired keymgr_key_i sideload (TODO "
      "#31026) "
      "and reserved CFG[31:15] bits");

  // Ensure HMAC is idle and sha_en = 0.
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, 0u);

  // Attempt to set upper bits [31:15] of HMAC_CFG (where a SIDELOAD bit would
  // reside if implemented); verify hardware masks them to 0 and only bits
  // [14:0] exist in HMAC_CFG.
  uint32_t cfg_val =
      (1u << HMAC_CFG_SHA_EN_BIT) | (1u << HMAC_CFG_HMAC_EN_BIT) |
      (HMAC_CFG_DIGEST_SIZE_VALUE_SHA2_256 << HMAC_CFG_DIGEST_SIZE_OFFSET) |
      (HMAC_CFG_KEY_LENGTH_VALUE_KEY_256 << HMAC_CFG_KEY_LENGTH_OFFSET);
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg_val | 0xFFFF8000u);
  uint32_t cfg_rb = abs_mmio_read32(kHmacBase + HMAC_CFG_REG_OFFSET);
  CHECK((cfg_rb & 0xFFFF8000u) == 0u,
        "Expected HMAC_CFG[31:15] to read back 0 (no SIDELOAD CSR bit)");
  CHECK(cfg_rb == cfg_val);
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, 0u);
}

/**
 * Test 3: `hmac.sv:215-228` `CFG.KEY_SWAP` write-time evaluation &
 * `dif_hmac.c:103-132` `hmac.sv:221-225` evaluates
 * `conv_endian32(reg2hw.key[31-i].q, key_swap)` ONLY during the 1-cycle
 * `reg2hw.key[i].qe` pulse when `KEY_i` is written! Toggling `CFG.KEY_SWAP`
 * AFTER writing `KEY_0..KEY_7` (or calling `dif_hmac_mode_hmac_start()` when
 * `CFG.KEY_SWAP` was previously `1`) fails to update the endianness of
 * `secret_key`!
 */
static void test_hmac_key_swap_write_time_latch_hazard(dif_hmac_t *hmac) {
  LOG_INFO(
      "Testing hmac.sv:215-228: CFG.KEY_SWAP write-time evaluation hazard "
      "& dif_hmac_mode_hmac_start ordering bug");

  const uint32_t kTestKey[8] = {
      0x01020304u, 0x05060708u, 0x090A0B0Cu, 0x0D0E0F10u,
      0x11121314u, 0x15161718u, 0x191A1B1Cu, 0x1D1E1F20u,
  };
  const uint32_t kBaseCfg =
      (1u << HMAC_CFG_SHA_EN_BIT) | (1u << HMAC_CFG_HMAC_EN_BIT) |
      (HMAC_CFG_DIGEST_SIZE_VALUE_SHA2_256 << HMAC_CFG_DIGEST_SIZE_OFFSET) |
      (HMAC_CFG_KEY_LENGTH_VALUE_KEY_256 << HMAC_CFG_KEY_LENGTH_OFFSET);

  // Run A: Set CFG.KEY_SWAP = 0 FIRST, then write KEY_0..7, then hash "abc".
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, kBaseCfg);
  for (uint32_t i = 0; i < 8; ++i) {
    abs_mmio_write32(kHmacBase + HMAC_KEY_0_REG_OFFSET + i * 4u, kTestKey[i]);
  }
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   (1u << HMAC_CMD_HASH_START_BIT));
  abs_mmio_write32(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET, 0x00636261u);
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   (1u << HMAC_CMD_HASH_PROCESS_BIT));
  hmac_wait_done();
  uint32_t digest_swap0_first =
      abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET);

  // Run B: Now toggle CFG.KEY_SWAP = 1 WITHOUT re-writing KEY_0..7, and run
  // HMAC on "abc" again!
  // Because `hmac.sv:221-225` only applies `key_swap` on `reg2hw.key[i].qe`,
  // toggling `CFG.KEY_SWAP = 1` AFTER `KEY_0..7` were written has ZERO effect
  // on `secret_key`!
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET,
                   kBaseCfg | (1u << HMAC_CFG_KEY_SWAP_BIT));
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   (1u << HMAC_CMD_HASH_START_BIT));
  abs_mmio_write32(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET, 0x00636261u);
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   (1u << HMAC_CMD_HASH_PROCESS_BIT));
  hmac_wait_done();
  uint32_t digest_swap1_after_key =
      abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET);
  CHECK(digest_swap1_after_key == digest_swap0_first,
        "Expected toggling CFG.KEY_SWAP=1 after writing KEY_0..7 to have zero "
        "effect on secret_key (0x%x vs 0x%x)",
        digest_swap1_after_key, digest_swap0_first);

  // Run C: Now re-write KEY_0..7 WHILE `CFG.KEY_SWAP == 1` is already set!
  // Now `conv_endian32(reg2hw.key[i].q, 1)` swaps each key word and produces a
  // completely different HMAC digest!
  for (uint32_t i = 0; i < 8; ++i) {
    abs_mmio_write32(kHmacBase + HMAC_KEY_0_REG_OFFSET + i * 4u, kTestKey[i]);
  }
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   (1u << HMAC_CMD_HASH_START_BIT));
  abs_mmio_write32(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET, 0x00636261u);
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   (1u << HMAC_CMD_HASH_PROCESS_BIT));
  hmac_wait_done();
  uint32_t digest_swap1_before_key =
      abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET);
  CHECK(
      digest_swap1_before_key != digest_swap0_first,
      "Expected writing KEY_0..7 while CFG.KEY_SWAP==1 to produce swapped-key "
      "HMAC digest");

  // Also verify dif_hmac_mode_hmac_start() bug: because
  // dif_hmac_mode_hmac_start reads CFG (keeping KEY_SWAP=1!) and writes
  // KEY_7..0 BEFORE writing CFG, calling dif_hmac_mode_hmac_start() while
  // KEY_SWAP==1 vs KEY_SWAP==0 produces different digests for the exact same
  // `dif_hmac_transaction_t` and key!
  dif_hmac_transaction_t txn = {
      .digest_endianness = kDifHmacEndiannessBig,
      .message_endianness = kDifHmacEndiannessLittle,
  };
  CHECK_DIF_OK(dif_hmac_mode_hmac_start(hmac, (const uint8_t *)kTestKey, txn));
  abs_mmio_write32(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET, 0x00636261u);
  CHECK_DIF_OK(dif_hmac_process(hmac));
  hmac_wait_done();
  uint32_t dif_digest_with_stale_key_swap =
      abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET);

  // Now clear KEY_SWAP=0 in CFG and call dif_hmac_mode_hmac_start() with the
  // exact same arguments!
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, kBaseCfg);
  CHECK_DIF_OK(dif_hmac_mode_hmac_start(hmac, (const uint8_t *)kTestKey, txn));
  abs_mmio_write32(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET, 0x00636261u);
  CHECK_DIF_OK(dif_hmac_process(hmac));
  hmac_wait_done();
  uint32_t dif_digest_with_clean_key_swap =
      abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET);
  CHECK(dif_digest_with_stale_key_swap != dif_digest_with_clean_key_swap,
        "Expected dif_hmac_mode_hmac_start() to be corrupted by sticky "
        "CFG.KEY_SWAP=1");
}

/**
 * Test 4: `prim_sha2.sv:163` `!sha_en_i` digest write gate & `hmac.sv:252-284`
 * SHA-512 readback 1) `hmac.hjson:488` claims `DIGEST_0..15` are writable
 * whenever `STATUS.hmac_idle == 1`, but `prim_sha2.sv:163` gates `digest_we_i`
 * by
 *    `!sha_en_i` (`CFG.SHA_EN == 0`), silently dropping `DIGEST_0..15` writes
 *    when `STATUS.hmac_idle == 1` and `CFG.SHA_EN == 1`.
 * 2) `hmac.hjson:510` claims `MSG_LENGTH_LOWER` lower 3 bits `[2:0]` are
 *    ignored, but `hmac.sv:650` latches all 32 bits verbatim (`0x205` reads
 *    back `0x205`) even while `CFG.SHA_EN == 1`.
 * 3) When `CFG.SHA_EN == 0` and `CFG.DIGEST_SIZE == SHA2_512`, writing
 *    `DIGEST_0..15` updates 64-bit `digest_q[0..7]`, but reading `DIGEST_0..15`
 *    before `CMD.HASH_CONTINUE` multiplexes on `digest_size_started_q`
 *    (`SHA2_256`), duplicating the odd words (`DIGEST_1,3..15`) across
 *    `DIGEST_0..7` and `DIGEST_8..15` until `CMD.HASH_CONTINUE` is pulsed!
 */
static void test_hmac_digest_write_gate_and_sha512_readback(void) {
  LOG_INFO(
      "Testing prim_sha2.sv:163 & hmac.sv:252-284: DIGEST write gating "
      "(!sha_en), "
      "MSG_LENGTH_LOWER[2:0] retention, and SHA-512 readback duplication");

  // 1. Verify STATUS.HMAC_IDLE == 1 while CFG.SHA_EN == 1 (from end of Test 3).
  uint32_t status = abs_mmio_read32(kHmacBase + HMAC_STATUS_REG_OFFSET);
  CHECK((status & (1u << HMAC_STATUS_HMAC_IDLE_BIT)) != 0u);
  uint32_t prev_d0 = abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET);
  CHECK(prev_d0 != 0x11223344u);

  // Attempt to write DIGEST_0 while STATUS.HMAC_IDLE == 1 and CFG.SHA_EN == 1:
  // Silently dropped by `prim_sha2.sv:163` (`else if (!sha_en_i)`)!
  abs_mmio_write32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET, 0x11223344u);
  CHECK(abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET) == prev_d0,
        "Expected DIGEST_0 write while CFG.SHA_EN==1 & HMAC_IDLE==1 to be "
        "silently dropped");

  // Meanwhile, write MSG_LENGTH_LOWER = 0x205 while CFG.SHA_EN == 1 &
  // HMAC_IDLE == 1: succeeds AND retains lower 3 bits [2:0] == 5!
  abs_mmio_write32(kHmacBase + HMAC_MSG_LENGTH_LOWER_REG_OFFSET, 0x205u);
  CHECK(abs_mmio_read32(kHmacBase + HMAC_MSG_LENGTH_LOWER_REG_OFFSET) == 0x205u,
        "Expected MSG_LENGTH_LOWER to retain lower 3 bits [2:0] == 5");

  // 2. Also verify `hmac.sv:252-265`: when `CFG == 0` (`CFG.SHA_EN == 0` AND
  // `CFG.DIGEST_SIZE == SHA2_None`), writing `DIGEST_0` is ALSO silently
  // dropped because `digest_sw_we` is gated by `digest_size != SHA2_None`!
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, 0u);
  abs_mmio_write32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET, 0x55667788u);
  CHECK(abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET) == prev_d0,
        "Expected DIGEST_0 write while CFG.SHA_EN==0 & DIGEST_SIZE==SHA2_None "
        "to be silently dropped");

  // 3. Now keep CFG.SHA_EN = 0 and set CFG.DIGEST_SIZE = SHA2_512 (0x4).
  // Note that `digest_size_started_q` is still `SHA2_256` from Test 3!
  abs_mmio_write32(
      kHmacBase + HMAC_CFG_REG_OFFSET,
      (HMAC_CFG_DIGEST_SIZE_VALUE_SHA2_512 << HMAC_CFG_DIGEST_SIZE_OFFSET) |
          (HMAC_CFG_KEY_LENGTH_VALUE_KEY_512 << HMAC_CFG_KEY_LENGTH_OFFSET));

  // Write 16 distinct words `0xA0000000 + i` into `DIGEST_0..15` while
  // `CFG.SHA_EN == 0` and `CFG.DIGEST_SIZE == SHA2_512`.
  for (uint32_t i = 0; i < 16; ++i) {
    abs_mmio_write32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET + i * 4u,
                     0xA0000000u + i);
  }

  // Read back `DIGEST_0..15` BEFORE pulsing `CMD.HASH_CONTINUE`!
  // Because `hw2reg.digest` (`hmac.sv:268-284`) uses `digest_size_started_q`
  // (`SHA2_256`) instead of `digest_size` (`SHA2_512`), `DIGEST_i` and
  // `DIGEST_{i+8}` BOTH return `digest[i][31:0]` (`0xA0000000 + 2*i + 1`, the
  // odd words!), hiding `digest[i][63:32]` (`0xA0000000 + 2*i`)!
  for (uint32_t i = 0; i < 8; ++i) {
    uint32_t d_lo =
        abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET + i * 4u);
    uint32_t d_hi =
        abs_mmio_read32(kHmacBase + HMAC_DIGEST_8_REG_OFFSET + i * 4u);
    uint32_t expected_odd_word = 0xA0000000u + (2u * i + 1u);
    CHECK(d_lo == expected_odd_word && d_hi == expected_odd_word,
          "Expected pre-continue readback of DIGEST_%u/DIGEST_%u to duplicate "
          "odd word 0x%x, got 0x%x / 0x%x",
          i, i + 8, expected_odd_word, d_lo, d_hi);
  }

  // Now enable CFG.SHA_EN = 1 (with SHA2_512) and pulse CMD.HASH_CONTINUE = 1
  // followed by CMD.HASH_STOP = 1: `hash_continue` latches
  // `digest_size_started_q = SHA2_512`, revealing all 16 distinct words
  // `0xA0000000 + i` that were stored in `digest_q[0..7][63:0]`!
  abs_mmio_write32(
      kHmacBase + HMAC_CFG_REG_OFFSET,
      (1u << HMAC_CFG_SHA_EN_BIT) |
          (HMAC_CFG_DIGEST_SIZE_VALUE_SHA2_512 << HMAC_CFG_DIGEST_SIZE_OFFSET) |
          (HMAC_CFG_KEY_LENGTH_VALUE_KEY_512 << HMAC_CFG_KEY_LENGTH_OFFSET));
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   (1u << HMAC_CMD_HASH_CONTINUE_BIT));
  for (uint32_t i = 0; i < 16; ++i) {
    uint32_t d_word =
        abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET + i * 4u);
    CHECK(d_word == 0xA0000000u + i,
          "Expected DIGEST_%u == 0x%x after HASH_CONTINUE latched "
          "digest_size_started_q=SHA2_512, got 0x%x",
          i, 0xA0000000u + i, d_word);
  }
  // Reset HMAC engine state cleanly by disabling SHA_EN = 0.
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, 0u);
}

/**
 * Test 5: `hmac.sv:598-618` `MSG_FIFO` `ErrOnRead=1` &
 * `hmac_reg_pkg.sv:449-509` `HMAC_PERMIT` Reading `HMAC_MSG_FIFO`
 * (`0x41111000`) hits `tlul_adapter_sram`
 * (`ErrOnRead = 1`, `rvalid_i = 1'b0` in `hmac.sv:611-618`) and raises a
 * synchronous Load Access Fault (`mcause = 5`) without setting `ERR_CODE`, and
 * narrow sub-word writes to `HMAC_KEY_0` (`HMAC_PERMIT[9] = 4'b1111`) or
 * `HMAC_CFG` (`HMAC_PERMIT[4] = 4'b0011`) raise a synchronous Store Access
 * Fault (`mcause = 7`).
 */
static void test_hmac_msg_fifo_read_and_permit_faults(void) {
  LOG_INFO(
      "Testing hmac.sv:598-618 & hmac_reg_pkg.sv:449-509: MSG_FIFO ErrOnRead "
      "(mcause=5, ERR_CODE=0) & HMAC_PERMIT sub-word write faults (mcause=7)");

  // 1. 32-bit load from HMAC_MSG_FIFO (0x41111000) -> Load Access Fault (mcause
  // = 5) without setting HMAC_ERR_CODE.
  g_saw_bus_fault = false;
  g_last_mcause = 0;
  (void)abs_mmio_read32(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET);
  CHECK(g_saw_bus_fault && g_last_mcause == 5u,
        "Expected Load Access Fault (mcause=5) on reading HMAC_MSG_FIFO");
  CHECK(abs_mmio_read32(kHmacBase + HMAC_ERR_CODE_REG_OFFSET) == 0u,
        "Expected HMAC_ERR_CODE == 0 after MSG_FIFO read fault");

  // 2. 1-byte store to HMAC_KEY_0 (0x28, HMAC_PERMIT[9] = 4'b1111) -> Store
  // Access Fault (mcause = 7)
  g_saw_bus_fault = false;
  g_last_mcause = 0;
  abs_mmio_write8(kHmacBase + HMAC_KEY_0_REG_OFFSET, 0xAAu);
  CHECK(g_saw_bus_fault && g_last_mcause == 7u,
        "Expected Store Access Fault (mcause=7) on 1-byte write to HMAC_KEY_0");

  // 3. 1-byte store to byte 0 and byte 2 of HMAC_CFG (0x10 / 0x12,
  // HMAC_PERMIT[4] = 4'b0011) -> Store Access Fault (mcause = 7), whereas
  // 2-byte halfword store (`sh`, reg_be = 4'b0011) to HMAC_CFG+0 succeeds!
  g_saw_bus_fault = false;
  g_last_mcause = 0;
  abs_mmio_write8(kHmacBase + HMAC_CFG_REG_OFFSET, 0x01u);
  CHECK(g_saw_bus_fault && g_last_mcause == 7u,
        "Expected Store Access Fault (mcause=7) on 1-byte write to HMAC_CFG+0");

  g_saw_bus_fault = false;
  g_last_mcause = 0;
  abs_mmio_write8(kHmacBase + HMAC_CFG_REG_OFFSET + 2u, 0x01u);
  CHECK(g_saw_bus_fault && g_last_mcause == 7u,
        "Expected Store Access Fault (mcause=7) on 1-byte write to HMAC_CFG+2");

  g_saw_bus_fault = false;
  *(volatile uint16_t *)(uintptr_t)(kHmacBase + HMAC_CFG_REG_OFFSET) = 0x0000u;
  CHECK(!g_saw_bus_fault,
        "Expected 2-byte sh to HMAC_CFG+0 (HMAC_PERMIT[4]=4'b0011) to succeed");
}

bool test_main(void) {
  irq_external_ctrl(false);

  dif_hmac_t hmac;
  CHECK_DIF_OK(dif_hmac_init(mmio_region_from_addr(kHmacBase), &hmac));

  LOG_INFO(
      "=== Running P32 pattgen (+ hmac) v2 errata suite on CW340 FPGA ===");
  test_pattgen_v2_top_removal_and_unmapped_fault();
  test_hmac_unwired_keymgr_sideload();
  test_hmac_key_swap_write_time_latch_hazard(&hmac);
  test_hmac_digest_write_gate_and_sha512_readback();
  test_hmac_msg_fifo_read_and_permit_faults();
  LOG_INFO("=== All P32 pattgen (+ hmac) v2 errata tests PASSED! ===");
  return true;
}
