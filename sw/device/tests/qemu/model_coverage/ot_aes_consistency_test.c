// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "aes_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "rv_core_ibex_regs.h"
#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

OTTF_DEFINE_TEST_CONFIG(.ignore_alerts = true);

enum {
  kAesBase = TOP_EARLGREY_AES_BASE_ADDR,
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
  CHECK(trig_read == 0u,
        "W/O TRIGGER read must return 0, got 0x%08x", trig_read);
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
  uint32_t ctrl_sideload = make_ctrl_val(
      AES_CTRL_SHADOWED_OPERATION_VALUE_AES_ENC,
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
  uint32_t ctrl_aes192_auto = make_ctrl_val(
      AES_CTRL_SHADOWED_OPERATION_VALUE_AES_ENC,
      AES_CTRL_SHADOWED_MODE_VALUE_AES_ECB,
      AES_CTRL_SHADOWED_KEY_LEN_VALUE_AES_192, /*sideload=*/false,
      /*manual=*/false);
  write_ctrl_shadowed(ctrl_aes192_auto);

  const uint32_t kKey192[8] = {
      0xf7b0738eu, 0x52640edau, 0x2bf310c8u, 0xe5799080u,
      0xd2eaf862u, 0x7b6b2c52u, 0x00000000u, 0x00000000u,
  };
  const uint32_t kPlaintext[4] = {
      0xe2bec16bu, 0x969f402eu, 0x117e3de9u, 0x2a179373u,
  };
  const uint32_t kExpectedCiphertext[4] = {
      0x1d4f33bdu, 0x5ff2456eu, 0x14a212f7u, 0xcca51f57u,
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
  // encryption triggers automatically and matches the golden AES-192 ciphertext.
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

  return true;
}
