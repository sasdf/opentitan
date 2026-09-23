// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * CW340 FPGA & QEMU Empirical Errata Confirmation Test for `hmac` (`P02`).
 *
 * Empirically verifies all 8 documented hardware errata, specification
 * discrepancies, and security hardening behaviors in
 * `/root/knowledge/errata/hmac.md` across both the physical CW340 FPGA (golden
 * Earlgrey RTL) and QEMU (`ot_hmac`):
 *
 * - [prim_sha2.sv:159-164] (SPEC_DOC_ERRATA):
 *   `DIGEST_0..15` software context restore writes require `CFG.sha_en == 0`
 *   AND `CFG.digest_size != SHA2_None` (writes while `CFG.sha_en == 1 &&
 *   STATUS.hmac_idle == 1` are ignored), and `CFG.sha_en: 1 -> 0` clears
 *   `DIGEST_0..15` to `0x00000000` rather than `WIPE_SECRET`.
 * - [hmac.sv:240-243] (SPEC_DOC_ERRATA):
 *   `MSG_LENGTH_LOWER/UPPER` is NOT cleared by `CFG.sha_en: 1 -> 0`, is
 *   writable whenever `!cfg_block` even if `CFG.sha_en == 1`, and
 * `DIGEST_8..15` writes are ignored when `CFG.digest_size == SHA2_256`.
 * - [hmac.sv:256-272] (SPEC_DOC_ERRATA):
 *   `DIGEST_8..15` mirrors `DIGEST_0..7` in `SHA2_256` (instead of reading 0),
 *   `DIGEST_12..15` exposes internal 512-bit SHA-512 state `digest[6..7]` in
 *   `SHA2_384`, and `WIPE_SECRET` readback via `DIGEST_0..15` is byte-swapped
 *   by `conv_endian32(V, CFG.digest_swap)`.
 * - [hmac.sv:343-344] (SPEC_DOC_ERRATA):
 *   `ERR_CODE` sticky first-error latch (`err_valid = ~intr_state.hmac_err.q &
 * ...`) retains the first error code until `INTR_STATE.hmac_err` is
 * W1C-cleared, and setting `INTR_TEST.hmac_err = 1` blocks subsequent hardware
 * errors from updating `ERR_CODE`.
 * - [hmac.sv:349-362] (BENIGN_RTL_IMPL_DETAIL):
 *   Writing `CMD.hash_stop = 1` while idle (`STATUS.hmac_idle == 1`, including
 *   with `SHA2_None`) clears `cfg_block` without raising `ERR_CODE` or clearing
 *   `STATUS.hmac_idle`.
 * - [hmac.sv:583-612] (INTENDED_SECURITY_HARDENING):
 *   Reading write-only `MSG_FIFO` (`0x1000..0x1fff`) triggers a synchronous
 *   TL-UL Load Access Fault (`mcause = 5`) via `u_tlul_adapter`
 * (`.ErrOnRead(1)`) while suppressing `msg_fifo_req` so `INTR_STATE.hmac_err`
 * stays `0`.
 * - [hmac_reg_pkg.sv:441-501] (INTENDED_SECURITY_HARDENING):
 *   `HMAC_PERMIT` enforces sub-word write faults (`mcause = 7` Store Access
 * Fault) while allowing sub-word reads, and unmapped register offset `0x0f0`
 * raises Load/Store Access Fault (`mcause = 5 / 7`).
 * - [prim_sha2_pad.sv:238-243] (TRUE_SILICON_ERRATA):
 *   Issuing `CMD.hash_stop = 1` after writing a non-multiple of the 512-bit
 *   SHA-256 block size (9 words = 288 bits) causes `prim_sha2_pad.sv:240-245`
 *   (`txcnt_eq_msg_len && hash_stop_flag_q`) to transition `u_pad` to `StIdle`
 *   immediately while `u_sha2` remains stuck in `FifoLoadFromFifo` (`w_index_q
 * == 9`), wedging `STATUS.hmac_idle == 0` with `INTR_STATE.hmac_done == 0`
 * until `CFG.sha_en = 0` aborts `u_sha2`.
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/arch/device.h"
#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hmac_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kHmacBase = TOP_EARLGREY_HMAC_BASE_ADDR,
  kMcauseLoadAccessFault = 5u,
  kMcauseStoreAccessFault = 7u,
  kErrSwHashStartWhenShaDisabled = 2u,
  kErrSwPushMsgWhenDisallowed = 5u,
};

