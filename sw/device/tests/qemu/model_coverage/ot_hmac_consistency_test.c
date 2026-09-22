// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * CW340 FPGA & QEMU Consistency Test for `ot_hmac` (`hw/opentitan/ot_hmac.c`).
 *
 * Strictly verifies identical hardware behavior between CW340 FPGA and QEMU
 * for:
 * 1. `CMD.HASH_STOP` & `CMD.HASH_CONTINUE` mid-stream SHA-256 block-boundary
 *    pause, intermediate state save (`DIGEST_0..7`, `MSG_LENGTH_LOWER/UPPER`),
 *    secret wipe (`WIPE_SECRET` & `CFG.SHA_EN` falling edge), context restore,
 *    and `CMD.HASH_CONTINUE` resume to completion.
 * 2. `CMD.HASH_CONTINUE` error conditions (`SwHashStartWhenShaDisabled = 0x2`
 *    when `CFG.SHA_EN == 0`, and `SwHashStartWhenActive = 0x4` when active).
 * 3. Register access restrictions while non-idle (`SwUpdateSecretKeyInProcess =
 * 0x3` on `KEY_0` write, ignored writes to `DIGEST_0` and
 * `MSG_LENGTH_LOWER/UPPER`), write-only `KEY_0..7` readback `0`, and read-only
 * `ERR_CODE`/`STATUS` writes.
 * 4. 16-bit sub-word `MSG_FIFO` writes (`uint16_t` MMIO store) with
 * `CFG.ENDIAN_SWAP = 1`.
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/ip/hmac/data/hmac_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kHmacBase = TOP_EARLGREY_HMAC_BASE_ADDR,
  kErrHashStartWhenShaDisabled = 0x02u,
  kErrUpdateSecretKeyInProcess = 0x03u,
  kErrHashStartWhenActive = 0x04u,
  kErrPushMsgWhenDisallowed = 0x05u,
  kErrInvalidConfig = 0x06u,
};

// Standard NIST SHA-256("abc") big-endian digest words:
static const uint32_t kSha256AbcDigest[8] = {
    0xba7816bfu, 0x8f01cfeau, 0x414140deu, 0x5dae2223u,
    0xb00361a3u, 0x96177a9cu, 0xb410ff61u, 0xf20015adu,
};

// Standard NIST SHA-256("") big-endian digest words:
static const uint32_t kSha256EmptyDigest[8] = {
    0xe3b0c442u, 0x98fc1c14u, 0x9afbf4c8u, 0x996fb924u,
    0x27ae41e4u, 0x649b934cu, 0xa495991bu, 0x7852b855u,
};

static void wait_for_hmac_idle_and_done(void) {
  for (uint32_t i = 0; i < 100000; ++i) {
    uint32_t status = abs_mmio_read32(kHmacBase + HMAC_STATUS_REG_OFFSET);
    if (bitfield_bit32_read(status, HMAC_STATUS_HMAC_IDLE_BIT)) {
      break;
    }
  }
  uint32_t status = abs_mmio_read32(kHmacBase + HMAC_STATUS_REG_OFFSET);
  CHECK(bitfield_bit32_read(status, HMAC_STATUS_HMAC_IDLE_BIT));

  uint32_t intr = abs_mmio_read32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET);
  CHECK(bitfield_bit32_read(intr, HMAC_INTR_STATE_HMAC_DONE_BIT));
  abs_mmio_write32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET,
                   1u << HMAC_INTR_STATE_HMAC_DONE_BIT);
  CHECK(abs_mmio_read32(kHmacBase + HMAC_CMD_REG_OFFSET) == 0u);
}

