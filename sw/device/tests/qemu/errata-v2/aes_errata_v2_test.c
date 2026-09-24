// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file aes_errata_v2_test.c
 * @brief CW340 FPGA Earlgrey v2 (`trunk-v2`) Errata Confirmation & Discovery
 * Test for `aes` (`P06`).
 *
 * Empirically verifies on physical CW340 FPGA silicon (`trunk-v2` bitstream):
 *
 * Part A — All 7 v1 Errata Confirmed Present on `trunk-v2`:
 * 1. `[aes_control_fsm.sv:1059-1063]` (`CONFIRMED_PRESENT_ON_V2`):
 *    `TRIGGER.DATA_OUT_CLEAR` sets `STATUS.OUTPUT_LOST = 1` whenever
 *    `STATUS.OUTPUT_VALID == 1` (`output_lost_we = ctrl_we_o | data_out_we_o`
 *    omits `& ~data_out_clear_we`), even when `MANUAL_OPERATION == 0`.
 * 2. `[aes_ctrl_reg_shadowed.sv:76-122]` (`CONFIRMED_PRESENT_ON_V2`):
 *    `CTRL_SHADOWED` pre-shadow sparse field sanitization resolves invalid
 *    `OPERATION`, `MODE` (`AES_NONE = 6'b11_1111 = 0x3f` in `trunk-v2`),
 *    `KEY_LEN`, and `PRNG_RESEED_RATE` encodings into `ctrl_wd = 0x14fd`
 *    before `prim_subreg_shadow`, so writing two distinct invalid values
 *    (`0x0000` then `0x00ff`) commits `0x14fd` without triggering
 *    `STATUS.ALERT_RECOV_CTRL_UPDATE_ERR`.
 * 3. `[aes_ctrl_reg_shadowed.sv:71-75 / aes_control_fsm.sv:1000]`
 *    (`CONFIRMED_PRESENT_ON_V2`): First (staged) write and mismatched
 *    update-error write to `CTRL_SHADOWED` immediately assert `qe_o` /
 *    `ctrl_we_o` (`clear_in_out_status = ctrl_we_q` omits `& ~ctrl_phase_i`),
 *    clearing `STATUS.OUTPUT_VALID`, `STATUS.OUTPUT_LOST`, and `DATA_IN`
 *    readiness tracking (`STATUS.INPUT_READY = 1`) before commit.
 * 4. `[aes_reg_status.sv:31-78]` (`CONFIRMED_PRESENT_ON_V2`):
 *    `aes_reg_status` arming (`armed_q = 1`) requires all 16 `KEY_SHARE` words
 *    even for `AES-128` / `AES-192` and resets the readiness bitmap to a single
 *    bit on any partial word write.
 * 5. `[aes_core.sv:285-290, 444-450]` (`CONFIRMED_PRESENT_ON_V2`):
 *    `DATA_IN_0..3` writes are never gated by `STATUS.IDLE` or
 *    `STATUS.INPUT_READY`; overwriting a single `DATA_IN_0` word while
 *    `STATUS.INPUT_READY == 0` updates the pending block in place without
 *    resetting `data_in_new_q`.
 * 6. `[aes_control_fsm.sv:337-360]` (`CONFIRMED_PRESENT_ON_V2`):
 *    While `TRIGGER.PRNG_RESEED` is pending (`STATUS.IDLE == 0`), writes to
 *    `KEY_SHARE`, `IV`, and `CTRL_SHADOWED` are silently dropped while writes
 *    to `DATA_IN_0..3` are accepted.
 * 7. `[aes_reg_pkg.sv:397-433 / aes_reg_top.sv:1515-1556]`
 *    (`CONFIRMED_PRESENT_ON_V2`): 3-tier `AES_PERMIT[35]` byte-enable masks
 *    (`4'b1111`, `4'b0011`, `4'b0001`) raise Store Access Fault (`mcause = 7`)
 *    on narrower sub-word writes.
 *
 * Part B — 2 Newly Discovered `trunk-v2` Errata (`NEW_IN_V2`):
 * 8. `[dif_aes.c:235-242, 309-326 / aes_ctrl_reg_shadowed.sv:94 /
 *    aes_ctrl_gcm_reg_shadowed.sv:154-196]` (`NEW_IN_V2`):
 *    On Earlgrey v2 (`AesAESGCMEnable = 0` in `top_earlgrey.sv:62`),
 *    `dif_aes_set_gcm_phase()` and `dif_aes_start(..., kDifAesModeGcm)` return
 *    `kDifOk` even though `aes_ctrl_reg_shadowed.sv:94` silently maps
 *    `kDifAesModeGcm` (`0x20`) to `AES_NONE` (`0x3f`) and
 *    `gen_no_ctrl_gcm_reg_shadowed` (`aes_ctrl_gcm_reg_shadowed.sv:175-184`)
 *    ties off `CTRL_GCM_SHADOWED` (`0x88`) to `0x00000401` (`GCM_INIT`,
 *    `NUM_VALID_BYTES = 16`) and ties off `err_update_o = 1'b0` so mismatched
 *    shadowed writes to `CTRL_GCM_SHADOWED` (`0x0002` then `0x07fe`) never
 *    trigger `STATUS.ALERT_RECOV_CTRL_UPDATE_ERR` (contradicting
 *    `aes.hjson:1045-1046`).
 * 9. `[aes_reg_pkg.sv:432 / aes_reg_top.sv:1554 / aes_core.sv:1056-1062]`
 *    (`NEW_IN_V2`): Even though `CTRL_GCM_SHADOWED` (`0x88`) is unimplemented /
 *    read-only tied off (`gen_no_ctrl_gcm_reg_shadowed`) on Earlgrey v2,
 *    `AES_PERMIT[34] = 4'b0011` enforces a 16-bit minimum write mask on offset
 *    `0x88`, causing 8-bit `sb` writes to `CTRL_GCM_SHADOWED.PHASE` (`0x88`) to
 *    raise a synchronous Store Access Fault (`mcause = 7`) while 16-bit `sh`
 *    and 32-bit `sw` writes succeed without fault and read back `0x00000401`.
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_aes.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top/aes_regs.h"
#include "hw/top/csrng_regs.h"
#include "hw/top/edn_regs.h"
#include "hw/top/entropy_src_regs.h"
#include "hw/top/rv_core_ibex_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG(.ignore_alerts = true);