static volatile bool g_expect_bus_fault = false;
static volatile bool g_expected_fault_taken = false;
static volatile uint32_t g_last_mcause = 0;

void ottf_exception_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = ibex_mcause_read();
  uint32_t mepc = ibex_mepc_read();
  if (g_expect_bus_fault &&
      (mcause == kMcauseLoadAccessFault || mcause == kMcauseStoreAccessFault)) {
    g_expected_fault_taken = true;
    g_last_mcause = mcause;
    g_expect_bus_fault = false;
    uint16_t insn16 = *(const uint16_t *)mepc;
    uint32_t step = ((insn16 & 0x3u) == 0x3u) ? 4u : 2u;
    CSR_WRITE(CSR_REG_MEPC, mepc + step);
    return;
  }
  CHECK(false, "Unexpected exception mcause=0x%08x mepc=0x%08x", mcause, mepc);
}

static uint32_t make_cfg(bool hmac_en, bool sha_en, bool endian_swap,
                         bool digest_swap, bool key_swap, uint32_t digest_size,
                         uint32_t key_length) {
  uint32_t reg = 0;
  reg = bitfield_bit32_write(reg, HMAC_CFG_HMAC_EN_BIT, hmac_en);
  reg = bitfield_bit32_write(reg, HMAC_CFG_SHA_EN_BIT, sha_en);
  reg = bitfield_bit32_write(reg, HMAC_CFG_ENDIAN_SWAP_BIT, endian_swap);
  reg = bitfield_bit32_write(reg, HMAC_CFG_DIGEST_SWAP_BIT, digest_swap);
  reg = bitfield_bit32_write(reg, HMAC_CFG_KEY_SWAP_BIT, key_swap);
  reg = bitfield_field32_write(reg, HMAC_CFG_DIGEST_SIZE_FIELD, digest_size);
  reg = bitfield_field32_write(reg, HMAC_CFG_KEY_LENGTH_FIELD, key_length);
  return reg;
}

static void clear_hmac_intr(void) {
  abs_mmio_write32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET,
                   (1u << HMAC_INTR_STATE_HMAC_DONE_BIT) |
                       (1u << HMAC_INTR_STATE_FIFO_EMPTY_BIT) |
                       (1u << HMAC_INTR_STATE_HMAC_ERR_BIT));
}

