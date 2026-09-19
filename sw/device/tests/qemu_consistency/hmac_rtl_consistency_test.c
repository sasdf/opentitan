// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/dif/dif_hmac.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hmac_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kHmacBase = TOP_EARLGREY_HMAC_BASE_ADDR,
  kHmacErrPushMsgWhenShaDisabled = 0x1u,
  kHmacErrHashStartWhenShaDisabled = 0x2u,
  kHmacErrUpdateSecretKeyInProcess = 0x3u,
  kHmacErrHashStartWhenActive = 0x4u,
  kHmacErrPushMsgWhenDisallowed = 0x5u,
  kHmacErrInvalidConfig = 0x6u,
};

#define EXPECT_RTL(cond, ...)                    \
  do {                                           \
    if (!(cond)) {                               \
      LOG_ERROR("RTL_MISMATCH: " __VA_ARGS__);   \
      all_ok = false;                            \
    }                                            \
  } while (0)

bool test_main(void) {
  bool all_ok = true;

  // Clear any pending interrupts left from ROM/ROM_EXT boot.
  abs_mmio_write32(kHmacBase + HMAC_INTR_ENABLE_REG_OFFSET, 0u);
  abs_mmio_write32(kHmacBase + HMAC_INTR_TEST_REG_OFFSET, 0u);
  abs_mmio_write32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET, 0x7u);

  // 1. CFG invalid digest_size / key_length normalization & reserved bits mask.
  // In RTL (hmac.sv:286-336), writing 0 to digest_size (bits 8:5) maps to
  // SHA2_None (0x8 -> 0x100) and writing 0 to key_length (bits 14:9) maps to
  // Key_None (0x20 -> 0x4000), producing 0x4100. Reserved bits [31:15] read as 0.
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, 0u);
  uint32_t cfg = abs_mmio_read32(kHmacBase + HMAC_CFG_REG_OFFSET);
  EXPECT_RTL(cfg == 0x4100u,
             "CFG(0) expected normalized 0x4100 (SHA2_None|Key_None), got 0x%08x",
             cfg);

  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, 0xffff8000u);
  cfg = abs_mmio_read32(kHmacBase + HMAC_CFG_REG_OFFSET);
  EXPECT_RTL(cfg == 0x4100u,
             "CFG(0xffff8000) reserved bits should be masked out, got 0x%08x",
             cfg);


  // 2. WIPE_SECRET readback endianness on DIGEST_0..15.
  // In RTL (prim_sha2.sv:148 & hmac.sv:270), writing WIPE_SECRET = V sets each
  // 32-bit digest word to V, and reading DIGEST_k returns conv_endian32(V, digest_swap):
  // - when CFG.digest_swap == 0, DIGEST_k reads as V (0x11223344).
  // - when CFG.digest_swap == 1, DIGEST_k reads as bswap32(V) (0x44332211).
  uint32_t cfg_sha256_disabled =
      bitfield_field32_write(0, HMAC_CFG_DIGEST_SIZE_FIELD,
                             HMAC_CFG_DIGEST_SIZE_VALUE_SHA2_256) |
      bitfield_field32_write(0, HMAC_CFG_KEY_LENGTH_FIELD,
                             HMAC_CFG_KEY_LENGTH_VALUE_KEY_256);
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg_sha256_disabled);
  abs_mmio_write32(kHmacBase + HMAC_WIPE_SECRET_REG_OFFSET, 0x11223344u);
  uint32_t d0 = abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET);
  EXPECT_RTL(d0 == 0x11223344u,
             "After WIPE_SECRET=0x11223344 with digest_swap=0, DIGEST_0 expected "
             "0x11223344, got 0x%08x",
             d0);

  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET,
                   cfg_sha256_disabled | (1u << HMAC_CFG_DIGEST_SWAP_BIT));
  d0 = abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET);
  EXPECT_RTL(d0 == 0x44332211u,
             "After WIPE_SECRET=0x11223344 with digest_swap=1, DIGEST_0 expected "
             "0x44332211, got 0x%08x",
             d0);

  // 3. CFG.sha_en falling edge (1 -> 0) clears DIGEST_0..15 to 0x00000000 (not WIPE_SECRET).
  // In RTL (prim_sha2.sv:396 & 159-160), `clear_digest = hash_start_i | (~sha_en_i & sha_en_q)`
  // zeroes `digest_q` to '0 on the 1 -> 0 transition of sha_en.
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET,
                   cfg_sha256_disabled | (1u << HMAC_CFG_SHA_EN_BIT));
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg_sha256_disabled);
  d0 = abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET);
  EXPECT_RTL(d0 == 0x00000000u,
             "After sha_en 1->0 transition, DIGEST_0 expected 0x00000000 (not "
             "WIPE_SECRET), got 0x%08x",
             d0);

  // 4. Context restore (DIGEST_0..7 writes) preserved across CFG (sha_en 0->0 and 0->1)
  // writes, and ignored when CFG.sha_en == 1 or CFG.digest_size == SHA2_None.
  abs_mmio_write32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET, 0xaabbccddu);
  // Writing CFG with sha_en=0 (0 -> 0 transition) must NOT clear DIGEST_0.
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg_sha256_disabled);
  d0 = abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET);
  EXPECT_RTL(d0 == 0xaabbccddu,
             "Writing CFG with sha_en 0->0 must preserve restored DIGEST_0 "
             "(expected 0xaabbccdd, got 0x%08x)",
             d0);

  // Enabling sha_en (0 -> 1) preserves DIGEST_0, and SW writes to DIGEST_0 while
  // sha_en == 1 must be ignored (prim_sha2.sv:161 `else if (!sha_en_i)`).
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET,
                   cfg_sha256_disabled | (1u << HMAC_CFG_SHA_EN_BIT));
  abs_mmio_write32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET, 0x55667788u);
  d0 = abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET);
  EXPECT_RTL(d0 == 0xaabbccddu,
             "SW write to DIGEST_0 while CFG.sha_en==1 must be ignored "
             "(expected 0xaabbccdd, got 0x%08x)",
             d0);

  // Disabling sha_en (1 -> 0) with digest_size = SHA2_None clears DIGEST_0 to 0,
  // and SW writes to DIGEST_0 while digest_size == SHA2_None must be ignored
  // (hmac.sv:240-253).
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, 0u);
  abs_mmio_write32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET, 0xdeadbeefu);
  d0 = abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET);
  EXPECT_RTL(d0 == 0x00000000u,
             "SW write to DIGEST_0 while CFG.digest_size==SHA2_None must be "
             "ignored (expected 0x00000000, got 0x%08x)",
             d0);

  // 5. SHA2_256 computation & DIGEST_0..7 replication into DIGEST_8..15.
  // In RTL (hmac.sv:256-261), when digest_size_started_q == SHA2_256,
  // hw2reg.digest[i+8].d = hw2reg.digest[i].d for i = 0..7.
  uint32_t cfg_sha256_active =
      cfg_sha256_disabled | (1u << HMAC_CFG_SHA_EN_BIT) |
      (1u << HMAC_CFG_DIGEST_SWAP_BIT);
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg_sha256_active);
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_START_BIT);
  abs_mmio_write8(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET, 'a');
  abs_mmio_write8(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET, 'b');
  abs_mmio_write8(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET, 'c');
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_PROCESS_BIT);

  while ((abs_mmio_read32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET) &
          (1u << HMAC_INTR_STATE_HMAC_DONE_BIT)) == 0u) {
  }
  abs_mmio_write32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET,
                   1u << HMAC_INTR_STATE_HMAC_DONE_BIT);

  d0 = abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET);
  uint32_t d8 = abs_mmio_read32(kHmacBase + HMAC_DIGEST_8_REG_OFFSET);
  EXPECT_RTL(d0 == 0xbf1678bau,
             "SHA2_256('abc') DIGEST_0 expected 0xbf1678ba, got 0x%08x", d0);
  EXPECT_RTL(d8 == d0,
             "SHA2_256('abc') DIGEST_8 should replicate DIGEST_0 (expected "
             "0x%08x, got 0x%08x)",
             d0, d8);

  // 6. ERR_CODE latch-until-cleared (`err_valid = ~reg2hw.intr_state.hmac_err.q & ...`).
  // In RTL (hmac.sv:831), once INTR_STATE.hmac_err == 1, subsequent errors do NOT
  // overwrite ERR_CODE until INTR_STATE.hmac_err is cleared (W1C).
  abs_mmio_write32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET, 0x7u);
  // Push message while idle (`!msg_allowed`) -> SwPushMsgWhenDisallowed (0x5).
  abs_mmio_write32(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET, 0x12345678u);
  uint32_t err_code = abs_mmio_read32(kHmacBase + HMAC_ERR_CODE_REG_OFFSET);
  EXPECT_RTL(err_code == kHmacErrPushMsgWhenDisallowed,
             "First error expected SwPushMsgWhenDisallowed (0x5), got 0x%08x",
             err_code);

  // Now trigger SwHashStartWhenShaDisabled (0x2) WITHOUT clearing INTR_STATE.hmac_err.
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg_sha256_disabled);
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_START_BIT);
  err_code = abs_mmio_read32(kHmacBase + HMAC_ERR_CODE_REG_OFFSET);
  EXPECT_RTL(err_code == kHmacErrPushMsgWhenDisallowed,
             "ERR_CODE must remain latched at 0x5 while INTR_STATE.hmac_err==1, "
             "got 0x%08x",
             err_code);

  // Clear INTR_STATE.hmac_err (W1C) and trigger SwHashStartWhenShaDisabled (0x2) again.
  abs_mmio_write32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET,
                   1u << HMAC_INTR_STATE_HMAC_ERR_BIT);
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_START_BIT);
  err_code = abs_mmio_read32(kHmacBase + HMAC_ERR_CODE_REG_OFFSET);
  EXPECT_RTL(err_code == kHmacErrHashStartWhenShaDisabled,
             "After clearing INTR_STATE.hmac_err, ERR_CODE expected "
             "SwHashStartWhenShaDisabled (0x2), got 0x%08x",
             err_code);
  abs_mmio_write32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET,
                   1u << HMAC_INTR_STATE_HMAC_ERR_BIT);

  // 7. Writing CMD.hash_stop when idle (including with CFG.digest_size = SHA2_None)
  // must not crash or clear STATUS.hmac_idle.
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, 0u);
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_STOP_BIT);
  uint32_t status = abs_mmio_read32(kHmacBase + HMAC_STATUS_REG_OFFSET);
  EXPECT_RTL((status & (1u << HMAC_STATUS_HMAC_IDLE_BIT)) != 0u,
             "STATUS.hmac_idle must remain 1 after CMD.hash_stop while idle "
             "(got 0x%08x)",
             status);

  return all_ok;
}
