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

OTTF_DEFINE_TEST_CONFIG(.ignore_alerts = true);

enum {
  kKmacBase = TOP_EARLGREY_KMAC_BASE_ADDR,
  kIbexBase = TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR,
  kEdn0Base = TOP_EARLGREY_EDN0_BASE_ADDR,
};

static volatile bool load_store_fault_seen = false;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  load_store_fault_seen = true;
  abs_mmio_write32(kIbexBase + RV_CORE_IBEX_ERR_STATUS_REG_OFFSET, UINT32_MAX);
}

void ottf_internal_isr(uint32_t *exc_info) {
  (void)exc_info;
  load_store_fault_seen = true;
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
  CHECK(share0 != 0u && share1 != 0u,
        "Expected non-zero masked shares in STATE[%u]: share0=0x%08x "
        "share1=0x%08x",
        word_idx, share0, share1);
  return share0 ^ share1;
}

bool test_main(void) {
  wait_kmac_idle();

  // 1. W/O register reads (KEY_LEN, KEY_SHARE0_0, ENTROPY_SEED), STATE read
  // while in IDLE, ENTROPY_REFRESH_THRESHOLD_SHADOWED read & shadow error, and
  // out-of-bounds MMIO read at 0xe4.
  CHECK(abs_mmio_read32(kKmacBase + KMAC_KEY_LEN_REG_OFFSET) == 0u,
        "Expected W/O KEY_LEN read to return 0");
  CHECK(abs_mmio_read32(kKmacBase + KMAC_KEY_SHARE0_0_REG_OFFSET) == 0u,
        "Expected W/O KEY_SHARE0_0 read to return 0");
  CHECK(abs_mmio_read32(kKmacBase + KMAC_ENTROPY_SEED_REG_OFFSET) == 0u,
        "Expected W/O ENTROPY_SEED read to return 0");
  CHECK(abs_mmio_read32(kKmacBase + KMAC_STATE_REG_OFFSET) == 0u,
        "Expected STATE read in IDLE to return 0");

  abs_mmio_write32_shadowed(
      kKmacBase + KMAC_ENTROPY_REFRESH_THRESHOLD_SHADOWED_REG_OFFSET, 0x15u);
  uint32_t thresh = abs_mmio_read32(
      kKmacBase + KMAC_ENTROPY_REFRESH_THRESHOLD_SHADOWED_REG_OFFSET);
  CHECK(thresh == 0x15u,
        "ENTROPY_REFRESH_THRESHOLD_SHADOWED read mismatch: got 0x%x", thresh);

  abs_mmio_write32(
      kKmacBase + KMAC_ENTROPY_REFRESH_THRESHOLD_SHADOWED_REG_OFFSET, 0x2au);
  abs_mmio_write32(
      kKmacBase + KMAC_ENTROPY_REFRESH_THRESHOLD_SHADOWED_REG_OFFSET, 0x15u);
  uint32_t status_after_shadow_err =
      abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET);
  CHECK((status_after_shadow_err &
         (1u << KMAC_STATUS_ALERT_RECOV_CTRL_UPDATE_ERR_BIT)) != 0u,
        "Expected ALERT_RECOV_CTRL_UPDATE_ERR after mismatched threshold write,"
        " got 0x%08x",
        status_after_shadow_err);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   1u << KMAC_CMD_ERR_PROCESSED_BIT);

  load_store_fault_seen = false;
  abs_mmio_write32(kIbexBase + RV_CORE_IBEX_ERR_STATUS_REG_OFFSET, UINT32_MAX);
  (void)abs_mmio_read32(kKmacBase + 0xe4u);
  icache_invalidate();
  uint32_t err_status =
      abs_mmio_read32(kIbexBase + RV_CORE_IBEX_ERR_STATUS_REG_OFFSET);
  if (err_status != 0u) {
    abs_mmio_write32(kIbexBase + RV_CORE_IBEX_ERR_STATUS_REG_OFFSET,
                     err_status);
  }
  CHECK(load_store_fault_seen || (err_status != 0u),
        "Expected bus fault on out-of-bounds KMAC register read at 0xe4");

  // 2. Invalid MODE=1 and KSTRENGTH=5 with EN_UNSUPPORTED_MODESTRENGTH=0.
  uint32_t cfg_bad_mode = 0;
  cfg_bad_mode =
      bitfield_field32_write(cfg_bad_mode, KMAC_CFG_SHADOWED_MODE_FIELD, 1u);
  cfg_bad_mode =
      bitfield_field32_write(cfg_bad_mode, KMAC_CFG_SHADOWED_KSTRENGTH_FIELD,
                             KMAC_CFG_SHADOWED_KSTRENGTH_VALUE_L256);
  write_cfg_shadowed(cfg_bad_mode);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_START << KMAC_CMD_CMD_OFFSET);
  uint32_t err_bad_mode = abs_mmio_read32(kKmacBase + KMAC_ERR_CODE_REG_OFFSET);
  CHECK(err_bad_mode == 0x06020012u,
        "Expected ERR_CODE 0x06020012 for MODE=1, got 0x%08x", err_bad_mode);
  CHECK((abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET) &
         (1u << KMAC_STATUS_SHA3_IDLE_BIT)) != 0u,
        "Expected KMAC to stay in IDLE when EN_UNSUPPORTED_MODESTRENGTH=0");
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   1u << KMAC_CMD_ERR_PROCESSED_BIT);

  uint32_t cfg_bad_strength = 0;
  cfg_bad_strength =
      bitfield_field32_write(cfg_bad_strength, KMAC_CFG_SHADOWED_MODE_FIELD,
                             KMAC_CFG_SHADOWED_MODE_VALUE_SHA3);
  cfg_bad_strength = bitfield_field32_write(
      cfg_bad_strength, KMAC_CFG_SHADOWED_KSTRENGTH_FIELD, 5u);
  write_cfg_shadowed(cfg_bad_strength);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_START << KMAC_CMD_CMD_OFFSET);
  uint32_t err_bad_strength =
      abs_mmio_read32(kKmacBase + KMAC_ERR_CODE_REG_OFFSET);
  CHECK(err_bad_strength == 0x06020005u,
        "Expected ERR_CODE 0x06020005 for KSTRENGTH=5, got 0x%08x",
        err_bad_strength);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   1u << KMAC_CMD_ERR_PROCESSED_BIT);

  // 3. EN_UNSUPPORTED_MODESTRENGTH=1 with MODE=1 (raw pad10*1, funcpad=0x01)
  // and KSTRENGTH=L256 (rate=136), plus ignored register writes while busy.
  uint32_t cfg_unsupp = cfg_bad_mode;
  cfg_unsupp = bitfield_bit32_write(
      cfg_unsupp, KMAC_CFG_SHADOWED_EN_UNSUPPORTED_MODESTRENGTH_BIT, true);
  write_cfg_shadowed(cfg_unsupp);
  abs_mmio_write32(kKmacBase + KMAC_PREFIX_0_REG_OFFSET, 0x12345678u);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_START << KMAC_CMD_CMD_OFFSET);
  wait_kmac_absorb();

  // Verify CFG_REGWEN == 0 and writes to PREFIX_0 / CFG_SHADOWED are ignored.
  CHECK(abs_mmio_read32(kKmacBase + KMAC_CFG_REGWEN_REG_OFFSET) == 0u,
        "Expected CFG_REGWEN == 0 during SHA3_ABSORB");
  abs_mmio_write32(kKmacBase + KMAC_PREFIX_0_REG_OFFSET, 0xdeadbeefu);
  abs_mmio_write32_shadowed(kKmacBase + KMAC_CFG_SHADOWED_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kKmacBase + KMAC_PREFIX_0_REG_OFFSET) == 0x12345678u,
        "Write to PREFIX_0 while busy must be ignored");

  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_PROCESS << KMAC_CMD_CMD_OFFSET);
  wait_kmac_squeeze();

  // Keccak-f[1600] of 136-byte block with 0x01 at byte 0 and 0x80 at byte 135:
  uint32_t mode1_w0 = read_unmasked_state_word(0);
  uint32_t mode1_w1 = read_unmasked_state_word(1);
  LOG_INFO("MODE=1 L256 empty msg digest w0=0x%08x w1=0x%08x", mode1_w0,
           mode1_w1);
  CHECK(mode1_w0 != 0u && mode1_w1 != 0u,
        "Expected non-zero digest in MODE=1 with EN_UNSUPPORTED_MODESTRENGTH");

  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   (KMAC_CMD_CMD_VALUE_DONE << KMAC_CMD_CMD_OFFSET));
  wait_kmac_idle();
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   1u << KMAC_CMD_ERR_PROCESSED_BIT);

  // 4. Raw CShake (MODE=3, KMAC_EN=0, KSTRENGTH=L256) with empty N="" and S=""
  // (PREFIX_0 = 0x00010001). Verify hardware still absorbs StPrefix and uses
  // funcpad=0x04.
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
  uint32_t cshake_empty_ns_w0 = read_unmasked_state_word(0);
  uint32_t cshake_empty_ns_w1 = read_unmasked_state_word(1);
  LOG_INFO("CShake empty N/S w0=0x%08x w1=0x%08x", cshake_empty_ns_w0,
           cshake_empty_ns_w1);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_DONE << KMAC_CMD_CMD_OFFSET);
  wait_kmac_idle();

  // 4b. ENTROPY_MODE=EdnMode (1) with EDN0 disabled and ENTROPY_PERIOD
  // WAIT_TIMER=2000: verify KMAC stalls in KMAC_ST_PROCESSING until the wait
  // timer expires, raising ErrWaitTimerExpired (0x04000000) and unblocking
  // StProcessing -> StAbsorbed (kmac_entropy.sv:702-703, ot_kmac.c:564), and
  // verify CMD.ERR_PROCESSED resets StRandErr -> StRandReset so SwMode can be
  // latched next (kmac_entropy.sv:710).
  uint32_t saved_edn0_ctrl = abs_mmio_read32(kEdn0Base + EDN_CTRL_REG_OFFSET);
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9999u);
  abs_mmio_write32(kKmacBase + KMAC_ENTROPY_PERIOD_REG_OFFSET,
                   2000u << KMAC_ENTROPY_PERIOD_WAIT_TIMER_OFFSET);
  abs_mmio_write32(kKmacBase + KMAC_PREFIX_0_REG_OFFSET, 0x4d4b2001u);
  abs_mmio_write32(kKmacBase + KMAC_PREFIX_1_REG_OFFSET, 0x00014341u);
  abs_mmio_write32(kKmacBase + KMAC_KEY_LEN_REG_OFFSET,
                   KMAC_KEY_LEN_LEN_VALUE_KEY128);
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, UINT32_MAX);

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

  uint32_t edn_proc_status =
      abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET);
  CHECK((edn_proc_status & (1u << KMAC_STATUS_SHA3_ABSORB_BIT)) != 0u &&
            (edn_proc_status & (1u << KMAC_STATUS_SHA3_SQUEEZE_BIT)) == 0u,
        "Expected KMAC to be stalled in PROCESSING waiting for EDN timer, got "
        "0x%08x",
        edn_proc_status);

  while ((abs_mmio_read32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET) &
          (1u << KMAC_INTR_STATE_KMAC_ERR_BIT)) == 0u) {
  }
  uint32_t err_edn_timeout =
      abs_mmio_read32(kKmacBase + KMAC_ERR_CODE_REG_OFFSET);
  CHECK(err_edn_timeout == 0x04000000u,
        "Expected ERR_CODE 0x04000000 (ErrWaitTimerExpired), got 0x%08x",
        err_edn_timeout);
  wait_kmac_squeeze();

  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_DONE << KMAC_CMD_CMD_OFFSET);
  wait_kmac_idle();
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   1u << KMAC_CMD_ERR_PROCESSED_BIT);
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, UINT32_MAX);
  abs_mmio_write32(kKmacBase + KMAC_ENTROPY_PERIOD_REG_OFFSET, 0u);
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, saved_edn0_ctrl);

  // 5. ENTROPY_MODE=SwMode (2) stalling in KMAC_ST_PROCESSING, ErrSwCmdSequence
  // in KMAC_ST_PROCESSING, 5-word ENTROPY_SEED unblock, and invalid KEY_LEN=5.
  abs_mmio_write32(kKmacBase + KMAC_PREFIX_0_REG_OFFSET, 0x4b412001u);
  abs_mmio_write32(kKmacBase + KMAC_PREFIX_1_REG_OFFSET, 0x0001434du);
  abs_mmio_write32(kKmacBase + KMAC_KEY_LEN_REG_OFFSET, 5u);
  for (uint32_t i = 0; i < 16u; ++i) {
    abs_mmio_write32(kKmacBase + KMAC_KEY_SHARE0_0_REG_OFFSET + i * 4u,
                     0x55555555u);
    abs_mmio_write32(kKmacBase + KMAC_KEY_SHARE1_0_REG_OFFSET + i * 4u, 0u);
  }
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

  // Verify SHA3_ABSORB remains 1 and SHA3_SQUEEZE is 0 while stalled in
  // StProcessing waiting for 5-word ENTROPY_SEED (sha3.sv:265, kmac.sv:465).
  uint32_t proc_status = abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET);
  CHECK((proc_status & (1u << KMAC_STATUS_SHA3_ABSORB_BIT)) != 0u &&
            (proc_status & (1u << KMAC_STATUS_SHA3_SQUEEZE_BIT)) == 0u,
        "Expected SHA3_ABSORB=1 and SHA3_SQUEEZE=0 in PROCESSING before "
        "ENTROPY_SEED, got 0x%08x",
        proc_status);

  // Issue CMD.START while in KMAC_ST_PROCESSING -> ErrSwCmdSequence
  // (0x0804021d).
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_START << KMAC_CMD_CMD_OFFSET);
  uint32_t err_in_proc = abs_mmio_read32(kKmacBase + KMAC_ERR_CODE_REG_OFFSET);
  CHECK(err_in_proc == 0x0804021du,
        "Expected ERR_CODE 0x0804021d for CMD.START in PROCESSING, got 0x%08x",
        err_in_proc);

  // Write 5 words to ENTROPY_SEED; verify KMAC remains stalled in PROCESSING
  // because BiviumStateWidth=177 / PartialSeedWidth=32 requires 6 words.
  for (uint32_t i = 0; i < 5u; ++i) {
    abs_mmio_write32(kKmacBase + KMAC_ENTROPY_SEED_REG_OFFSET, 0x12345678u + i);
  }
  uint32_t status_after_5_seeds =
      abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET);
  CHECK((status_after_5_seeds & (1u << KMAC_STATUS_SHA3_SQUEEZE_BIT)) == 0u &&
            (status_after_5_seeds & (1u << KMAC_STATUS_SHA3_ABSORB_BIT)) != 0u,
        "Expected KMAC to remain in PROCESSING after only 5 ENTROPY_SEED "
        "words, got 0x%08x",
        status_after_5_seeds);

  // Write the 6th word to ENTROPY_SEED to unblock PROCESSING -> SQUEEZE.
  abs_mmio_write32(kKmacBase + KMAC_ENTROPY_SEED_REG_OFFSET, 0x12345678u + 5u);
  wait_kmac_squeeze();
  uint32_t keylen5_w0 = read_unmasked_state_word(0);
  uint32_t keylen5_w1 = read_unmasked_state_word(1);
  LOG_INFO("KEY_LEN=5 w0=0x%08x w1=0x%08x", keylen5_w0, keylen5_w1);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   KMAC_CMD_CMD_VALUE_DONE << KMAC_CMD_CMD_OFFSET);
  wait_kmac_idle();

  // Verify exact golden digests across CW340 FPGA and QEMU.
  CHECK(mode1_w0 == 0x0146d2c5u && mode1_w1 == 0x3c23f786u,
        "MODE=1 L256 digest mismatch: got 0x%08x 0x%08x", mode1_w0, mode1_w1);
  CHECK(cshake_empty_ns_w0 == 0xbb97943eu && cshake_empty_ns_w1 == 0xb3e58d57u,
        "CShake empty N/S digest mismatch: got 0x%08x 0x%08x",
        cshake_empty_ns_w0, cshake_empty_ns_w1);
  CHECK(keylen5_w0 == 0x85cc0469u && keylen5_w1 == 0x707e1850u,
        "KEY_LEN=5 digest mismatch: got 0x%08x 0x%08x", keylen5_w0, keylen5_w1);

  return true;
}
