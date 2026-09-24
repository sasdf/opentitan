// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top/kmac_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

#define CHECK_EQ(a, b, ...) CHECK((a) == (b), __VA_ARGS__)

OTTF_DEFINE_TEST_CONFIG(.ignore_alerts = true);

enum {
  kKmacBase = TOP_EARLGREY_KMAC_BASE_ADDR,
  kIbexBase = TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR,
  kIbexErrStatusRegOffset = 0x10u,
};

static volatile bool load_store_fault_seen = false;
static volatile uint32_t last_mcause = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  load_store_fault_seen = true;
  last_mcause = ibex_mcause_read();
  abs_mmio_write32(kIbexBase + kIbexErrStatusRegOffset, UINT32_MAX);
}

void ottf_internal_isr(uint32_t *exc_info) {
  (void)exc_info;
  load_store_fault_seen = true;
  last_mcause = ibex_mcause_read();
  abs_mmio_write32(kIbexBase + kIbexErrStatusRegOffset, UINT32_MAX);
}

static void wait_kmac_idle(void) {
  while ((abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET) &
          (1u << KMAC_STATUS_SHA3_IDLE_BIT)) == 0u) {
  }
}

static void wait_kmac_absorb(void) {
  while ((abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET) &
          (1u << KMAC_STATUS_SHA3_ABSORB_BIT)) == 0u) {
  }
}

static void wait_kmac_squeeze(void) {
  while ((abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET) &
          (1u << KMAC_STATUS_SHA3_SQUEEZE_BIT)) == 0u) {
  }
}

static void write_cfg_shadowed(uint32_t val) {
  abs_mmio_write32_shadowed(kKmacBase + KMAC_CFG_SHADOWED_REG_OFFSET, val);
}

static uint32_t read_unmasked_state_word(uint32_t word_idx) {
  uint32_t share0 =
      abs_mmio_read32(kKmacBase + KMAC_STATE_REG_OFFSET + word_idx * 4u);
  uint32_t share1 = abs_mmio_read32(kKmacBase + KMAC_STATE_REG_OFFSET + 0x100u +
                                    word_idx * 4u);
  return share0 ^ share1;
}

