// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/dif/dif_kmac.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "kmac_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kKmacBase = TOP_EARLGREY_KMAC_BASE_ADDR,
};

static volatile uint32_t fault_count = 0;
static volatile uint32_t last_mcause = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = 0;
  CSR_READ(CSR_REG_MCAUSE, &mcause);
  last_mcause = mcause;
  fault_count++;
}

#define EXPECT_RTL(cond, ...)                  \
  do {                                         \
    if (!(cond)) {                             \
      LOG_ERROR("RTL_MISMATCH: " __VA_ARGS__); \
      all_ok = false;                          \
    }                                          \
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
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   bitfield_field32_write(0, KMAC_CMD_CMD_FIELD,
                                          KMAC_CMD_CMD_VALUE_PROCESS));
  uint32_t err_code = abs_mmio_read32(kKmacBase + KMAC_ERR_CODE_REG_OFFSET);
  EXPECT_RTL(err_code == 0x0804002eu,
             "ERR_CODE for CmdProcess in StIdle expected 0x0804002e, got "
             "0x%08x",
             err_code);

  // Acknowledge error via CMD.err_processed and clear INTR_STATE.
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
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
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   bitfield_bit32_write(0, KMAC_CMD_HASH_CNT_CLR_BIT, true));
  EXPECT_RTL(abs_mmio_read32(kKmacBase +
                             KMAC_ENTROPY_REFRESH_HASH_CNT_REG_OFFSET) == 0u,
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

  uint32_t intr_state = abs_mmio_read32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET);
  EXPECT_RTL(
      bitfield_bit32_read(intr_state, KMAC_INTR_COMMON_FIFO_EMPTY_BIT) == false,
      "INTR_STATE.fifo_empty should remain 0 when MSG_FIFO was never full "
      "(got intr_state=0x%08x)",
      intr_state);

  // 4. Issue CmdProcess and wait for StSqueeze.
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   bitfield_field32_write(0, KMAC_CMD_CMD_FIELD,
                                          KMAC_CMD_CMD_VALUE_PROCESS));
  do {
    status = abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET);
  } while (!bitfield_bit32_read(status, KMAC_STATUS_SHA3_SQUEEZE_BIT));

  // 5. Verify ENTROPY_REFRESH_HASH_CNT incremented to 1, and that
  // CMD.entropy_req clears ENTROPY_REFRESH_HASH_CNT (`hash_cnt_clr =
  // hash_cnt_clr_i || threshold_hit || entropy_refresh_req_i` in
  // kmac_entropy.sv:311).
  uint32_t hash_cnt =
      abs_mmio_read32(kKmacBase + KMAC_ENTROPY_REFRESH_HASH_CNT_REG_OFFSET);
  EXPECT_RTL(hash_cnt == 1u,
             "ENTROPY_REFRESH_HASH_CNT expected 1 after KMAC op, got %u",
             hash_cnt);
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
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
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   bitfield_field32_write(0, KMAC_CMD_CMD_FIELD,
                                          KMAC_CMD_CMD_VALUE_PROCESS));
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
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   bitfield_field32_write(0, KMAC_CMD_CMD_FIELD,
                                          KMAC_CMD_CMD_VALUE_PROCESS));
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

  do {
    status = abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET);
  } while (!bitfield_bit32_read(status, KMAC_STATUS_SHA3_IDLE_BIT));

  // 7. Wave 2 Check A: Writing to MSG_FIFO while in StIdle triggers
  // ErrSwPushedMsgFifo (0x02) with info = {8'h00, 8'(StIdle=0x2be),
  // 8'(SelNone=0x14)} = 0x00be14 -> ERR_CODE = 0x0200be14
  // (kmac_app.sv:200,728-734; kmac_pkg.sv:295).
  abs_mmio_write32(kKmacBase + KMAC_MSG_FIFO_REG_OFFSET, 0xdeadbeefu);
  err_code = abs_mmio_read32(kKmacBase + KMAC_ERR_CODE_REG_OFFSET);
  EXPECT_RTL(err_code == 0x0200be14u,
             "ERR_CODE for MSG_FIFO write in StIdle expected 0x0200be14, got "
             "0x%08x",
             err_code);

  // 8. Wave 2 Check B: ERR_CODE is a RO prim_subreg gated only by
  // hw2reg.err_code.de = event_error (kmac.sv:684, kmac_reg_top.sv:2564-2587).
  // Writing CMD.err_processed = 1 does NOT clear ERR_CODE to 0.
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   bitfield_bit32_write(0, KMAC_CMD_ERR_PROCESSED_BIT, true));
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, 0xffffffffu);
  err_code = abs_mmio_read32(kKmacBase + KMAC_ERR_CODE_REG_OFFSET);
  EXPECT_RTL(err_code == 0x0200be14u,
             "ERR_CODE should retain 0x0200be14 after CMD.err_processed, got "
             "0x%08x",
             err_code);

  // 9. Wave 2 Check C: In kmac_errchk.sv:336-356, when err_modestrength (or
  // err_prefix) wins priority over err_entropy_ready on CmdStart, info[23:16]
  // is {5'h0, err_swsequence, err_modestrength, err_prefix} (bit 19 is 0 even
  // when err_entropy_ready is 1).
  // Because step 8 wrote CMD.err_processed = 1, cfg_entropy_ready in
  // kmac_errchk.sv:279 is now 0. Configure KMAC_EN=1, MODE=SHA3(0),
  // KSTRENGTH=L128(0) (invalid mode+strength) with PREFIX_0=0 (invalid KMAC
  // prefix) and without setting ENTROPY_READY:
  //   err_modestrength=1, err_prefix=1, err_entropy_ready=1 ->
  //   code = ErrUnexpectedModeStrength (0x06),
  //   info = {5'h0, 3'b011, 8'h00, 4'h0, 4'h0} = 0x030000 -> 0x06030000.
  abs_mmio_write32(kKmacBase + KMAC_PREFIX_0_REG_OFFSET, 0u);
  uint32_t bad_cfg = 0;
  bad_cfg = bitfield_bit32_write(bad_cfg, KMAC_CFG_SHADOWED_KMAC_EN_BIT, true);
  bad_cfg = bitfield_field32_write(bad_cfg, KMAC_CFG_SHADOWED_KSTRENGTH_FIELD,
                                   KMAC_CFG_SHADOWED_KSTRENGTH_VALUE_L128);
  bad_cfg = bitfield_field32_write(bad_cfg, KMAC_CFG_SHADOWED_MODE_FIELD,
                                   KMAC_CFG_SHADOWED_MODE_VALUE_SHA3);
  write_cfg_shadowed(bad_cfg);
  abs_mmio_write32(
      kKmacBase + KMAC_CMD_REG_OFFSET,
      bitfield_field32_write(0, KMAC_CMD_CMD_FIELD, KMAC_CMD_CMD_VALUE_START));
  err_code = abs_mmio_read32(kKmacBase + KMAC_ERR_CODE_REG_OFFSET);
  EXPECT_RTL(err_code == 0x06030000u,
             "ERR_CODE for err_modestrength+err_prefix+err_entropy_ready "
             "expected 0x06030000, got 0x%08x",
             err_code);

  // 10. Wave 2 Check D: During a SW hashing operation (kmac_app in StSw),
  // an errchecker_err (such as issuing CmdDone while in StMsgFeed) blocks the
  // invalid command and stays in StMsgFeed (STATUS.SHA3_ABSORB=1). Writing
  // CMD.err_processed = 1 while kmac_app is in StSw (not StError) does NOT
  // abort the active SHA3 operation back to Idle.
  write_cfg_shadowed(cfg);
  abs_mmio_write32(
      kKmacBase + KMAC_CMD_REG_OFFSET,
      bitfield_field32_write(0, KMAC_CMD_CMD_FIELD, KMAC_CMD_CMD_VALUE_START));
  do {
    status = abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET);
  } while (!bitfield_bit32_read(status, KMAC_STATUS_SHA3_ABSORB_BIT));

  // Issue invalid CmdDone in StMsgFeed -> ERR_CODE = 0x08040116.
  abs_mmio_write32(
      kKmacBase + KMAC_CMD_REG_OFFSET,
      bitfield_field32_write(0, KMAC_CMD_CMD_FIELD, KMAC_CMD_CMD_VALUE_DONE));
  err_code = abs_mmio_read32(kKmacBase + KMAC_ERR_CODE_REG_OFFSET);
  EXPECT_RTL(err_code == 0x08040116u,
             "ERR_CODE for CmdDone in StMsgFeed expected 0x08040116, got "
             "0x%08x",
             err_code);

  // Write CMD.err_processed = 1: KMAC must remain in StAbsorb (not abort to
  // StIdle).
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   bitfield_bit32_write(0, KMAC_CMD_ERR_PROCESSED_BIT, true));
  status = abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET);
  EXPECT_RTL(bitfield_bit32_read(status, KMAC_STATUS_SHA3_ABSORB_BIT) &&
                 !bitfield_bit32_read(status, KMAC_STATUS_SHA3_IDLE_BIT),
             "KMAC should remain in SHA3_ABSORB after CMD.err_processed in "
             "StSw, got status=0x%08x",
             status);

  // Cleanly finish the operation with CmdProcess -> CmdDone if still in
  // SHA3_ABSORB.
  if (bitfield_bit32_read(status, KMAC_STATUS_SHA3_ABSORB_BIT)) {
    abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                     bitfield_field32_write(0, KMAC_CMD_CMD_FIELD,
                                            KMAC_CMD_CMD_VALUE_PROCESS));
    do {
      status = abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET);
    } while (!bitfield_bit32_read(status, KMAC_STATUS_SHA3_SQUEEZE_BIT));
    abs_mmio_write32(
        kKmacBase + KMAC_CMD_REG_OFFSET,
        bitfield_field32_write(0, KMAC_CMD_CMD_FIELD, KMAC_CMD_CMD_VALUE_DONE));
    do {
      status = abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET);
    } while (!bitfield_bit32_read(status, KMAC_STATUS_SHA3_IDLE_BIT));
  }

  // 11. Wave 3 Check A: Writing to the STATE window (0x400..0x5ff) triggers a
  // TL-UL bus error (d_error=1 -> Ibex Store Access Fault mcause=7) because
  // u_tlul_adapter in kmac_staterd.sv:56 is instantiated with .ErrOnWrite(1).
  uint32_t faults_before = fault_count;
  last_mcause = 0;
  abs_mmio_write32(kKmacBase + KMAC_STATE_REG_OFFSET, 0xdeadbeefu);
  EXPECT_RTL(fault_count == faults_before + 1u && last_mcause == 7u,
             "Write to KMAC STATE window should trigger Store Access Fault "
             "(mcause=7), got fault_delta=%u mcause=%u",
             fault_count - faults_before, last_mcause);

  // 12. Wave 3 Check B: Reading from the MSG_FIFO window (0x800..0xfff)
  // triggers a TL-UL bus error (d_error=1 -> Ibex Load Access Fault mcause=5)
  // because u_tlul_adapter_msgfifo in kmac.sv:999 is instantiated with
  // .ErrOnRead(1).
  faults_before = fault_count;
  last_mcause = 0;
  (void)abs_mmio_read32(kKmacBase + KMAC_MSG_FIFO_REG_OFFSET);
  EXPECT_RTL(fault_count == faults_before + 1u && last_mcause == 5u,
             "Read from KMAC MSG_FIFO window should trigger Load Access Fault "
             "(mcause=5), got fault_delta=%u mcause=%u",
             fault_count - faults_before, last_mcause);

  // 13. Wave 3 Check C: ENTROPY_REFRESH_HASH_CNT (0x28) increments only when
  // KMAC_EN=1 (in_keyblock_i in kmac_entropy.sv:308-314), not when KMAC_EN=0
  // (SHA3/cSHAKE), and resets to 0 when reaching
  // ENTROPY_REFRESH_THRESHOLD_SHADOWED (kmac_entropy.sv:311, 337).
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   bitfield_bit32_write(0, KMAC_CMD_HASH_CNT_CLR_BIT, true));
  uint32_t sha3_cfg = 0;
  sha3_cfg =
      bitfield_bit32_write(sha3_cfg, KMAC_CFG_SHADOWED_KMAC_EN_BIT, false);
  sha3_cfg = bitfield_field32_write(sha3_cfg, KMAC_CFG_SHADOWED_KSTRENGTH_FIELD,
                                    KMAC_CFG_SHADOWED_KSTRENGTH_VALUE_L256);
  sha3_cfg = bitfield_field32_write(sha3_cfg, KMAC_CFG_SHADOWED_MODE_FIELD,
                                    KMAC_CFG_SHADOWED_MODE_VALUE_SHA3);
  write_cfg_shadowed(sha3_cfg);
  abs_mmio_write32(
      kKmacBase + KMAC_CMD_REG_OFFSET,
      bitfield_field32_write(0, KMAC_CMD_CMD_FIELD, KMAC_CMD_CMD_VALUE_START));
  do {
    status = abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET);
  } while (!bitfield_bit32_read(status, KMAC_STATUS_SHA3_ABSORB_BIT));
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   bitfield_field32_write(0, KMAC_CMD_CMD_FIELD,
                                          KMAC_CMD_CMD_VALUE_PROCESS));
  do {
    status = abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET);
  } while (!bitfield_bit32_read(status, KMAC_STATUS_SHA3_SQUEEZE_BIT));
  abs_mmio_write32(
      kKmacBase + KMAC_CMD_REG_OFFSET,
      bitfield_field32_write(0, KMAC_CMD_CMD_FIELD, KMAC_CMD_CMD_VALUE_DONE));
  do {
    status = abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET);
  } while (!bitfield_bit32_read(status, KMAC_STATUS_SHA3_IDLE_BIT));
  EXPECT_RTL(abs_mmio_read32(kKmacBase +
                             KMAC_ENTROPY_REFRESH_HASH_CNT_REG_OFFSET) == 0u,
             "ENTROPY_REFRESH_HASH_CNT must remain 0 after SHA3 (KMAC_EN=0)");

  // 14. Wave 5 Check: KMAC_PERMIT sub-word read/write and addrmiss checks.
  // 14a. Sub-word read (lb) on KMAC_STATUS (0x1c) succeeds without fault.
  faults_before = fault_count;
  uint8_t status_b0 = abs_mmio_read8(kKmacBase + KMAC_STATUS_REG_OFFSET);
  EXPECT_RTL(fault_count == faults_before && (status_b0 & 0x1u) == 0x1u,
             "lb from KMAC_STATUS expected SHA3_IDLE=1 without fault, got "
             "fault_delta=%u status_b0=0x%02x",
             fault_count - faults_before, status_b0);

  // 14b. KMAC_INTR_ENABLE has KMAC_PERMIT = 0x1: sb at byte 0 succeeds,
  // whereas sb at byte 1 faults with mcause=7.
  faults_before = fault_count;
  abs_mmio_write8(kKmacBase + KMAC_INTR_ENABLE_REG_OFFSET, 0x3u);
  EXPECT_RTL(
      fault_count == faults_before &&
          abs_mmio_read32(kKmacBase + KMAC_INTR_ENABLE_REG_OFFSET) == 0x3u,
      "sb at byte 0 of KMAC_INTR_ENABLE (PERMIT=0x1) should succeed");
  faults_before = fault_count;
  last_mcause = 0;
  abs_mmio_write8(kKmacBase + KMAC_INTR_ENABLE_REG_OFFSET + 1u, 0x0u);
  EXPECT_RTL(
      fault_count == faults_before + 1u && last_mcause == 7u &&
          abs_mmio_read32(kKmacBase + KMAC_INTR_ENABLE_REG_OFFSET) == 0x3u,
      "sb at byte 1 of KMAC_INTR_ENABLE (PERMIT=0x1) expected fault(7)");
  abs_mmio_write32(kKmacBase + KMAC_INTR_ENABLE_REG_OFFSET, 0u);

  // 14c. KMAC_PREFIX_0 has KMAC_PERMIT = 0xf: sb at byte 0 faults with mcause=7
  // and does not modify KMAC_PREFIX_0.
  abs_mmio_write32(kKmacBase + KMAC_PREFIX_0_REG_OFFSET, 0x11223344u);
  faults_before = fault_count;
  last_mcause = 0;
  abs_mmio_write8(kKmacBase + KMAC_PREFIX_0_REG_OFFSET, 0x99u);
  EXPECT_RTL(
      fault_count == faults_before + 1u && last_mcause == 7u &&
          abs_mmio_read32(kKmacBase + KMAC_PREFIX_0_REG_OFFSET) == 0x11223344u,
      "sb to KMAC_PREFIX_0 (PERMIT=0xf) expected fault(7) & unchanged");

  // 15. Wave 7 Check: KMAC_STATE sub-word read (`lb`) with STATE_ENDIANNESS=1.
  // In kmac_staterd.sv:90, conv_endian32() swaps the 32-bit word tlram_rdata
  // ({s[0], s[1], s[2], s[3]} = 0xa7ffc6f8 for SHA3-256("")), so lb at byte
  // offsets 0..3 (share0 ^ share1) returns 0xf8, 0xc6, 0xff, 0xa7.
  uint32_t endian_cfg = sha3_cfg;
  endian_cfg = bitfield_bit32_write(
      endian_cfg, KMAC_CFG_SHADOWED_STATE_ENDIANNESS_BIT, true);
  write_cfg_shadowed(endian_cfg);
  abs_mmio_write32(
      kKmacBase + KMAC_CMD_REG_OFFSET,
      bitfield_field32_write(0, KMAC_CMD_CMD_FIELD, KMAC_CMD_CMD_VALUE_START));
  do {
    status = abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET);
  } while (!bitfield_bit32_read(status, KMAC_STATUS_SHA3_ABSORB_BIT));
  abs_mmio_write32(kKmacBase + KMAC_CMD_REG_OFFSET,
                   bitfield_field32_write(0, KMAC_CMD_CMD_FIELD,
                                          KMAC_CMD_CMD_VALUE_PROCESS));
  do {
    status = abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET);
  } while (!bitfield_bit32_read(status, KMAC_STATUS_SHA3_SQUEEZE_BIT));

  uint32_t share0_w0 = abs_mmio_read32(kKmacBase + KMAC_STATE_REG_OFFSET);
  uint32_t share1_w0 =
      abs_mmio_read32(kKmacBase + KMAC_STATE_REG_OFFSET + 0x100u);
  uint32_t state_w0 = share0_w0 ^ share1_w0;
  EXPECT_RTL(share0_w0 != 0u && share1_w0 != 0u && share0_w0 != state_w0,
             "KMAC_STATE Share 0 and Share 1 must both be non-zero masked "
             "shares (EnMasking=1): share0=0x%08x share1=0x%08x",
             share0_w0, share1_w0);
  EXPECT_RTL(
      abs_mmio_read32(kKmacBase + KMAC_STATE_REG_OFFSET + 0x0c8u) == 0u &&
          abs_mmio_read32(kKmacBase + KMAC_STATE_REG_OFFSET + 0x1c8u) == 0u,
      "KMAC_STATE offsets >= 200B (0x0c8, 0x1c8) must read as 0");
  uint32_t state_reconstructed = 0;
  for (uint32_t i = 0; i < 4u; ++i) {
    uint8_t s0_b = abs_mmio_read8(kKmacBase + KMAC_STATE_REG_OFFSET + i);
    uint8_t s1_b =
        abs_mmio_read8(kKmacBase + KMAC_STATE_REG_OFFSET + 0x100u + i);
    EXPECT_RTL(s1_b != 0u,
               "KMAC_STATE Share 1 byte %u should be non-zero, got 0", i);
    uint8_t b = s0_b ^ s1_b;
    state_reconstructed |= ((uint32_t)b << (i * 8u));
  }
  EXPECT_RTL(state_w0 == 0xa7ffc6f8u && state_reconstructed == state_w0,
             "KMAC_STATE sub-word lb with STATE_ENDIANNESS=1 mismatch: "
             "w0=0x%08x reconstructed=0x%08x",
             state_w0, state_reconstructed);

  abs_mmio_write32(
      kKmacBase + KMAC_CMD_REG_OFFSET,
      bitfield_field32_write(0, KMAC_CMD_CMD_FIELD, KMAC_CMD_CMD_VALUE_DONE));
  do {
    status = abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET);
  } while (!bitfield_bit32_read(status, KMAC_STATUS_SHA3_IDLE_BIT));

  // 16. Verify INTR_TEST Status-type (fifo_empty, bit 1) vs Event-type
  // (kmac_done, bit 0) behavior (kmac.sv:660-668).
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, 0x7u);
  abs_mmio_write32(kKmacBase + KMAC_INTR_TEST_REG_OFFSET, 0x3u);
  EXPECT_RTL(
      (abs_mmio_read32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET) & 0x3u) == 0x3u,
      "INTR_TEST=0x3 should set both kmac_done and fifo_empty");
  // Writing W1C to INTR_STATE clears Event-type kmac_done (bit 0) while
  // Status-type fifo_empty (bit 1) remains driven by INTR_TEST.
  abs_mmio_write32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET, 0x3u);
  EXPECT_RTL(
      (abs_mmio_read32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET) & 0x3u) == 0x2u,
      "INTR_STATE W1C should clear kmac_done(0) but keep fifo_empty(1) "
      "while INTR_TEST.fifo_empty=1");
  // Writing INTR_TEST=0 deasserts Status-type fifo_empty (bit 1).
  abs_mmio_write32(kKmacBase + KMAC_INTR_TEST_REG_OFFSET, 0x0u);
  EXPECT_RTL(
      (abs_mmio_read32(kKmacBase + KMAC_INTR_STATE_REG_OFFSET) & 0x3u) == 0x0u,
      "INTR_TEST=0x0 should clear Status-type fifo_empty(1)");

  return all_ok;
}
