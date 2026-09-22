// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/dif/dif_hmac.h"
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
  kHmacErrPushMsgWhenShaDisabled = 0x1u,
  kHmacErrHashStartWhenShaDisabled = 0x2u,
  kHmacErrUpdateSecretKeyInProcess = 0x3u,
  kHmacErrHashStartWhenActive = 0x4u,
  kHmacErrPushMsgWhenDisallowed = 0x5u,
  kHmacErrInvalidConfig = 0x6u,
};

static volatile bool load_access_fault_seen = false;
static volatile bool store_access_fault_seen = false;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  uint32_t mcause = ibex_mcause_read();
  ibex_exc_t exc = (ibex_exc_t)(mcause & kIbexExcMax);
  if (exc == kIbexExcLoadAccessFault) {
    load_access_fault_seen = true;
  } else if (exc == kIbexExcStoreAccessFault) {
    store_access_fault_seen = true;
  } else {
    ottf_generic_fault_print(exc_info, "Unexpected Load/Store Fault", mcause);
    abort();
  }
}

#define EXPECT_RTL(cond, ...)                  \
  do {                                         \
    if (!(cond)) {                             \
      LOG_ERROR("RTL_MISMATCH: " __VA_ARGS__); \
      all_ok = false;                          \
    }                                          \
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
  // Key_None (0x20 -> 0x4000), producing 0x4100. Reserved bits [31:15] read as
  // 0.
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, 0u);
  uint32_t cfg = abs_mmio_read32(kHmacBase + HMAC_CFG_REG_OFFSET);
  EXPECT_RTL(
      cfg == 0x4100u,
      "CFG(0) expected normalized 0x4100 (SHA2_None|Key_None), got 0x%08x",
      cfg);

  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, 0xffff8000u);
  cfg = abs_mmio_read32(kHmacBase + HMAC_CFG_REG_OFFSET);
  EXPECT_RTL(cfg == 0x4100u,
             "CFG(0xffff8000) reserved bits should be masked out, got 0x%08x",
             cfg);

  // 2. WIPE_SECRET readback endianness on DIGEST_0..15.
  // In RTL (prim_sha2.sv:148 & hmac.sv:270), writing WIPE_SECRET = V sets each
  // 32-bit digest word to V, and reading DIGEST_k returns conv_endian32(V,
  // digest_swap):
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
  EXPECT_RTL(
      d0 == 0x11223344u,
      "After WIPE_SECRET=0x11223344 with digest_swap=0, DIGEST_0 expected "
      "0x11223344, got 0x%08x",
      d0);

  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET,
                   cfg_sha256_disabled | (1u << HMAC_CFG_DIGEST_SWAP_BIT));
  d0 = abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET);
  EXPECT_RTL(
      d0 == 0x44332211u,
      "After WIPE_SECRET=0x11223344 with digest_swap=1, DIGEST_0 expected "
      "0x44332211, got 0x%08x",
      d0);

  // 3. CFG.sha_en falling edge (1 -> 0) clears DIGEST_0..15 to 0x00000000 (not
  // WIPE_SECRET). In RTL (prim_sha2.sv:396 & 159-160), `clear_digest =
  // hash_start_i | (~sha_en_i & sha_en_q)` zeroes `digest_q` to '0 on the 1 ->
  // 0 transition of sha_en.
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET,
                   cfg_sha256_disabled | (1u << HMAC_CFG_SHA_EN_BIT));
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg_sha256_disabled);
  d0 = abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET);
  EXPECT_RTL(d0 == 0x00000000u,
             "After sha_en 1->0 transition, DIGEST_0 expected 0x00000000 (not "
             "WIPE_SECRET), got 0x%08x",
             d0);

  // 4. Context restore (DIGEST_0..7 writes) preserved across CFG (sha_en 0->0
  // and 0->1) writes, and ignored when CFG.sha_en == 1 or CFG.digest_size ==
  // SHA2_None.
  abs_mmio_write32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET, 0xaabbccddu);
  // Writing CFG with sha_en=0 (0 -> 0 transition) must NOT clear DIGEST_0.
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg_sha256_disabled);
  d0 = abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET);
  EXPECT_RTL(d0 == 0xaabbccddu,
             "Writing CFG with sha_en 0->0 must preserve restored DIGEST_0 "
             "(expected 0xaabbccdd, got 0x%08x)",
             d0);

  // Enabling sha_en (0 -> 1) preserves DIGEST_0, and SW writes to DIGEST_0
  // while sha_en == 1 must be ignored (prim_sha2.sv:161 `else if (!sha_en_i)`).
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET,
                   cfg_sha256_disabled | (1u << HMAC_CFG_SHA_EN_BIT));
  abs_mmio_write32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET, 0x55667788u);
  d0 = abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET);
  EXPECT_RTL(d0 == 0xaabbccddu,
             "SW write to DIGEST_0 while CFG.sha_en==1 must be ignored "
             "(expected 0xaabbccdd, got 0x%08x)",
             d0);

  // Disabling sha_en (1 -> 0) with digest_size = SHA2_None clears DIGEST_0 to
  // 0, and SW writes to DIGEST_0 while digest_size == SHA2_None must be ignored
  // (hmac.sv:240-253).
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, 0u);
  abs_mmio_write32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET, 0xdeadbeefu);
  d0 = abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET);
  EXPECT_RTL(d0 == 0x00000000u,
             "SW write to DIGEST_0 while CFG.digest_size==SHA2_None must be "
             "ignored (expected 0x00000000, got 0x%08x)",
             d0);

  // 5a. SHA2_384 computation populates all 16 DIGEST registers (`DIGEST_12..15`
  // hold the upper 128 bits `state[6..7]` of the 512-bit SHA-512 state;
  // `prim_sha2.sv:371-394`, `hmac.sv:256-267`).
  uint32_t cfg_sha384_active =
      bitfield_field32_write(0, HMAC_CFG_DIGEST_SIZE_FIELD,
                             HMAC_CFG_DIGEST_SIZE_VALUE_SHA2_384) |
      bitfield_field32_write(0, HMAC_CFG_KEY_LENGTH_FIELD,
                             HMAC_CFG_KEY_LENGTH_VALUE_KEY_NONE) |
      (1u << HMAC_CFG_SHA_EN_BIT) | (1u << HMAC_CFG_DIGEST_SWAP_BIT);
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg_sha384_active);
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
  uint32_t sha384_d0 = abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET);
  uint32_t sha384_d12 = abs_mmio_read32(kHmacBase + HMAC_DIGEST_12_REG_OFFSET);
  uint32_t sha384_d15 = abs_mmio_read32(kHmacBase + HMAC_DIGEST_15_REG_OFFSET);
  EXPECT_RTL(sha384_d0 == 0x3f7500cbu && sha384_d12 != 0u && sha384_d15 != 0u,
             "SHA2_384('abc') must write back full 512-bit SHA-512 state to "
             "DIGEST_0..15 (d0=0x%08x, d12=0x%08x, d15=0x%08x)",
             sha384_d0, sha384_d12, sha384_d15);

  // 5b. SHA2_256 computation & DIGEST_0..7 replication into DIGEST_8..15.
  // In RTL (hmac.sv:256-261), when digest_size_started_q == SHA2_256,
  // hw2reg.digest[i+8].d = hw2reg.digest[i].d for i = 0..7.
  uint32_t cfg_sha256_active = cfg_sha256_disabled |
                               (1u << HMAC_CFG_SHA_EN_BIT) |
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

  // 6. ERR_CODE latch-until-cleared (`err_valid = ~reg2hw.intr_state.hmac_err.q
  // & ...`). In RTL (hmac.sv:831), once INTR_STATE.hmac_err == 1, subsequent
  // errors do NOT overwrite ERR_CODE until INTR_STATE.hmac_err is cleared
  // (W1C).
  abs_mmio_write32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET, 0x7u);
  // Push message while idle (`!msg_allowed`) -> SwPushMsgWhenDisallowed (0x5).
  abs_mmio_write32(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET, 0x12345678u);
  uint32_t err_code = abs_mmio_read32(kHmacBase + HMAC_ERR_CODE_REG_OFFSET);
  EXPECT_RTL(err_code == kHmacErrPushMsgWhenDisallowed,
             "First error expected SwPushMsgWhenDisallowed (0x5), got 0x%08x",
             err_code);

  // Now trigger SwHashStartWhenShaDisabled (0x2) WITHOUT clearing
  // INTR_STATE.hmac_err.
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg_sha256_disabled);
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_START_BIT);
  err_code = abs_mmio_read32(kHmacBase + HMAC_ERR_CODE_REG_OFFSET);
  EXPECT_RTL(
      err_code == kHmacErrPushMsgWhenDisallowed,
      "ERR_CODE must remain latched at 0x5 while INTR_STATE.hmac_err==1, "
      "got 0x%08x",
      err_code);

  // Clear INTR_STATE.hmac_err (W1C) and trigger SwHashStartWhenShaDisabled
  // (0x2) again.
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

  // 7. Writing CMD.hash_stop when idle (including with CFG.digest_size =
  // SHA2_None) must not crash or clear STATUS.hmac_idle.
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, 0u);
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_STOP_BIT);
  uint32_t status = abs_mmio_read32(kHmacBase + HMAC_STATUS_REG_OFFSET);
  EXPECT_RTL((status & (1u << HMAC_STATUS_HMAC_IDLE_BIT)) != 0u,
             "STATUS.hmac_idle must remain 1 after CMD.hash_stop while idle "
             "(got 0x%08x)",
             status);

  // 8. Wave 2:
  // (a) `MSG_LENGTH_LOWER` / `MSG_LENGTH_UPPER` is NOT cleared by `CFG.sha_en`
  // falling edge (1 -> 0); in RTL (hmac.sv:629-650), `message_length` is only
  // cleared on reset or `hash_start`, so after the 3-byte ('abc') hash in
  // step 5 and `sha_en` 1->0 in step 6, `MSG_LENGTH_LOWER` must still be 24.
  uint32_t msg_len_lo =
      abs_mmio_read32(kHmacBase + HMAC_MSG_LENGTH_LOWER_REG_OFFSET);
  EXPECT_RTL(msg_len_lo == 24u,
             "MSG_LENGTH_LOWER must be retained (24 bits) across CFG.sha_en "
             "1->0 transition, got %u (0x%08x)",
             msg_len_lo, msg_len_lo);

  // (b) In RTL (hmac.sv:636-643), SW writes to `MSG_LENGTH_LOWER` and
  // `MSG_LENGTH_UPPER` are gated ONLY by `!cfg_block` (idle), NOT by `!sha_en`.
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET,
                   cfg_sha256_disabled | (1u << HMAC_CFG_SHA_EN_BIT));
  abs_mmio_write32(kHmacBase + HMAC_MSG_LENGTH_LOWER_REG_OFFSET, 0x12345678u);
  abs_mmio_write32(kHmacBase + HMAC_MSG_LENGTH_UPPER_REG_OFFSET, 0x9abcdef0u);
  msg_len_lo = abs_mmio_read32(kHmacBase + HMAC_MSG_LENGTH_LOWER_REG_OFFSET);
  uint32_t msg_len_hi =
      abs_mmio_read32(kHmacBase + HMAC_MSG_LENGTH_UPPER_REG_OFFSET);
  EXPECT_RTL(msg_len_lo == 0x12345678u && msg_len_hi == 0x9abcdef0u,
             "MSG_LENGTH_LOWER/UPPER must be writable while idle even when "
             "CFG.sha_en==1 (got 0x%08x_0x%08x)",
             msg_len_hi, msg_len_lo);

  // (c) In RTL (hmac.sv:240-243), when `CFG.digest_size == SHA2_256` and
  // `CFG.sha_en == 0`, only `reg2hw.digest[0..7].qe` drives
  // `digest_sw_we[0..7]`. Writes to `DIGEST_8..15` are ignored, and
  // `DIGEST_8..15` continue to mirror `DIGEST_0..7`.
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg_sha256_disabled);
  abs_mmio_write32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET, 0x13572468u);
  abs_mmio_write32(kHmacBase + HMAC_DIGEST_8_REG_OFFSET, 0xdeadc0deu);
  d0 = abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET);
  d8 = abs_mmio_read32(kHmacBase + HMAC_DIGEST_8_REG_OFFSET);
  EXPECT_RTL(d0 == 0x13572468u && d8 == 0x13572468u,
             "Write to DIGEST_8 in SHA2_256 mode must be ignored and DIGEST_8 "
             "must still mirror DIGEST_0 (0x13572468), got d0=0x%08x d8=0x%08x",
             d0, d8);

  // 9. Wave 4: Reading from write-only `MSG_FIFO` window (`0x1000..0x1fff`):
  // In RTL (`hmac.sv:588`, `u_tlul_adapter` `.ErrOnRead(1)`), a TL-UL Get
  // asserts `rd_vld_error = 1'b1` (`error_internal = 1'b1`), returning
  // `tl_win_d2h.d_error = 1'b1` (Ibex Load Access Fault `MCAUSE = 5`) and
  // suppressing `req_o` (`msg_fifo_req = 0`), so `INTR_STATE.hmac_err` is NOT
  // asserted and `ERR_CODE` is NOT modified.
  abs_mmio_write32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET, 0x7u);
  uint32_t err_before = abs_mmio_read32(kHmacBase + HMAC_ERR_CODE_REG_OFFSET);
  load_access_fault_seen = false;
  (void)abs_mmio_read32(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET);
  EXPECT_RTL(load_access_fault_seen,
             "Reading MSG_FIFO must raise a synchronous Load Access Fault "
             "(ErrOnRead=1 in u_tlul_adapter)");
  uint32_t intr_after = abs_mmio_read32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET);
  uint32_t err_after = abs_mmio_read32(kHmacBase + HMAC_ERR_CODE_REG_OFFSET);
  EXPECT_RTL(
      (intr_after & (1u << HMAC_INTR_STATE_HMAC_ERR_BIT)) == 0u,
      "Reading MSG_FIFO must NOT assert INTR_STATE.hmac_err (got 0x%08x)",
      intr_after);
  EXPECT_RTL(err_after == err_before,
             "Reading MSG_FIFO must NOT modify ERR_CODE (expected 0x%08x, got "
             "0x%08x)",
             err_before, err_after);

  // 10. Wave 5: `HMAC_PERMIT` sub-word read/write check
  // (`hmac_reg_pkg.sv:441-501`, `hmac_reg_top.sv:2007-2070`) and `addrmiss`
  // decode error (`0x0f0..0xfff`): (a) Sub-word reads (`lbu`/`lhu`) to valid
  // CSRs (`CFG`) succeed (`d_error = 0`).
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg_sha256_disabled);
  load_access_fault_seen = false;
  uint16_t cfg_lo16 = *(volatile uint16_t *)(kHmacBase + HMAC_CFG_REG_OFFSET);
  uint8_t cfg_b1 = abs_mmio_read8(kHmacBase + HMAC_CFG_REG_OFFSET + 1u);
  EXPECT_RTL(!load_access_fault_seen &&
                 cfg_lo16 == (uint16_t)cfg_sha256_disabled &&
                 cfg_b1 == (uint8_t)(cfg_sha256_disabled >> 8),
             "Sub-word reads (lhu/lbu) of CFG must succeed without Load Access "
             "Fault (fault=%d, lo16=0x%04x, b1=0x%02x)",
             load_access_fault_seen, cfg_lo16, cfg_b1);

  // (b) Permitted sub-word writes (`HMAC_PERMIT[INTR_ENABLE] = 4'b0001` via
  // `sb`, and `HMAC_PERMIT[CFG] = 4'b0011` via `sh`) succeed (`wr_err = 0`).
  store_access_fault_seen = false;
  abs_mmio_write8(kHmacBase + HMAC_INTR_ENABLE_REG_OFFSET, 0x5u);
  uint32_t ie_val = abs_mmio_read32(kHmacBase + HMAC_INTR_ENABLE_REG_OFFSET);
  abs_mmio_write8(kHmacBase + HMAC_INTR_ENABLE_REG_OFFSET, 0x0u);
  *(volatile uint16_t *)(kHmacBase + HMAC_CFG_REG_OFFSET) = 0u;
  uint32_t cfg_after_sh = abs_mmio_read32(kHmacBase + HMAC_CFG_REG_OFFSET);
  EXPECT_RTL(
      !store_access_fault_seen && ie_val == 0x5u && cfg_after_sh == 0x4100u,
      "Permitted sub-word writes (sb to INTR_ENABLE, sh to CFG) must "
      "succeed (fault=%d, ie=0x%x, cfg=0x%04x)",
      store_access_fault_seen, ie_val, cfg_after_sh);

  // (c) Prohibited sub-word writes (`sb` to `CFG` [`4'b0011`], `sh` to
  // `MSG_LENGTH_LOWER` [`4'b1111`]) assert `wr_err = 1` (Store Access Fault
  // `MCAUSE = 7`) and suppress the register write.
  abs_mmio_write32(kHmacBase + HMAC_MSG_LENGTH_LOWER_REG_OFFSET, 0x12345678u);
  store_access_fault_seen = false;
  abs_mmio_write8(kHmacBase + HMAC_CFG_REG_OFFSET, 0x02u);
  EXPECT_RTL(store_access_fault_seen &&
                 abs_mmio_read32(kHmacBase + HMAC_CFG_REG_OFFSET) == 0x4100u,
             "8-bit write (sb) to CFG (HMAC_PERMIT=4'b0011) must raise Store "
             "Access Fault and leave CFG unchanged");
  store_access_fault_seen = false;
  *(volatile uint16_t *)(kHmacBase + HMAC_MSG_LENGTH_LOWER_REG_OFFSET) =
      0xaaaau;
  EXPECT_RTL(
      store_access_fault_seen &&
          abs_mmio_read32(kHmacBase + HMAC_MSG_LENGTH_LOWER_REG_OFFSET) ==
              0x12345678u,
      "16-bit write (sh) to MSG_LENGTH_LOWER (HMAC_PERMIT=4'b1111) must "
      "raise Store Access Fault and leave MSG_LENGTH_LOWER unchanged");

  // (d) Unmapped CSR offset (`addrmiss` at `0x0f0`): raises Load/Store Access
  // Fault.
  load_access_fault_seen = false;
  (void)abs_mmio_read32(kHmacBase + 0x0f0u);
  store_access_fault_seen = false;
  abs_mmio_write32(kHmacBase + 0x0f0u, 0x1u);
  EXPECT_RTL(
      load_access_fault_seen && store_access_fault_seen,
      "Unmapped CSR offset 0x0f0 (addrmiss) must raise Load/Store Access "
      "Fault (load_fault=%d, store_fault=%d)",
      load_access_fault_seen, store_access_fault_seen);

  // 11. `INTR_TEST` Status (`fifo_empty`) vs Event (`hmac_done`) semantics
  // (`hmac.sv:486-512`, `prim_intr_hw.sv:60-89`):
  abs_mmio_write32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET, 0x7u);
  abs_mmio_write32(kHmacBase + HMAC_INTR_TEST_REG_OFFSET,
                   (1u << HMAC_INTR_STATE_FIFO_EMPTY_BIT) |
                       (1u << HMAC_INTR_STATE_HMAC_DONE_BIT));
  uint32_t istate = abs_mmio_read32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET);
  EXPECT_RTL(
      istate == ((1u << HMAC_INTR_STATE_FIFO_EMPTY_BIT) |
                 (1u << HMAC_INTR_STATE_HMAC_DONE_BIT)),
      "INTR_TEST=0x3 must assert both fifo_empty and hmac_done (got 0x%x)",
      istate);
  // W1C on INTR_STATE clears Event (hmac_done) but NOT Status (fifo_empty)
  // while test_q=1
  abs_mmio_write32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET,
                   (1u << HMAC_INTR_STATE_FIFO_EMPTY_BIT) |
                       (1u << HMAC_INTR_STATE_HMAC_DONE_BIT));
  istate = abs_mmio_read32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET);
  EXPECT_RTL(
      istate == (1u << HMAC_INTR_STATE_FIFO_EMPTY_BIT),
      "W1C to INTR_STATE must clear hmac_done (Event) but NOT fifo_empty "
      "(Status, RO while test_q=1); got 0x%x",
      istate);
  // Writing INTR_TEST=0 deasserts test_q for Status interrupt fifo_empty
  abs_mmio_write32(kHmacBase + HMAC_INTR_TEST_REG_OFFSET, 0u);
  istate = abs_mmio_read32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET);
  EXPECT_RTL(istate == 0u,
             "INTR_TEST=0 must deassert Status interrupt fifo_empty (got 0x%x)",
             istate);

  return all_ok;
}
