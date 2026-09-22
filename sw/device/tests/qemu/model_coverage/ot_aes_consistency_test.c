// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/multibits.h"
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

bool test_main(void) {
  wait_aes_idle();

  // 1. Sub-word read of STATUS (ot_aes.c:1532-1532) & R/O write to STATUS
  // (ot_aes.c:1336-1336) & W/O read of TRIGGER (ot_aes.c:1245-1245).
  uint32_t status32 = abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  uint8_t status8 = abs_mmio_read8(kAesBase + AES_STATUS_REG_OFFSET);
  uint16_t status16 =
      *(const volatile uint16_t *)(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK(status8 == (uint8_t)(status32 & 0xffu),
        "8-bit STATUS read mismatch: got 0x%02x, expected 0x%02x", status8,
        (uint8_t)(status32 & 0xffu));
  CHECK(status16 == (uint16_t)(status32 & 0xffffu),
        "16-bit STATUS read mismatch: got 0x%04x, expected 0x%04x", status16,
        (uint16_t)(status32 & 0xffffu));

  abs_mmio_write32(kAesBase + AES_STATUS_REG_OFFSET, UINT32_MAX);
  uint32_t status_after_ro_write =
      abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK(status_after_ro_write == status32,
        "R/O STATUS write modified register: got 0x%08x, expected 0x%08x",
        status_after_ro_write, status32);

  uint32_t trig_read = abs_mmio_read32(kAesBase + AES_TRIGGER_REG_OFFSET);
  CHECK(trig_read == 0u, "W/O TRIGGER read must return 0, got 0x%08x",
        trig_read);
  CHECK(abs_mmio_read32(kIbexBase + RV_CORE_IBEX_ERR_STATUS_REG_OFFSET) == 0u,
        "Unexpected Ibex bus fault on TRIGGER read");

  // 2. Out-of-bounds MMIO read at offset 0x88 (past R_STATUS at 0x84).
  load_store_fault_seen = false;
  abs_mmio_write32(kIbexBase + RV_CORE_IBEX_ERR_STATUS_REG_OFFSET, UINT32_MAX);
  (void)abs_mmio_read32(kAesBase + 0x88u);
  icache_invalidate();
  uint32_t err_status =
      abs_mmio_read32(kIbexBase + RV_CORE_IBEX_ERR_STATUS_REG_OFFSET);
  if (err_status != 0u) {
    abs_mmio_write32(kIbexBase + RV_CORE_IBEX_ERR_STATUS_REG_OFFSET,
                     err_status);
  }
  CHECK(load_store_fault_seen || (err_status != 0u),
        "Expected bus fault on out-of-bounds AES register read at 0x88");

  // 3. CTRL_AUX_SHADOWED shadow update error (ot_aes.c:1469-1472).
  uint32_t aux_regwen =
      abs_mmio_read32(kAesBase + AES_CTRL_AUX_REGWEN_REG_OFFSET);
  CHECK((aux_regwen & 0x1u) == 1u, "Expected CTRL_AUX_REGWEN == 1");
  uint32_t orig_aux =
      abs_mmio_read32(kAesBase + AES_CTRL_AUX_SHADOWED_REG_OFFSET);
  abs_mmio_write32(kAesBase + AES_CTRL_AUX_SHADOWED_REG_OFFSET,
                   orig_aux ^ (1u << AES_CTRL_AUX_SHADOWED_FORCE_MASKS_BIT));
  abs_mmio_write32(kAesBase + AES_CTRL_AUX_SHADOWED_REG_OFFSET, orig_aux);
  uint32_t status_after_aux_err =
      abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK((status_after_aux_err &
         (1u << AES_STATUS_ALERT_RECOV_CTRL_UPDATE_ERR_BIT)) != 0u,
        "Expected STATUS.ALERT_RECOV_CTRL_UPDATE_ERR after mismatched "
        "CTRL_AUX_SHADOWED write, got 0x%08x",
        status_after_aux_err);

  // 4. Key share write ignored when SIDELOAD = 1 (ot_aes.c:1356-1356).
  // Verify that writing two completely different software keys to KEY_SHARE0/1
  // while SIDELOAD = 1 is ignored by hardware and produces identical DATA_OUT.
  uint32_t ctrl_sideload =
      make_ctrl_val(AES_CTRL_SHADOWED_OPERATION_VALUE_AES_ENC,
                    AES_CTRL_SHADOWED_MODE_VALUE_AES_ECB,
                    AES_CTRL_SHADOWED_KEY_LEN_VALUE_AES_128, /*sideload=*/true,
                    /*manual=*/true);
  write_ctrl_shadowed(ctrl_sideload);
  for (uint32_t i = 0; i < 8u; ++i) {
    abs_mmio_write32(kAesBase + AES_KEY_SHARE0_0_REG_OFFSET + i * 4u,
                     0x11111111u * (i + 1u));
    abs_mmio_write32(kAesBase + AES_KEY_SHARE1_0_REG_OFFSET + i * 4u, 0u);
  }
  for (uint32_t i = 0; i < 4u; ++i) {
    abs_mmio_write32(kAesBase + AES_DATA_IN_0_REG_OFFSET + i * 4u,
                     0x22222222u * (i + 1u));
  }
  abs_mmio_write32(kAesBase + AES_TRIGGER_REG_OFFSET,
                   1u << AES_TRIGGER_START_BIT);
  while ((abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET) &
          (1u << AES_STATUS_OUTPUT_VALID_BIT)) == 0u) {
  }
  uint32_t sideload_ct_a[4];
  for (uint32_t i = 0; i < 4u; ++i) {
    sideload_ct_a[i] =
        abs_mmio_read32(kAesBase + AES_DATA_OUT_0_REG_OFFSET + i * 4u);
  }

  for (uint32_t i = 0; i < 8u; ++i) {
    abs_mmio_write32(kAesBase + AES_KEY_SHARE0_0_REG_OFFSET + i * 4u,
                     0x99999999u ^ (0x11111111u * (i + 1u)));
    abs_mmio_write32(kAesBase + AES_KEY_SHARE1_0_REG_OFFSET + i * 4u,
                     0xffffffffu);
  }
  for (uint32_t i = 0; i < 4u; ++i) {
    abs_mmio_write32(kAesBase + AES_DATA_IN_0_REG_OFFSET + i * 4u,
                     0x22222222u * (i + 1u));
  }
  abs_mmio_write32(kAesBase + AES_TRIGGER_REG_OFFSET,
                   1u << AES_TRIGGER_START_BIT);
  while ((abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET) &
          (1u << AES_STATUS_OUTPUT_VALID_BIT)) == 0u) {
  }
  for (uint32_t i = 0; i < 4u; ++i) {
    uint32_t sideload_ct_b =
        abs_mmio_read32(kAesBase + AES_DATA_OUT_0_REG_OFFSET + i * 4u);
    CHECK(sideload_ct_b == sideload_ct_a[i],
          "SIDELOAD=1 must ignore KEY_SHARE writes: word %u got 0x%08x vs "
          "0x%08x",
          i, sideload_ct_b, sideload_ct_a[i]);
  }

  // 5. AES-192 encryption (ot_aes.c:357-359) & armed keyshare partial overwrite
  // invalidation (ot_aes.c:1365-1367) in automatic mode (manual=false).
  // NIST FIPS-197 Appendix C.2 AES-192 test vector:
  // Key = 8e73b0f7 da0e6452 c810f32b 809079e5 62f8ead2 522c6b7b
  // Plaintext = 6bc1bee2 2e409f96 e93d7e11 7393172a
  // Ciphertext = bd334f1d 6e45f25f f712a214 571fa5cc
  uint32_t ctrl_aes192_auto =
      make_ctrl_val(AES_CTRL_SHADOWED_OPERATION_VALUE_AES_ENC,
                    AES_CTRL_SHADOWED_MODE_VALUE_AES_ECB,
                    AES_CTRL_SHADOWED_KEY_LEN_VALUE_AES_192, /*sideload=*/false,
                    /*manual=*/false);
  write_ctrl_shadowed(ctrl_aes192_auto);

  const uint32_t kKey192[8] = {
      0xf7b0738eu, 0x52640edau, 0x2bf310c8u, 0xe5799080u,
      0xd2eaf862u, 0x7b6b2c52u, 0x00000000u, 0x00000000u,
  };
  const uint32_t kPlaintext[4] = {
      0xe2bec16bu,
      0x969f402eu,
      0x117e3de9u,
      0x2a179373u,
  };
  const uint32_t kExpectedCiphertext[4] = {
      0x1d4f33bdu,
      0x5ff2456eu,
      0x14a212f7u,
      0xcca51f57u,
  };

  for (uint32_t i = 0; i < 8u; ++i) {
    abs_mmio_write32(kAesBase + AES_KEY_SHARE0_0_REG_OFFSET + i * 4u,
                     kKey192[i]);
    abs_mmio_write32(kAesBase + AES_KEY_SHARE1_0_REG_OFFSET + i * 4u, 0u);
  }
  for (uint32_t i = 0; i < 4u; ++i) {
    abs_mmio_write32(kAesBase + AES_DATA_IN_0_REG_OFFSET + i * 4u,
                     kPlaintext[i]);
  }
  while ((abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET) &
          (1u << AES_STATUS_OUTPUT_VALID_BIT)) == 0u) {
  }
  for (uint32_t i = 0; i < 4u; ++i) {
    uint32_t got =
        abs_mmio_read32(kAesBase + AES_DATA_OUT_0_REG_OFFSET + i * 4u);
    CHECK(got == kExpectedCiphertext[i],
          "AES-192 DATA_OUT_%u mismatch: got 0x%08x, expected 0x%08x", i, got,
          kExpectedCiphertext[i]);
  }

  // Now keyshare_armed is true. Write only KEY_SHARE0_0 (partial key update)
  // and write all 4 DATA_IN words in auto mode; verify OUTPUT_VALID remains 0
  // because key_ready was invalidated by the partial keyshare write.
  abs_mmio_write32(kAesBase + AES_KEY_SHARE0_0_REG_OFFSET, kKey192[0]);
  for (uint32_t i = 0; i < 4u; ++i) {
    abs_mmio_write32(kAesBase + AES_DATA_IN_0_REG_OFFSET + i * 4u,
                     kPlaintext[i]);
  }
  wait_aes_idle();
  uint32_t partial_key_status =
      abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK((partial_key_status & (1u << AES_STATUS_OUTPUT_VALID_BIT)) == 0u,
        "Expected OUTPUT_VALID == 0 after partial keyshare write, got 0x%08x",
        partial_key_status);

  // Complete writing the remaining keyshare registers and verify auto-mode
  // encryption triggers automatically and matches the golden AES-192
  // ciphertext.
  for (uint32_t i = 1; i < 8u; ++i) {
    abs_mmio_write32(kAesBase + AES_KEY_SHARE0_0_REG_OFFSET + i * 4u,
                     kKey192[i]);
  }
  for (uint32_t i = 0; i < 8u; ++i) {
    abs_mmio_write32(kAesBase + AES_KEY_SHARE1_0_REG_OFFSET + i * 4u, 0u);
  }
  while ((abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET) &
          (1u << AES_STATUS_OUTPUT_VALID_BIT)) == 0u) {
  }
  for (uint32_t i = 0; i < 4u; ++i) {
    uint32_t got =
        abs_mmio_read32(kAesBase + AES_DATA_OUT_0_REG_OFFSET + i * 4u);
    CHECK(got == kExpectedCiphertext[i],
          "AES-192 post-reload DATA_OUT_%u mismatch: got 0x%08x, expected "
          "0x%08x",
          i, got, kExpectedCiphertext[i]);
  }

  // 6. AES-192 in manual mode (ot_aes.c:357-359 `ot_aes_get_key_mask` case
  // 0x02u). In MANUAL_OPERATION mode, `ot_aes_update_key` calls
  // `ot_aes_get_key_mask` which only requires the 6 lower words (192 bits) of
  // KEY_SHARE0 and KEY_SHARE1.
  uint32_t ctrl_aes192_manual =
      make_ctrl_val(AES_CTRL_SHADOWED_OPERATION_VALUE_AES_ENC,
                    AES_CTRL_SHADOWED_MODE_VALUE_AES_ECB,
                    AES_CTRL_SHADOWED_KEY_LEN_VALUE_AES_192, /*sideload=*/false,
                    /*manual=*/true);
  write_ctrl_shadowed(ctrl_aes192_manual);

  for (uint32_t i = 0; i < 6u; ++i) {
    abs_mmio_write32(kAesBase + AES_KEY_SHARE0_0_REG_OFFSET + i * 4u,
                     kKey192[i]);
    abs_mmio_write32(kAesBase + AES_KEY_SHARE1_0_REG_OFFSET + i * 4u, 0u);
  }
  for (uint32_t i = 0; i < 4u; ++i) {
    abs_mmio_write32(kAesBase + AES_DATA_IN_0_REG_OFFSET + i * 4u,
                     kPlaintext[i]);
  }
  abs_mmio_write32(kAesBase + AES_TRIGGER_REG_OFFSET,
                   1u << AES_TRIGGER_START_BIT);
  while ((abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET) &
          (1u << AES_STATUS_OUTPUT_VALID_BIT)) == 0u) {
  }
  for (uint32_t i = 0; i < 4u; ++i) {
    uint32_t got =
        abs_mmio_read32(kAesBase + AES_DATA_OUT_0_REG_OFFSET + i * 4u);
    CHECK(got == kExpectedCiphertext[i],
          "AES-192 manual mode DATA_OUT_%u mismatch: got 0x%08x, expected "
          "0x%08x",
          i, got, kExpectedCiphertext[i]);
  }

  // 7. Gate KEY_SHARE and IV writes when STATUS.IDLE == 0 during pending
  // PRNG_RESEED (aes_control_fsm.sv:315-343, 472-495; ot_aes.c:1360, 1378).
  uint32_t ctrl_aes192_cbc =
      make_ctrl_val(AES_CTRL_SHADOWED_OPERATION_VALUE_AES_ENC,
                    AES_CTRL_SHADOWED_MODE_VALUE_AES_CBC,
                    AES_CTRL_SHADOWED_KEY_LEN_VALUE_AES_192, /*sideload=*/false,
                    /*manual=*/true);
  write_ctrl_shadowed(ctrl_aes192_cbc);

  const uint32_t kKnownIv[4] = {
      0x10203040u,
      0x50607080u,
      0x90a0b0c0u,
      0xd0e0f001u,
  };
  for (uint32_t i = 0; i < 4u; ++i) {
    abs_mmio_write32(kAesBase + AES_IV_0_REG_OFFSET + i * 4u, kKnownIv[i]);
  }
  for (uint32_t i = 0; i < 6u; ++i) {
    abs_mmio_write32(kAesBase + AES_KEY_SHARE0_0_REG_OFFSET + i * 4u,
                     kKey192[i]);
    abs_mmio_write32(kAesBase + AES_KEY_SHARE1_0_REG_OFFSET + i * 4u, 0u);
  }
  CHECK(abs_mmio_read32(kAesBase + AES_IV_0_REG_OFFSET) == kKnownIv[0],
        "Initial IV_0 readback mismatch");

  // Disable EDN0, EDN1, CSRNG, and ENTROPY_SRC so AES stalls in
  // CTRL_PRNG_RESEED (STATUS.IDLE == 0).
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
  uint32_t reseed_status = abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET);
  CHECK((reseed_status & (1u << AES_STATUS_IDLE_BIT)) == 0u,
        "Expected STATUS.IDLE == 0 while PRNG_RESEED awaits EDN0, got 0x%08x",
        reseed_status);

  // Attempt to overwrite IV_0..3 and KEY_SHARE0_0..5 while STATUS.IDLE == 0.
  for (uint32_t i = 0; i < 4u; ++i) {
    abs_mmio_write32(kAesBase + AES_IV_0_REG_OFFSET + i * 4u, 0xdeadbeefu);
  }
  for (uint32_t i = 0; i < 6u; ++i) {
    abs_mmio_write32(kAesBase + AES_KEY_SHARE0_0_REG_OFFSET + i * 4u,
                     0xbadc0de0u + i);
  }

  // Re-enable ENTROPY_SRC, CSRNG, and EDN0 in boot request mode (0x9966u) and
  // wait for PRNG_RESEED to complete (STATUS.IDLE == 1).
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
    uint32_t iv_after =
        abs_mmio_read32(kAesBase + AES_IV_0_REG_OFFSET + i * 4u);
    CHECK(iv_after == kKnownIv[i],
          "IV_%u write while STATUS.IDLE==0 must be ignored: got 0x%08x, "
          "expected 0x%08x",
          i, iv_after, kKnownIv[i]);
  }

  // Switch back to ECB manual mode (preserving KEY_SHARE registers) and verify
  // encryption still uses kKey192 rather than the ignored 0xbadc0de0 writes.
  write_ctrl_shadowed(ctrl_aes192_manual);
  for (uint32_t i = 0; i < 4u; ++i) {
    abs_mmio_write32(kAesBase + AES_DATA_IN_0_REG_OFFSET + i * 4u,
                     kPlaintext[i]);
  }
  abs_mmio_write32(kAesBase + AES_TRIGGER_REG_OFFSET,
                   1u << AES_TRIGGER_START_BIT);
  while ((abs_mmio_read32(kAesBase + AES_STATUS_REG_OFFSET) &
          (1u << AES_STATUS_OUTPUT_VALID_BIT)) == 0u) {
  }
  for (uint32_t i = 0; i < 4u; ++i) {
    uint32_t got =
        abs_mmio_read32(kAesBase + AES_DATA_OUT_0_REG_OFFSET + i * 4u);
    CHECK(got == kExpectedCiphertext[i],
          "KEY_SHARE write while STATUS.IDLE==0 must be ignored: word %u got "
          "0x%08x, expected 0x%08x",
          i, got, kExpectedCiphertext[i]);
  }

  return true;
}