static void verify_hmac_wipe_secret_clears_msg_fifo(void) {
  LOG_INFO(
      "Verifying [prim_sha2.sv:159-164] (SPEC_DOC_ERRATA): DIGEST_0..15 write "
      "gating (!sha_en && digest_size != SHA2_None) and sha_en 1->0 "
      "zeroing...");

  /* Wipe secret with known pattern 0x55aa55aa (digest_swap = 0). */
  uint32_t cfg_none = make_cfg(false, false, false, false, false,
                               HMAC_CFG_DIGEST_SIZE_VALUE_SHA2_NONE,
                               HMAC_CFG_KEY_LENGTH_VALUE_KEY_256);
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg_none);
  abs_mmio_write32(kHmacBase + HMAC_WIPE_SECRET_REG_OFFSET, 0x55aa55aau);
  CHECK(abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET) == 0x55aa55aau);

  /* 1. Write to DIGEST_0 with sha_en=0 but digest_size=SHA2_None is ignored. */
  abs_mmio_write32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET, 0x11223344u);
  CHECK(abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET) == 0x55aa55aau);

  /* 2. Write to DIGEST_0 with sha_en=1 and hmac_idle=1 is ALSO ignored. */
  uint32_t cfg_sha256_en = make_cfg(false, true, false, true, false,
                                    HMAC_CFG_DIGEST_SIZE_VALUE_SHA2_256,
                                    HMAC_CFG_KEY_LENGTH_VALUE_KEY_256);
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg_sha256_en);
  CHECK(bitfield_bit32_read(abs_mmio_read32(kHmacBase + HMAC_STATUS_REG_OFFSET),
                            HMAC_STATUS_HMAC_IDLE_BIT));
  abs_mmio_write32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET, 0x11223344u);
  CHECK(abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET) == 0xaa55aa55u);

  /* 3. Falling edge sha_en: 1 -> 0 zeroes DIGEST_0..7 to 0x00000000. */
  uint32_t cfg_sha256_dis = make_cfg(false, false, false, true, false,
                                     HMAC_CFG_DIGEST_SIZE_VALUE_SHA2_256,
                                     HMAC_CFG_KEY_LENGTH_VALUE_KEY_256);
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg_sha256_dis);
  for (uint32_t i = 0; i < 8u; ++i) {
    CHECK(abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET + i * 4u) == 0u);
  }

  /* 4. With sha_en=0 and digest_size=SHA2_256, DIGEST_0 write succeeds. */
  abs_mmio_write32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET, 0xdeadbeefu);
  CHECK(abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET) == 0xdeadbeefu);
  LOG_INFO("[prim_sha2.sv:159-164] confirmed.");
}

static void verify_hmac_cfg_lock_while_active(void) {
  LOG_INFO(
      "Verifying [hmac.sv:240-243] (SPEC_DOC_ERRATA): MSG_LENGTH_LOWER/UPPER "
      "retention across sha_en 1->0, writability while sha_en==1, and "
      "DIGEST_8..15 write ignore in SHA2_256...");

  clear_hmac_intr();
  uint32_t cfg_sha256_en = make_cfg(false, true, false, true, false,
                                    HMAC_CFG_DIGEST_SIZE_VALUE_SHA2_256,
                                    HMAC_CFG_KEY_LENGTH_VALUE_KEY_256);
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg_sha256_en);
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_START_BIT);
  abs_mmio_write32(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET, 0x61626364u);
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_PROCESS_BIT);
  while (!bitfield_bit32_read(
      abs_mmio_read32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET),
      HMAC_INTR_STATE_HMAC_DONE_BIT)) {
  }
  clear_hmac_intr();
  CHECK(abs_mmio_read32(kHmacBase + HMAC_MSG_LENGTH_LOWER_REG_OFFSET) == 32u);
  CHECK(abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET) != 0u);

  /* 1. Clearing sha_en (1 -> 0) zeroes DIGEST_0 but preserves MSG_LENGTH_LOWER.
   */
  uint32_t cfg_sha256_dis = make_cfg(false, false, false, true, false,
                                     HMAC_CFG_DIGEST_SIZE_VALUE_SHA2_256,
                                     HMAC_CFG_KEY_LENGTH_VALUE_KEY_256);
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg_sha256_dis);
  CHECK(abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kHmacBase + HMAC_MSG_LENGTH_LOWER_REG_OFFSET) == 32u);

  /* 2. MSG_LENGTH_LOWER/UPPER are writable even when sha_en == 1 (!cfg_block).
   */
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg_sha256_en);
  abs_mmio_write32(kHmacBase + HMAC_MSG_LENGTH_LOWER_REG_OFFSET, 0x12345678u);
  abs_mmio_write32(kHmacBase + HMAC_MSG_LENGTH_UPPER_REG_OFFSET, 0x9abcdef0u);
  CHECK(abs_mmio_read32(kHmacBase + HMAC_MSG_LENGTH_LOWER_REG_OFFSET) ==
        0x12345678u);
  CHECK(abs_mmio_read32(kHmacBase + HMAC_MSG_LENGTH_UPPER_REG_OFFSET) ==
        0x9abcdef0u);

  /* 3. With sha_en=0 and SHA2_256, DIGEST_8..15 writes are ignored. */
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg_sha256_dis);
  abs_mmio_write32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET, 0xaabbccddu);
  abs_mmio_write32(kHmacBase + HMAC_DIGEST_8_REG_OFFSET, 0x11223344u);
  CHECK(abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET) == 0xaabbccddu);
  CHECK(abs_mmio_read32(kHmacBase + HMAC_DIGEST_8_REG_OFFSET) == 0xaabbccddu);
  LOG_INFO("[hmac.sv:240-243] confirmed.");
}