enum {
  kAesBase = TOP_EARLGREY_AES_BASE_ADDR,
  kCsrngBase = TOP_EARLGREY_CSRNG_BASE_ADDR,
  kEdn0Base = TOP_EARLGREY_EDN0_BASE_ADDR,
  kEdn1Base = TOP_EARLGREY_EDN1_BASE_ADDR,
  kEntropySrcBase = TOP_EARLGREY_ENTROPY_SRC_BASE_ADDR,
  kIbexBase = TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR,
};

static volatile uint32_t g_fault_count = 0;
static volatile uint32_t g_last_mcause = 0;

void ottf_exception_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = ibex_mcause_read();
  g_last_mcause = mcause;
  g_fault_count++;
  abs_mmio_write32(kIbexBase + RV_CORE_IBEX_ERR_STATUS_REG_OFFSET, UINT32_MAX);
  uint32_t mepc = ibex_mepc_read();
  uint16_t inst16 = *(const volatile uint16_t *)mepc;
  ibex_mepc_write(mepc + (((inst16 & 0x3u) == 0x3u) ? 4u : 2u));
}

void ottf_internal_isr(uint32_t *exc_info) { ottf_exception_handler(exc_info); }

static void wait_aes_idle(void) {
  while ((abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET) &
          (1u << AES_STATUS_IDLE_BIT)) == 0u) {
  }
}

static void write_ctrl_shadowed(uint32_t val) {
  wait_aes_idle();
  abs_mmio_write32_shadowed(kAesBase + AES_CTRL_SHADOWED_REG_OFFSET, val);
  wait_aes_idle();
}

