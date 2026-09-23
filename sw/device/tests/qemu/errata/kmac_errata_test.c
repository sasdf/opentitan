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
  // 1. [ERRATA-KMAC-001] (SPEC_DOC_ERRATA):
  //    SwPushedMsgFifo (0x02) fires in StIdle and encodes 5-bit sparse
  //    kmac_mux_sel_e (SelNone = 0x14) + truncated 8-bit st_e (0xbe) =
  //    0x0200be14.
  // =========================================================================
  LOG_INFO(
      "Verifying [ERRATA-KMAC-001] (SPEC_DOC_ERRATA): SwPushedMsgFifo in "
      "StIdle...");
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, UINT32_MAX);
  abs_mmio_write32(kKmacBase + KMAC_MSG_FIFO_REG_OFFSET, 0x11223344u);
  uint32_t err_code_001 = abs_mmio_read32(kKmacBase + KMAC_ERR_CODE_REG_OFFSET);
  CHECK_EQ(err_code_001, 0x0200be14u,
           "[ERRATA-KMAC-001] Expected ERR_CODE 0x0200be14 in StIdle");
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   1u << KMAC_CMD_ERR_PROCESSED_BIT);
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, UINT32_MAX);

  // =========================================================================
  // 2. [ERRATA-KMAC-002] (SPEC_DOC_ERRATA):
  //    kmac_errchk blocks CmdStart on UnexpectedModeStrength (0x06) when
  //    EN_UNSUPPORTED_MODESTRENGTH = 0, and packs 6-bit sparse cmd (0x2e) in
  //    ERR_CODE[5:0].
  // =========================================================================
  LOG_INFO(
      "Verifying [ERRATA-KMAC-002] (SPEC_DOC_ERRATA): block_swcmd on "
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
           "[ERRATA-KMAC-002] Expected ERR_CODE 0x06020012 for MODE=1");
  CHECK((status_002a & (1u << KMAC_STATUS_SHA3_IDLE_BIT)) != 0u &&
            (status_002a & (1u << KMAC_STATUS_SHA3_ABSORB_BIT)) == 0u,
        "[ERRATA-KMAC-002] Expected CmdStart to be blocked (SHA3_IDLE=1), got "
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
           "[ERRATA-KMAC-002] Expected 6-bit sparse CmdProcess (0x2e) in "
           "ERR_CODE 0x0804002e");
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   1u << KMAC_CMD_ERR_PROCESSED_BIT);
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, UINT32_MAX);

  // =========================================================================
  // 3. [ERRATA-KMAC-007] & [ERRATA-KMAC-008] (INTENDED_SECURITY_HARDENING):
  //    Mandatory 6-word EDN0 seed handshake & one-time ENTROPY_MODE latch.
  //    First trigger ErrWaitTimerExpired (0x04000000) in EdnMode with EDN0
  //    disabled so CMD.err_processed resets kmac_entropy from StRandErr to
  //    StRandReset, then latch SwMode and seed 6 words of ENTROPY_SEED.
  // =========================================================================
  LOG_INFO(
      "Verifying [ERRATA-KMAC-007] & [ERRATA-KMAC-008] "
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
  abs_mmio_write32(kKmacBase + KMAC_KEY_LEN_REG_OFFSET,
                   KMAC_KEY_LEN_LEN_VALUE_KEY128);
  for (uint32_t i = 0; i < 16u; ++i) {
    abs_mmio_write32(kKmacBase + KMAC_KEY_SHARE0_0_REG_OFFSET + i * 4u,
                     0x55555555u);
    abs_mmio_write32(kKmacBase + KMAC_KEY_SHARE1_0_REG_OFFSET + i * 4u, 0u);
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

  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_START << KMAC_CMD_CMD_OFFSET);
  wait_kmac_absorb();
  abs_mmio_write32(kKmacBase + KMAC_MSG_FIFO_REG_OFFSET, 0x00020100u);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_PROCESS << KMAC_CMD_CMD_OFFSET);
  while ((abs_mmio_read32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET) &
          (1u << KMAC_INTR_STATE_KMAC_ERR_BIT)) == 0u) {
  }
  CHECK_EQ(abs_mmio_read32(kKmacBase + KMAC_ERR_CODE_REG_OFFSET), 0x04000000u,
           "[ERRATA-KMAC-008] Expected ErrWaitTimerExpired (0x04000000)");
  wait_kmac_squeeze();
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_DONE << KMAC_CMD_CMD_OFFSET);
  wait_kmac_idle();
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   1u << KMAC_CMD_ERR_PROCESSED_BIT);
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, UINT32_MAX);

  // Now latch SwMode (2) from StRandReset and verify 5 words stall while the
  // 6th word of ENTROPY_SEED transitions kmac_entropy to StRandReady.
  uint32_t cfg_sw_entropy = 0;
  cfg_sw_entropy =
      bitfield_bit32_write(cfg_sw_entropy, KMAC_CFG_SHADOWED_KMAC_EN_BIT, true);
  cfg_sw_entropy =
      bitfield_field32_write(cfg_sw_entropy, KMAC_CFG_SHADOWED_MODE_FIELD,
                             KMAC_CFG_SHADOWED_MODE_VALUE_CSHAKE);
  cfg_sw_entropy =
      bitfield_field32_write(cfg_sw_entropy, KMAC_CFG_SHADOWED_KSTRENGTH_FIELD,
                             KMAC_CFG_SHADOWED_KSTRENGTH_VALUE_L256);
  cfg_sw_entropy = bitfield_field32_write(
      cfg_sw_entropy, KMAC_CFG_SHADOWED_ENTROPY_MODE_FIELD,
      KMAC_CFG_SHADOWED_ENTROPY_MODE_VALUE_SW_MODE);
  cfg_sw_entropy = bitfield_bit32_write(
      cfg_sw_entropy, KMAC_CFG_SHADOWED_ENTROPY_READY_BIT, true);
  write_cfg_shadowed(cfg_sw_entropy);

  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_START << KMAC_CMD_CMD_OFFSET);
  wait_kmac_absorb();
  abs_mmio_write32(kKmacBase + KMAC_MSG_FIFO_REG_OFFSET, 0x00020100u);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_PROCESS << KMAC_CMD_CMD_OFFSET);
  for (uint32_t i = 0; i < 5u; ++i) {
    abs_mmio_write32(kKmacBase + KMAC_ENTROPY_SEED_REG_OFFSET, 0x12345678u + i);
  }
  uint32_t status_5seeds = abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET);
  CHECK((status_5seeds & (1u << KMAC_STATUS_SHA3_SQUEEZE_BIT)) == 0u,
        "[ERRATA-KMAC-008] Expected stall after 5 ENTROPY_SEED words");
  abs_mmio_write32(kKmacBase + KMAC_ENTROPY_SEED_REG_OFFSET, 0x12345678u + 5u);
  wait_kmac_squeeze();
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_DONE << KMAC_CMD_CMD_OFFSET);
  wait_kmac_idle();

  // Verify [ERRATA-KMAC-007]: While in StRandReady (SwMode), writing
  // ENTROPY_MODE=EdnMode updates CFG_SHADOWED readback, but hardware ignores
  // the mode change (does not enter StRandEdn or timeout even with EDN0 off).
  write_cfg_shadowed(cfg_edn_timeout);
  uint32_t cfg_rb = abs_mmio_read32(kKmacBase + KMAC_CFG_SHADOWED_REG_OFFSET);
  CHECK_EQ(bitfield_field32_read(cfg_rb, KMAC_CFG_SHADOWED_ENTROPY_MODE_FIELD),
           KMAC_CFG_SHADOWED_ENTROPY_MODE_VALUE_EDN_MODE,
           "[ERRATA-KMAC-007] Expected CFG_SHADOWED readback to show EdnMode");
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
           "[ERRATA-KMAC-007] Expected mode_q to stay locked to SwMode without "
           "EDN timeout");
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_DONE << KMAC_CMD_CMD_OFFSET);
  wait_kmac_idle();
  abs_mmio_write32(kKmacBase + KMAC_ENTROPY_PERIOD_REG_OFFSET, 0u);
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, saved_edn0_ctrl);

  // =========================================================================
  // 4. [ERRATA-KMAC-003] (TRUE_SILICON_ERRATA):
  //    CFG_SHADOWED.KMAC_EN is evaluated independently of CFG_SHADOWED.MODE.
  //    In MODE=Sha3 (0) with KMAC_EN=1:
  //    (a) If PREFIX_0..1 != "KMAC", kmac_errchk raises
  //    ErrIncorrectFunctionName
  //        (0x07010000) even in SHA-3 mode.
  //    (b) If PREFIX_0..1 == "KMAC", kmac_core prepends the secret key block to
  //        the SHA-3 sponge, altering the SHA-3 digest compared to KMAC_EN=0.
  // =========================================================================
  LOG_INFO(
      "Verifying [ERRATA-KMAC-003] (TRUE_SILICON_ERRATA): KMAC_EN active "
      "when MODE == Sha3...");
  abs_mmio_write32(kKmacBase + KMAC_PREFIX_0_REG_OFFSET, 0u);
  uint32_t cfg_sha3_kmac1 = 0;
  cfg_sha3_kmac1 =
      bitfield_bit32_write(cfg_sha3_kmac1, KMAC_CFG_SHADOWED_KMAC_EN_BIT, true);
  cfg_sha3_kmac1 =
      bitfield_field32_write(cfg_sha3_kmac1, KMAC_CFG_SHADOWED_MODE_FIELD,
                             KMAC_CFG_SHADOWED_MODE_VALUE_SHA3);
  cfg_sha3_kmac1 =
      bitfield_field32_write(cfg_sha3_kmac1, KMAC_CFG_SHADOWED_KSTRENGTH_FIELD,
                             KMAC_CFG_SHADOWED_KSTRENGTH_VALUE_L256);
  cfg_sha3_kmac1 = bitfield_field32_write(
      cfg_sha3_kmac1, KMAC_CFG_SHADOWED_ENTROPY_MODE_FIELD,
      KMAC_CFG_SHADOWED_ENTROPY_MODE_VALUE_SW_MODE);
  cfg_sha3_kmac1 = bitfield_bit32_write(
      cfg_sha3_kmac1, KMAC_CFG_SHADOWED_ENTROPY_READY_BIT, true);
  write_cfg_shadowed(cfg_sha3_kmac1);

  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_START << KMAC_CMD_CMD_OFFSET);
  uint32_t err_code_003 = abs_mmio_read32(kKmacBase + KMAC_ERR_CODE_REG_OFFSET);
  CHECK_EQ(err_code_003, 0x07010000u,
           "[ERRATA-KMAC-003] Expected ErrIncorrectFunctionName (0x07010000) "
           "in Sha3 mode when KMAC_EN=1 and PREFIX != KMAC");
  wait_kmac_absorb();
  abs_mmio_write32(kKmacBase + KMAC_MSG_FIFO_REG_OFFSET, 0x64636261u);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_PROCESS << KMAC_CMD_CMD_OFFSET);
  wait_kmac_squeeze();
  uint32_t sha3_kmac1_w0 = read_unmasked_state_word(0);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_DONE << KMAC_CMD_CMD_OFFSET);
  wait_kmac_idle();
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   1u << KMAC_CMD_ERR_PROCESSED_BIT);
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, UINT32_MAX);

  // Compare against clean SHA3-256 with KMAC_EN=0 on the exact same message.
  uint32_t cfg_sha3_kmac0 = bitfield_bit32_write(
      cfg_sha3_kmac1, KMAC_CFG_SHADOWED_KMAC_EN_BIT, false);
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
  CHECK(sha3_kmac1_w0 != sha3_kmac0_w0,
        "[ERRATA-KMAC-003] Expected KMAC_EN=1 to prepend secret key block and "
        "alter Sha3 digest (got 0x%08x vs 0x%08x)",
        sha3_kmac1_w0, sha3_kmac0_w0);

  // =========================================================================
  // 5. [ERRATA-KMAC-004] (SPEC_DOC_ERRATA):
  //    Raw 44-byte PREFIX_0..10 absorption in cSHAKE absorbs stale upper
  //    PREFIX_10 bytes even when PREFIX_0 encodes empty N="" and S="".
  // =========================================================================
  LOG_INFO(
      "Verifying [ERRATA-KMAC-004] (SPEC_DOC_ERRATA): Raw 44-byte "
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
           "[ERRATA-KMAC-004] Expected golden cSHAKE w0=0xbb97943e");
  CHECK(cshake_stale_w0 != cshake_clean_w0,
        "[ERRATA-KMAC-004] Expected stale PREFIX_10 to alter cSHAKE digest "
        "(0x%08x vs 0x%08x)",
        cshake_stale_w0, cshake_clean_w0);

  // =========================================================================
  // 6. [ERRATA-KMAC-005a] & [ERRATA-KMAC-005b] (SPEC_DOC_ERRATA &
  //    TRUE_SILICON_ERRATA):
  //    ERR_CODE remains sticky across CMD.err_processed = 1, and
  //    CMD.err_processed = 1 does NOT abort active SW hashing in StSw.
  // =========================================================================
  LOG_INFO(
      "Verifying [ERRATA-KMAC-005a/b] (TRUE_SILICON_ERRATA): ERR_CODE "
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
           "[ERRATA-KMAC-005] Expected ERR_CODE 0x08040131 on CMD.RUN in "
           "StAbsorb");

  // Write CMD.err_processed = 1.
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   1u << KMAC_CMD_ERR_PROCESSED_BIT);
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, UINT32_MAX);

  // Verify ERR_CODE is still 0x08040131 (sticky) AND SHA3_ABSORB is still 1!
  CHECK_EQ(abs_mmio_read32(kKmacBase + KMAC_ERR_CODE_REG_OFFSET), 0x08040131u,
           "[ERRATA-KMAC-005a] Expected ERR_CODE to remain sticky across "
           "CMD.err_processed = 1");
  uint32_t status_005b = abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET);
  CHECK((status_005b & (1u << KMAC_STATUS_SHA3_ABSORB_BIT)) != 0u,
        "[ERRATA-KMAC-005b] Expected SHA3_ABSORB=1 to remain active after "
        "CMD.err_processed = 1 in StSw, got 0x%08x",
        status_005b);

  // Finish the active SW hash and verify digest matches clean SHA3-256.
  abs_mmio_write32(kKmacBase + KMAC_MSG_FIFO_REG_OFFSET, 0x64636261u);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_PROCESS << KMAC_CMD_CMD_OFFSET);
  wait_kmac_squeeze();
  CHECK_EQ(read_unmasked_state_word(0), sha3_kmac0_w0,
           "[ERRATA-KMAC-005b] Expected active SW hash to complete normally");

  // =========================================================================
  // 7. [ERRATA-KMAC-006a/b/c] (INTENDED_SECURITY_HARDENING):
  //    (a) STATE write -> Store Access Fault (mcause=7);
  //        MSG_FIFO read -> Load Access Fault (mcause=5).
  //    (b) KMAC_PERMIT sub-word CSR write fault (mcause=7) while sub-word read
  //        succeeds.
  //    (c) STATE sub-word byte read reflects word-first STATE_ENDIANNESS swap.
  // =========================================================================
  LOG_INFO(
      "Verifying [ERRATA-KMAC-006a/b/c] (INTENDED_SECURITY_HARDENING): "
      "TL-UL bus faults & STATE sub-word endianness...");
  uint8_t byte0_s0_le = abs_mmio_read8(kKmacBase + KMAC_STATE_REG_OFFSET);
  uint8_t byte0_s1_le =
      abs_mmio_read8(kKmacBase + KMAC_STATE_REG_OFFSET + 0x100u);
  CHECK_EQ((uint8_t)(byte0_s0_le ^ byte0_s1_le),
           (uint8_t)(sha3_kmac0_w0 & 0xffu),
           "[ERRATA-KMAC-006c] Expected unmasked lb[0] == w0[7:0] when "
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
           "[ERRATA-KMAC-006c] Expected unmasked lb[0] == w0[31:24] when "
           "STATE_ENDIANNESS=1");
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_DONE << KMAC_CMD_CMD_OFFSET);
  wait_kmac_idle();

  // (a) STATE write fault (mcause = 7).
  load_store_fault_seen = false;
  last_mcause = 0;
  abs_mmio_write32(kKmacBase + KMAC_STATE_REG_OFFSET, 0x12345678u);
  CHECK(load_store_fault_seen && last_mcause == 7u,
        "[ERRATA-KMAC-006a] Expected Store Access Fault (mcause=7) on STATE "
        "write, got fault=%d mcause=%u",
        load_store_fault_seen, last_mcause);

  // (a) MSG_FIFO read fault (mcause = 5).
  load_store_fault_seen = false;
  last_mcause = 0;
  (void)abs_mmio_read32(kKmacBase + KMAC_MSG_FIFO_REG_OFFSET);
  CHECK(load_store_fault_seen && last_mcause == 5u,
        "[ERRATA-KMAC-006a] Expected Load Access Fault (mcause=5) on MSG_FIFO "
        "read, got fault=%d mcause=%u",
        load_store_fault_seen, last_mcause);

  // (b) KMAC_PERMIT sub-word CSR write fault (mcause = 7) vs sub-word CSR read.
  abs_mmio_write32(kKmacBase + KMAC_PREFIX_0_REG_OFFSET, 0xa5a55a5au);
  load_store_fault_seen = false;
  uint8_t pfx_b0 = abs_mmio_read8(kKmacBase + KMAC_PREFIX_0_REG_OFFSET);
  CHECK(!load_store_fault_seen && pfx_b0 == 0x5au,
        "[ERRATA-KMAC-006b] Expected sub-word CSR read to succeed");

  load_store_fault_seen = false;
  last_mcause = 0;
  abs_mmio_write8(kKmacBase + KMAC_PREFIX_0_REG_OFFSET, 0xffu);
  CHECK(load_store_fault_seen && last_mcause == 7u,
        "[ERRATA-KMAC-006b] Expected Store Access Fault (mcause=7) on "
        "sub-word write to PREFIX_0");
  CHECK_EQ(abs_mmio_read32(kKmacBase + KMAC_PREFIX_0_REG_OFFSET), 0xa5a55a5au,
           "[ERRATA-KMAC-006b] Expected sub-word write to PREFIX_0 to be "
           "dropped");

  LOG_INFO("All KMAC errata & security hardening checks passed!");
  return true;
}