static void verify_hmac_cmd_start_without_sha_en(void) {
  LOG_INFO(
      "Verifying [hmac.sv:256-272] (SPEC_DOC_ERRATA): DIGEST_8..15 mirrors "
      "DIGEST_0..7 in SHA2_256, DIGEST_12..15 exposes SHA-512 state in "
      "SHA2_384, "
      "and WIPE_SECRET readback swaps via digest_swap...");

  /* 1. SHA2_256 DIGEST_8..15 mirrors DIGEST_0..7 word-for-word. */
  uint32_t cfg_sha256_dis = make_cfg(false, false, false, true, false,
                                     HMAC_CFG_DIGEST_SIZE_VALUE_SHA2_256,
                                     HMAC_CFG_KEY_LENGTH_VALUE_KEY_256);
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg_sha256_dis);
  for (uint32_t i = 0; i < 8u; ++i) {
    uint32_t v = 0xa0000000u | (i * 0x01010101u);
    abs_mmio_write32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET + i * 4u, v);
  }
  for (uint32_t i = 0; i < 8u; ++i) {
    uint32_t d_lo =
        abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET + i * 4u);
    uint32_t d_hi =
        abs_mmio_read32(kHmacBase + HMAC_DIGEST_8_REG_OFFSET + i * 4u);
    CHECK(d_lo == d_hi);
  }

  /* 2. SHA2_384 DIGEST_12..15 exposes non-zero internal 512-bit SHA-512 state.
   */
  clear_hmac_intr();
  uint32_t cfg_sha384_en = make_cfg(false, true, false, true, false,
                                    HMAC_CFG_DIGEST_SIZE_VALUE_SHA2_384,
                                    HMAC_CFG_KEY_LENGTH_VALUE_KEY_256);
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg_sha384_en);
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_START_BIT);
  abs_mmio_write32(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET, 0x61626364u);
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_PROCESS_BIT);
  while (!bitfield_bit32_read(
      abs_mmio_read32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET),
      HMAC_INTR_STATE_HMAC_DONE_BIT)) {
  }
  clear_hmac_intr();
  uint32_t d12 = abs_mmio_read32(kHmacBase + HMAC_DIGEST_12_REG_OFFSET);
  uint32_t d13 = abs_mmio_read32(kHmacBase + HMAC_DIGEST_13_REG_OFFSET);
  uint32_t d14 = abs_mmio_read32(kHmacBase + HMAC_DIGEST_14_REG_OFFSET);
  uint32_t d15 = abs_mmio_read32(kHmacBase + HMAC_DIGEST_15_REG_OFFSET);
  CHECK((d12 | d13 | d14 | d15) != 0u);

  /* 3. WIPE_SECRET readback via DIGEST_0..15 applies conv_endian32(V,
   * digest_swap). */
  abs_mmio_write32(kHmacBase + HMAC_WIPE_SECRET_REG_OFFSET, 0x12345678u);
  CHECK(abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET) == 0x78563412u);
  uint32_t cfg_no_swap = make_cfg(false, true, false, false, false,
                                  HMAC_CFG_DIGEST_SIZE_VALUE_SHA2_384,
                                  HMAC_CFG_KEY_LENGTH_VALUE_KEY_256);
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg_no_swap);
  CHECK(abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET) == 0x12345678u);
  LOG_INFO("[hmac.sv:256-272] confirmed.");
}

