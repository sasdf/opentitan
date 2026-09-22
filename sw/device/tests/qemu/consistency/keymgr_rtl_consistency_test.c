// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/dif/dif_flash_ctrl.h"
#include "sw/device/lib/dif/dif_keymgr.h"
#include "sw/device/lib/dif/dif_kmac.h"
#include "sw/device/lib/dif/dif_otp_ctrl.h"
#include "sw/device/lib/dif/dif_rstmgr.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/entropy_testutils.h"
#include "sw/device/lib/testing/flash_ctrl_testutils.h"
#include "sw/device/lib/testing/keymgr_testutils.h"
#include "sw/device/lib/testing/kmac_testutils.h"
#include "sw/device/lib/testing/otp_ctrl_testutils.h"
#include "sw/device/lib/testing/rstmgr_testutils.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "keymgr_regs.h"
#include "kmac_regs.h"

OTTF_DEFINE_TEST_CONFIG(.ignore_alerts = true);

enum {
  kKeymgrBase = TOP_EARLGREY_KEYMGR_BASE_ADDR,
  kKmacBase = TOP_EARLGREY_KMAC_BASE_ADDR,
  kFlashInfoPartitionId = 0,
  kFlashInfoBankId = 0,
  kFlashInfoPageIdCreatorSecret = 1,
  kFlashInfoPageIdOwnerSecret = 2,
};

#define EXPECT_RTL(cond, ...)                  \
  do {                                         \
    if (!(cond)) {                             \
      LOG_ERROR("RTL_MISMATCH: " __VA_ARGS__); \
      all_ok = false;                          \
    }                                          \
  } while (0)

static volatile uint32_t g_fault_count = 0;
static volatile uint32_t g_last_mcause = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  g_fault_count++;
  g_last_mcause = ibex_mcause_read();
}

static void write_shadowed(uint32_t addr, uint32_t val) {
  abs_mmio_write32(addr, val);
  abs_mmio_write32(addr, val);
}

static uint32_t wait_keymgr_op_done_bounded(uint32_t max_iters) {
  uint32_t st = KEYMGR_OP_STATUS_STATUS_VALUE_WIP;
  for (uint32_t i = 0; i < max_iters; ++i) {
    st = abs_mmio_read32(kKeymgrBase + KEYMGR_OP_STATUS_REG_OFFSET) &
         KEYMGR_OP_STATUS_STATUS_MASK;
    if (st != KEYMGR_OP_STATUS_STATUS_VALUE_WIP) {
      break;
    }
    busy_spin_micros(5);
  }
  return st;
}

static void clear_keymgr_status_and_err(void) {
  uint32_t op_st = abs_mmio_read32(kKeymgrBase + KEYMGR_OP_STATUS_REG_OFFSET);
  abs_mmio_write32(kKeymgrBase + KEYMGR_OP_STATUS_REG_OFFSET, op_st);
  uint32_t err = abs_mmio_read32(kKeymgrBase + KEYMGR_ERR_CODE_REG_OFFSET);
  abs_mmio_write32(kKeymgrBase + KEYMGR_ERR_CODE_REG_OFFSET, err);
  uint32_t intr = abs_mmio_read32(kKeymgrBase + KEYMGR_INTR_STATE_REG_OFFSET);
  abs_mmio_write32(kKeymgrBase + KEYMGR_INTR_STATE_REG_OFFSET, intr);
}

