// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/dif/dif_hmac.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
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

bool test_main(void) {
  // Clear any pending interrupts left from ROM/ROM_EXT boot.
  abs_mmio_write32(kHmacBase + HMAC_INTR_ENABLE_REG_OFFSET, 0u);
  abs_mmio_write32(kHmacBase + HMAC_INTR_TEST_REG_OFFSET, 0u);
  abs_mmio_write32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET, 0x7u);

  uint32_t cfg_sha256_disabled =
      bitfield_field32_write(0, HMAC_CFG_DIGEST_SIZE_FIELD,
                             HMAC_CFG_DIGEST_SIZE_VALUE_SHA2_256) |
      bitfield_field32_write(0, HMAC_CFG_KEY_LENGTH_FIELD,
                             HMAC_CFG_KEY_LENGTH_VALUE_KEY_256);
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg_sha256_disabled);

  // 1. Write-only KEY_0..KEY_31 readback returns 0 (ot_hmac.c:815).
  abs_mmio_write32(kHmacBase + HMAC_KEY_0_REG_OFFSET, 0x11223344u);
  uint32_t key0_read = abs_mmio_read32(kHmacBase + HMAC_KEY_0_REG_OFFSET);
  CHECK(key0_read == 0u, "Write-only KEY_0 must read back as 0, got 0x%08x",
        key0_read);

  // 2. CMD.HASH_CONTINUE when CFG.SHA_EN == 0 triggers
  // SwHashStartWhenShaDisabled (0x2) (ot_hmac.c:1036-1039).
  abs_mmio_write32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET, 0x7u);
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_CONTINUE_BIT);
  uint32_t intr = abs_mmio_read32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET);
  uint32_t err = abs_mmio_read32(kHmacBase + HMAC_ERR_CODE_REG_OFFSET);
  CHECK((intr & (1u << HMAC_INTR_STATE_HMAC_ERR_BIT)) != 0u,
        "CMD.HASH_CONTINUE with SHA_EN=0 must assert INTR_STATE.hmac_err");
  CHECK(err == kHmacErrHashStartWhenShaDisabled,
        "CMD.HASH_CONTINUE with SHA_EN=0 expected ERR_CODE=0x2, got 0x%08x",
        err);
  abs_mmio_write32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET,
                   1u << HMAC_INTR_STATE_HMAC_ERR_BIT);

  // 3. Enable SHA-256 with ENDIAN_SWAP=1, DIGEST_SWAP=0, and start hashing.
  uint32_t cfg_sha256_active =
      cfg_sha256_disabled | (1u << HMAC_CFG_SHA_EN_BIT) |
      (1u << HMAC_CFG_ENDIAN_SWAP_BIT);
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg_sha256_active);
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_START_BIT);

  // (a) CMD.HASH_CONTINUE while already active triggers
  // SwHashStartWhenActive (0x4) (ot_hmac.c:1041-1043).
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_CONTINUE_BIT);
  intr = abs_mmio_read32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET);
  err = abs_mmio_read32(kHmacBase + HMAC_ERR_CODE_REG_OFFSET);
  CHECK((intr & (1u << HMAC_INTR_STATE_HMAC_ERR_BIT)) != 0u,
        "CMD.HASH_CONTINUE while active must assert INTR_STATE.hmac_err");
  CHECK(err == kHmacErrHashStartWhenActive,
        "CMD.HASH_CONTINUE while active expected ERR_CODE=0x4, got 0x%08x",
        err);

  // (b) Write to read-only ERR_CODE (ot_hmac.c:1129) is ignored.
  abs_mmio_write32(kHmacBase + HMAC_ERR_CODE_REG_OFFSET, 0xdeadbeefu);
  CHECK(abs_mmio_read32(kHmacBase + HMAC_ERR_CODE_REG_OFFSET) ==
            kHmacErrHashStartWhenActive,
        "Write to read-only ERR_CODE must be ignored");
  abs_mmio_write32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET,
                   1u << HMAC_INTR_STATE_HMAC_ERR_BIT);

  // (c) Write to KEY_0 while active triggers SwUpdateSecretKeyInProcess (0x3)
  // (ot_hmac.c:1150).
  abs_mmio_write32(kHmacBase + HMAC_KEY_0_REG_OFFSET, 0xdeadbeefu);
  intr = abs_mmio_read32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET);
  err = abs_mmio_read32(kHmacBase + HMAC_ERR_CODE_REG_OFFSET);
  CHECK((intr & (1u << HMAC_INTR_STATE_HMAC_ERR_BIT)) != 0u,
        "KEY_0 write while active must assert INTR_STATE.hmac_err");
  CHECK(err == kHmacErrUpdateSecretKeyInProcess,
        "KEY_0 write while active expected ERR_CODE=0x3, got 0x%08x", err);
  abs_mmio_write32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET,
                   1u << HMAC_INTR_STATE_HMAC_ERR_BIT);

  // (d) Writes to DIGEST_0, MSG_LENGTH_LOWER, and MSG_LENGTH_UPPER while active
  // are ignored (ot_hmac.c:1156, 1189, 1199).
  abs_mmio_write32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET, 0xdeadbeefu);
  abs_mmio_write32(kHmacBase + HMAC_MSG_LENGTH_LOWER_REG_OFFSET, 0xdeadbeefu);
  abs_mmio_write32(kHmacBase + HMAC_MSG_LENGTH_UPPER_REG_OFFSET, 0xdeadbeefu);
  CHECK(abs_mmio_read32(kHmacBase + HMAC_MSG_LENGTH_LOWER_REG_OFFSET) == 0u,
        "MSG_LENGTH_LOWER write while active must be ignored");
  CHECK(abs_mmio_read32(kHmacBase + HMAC_MSG_LENGTH_UPPER_REG_OFFSET) == 0u,
        "MSG_LENGTH_UPPER write while active must be ignored");

  // 4. Halfword (16-bit) MSG_FIFO writes with ENDIAN_SWAP=1 (ot_hmac.c:1248-1249),
  // CMD.HASH_STOP with non-empty input_fifo and curlen != 0 (ot_hmac.c:511-512,
  // 642, 644, 655-657, 1018-1032), and CMD.HASH_CONTINUE + CMD.HASH_PROCESS.
  // Push 64 bytes ("ABCD" + 60*'A') as:
  // - 31 bytes: 0x4142 ("AB", sh), 0x43 ('C', sb), 7 words of 0x41414141 (28B)
  // - 4 bytes:  1 word of 0x41414141 -> 35 bytes total (drains QEMU input_fifo
  //             at >= 32 bytes, leaving curlen = 35 and input_fifo = 0)
  // - 29 bytes: 0x44 ('D', sb), 0x4141 ("AA", sh), 6 words of 0x41414141 (24B),
  //             0x4141 ("AA", sh) -> 29 bytes in QEMU input_fifo (< 32) and
  //             35 + 29 = 64 bytes (512 bits) in RTL MSG_FIFO.
  *(volatile uint16_t *)(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET) = 0x4142u;
  abs_mmio_write8(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET, 'C');
  for (int i = 0; i < 7; ++i) {
    abs_mmio_write32(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET, 0x41414141u);
  }
  abs_mmio_write32(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET, 0x41414141u);
  abs_mmio_write8(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET, 'D');
  *(volatile uint16_t *)(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET) = 0x4141u;
  for (int i = 0; i < 6; ++i) {
    abs_mmio_write32(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET, 0x41414141u);
  }
  *(volatile uint16_t *)(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET) = 0x4141u;
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_STOP_BIT);

  while ((abs_mmio_read32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET) &
          (1u << HMAC_INTR_STATE_HMAC_DONE_BIT)) == 0u) {
  }
  abs_mmio_write32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET,
                   1u << HMAC_INTR_STATE_HMAC_DONE_BIT);

  uint32_t status = abs_mmio_read32(kHmacBase + HMAC_STATUS_REG_OFFSET);
  CHECK((status & (1u << HMAC_STATUS_HMAC_IDLE_BIT)) != 0u,
        "HMAC must return to idle after completing block following HASH_STOP");
  CHECK(abs_mmio_read32(kHmacBase + HMAC_MSG_LENGTH_LOWER_REG_OFFSET) == 512u,
        "Expected 512 bits after first 64-byte block");

  // Resume via CMD.HASH_CONTINUE, push "BCD" (3 bytes), and finalize with
  // CMD.HASH_PROCESS.
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_CONTINUE_BIT);
  abs_mmio_write8(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET, 'B');
  abs_mmio_write8(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET, 'C');
  abs_mmio_write8(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET, 'D');
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_PROCESS_BIT);

  while ((abs_mmio_read32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET) &
          (1u << HMAC_INTR_STATE_HMAC_DONE_BIT)) == 0u) {
  }
  abs_mmio_write32(kHmacBase + HMAC_INTR_STATE_REG_OFFSET,
                   1u << HMAC_INTR_STATE_HMAC_DONE_BIT);

  CHECK(abs_mmio_read32(kHmacBase + HMAC_MSG_LENGTH_LOWER_REG_OFFSET) == 536u,
        "Expected 536 bits total");

  const uint32_t kExpectedDigest[8] = {
      0x4c35edadu, 0x300f956au, 0x2ea459dbu, 0x7563125au,
      0x45e55f96u, 0x7a5ead80u, 0xc8312b07u, 0x8b2a86dfu,
  };
  for (uint32_t i = 0; i < 8u; ++i) {
    uint32_t got =
        abs_mmio_read32(kHmacBase + HMAC_DIGEST_0_REG_OFFSET + i * 4u);
    CHECK(got == kExpectedDigest[i],
          "DIGEST_%u mismatch: expected 0x%08x, got 0x%08x", i,
          kExpectedDigest[i], got);
  }

  // 5. Exercise repeated CMD.HASH_PROCESS while msg_length_overflow stalls
  // processing (ot_hmac.c:1001-1006), then clear cfg_block via CMD.HASH_STOP
  // and disable CFG.SHA_EN to return idle.
  abs_mmio_write32(kHmacBase + HMAC_MSG_LENGTH_UPPER_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kHmacBase + HMAC_MSG_LENGTH_LOWER_REG_OFFSET, 0xfffffff0u);
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_CONTINUE_BIT);
  abs_mmio_write32(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET, 0x11223344u);
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_PROCESS_BIT);
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_PROCESS_BIT);
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_STOP_BIT);
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg_sha256_disabled);
  status = abs_mmio_read32(kHmacBase + HMAC_STATUS_REG_OFFSET);
  CHECK((status & (1u << HMAC_STATUS_HMAC_IDLE_BIT)) != 0u,
        "Disabling CFG.SHA_EN must reset overflowed HMAC back to idle");

  // 6. Exercise CMD.HASH_STOP in SHA2_512 mode (ot_hmac_get_curlen SHA2_512).
  uint32_t cfg_sha512_active =
      bitfield_field32_write(0, HMAC_CFG_DIGEST_SIZE_FIELD,
                             HMAC_CFG_DIGEST_SIZE_VALUE_SHA2_512) |
      bitfield_field32_write(0, HMAC_CFG_KEY_LENGTH_FIELD,
                             HMAC_CFG_KEY_LENGTH_VALUE_KEY_512) |
      (1u << HMAC_CFG_SHA_EN_BIT);
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg_sha512_active);
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_START_BIT);
  abs_mmio_write32(kHmacBase + HMAC_MSG_FIFO_REG_OFFSET, 0x11223344u);
  abs_mmio_write32(kHmacBase + HMAC_CMD_REG_OFFSET,
                   1u << HMAC_CMD_HASH_STOP_BIT);
  abs_mmio_write32(kHmacBase + HMAC_CFG_REG_OFFSET, cfg_sha256_disabled);
  status = abs_mmio_read32(kHmacBase + HMAC_STATUS_REG_OFFSET);
  CHECK((status & (1u << HMAC_STATUS_HMAC_IDLE_BIT)) != 0u,
        "Disabling CFG.SHA_EN must reset SHA2_512 HASH_STOP back to idle");

  return true;
}
