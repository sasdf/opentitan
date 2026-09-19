// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/dif/dif_kmac.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "kmac_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kKmacBase = TOP_EARLGREY_KMAC_BASE_ADDR,
};

#define EXPECT_RTL(cond, ...)                   \
  do {                                          \
    if (!(cond)) {                              \
      LOG_ERROR("RTL_MISMATCH: " __VA_ARGS__);  \
      all_ok = false;                           \
    }                                           \
  } while (0)

static void write_cfg_shadowed(uint32_t val) {
  abs_mmio_write32(kKmacBase + KMAC_CFG_SHADOWED_REG_OFFSET, val);
  abs_mmio_write32(kKmacBase + KMAC_CFG_SHADOWED_REG_OFFSET, val);
}

bool test_main(void) {
  bool all_ok = true;

  // Verify KMAC is initially idle.
  uint32_t status = abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET);
  CHECK(bitfield_bit32_read(status, KMAC_STATUS_SHA3_IDLE_BIT));

  // 1. Verify kmac_errchk ERR_CODE formatting on invalid command in Idle
  // (`CmdProcess` = 0x2e issued while in StIdle = 0).
  // In RTL (kmac_pkg.sv:384, kmac_errchk.sv:328-334):
  //   error_o.code = ErrSwCmdSequence (0x08)
  //   error_o.info[23:16] = {5'h0, err_swsequence(1), err_modestrength(0),
  //                          err_prefix(0)} = 0x04 (bit 18)
  //   error_o.info[15:8]  = {5'h0, stL(0)} = 0x00
  //   error_o.info[7:0]   = {2'b0, sw_cmd_i(0x2e)} = 0x2e
  // Expected ERR_CODE = 0x0804002e (QEMU shifts err_swsequence to bit 11 ->
  // 0x0800082e).
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(
      kKmacBase + KMAC_CMD_REG_OFFSET,
      bitfield_field32_write(0, KMAC_CMD_CMD_FIELD, KMAC_CMD_CMD_VALUE_PROCESS));
  uint32_t err_code = abs_mmio_read32(kKmacBase + KMAC_ERR_CODE_REG_OFFSET);
  EXPECT_RTL(err_code == 0x0804002eu,
             "ERR_CODE for CmdProcess in StIdle expected 0x0804002e, got "
             "0x%08x",
             err_code);

  // Acknowledge error via CMD.err_processed and clear INTR_STATE.
  abs_mmio_write32(
      kKmacBase + KMAC_CMD_REG_OFFSET,
      bitfield_bit32_write(0, KMAC_CMD_ERR_PROCESSED_BIT, true));
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, 0xffffffffu);

  // 2. Configure KMAC-128 in cSHAKE mode with software entropy.
  uint32_t cfg = 0;
  cfg = bitfield_bit32_write(cfg, KMAC_CFG_SHADOWED_KMAC_EN_BIT, true);
  cfg = bitfield_field32_write(cfg, KMAC_CFG_SHADOWED_KSTRENGTH_FIELD,
                               KMAC_CFG_SHADOWED_KSTRENGTH_VALUE_L128);
  cfg = bitfield_field32_write(cfg, KMAC_CFG_SHADOWED_MODE_FIELD,
                               KMAC_CFG_SHADOWED_MODE_VALUE_CSHAKE);
  cfg = bitfield_field32_write(cfg, KMAC_CFG_SHADOWED_ENTROPY_MODE_FIELD,
                               KMAC_CFG_SHADOWED_ENTROPY_MODE_VALUE_SW_MODE);
  cfg = bitfield_bit32_write(cfg, KMAC_CFG_SHADOWED_ENTROPY_READY_BIT, true);
  write_cfg_shadowed(cfg);

  for (int i = 0; i < 6; ++i) {
    abs_mmio_write32(kKmacBase + KMAC_ENTROPY_SEED_REG_OFFSET,
                     0x11223344u + (uint32_t)i);
  }

  // Clear ENTROPY_REFRESH_HASH_CNT via CMD.hash_cnt_clr.
  abs_mmio_write32(
      kKmacBase + KMAC_CMD_REG_OFFSET,
      bitfield_bit32_write(0, KMAC_CMD_HASH_CNT_CLR_BIT, true));
  EXPECT_RTL(
      abs_mmio_read32(kKmacBase + KMAC_ENTROPY_REFRESH_HASH_CNT_REG_OFFSET) ==
          0u,
      "ENTROPY_REFRESH_HASH_CNT should be 0 after HASH_CNT_CLR");

  // Program 128-bit key (KEY_SHARE0 ^ KEY_SHARE1 = 0x40..0x4f).
  const uint32_t kKeyShare0[4] = {0x43424140u, 0x47464544u, 0x4b4a4948u,
                                  0x4f4e4d4cu};
  const uint32_t kKeyShare1[4] = {0x11111111u, 0x22222222u, 0x33333333u,
                                  0x44444444u};
  for (int i = 0; i < 4; ++i) {
    abs_mmio_write32(
        kKmacBase + KMAC_KEY_SHARE0_0_REG_OFFSET + (uint32_t)(i * 4),
        kKeyShare0[i] ^ kKeyShare1[i]);
    abs_mmio_write32(
        kKmacBase + KMAC_KEY_SHARE1_0_REG_OFFSET + (uint32_t)(i * 4),
        kKeyShare1[i]);
  }
  abs_mmio_write32(kKmacBase + KMAC_KEY_LEN_REG_OFFSET,
                   KMAC_KEY_LEN_LEN_VALUE_KEY128);

  // Program PREFIX: N = "KMAC", S = "My Tagged Application".
  const uint32_t kPrefix[11] = {
      0x4d4b2001u, 0xa8014341u, 0x5420794du, 0x65676761u,
      0x70412064u, 0x63696c70u, 0x6f697461u, 0x0000006eu,
      0x00000000u, 0x00000000u, 0x00000000u,
  };
  for (int i = 0; i < 11; ++i) {
    abs_mmio_write32(kKmacBase + KMAC_PREFIX_0_REG_OFFSET + (uint32_t)(i * 4),
                     kPrefix[i]);
  }

  // Start first KMAC operation.
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(
      kKmacBase + KMAC_CMD_REG_OFFSET,
      bitfield_field32_write(0, KMAC_CMD_CMD_FIELD, KMAC_CMD_CMD_VALUE_START));

  do {
    status = abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET);
  } while (!bitfield_bit32_read(status, KMAC_STATUS_SHA3_ABSORB_BIT));

  // 3. Write a 4-byte message (0x03020100) to MSG_FIFO and verify that
  // INTR_STATE.fifo_empty is NOT asserted because MSG_FIFO never became full
  // (`msgfifo_full_seen_q == 0` in kmac.sv:635-648).
  abs_mmio_write32(kKmacBase + KMAC_MSG_FIFO_REG_OFFSET, 0x03020100u);
  do {
    status = abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET);
  } while (!bitfield_bit32_read(status, KMAC_STATUS_FIFO_EMPTY_BIT));

  uint32_t intr_state =
      abs_mmio_read32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET);
  EXPECT_RTL(
      bitfield_bit32_read(intr_state, KMAC_INTR_COMMON_FIFO_EMPTY_BIT) == false,
      "INTR_STATE.fifo_empty should remain 0 when MSG_FIFO was never full "
      "(got intr_state=0x%08x)",
      intr_state);

  // 4. Issue CmdProcess and wait for StSqueeze.
  abs_mmio_write32(
      kKmacBase + KMAC_CMD_REG_OFFSET,
      bitfield_field32_write(0, KMAC_CMD_CMD_FIELD, KMAC_CMD_CMD_VALUE_PROCESS));
  do {
    status = abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET);
  } while (!bitfield_bit32_read(status, KMAC_STATUS_SHA3_SQUEEZE_BIT));

  // 5. Verify ENTROPY_REFRESH_HASH_CNT incremented to 1, and that CMD.entropy_req
  // clears ENTROPY_REFRESH_HASH_CNT (`hash_cnt_clr = hash_cnt_clr_i ||
  // threshold_hit || entropy_refresh_req_i` in kmac_entropy.sv:311).
  uint32_t hash_cnt =
      abs_mmio_read32(kKmacBase + KMAC_ENTROPY_REFRESH_HASH_CNT_REG_OFFSET);
  EXPECT_RTL(hash_cnt == 1u,
             "ENTROPY_REFRESH_HASH_CNT expected 1 after KMAC op, got %u",
             hash_cnt);
  abs_mmio_write32(
      kKmacBase + KMAC_CMD_REG_OFFSET,
      bitfield_bit32_write(0, KMAC_CMD_ENTROPY_REQ_BIT, true));
  hash_cnt =
      abs_mmio_read32(kKmacBase + KMAC_ENTROPY_REFRESH_HASH_CNT_REG_OFFSET);
  EXPECT_RTL(
      hash_cnt == 0u,
      "ENTROPY_REFRESH_HASH_CNT should be cleared to 0 by CMD.entropy_req "
      "(got %u)",
      hash_cnt);

  // Finish KMAC operation.
  abs_mmio_write32(
      kKmacBase + KMAC_CMD_REG_OFFSET,
      bitfield_field32_write(0, KMAC_CMD_CMD_FIELD, KMAC_CMD_CMD_VALUE_DONE));
  do {
    status = abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET);
  } while (!bitfield_bit32_read(status, KMAC_STATUS_SHA3_IDLE_BIT));

  // 6. Test cSHAKE128 (`kmac_en = 0`) with a 24-byte function name N and
  // 4-byte customization string S (total 2 + 24 + 2 + 4 = 32 <= 44 bytes in
  // PREFIX_0..10).
  // In QEMU (`ot_kmac_decode_sw_prefix`, line 1051), `offset += funcname_len`
  // (24) instead of `customstr_len` (4) computes offset = 52 > 44 and discards
  // the entire prefix, causing different customization strings S ("BBBB" vs
  // "CCCC") to produce the exact same empty-prefix SHAKE128 digest!
  cfg = bitfield_bit32_write(cfg, KMAC_CFG_SHADOWED_KMAC_EN_BIT, false);
  write_cfg_shadowed(cfg);

  const uint32_t kCshakePrefixB[11] = {
      0x4141c001u, 0x41414141u, 0x41414141u, 0x41414141u,
      0x41414141u, 0x41414141u, 0x20014141u, 0x42424242u,
      0x00000000u, 0x00000000u, 0x00000000u,
  };
  for (int i = 0; i < 11; ++i) {
    abs_mmio_write32(kKmacBase + KMAC_PREFIX_0_REG_OFFSET + (uint32_t)(i * 4),
                     kCshakePrefixB[i]);
  }
  abs_mmio_write32(
      kKmacBase + KMAC_CMD_REG_OFFSET,
      bitfield_field32_write(0, KMAC_CMD_CMD_FIELD, KMAC_CMD_CMD_VALUE_START));
  do {
    status = abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET);
  } while (!bitfield_bit32_read(status, KMAC_STATUS_SHA3_ABSORB_BIT));
  abs_mmio_write32(kKmacBase + KMAC_MSG_FIFO_REG_OFFSET, 0x03020100u);
  abs_mmio_write32(
      kKmacBase + KMAC_CMD_REG_OFFSET,
      bitfield_field32_write(0, KMAC_CMD_CMD_FIELD, KMAC_CMD_CMD_VALUE_PROCESS));
  do {
    status = abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET);
  } while (!bitfield_bit32_read(status, KMAC_STATUS_SHA3_SQUEEZE_BIT));
  uint32_t cshake_d0_b =
      abs_mmio_read32(kKmacBase + KMAC_STATE_REG_OFFSET) ^
      abs_mmio_read32(kKmacBase + KMAC_STATE_REG_OFFSET + 0x100u);
  abs_mmio_write32(
      kKmacBase + KMAC_CMD_REG_OFFSET,
      bitfield_field32_write(0, KMAC_CMD_CMD_FIELD, KMAC_CMD_CMD_VALUE_DONE));
  do {
    status = abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET);
  } while (!bitfield_bit32_read(status, KMAC_STATUS_SHA3_IDLE_BIT));

  // Change S from "BBBB" (0x42424242) to "CCCC" (0x43434343) at PREFIX_7.
  abs_mmio_write32(kKmacBase + KMAC_PREFIX_7_REG_OFFSET, 0x43434343u);
  abs_mmio_write32(
      kKmacBase + KMAC_CMD_REG_OFFSET,
      bitfield_field32_write(0, KMAC_CMD_CMD_FIELD, KMAC_CMD_CMD_VALUE_START));
  do {
    status = abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET);
  } while (!bitfield_bit32_read(status, KMAC_STATUS_SHA3_ABSORB_BIT));
  abs_mmio_write32(kKmacBase + KMAC_MSG_FIFO_REG_OFFSET, 0x03020100u);
  abs_mmio_write32(
      kKmacBase + KMAC_CMD_REG_OFFSET,
      bitfield_field32_write(0, KMAC_CMD_CMD_FIELD, KMAC_CMD_CMD_VALUE_PROCESS));
  do {
    status = abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET);
  } while (!bitfield_bit32_read(status, KMAC_STATUS_SHA3_SQUEEZE_BIT));
  uint32_t cshake_d0_c =
      abs_mmio_read32(kKmacBase + KMAC_STATE_REG_OFFSET) ^
      abs_mmio_read32(kKmacBase + KMAC_STATE_REG_OFFSET + 0x100u);
  abs_mmio_write32(
      kKmacBase + KMAC_CMD_REG_OFFSET,
      bitfield_field32_write(0, KMAC_CMD_CMD_FIELD, KMAC_CMD_CMD_VALUE_DONE));

  EXPECT_RTL(cshake_d0_b != cshake_d0_c,
             "cSHAKE128 with 24B N and 4B S ignored S due to prefix offset bug "
             "(both produced 0x%08x)",
             cshake_d0_b);

  return all_ok;
}