static void test_hash_stop_and_continue(void) {
  LOG_INFO("Section 1: CMD.HASH_STOP & CMD.HASH_CONTINUE mid-stream SHA-256");

  // Prepare two 64-byte (16-word) message blocks.
  uint32_t block0[16];
  uint32_t block1[16];
  for (uint32_t i = 0; i < 16; ++i) {
    block0[i] = 0x30313233u ^ (i * 0x01010101u);
    block1[i] = 0x41424344u ^ (i * 0x02020202u);
  }

  uint32_t cfg = bitfield_bit32_write(0u, HMAC_CFG_SHA_EN_BIT, true);
  cfg = bitfield_bit32_write(cfg, HMAC_CFG_HMAC_EN_BIT, false);
  cfg = bitfield_bit32_write(cfg, HMAC_CFG_ENDIAN_SWAP_BIT, false);
  cfg = bitfield_bit32_write(cfg, HMAC_CFG_DIGEST_SWAP_BIT, false);
  cfg = bitfield_field32_write(cfg, HMAC_CFG_DIGEST_SIZE_FIELD,
                               HMAC_CFG_DIGEST_SIZE_VALUE_SHA2_256);

  // First, compute the reference 2-block SHA-256 digest in a single shot.
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, 0u);
  abs_mmio_write32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET, 0x7u);
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg);
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_START_BIT);
  for (uint32_t i = 0; i < 16; ++i) {
    abs_mmio_write32(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET, block0[i]);
  }
  for (uint32_t i = 0; i < 16; ++i) {
    abs_mmio_write32(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET, block1[i]);
  }
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_PROCESS_BIT);
  wait_for_hmac_idle_and_done();
  CHECK(abs_mmio_read32(kHmacBase + HMAC_MSG_LENGTH_LOWER_REG_OFFSET) == 1024u);
  CHECK(abs_mmio_read32(kHmacBase + HMAC_MSG_LENGTH_UPPER_REG_OFFSET) == 0u);

  uint32_t expected_digest[8];
  for (uint32_t i = 0; i < 8; ++i) {
    expected_digest[i] =
        abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET + i * 4u);
  }

  // Clear SHA_EN (1 -> 0) and verify DIGEST_0..7 clears to 0.
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, 0u);
  for (uint32_t i = 0; i < 8; ++i) {
    CHECK(abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET + i * 4u) == 0u);
  }

  // Now start the split SHA-256 operation: push block0 (64 bytes = 512 bits)
  // and immediately issue CMD.HASH_STOP.
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg);
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_START_BIT);
  for (uint32_t i = 0; i < 16; ++i) {
    abs_mmio_write32(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET, block0[i]);
  }
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_STOP_BIT);

  wait_for_hmac_idle_and_done();
  CHECK(abs_mmio_read32(kHmacBase + HMAC_MSG_LENGTH_LOWER_REG_OFFSET) == 512u);
  CHECK(abs_mmio_read32(kHmacBase + HMAC_MSG_LENGTH_UPPER_REG_OFFSET) == 0u);

  uint32_t saved_state[8];
  bool any_nonzero = false;
  for (uint32_t i = 0; i < 8; ++i) {
    saved_state[i] =
        abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET + i * 4u);
    if (saved_state[i] != 0u) {
      any_nonzero = true;
    }
  }
  CHECK(any_nonzero);

  // Deassert SHA_EN and wipe secret state.
  uint32_t cfg_disabled = bitfield_bit32_write(cfg, HMAC_CFG_SHA_EN_BIT, false);
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg_disabled);
  abs_mmio_write32(kHmacBase + HMAC_WIPE_SECRET_REG_OFFSET, 0xa5a55a5au);
  for (uint32_t i = 0; i < 8; ++i) {
    CHECK(abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET + i * 4u) ==
          0xa5a55a5au);
  }

  // Restore saved intermediate state and message length while SHA_EN == 0.
  for (uint32_t i = 0; i < 8; ++i) {
    abs_mmio_write32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET + i * 4u,
                     saved_state[i]);
    CHECK(abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET + i * 4u) ==
          saved_state[i]);
  }
  abs_mmio_write32(kHmacBase + HMAC_MSG_LENGTH_LOWER_REG_OFFSET, 512u);
  abs_mmio_write32(kHmacBase + HMAC_MSG_LENGTH_UPPER_REG_OFFSET, 0u);

  // Re-enable SHA_EN and issue CMD.HASH_CONTINUE.
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg);
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_CONTINUE_BIT);

  // Push block1 (64 bytes) and issue CMD.HASH_PROCESS.
  for (uint32_t i = 0; i < 16; ++i) {
    abs_mmio_write32(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET, block1[i]);
  }
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_PROCESS_BIT);
  wait_for_hmac_idle_and_done();

  CHECK(abs_mmio_read32(kHmacBase + HMAC_MSG_LENGTH_LOWER_REG_OFFSET) == 1024u);
  CHECK(abs_mmio_read32(kHmacBase + HMAC_MSG_LENGTH_UPPER_REG_OFFSET) == 0u);
  for (uint32_t i = 0; i < 8; ++i) {
    uint32_t actual =
        abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET + i * 4u);
    CHECK(actual == expected_digest[i]);
  }
}

