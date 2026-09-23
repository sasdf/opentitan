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

#include "edn_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "kmac_regs.h"
#include "rv_core_ibex_regs.h"

#define CHECK_EQ(a, b, ...) CHECK((a) == (b), __VA_ARGS__)

OTTF_DEFINE_TEST_CONFIG(.ignore_alerts = true);

enum {
  kKmacBase = TOP_EARLGREY_KMAC_BASE_ADDR,
  kIbexBase = TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR,
  kEdn0Base = TOP_EARLGREY_EDN0_BASE_ADDR,
};

static volatile bool load_store_fault_seen = false;
static volatile uint32_t last_mcause = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  load_store_fault_seen = true;
  last_mcause = ibex_mcause_read();
  abs_mmio_write32(kIbexBase + RV_CORE_IBEX_ERR_STATUS_REG_OFFSET, UINT32_MAX);
}

void ottf_internal_isr(uint32_t *exc_info) {
  (void)exc_info;
  load_store_fault_seen = true;
  last_mcause = ibex_mcause_read();
  abs_mmio_write32(kIbexBase + RV_CORE_IBEX_ERR_STATUS_REG_OFFSET, UINT32_MAX);
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
  // 1. [kmac_errchk.sv:268] (TRUE_SILICON_ERRATA — Verified First While
  // ERR_CODE==0):
  //    CFG_SHADOWED.KMAC_EN is evaluated independently of CFG_SHADOWED.MODE
  //    across kmac_errchk.sv, kmac_core.sv, and sha3pad.sv:
  //    - Subcase 3A (kmac_en=1, mode=Sha3 (0), kstrength=L256, PREFIX="KMAC"):
  //      Raises ZERO error (kmac_err=0, ERR_CODE=0x00000000), skips cSHAKE
  //      StPrefix ("KMAC"), absorbs bytepad(encode_string(K), 136) + M, and
  //      pads with SHA-3 suffix 0x06 instead of cSHAKE suffix 0x04 — producing
  //      a non-standard hybrid digest d_kmac_sha3 that differs from both true
  //      KMAC-256 (d_true_kmac) and plain SHA3-256 (d_plain_sha3), and matches
  //      bit-for-bit unkeyed SHA3-256 fed manual bytepad(encode_string(K), 136)
  //      + M (d_manual_hybrid)!
  //    - Subcase 3B (kmac_en=1, mode=Shake (2), kstrength=L256, PREFIX="KMAC"):
  //      Also raises ZERO error (kmac_err=0, ERR_CODE=0x00000000), producing
  //      SHAKE-256(bytepad(encode_string(K), 136) || M) (suffix 0x1f).
  //    - Subcase 3C (kmac_en=1, mode=Sha3 (0), kstrength=L256, PREFIX=0):
  //      Raises ErrIncorrectFunctionName (0x07010000) even though mode=Sha3.
  // =========================================================================
  LOG_INFO(
      "Verifying [kmac_errchk.sv:268] (TRUE_SILICON_ERRATA): Subcase 3A "
      "(kmac_en=1, mode=Sha3, kstrength=L256, PREFIX='KMAC')...");

  CHECK_EQ(abs_mmio_read32(kKmacBase + KMAC_ERR_CODE_REG_OFFSET), 0x00000000u,
           "Expected initial ERR_CODE == 0x00000000 before Subcase 3A");

  // Configure PREFIX = encode_string("KMAC") and 128-bit key 0x55555555 * 4.
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

  // --- Subcase 3A: kmac_en = 1, mode = Sha3 (0), kstrength = L256, PREFIX =
  // "KMAC" --- Also exercises [kmac_entropy.sv:357-381,527-556,604-644]
  // (latching SwMode from StRandReset and verifying that 5 ENTROPY_SEED words
  // stall while the 6th word completes StSwSeedWait -> StRandReady with
  // ERR_CODE == 0x00000000).
  uint32_t cfg_3a = 0;
  cfg_3a = bitfield_bit32_write(cfg_3a, KMAC_CFG_SHADOWED_KMAC_EN_BIT, true);
  cfg_3a = bitfield_field32_write(cfg_3a, KMAC_CFG_SHADOWED_MODE_FIELD,
                                  KMAC_CFG_SHADOWED_MODE_VALUE_SHA3);
  cfg_3a = bitfield_field32_write(cfg_3a, KMAC_CFG_SHADOWED_KSTRENGTH_FIELD,
                                  KMAC_CFG_SHADOWED_KSTRENGTH_VALUE_L256);
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
  for (uint32_t i = 0; i < 5u; ++i) {
    abs_mmio_write32(kKmacBase + KMAC_ENTROPY_SEED_REG_OFFSET, 0x12345678u + i);
  }
  uint32_t status_5seeds = abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET);
  CHECK((status_5seeds & (1u << KMAC_STATUS_SHA3_SQUEEZE_BIT)) == 0u,
        "[kmac_entropy.sv:357-381,527-556,604-644] Expected stall after 5 "
        "ENTROPY_SEED words");
  abs_mmio_write32(kKmacBase + KMAC_ENTROPY_SEED_REG_OFFSET, 0x12345678u + 5u);
  wait_kmac_squeeze();
  uint32_t intr_3a = abs_mmio_read32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET);
  uint32_t err_3a = abs_mmio_read32(kKmacBase + KMAC_ERR_CODE_REG_OFFSET);
  uint32_t d_kmac_sha3 = read_unmasked_state_word(0);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_DONE << KMAC_CMD_CMD_OFFSET);
  wait_kmac_idle();
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, UINT32_MAX);

  CHECK_EQ(intr_3a & (1u << KMAC_INTR_STATE_KMAC_ERR_BIT), 0u,
           "[kmac_errchk.sv:268 3A] Expected kmac_err == 0 when kmac_en=1, "
           "mode=Sha3, PREFIX='KMAC'");
  CHECK_EQ(err_3a, 0x00000000u,
           "[kmac_errchk.sv:268 3A] Expected ERR_CODE == 0 when kmac_en=1, "
           "mode=Sha3, PREFIX='KMAC'");

  // Compute True NIST KMAC-256 (kmac_en = 1, mode = CShake (3), kstrength =
  // L256)
  uint32_t cfg_true_kmac =
      bitfield_field32_write(cfg_3a, KMAC_CFG_SHADOWED_MODE_FIELD,
                             KMAC_CFG_SHADOWED_MODE_VALUE_CSHAKE);
  write_cfg_shadowed(cfg_true_kmac);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_START << KMAC_CMD_CMD_OFFSET);
  wait_kmac_absorb();
  abs_mmio_write32(kKmacBase + KMAC_MSG_FIFO_REG_OFFSET, 0x44332211u);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_PROCESS << KMAC_CMD_CMD_OFFSET);
  wait_kmac_squeeze();
  uint32_t d_true_kmac = read_unmasked_state_word(0);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_DONE << KMAC_CMD_CMD_OFFSET);
  wait_kmac_idle();
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, UINT32_MAX);

  // Compute Plain Unkeyed SHA3-256 (kmac_en = 0, mode = Sha3 (0), kstrength =
  // L256)
  uint32_t cfg_plain_sha3 =
      bitfield_bit32_write(cfg_3a, KMAC_CFG_SHADOWED_KMAC_EN_BIT, false);
  write_cfg_shadowed(cfg_plain_sha3);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_START << KMAC_CMD_CMD_OFFSET);
  wait_kmac_absorb();
  abs_mmio_write32(kKmacBase + KMAC_MSG_FIFO_REG_OFFSET, 0x44332211u);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_PROCESS << KMAC_CMD_CMD_OFFSET);
  wait_kmac_squeeze();
  uint32_t d_plain_sha3 = read_unmasked_state_word(0);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_DONE << KMAC_CMD_CMD_OFFSET);
  wait_kmac_idle();
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, UINT32_MAX);

  // Compute Manual Hybrid SHA3-256 (kmac_en = 0, mode = Sha3 (0), kstrength =
  // L256) where software manually feeds bytepad(encode_string(K), 136) +
  // 0x44332211:
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_START << KMAC_CMD_CMD_OFFSET);
  wait_kmac_absorb();
  // Word 0: left_encode(136) = {0x01, 0x88}, left_encode(128) = {0x01, 0x80}
  abs_mmio_write32(kKmacBase + KMAC_MSG_FIFO_REG_OFFSET, 0x80018801u);
  // Words 1..4: 16-byte key (0x55555555 * 4)
  for (uint32_t i = 0; i < 4u; ++i) {
    abs_mmio_write32(kKmacBase + KMAC_MSG_FIFO_REG_OFFSET, 0x55555555u);
  }
  // Words 5..33: 29 words (116 bytes) of zero padding to complete 136-byte rate
  // block
  for (uint32_t i = 0; i < 29u; ++i) {
    abs_mmio_write32(kKmacBase + KMAC_MSG_FIFO_REG_OFFSET, 0x00000000u);
  }
  // Message word: 0x44332211
  abs_mmio_write32(kKmacBase + KMAC_MSG_FIFO_REG_OFFSET, 0x44332211u);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_PROCESS << KMAC_CMD_CMD_OFFSET);
  wait_kmac_squeeze();
  uint32_t d_manual_hybrid = read_unmasked_state_word(0);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_DONE << KMAC_CMD_CMD_OFFSET);
  wait_kmac_idle();
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, UINT32_MAX);

  LOG_INFO(
      "[kmac_errchk.sv:268 3A] Digests: d_kmac_sha3=0x%08x, "
      "d_manual_hybrid=0x%08x, "
      "d_true_kmac=0x%08x, d_plain_sha3=0x%08x (intr=0x%08x, err_code=0x%08x)",
      d_kmac_sha3, d_manual_hybrid, d_true_kmac, d_plain_sha3, intr_3a, err_3a);
  CHECK(d_kmac_sha3 != d_true_kmac,
        "[kmac_errchk.sv:268 3A] Expected d_kmac_sha3 != d_true_kmac");
  CHECK(d_kmac_sha3 != d_plain_sha3,
        "[kmac_errchk.sv:268 3A] Expected d_kmac_sha3 != d_plain_sha3");
  CHECK_EQ(d_kmac_sha3, d_manual_hybrid,
           "[kmac_errchk.sv:268 3A] Expected bit-exact match d_kmac_sha3 == "
           "d_manual_hybrid (SHA3-256(bytepad(K,136) || M))");

  // --- Subcase 3B: kmac_en = 1, mode = Shake (2), kstrength = L256, PREFIX =
  // "KMAC" ---
  LOG_INFO(
      "Verifying [kmac_errchk.sv:268] Subcase 3B (kmac_en=1, mode=Shake, "
      "kstrength=L256, PREFIX='KMAC')...");
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

  LOG_INFO(
      "[kmac_errchk.sv:268 3B] d_kmac_shake=0x%08x (intr=0x%08x, "
      "err_code=0x%08x)",
      d_kmac_shake, intr_3b, err_3b);
  CHECK_EQ(intr_3b & (1u << KMAC_INTR_STATE_KMAC_ERR_BIT), 0u,
           "[kmac_errchk.sv:268 3B] Expected kmac_err == 0 when kmac_en=1, "
           "mode=Shake, PREFIX='KMAC'");
  CHECK_EQ(err_3b, 0x00000000u,
           "[kmac_errchk.sv:268 3B] Expected ERR_CODE == 0 when kmac_en=1, "
           "mode=Shake, PREFIX='KMAC'");
  CHECK(d_kmac_shake != d_true_kmac && d_kmac_shake != d_kmac_sha3,
        "[kmac_errchk.sv:268 3B] Expected d_kmac_shake to differ from "
        "d_true_kmac "
        "and d_kmac_sha3");

  // --- Subcase 3C: kmac_en = 1, mode = Sha3 (0), kstrength = L256, PREFIX = 0
  // ---
  LOG_INFO(
      "Verifying [kmac_errchk.sv:268] Subcase 3C (kmac_en=1, mode=Sha3, "
      "kstrength=L256, PREFIX=0 -> ErrIncorrectFunctionName)...");
  abs_mmio_write32(kKmacBase + KMAC_PREFIX_0_REG_OFFSET, 0u);
  write_cfg_shadowed(cfg_3a);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_START << KMAC_CMD_CMD_OFFSET);
  uint32_t err_code_3c = abs_mmio_read32(kKmacBase + KMAC_ERR_CODE_REG_OFFSET);
  LOG_INFO(
      "[kmac_errchk.sv:268 3C] ERR_CODE=0x%08x when PREFIX=0 and mode=Sha3",
      err_code_3c);
  CHECK_EQ(
      err_code_3c, 0x07010000u,
      "[kmac_errchk.sv:268 3C] Expected ErrIncorrectFunctionName (0x07010000) "
      "in Sha3 mode when KMAC_EN=1 and PREFIX != KMAC");
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
  // 2. [kmac_app.sv:200,728-734] (SPEC_DOC_ERRATA):
  //    SwPushedMsgFifo (0x02) fires in StIdle and encodes 5-bit sparse
  //    kmac_mux_sel_e (SelNone = 0x14) + truncated 8-bit st_e (0xbe) =
  //    0x0200be14.
  // =========================================================================
  LOG_INFO(
      "Verifying [kmac_app.sv:200,728-734] (SPEC_DOC_ERRATA): SwPushedMsgFifo "
      "in "
      "StIdle...");
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, UINT32_MAX);
  abs_mmio_write32(kKmacBase + KMAC_MSG_FIFO_REG_OFFSET, 0x11223344u);
  uint32_t err_code_001 = abs_mmio_read32(kKmacBase + KMAC_ERR_CODE_REG_OFFSET);
  CHECK_EQ(err_code_001, 0x0200be14u,
           "[kmac_app.sv:200,728-734] Expected ERR_CODE 0x0200be14 in StIdle");
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   1u << KMAC_CMD_ERR_PROCESSED_BIT);
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, UINT32_MAX);

  // =========================================================================
  // 3. [kmac_errchk.sv:236-246,321-376,399] (SPEC_DOC_ERRATA):
  //    kmac_errchk blocks CmdStart on UnexpectedModeStrength (0x06) when
  //    EN_UNSUPPORTED_MODESTRENGTH = 0, and packs 6-bit sparse cmd (0x2e) in
  //    ERR_CODE[5:0].
  // =========================================================================
  LOG_INFO(
      "Verifying [kmac_errchk.sv:236-246,321-376,399] (SPEC_DOC_ERRATA): "
      "block_swcmd on "
      "UnexpectedModeStrength & 6-bit sparse cmd...");
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
           "[kmac_errchk.sv:236-246,321-376,399] Expected ERR_CODE 0x06020012 "
           "for MODE=1");
  CHECK((status_002a & (1u << KMAC_STATUS_SHA3_IDLE_BIT)) != 0u &&
            (status_002a & (1u << KMAC_STATUS_SHA3_ABSORB_BIT)) == 0u,
        "[kmac_errchk.sv:236-246,321-376,399] Expected CmdStart to be blocked "
        "(SHA3_IDLE=1), got "
        "0x%08x",
        status_002a);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   1u << KMAC_CMD_ERR_PROCESSED_BIT);
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, UINT32_MAX);

  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_PROCESS << KMAC_CMD_CMD_OFFSET);
  uint32_t err_code_002b =
      abs_mmio_read32(kKmacBase + KMAC_ERR_CODE_REG_OFFSET);
  CHECK_EQ(err_code_002b, 0x0804002eu,
           "[kmac_errchk.sv:236-246,321-376,399] Expected 6-bit sparse "
           "CmdProcess (0x2e) in "
           "ERR_CODE 0x0804002e");
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   1u << KMAC_CMD_ERR_PROCESSED_BIT);
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, UINT32_MAX);

  // =========================================================================
  // 4. [kmac_entropy.sv:345-348,526-556,683-717] &
  // [kmac_entropy.sv:357-381,527-556,604-644] (INTENDED_SECURITY_HARDENING):
  //    Verify that once mode_q is latched to SwMode (in Subcase 3A above),
  //    writing ENTROPY_MODE=EdnMode updates CFG_SHADOWED readback, but hardware
  //    ignores the mode change (does not enter StRandEdn or timeout even with
  //    EDN0 disabled and WAIT_TIMER=2000).
  // =========================================================================
  LOG_INFO(
      "Verifying [kmac_entropy.sv:345-348,526-556,683-717] & "
      "[kmac_entropy.sv:357-381,527-556,604-644] "
      "(INTENDED_SECURITY_HARDENING): 6-word seed & ENTROPY_MODE latch...");
  uint32_t saved_edn0_ctrl = abs_mmio_read32(kEdn0Base + EDN_CTRL_REG_OFFSET);
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9999u);
  abs_mmio_write32(kKmacBase + KMAC_ENTROPY_PERIOD_REG_OFFSET,
                   2000u << KMAC_ENTROPY_PERIOD_WAIT_TIMER_OFFSET);
  abs_mmio_write32(kKmacBase + KMAC_PREFIX_0_REG_OFFSET, 0x4d4b2001u);
  abs_mmio_write32(kKmacBase + KMAC_PREFIX_1_REG_OFFSET, 0x00014341u);
  for (uint32_t i = 2; i < 11u; ++i) {
    abs_mmio_write32(kKmacBase + KMAC_PREFIX_0_REG_OFFSET + i * 4u, 0u);
  }
  uint32_t cfg_edn_timeout = 0;
  cfg_edn_timeout = bitfield_bit32_write(cfg_edn_timeout,
                                         KMAC_CFG_SHADOWED_KMAC_EN_BIT, true);
  cfg_edn_timeout =
      bitfield_field32_write(cfg_edn_timeout, KMAC_CFG_SHADOWED_MODE_FIELD,
                             KMAC_CFG_SHADOWED_MODE_VALUE_CSHAKE);
  cfg_edn_timeout =
      bitfield_field32_write(cfg_edn_timeout, KMAC_CFG_SHADOWED_KSTRENGTH_FIELD,
                             KMAC_CFG_SHADOWED_KSTRENGTH_VALUE_L256);
  cfg_edn_timeout = bitfield_field32_write(
      cfg_edn_timeout, KMAC_CFG_SHADOWED_ENTROPY_MODE_FIELD,
      KMAC_CFG_SHADOWED_ENTROPY_MODE_VALUE_EDN_MODE);
  cfg_edn_timeout = bitfield_bit32_write(
      cfg_edn_timeout, KMAC_CFG_SHADOWED_ENTROPY_READY_BIT, true);
  write_cfg_shadowed(cfg_edn_timeout);
  uint32_t cfg_rb = abs_mmio_read32(kKmacBase + KMAC_CFG_SHADOWED_REG_OFFSET);
  CHECK_EQ(bitfield_field32_read(cfg_rb, KMAC_CFG_SHADOWED_ENTROPY_MODE_FIELD),
           KMAC_CFG_SHADOWED_ENTROPY_MODE_VALUE_EDN_MODE,
           "[kmac_entropy.sv:345-348,526-556,683-717] Expected CFG_SHADOWED "
           "readback to show EdnMode");
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_START << KMAC_CMD_CMD_OFFSET);
  wait_kmac_absorb();
  abs_mmio_write32(kKmacBase + KMAC_MSG_FIFO_REG_OFFSET, 0x00020100u);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_PROCESS << KMAC_CMD_CMD_OFFSET);
  wait_kmac_squeeze();
  CHECK_EQ(abs_mmio_read32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET) &
               (1u << KMAC_INTR_STATE_KMAC_ERR_BIT),
           0u,
           "[kmac_entropy.sv:345-348,526-556,683-717] Expected mode_q to stay "
           "locked to SwMode without "
           "EDN timeout");
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_DONE << KMAC_CMD_CMD_OFFSET);
  wait_kmac_idle();
  abs_mmio_write32(kKmacBase + KMAC_ENTROPY_PERIOD_REG_OFFSET, 0u);
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, saved_edn0_ctrl);

  uint32_t cfg_sha3_kmac0 = cfg_plain_sha3;
  write_cfg_shadowed(cfg_sha3_kmac0);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_START << KMAC_CMD_CMD_OFFSET);
  wait_kmac_absorb();
  abs_mmio_write32(kKmacBase + KMAC_MSG_FIFO_REG_OFFSET, 0x64636261u);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_PROCESS << KMAC_CMD_CMD_OFFSET);
  wait_kmac_squeeze();
  uint32_t sha3_kmac0_w0 = read_unmasked_state_word(0);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_DONE << KMAC_CMD_CMD_OFFSET);
  wait_kmac_idle();

  // =========================================================================
  // 5. [sha3pad.sv:327-358,509-533] (SPEC_DOC_ERRATA):
  //    Raw 44-byte PREFIX_0..10 absorption in cSHAKE absorbs stale upper
  //    PREFIX_10 bytes even when PREFIX_0 encodes empty N="" and S="".
  // =========================================================================
  LOG_INFO(
      "Verifying [sha3pad.sv:327-358,509-533] (SPEC_DOC_ERRATA): Raw 44-byte "
      "PREFIX_0..10 absorption...");
  uint32_t cfg_cshake = 0;
  cfg_cshake = bitfield_field32_write(cfg_cshake, KMAC_CFG_SHADOWED_MODE_FIELD,
                                      KMAC_CFG_SHADOWED_MODE_VALUE_CSHAKE);
  cfg_cshake =
      bitfield_field32_write(cfg_cshake, KMAC_CFG_SHADOWED_KSTRENGTH_FIELD,
                             KMAC_CFG_SHADOWED_KSTRENGTH_VALUE_L256);
  write_cfg_shadowed(cfg_cshake);
  abs_mmio_write32(kKmacBase + KMAC_PREFIX_0_REG_OFFSET, 0x00010001u);
  for (uint32_t i = 1; i < 11u; ++i) {
    abs_mmio_write32(kKmacBase + KMAC_PREFIX_0_REG_OFFSET + i * 4u, 0u);
  }
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_START << KMAC_CMD_CMD_OFFSET);
  wait_kmac_absorb();
  abs_mmio_write32(kKmacBase + KMAC_MSG_FIFO_REG_OFFSET, 0x64636261u);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_PROCESS << KMAC_CMD_CMD_OFFSET);
  wait_kmac_squeeze();
  uint32_t cshake_clean_w0 = read_unmasked_state_word(0);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_DONE << KMAC_CMD_CMD_OFFSET);
  wait_kmac_idle();

  // Now write stale non-zero bytes into PREFIX_10 while keeping PREFIX_0 =
  // 0x00010001 (empty N="", S="").
  abs_mmio_write32(kKmacBase + KMAC_PREFIX_10_REG_OFFSET, 0xdeadbeefu);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_START << KMAC_CMD_CMD_OFFSET);
  wait_kmac_absorb();
  abs_mmio_write32(kKmacBase + KMAC_MSG_FIFO_REG_OFFSET, 0x64636261u);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_PROCESS << KMAC_CMD_CMD_OFFSET);
  wait_kmac_squeeze();
  uint32_t cshake_stale_w0 = read_unmasked_state_word(0);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_DONE << KMAC_CMD_CMD_OFFSET);
  wait_kmac_idle();
  CHECK_EQ(cshake_clean_w0, 0xbb97943eu,
           "[sha3pad.sv:327-358,509-533] Expected golden cSHAKE w0=0xbb97943e");
  CHECK(cshake_stale_w0 != cshake_clean_w0,
        "[sha3pad.sv:327-358,509-533] Expected stale PREFIX_10 to alter cSHAKE "
        "digest "
        "(0x%08x vs 0x%08x)",
        cshake_stale_w0, cshake_clean_w0);

  // =========================================================================
  // 6. [kmac.sv:684] & [kmac_app.sv:497-513] (SPEC_DOC_ERRATA &
  //    TRUE_SILICON_ERRATA):
  //    ERR_CODE remains sticky across CMD.err_processed = 1, and
  //    CMD.err_processed = 1 does NOT abort active SW hashing in StSw.
  // =========================================================================
  LOG_INFO(
      "Verifying [kmac.sv:684 / kmac_app.sv:497-513] (TRUE_SILICON_ERRATA): "
      "ERR_CODE "
      "sticky & CMD.err_processed no-abort in StSw...");
  write_cfg_shadowed(cfg_sha3_kmac0);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_START << KMAC_CMD_CMD_OFFSET);
  wait_kmac_absorb();

  // Issue invalid CMD.RUN (0x31) during StAbsorb -> ErrSwCmdSequence
  // (0x08040131).
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_RUN << KMAC_CMD_CMD_OFFSET);
  CHECK_EQ(abs_mmio_read32(kKmacBase + KMAC_ERR_CODE_REG_OFFSET), 0x08040131u,
           "[kmac.hjson:664-669] Expected ERR_CODE 0x08040131 on CMD.RUN in "
           "StAbsorb");

  // Write CMD.err_processed = 1.
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   1u << KMAC_CMD_ERR_PROCESSED_BIT);
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, UINT32_MAX);

  // Verify ERR_CODE is still 0x08040131 (sticky) AND SHA3_ABSORB is still 1!
  CHECK_EQ(abs_mmio_read32(kKmacBase + KMAC_ERR_CODE_REG_OFFSET), 0x08040131u,
           "[kmac.sv:684] Expected ERR_CODE to remain sticky across "
           "CMD.err_processed = 1");
  uint32_t status_005b = abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET);
  CHECK((status_005b & (1u << KMAC_STATUS_SHA3_ABSORB_BIT)) != 0u,
        "[kmac_app.sv:497-513] Expected SHA3_ABSORB=1 to remain active after "
        "CMD.err_processed = 1 in StSw, got 0x%08x",
        status_005b);

  // Finish the active SW hash and verify digest matches clean SHA3-256.
  abs_mmio_write32(kKmacBase + KMAC_MSG_FIFO_REG_OFFSET, 0x64636261u);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_PROCESS << KMAC_CMD_CMD_OFFSET);
  wait_kmac_squeeze();
  CHECK_EQ(
      read_unmasked_state_word(0), sha3_kmac0_w0,
      "[kmac_app.sv:497-513] Expected active SW hash to complete normally");

  // =========================================================================
  // 7. [kmac_staterd.sv:45-92 / kmac_reg_pkg.sv:544-592]
  // (INTENDED_SECURITY_HARDENING):
  //    (a) STATE write -> Store Access Fault (mcause=7);
  //        MSG_FIFO read -> Load Access Fault (mcause=5).
  //    (b) KMAC_PERMIT sub-word CSR write fault (mcause=7) while sub-word read
  //        succeeds.
  //    (c) STATE sub-word byte read reflects word-first STATE_ENDIANNESS swap.
  // =========================================================================
  LOG_INFO(
      "Verifying [kmac_staterd.sv:45-92 / kmac_reg_pkg.sv:544-592] "
      "(INTENDED_SECURITY_HARDENING): "
      "TL-UL bus faults & STATE sub-word endianness...");
  uint8_t byte0_s0_le = abs_mmio_read8(kKmacBase + KMAC_STATE_REG_OFFSET);
  uint8_t byte0_s1_le =
      abs_mmio_read8(kKmacBase + KMAC_STATE_REG_OFFSET + 0x100u);
  CHECK_EQ((uint8_t)(byte0_s0_le ^ byte0_s1_le),
           (uint8_t)(sha3_kmac0_w0 & 0xffu),
           "[kmac_staterd.sv:45-92] Expected unmasked lb[0] == w0[7:0] when "
           "STATE_ENDIANNESS=0");

  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_DONE << KMAC_CMD_CMD_OFFSET);
  wait_kmac_idle();

  // Re-run with STATE_ENDIANNESS = 1 and check unmasked lb[0] == w0[31:24].
  uint32_t cfg_be = bitfield_bit32_write(
      cfg_sha3_kmac0, KMAC_CFG_SHADOWED_STATE_ENDIANNESS_BIT, true);
  write_cfg_shadowed(cfg_be);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_START << KMAC_CMD_CMD_OFFSET);
  wait_kmac_absorb();
  abs_mmio_write32(kKmacBase + KMAC_MSG_FIFO_REG_OFFSET, 0x64636261u);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_PROCESS << KMAC_CMD_CMD_OFFSET);
  wait_kmac_squeeze();
  uint8_t byte0_s0_be = abs_mmio_read8(kKmacBase + KMAC_STATE_REG_OFFSET);
  uint8_t byte0_s1_be =
      abs_mmio_read8(kKmacBase + KMAC_STATE_REG_OFFSET + 0x100u);
  CHECK_EQ((uint8_t)(byte0_s0_be ^ byte0_s1_be),
           (uint8_t)((sha3_kmac0_w0 >> 24) & 0xffu),
           "[kmac_staterd.sv:45-92] Expected unmasked lb[0] == w0[31:24] when "
           "STATE_ENDIANNESS=1");
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_DONE << KMAC_CMD_CMD_OFFSET);
  wait_kmac_idle();

  // (a) STATE write fault (mcause = 7).
  load_store_fault_seen = false;
  last_mcause = 0;
  abs_mmio_write32(kKmacBase + KMAC_STATE_REG_OFFSET, 0x12345678u);
  CHECK(load_store_fault_seen && last_mcause == 7u,
        "[kmac_staterd.sv:56] Expected Store Access Fault (mcause=7) on STATE "
        "write, got fault=%d mcause=%u",
        load_store_fault_seen, last_mcause);

  // (a) MSG_FIFO read fault (mcause = 5).
  load_store_fault_seen = false;
  last_mcause = 0;
  (void)abs_mmio_read32(kKmacBase + KMAC_MSG_FIFO_REG_OFFSET);
  CHECK(
      load_store_fault_seen && last_mcause == 5u,
      "[kmac_staterd.sv:56] Expected Load Access Fault (mcause=5) on MSG_FIFO "
      "read, got fault=%d mcause=%u",
      load_store_fault_seen, last_mcause);

  // (b) KMAC_PERMIT sub-word CSR write fault (mcause = 7) vs sub-word CSR read.
  abs_mmio_write32(kKmacBase + KMAC_PREFIX_0_REG_OFFSET, 0xa5a55a5au);
  load_store_fault_seen = false;
  uint8_t pfx_b0 = abs_mmio_read8(kKmacBase + KMAC_PREFIX_0_REG_OFFSET);
  CHECK(!load_store_fault_seen && pfx_b0 == 0x5au,
        "[kmac_reg_pkg.sv:544-592] Expected sub-word CSR read to succeed");

  load_store_fault_seen = false;
  last_mcause = 0;
  abs_mmio_write8(kKmacBase + KMAC_PREFIX_0_REG_OFFSET, 0xffu);
  CHECK(load_store_fault_seen && last_mcause == 7u,
        "[kmac_reg_pkg.sv:544-592] Expected Store Access Fault (mcause=7) on "
        "sub-word write to PREFIX_0");
  CHECK_EQ(
      abs_mmio_read32(kKmacBase + KMAC_PREFIX_0_REG_OFFSET), 0xa5a55a5au,
      "[kmac_reg_pkg.sv:544-592] Expected sub-word write to PREFIX_0 to be "
      "dropped");

  LOG_INFO("All KMAC errata & security hardening checks passed!");
  return true;
}