static status_t init_flash_and_otp_with_zero_owner_seed(void) {
  dif_flash_ctrl_state_t flash;
  dif_rstmgr_t rstmgr;
  dif_otp_ctrl_t otp_ctrl;

  TRY(dif_rstmgr_init(mmio_region_from_addr(TOP_EARLGREY_RSTMGR_AON_BASE_ADDR),
                      &rstmgr));
  const dif_rstmgr_reset_info_bitfield_t reset_info =
      rstmgr_testutils_reason_get();

  if (reset_info == kDifRstmgrResetInfoPor) {
    TRY(dif_flash_ctrl_init_state(
        &flash, mmio_region_from_addr(TOP_EARLGREY_FLASH_CTRL_CORE_BASE_ADDR)));
    TRY(dif_otp_ctrl_init(
        mmio_region_from_addr(TOP_EARLGREY_OTP_CTRL_CORE_BASE_ADDR),
        &otp_ctrl));

    bool secret2_computed = false;
    TRY(dif_otp_ctrl_is_digest_computed(&otp_ctrl, kDifOtpCtrlPartitionSecret2,
                                        &secret2_computed));

    const keymgr_testutils_secret_t *creator_secret = NULL;
    if (!secret2_computed) {
      creator_secret = &kCreatorSecret;
    }
    // Program OwnerSecret to all-zeros so that advancing into OwnerKey is
    // guaranteed to fail keymgr_input_checks (valid_chk rejects all-0s/all-1s).
    const keymgr_testutils_secret_t kZeroOwnerSecret = {.value = {0}};
    TRY(keymgr_testutils_flash_init(&flash, creator_secret, &kZeroOwnerSecret));

    if (!secret2_computed) {
      // When SECRET2 digest is 0 (unlocked, e.g. in TEST_UNLOCKED0 where
      // SEED_HW_RD_EN == 0), advancing keymgr from StReset fetches an invalid
      // root key (secret.valid == false in ot_otp_eg.c) and transitions
      // WORKING_STATE to INVALID (KEYMGR_ST_WIPE -> KEYMGR_ST_INVALID).
      TRY(entropy_testutils_auto_mode_init());
      uint32_t ctrl_adv = bitfield_field32_write(
          0, KEYMGR_CONTROL_SHADOWED_OPERATION_FIELD,
          KEYMGR_CONTROL_SHADOWED_OPERATION_VALUE_ADVANCE);
      write_shadowed(kKeymgrBase + KEYMGR_CONTROL_SHADOWED_REG_OFFSET,
                     ctrl_adv);
      abs_mmio_write32(kKeymgrBase + KEYMGR_START_REG_OFFSET, 1u);
      (void)wait_keymgr_op_done_bounded(5000);
      uint32_t ws_inval =
          abs_mmio_read32(kKeymgrBase + KEYMGR_WORKING_STATE_REG_OFFSET);
      TRY_CHECK(ws_inval == KEYMGR_WORKING_STATE_STATE_VALUE_INVALID,
                "Expected WORKING_STATE=INVALID(7) when SECRET2 is unlocked, "
                "got %u",
                ws_inval);
      clear_keymgr_status_and_err();

      TRY(otp_ctrl_testutils_lock_partition(&otp_ctrl,
                                            kDifOtpCtrlPartitionSecret2, 0));
    }

    rstmgr_testutils_reason_clear();
    TRY(dif_rstmgr_software_device_reset(&rstmgr));
    wait_for_interrupt();
    return INTERNAL();
  }
  return OK_STATUS();
}

static bool test_kmac_sideload_key_valid(dif_kmac_t *kmac) {
  static const dif_kmac_key_t kDummySwKey = {
      .share0 = {0x43424140, 0x47464544, 0x4B4A4948, 0x4F4E4D4C, 0x53525150,
                 0x57565554, 0x5B5A5958, 0x5F5E5D5C},
      .share1 = {0},
      .length = kDifKmacKeyLen256,
  };
  CHECK_STATUS_OK(kmac_testutils_config(kmac, /*sideload=*/true));
  uint32_t out_buf[8] = {0};
  status_t res =
      kmac_testutils_kmac(kmac, kDifKmacModeKmacLen256, &kDummySwKey, "", 0,
                          "abcd", 4, ARRAYSIZE(out_buf), out_buf, NULL);
  // Wait for KMAC SHA3 core to return to IDLE after error or completion.
  for (int i = 0; i < 100; ++i) {
    uint32_t st = abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET);
    if (bitfield_bit32_read(st, KMAC_STATUS_SHA3_IDLE_BIT)) {
      break;
    }
    busy_spin_micros(5);
  }
  CHECK_STATUS_OK(kmac_testutils_config(kmac, /*sideload=*/false));
  return status_ok(res);
}