static uint32_t make_ctrl_val(uint32_t op, uint32_t mode, uint32_t key_len,
                              bool sideload, bool manual) {
  uint32_t ctrl = 0;
  ctrl = bitfield_field32_write(ctrl, AES_CTRL_SHADOWED_OPERATION_FIELD, op);
  ctrl = bitfield_field32_write(ctrl, AES_CTRL_SHADOWED_MODE_FIELD, mode);
  ctrl = bitfield_field32_write(ctrl, AES_CTRL_SHADOWED_KEY_LEN_FIELD, key_len);
  ctrl = bitfield_bit32_write(ctrl, AES_CTRL_SHADOWED_SIDELOAD_BIT, sideload);
  ctrl = bitfield_field32_write(ctrl, AES_CTRL_SHADOWED_PRNG_RESEED_RATE_FIELD,
                                AES_CTRL_SHADOWED_PRNG_RESEED_RATE_VALUE_PER_1);
  ctrl = bitfield_bit32_write(ctrl, AES_CTRL_SHADOWED_MANUAL_OPERATION_BIT,
                              manual);
  return ctrl;
}

static const uint32_t kKey128[8] = {
    0x16157e2bu, 0xa6d2ae28u, 0x8815f7abu, 0x3c4fcf09u, 0u, 0u, 0u, 0u,
};

static const uint32_t kPlaintext128[4] = {
    0xe2bec16bu,
    0x969f402eu,
    0x117e3de9u,
    0x2a179373u,
};

static const uint32_t kExpectedCipher128[4] = {
    0xb47bd73au,
    0x60367a0du,
    0xf3ca9ea8u,
    0x97ef6624u,
};