static void test_hash_continue_errors_and_non_idle_restrictions(void) {
  LOG_INFO(
      "Section 2 & 3: CMD.HASH_CONTINUE errors & non-idle register guards");

  // Configure valid digest_size = SHA2_256 with SHA_EN = 0.
  uint32_t cfg_disabled = bitfield_field32_write(
      0u, HMAC_CFG_DIGEST_SIZE_FIELD, HMAC_CFG_DIGEST_SIZE_VALUE_SHA2_256);
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg_disabled);
  abs_mmio_write32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET, 0x7u);

  // 2a. Issue CMD.HASH_CONTINUE when SHA_EN == 0 -> SwHashStartWhenShaDisabled
  // (0x2).
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_CONTINUE_BIT);
  uint32_t intr = abs_mmio_read32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET);
  CHECK(bitfield_bit32_read(intr, HMAC_INTR_STATE_HMAC_ERR_BIT));
  CHECK(abs_mmio_read32(kHmacBase + HMAC_ERR_CODE_REG_OFFSET) ==
        kErrHashStartWhenShaDisabled);

  // 3a. Write to read-only ERR_CODE and STATUS registers: must be ignored.
  abs_mmio_write32(kHmacBase + HMAC_ERR_CODE_REG_OFFSET, 0xffffffffu);
  CHECK(abs_mmio_read32(kHmacBase + HMAC_ERR_CODE_REG_OFFSET) ==
        kErrHashStartWhenShaDisabled);
  uint32_t status_before = abs_mmio_read32(kHmacBase + HMAC_STATUS_REG_OFFSET);
  abs_mmio_write32(kHmacBase + HMAC_STATUS_REG_OFFSET, 0xffffffffu);
  CHECK(abs_mmio_read32(kHmacBase + HMAC_STATUS_REG_OFFSET) == status_before);

  // Clear HMAC_ERR W1C.
  abs_mmio_write32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET,
                   1u << HMAC_INTR_STATE_HMAC_ERR_BIT);
  CHECK((abs_mmio_read32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET) &
         (1u << HMAC_INTR_STATE_HMAC_ERR_BIT)) == 0u);

  // 3b. Write KEY_0..7 while idle, then read back write-only KEY_0..7 -> must
  // read 0.
  for (uint32_t i = 0; i < 8; ++i) {
    abs_mmio_write32(kHmacBase + HMAC_KEY_0_REG_OFFSET + i * 4u,
                     0x11111111u * (i + 1u));
    CHECK(abs_mmio_read32(kHmacBase + HMAC_KEY_0_REG_OFFSET + i * 4u) == 0u);
  }

  // Enable SHA_EN = 1 and start hash (engine is now non-idle, cfg_block == 1).
  uint32_t cfg_enabled =
      bitfield_bit32_write(cfg_disabled, HMAC_CFG_SHA_EN_BIT, true);
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg_enabled);
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_START_BIT);

  // 2b. Issue CMD.HASH_CONTINUE while active -> SwHashStartWhenActive (0x4).
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_CONTINUE_BIT);
  intr = abs_mmio_read32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET);
  CHECK(bitfield_bit32_read(intr, HMAC_INTR_STATE_HMAC_ERR_BIT));
  CHECK(abs_mmio_read32(kHmacBase + HMAC_ERR_CODE_REG_OFFSET) ==
        kErrHashStartWhenActive);
  abs_mmio_write32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET,
                   1u << HMAC_INTR_STATE_HMAC_ERR_BIT);

  // 3c. Write KEY_0 while non-idle -> SwUpdateSecretKeyInProcess (0x3).
  abs_mmio_write32(kHmacBase + HMAC_KEY_0_REG_OFFSET, 0xdeadbeefu);
  intr = abs_mmio_read32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET);
  CHECK(bitfield_bit32_read(intr, HMAC_INTR_STATE_HMAC_ERR_BIT));
  CHECK(abs_mmio_read32(kHmacBase + HMAC_ERR_CODE_REG_OFFSET) ==
        kErrUpdateSecretKeyInProcess);
  abs_mmio_write32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET,
                   1u << HMAC_INTR_STATE_HMAC_ERR_BIT);

  // 3d. Write DIGEST_0, MSG_LENGTH_LOWER, and MSG_LENGTH_UPPER while non-idle:
  // all must be ignored.
  abs_mmio_write32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET, 0x12345678u);
  abs_mmio_write32(kHmacBase + HMAC_MSG_LENGTH_LOWER_REG_OFFSET, 0x00001000u);
  abs_mmio_write32(kHmacBase + HMAC_MSG_LENGTH_UPPER_REG_OFFSET, 0x00000001u);
  CHECK(abs_mmio_read32(kHmacBase + HMAC_MSG_LENGTH_LOWER_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kHmacBase + HMAC_MSG_LENGTH_UPPER_REG_OFFSET) == 0u);

  // Complete the empty SHA-256 hash and verify DIGEST_0..7 matches SHA-256("").
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_PROCESS_BIT);
  wait_for_hmac_idle_and_done();
  for (uint32_t i = 0; i < 8; ++i) {
    CHECK(abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET + i * 4u) ==
          kSha256EmptyDigest[i]);
  }
}