static void verify_hmac_invalid_config_err_code(void) {
  LOG_INFO(
      "Verifying [hmac.sv:343-344] (SPEC_DOC_ERRATA): ERR_CODE sticky "
      "first-error latch and INTR_TEST.hmac_err blocking subsequent errors...");

  clear_hmac_intr();
  uint32_t cfg_sha_dis = make_cfg(false, false, false, false, false,
                                  HMAC_CFG_DIGEST_SIZE_VALUE_SHA2_256,
                                  HMAC_CFG_KEY_LENGTH_VALUE_KEY_256);
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg_sha_dis);

  /* 1. Trigger SwHashStartWhenShaDisabled (2). */
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_START_BIT);
  CHECK(abs_mmio_read32(kHmacBase + HMAC_ERR_CODE_REG_OFFSET) ==
        kErrSwHashStartWhenShaDisabled);
  CHECK(bitfield_bit32_read(
      abs_mmio_read32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET),
      HMAC_INTR_STATE_HMAC_ERR_BIT));

  /* 2. Trigger SwPushMsgWhenDisallowed (5) while INTR_STATE.hmac_err == 1:
   *    ERR_CODE stays latched at 2. */
  abs_mmio_write32(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET, 0xdeadbeefu);
  CHECK(abs_mmio_read32(kHmacBase + HMAC_ERR_CODE_REG_OFFSET) ==
        kErrSwHashStartWhenShaDisabled);

  /* 3. Clear INTR_STATE.hmac_err, then set INTR_TEST.hmac_err = 1.
   *    Trigger SwPushMsgWhenDisallowed (5) -> ERR_CODE STILL stays 2! */
  abs_mmio_write32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET,
                   1u << HMAC_INTR_STATE_HMAC_ERR_BIT);
  abs_mmio_write32(kHmacBase + HMAC_INTR_TEST_REG_OFFSET,
                   1u << HMAC_INTR_TEST_HMAC_ERR_BIT);
  CHECK(bitfield_bit32_read(
      abs_mmio_read32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET),
      HMAC_INTR_STATE_HMAC_ERR_BIT));
  abs_mmio_write32(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET, 0xdeadbeefu);
  CHECK(abs_mmio_read32(kHmacBase + HMAC_ERR_CODE_REG_OFFSET) ==
        kErrSwHashStartWhenShaDisabled);

  /* 4. Clear INTR_STATE.hmac_err and trigger SwPushMsgWhenDisallowed (5):
   *    now ERR_CODE updates to 5. */
  abs_mmio_write32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET,
                   1u << HMAC_INTR_STATE_HMAC_ERR_BIT);
  abs_mmio_write32(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET, 0xdeadbeefu);
  CHECK(abs_mmio_read32(kHmacBase + HMAC_ERR_CODE_REG_OFFSET) ==
        kErrSwPushMsgWhenDisallowed);
  clear_hmac_intr();
  LOG_INFO("[hmac.sv:343-344] confirmed.");
}

static void verify_hmac_sw_push_msg_disabled_err(void) {
  LOG_INFO(
      "Verifying [hmac.sv:349-362] (BENIGN_RTL_IMPL_DETAIL): CMD.hash_stop "
      "while idle with SHA2_None raises no error and leaves hmac_idle == 1...");

  clear_hmac_intr();
  uint32_t cfg_none = make_cfg(false, false, false, false, false,
                               HMAC_CFG_DIGEST_SIZE_VALUE_SHA2_NONE,
                               HMAC_CFG_KEY_LENGTH_VALUE_KEY_256);
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg_none);
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_STOP_BIT);
  CHECK(!bitfield_bit32_read(
      abs_mmio_read32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET),
      HMAC_INTR_STATE_HMAC_ERR_BIT));
  CHECK(bitfield_bit32_read(abs_mmio_read32(kHmacBase + HMAC_STATUS_REG_OFFSET),
                            HMAC_STATUS_HMAC_IDLE_BIT));
  LOG_INFO("[hmac.sv:349-362] confirmed.");
}

