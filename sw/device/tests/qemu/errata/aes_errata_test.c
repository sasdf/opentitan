// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file aes_errata_test.c
 * @brief CW340 FPGA & QEMU Errata Confirmation Test for Earlgrey `aes` (`P20`).
 *
 * Empirically verifies all 7 documented hardware errata, specification errata,
 * and security hardening behaviors in `/root/knowledge/errata/aes.md` and
 * `/root/knowledge/errata/aes_ctrl_shadowed_and_trigger_quirks.md` on both
 * physical CW340 FPGA silicon (`fpga_cw340_rom_with_fake_keys`) and QEMU
 * (`sim_qemu_rom_with_fake_keys`):
 *
 * - `[aes_control_fsm.sv:762]` (`TRUE_SILICON_ERRATA`):
 * `TRIGGER.DATA_OUT_CLEAR` sets `STATUS.OUTPUT_LOST = 1` whenever
 * `STATUS.OUTPUT_VALID == 1`
 * (`aes_control_fsm.sv:757-762` `output_lost_we = ctrl_we_o | data_out_we_o`
 * omits `& ~data_out_clear_we`), even when `MANUAL_OPERATION == 0`.
 * - `[aes_ctrl_reg_shadowed.sv:75-120]` (`INTENDED_SECURITY_HARDENING`):
 * `CTRL_SHADOWED` pre-shadow sparse field sanitization
 * (`aes_ctrl_reg_shadowed.sv:75-120`) resolves invalid `OPERATION`, `MODE`,
 * `KEY_LEN`, and `PRNG_RESEED_RATE` encodings into `ctrl_wd = 0x1481` before
 * `prim_subreg_shadow`, so writing two distinct invalid values (`0x0000` then
 * `0x00ff`) commits `0x1481` without triggering
 * `STATUS.ALERT_RECOV_CTRL_UPDATE_ERR`.
 * - `[aes_ctrl_reg_shadowed.sv:70-74]` (`TRUE_SILICON_ERRATA` /
 * `INTENDED_SECURITY_HARDENING`): The first (staged) write and update-error
 * write to `CTRL_SHADOWED` immediately assert `qe_o` / `ctrl_we_o`
 * (`aes_ctrl_reg_shadowed.sv:70-74`, `aes_control_fsm.sv:338-343, 698-762`),
 * clearing `STATUS.OUTPUT_VALID`, `STATUS.OUTPUT_LOST`, and `DATA_IN` tracking
 * (`STATUS.INPUT_READY = 1`) before the shadow register commits.
 * - `[aes_reg_status.sv:31-78]` (`INTENDED_SECURITY_HARDENING`):
 * `aes_reg_status` arming (`armed_q = 1`, `aes_reg_status.sv:31-78`) requires
 * all 16 `KEY_SHARE` words (`8` per share, even in `AES-128` / `AES-192` modes)
 *   and resets the readiness bitmap to a single bit on any partial word write.
 * - `[aes_core.sv:280-285]` (`E2` — In-Place `DATA_IN_0..3` Single-Word
 * Overwrite Before `CTRL_LOAD`): `DATA_IN_0..3` writes are never gated by
 * `STATUS.IDLE` or `STATUS.INPUT_READY`; overwriting a single `DATA_IN_0` word
 * while `STATUS.INPUT_READY == 0` updates the pending block in place without
 *   resetting `data_in_new_q`.
 * - `[aes_control_fsm.sv:317-335]` (`SPEC_DOC_ERRATA`): While
 * `TRIGGER.PRNG_RESEED` is pending (`STATUS.IDLE == 0`),
 * `aes_control_fsm.sv:317-335` gates off `KEY_SHARE`, `IV`, and `CTRL_SHADOWED`
 * writes (silently dropping them) while accepting `DATA_IN_0..3` writes.
 * - `[aes_reg_pkg.sv:377-412]` (`INTENDED_SECURITY_HARDENING`): 3-tier
 * `AES_PERMIT[34]` byte-enable masks (`4'b1111` on
 * `KEY_SHARE`/`IV`/`DATA_IN`/`DATA_OUT`, `4'b0011` on `CTRL_SHADOWED`,
 * `4'b0001` on `CTRL_AUX_SHADOWED`/`TRIGGER`) raise synchronous Store Access
 * Fault (`mcause = 7`) on narrower sub-word writes.
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "aes_regs.h"
#include "csrng_regs.h"
#include "edn_regs.h"
#include "entropy_src_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "rv_core_ibex_regs.h"

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