bool test_main(void) {
  wait_kmac_idle();

  // =========================================================================
  // 1. [kmac_errchk.sv:268] (CONFIRMED_PRESENT_ON_V2):
  //    CFG_SHADOWED.KMAC_EN is evaluated independently of CFG_SHADOWED.MODE
  //    across kmac_errchk.sv, kmac_core.sv, and sha3pad.sv:
  //    - Subcase 1A (kmac_en=1, mode=Sha3 (0), kstrength=L256, PREFIX="KMAC"):
  //      Raises ZERO error (kmac_err=0, ERR_CODE=0x00000000), skips cSHAKE
  //      StPrefix ("KMAC"), absorbs bytepad(encode_string(K), 136) + M, and
  //      pads with SHA-3 suffix 0x06 instead of cSHAKE suffix 0x04 — matching
  //      unkeyed SHA3-256 fed manual bytepad(encode_string(K), 136) + M.
  //    - Subcase 1B (kmac_en=1, mode=Shake (2), kstrength=L256, PREFIX="KMAC"):
  //      Also raises ZERO error (kmac_err=0, ERR_CODE=0x00000000), producing
  //      SHAKE-256(bytepad(encode_string(K), 136) || M).
  //    - Subcase 1C (kmac_en=1, mode=Sha3 (0), kstrength=L256, PREFIX=0):
  //      Raises ErrIncorrectFunctionName (0x07010000) even though mode=Sha3.
  // =========================================================================
  LOG_INFO(
      "Verifying [kmac_errchk.sv:268] (CONFIRMED_PRESENT_ON_V2): "
      "kmac_en=1 with mode=Sha3/Shake...");

  CHECK_EQ(abs_mmio_read32(kKmacBase + KMAC_ERR_CODE_REG_OFFSET), 0x00000000u,
           "Expected initial ERR_CODE == 0x00000000");

  abs_mmio_write32(kKmacBase + KMAC_PREFIX_0_REG_OFFSET, 0x4d4b2001u);
  abs_mmio_write32(kKmacBase + KMAC_PREFIX_1_REG_OFFSET, 0x00014341u);
  for (uint32_t i = 2; i < 11u; ++i) {
    abs_mmio_write32(kKmacBase + KMAC_PREFIX_0_REG_OFFSET + i * 4u, 0u);
  }
  abs_mmio_write32(kKmacBase + KMAC_KEY_LEN_REG_OFFSET,
                   KMAC_KEY_LEN_LEN_VALUE_KEY128);
  for (uint32_t i = 0; i < 16u; ++i) {
    abs_mmio_write32(kKmacBase + KMAC_KEY_SHARE0_0_REG_OFFSET + i * 4u,
                     0x55555555u);
    abs_mmio_write32(kKmacBase + KMAC_KEY_SHARE1_0_REG_OFFSET + i * 4u, 0u);
  }
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, UINT32_MAX);

  for (uint32_t i = 0; i < 6u; ++i) {
    abs_mmio_write32(kKmacBase + KMAC_ENTROPY_SEED_REG_OFFSET,
                     0x11111111u * (i + 1u));
  }
  uint32_t cfg_3a = 0;
  cfg_3a = bitfield_bit32_write(cfg_3a, KMAC_CFG_SHADOWED_KMAC_EN_BIT, true);
  cfg_3a = bitfield_field32_write(cfg_3a, KMAC_CFG_SHADOWED_KSTRENGTH_FIELD,
                                  KMAC_CFG_SHADOWED_KSTRENGTH_VALUE_L256);
  cfg_3a = bitfield_field32_write(cfg_3a, KMAC_CFG_SHADOWED_MODE_FIELD,
                                  KMAC_CFG_SHADOWED_MODE_VALUE_SHA3);
  cfg_3a = bitfield_field32_write(cfg_3a, KMAC_CFG_SHADOWED_ENTROPY_MODE_FIELD,
                                  KMAC_CFG_SHADOWED_ENTROPY_MODE_VALUE_SW_MODE);
  cfg_3a =
      bitfield_bit32_write(cfg_3a, KMAC_CFG_SHADOWED_ENTROPY_READY_BIT, true);
  write_cfg_shadowed(cfg_3a);

  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_START << KMAC_CMD_CMD_OFFSET);
  wait_kmac_absorb();
  abs_mmio_write32(kKmacBase + KMAC_MSG_FIFO_REG_OFFSET, 0x44332211u);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_PROCESS << KMAC_CMD_CMD_OFFSET);
  wait_kmac_squeeze();

  uint32_t intr_3a = abs_mmio_read32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET);
  uint32_t err_3a = abs_mmio_read32(kKmacBase + KMAC_ERR_CODE_REG_OFFSET);
  uint32_t d_kmac_sha3 = read_unmasked_state_word(0);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_DONE << KMAC_CMD_CMD_OFFSET);
  wait_kmac_idle();
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, UINT32_MAX);

  CHECK_EQ(intr_3a & (1u << KMAC_INTR_STATE_KMAC_ERR_BIT), 0u,
           "Expected kmac_err == 0 when kmac_en=1, mode=Sha3, PREFIX='KMAC'");
  CHECK_EQ(err_3a, 0x00000000u,
           "Expected ERR_CODE == 0x00000000 when kmac_en=1, mode=Sha3");

  // Compute manual unkeyed SHA3-256(bytepad(encode_string(K), 136) || M).
  uint32_t cfg_plain_sha3 =
      bitfield_bit32_write(cfg_3a, KMAC_CFG_SHADOWED_KMAC_EN_BIT, false);
  write_cfg_shadowed(cfg_plain_sha3);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_START << KMAC_CMD_CMD_OFFSET);
  wait_kmac_absorb();
  abs_mmio_write32(kKmacBase + KMAC_MSG_FIFO_REG_OFFSET, 0x80018801u);
  for (uint32_t i = 0; i < 4u; ++i) {
    abs_mmio_write32(kKmacBase + KMAC_MSG_FIFO_REG_OFFSET, 0x55555555u);
  }
  for (uint32_t i = 0; i < 29u; ++i) {
    abs_mmio_write32(kKmacBase + KMAC_MSG_FIFO_REG_OFFSET, 0x00000000u);
  }
  abs_mmio_write32(kKmacBase + KMAC_MSG_FIFO_REG_OFFSET, 0x44332211u);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_PROCESS << KMAC_CMD_CMD_OFFSET);
  wait_kmac_squeeze();
  uint32_t d_manual_hybrid = read_unmasked_state_word(0);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_DONE << KMAC_CMD_CMD_OFFSET);
  wait_kmac_idle();
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, UINT32_MAX);

  LOG_INFO("[kmac_errchk.sv:268] d_kmac_sha3=0x%08x, d_manual_hybrid=0x%08x",
           d_kmac_sha3, d_manual_hybrid);
  CHECK_EQ(d_kmac_sha3, d_manual_hybrid,
           "Expected d_kmac_sha3 to match SHA3-256(bytepad(K, 136) || M)");

  // Subcase 1B: kmac_en=1, mode=Shake (2), kstrength=L256 (2), PREFIX='KMAC'
  // -> ERR_CODE == 0x00000000, kmac_err == 0 (computes non-standard
  // SHAKE-256(bytepad(encode_string(K), 136) || M || right_encode(0))).
  uint32_t cfg_3b = bitfield_field32_write(cfg_3a, KMAC_CFG_SHADOWED_MODE_FIELD,
                                           KMAC_CFG_SHADOWED_MODE_VALUE_SHAKE);
  write_cfg_shadowed(cfg_3b);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_START << KMAC_CMD_CMD_OFFSET);
  wait_kmac_absorb();
  abs_mmio_write32(kKmacBase + KMAC_MSG_FIFO_REG_OFFSET, 0x44332211u);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_PROCESS << KMAC_CMD_CMD_OFFSET);
  wait_kmac_squeeze();
  uint32_t intr_3b = abs_mmio_read32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET);
  uint32_t err_3b = abs_mmio_read32(kKmacBase + KMAC_ERR_CODE_REG_OFFSET);
  uint32_t d_kmac_shake = read_unmasked_state_word(0);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_DONE << KMAC_CMD_CMD_OFFSET);
  wait_kmac_idle();
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, UINT32_MAX);
  LOG_INFO("[kmac_errchk.sv:268 1B] d_kmac_shake=0x%08x, err_3b=0x%08x",
           d_kmac_shake, err_3b);
  CHECK_EQ(intr_3b & (1u << KMAC_INTR_STATE_KMAC_ERR_BIT), 0u,
           "Expected kmac_err == 0 when kmac_en=1, mode=Shake, PREFIX='KMAC'");
  CHECK_EQ(err_3b, 0x00000000u,
           "Expected ERR_CODE == 0x00000000 when kmac_en=1, mode=Shake");

  // Subcase 1C: kmac_en=1, mode=Sha3 (0), PREFIX=0 -> 0x07010000.
  abs_mmio_write32(kKmacBase + KMAC_PREFIX_0_REG_OFFSET, 0u);
  write_cfg_shadowed(cfg_3a);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_START << KMAC_CMD_CMD_OFFSET);
  uint32_t err_code_3c = abs_mmio_read32(kKmacBase + KMAC_ERR_CODE_REG_OFFSET);
  LOG_INFO("[kmac_errchk.sv:268 1C] ERR_CODE=0x%08x when PREFIX=0, mode=Sha3",
           err_code_3c);
  CHECK_EQ(err_code_3c, 0x07010000u,
           "Expected ErrIncorrectFunctionName (0x07010000) in Sha3 mode");
  wait_kmac_absorb();
  abs_mmio_write32(kKmacBase + KMAC_MSG_FIFO_REG_OFFSET, 0x44332211u);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_PROCESS << KMAC_CMD_CMD_OFFSET);
  wait_kmac_squeeze();
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_DONE << KMAC_CMD_CMD_OFFSET);
  wait_kmac_idle();
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   1u << KMAC_CMD_ERR_PROCESSED_BIT);
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, UINT32_MAX);

  // =========================================================================
  // 2. [kmac_app.sv:1049 & kmac_pkg.sv:382] (MODIFIED_IN_V2):
  //    SwPushedMsgFifo (0x02) in StIdle packs 8'(st) and 8'(mux_sel), where
  //    10-bit sparse StIdle on trunk-v2 is 10'b0110100101 (0x1a5), truncated
  //    to 8'(StIdle) = 0xa5, and SelNone = 5'b10100 (0x14) -> 0x0200a514
  //    (vs 0x0200be14 on v1).
  // =========================================================================
  LOG_INFO(
      "Verifying [kmac_app.sv:1049 & kmac_pkg.sv:382] SwPushedMsgFifo in "
      "StIdle on trunk-v2...");
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, UINT32_MAX);
  abs_mmio_write32(kKmacBase + KMAC_MSG_FIFO_REG_OFFSET, 0x11223344u);
  uint32_t err_code_idle =
      abs_mmio_read32(kKmacBase + KMAC_ERR_CODE_REG_OFFSET);
  LOG_INFO("[kmac_app.sv:1049] SwPushedMsgFifo ERR_CODE=0x%08x", err_code_idle);
  CHECK_EQ(err_code_idle, 0x0200a514u,
           "Expected ERR_CODE 0x0200a514 in StIdle on trunk-v2");
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   1u << KMAC_CMD_ERR_PROCESSED_BIT);
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, UINT32_MAX);

  // =========================================================================
  // 3. [kmac_errchk.sv:236-246,321-376,400] (CONFIRMED_PRESENT_ON_V2):
  //    kmac_errchk blocks CmdStart on UnexpectedModeStrength (0x06020012) when
  //    EN_UNSUPPORTED_MODESTRENGTH = 0, and packs 6-bit sparse cmd (0x2e) in
  //    ERR_CODE[5:0] (0x0804002e).
  // =========================================================================
  uint32_t cfg_bad_mode = 0;
  cfg_bad_mode =
      bitfield_field32_write(cfg_bad_mode, KMAC_CFG_SHADOWED_MODE_FIELD, 1u);
  cfg_bad_mode =
      bitfield_field32_write(cfg_bad_mode, KMAC_CFG_SHADOWED_KSTRENGTH_FIELD,
                             KMAC_CFG_SHADOWED_KSTRENGTH_VALUE_L256);
  write_cfg_shadowed(cfg_bad_mode);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_START << KMAC_CMD_CMD_OFFSET);
  uint32_t err_code_002a =
      abs_mmio_read32(kKmacBase + KMAC_ERR_CODE_REG_OFFSET);
  uint32_t status_002a = abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET);
  CHECK_EQ(err_code_002a, 0x06020012u,
           "Expected ERR_CODE 0x06020012 for MODE=1");
  CHECK((status_002a & (1u << KMAC_STATUS_SHA3_IDLE_BIT)) != 0u &&
            (status_002a & (1u << KMAC_STATUS_SHA3_ABSORB_BIT)) == 0u,
        "Expected CmdStart to be blocked (SHA3_IDLE=1)");
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   1u << KMAC_CMD_ERR_PROCESSED_BIT);
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, UINT32_MAX);

  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_PROCESS << KMAC_CMD_CMD_OFFSET);
  uint32_t err_code_002b =
      abs_mmio_read32(kKmacBase + KMAC_ERR_CODE_REG_OFFSET);
  CHECK_EQ(err_code_002b, 0x0804002eu,
           "Expected 6-bit sparse CmdProcess (0x2e) in ERR_CODE 0x0804002e");
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   1u << KMAC_CMD_ERR_PROCESSED_BIT);
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, UINT32_MAX);

  // =========================================================================
  // 4. [kmac.sv:691 & kmac_app.sv:665-676] (CONFIRMED_PRESENT_ON_V2):
  //    ERR_CODE remains sticky across CMD.err_processed = 1, and
  //    CMD.err_processed = 1 does NOT abort active SW hashing in StSw.
  // =========================================================================
  write_cfg_shadowed(cfg_plain_sha3);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_START << KMAC_CMD_CMD_OFFSET);
  wait_kmac_absorb();
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_RUN << KMAC_CMD_CMD_OFFSET);
  CHECK_EQ(abs_mmio_read32(kKmacBase + KMAC_ERR_CODE_REG_OFFSET), 0x08040131u,
           "Expected ERR_CODE 0x08040131 on CMD.RUN in StAbsorb");
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   1u << KMAC_CMD_ERR_PROCESSED_BIT);
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, UINT32_MAX);
  CHECK_EQ(abs_mmio_read32(kKmacBase + KMAC_ERR_CODE_REG_OFFSET), 0x08040131u,
           "Expected ERR_CODE to remain sticky across CMD.err_processed = 1");
  uint32_t status_005b = abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET);
  CHECK((status_005b & (1u << KMAC_STATUS_SHA3_ABSORB_BIT)) != 0u,
        "Expected SHA3_ABSORB=1 to remain active after CMD.err_processed = 1");
  abs_mmio_write32(kKmacBase + KMAC_MSG_FIFO_REG_OFFSET, 0x64636261u);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_PROCESS << KMAC_CMD_CMD_OFFSET);
  wait_kmac_squeeze();
  uint32_t sha3_kmac0_w0 = read_unmasked_state_word(0);
  CHECK(sha3_kmac0_w0 != 0u, "Expected non-zero SHA3-256 digest word 0");
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_DONE << KMAC_CMD_CMD_OFFSET);
  wait_kmac_idle();
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, UINT32_MAX);

  // =========================================================================
  // 5. [kmac_app.sv:755-787] (FIXED_IN_V2_RTL & NEW_IN_V2):
  //    - FIXED_IN_V2_RTL: On ErrKeyNotValid (0x01000000) with sideload=1,
  //      writing CMD.err_processed = 1 causes StErrorAwaitAbsorbed to issue
  //      cmd_o = CmdDone and transition st_d = StIdle (mux_sel = SelNone),
  //      returning STATUS directly to sha3_idle = 1 (0x4001) and zeroing
  //      STATE (0x400..0x5FF == 0x00000000), fixing v1's dummy digest leak!
  //    - NEW_IN_V2: Simultaneously, StErrorAwaitAbsorbed (kmac_app.sv:783-785)
  //      pulses absorbed_o = MuBi4True (asserting INTR_STATE.kmac_done = 1)
  //      on the exact same clock edge that cmd_o = CmdDone returns KMAC to
  //      sha3_idle = 1. Consequently, INTR_STATE.kmac_done is set while KMAC
  //      is already in StIdle (sha3_squeeze = 0) and STATE is zeroed, and
  //      issuing CMD.done in response to kmac_done raises a secondary
  //      ErrSwCmdSequence (0x08040016)!
  // =========================================================================
  LOG_INFO(
      "Verifying [kmac_app.sv:755-787] (FIXED_IN_V2_RTL & NEW_IN_V2): "
      "ErrKeyNotValid recovery & spurious kmac_done in StIdle...");
  abs_mmio_write32(kKmacBase + KMAC_PREFIX_0_REG_OFFSET, 0x4d4b2001u);
  abs_mmio_write32(kKmacBase + KMAC_PREFIX_1_REG_OFFSET, 0x00014341u);
  uint32_t cfg_sideload =
      bitfield_bit32_write(cfg_3a, KMAC_CFG_SHADOWED_SIDELOAD_BIT, true);
  write_cfg_shadowed(cfg_sideload);
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, UINT32_MAX);

  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_START << KMAC_CMD_CMD_OFFSET);
  uint32_t err_sideload = abs_mmio_read32(kKmacBase + KMAC_ERR_CODE_REG_OFFSET);
  LOG_INFO("[kmac_app.sv:690] ErrKeyNotValid ERR_CODE=0x%08x", err_sideload);
  CHECK_EQ(err_sideload, 0x01000000u,
           "Expected ErrKeyNotValid (0x01000000) when sideload=1 without key");

  // Clear INTR_STATE before writing CMD.err_processed = 1 so we isolate pulses
  // generated by StErrorAwaitAbsorbed.
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, UINT32_MAX);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   1u << KMAC_CMD_ERR_PROCESSED_BIT);
  wait_kmac_idle();

  uint32_t status_after_recov =
      abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET);
  uint32_t intr_after_recov =
      abs_mmio_read32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET);
  uint32_t state_share0_after =
      abs_mmio_read32(kKmacBase + KMAC_STATE_REG_OFFSET);
  uint32_t state_share1_after =
      abs_mmio_read32(kKmacBase + KMAC_STATE_REG_OFFSET + 0x100u);
  LOG_INFO(
      "[kmac_app.sv:776-785] After CMD.err_processed=1: STATUS=0x%08x, "
      "INTR_STATE=0x%08x, STATE[0]=0x%08x/0x%08x",
      status_after_recov, intr_after_recov, state_share0_after,
      state_share1_after);

  // FIXED_IN_V2_RTL: STATUS is back in SHA3_IDLE=1 (SHA3_SQUEEZE=0) and STATE
  // shares read 0x00000000 (no dummy digest leakage!).
  CHECK((status_after_recov & (1u << KMAC_STATUS_SHA3_IDLE_BIT)) != 0u &&
            (status_after_recov & (1u << KMAC_STATUS_SHA3_SQUEEZE_BIT)) == 0u,
        "Expected StErrorAwaitAbsorbed to return directly to SHA3_IDLE=1");
  CHECK_EQ(state_share0_after, 0x00000000u,
           "Expected STATE share 0 to be zeroed in StIdle (FIXED_IN_V2_RTL)");
  CHECK_EQ(state_share1_after, 0x00000000u,
           "Expected STATE share 1 to be zeroed in StIdle (FIXED_IN_V2_RTL)");

  // NEW_IN_V2: StErrorAwaitAbsorbed simultaneously asserts absorbed_o =
  // MuBi4True (setting INTR_STATE.kmac_done = 1) while already returning KMAC
  // to StIdle!
  CHECK((intr_after_recov & (1u << KMAC_INTR_STATE_KMAC_DONE_BIT)) != 0u,
        "Expected spurious INTR_STATE.kmac_done=1 from StErrorAwaitAbsorbed");

  // Writing CMD.done in response to that spurious kmac_done triggers a
  // secondary ErrSwCmdSequence (0x08040016)!
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_DONE << KMAC_CMD_CMD_OFFSET);
  uint32_t err_after_done =
      abs_mmio_read32(kKmacBase + KMAC_ERR_CODE_REG_OFFSET);
  LOG_INFO(
      "[kmac_app.sv:783-785] Secondary ERR_CODE after CMD.done in StIdle: "
      "0x%08x",
      err_after_done);
  CHECK_EQ(err_after_done, 0x08040016u,
           "Expected secondary ErrSwCmdSequence (0x08040016) when issuing "
           "CMD.done after StErrorAwaitAbsorbed");
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   1u << KMAC_CMD_ERR_PROCESSED_BIT);
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, UINT32_MAX);

  // =========================================================================
  // 6. [kmac_staterd.sv:54-61 & kmac.sv:1005-1011] (CONFIRMED_PRESENT_ON_V2):
  //    TL-UL bus faults on STATE write (mcause=7), MSG_FIFO read (mcause=5),
  //    and sub-word KMAC_PERMIT CSR write (mcause=7).
  // =========================================================================
  load_store_fault_seen = false;
  last_mcause = 0;
  abs_mmio_write32(kKmacBase + KMAC_STATE_REG_OFFSET, 0xdeadbeefu);
  CHECK(load_store_fault_seen && last_mcause == 7u,
        "Expected Store Access Fault (mcause=7) on STATE write");

  load_store_fault_seen = false;
  last_mcause = 0;
  (void)abs_mmio_read32(kKmacBase + KMAC_MSG_FIFO_REG_OFFSET);
  CHECK(load_store_fault_seen && last_mcause == 5u,
        "Expected Load Access Fault (mcause=5) on MSG_FIFO read");

  load_store_fault_seen = false;
  last_mcause = 0;
  abs_mmio_write8(kKmacBase + KMAC_PREFIX_0_REG_OFFSET, 0xaau);
  CHECK(load_store_fault_seen && last_mcause == 7u,
        "Expected Store Access Fault (mcause=7) on sub-word CSR write");

  LOG_INFO("All KMAC Earlgrey v2 errata checks passed on CW340 FPGA!");
  return true;
}