bool test_main(void) {
  LOG_INFO("Starting Earlgrey v2 AES Errata Confirmation & Discovery Suite...");

  // Ensure entropy complex is active for AES PRNG reseeding.
  uint32_t es_conf = (6u << ENTROPY_SRC_CONF_FIPS_ENABLE_OFFSET) |
                     (6u << ENTROPY_SRC_CONF_FIPS_FLAG_OFFSET) |
                     (9u << ENTROPY_SRC_CONF_RNG_FIPS_OFFSET) |
                     (9u << ENTROPY_SRC_CONF_RNG_BIT_ENABLE_OFFSET) |
                     (9u << ENTROPY_SRC_CONF_THRESHOLD_SCOPE_OFFSET) |
                     (9u << ENTROPY_SRC_CONF_ENTROPY_DATA_REG_ENABLE_OFFSET);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_CONF_REG_OFFSET, es_conf);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_HEALTH_TEST_WINDOWS_REG_OFFSET,
                   0x00100020u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_ENTROPY_CONTROL_REG_OFFSET,
                   0x99u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   0x6u);
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, 0x9666u);
  abs_mmio_write32(kEdn0Base + EDN_BOOT_INS_CMD_REG_OFFSET, 0x00000601u);
  abs_mmio_write32(kEdn0Base + EDN_BOOT_GEN_CMD_REG_OFFSET, 0x00040003u);
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9966u);

  // =========================================================================
  // Check 1: [aes_control_fsm.sv:1059-1063] (CONFIRMED_PRESENT_ON_V2)
  // TRIGGER.DATA_OUT_CLEAR sets STATUS.OUTPUT_LOST = 1 whenever
  // STATUS.OUTPUT_VALID == 1, even when MANUAL_OPERATION == 0.
  // =========================================================================
  LOG_INFO(
      "Check 1 [aes_control_fsm.sv:1059-1063]: TRIGGER.DATA_OUT_CLEAR sets "
      "STATUS.OUTPUT_LOST = 1 when OUTPUT_VALID == 1 (MANUAL_OPERATION=0)...");
  uint32_t ctrl_auto_ecb128 =
      make_ctrl_val(AES_CTRL_SHADOWED_OPERATION_VALUE_AES_ENC,
                    AES_CTRL_SHADOWED_MODE_VALUE_AES_ECB,
                    AES_CTRL_SHADOWED_KEY_LEN_VALUE_AES_128, /*sideload=*/false,
                    /*manual=*/false);
  write_ctrl_shadowed(ctrl_auto_ecb128);

  for (uint32_t i = 0; i < 8u; ++i) {
    abs_mmio_write32(kAesBase + AES_KEY_SHARE0_0_REG_OFFSET + i * 4u,
                     kKey128[i]);
    abs_mmio_write32(kAesBase + AES_KEY_SHARE1_0_REG_OFFSET + i * 4u, 0u);
  }
  wait_aes_idle();
  for (uint32_t i = 0; i < 4u; ++i) {
    abs_mmio_write32(kAesBase + AES_DATA_IN_0_REG_OFFSET + i * 4u,
                     kPlaintext128[i]);
  }
  while ((abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET) &
          (1u << AES_STATUS_OUTPUT_VALID_BIT)) == 0u) {
  }
  uint32_t st_before = abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK((st_before & (1u << AES_STATUS_OUTPUT_LOST_BIT)) == 0u,
        "Expected OUTPUT_LOST == 0 before DATA_OUT_CLEAR, got 0x%08x",
        st_before);

  abs_mmio_write32(kAesBase + AES_TRIGGER_REG_OFFSET,
                   1u << AES_TRIGGER_DATA_OUT_CLEAR_BIT);
  wait_aes_idle();

  uint32_t st_after = abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK((st_after & (1u << AES_STATUS_OUTPUT_VALID_BIT)) == 0u,
        "Expected OUTPUT_VALID == 0 after DATA_OUT_CLEAR, got 0x%08x",
        st_after);
  CHECK((st_after & (1u << AES_STATUS_OUTPUT_LOST_BIT)) != 0u,
        "[aes_control_fsm.sv:1059-1063] Expected OUTPUT_LOST == 1 after "
        "DATA_OUT_CLEAR on unread output in automatic mode, got 0x%08x",
        st_after);

  // =========================================================================
  // Check 2 & 3: [aes_ctrl_reg_shadowed.sv:71-122 / aes_control_fsm.sv:1000]
  // - Check 3: First (staged) write and update-error write to CTRL_SHADOWED
  //   immediately assert ctrl_we_o and clear STATUS.OUTPUT_VALID,
  //   STATUS.OUTPUT_LOST, and DATA_IN readiness tracking (INPUT_READY = 1).
  // - Check 2: Pre-shadow sparse sanitization maps invalid encodings (0x0000
  //   and 0x00ff) to ctrl_wd = 0x14fd (with AES_NONE = 6'b11_1111 = 0x3f in
  //   trunk-v2) before prim_subreg_shadow, suppressing update error!
  // =========================================================================
  LOG_INFO(
      "Check 3 [aes_ctrl_reg_shadowed.sv:71-75 / aes_control_fsm.sv:1000]: "
      "Staged 1st write and mismatched 2nd write to CTRL_SHADOWED clear "
      "STATUS...");
  // First, note OUTPUT_LOST is currently 1 from Check 1. Perform a single
  // (staged) write to CTRL_SHADOWED followed by a mismatched 2nd write:
  abs_mmio_write32(kAesBase + AES_CTRL_SHADOWED_REG_OFFSET, ctrl_auto_ecb128);
  uint32_t st_staged = abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK((st_staged & (1u << AES_STATUS_OUTPUT_LOST_BIT)) == 0u,
        "[aes_control_fsm.sv:1000] Expected staged 1st write to CTRL_SHADOWED "
        "to immediately clear OUTPUT_LOST, got 0x%08x",
        st_staged);
  // Trigger a shadow update error with a mismatched 2nd write:
  abs_mmio_write32(kAesBase + AES_CTRL_SHADOWED_REG_OFFSET,
                   ctrl_auto_ecb128 ^ (1u << AES_CTRL_SHADOWED_SIDELOAD_BIT));
  uint32_t st_err = abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK((st_err & (1u << AES_STATUS_ALERT_RECOV_CTRL_UPDATE_ERR_BIT)) != 0u,
        "Expected ALERT_RECOV_CTRL_UPDATE_ERR == 1 on mismatched valid write, "
        "got 0x%08x",
        st_err);

  LOG_INFO(
      "Check 2 [aes_ctrl_reg_shadowed.sv:76-122]: Two distinct invalid writes "
      "(0x0000 then 0x00ff) sanitize to 0x14fd before prim_subreg_shadow...");
  abs_mmio_write32(kAesBase + AES_CTRL_SHADOWED_REG_OFFSET, 0x0000u);
  abs_mmio_write32(kAesBase + AES_CTRL_SHADOWED_REG_OFFSET, 0x00ffu);
  uint32_t ctrl_sanitized =
      abs_mmio_read32(kAesBase + AES_CTRL_SHADOWED_REG_OFFSET);
  uint32_t st_sanitized = abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK(ctrl_sanitized == 0x14fdu,
        "[aes_ctrl_reg_shadowed.sv:76-122] Expected sanitized CTRL_SHADOWED "
        "== 0x14fd (AES_NONE=0x3f in trunk-v2), got 0x%08x",
        ctrl_sanitized);
  CHECK(
      (st_sanitized & (1u << AES_STATUS_ALERT_RECOV_CTRL_UPDATE_ERR_BIT)) == 0u,
      "[aes_ctrl_reg_shadowed.sv:76-122] Expected pre-shadow sanitization to "
      "suppress ALERT_RECOV_CTRL_UPDATE_ERR, got 0x%08x",
      st_sanitized);

  // =========================================================================
  // Check 4 & 5: [aes_reg_status.sv:31-78 / aes_core.sv:285-290]
  // - Check 4: aes_reg_status arming requires all 16 KEY_SHARE words even for
  //   AES-128 and resets bitmap on single-word write.
  // - Check 5: Overwriting a single DATA_IN_0 word while INPUT_READY == 0
  //   updates the plaintext block in place without resetting data_in_new_q.
  // =========================================================================
  LOG_INFO(
      "Check 4 & 5 [aes_reg_status.sv:31-78 / aes_core.sv:285-290]: "
      "16-word KEY_SHARE requirement in AES-128 & in-place DATA_IN_0 "
      "overwrite...");
  write_ctrl_shadowed(ctrl_auto_ecb128);
  // Write only the first 4 words of KEY_SHARE0/1 (8 registers total) + all 4
  // DATA_IN words (with bogus DATA_IN_0 = 0xffffffff):
  for (uint32_t i = 0; i < 4u; ++i) {
    abs_mmio_write32(kAesBase + AES_KEY_SHARE0_0_REG_OFFSET + i * 4u,
                     kKey128[i]);
    abs_mmio_write32(kAesBase + AES_KEY_SHARE1_0_REG_OFFSET + i * 4u, 0u);
  }
  abs_mmio_write32(kAesBase + AES_DATA_IN_0_REG_OFFSET, 0xffffffffu);
  for (uint32_t i = 1; i < 4u; ++i) {
    abs_mmio_write32(kAesBase + AES_DATA_IN_0_REG_OFFSET + i * 4u,
                     kPlaintext128[i]);
  }
  uint32_t st_8words = abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK((st_8words & (1u << AES_STATUS_OUTPUT_VALID_BIT)) == 0u,
        "[aes_reg_status.sv:31-78] Expected AES-128 to stall when only 8 of 16 "
        "KEY_SHARE registers are written, got 0x%08x",
        st_8words);
  CHECK((st_8words & (1u << AES_STATUS_INPUT_READY_BIT)) == 0u,
        "Expected INPUT_READY == 0 after writing 4 DATA_IN words, got 0x%08x",
        st_8words);

  // Overwrite ONLY DATA_IN_0 in place while INPUT_READY == 0, then finish
  // writing the remaining 8 KEY_SHARE words:
  abs_mmio_write32(kAesBase + AES_DATA_IN_0_REG_OFFSET, kPlaintext128[0]);
  for (uint32_t i = 4; i < 8u; ++i) {
    abs_mmio_write32(kAesBase + AES_KEY_SHARE0_0_REG_OFFSET + i * 4u,
                     kKey128[i]);
    abs_mmio_write32(kAesBase + AES_KEY_SHARE1_0_REG_OFFSET + i * 4u, 0u);
  }
  while ((abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET) &
          (1u << AES_STATUS_OUTPUT_VALID_BIT)) == 0u) {
  }
  for (uint32_t i = 0; i < 4u; ++i) {
    uint32_t ct =
        abs_mmio_read32(kAesBase + AES_DATA_OUT_0_REG_OFFSET + i * 4u);
    CHECK(ct == kExpectedCipher128[i],
          "[aes_core.sv:285-290] Ciphertext word %u mismatch: got 0x%08x, "
          "expected 0x%08x",
          i, ct, kExpectedCipher128[i]);
  }

  // =========================================================================
  // Check 6: [aes_control_fsm.sv:337-360] (CONFIRMED_PRESENT_ON_V2)
  // Pending TRIGGER.PRNG_RESEED (STATUS.IDLE == 0) drops KEY_SHARE, IV, and
  // CTRL_SHADOWED writes while accepting DATA_IN_0..3 writes.
  // =========================================================================
  LOG_INFO(
      "Check 6 [aes_control_fsm.sv:337-360]: Pending TRIGGER.PRNG_RESEED "
      "(STATUS.IDLE==0) drops KEY_SHARE/IV/CTRL_SHADOWED while accepting "
      "DATA_IN...");
  uint32_t ctrl_manual_cbc128 =
      make_ctrl_val(AES_CTRL_SHADOWED_OPERATION_VALUE_AES_ENC,
                    AES_CTRL_SHADOWED_MODE_VALUE_AES_CBC,
                    AES_CTRL_SHADOWED_KEY_LEN_VALUE_AES_128, /*sideload=*/false,
                    /*manual=*/true);
  write_ctrl_shadowed(ctrl_manual_cbc128);
  const uint32_t kKnownIv[4] = {0x10203040u, 0x50607080u, 0x90a0b0c0u,
                                0xd0e0f001u};
  for (uint32_t i = 0; i < 4u; ++i) {
    abs_mmio_write32(kAesBase + AES_IV_0_REG_OFFSET + i * 4u, kKnownIv[i]);
  }
  wait_aes_idle();

  // Temporarily pause EDN0/CSRNG/ENTROPY_SRC so PRNG_RESEED holds IDLE == 0:
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9999u);
  abs_mmio_write32(kEdn1Base + EDN_CTRL_REG_OFFSET, 0x9999u);
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, 0x9999u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   0x9u);
  while (abs_mmio_read32(kEntropySrcBase + ENTROPY_SRC_REGWEN_REG_OFFSET) ==
         0u) {
  }

  abs_mmio_write32(kAesBase + AES_TRIGGER_REG_OFFSET,
                   1u << AES_TRIGGER_PRNG_RESEED_BIT);
  uint32_t st_reseed = abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK((st_reseed & (1u << AES_STATUS_IDLE_BIT)) == 0u,
        "Expected STATUS.IDLE == 0 during stalled PRNG_RESEED, got 0x%08x",
        st_reseed);

  for (uint32_t i = 0; i < 4u; ++i) {
    abs_mmio_write32(kAesBase + AES_IV_0_REG_OFFSET + i * 4u, 0xdeadbeefu);
    abs_mmio_write32(kAesBase + AES_DATA_IN_0_REG_OFFSET + i * 4u,
                     kPlaintext128[i]);
  }
  abs_mmio_write32(kAesBase + AES_CTRL_SHADOWED_REG_OFFSET, ctrl_auto_ecb128);
  abs_mmio_write32(kAesBase + AES_CTRL_SHADOWED_REG_OFFSET, ctrl_auto_ecb128);

  // Re-enable entropy complex and wait for IDLE == 1:
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_CONF_REG_OFFSET, es_conf);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_HEALTH_TEST_WINDOWS_REG_OFFSET,
                   0x00100020u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_ENTROPY_CONTROL_REG_OFFSET,
                   0x99u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   0x6u);
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, 0x9666u);
  abs_mmio_write32(kEdn0Base + EDN_BOOT_INS_CMD_REG_OFFSET, 0x00000601u);
  abs_mmio_write32(kEdn0Base + EDN_BOOT_GEN_CMD_REG_OFFSET, 0x00040003u);
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9966u);
  wait_aes_idle();

  for (uint32_t i = 0; i < 4u; ++i) {
    CHECK(
        abs_mmio_read32(kAesBase + AES_IV_0_REG_OFFSET + i * 4u) == kKnownIv[i],
        "[aes_control_fsm.sv:337-360] IV_%u write while IDLE==0 must be "
        "ignored",
        i);
  }
  CHECK(abs_mmio_read32(kAesBase + AES_CTRL_SHADOWED_REG_OFFSET) ==
            ctrl_manual_cbc128,
        "[aes_control_fsm.sv:337-360] CTRL_SHADOWED write while IDLE==0 must "
        "be ignored");
  CHECK((abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET) &
         (1u << AES_STATUS_INPUT_READY_BIT)) == 0u,
        "[aes_control_fsm.sv:337-360] DATA_IN_0..3 writes while IDLE==0 were "
        "accepted (INPUT_READY == 0)");

  // =========================================================================
  // Check 7: [aes_reg_pkg.sv:397-433] (CONFIRMED_PRESENT_ON_V2)
  // 3-tier AES_PERMIT sub-word write faults (mcause = 7).
  // =========================================================================
  LOG_INFO(
      "Check 7 [aes_reg_pkg.sv:397-433]: 3-tier AES_PERMIT sub-word write "
      "faults (mcause=7)...");
  g_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write8(kAesBase + AES_CTRL_SHADOWED_REG_OFFSET, 0x01u);
  CHECK(g_fault_count == 1u && g_last_mcause == 7u,
        "[aes_reg_pkg.sv:397-433] Expected 8-bit write to CTRL_SHADOWED (0x74) "
        "to fault with mcause=7");

  g_fault_count = 0;
  g_last_mcause = 0;
  *(volatile uint16_t *)(kAesBase + AES_IV_0_REG_OFFSET) = 0x1234u;
  CHECK(g_fault_count == 1u && g_last_mcause == 7u,
        "[aes_reg_pkg.sv:397-433] Expected 16-bit write to IV_0 (0x40) to "
        "fault with mcause=7");

  // =========================================================================
  // Check 8 (NEW_IN_V2 #1): [dif_aes.c:235-242, 309-326 /
  // aes_ctrl_reg_shadowed.sv:94 / aes_ctrl_gcm_reg_shadowed.sv:154-196]
  // On Earlgrey v2 (`AesAESGCMEnable = 0` in `top_earlgrey.sv:62`):
  // - `dif_aes_set_gcm_phase()` returns `kDifOk`, but `CTRL_GCM_SHADOWED`
  //   (`0x88`) ignores writes and stays tied to `0x00000401` (`PHASE =
  //   GCM_INIT = 1`, `NUM_VALID_BYTES = 16`).
  // - Writing two mismatched values (`0x0002` then `0x07fe`) to
  //   `CTRL_GCM_SHADOWED` (`0x88`) does NOT trigger
  //   `STATUS.ALERT_RECOV_CTRL_UPDATE_ERR` (`err_update_o = 1'b0`),
  //   contradicting `aes.hjson:1045-1046`.
  // - `dif_aes_start()` with `mode = kDifAesModeGcm` (`0x20`) returns `kDifOk`,
  //   while `aes_ctrl_reg_shadowed.sv:94` silently maps `kDifAesModeGcm`
  //   (`0x20`) to `AES_NONE` (`0x3f`).
  // =========================================================================
  LOG_INFO(
      "Check 8 (NEW_IN_V2 #1) [dif_aes.c:235-242, 309-326 / "
      "aes_ctrl_gcm_reg_shadowed.sv:154-196]: dif_aes_set_gcm_phase & "
      "dif_aes_start(kDifAesModeGcm) return kDifOk while hardware maps "
      "AES_GCM->AES_NONE (0x3f) and suppresses CTRL_GCM_SHADOWED update "
      "alerts...");
  dif_aes_t aes;
  CHECK_DIF_OK(dif_aes_init(mmio_region_from_addr(kAesBase), &aes));

  CHECK_DIF_OK(dif_aes_set_gcm_phase(
      &aes, AES_CTRL_GCM_SHADOWED_PHASE_VALUE_GCM_AAD, /*num_valid_bytes=*/8));
  uint32_t gcm_reg =
      abs_mmio_read32(kAesBase + AES_CTRL_GCM_SHADOWED_REG_OFFSET);
  CHECK(gcm_reg == 0x00000401u,
        "[aes_ctrl_gcm_reg_shadowed.sv:175-176] Expected CTRL_GCM_SHADOWED "
        "readback to remain tied to 0x00000401 (GCM_INIT, 16 bytes) after "
        "dif_aes_set_gcm_phase(GCM_AAD, 8), got 0x%08x",
        gcm_reg);

  // Deliberately write two mismatched values to CTRL_GCM_SHADOWED (0x88):
  abs_mmio_write32(kAesBase + AES_CTRL_GCM_SHADOWED_REG_OFFSET, 0x0002u);
  abs_mmio_write32(kAesBase + AES_CTRL_GCM_SHADOWED_REG_OFFSET, 0x07feu);
  uint32_t st_gcm_mismatch = abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK((st_gcm_mismatch &
         (1u << AES_STATUS_ALERT_RECOV_CTRL_UPDATE_ERR_BIT)) == 0u,
        "[aes_ctrl_gcm_reg_shadowed.sv:180-184] Expected mismatched writes to "
        "CTRL_GCM_SHADOWED (0x88) to NOT set ALERT_RECOV_CTRL_UPDATE_ERR on "
        "Earlgrey v2 (err_update tied to 0), got 0x%08x",
        st_gcm_mismatch);

  // Call dif_aes_start with kDifAesModeGcm: returns kDifOk while hardware maps
  // MODE = 0x20 (AES_GCM) -> 0x3f (AES_NONE)!
  dif_aes_transaction_t gcm_tx = {
      .operation = kDifAesOperationEncrypt,
      .mode = kDifAesModeGcm,
      .key_len = kDifAesKey128,
      .key_provider = kDifAesKeySoftwareProvided,
      .mask_reseeding = kDifAesReseedPerBlock,
      .manual_operation = kDifAesManualOperationManual,
      .reseed_on_key_change = false,
      .ctrl_aux_lock = false,
  };
  dif_aes_key_share_t gcm_key = {.share0 = {0}, .share1 = {0}};
  dif_aes_iv_t gcm_iv = {.iv = {0}};
  CHECK_DIF_OK(dif_aes_start(&aes, &gcm_tx, &gcm_key, &gcm_iv));
  uint32_t ctrl_after_gcm_start =
      abs_mmio_read32(kAesBase + AES_CTRL_SHADOWED_REG_OFFSET);
  uint32_t mode_after_gcm_start =
      bitfield_field32_read(ctrl_after_gcm_start, AES_CTRL_SHADOWED_MODE_FIELD);
  CHECK(mode_after_gcm_start == AES_CTRL_SHADOWED_MODE_VALUE_AES_NONE,
        "[aes_ctrl_reg_shadowed.sv:94 / dif_aes.c:235-242] Expected "
        "dif_aes_start(kDifAesModeGcm) to return kDifOk while hardware maps "
        "CTRL_SHADOWED.MODE to AES_NONE (0x3f), got mode=0x%02x",
        mode_after_gcm_start);

  // =========================================================================
  // Check 9 (NEW_IN_V2 #2): [aes_reg_pkg.sv:432 / aes_reg_top.sv:1554]
  // Even though CTRL_GCM_SHADOWED (0x88) is tied off read-only in
  // gen_no_ctrl_gcm_reg_shadowed on Earlgrey v2, AES_PERMIT[34] = 4'b0011
  // enforces a 16-bit minimum write mask on offset 0x88:
  // - 8-bit `sb` write to CTRL_GCM_SHADOWED (0x88) raises Store Access Fault
  //   (mcause = 7).
  // - 16-bit `sh` write to CTRL_GCM_SHADOWED (0x88) succeeds without fault.
  // =========================================================================
  LOG_INFO(
      "Check 9 (NEW_IN_V2 #2) [aes_reg_pkg.sv:432 / aes_reg_top.sv:1554]: "
      "AES_PERMIT[34]=4'b0011 on CTRL_GCM_SHADOWED (0x88) faults on 8-bit sb "
      "write (mcause=7) while accepting 16-bit sh write...");
  g_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write8(kAesBase + AES_CTRL_GCM_SHADOWED_REG_OFFSET, 0x01u);
  CHECK(g_fault_count == 1u && g_last_mcause == 7u,
        "[aes_reg_pkg.sv:432] Expected 8-bit write to CTRL_GCM_SHADOWED (0x88) "
        "to fault with mcause=7, got faults=%u mcause=0x%08x",
        g_fault_count, g_last_mcause);

  g_fault_count = 0;
  *(volatile uint16_t *)(kAesBase + AES_CTRL_GCM_SHADOWED_REG_OFFSET) = 0x0401u;
  *(volatile uint16_t *)(kAesBase + AES_CTRL_GCM_SHADOWED_REG_OFFSET) = 0x0401u;
  CHECK(g_fault_count == 0u,
        "[aes_reg_pkg.sv:432] Expected 16-bit write to CTRL_GCM_SHADOWED "
        "(0x88) to succeed without fault, got faults=%u",
        g_fault_count);

  LOG_INFO(
      "All 9 Earlgrey v2 AES errata items (7 confirmed v1 + 2 new v2) "
      "verified on CW340 FPGA!");
  return true;
}