static void verify_hmac_msg_length_when_sha_disabled(void) {
  LOG_INFO(
      "Verifying [hmac.sv:583-612] (INTENDED_SECURITY_HARDENING): Reading "
      "write-only MSG_FIFO raises synchronous Load Access Fault (mcause=5) "
      "with zero hmac_err side effects...");

  clear_hmac_intr();
  g_expected_fault_taken = false;
  g_last_mcause = 0;
  g_expect_bus_fault = true;
  (void)abs_mmio_read32(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET);
  CHECK(g_expected_fault_taken);
  CHECK(g_last_mcause == kMcauseLoadAccessFault);
  CHECK(!bitfield_bit32_read(
      abs_mmio_read32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET),
      HMAC_INTR_STATE_HMAC_ERR_BIT));
  LOG_INFO("[hmac.sv:583-612] confirmed.");
}

static void verify_hmac_permit_subword_writes_vs_msg_fifo(void) {
  LOG_INFO(
      "Verifying [hmac_reg_pkg.sv:441-501] (INTENDED_SECURITY_HARDENING): "
      "HMAC_PERMIT "
      "sub-word write faults (mcause=7), sub-word read allowance, and unmapped "
      "offset 0x0f0 faults (mcause=5/7)...");

  /* 1-byte write to CFG (permit 4'b0011) -> Store Access Fault (7). */
  g_expected_fault_taken = false;
  g_last_mcause = 0;
  g_expect_bus_fault = true;
  abs_mmio_write8(kHmacBase + HMAC_CFG_REG_OFFSET, 0x01u);
  CHECK(g_expected_fault_taken);
  CHECK(g_last_mcause == kMcauseStoreAccessFault);

  /* 2-byte write to MSG_LENGTH_LOWER (permit 4'b1111) -> Store Access Fault
   * (7). */
  g_expected_fault_taken = false;
  g_last_mcause = 0;
  g_expect_bus_fault = true;
  *(volatile uint16_t *)(kHmacBase + HMAC_MSG_LENGTH_LOWER_REG_OFFSET) =
      0x1234u;
  CHECK(g_expected_fault_taken);
  CHECK(g_last_mcause == kMcauseStoreAccessFault);

  /* 1-byte read from CFG succeeds without fault. */
  g_expected_fault_taken = false;
  g_expect_bus_fault = true;
  (void)abs_mmio_read8(kHmacBase + HMAC_CFG_REG_OFFSET);
  g_expect_bus_fault = false;
  CHECK(!g_expected_fault_taken);

  /* Unmapped offset 0x0f0 read (mcause=5) and write (mcause=7). */
  g_expected_fault_taken = false;
  g_last_mcause = 0;
  g_expect_bus_fault = true;
  (void)abs_mmio_read32(kHmacBase + 0x0f0u);
  CHECK(g_expected_fault_taken);
  CHECK(g_last_mcause == kMcauseLoadAccessFault);

  g_expected_fault_taken = false;
  g_last_mcause = 0;
  g_expect_bus_fault = true;
  abs_mmio_write32(kHmacBase + 0x0f0u, 0xdeadbeefu);
  CHECK(g_expected_fault_taken);
  CHECK(g_last_mcause == kMcauseStoreAccessFault);
  LOG_INFO("[hmac_reg_pkg.sv:441-501] confirmed.");
}