static void test_subword_16bit_fifo_write_endian_swap(void) {
  LOG_INFO("Section 4: 16-bit MSG_FIFO write with CFG.ENDIAN_SWAP = 1");

  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, 0u);
  uint32_t cfg = bitfield_bit32_write(0u, HMAC_CFG_SHA_EN_BIT, true);
  cfg = bitfield_bit32_write(cfg, HMAC_CFG_ENDIAN_SWAP_BIT, true);
  cfg = bitfield_bit32_write(cfg, HMAC_CFG_DIGEST_SWAP_BIT, false);
  cfg = bitfield_field32_write(cfg, HMAC_CFG_DIGEST_SIZE_FIELD,
                               HMAC_CFG_DIGEST_SIZE_VALUE_SHA2_256);
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg);

  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_START_BIT);

  // With ENDIAN_SWAP = 1, a 16-bit write of 0x6162 ('a' in MSB, 'b' in LSB)
  // byte-swaps to push 'a' (0x61) then 'b' (0x62), followed by 8-bit 'c'
  // (0x63).
  *((volatile uint16_t *)(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET)) = 0x6162u;
  abs_mmio_write8(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET, 0x63u);

  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_PROCESS_BIT);
  wait_for_hmac_idle_and_done();

  CHECK(abs_mmio_read32(kHmacBase + HMAC_MSG_LENGTH_LOWER_REG_OFFSET) == 24u);
  CHECK(abs_mmio_read32(kHmacBase + HMAC_MSG_LENGTH_UPPER_REG_OFFSET) == 0u);
  for (uint32_t i = 0; i < 8; ++i) {
    CHECK(abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET + i * 4u) ==
          kSha256AbcDigest[i]);
  }
}

static void test_msg_length_overflow_and_duplicate_hash_process(void) {
  LOG_INFO(
      "Section 5: 64-bit MSG_LENGTH overflow stall & duplicate "
      "CMD.HASH_PROCESS");

  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, 0u);
  abs_mmio_write32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET, 0x7u);

  // Seed 64-bit MSG_LENGTH to 2^64 - 32 bits (0xffffffff_ffffffe0) while idle.
  abs_mmio_write32(kHmacBase + HMAC_MSG_LENGTH_UPPER_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kHmacBase + HMAC_MSG_LENGTH_LOWER_REG_OFFSET, 0xffffffe0u);

  uint32_t cfg = bitfield_bit32_write(0u, HMAC_CFG_SHA_EN_BIT, true);
  cfg = bitfield_bit32_write(cfg, HMAC_CFG_HMAC_EN_BIT, false);
  cfg = bitfield_field32_write(cfg, HMAC_CFG_DIGEST_SIZE_FIELD,
                               HMAC_CFG_DIGEST_SIZE_VALUE_SHA2_256);
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg);

  // Resume via CMD.HASH_CONTINUE so seeded MSG_LENGTH is preserved.
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_CONTINUE_BIT);

  // Push 4 bytes (32 bits) to wrap 64-bit MSG_LENGTH to 0 and latch overflow.
  abs_mmio_write32(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET, 0x61626364u);
  CHECK(abs_mmio_read32(kHmacBase + HMAC_MSG_LENGTH_UPPER_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kHmacBase + HMAC_MSG_LENGTH_LOWER_REG_OFFSET) == 0u);

  // First CMD.HASH_PROCESS: stalls without asserting hmac_done due to overflow.
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_PROCESS_BIT);
  uint32_t intr = abs_mmio_read32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET);
  CHECK((intr & ((1u << HMAC_INTR_STATE_HMAC_DONE_BIT) |
                 (1u << HMAC_INTR_STATE_HMAC_ERR_BIT))) == 0u);

  // Second CMD.HASH_PROCESS while hash_process is already active: ignored
  // without asserting hmac_done or hmac_err.
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_PROCESS_BIT);
  intr = abs_mmio_read32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET);
  CHECK((intr & ((1u << HMAC_INTR_STATE_HMAC_DONE_BIT) |
                 (1u << HMAC_INTR_STATE_HMAC_ERR_BIT))) == 0u);

  // While stalled in HASH_PROCESS (cfg_block == 1), writing CFG = 0 is ignored
  // and STATUS.HMAC_IDLE remains 0.
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, 0u);
  uint32_t status = abs_mmio_read32(kHmacBase + HMAC_STATUS_REG_OFFSET);
  CHECK(!bitfield_bit32_read(status, HMAC_STATUS_HMAC_IDLE_BIT));

  // Issuing CMD.HASH_STOP clears cfg_block (hmac.sv:359), allowing CFG = 0
  // (SHA_EN = 0) to reset the SHA2 pad/core FSMs and restore HMAC_IDLE = 1.
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_STOP_BIT);
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, 0u);
  status = abs_mmio_read32(kHmacBase + HMAC_STATUS_REG_OFFSET);
  CHECK(bitfield_bit32_read(status, HMAC_STATUS_HMAC_IDLE_BIT));
}

bool test_main(void) {
  test_hash_stop_and_continue();
  test_hash_continue_errors_and_non_idle_restrictions();
  test_subword_16bit_fifo_write_endian_swap();
  test_msg_length_overflow_and_duplicate_hash_process();
  return true;
}