// NIST FIPS-197 Appendix C.1 AES-128 ECB test vector:
// Key       = 00010203 04050607 08090a0b 0c0d0e0f
// Plaintext = 00112233 44556677 8899aabb ccddeeff
// Cipher    = 69c4e0d8 6a7b0430 d8cdb780 70b4c55a
static const uint32_t kKey128[8] = {
    0x03020100u, 0x07060504u, 0x0b0a0908u, 0x0f0e0d0cu,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
};
static const uint32_t kPlaintext128[4] = {
    0x33221100u,
    0x77665544u,
    0xbbaa9988u,
    0xffeeddccu,
};
static const uint32_t kExpectedCipher128[4] = {
    0xd8e0c469u,
    0x30047b6au,
    0x80b7cdd8u,
    0x5ac5b470u,
};

bool test_main(void) {
  LOG_INFO("Starting AES CW340/QEMU Errata Confirmation Test (P20)...");
  wait_aes_idle();

  // =========================================================================
  // Check 1: [aes_control_fsm.sv:762] (TRUE_SILICON_ERRATA)
  // `TRIGGER.DATA_OUT_CLEAR` sets `STATUS.OUTPUT_LOST = 1` whenever
  // `STATUS.OUTPUT_VALID == 1`, even when `MANUAL_OPERATION == 0`.
  // =========================================================================
  LOG_INFO(
      "Verifying [aes_control_fsm.sv:762] (TRUE_SILICON_ERRATA): "
      "TRIGGER.DATA_OUT_CLEAR sets STATUS.OUTPUT_LOST=1 when OUTPUT_VALID==1 "
      "even with MANUAL_OPERATION=0...");
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
  uint32_t status_before_clear =
      abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK((status_before_clear & (1u << AES_STATUS_OUTPUT_LOST_BIT)) == 0u,
        "[aes_control_fsm.sv:762] Expected OUTPUT_LOST == 0 before "
        "DATA_OUT_CLEAR");

  // Issue TRIGGER.DATA_OUT_CLEAR while OUTPUT_VALID == 1 without reading
  // DATA_OUT_0..3.
  abs_mmio_write32(kAesBase + AES_TRIGGER_REG_OFFSET,
                   1u << AES_TRIGGER_DATA_OUT_CLEAR_BIT);
  wait_aes_idle();
  uint32_t status_after_clear =
      abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK((status_after_clear & (1u << AES_STATUS_OUTPUT_VALID_BIT)) == 0u,
        "[aes_control_fsm.sv:762] Expected OUTPUT_VALID == 0 after "
        "DATA_OUT_CLEAR, "
        "got 0x%08x",
        status_after_clear);
  CHECK((status_after_clear & (1u << AES_STATUS_OUTPUT_LOST_BIT)) != 0u,
        "[aes_control_fsm.sv:762] Expected OUTPUT_LOST == 1 after "
        "DATA_OUT_CLEAR while "
        "OUTPUT_VALID == 1 in MANUAL_OPERATION=0 mode, got 0x%08x",
        status_after_clear);

  // =========================================================================
  // Check 2: [aes_ctrl_reg_shadowed.sv:70-74] (TRUE_SILICON_ERRATA /
  // INTENDED_SECURITY_HARDENING) First (staged) write and update-error write to
  // `CTRL_SHADOWED` immediately assert `ctrl_we_o`, clearing
  // `STATUS.OUTPUT_LOST` and `STATUS.OUTPUT_VALID` before the shadow register
  // commits.
  // =========================================================================
  LOG_INFO(
      "Verifying [aes_ctrl_reg_shadowed.sv:70-74] (TRUE_SILICON_ERRATA): "
      "First (staged) write and update-error write to CTRL_SHADOWED "
      "immediately clear STATUS.OUTPUT_LOST and STATUS.OUTPUT_VALID...");
  // Issue ONLY the 1st (staged) write to CTRL_SHADOWED while OUTPUT_LOST == 1:
  abs_mmio_write32(kAesBase + AES_CTRL_SHADOWED_REG_OFFSET, ctrl_auto_ecb128);
  wait_aes_idle();
  uint32_t status_after_staged_ctrl =
      abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK((status_after_staged_ctrl & (1u << AES_STATUS_OUTPUT_LOST_BIT)) == 0u,
        "[aes_ctrl_reg_shadowed.sv:70-74] Expected 1st (staged) CTRL_SHADOWED "
        "write to clear "
        "OUTPUT_LOST immediately, got 0x%08x",
        status_after_staged_ctrl);
  // Complete the 2nd matching write to commit CTRL_SHADOWED:
  abs_mmio_write32(kAesBase + AES_CTRL_SHADOWED_REG_OFFSET, ctrl_auto_ecb128);
  wait_aes_idle();

  // Now produce a valid output block (`OUTPUT_VALID == 1`) and verify that a
  // 1st staged write followed by a mismatched 2nd write (`err_update = 1`)
  // immediately clears `OUTPUT_VALID` and raises `ALERT_RECOV_CTRL_UPDATE_ERR`:
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
  // 1st staged write:
  abs_mmio_write32(kAesBase + AES_CTRL_SHADOWED_REG_OFFSET, ctrl_auto_ecb128);
  wait_aes_idle();
  uint32_t status_staged_ov = abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK((status_staged_ov & (1u << AES_STATUS_OUTPUT_VALID_BIT)) == 0u,
        "[aes_ctrl_reg_shadowed.sv:70-74] Expected 1st (staged) CTRL_SHADOWED "
        "write to clear "
        "OUTPUT_VALID immediately, got 0x%08x",
        status_staged_ov);
  // Mismatched 2nd write -> shadow update error:
  abs_mmio_write32(
      kAesBase + AES_CTRL_SHADOWED_REG_OFFSET,
      ctrl_auto_ecb128 ^ (1u << AES_CTRL_SHADOWED_MANUAL_OPERATION_BIT));
  wait_aes_idle();
  uint32_t status_err_update =
      abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK((status_err_update &
         (1u << AES_STATUS_ALERT_RECOV_CTRL_UPDATE_ERR_BIT)) != 0u,
        "[aes_ctrl_reg_shadowed.sv:70-74] Expected ALERT_RECOV_CTRL_UPDATE_ERR "
        "== 1 after "
        "mismatched 2nd write, got 0x%08x",
        status_err_update);

  // =========================================================================
  // Check 3: [aes_ctrl_reg_shadowed.sv:75-120] (INTENDED_SECURITY_HARDENING)
  // `CTRL_SHADOWED` pre-shadow sparse sanitization maps two distinct invalid
  // encodings (`0x0000` on Phase 1 and `0x00ff` on Phase 2) to `0x1481` before
  // `prim_subreg_shadow`, committing `0x1481` without raising
  // `ALERT_RECOV_CTRL_UPDATE_ERR`.
  // =========================================================================
  LOG_INFO(
      "Verifying [aes_ctrl_reg_shadowed.sv:75-120] "
      "(INTENDED_SECURITY_HARDENING): "
      "Pre-shadow sparse sanitization commits 0x1481 on mismatched invalid "
      "writes (0x0000 then 0x00ff) without shadow update error...");
  abs_mmio_write32(kAesBase + AES_CTRL_SHADOWED_REG_OFFSET, 0x0000u);
  abs_mmio_write32(kAesBase + AES_CTRL_SHADOWED_REG_OFFSET, 0x00ffu);
  wait_aes_idle();
  uint32_t sanitized_ctrl =
      abs_mmio_read32(kAesBase + AES_CTRL_SHADOWED_REG_OFFSET);
  uint32_t status_sanitized = abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK(sanitized_ctrl == 0x1481u,
        "[aes_ctrl_reg_shadowed.sv:75-120] Expected sanitized CTRL_SHADOWED == "
        "0x1481, got "
        "0x%08x",
        sanitized_ctrl);
  CHECK((status_sanitized &
         (1u << AES_STATUS_ALERT_RECOV_CTRL_UPDATE_ERR_BIT)) == 0u,
        "[aes_ctrl_reg_shadowed.sv:75-120] Expected "
        "ALERT_RECOV_CTRL_UPDATE_ERR == 0 after "
        "pre-shadow sanitized writes, got 0x%08x",
        status_sanitized);

  // =========================================================================
  // Check 4 & Check 5: [aes_reg_status.sv:31-78] & [aes_core.sv:280-285]
  // - [aes_reg_status.sv:31-78]: `aes_reg_status` arming forces all 16
  // `KEY_SHARE` words
  //   (`8` per share, even in `AES-128` mode) to be written and resets the
  //   readiness bitmap on any single-word write.
  // - [aes_core.sv:280-285]: Overwriting a single `DATA_IN_0` word while
  //   `STATUS.INPUT_READY == 0` (before `CTRL_LOAD`) updates the plaintext
  //   block in place without resetting `data_in_new_q`.
  // =========================================================================
  LOG_INFO(
      "Verifying [aes_reg_status.sv:31-78] (INTENDED_SECURITY_HARDENING) & "
      "[aes_core.sv:280-285] (IN-PLACE DATA_IN OVERWRITE): "
      "Armed 16-word KEY_SHARE tracking & single-word DATA_IN_0 overwrite...");
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
  for (uint32_t i = 0; i < 4u; ++i) {
    uint32_t ct =
        abs_mmio_read32(kAesBase + AES_DATA_OUT_0_REG_OFFSET + i * 4u);
    CHECK(ct == kExpectedCipher128[i],
          "[aes_reg_status.sv:31-78] Initial AES-128 block word %u mismatch: "
          "got 0x%08x",
          i, ct);
  }

  // `u_reg_status_key_init` is now armed (`armed_q = 1`).
  // Touch ONLY `KEY_SHARE0_0` (1 word) and write all 4 `DATA_IN_0..3` words
  // (with a bogus `DATA_IN_0 = 0xdeadbeef`):
  abs_mmio_write32(kAesBase + AES_KEY_SHARE0_0_REG_OFFSET, kKey128[0]);
  abs_mmio_write32(kAesBase + AES_DATA_IN_0_REG_OFFSET, 0xdeadbeefu);
  for (uint32_t i = 1; i < 4u; ++i) {
    abs_mmio_write32(kAesBase + AES_DATA_IN_0_REG_OFFSET + i * 4u,
                     kPlaintext128[i]);
  }
  wait_aes_idle();
  uint32_t st_partial_key = abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK((st_partial_key & (1u << AES_STATUS_OUTPUT_VALID_BIT)) == 0u,
        "[aes_reg_status.sv:31-78] Expected OUTPUT_VALID == 0 after 1-word "
        "KEY_SHARE0_0 "
        "write, got 0x%08x",
        st_partial_key);
  CHECK((st_partial_key & (1u << AES_STATUS_INPUT_READY_BIT)) == 0u,
        "[aes_core.sv:280-285] Expected INPUT_READY == 0 while 4 DATA_IN words "
        "are "
        "buffered waiting for KEY_SHARE, got 0x%08x",
        st_partial_key);

  // [aes_core.sv:280-285]: While `STATUS.INPUT_READY == 0`, overwrite ONLY
  // `DATA_IN_0` with the true `kPlaintext128[0]` (without rewriting
  // `DATA_IN_1..3`).
  abs_mmio_write32(kAesBase + AES_DATA_IN_0_REG_OFFSET, kPlaintext128[0]);

  // [aes_reg_status.sv:31-78]: Write `KEY_SHARE0_1..7` and only the lower 4
  // words `KEY_SHARE1_0..3` (enough for a 128-bit key, but omitting upper words
  // `KEY_SHARE1_4..7`). Verify automatic encryption STILL does not start
  // because `u_reg_status_key_init` has `Width = 16` independent of `KEY_LEN`!
  for (uint32_t i = 1; i < 8u; ++i) {
    abs_mmio_write32(kAesBase + AES_KEY_SHARE0_0_REG_OFFSET + i * 4u,
                     kKey128[i]);
  }
  for (uint32_t i = 0; i < 4u; ++i) {
    abs_mmio_write32(kAesBase + AES_KEY_SHARE1_0_REG_OFFSET + i * 4u, 0u);
  }
  wait_aes_idle();
  uint32_t st_12words = abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK((st_12words & (1u << AES_STATUS_OUTPUT_VALID_BIT)) == 0u,
        "[aes_reg_status.sv:31-78] Expected OUTPUT_VALID == 0 when "
        "KEY_SHARE1_4..7 are "
        "unwritten in AES-128 mode, got 0x%08x",
        st_12words);

  // Now write the remaining 4 upper words `KEY_SHARE1_4..7`:
  for (uint32_t i = 4; i < 8u; ++i) {
    abs_mmio_write32(kAesBase + AES_KEY_SHARE1_0_REG_OFFSET + i * 4u, 0u);
  }
  while ((abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET) &
          (1u << AES_STATUS_OUTPUT_VALID_BIT)) == 0u) {
  }
  for (uint32_t i = 0; i < 4u; ++i) {
    uint32_t ct =
        abs_mmio_read32(kAesBase + AES_DATA_OUT_0_REG_OFFSET + i * 4u);
    CHECK(ct == kExpectedCipher128[i],
          "[aes_reg_status.sv:31-78 / aes_core.sv:280-285] Post-16-word key & "
          "in-place DATA_IN_0 "
          "overwrite word %u mismatch: got 0x%08x",
          i, ct);
  }

  // =========================================================================
  // Check 6: [aes_control_fsm.sv:317-335] (SPEC_DOC_ERRATA)
  // While `TRIGGER.PRNG_RESEED` is pending (`STATUS.IDLE == 0`), writes to
  // `KEY_SHARE`, `IV`, and `CTRL_SHADOWED` are silently dropped, whereas writes
  // to `DATA_IN_0..3` are accepted.
  // =========================================================================
  LOG_INFO(
      "Verifying [aes_control_fsm.sv:317-335] (SPEC_DOC_ERRATA): "
      "Pending TRIGGER.PRNG_RESEED (STATUS.IDLE==0) drops KEY_SHARE, IV, and "
      "CTRL_SHADOWED writes while accepting DATA_IN_0..3 writes...");
  uint32_t ctrl_manual_cbc128 =
      make_ctrl_val(AES_CTRL_SHADOWED_OPERATION_VALUE_AES_ENC,
                    AES_CTRL_SHADOWED_MODE_VALUE_AES_CBC,
                    AES_CTRL_SHADOWED_KEY_LEN_VALUE_AES_128, /*sideload=*/false,
                    /*manual=*/true);
  write_ctrl_shadowed(ctrl_manual_cbc128);

  const uint32_t kKnownIv[4] = {
      0x10203040u,
      0x50607080u,
      0x90a0b0c0u,
      0xd0e0f001u,
  };
  for (uint32_t i = 0; i < 4u; ++i) {
    abs_mmio_write32(kAesBase + AES_IV_0_REG_OFFSET + i * 4u, kKnownIv[i]);
  }
  for (uint32_t i = 0; i < 8u; ++i) {
    abs_mmio_write32(kAesBase + AES_KEY_SHARE0_0_REG_OFFSET + i * 4u,
                     kKey128[i]);
    abs_mmio_write32(kAesBase + AES_KEY_SHARE1_0_REG_OFFSET + i * 4u, 0u);
  }
  wait_aes_idle();

  // Disable EDN0/EDN1/CSRNG/ENTROPY_SRC so PRNG_RESEED stalls with IDLE == 0:
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
        "[aes_control_fsm.sv:317-335] Expected STATUS.IDLE == 0 during stalled "
        "PRNG_RESEED, got 0x%08x",
        st_reseed);

  // While IDLE == 0, attempt to overwrite IV_0..3, KEY_SHARE0_0..7, and
  // CTRL_SHADOWED, AND write DATA_IN_0..3:
  for (uint32_t i = 0; i < 4u; ++i) {
    abs_mmio_write32(kAesBase + AES_IV_0_REG_OFFSET + i * 4u, 0xdeadbeefu);
    abs_mmio_write32(kAesBase + AES_DATA_IN_0_REG_OFFSET + i * 4u,
                     kPlaintext128[i]);
  }
  for (uint32_t i = 0; i < 8u; ++i) {
    abs_mmio_write32(kAesBase + AES_KEY_SHARE0_0_REG_OFFSET + i * 4u,
                     0xbadc0de0u + i);
  }
  abs_mmio_write32(kAesBase + AES_CTRL_SHADOWED_REG_OFFSET, ctrl_auto_ecb128);
  abs_mmio_write32(kAesBase + AES_CTRL_SHADOWED_REG_OFFSET, ctrl_auto_ecb128);

  // Re-enable entropy complex and wait for PRNG_RESEED to finish (IDLE == 1):
  uint32_t conf = (6u << ENTROPY_SRC_CONF_FIPS_ENABLE_OFFSET) |
                  (6u << ENTROPY_SRC_CONF_FIPS_FLAG_OFFSET) |
                  (9u << ENTROPY_SRC_CONF_RNG_FIPS_OFFSET) |
                  (9u << ENTROPY_SRC_CONF_RNG_BIT_ENABLE_OFFSET) |
                  (9u << ENTROPY_SRC_CONF_THRESHOLD_SCOPE_OFFSET) |
                  (9u << ENTROPY_SRC_CONF_ENTROPY_DATA_REG_ENABLE_OFFSET);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_CONF_REG_OFFSET, conf);
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
    uint32_t iv_got = abs_mmio_read32(kAesBase + AES_IV_0_REG_OFFSET + i * 4u);
    CHECK(iv_got == kKnownIv[i],
          "[aes_control_fsm.sv:317-335] IV_%u write while IDLE==0 must be "
          "ignored: got "
          "0x%08x",
          i, iv_got);
  }
  uint32_t ctrl_got = abs_mmio_read32(kAesBase + AES_CTRL_SHADOWED_REG_OFFSET);
  CHECK(ctrl_got == ctrl_manual_cbc128,
        "[aes_control_fsm.sv:317-335] CTRL_SHADOWED write while IDLE==0 must "
        "be ignored: "
        "got 0x%08x",
        ctrl_got);
  uint32_t st_after_reseed = abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK((st_after_reseed & (1u << AES_STATUS_INPUT_READY_BIT)) == 0u,
        "[aes_control_fsm.sv:317-335] Expected INPUT_READY == 0 proving "
        "DATA_IN_0..3 "
        "writes were accepted while IDLE == 0, got 0x%08x",
        st_after_reseed);

  // =========================================================================
  // Check 7: [aes_reg_pkg.sv:377-412] (INTENDED_SECURITY_HARDENING)
  // 3-tier `AES_PERMIT[34]` byte-enable masks (`4'b1111`, `4'b0011`,
  // `4'b0001`):
  // - 8-bit `sb` write to `CTRL_SHADOWED` (`0x74`, permit `4'b0011`) faults
  //   (`mcause = 7`).
  // - 16-bit `sh` write to `IV_0` (`0x40`, permit `4'b1111`) faults
  //   (`mcause = 7`).
  // - 16-bit `sh` write to `CTRL_SHADOWED` (`0x74`, permit `4'b0011`) and
  //   8-bit `sb` write to `CTRL_AUX_SHADOWED` (`0x78`, permit `4'b0001`)
  //   succeed without fault.
  // =========================================================================
  LOG_INFO(
      "Verifying [aes_reg_pkg.sv:377-412] (INTENDED_SECURITY_HARDENING): "
      "3-tier AES_PERMIT sub-word write faults (mcause=7) vs permitted "
      "widths...");
  g_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write8(kAesBase + AES_CTRL_SHADOWED_REG_OFFSET, 0x01u);
  CHECK(g_fault_count == 1u,
        "[aes_reg_pkg.sv:377-412] Expected 8-bit write to CTRL_SHADOWED (0x74) "
        "to "
        "fault");
  CHECK(g_last_mcause == 7u,
        "[aes_reg_pkg.sv:377-412] Expected Store Access Fault (mcause=7), got "
        "0x%08x",
        g_last_mcause);

  g_fault_count = 0;
  g_last_mcause = 0;
  *(volatile uint16_t *)(kAesBase + AES_IV_0_REG_OFFSET) = 0x1234u;
  CHECK(
      g_fault_count == 1u,
      "[aes_reg_pkg.sv:377-412] Expected 16-bit write to IV_0 (0x40) to fault");
  CHECK(g_last_mcause == 7u,
        "[aes_reg_pkg.sv:377-412] Expected Store Access Fault (mcause=7), got "
        "0x%08x",
        g_last_mcause);

  g_fault_count = 0;
  *(volatile uint16_t *)(kAesBase + AES_CTRL_SHADOWED_REG_OFFSET) =
      (uint16_t)ctrl_manual_cbc128;
  *(volatile uint16_t *)(kAesBase + AES_CTRL_SHADOWED_REG_OFFSET) =
      (uint16_t)ctrl_manual_cbc128;
  uint8_t aux_val =
      (uint8_t)abs_mmio_read32(kAesBase + AES_CTRL_AUX_SHADOWED_REG_OFFSET);
  abs_mmio_write8(kAesBase + AES_CTRL_AUX_SHADOWED_REG_OFFSET, aux_val);
  abs_mmio_write8(kAesBase + AES_CTRL_AUX_SHADOWED_REG_OFFSET, aux_val);
  CHECK(g_fault_count == 0u,
        "[aes_reg_pkg.sv:377-412] Expected 16-bit CTRL_SHADOWED and 8-bit "
        "CTRL_AUX_SHADOWED writes to succeed without fault");

  LOG_INFO("All 7 AES errata & hardening items verified successfully!");
  return true;
}