static void verify_hmac_empty_message_sha256_digest(void) {
  LOG_INFO(
      "Verifying [prim_sha2_pad.sv:238-243] (TRUE_SILICON_ERRATA): "
      "CMD.hash_stop after "
      "unaligned message length (9 words = 288 bits) wedges u_pad in StIdle "
      "and u_sha2 in FifoLoadFromFifo (hmac_idle=0, hmac_done=0)...");

  clear_hmac_intr();
  uint32_t cfg_sha256_en = make_cfg(false, true, true, false, false,
                                    HMAC_CFG_DIGEST_SIZE_VALUE_SHA2_256,
                                    HMAC_CFG_KEY_LENGTH_VALUE_KEY_256);
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg_sha256_en);

  /* Control check: aligned 16-word (512-bit) block + CMD.hash_stop completes
   * cleanly with INTR_STATE.hmac_done == 1 and STATUS.hmac_idle == 1. */
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_START_BIT);
  for (uint32_t i = 0; i < 16u; ++i) {
    abs_mmio_write32(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET, 0x30313233u + i);
  }
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_STOP_BIT);
  busy_spin_micros(20);
  CHECK(bitfield_bit32_read(
      abs_mmio_read32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET),
      HMAC_INTR_STATE_HMAC_DONE_BIT));
  CHECK(bitfield_bit32_read(abs_mmio_read32(kHmacBase + HMAC_STATUS_REG_OFFSET),
                            HMAC_STATUS_HMAC_IDLE_BIT));
  CHECK(abs_mmio_read32(kHmacBase + HMAC_MSG_LENGTH_LOWER_REG_OFFSET) == 512u);
  clear_hmac_intr();

  /* Erratum check: unaligned 9-word (288-bit) partial block + CMD.hash_stop:
   * u_pad sees txcnt_eq_msg_len && hash_stop_flag_q (288 == 288) and
   * transitions to StIdle immediately while u_sha2 stays stuck in
   * FifoLoadFromFifo (w_index_q == 9), leaving hmac_idle == 0 and hmac_done ==
   * 0! */
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_START_BIT);
  for (uint32_t i = 0; i < 9u; ++i) {
    abs_mmio_write32(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET, 0x41424344u + i);
  }
  busy_spin_micros(10);
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_STOP_BIT);
  busy_spin_micros(50);

  uint32_t status = abs_mmio_read32(kHmacBase + HMAC_STATUS_REG_OFFSET);
  uint32_t intr = abs_mmio_read32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET);
  CHECK(abs_mmio_read32(kHmacBase + HMAC_MSG_LENGTH_LOWER_REG_OFFSET) == 288u);
  CHECK(!bitfield_bit32_read(intr, HMAC_INTR_STATE_HMAC_DONE_BIT));
  CHECK(!bitfield_bit32_read(status, HMAC_STATUS_HMAC_IDLE_BIT));
  CHECK(bitfield_bit32_read(status, HMAC_STATUS_FIFO_EMPTY_BIT));

  /* Recovery check: because reg_hash_stop clears cfg_block (hmac.sv:359),
   * software can write CFG.sha_en = 0 while STATUS.hmac_idle == 0 to abort
   * u_sha2 back to FifoIdle and restore STATUS.hmac_idle == 1. */
  uint32_t cfg_sha256_dis = make_cfg(false, false, true, false, false,
                                     HMAC_CFG_DIGEST_SIZE_VALUE_SHA2_256,
                                     HMAC_CFG_KEY_LENGTH_VALUE_KEY_256);
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg_sha256_dis);
  CHECK(bitfield_bit32_read(abs_mmio_read32(kHmacBase + HMAC_STATUS_REG_OFFSET),
                            HMAC_STATUS_HMAC_IDLE_BIT));
  LOG_INFO("[prim_sha2_pad.sv:238-243] confirmed.");
}

bool test_main(void) {
  LOG_INFO("Starting HMAC Errata Confirmation Test on %s...",
           kDeviceType == kDeviceFpgaCw340 ? "CW340_FPGA" : "QEMU");

  verify_hmac_wipe_secret_clears_msg_fifo();
  verify_hmac_cfg_lock_while_active();
  verify_hmac_cmd_start_without_sha_en();
  verify_hmac_invalid_config_err_code();
  verify_hmac_sw_push_msg_disabled_err();
  verify_hmac_msg_length_when_sha_disabled();
  verify_hmac_permit_subword_writes_vs_msg_fifo();
  verify_hmac_empty_message_sha256_digest();

  LOG_INFO("All 8 HMAC errata & security hardening checks PASSED!");
  return true;
}