bool test_main(void) {
  bool all_ok = true;

  uint32_t ws = abs_mmio_read32(kKeymgrBase + KEYMGR_WORKING_STATE_REG_OFFSET);
  if (ws == KEYMGR_WORKING_STATE_STATE_VALUE_RESET) {
    CHECK_STATUS_OK(init_flash_and_otp_with_zero_owner_seed());
  }
  CHECK_STATUS_OK(entropy_testutils_auto_mode_init());

  dif_kmac_t kmac;
  CHECK_DIF_OK(
      dif_kmac_init(mmio_region_from_addr(TOP_EARLGREY_KMAC_BASE_ADDR), &kmac));
  CHECK_STATUS_OK(kmac_testutils_config(&kmac, /*sideload=*/false));

  // ---------------------------------------------------------------------------
  // Check 1: In StReset (WORKING_STATE == 0), non-ADVANCE operation triggers
  // immediate INVALID_OP (OP_STATUS = DONE_ERROR, ERR_CODE.INVALID_OP = 1)
  // and SW_BINDING_REGWEN locked in Reset remains 0 across Reset -> Init.
  // ---------------------------------------------------------------------------
  ws = abs_mmio_read32(kKeymgrBase + KEYMGR_WORKING_STATE_REG_OFFSET);
  uint32_t ctrl_gen_sw = bitfield_field32_write(
      0, KEYMGR_CONTROL_SHADOWED_OPERATION_FIELD,
      KEYMGR_CONTROL_SHADOWED_OPERATION_VALUE_GENERATE_SW_OUTPUT);
  uint32_t ctrl_adv =
      bitfield_field32_write(0, KEYMGR_CONTROL_SHADOWED_OPERATION_FIELD,
                             KEYMGR_CONTROL_SHADOWED_OPERATION_VALUE_ADVANCE);
  uint32_t op_st = 0;
  uint32_t err_code = 0;

  if (ws == KEYMGR_WORKING_STATE_STATE_VALUE_RESET) {
    // Lock SW_BINDING_REGWEN in Reset (ROM also locks this).
    abs_mmio_write32(kKeymgrBase + KEYMGR_SW_BINDING_REGWEN_REG_OFFSET, 0u);
    EXPECT_RTL(abs_mmio_read32(kKeymgrBase +
                               KEYMGR_SW_BINDING_REGWEN_REG_OFFSET) == 0u,
               "SW_BINDING_REGWEN should be 0 in Reset after write 0");

    // Trigger invalid op (GENERATE_SW_OUTPUT) in Reset.
    write_shadowed(kKeymgrBase + KEYMGR_CONTROL_SHADOWED_REG_OFFSET,
                   ctrl_gen_sw);
    abs_mmio_write32(kKeymgrBase + KEYMGR_START_REG_OFFSET, 1u);
    op_st = wait_keymgr_op_done_bounded(1000);
    err_code = abs_mmio_read32(kKeymgrBase + KEYMGR_ERR_CODE_REG_OFFSET);
    EXPECT_RTL(op_st == KEYMGR_OP_STATUS_STATUS_VALUE_DONE_ERROR &&
                   err_code == (1u << KEYMGR_ERR_CODE_INVALID_OP_BIT),
               "GenSwOut in Reset expected DONE_ERROR(3) & INVALID_OP(1), got "
               "op_st=%u err=0x%x",
               op_st, err_code);
    clear_keymgr_status_and_err();

    // Advance Reset -> Init.
    write_shadowed(kKeymgrBase + KEYMGR_CONTROL_SHADOWED_REG_OFFSET, ctrl_adv);
    abs_mmio_write32(kKeymgrBase + KEYMGR_START_REG_OFFSET, 1u);
    op_st = wait_keymgr_op_done_bounded(5000);
    EXPECT_RTL(op_st == KEYMGR_OP_STATUS_STATUS_VALUE_DONE_SUCCESS,
               "Advance Reset->Init expected DONE_SUCCESS(2), got %u", op_st);
    clear_keymgr_status_and_err();

    ws = abs_mmio_read32(kKeymgrBase + KEYMGR_WORKING_STATE_REG_OFFSET);
    EXPECT_RTL(ws == KEYMGR_WORKING_STATE_STATE_VALUE_INIT,
               "After Reset->Init, WORKING_STATE expected INIT(1), got %u", ws);
    // In RTL (keymgr.sv:371), sw_binding_clr = op_done & adv_en & ~err &
    // ~fault. Because Reset->Init does not assert adv_en (no KMAC KDF),
    // SW_BINDING_REGWEN remains 0 in Init until Init->CreatorRootKey completes.
    uint32_t sw_bind_regwen =
        abs_mmio_read32(kKeymgrBase + KEYMGR_SW_BINDING_REGWEN_REG_OFFSET);
    EXPECT_RTL(sw_bind_regwen == 0u,
               "In Init after Reset->Init, SW_BINDING_REGWEN expected 0, got "
               "%u",
               sw_bind_regwen);

    // Advance Init -> CreatorRootKey.
    write_shadowed(kKeymgrBase + KEYMGR_CONTROL_SHADOWED_REG_OFFSET, ctrl_adv);
    abs_mmio_write32(kKeymgrBase + KEYMGR_START_REG_OFFSET, 1u);
    op_st = wait_keymgr_op_done_bounded(5000);
    EXPECT_RTL(op_st == KEYMGR_OP_STATUS_STATUS_VALUE_DONE_SUCCESS,
               "Advance Init->CreatorRootKey expected DONE_SUCCESS(2), got %u",
               op_st);
    clear_keymgr_status_and_err();

    ws = abs_mmio_read32(kKeymgrBase + KEYMGR_WORKING_STATE_REG_OFFSET);
    EXPECT_RTL(ws == KEYMGR_WORKING_STATE_STATE_VALUE_CREATOR_ROOT_KEY,
               "After Init->CreatorRootKey, WORKING_STATE expected "
               "CREATOR_ROOT_KEY(2), got %u",
               ws);
    sw_bind_regwen =
        abs_mmio_read32(kKeymgrBase + KEYMGR_SW_BINDING_REGWEN_REG_OFFSET);
    EXPECT_RTL(sw_bind_regwen == 1u,
               "After Init->CreatorRootKey, SW_BINDING_REGWEN expected 1, got "
               "%u",
               sw_bind_regwen);
  } else {
    // Under sival_rom_ext, ROM_EXT advances keymgr to OwnerKey (4).
    CHECK(ws == KEYMGR_WORKING_STATE_STATE_VALUE_OWNER_KEY ||
          ws == KEYMGR_WORKING_STATE_STATE_VALUE_OWNER_INTERMEDIATE_KEY);
  }

  // ---------------------------------------------------------------------------
  // Check 2: In CreatorRootKey (or OwnerKey under ROM_EXT), GEN_SW_OUT with
  // valid KEY_VERSION updates SW_SHARE0/1_OUTPUT (which are RC), whereas
  // GEN_SW_OUT with invalid KEY_VERSION (> max_key_ver) fails with
  // INVALID_KMAC_INPUT and MUST NOT update SW_SHARE0/1_OUTPUT (in RTL
  // data_valid_o = ~op_err).
  // ---------------------------------------------------------------------------
  uint32_t max_ver_reg_offset =
      (ws == KEYMGR_WORKING_STATE_STATE_VALUE_OWNER_KEY)
          ? KEYMGR_MAX_OWNER_KEY_VER_SHADOWED_REG_OFFSET
      : (ws == KEYMGR_WORKING_STATE_STATE_VALUE_OWNER_INTERMEDIATE_KEY)
          ? KEYMGR_MAX_OWNER_INT_KEY_VER_SHADOWED_REG_OFFSET
          : KEYMGR_MAX_CREATOR_KEY_VER_SHADOWED_REG_OFFSET;
  uint32_t max_creator_ver = abs_mmio_read32(kKeymgrBase + max_ver_reg_offset);
  abs_mmio_write32(kKeymgrBase + KEYMGR_KEY_VERSION_REG_OFFSET,
                   max_creator_ver);
  abs_mmio_write32(kKeymgrBase + KEYMGR_SALT_0_REG_OFFSET, 0x11223344u);
  write_shadowed(kKeymgrBase + KEYMGR_CONTROL_SHADOWED_REG_OFFSET, ctrl_gen_sw);
  abs_mmio_write32(kKeymgrBase + KEYMGR_START_REG_OFFSET, 1u);
  op_st = wait_keymgr_op_done_bounded(5000);
  EXPECT_RTL(op_st == KEYMGR_OP_STATUS_STATUS_VALUE_DONE_SUCCESS,
             "Valid GenSwOut expected DONE_SUCCESS(2), got %u", op_st);
  clear_keymgr_status_and_err();

  uint32_t share0_vld =
      abs_mmio_read32(kKeymgrBase + KEYMGR_SW_SHARE0_OUTPUT_0_REG_OFFSET);
  uint32_t share0_rc =
      abs_mmio_read32(kKeymgrBase + KEYMGR_SW_SHARE0_OUTPUT_0_REG_OFFSET);
  uint32_t share1_vld =
      abs_mmio_read32(kKeymgrBase + KEYMGR_SW_SHARE1_OUTPUT_0_REG_OFFSET);
  uint32_t share1_rc =
      abs_mmio_read32(kKeymgrBase + KEYMGR_SW_SHARE1_OUTPUT_0_REG_OFFSET);
  EXPECT_RTL(share0_vld != 0u && share0_rc == 0u && share1_vld != 0u &&
                 share1_rc == 0u && share0_vld != share1_vld,
             "SW_SHARE0/1_OUTPUT_0 expected non-zero distinct shares then 0 on "
             "RC read, got s0=0x%08x->0x%08x s1=0x%08x->0x%08x",
             share0_vld, share0_rc, share1_vld, share1_rc);
  // Read-clear all remaining SW_SHARE0/1_OUTPUT words so all 8 words are 0.
  for (uint32_t i = 0; i < 8; ++i) {
    (void)abs_mmio_read32(kKeymgrBase + KEYMGR_SW_SHARE0_OUTPUT_0_REG_OFFSET +
                          i * 4);
    (void)abs_mmio_read32(kKeymgrBase + KEYMGR_SW_SHARE1_OUTPUT_0_REG_OFFSET +
                          i * 4);
  }

  // Now run GEN_SW_OUT with invalid KEY_VERSION = max_creator_ver + 1.
  abs_mmio_write32(kKeymgrBase + KEYMGR_KEY_VERSION_REG_OFFSET,
                   max_creator_ver + 1u);
  abs_mmio_write32(kKeymgrBase + KEYMGR_SALT_0_REG_OFFSET, 0x55667788u);
  write_shadowed(kKeymgrBase + KEYMGR_CONTROL_SHADOWED_REG_OFFSET, ctrl_gen_sw);
  abs_mmio_write32(kKeymgrBase + KEYMGR_START_REG_OFFSET, 1u);
  op_st = wait_keymgr_op_done_bounded(5000);
  err_code = abs_mmio_read32(kKeymgrBase + KEYMGR_ERR_CODE_REG_OFFSET);
  EXPECT_RTL(op_st == KEYMGR_OP_STATUS_STATUS_VALUE_DONE_ERROR &&
                 err_code == (1u << KEYMGR_ERR_CODE_INVALID_KMAC_INPUT_BIT),
             "Invalid KEY_VERSION GenSwOut expected DONE_ERROR(3) & "
             "INVALID_KMAC_INPUT(2), got op_st=%u err=0x%x",
             op_st, err_code);
  clear_keymgr_status_and_err();

  uint32_t leaked_s0 =
      abs_mmio_read32(kKeymgrBase + KEYMGR_SW_SHARE0_OUTPUT_0_REG_OFFSET);
  uint32_t leaked_s1 =
      abs_mmio_read32(kKeymgrBase + KEYMGR_SW_SHARE1_OUTPUT_0_REG_OFFSET);
  EXPECT_RTL(leaked_s0 == 0u && leaked_s1 == 0u,
             "GenSwOut with INVALID_KMAC_INPUT must NOT update "
             "SW_SHARE0/1_OUTPUT (expected 0, got share0=0x%08x share1=0x%08x)",
             leaked_s0, leaked_s1);
  for (uint32_t i = 0; i < 8; ++i) {
    (void)abs_mmio_read32(kKeymgrBase + KEYMGR_SW_SHARE0_OUTPUT_0_REG_OFFSET +
                          i * 4);
    (void)abs_mmio_read32(kKeymgrBase + KEYMGR_SW_SHARE1_OUTPUT_0_REG_OFFSET +
                          i * 4);
  }

  // ---------------------------------------------------------------------------
  // Check 3: GEN_HW_OUT to KMAC with invalid KEY_VERSION (> max_creator_ver)
  // must NOT overwrite or invalidate a previously valid KMAC sideload key
  // (in RTL keymgr_sideload_key.sv, set_i = data_valid_i & slot_sel[KmacIdx]
  // is 0 when op_err == 1, so valid_q remains 1).
  // ---------------------------------------------------------------------------
  abs_mmio_write32(kKeymgrBase + KEYMGR_SIDELOAD_CLEAR_REG_OFFSET,
                   KEYMGR_SIDELOAD_CLEAR_VAL_VALUE_NONE);
  abs_mmio_write32(kKeymgrBase + KEYMGR_KEY_VERSION_REG_OFFSET,
                   max_creator_ver);
  uint32_t ctrl_gen_hw_kmac =
      bitfield_field32_write(
          0, KEYMGR_CONTROL_SHADOWED_OPERATION_FIELD,
          KEYMGR_CONTROL_SHADOWED_OPERATION_VALUE_GENERATE_HW_OUTPUT) |
      bitfield_field32_write(0, KEYMGR_CONTROL_SHADOWED_DEST_SEL_FIELD,
                             KEYMGR_CONTROL_SHADOWED_DEST_SEL_VALUE_KMAC);
  write_shadowed(kKeymgrBase + KEYMGR_CONTROL_SHADOWED_REG_OFFSET,
                 ctrl_gen_hw_kmac);
  abs_mmio_write32(kKeymgrBase + KEYMGR_START_REG_OFFSET, 1u);
  op_st = wait_keymgr_op_done_bounded(5000);
  EXPECT_RTL(op_st == KEYMGR_OP_STATUS_STATUS_VALUE_DONE_SUCCESS,
             "Valid GenHwOut(KMAC) expected DONE_SUCCESS(2), got %u", op_st);
  clear_keymgr_status_and_err();
  EXPECT_RTL(test_kmac_sideload_key_valid(&kmac),
             "KMAC sideload key should be valid after valid GenHwOut(KMAC)");

  // Now run invalid GenHwOut(KMAC) with KEY_VERSION = max_creator_ver + 1.
  abs_mmio_write32(kKeymgrBase + KEYMGR_KEY_VERSION_REG_OFFSET,
                   max_creator_ver + 1u);
  write_shadowed(kKeymgrBase + KEYMGR_CONTROL_SHADOWED_REG_OFFSET,
                 ctrl_gen_hw_kmac);
  abs_mmio_write32(kKeymgrBase + KEYMGR_START_REG_OFFSET, 1u);
  op_st = wait_keymgr_op_done_bounded(5000);
  err_code = abs_mmio_read32(kKeymgrBase + KEYMGR_ERR_CODE_REG_OFFSET);
  EXPECT_RTL(op_st == KEYMGR_OP_STATUS_STATUS_VALUE_DONE_ERROR &&
                 err_code == (1u << KEYMGR_ERR_CODE_INVALID_KMAC_INPUT_BIT),
             "Invalid GenHwOut(KMAC) expected DONE_ERROR(3) & "
             "INVALID_KMAC_INPUT(2), got op_st=%u err=0x%x",
             op_st, err_code);
  clear_keymgr_status_and_err();

  bool kmac_valid_after_err = test_kmac_sideload_key_valid(&kmac);
  EXPECT_RTL(kmac_valid_after_err,
             "KMAC sideload key must remain valid after failed GenHwOut(KMAC) "
             "(data_valid_i=0 in RTL does not clear valid_q), got valid=%d",
             kmac_valid_after_err);

  // ---------------------------------------------------------------------------
  // Check 4: SIDELOAD_CLEAR is a continuous level-clear signal in RTL
  // (keymgr_sideload_key.sv:35 `else if (!en_i || clr_i) valid_q <= 1'b0`).
  // If SIDELOAD_CLEAR = KMAC (2) is left active during a valid GenHwOut(KMAC),
  // clr_i overrides set_i so valid_q stays 0 even after SIDELOAD_CLEAR is later
  // returned to NONE (0).
  // ---------------------------------------------------------------------------
  abs_mmio_write32(kKeymgrBase + KEYMGR_SIDELOAD_CLEAR_REG_OFFSET,
                   KEYMGR_SIDELOAD_CLEAR_VAL_VALUE_KMAC);
  abs_mmio_write32(kKeymgrBase + KEYMGR_KEY_VERSION_REG_OFFSET,
                   max_creator_ver);
  write_shadowed(kKeymgrBase + KEYMGR_CONTROL_SHADOWED_REG_OFFSET,
                 ctrl_gen_hw_kmac);
  abs_mmio_write32(kKeymgrBase + KEYMGR_START_REG_OFFSET, 1u);
  op_st = wait_keymgr_op_done_bounded(5000);
  EXPECT_RTL(op_st == KEYMGR_OP_STATUS_STATUS_VALUE_DONE_SUCCESS,
             "GenHwOut(KMAC) while SIDELOAD_CLEAR=KMAC expected "
             "DONE_SUCCESS(2), got %u",
             op_st);
  clear_keymgr_status_and_err();

  // Now release SIDELOAD_CLEAR back to NONE (0) and verify the KMAC sideload
  // slot was NOT populated while SIDELOAD_CLEAR was held at KMAC.
  abs_mmio_write32(kKeymgrBase + KEYMGR_SIDELOAD_CLEAR_REG_OFFSET,
                   KEYMGR_SIDELOAD_CLEAR_VAL_VALUE_NONE);
  bool kmac_valid_while_clr = test_kmac_sideload_key_valid(&kmac);
  EXPECT_RTL(!kmac_valid_while_clr,
             "GenHwOut(KMAC) executed while SIDELOAD_CLEAR=KMAC must leave "
             "sideload key invalid (valid_q=0 in RTL), got valid=%d",
             kmac_valid_while_clr);

  // ---------------------------------------------------------------------------
  // Check 5: ADVANCE with invalid flash seed (ERR_CODE.INVALID_KMAC_INPUT)
  // must NOT advance WORKING_STATE and must NOT unlock SW_BINDING_REGWEN
  // (in RTL keymgr_ctrl.sv:185 `adv_state = ... & ~op_err & ~op_fault_err` and
  // keymgr.sv:371 `sw_binding_clr = op_done & adv_en & ~(|err_code)`).
  // ---------------------------------------------------------------------------
  uint32_t pre_adv_ws =
      abs_mmio_read32(kKeymgrBase + KEYMGR_WORKING_STATE_REG_OFFSET);
  if (pre_adv_ws == KEYMGR_WORKING_STATE_STATE_VALUE_CREATOR_ROOT_KEY) {
    abs_mmio_write32(kKeymgrBase + KEYMGR_DEBUG_REG_OFFSET, 0u);
    abs_mmio_write32(kKeymgrBase + KEYMGR_SW_BINDING_REGWEN_REG_OFFSET, 0u);
    write_shadowed(kKeymgrBase + KEYMGR_CONTROL_SHADOWED_REG_OFFSET, ctrl_adv);
    abs_mmio_write32(kKeymgrBase + KEYMGR_START_REG_OFFSET, 1u);
    op_st = wait_keymgr_op_done_bounded(5000);

    // In RTL (keymgr.sv:460, 538, 550), during CreatorRootKey -> OwnerIntKey
    // (stage_sel == OwnerInt), adv_matrix[OwnerInt] hashes creator_seed, while
    // adv_dvalid[OwnerInt] = owner_seed_vld and
    // hw2reg.debug.invalid_owner_seed.de = adv_en & (stage_sel == OwnerInt) &
    // ~owner_seed_vld. Because OwnerSeed is all-zero, this ADVANCE in
    // CreatorRootKey MUST immediately fail with DONE_ERROR(3),
    // ERR_CODE.INVALID_KMAC_INPUT(2), DEBUG.INVALID_OWNER_SEED(2),
    // WORKING_STATE remaining CREATOR_ROOT_KEY(2), and SW_BINDING_REGWEN
    // remaining 0.
    err_code = abs_mmio_read32(kKeymgrBase + KEYMGR_ERR_CODE_REG_OFFSET);
    uint32_t debug_val = abs_mmio_read32(kKeymgrBase + KEYMGR_DEBUG_REG_OFFSET);
    uint32_t post_adv_ws =
        abs_mmio_read32(kKeymgrBase + KEYMGR_WORKING_STATE_REG_OFFSET);
    uint32_t post_adv_bind_regwen =
        abs_mmio_read32(kKeymgrBase + KEYMGR_SW_BINDING_REGWEN_REG_OFFSET);
    EXPECT_RTL(op_st == KEYMGR_OP_STATUS_STATUS_VALUE_DONE_ERROR &&
                   err_code == (1u << KEYMGR_ERR_CODE_INVALID_KMAC_INPUT_BIT),
               "Advance CreatorRootKey->OwnerIntKey with zero OwnerSeed "
               "expected DONE_ERROR(3) & INVALID_KMAC_INPUT(2), got op_st=%u "
               "err=0x%x",
               op_st, err_code);
    EXPECT_RTL(debug_val == (1u << KEYMGR_DEBUG_INVALID_OWNER_SEED_BIT),
               "DEBUG after CreatorRootKey->OwnerIntKey with zero OwnerSeed "
               "expected INVALID_OWNER_SEED (0x2), got 0x%x",
               debug_val);
    EXPECT_RTL(post_adv_ws == KEYMGR_WORKING_STATE_STATE_VALUE_CREATOR_ROOT_KEY,
               "Failed ADVANCE (INVALID_KMAC_INPUT) must stay in "
               "CREATOR_ROOT_KEY(2), got %u",
               post_adv_ws);
    EXPECT_RTL(post_adv_bind_regwen == 0u,
               "Failed ADVANCE (INVALID_KMAC_INPUT) must NOT unlock "
               "SW_BINDING_REGWEN (expected 0, got %u)",
               post_adv_bind_regwen);
    clear_keymgr_status_and_err();
  }

  // ---------------------------------------------------------------------------
  // Check 6: Disable keymgr (OPERATION = DISABLE), then issue START=1 in
  // WORKING_STATE = Disabled (5). In RTL (keymgr_ctrl.sv:652-659, 247-248,
  // keymgr_err.sv:71-74), StCtrlDisabled runs op_req = op_start_i with
  // disabled = 1 and finishes with OP_STATUS = DONE_ERROR (3),
  // ERR_CODE.INVALID_OP = 1, START = 0, CFG_REGWEN = 1 (does NOT hang in WIP).
  // ---------------------------------------------------------------------------
  uint32_t ctrl_dis =
      bitfield_field32_write(0, KEYMGR_CONTROL_SHADOWED_OPERATION_FIELD,
                             KEYMGR_CONTROL_SHADOWED_OPERATION_VALUE_DISABLE);
  write_shadowed(kKeymgrBase + KEYMGR_CONTROL_SHADOWED_REG_OFFSET, ctrl_dis);
  abs_mmio_write32(kKeymgrBase + KEYMGR_START_REG_OFFSET, 1u);
  op_st = wait_keymgr_op_done_bounded(5000);
  EXPECT_RTL(op_st == KEYMGR_OP_STATUS_STATUS_VALUE_DONE_SUCCESS,
             "DISABLE operation expected DONE_SUCCESS(2), got %u", op_st);
  clear_keymgr_status_and_err();

  ws = abs_mmio_read32(kKeymgrBase + KEYMGR_WORKING_STATE_REG_OFFSET);
  EXPECT_RTL(ws == KEYMGR_WORKING_STATE_STATE_VALUE_DISABLED,
             "After DISABLE, WORKING_STATE expected DISABLED(5), got %u", ws);

  write_shadowed(kKeymgrBase + KEYMGR_CONTROL_SHADOWED_REG_OFFSET, ctrl_gen_sw);
  abs_mmio_write32(kKeymgrBase + KEYMGR_START_REG_OFFSET, 1u);
  op_st = wait_keymgr_op_done_bounded(5000);
  err_code = abs_mmio_read32(kKeymgrBase + KEYMGR_ERR_CODE_REG_OFFSET);
  uint32_t start_reg = abs_mmio_read32(kKeymgrBase + KEYMGR_START_REG_OFFSET);
  uint32_t cfg_regwen =
      abs_mmio_read32(kKeymgrBase + KEYMGR_CFG_REGWEN_REG_OFFSET);
  EXPECT_RTL(op_st == KEYMGR_OP_STATUS_STATUS_VALUE_DONE_ERROR &&
                 err_code == (1u << KEYMGR_ERR_CODE_INVALID_OP_BIT) &&
                 start_reg == 0u && cfg_regwen == 1u,
             "START=1 in Disabled state expected DONE_ERROR(3), INVALID_OP(1), "
             "START=0, CFG_REGWEN=1; got op_st=%u err=0x%x start=%u "
             "cfg_regwen=%u",
             op_st, err_code, start_reg, cfg_regwen);
  clear_keymgr_status_and_err();

  // ---------------------------------------------------------------------------
  // Check 7: Wave 5 KEYMGR_PERMIT sub-word write wr_err and addrmiss checks
  // ---------------------------------------------------------------------------
  // 7a. SALT_0 has KEYMGR_PERMIT = 0xf (4'b1111): sub-word sb/sh writes MUST
  // fault with mcause=7 (Store/AMO access fault) and MUST NOT modify SALT_0.
  abs_mmio_write32(kKeymgrBase + KEYMGR_SALT_0_REG_OFFSET, 0xa5a5a5a5u);
  g_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write8(kKeymgrBase + KEYMGR_SALT_0_REG_OFFSET, 0x12u);
  EXPECT_RTL(g_fault_count == 1u && g_last_mcause == 7u &&
                 abs_mmio_read32(kKeymgrBase + KEYMGR_SALT_0_REG_OFFSET) ==
                     0xa5a5a5a5u,
             "sb to SALT_0 (PERMIT=0xf) expected fault(mcause=7) & unchanged "
             "0xa5a5a5a5, got faults=%u mcause=%u val=0x%08x",
             g_fault_count, g_last_mcause,
             abs_mmio_read32(kKeymgrBase + KEYMGR_SALT_0_REG_OFFSET));

  // 7b. CONTROL_SHADOWED has KEYMGR_PERMIT = 0x3 (4'b0011): sb at byte 0 MUST
  // fault with mcause=7, whereas sh at halfword 0 (reg_be=0x3) is permitted.
  g_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write8(kKeymgrBase + KEYMGR_CONTROL_SHADOWED_REG_OFFSET, 0x10u);
  EXPECT_RTL(g_fault_count == 1u && g_last_mcause == 7u,
             "sb to CONTROL_SHADOWED (PERMIT=0x3) expected fault(mcause=7), "
             "got faults=%u mcause=%u",
             g_fault_count, g_last_mcause);

  // 7c. INTR_ENABLE has KEYMGR_PERMIT = 0x1 (4'b0001): sb at byte 0
  // (reg_be=0x1) is permitted, whereas sb at byte 1 (reg_be=0x2) MUST fault
  // with mcause=7.
  g_fault_count = 0;
  abs_mmio_write8(kKeymgrBase + KEYMGR_INTR_ENABLE_REG_OFFSET, 0x1u);
  EXPECT_RTL(
      g_fault_count == 0u &&
          abs_mmio_read32(kKeymgrBase + KEYMGR_INTR_ENABLE_REG_OFFSET) == 0x1u,
      "sb at byte 0 of INTR_ENABLE (PERMIT=0x1) should succeed, got "
      "faults=%u val=0x%x",
      g_fault_count,
      abs_mmio_read32(kKeymgrBase + KEYMGR_INTR_ENABLE_REG_OFFSET));
  abs_mmio_write8(kKeymgrBase + KEYMGR_INTR_ENABLE_REG_OFFSET + 1u, 0x0u);
  EXPECT_RTL(
      g_fault_count == 1u && g_last_mcause == 7u &&
          abs_mmio_read32(kKeymgrBase + KEYMGR_INTR_ENABLE_REG_OFFSET) == 0x1u,
      "sb at byte 1 of INTR_ENABLE (PERMIT=0x1) expected fault(mcause=7)"
      " & unchanged 0x1, got faults=%u mcause=%u val=0x%x",
      g_fault_count, g_last_mcause,
      abs_mmio_read32(kKeymgrBase + KEYMGR_INTR_ENABLE_REG_OFFSET));
  abs_mmio_write32(kKeymgrBase + KEYMGR_INTR_ENABLE_REG_OFFSET, 0x0u);

  // 7d. Out-of-bounds offset 0xfc (past DEBUG at 0xf8) MUST fault on
  // read/write.
  g_fault_count = 0;
  (void)abs_mmio_read32(kKeymgrBase + 0xfcu);
  EXPECT_RTL(g_fault_count == 1u && g_last_mcause == 5u,
             "read at 0xfc (addrmiss) expected fault(mcause=5), got faults=%u "
             "mcause=%u",
             g_fault_count, g_last_mcause);

  CHECK(all_ok);
  return true;
}
