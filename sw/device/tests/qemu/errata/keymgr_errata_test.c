// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file keymgr_errata_test.c
 * @brief CW340 FPGA & QEMU Empirical Errata Confirmation Test for KEYMGR.
 *
 * Empirically confirms the software-visible hardware errata, specification
 * discrepancies, and security hardening behaviors documented in
 * `/root/knowledge/errata/keymgr.md`:
 * - [keymgr.sv:436-464] (TRUE_SILICON_ERRATA): Off-by-one stage index in
 *   `adv_dvalid` seed validation (`keymgr.sv:436-464, 537-538`): advancing
 *   `CreatorRootKey -> OwnerIntKey` checks `owner_seed_vld` (Flash Info Page 2)
 *   instead of `creator_seed_vld` (Flash Info Page 1), setting
 *   `DEBUG.INVALID_OWNER_SEED` (bit 1) and `ERR_CODE.INVALID_KMAC_INPUT` (bit
 * 2) while preserving `WORKING_STATE = CreatorRootKey (2)` when `OwnerSeed` is
 *   all-zeros (`0x00...00`).
 * - [keymgr.sv:369-370] & [keymgr_ctrl.sv:185] (INTENDED_SECURITY_HARDENING):
 *   `SW_BINDING_REGWEN` stays locked (`0`) when `ADVANCE` fails with
 *   `INVALID_KMAC_INPUT`, and only unlocks (`1`) after a valid `ADVANCE`
 *   succeeds (`DONE_SUCCESS`).
 * - [keymgr_ctrl.sv:154] & Companion Item 1/3 (INTENDED_SECURITY_HARDENING):
 *   Unenumerated `CONTROL_SHADOWED.OPERATION` encodings (`5..7`) map to
 *   `dis_op` (`keymgr_ctrl.sv:154`), transitioning `WORKING_STATE` to
 *   `Disabled (5)` with `OP_STATUS = DONE_SUCCESS (2)` and `ERR_CODE = 0`;
 *   in `Disabled (5)`, `CFG_REGWEN` remains `1` and triggering `START = 1`
 *   completes with `OP_STATUS = DONE_ERROR (3)`, `ERR_CODE.INVALID_OP (0x1)`,
 *   and `FAULT_STATUS = 0`.
 * - [keymgr_sideload_key_ctrl.sv:78-87] (INTENDED_SECURITY_HARDENING):
 * `SIDELOAD_CLEAR` is a continuous level-clear register (`clr_i` overrides
 * `set_i` during `GEN_HW_OUT` while `GEN_HW_OUT` completes with
 * `DONE_SUCCESS`).
 * - [keymgr_ctrl.sv:259] (INTENDED_SECURITY_HARDENING): `INVALID_KMAC_INPUT`
 *   (`KEY_VERSION > MAX_*_KEY_VER_SHADOWED`) gates `data_valid_o = 0` so
 *   `SW_SHARE0/1_OUTPUT` retains its prior key shares and `WORKING_STATE` is
 *   preserved.
 * - [keymgr_reg_pkg.sv:472-536] (INTENDED_SECURITY_HARDENING): Sub-word write
 * (`sb`) to `KEYMGR_PERMIT` 4-byte register and read from unmapped offset `>=
 * 0xfc` trigger synchronous TL-UL `d_error = 1` bus faults (`mcause = 7 / 5`).
 */

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/dif/dif_flash_ctrl.h"
#include "sw/device/lib/dif/dif_keymgr.h"
#include "sw/device/lib/dif/dif_kmac.h"
#include "sw/device/lib/dif/dif_otp_ctrl.h"
#include "sw/device/lib/dif/dif_rstmgr.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/entropy_testutils.h"
#include "sw/device/lib/testing/keymgr_testutils.h"
#include "sw/device/lib/testing/kmac_testutils.h"
#include "sw/device/lib/testing/otp_ctrl_testutils.h"
#include "sw/device/lib/testing/rstmgr_testutils.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"
#include "sw/device/silicon_creator/lib/drivers/retention_sram.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "keymgr_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kBase = TOP_EARLGREY_KEYMGR_BASE_ADDR,
  kBootPhase1BadOwnerSeed = 0x4b4d4731u,   // "KMG1"
  kBootPhase2GoodOwnerSeed = 0x4b4d4732u,  // "KMG2"
};

static volatile bool g_fault_seen = false;
static volatile uint32_t g_fault_mcause = 0;

void ottf_exception_handler(uint32_t *exc_info) {
  (void)exc_info;
  g_fault_seen = true;
  g_fault_mcause = ibex_mcause_read();
}

static const keymgr_testutils_secret_t kZeroSecret = {
    .value = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u},
};

static uint32_t wait_for_op_done(void) {
  for (uint32_t i = 0; i < 4000u; ++i) {
    uint32_t st = abs_mmio_read32(kBase + KEYMGR_OP_STATUS_REG_OFFSET) & 0x3u;
    if (st != KEYMGR_OP_STATUS_STATUS_VALUE_WIP &&
        st != KEYMGR_OP_STATUS_STATUS_VALUE_IDLE) {
      return st;
    }
    busy_spin_micros(5);
  }
  return abs_mmio_read32(kBase + KEYMGR_OP_STATUS_REG_OFFSET) & 0x3u;
}

static void program_flash_secrets_and_reboot(
    const keymgr_testutils_secret_t *owner_secret, uint32_t next_phase_magic) {
  dif_flash_ctrl_state_t flash;
  dif_rstmgr_t rstmgr;
  dif_otp_ctrl_t otp_ctrl;

  CHECK_DIF_OK(dif_flash_ctrl_init_state(
      &flash, mmio_region_from_addr(TOP_EARLGREY_FLASH_CTRL_CORE_BASE_ADDR)));
  CHECK_DIF_OK(dif_otp_ctrl_init(
      mmio_region_from_addr(TOP_EARLGREY_OTP_CTRL_CORE_BASE_ADDR), &otp_ctrl));
  CHECK_DIF_OK(dif_rstmgr_init(
      mmio_region_from_addr(TOP_EARLGREY_RSTMGR_AON_BASE_ADDR), &rstmgr));

  bool secret2_computed = false;
  CHECK_DIF_OK(dif_otp_ctrl_is_digest_computed(
      &otp_ctrl, kDifOtpCtrlPartitionSecret2, &secret2_computed));

  const keymgr_testutils_secret_t *creator_secret =
      secret2_computed ? NULL : &kCreatorSecret;
  CHECK_STATUS_OK(
      keymgr_testutils_flash_init(&flash, creator_secret, owner_secret));

  if (!secret2_computed) {
    CHECK_STATUS_OK(otp_ctrl_testutils_lock_partition(
        &otp_ctrl, kDifOtpCtrlPartitionSecret2, 0));
  }

  retention_sram_get()->creator.reserved[0] = next_phase_magic;
  rstmgr_testutils_reason_clear();
  CHECK_DIF_OK(dif_rstmgr_software_device_reset(&rstmgr));
  wait_for_interrupt();
}

static void run_phase1_keymgr_cdi_and_zero_inputs(void) {
  LOG_INFO("Phase 1: Confirming [keymgr.sv:436-464] & [keymgr_ctrl.sv:185]");

  CHECK_STATUS_OK(entropy_testutils_auto_mode_init());
  dif_kmac_t kmac;
  dif_keymgr_t keymgr;
  CHECK_DIF_OK(
      dif_kmac_init(mmio_region_from_addr(TOP_EARLGREY_KMAC_BASE_ADDR), &kmac));
  CHECK_STATUS_OK(kmac_testutils_config(&kmac, /*sideload=*/true));
  CHECK_DIF_OK(dif_keymgr_init(mmio_region_from_addr(kBase), &keymgr));

  // Advance Reset (0) -> Init (1) -> CreatorRootKey (2).
  // CreatorSeed (Info Page 1) is valid (`kCreatorSecret`), so advancing to
  // CreatorRootKey (2) must succeed with zero errors.
  CHECK_STATUS_OK(keymgr_testutils_advance_state(&keymgr, NULL));
  CHECK(abs_mmio_read32(kBase + KEYMGR_WORKING_STATE_REG_OFFSET) ==
        KEYMGR_WORKING_STATE_STATE_VALUE_INIT);

  CHECK_DIF_OK(dif_keymgr_advance_state_raw(&keymgr));
  CHECK_STATUS_OK(keymgr_testutils_wait_for_operation_done(&keymgr));
  CHECK(abs_mmio_read32(kBase + KEYMGR_WORKING_STATE_REG_OFFSET) ==
        KEYMGR_WORKING_STATE_STATE_VALUE_CREATOR_ROOT_KEY);
  CHECK(abs_mmio_read32(kBase + KEYMGR_ERR_CODE_REG_OFFSET) == 0x0u);
  CHECK(abs_mmio_read32(kBase + KEYMGR_DEBUG_REG_OFFSET) == 0x0u);

  // Clear SW_BINDING_REGWEN (RW0C) before attempting CreatorRootKey ->
  // OwnerIntKey.
  CHECK(abs_mmio_read32(kBase + KEYMGR_SW_BINDING_REGWEN_REG_OFFSET) == 0x1u);
  abs_mmio_write32(kBase + KEYMGR_SW_BINDING_REGWEN_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kBase + KEYMGR_SW_BINDING_REGWEN_REG_OFFSET) == 0x0u);

  // [keymgr.sv:436-464]: Advance CreatorRootKey (2) -> OwnerIntKey (3).
  // Even though CreatorRootKey -> OwnerIntKey hashes CreatorSeed (Info Page 1,
  // which is valid), `adv_dvalid[OwnerInt]` in `keymgr.sv:459-460, 538` checks
  // `owner_seed_vld` (Info Page 2, which we programmed to all-zeros
  // `0x00...00`). Therefore, this advance must fail with:
  //   OP_STATUS = DONE_ERROR (3)
  //   ERR_CODE  = INVALID_KMAC_INPUT (bit 2 = 0x4)
  //   DEBUG     = INVALID_OWNER_SEED (bit 1 = 0x2)
  //   WORKING_STATE preserved at CreatorRootKey (2)
  //   SW_BINDING_REGWEN kept locked at 0 ([keymgr_ctrl.sv:185]).
  CHECK_STATUS_OK(ottf_alerts_expect_alert_start(
      kTopEarlgreyAlertIdKeymgrRecovOperationErr));
  uint32_t adv_ctrl = KEYMGR_CONTROL_SHADOWED_OPERATION_VALUE_ADVANCE
                      << KEYMGR_CONTROL_SHADOWED_OPERATION_OFFSET;
  abs_mmio_write32(kBase + KEYMGR_CONTROL_SHADOWED_REG_OFFSET, adv_ctrl);
  abs_mmio_write32(kBase + KEYMGR_CONTROL_SHADOWED_REG_OFFSET, adv_ctrl);
  abs_mmio_write32(kBase + KEYMGR_START_REG_OFFSET, 0x1u);

  uint32_t st = wait_for_op_done();
  CHECK(st == KEYMGR_OP_STATUS_STATUS_VALUE_DONE_ERROR);
  uint32_t err = abs_mmio_read32(kBase + KEYMGR_ERR_CODE_REG_OFFSET);
  uint32_t dbg = abs_mmio_read32(kBase + KEYMGR_DEBUG_REG_OFFSET);
  CHECK(err == (1u << KEYMGR_ERR_CODE_INVALID_KMAC_INPUT_BIT));
  CHECK(dbg == (1u << KEYMGR_DEBUG_INVALID_OWNER_SEED_BIT));
  CHECK(abs_mmio_read32(kBase + KEYMGR_WORKING_STATE_REG_OFFSET) ==
        KEYMGR_WORKING_STATE_STATE_VALUE_CREATOR_ROOT_KEY);
  CHECK(abs_mmio_read32(kBase + KEYMGR_SW_BINDING_REGWEN_REG_OFFSET) == 0x0u);

  abs_mmio_write32(kBase + KEYMGR_ERR_CODE_REG_OFFSET, err);
  abs_mmio_write32(kBase + KEYMGR_DEBUG_REG_OFFSET, dbg);
  abs_mmio_write32(kBase + KEYMGR_OP_STATUS_REG_OFFSET, st);
  CHECK_STATUS_OK(ottf_alerts_expect_alert_finish(
      kTopEarlgreyAlertIdKeymgrRecovOperationErr));

  // Now program valid `kOwnerSecret` into Flash Info Page 2 and reboot into
  // Phase 2 to verify the remaining errata items across all stages.
  program_flash_secrets_and_reboot(&kOwnerSecret, kBootPhase2GoodOwnerSeed);
}

static void run_phase2_remaining_errata(void) {
  LOG_INFO("Phase 2: Confirming [keymgr.sv:369-370..007] with valid seeds");

  CHECK_STATUS_OK(entropy_testutils_auto_mode_init());
  dif_kmac_t kmac;
  dif_keymgr_t keymgr;
  CHECK_DIF_OK(
      dif_kmac_init(mmio_region_from_addr(TOP_EARLGREY_KMAC_BASE_ADDR), &kmac));
  CHECK_STATUS_OK(kmac_testutils_config(&kmac, /*sideload=*/true));
  CHECK_DIF_OK(dif_keymgr_init(mmio_region_from_addr(kBase), &keymgr));

  // Advance Reset (0) -> Init (1) -> CreatorRootKey (2).
  CHECK_STATUS_OK(keymgr_testutils_advance_state(&keymgr, NULL));
  CHECK_DIF_OK(dif_keymgr_advance_state_raw(&keymgr));
  CHECK_STATUS_OK(keymgr_testutils_wait_for_operation_done(&keymgr));
  CHECK(abs_mmio_read32(kBase + KEYMGR_WORKING_STATE_REG_OFFSET) ==
        KEYMGR_WORKING_STATE_STATE_VALUE_CREATOR_ROOT_KEY);

  // Lock SW_BINDING_REGWEN = 0, then advance CreatorRootKey (2) -> OwnerIntKey
  // (3) with valid OwnerSeed. Verify [keymgr.sv:369-370]/[keymgr_ctrl.sv:185]
  // that SW_BINDING_REGWEN unlocks back to 1 upon successful ADVANCE.
  abs_mmio_write32(kBase + KEYMGR_SW_BINDING_REGWEN_REG_OFFSET, 0x0u);
  CHECK(abs_mmio_read32(kBase + KEYMGR_SW_BINDING_REGWEN_REG_OFFSET) == 0x0u);
  CHECK_DIF_OK(dif_keymgr_advance_state_raw(&keymgr));
  CHECK_STATUS_OK(keymgr_testutils_wait_for_operation_done(&keymgr));
  CHECK(abs_mmio_read32(kBase + KEYMGR_WORKING_STATE_REG_OFFSET) ==
        KEYMGR_WORKING_STATE_STATE_VALUE_OWNER_INTERMEDIATE_KEY);
  CHECK(abs_mmio_read32(kBase + KEYMGR_SW_BINDING_REGWEN_REG_OFFSET) == 0x1u);

  // [keymgr_sideload_key_ctrl.sv:78-87]: SIDELOAD_CLEAR continuous level-clear
  // behavior. Write SIDELOAD_CLEAR = 2 (KMAC) and trigger GEN_HW_OUT to KMAC.
  // Verify the operation succeeds (`DONE_SUCCESS`, `ERR_CODE = 0`) while
  // `SIDELOAD_CLEAR` remains latched at `2` (continuous level clear).
  abs_mmio_write32(kBase + KEYMGR_SIDELOAD_CLEAR_REG_OFFSET, 2u);
  CHECK((abs_mmio_read32(kBase + KEYMGR_SIDELOAD_CLEAR_REG_OFFSET) & 0x7u) ==
        2u);
  dif_keymgr_versioned_key_params_t hw_params = kKeyVersionedParams;
  hw_params.dest = kDifKeymgrVersionedKeyDestKmac;
  hw_params.version = 0;
  CHECK_STATUS_OK(keymgr_testutils_generate_versioned_key(&keymgr, hw_params));
  CHECK((abs_mmio_read32(kBase + KEYMGR_SIDELOAD_CLEAR_REG_OFFSET) & 0x7u) ==
        2u);
  abs_mmio_write32(kBase + KEYMGR_SIDELOAD_CLEAR_REG_OFFSET, 0u);

  // [keymgr_ctrl.sv:259]: INVALID_KMAC_INPUT (`KEY_VERSION >
  // MAX_OWNER_INT_KEY_VER`) gates `data_valid_o = 0`, preserving
  // `SW_SHARE0/1_OUTPUT` and `WORKING_STATE`.
  abs_mmio_write32(kBase + KEYMGR_MAX_OWNER_INT_KEY_VER_SHADOWED_REG_OFFSET,
                   1u);
  abs_mmio_write32(kBase + KEYMGR_MAX_OWNER_INT_KEY_VER_SHADOWED_REG_OFFSET,
                   1u);
  dif_keymgr_versioned_key_params_t sw_params = kKeyVersionedParams;
  sw_params.dest = kDifKeymgrVersionedKeyDestSw;
  sw_params.version = 1u;
  CHECK_STATUS_OK(keymgr_testutils_generate_versioned_key(&keymgr, sw_params));
  // Read SW_SHARE0_OUTPUT_0 (Read-Clear: reading word 0 returns the non-zero
  // generated share and clears word 0 to 0, while leaving word 1 untouched).
  uint32_t share0_w0 =
      abs_mmio_read32(kBase + KEYMGR_SW_SHARE0_OUTPUT_0_REG_OFFSET);
  CHECK(share0_w0 != 0u);
  CHECK(abs_mmio_read32(kBase + KEYMGR_SW_SHARE0_OUTPUT_0_REG_OFFSET) == 0u);

  CHECK_STATUS_OK(ottf_alerts_expect_alert_start(
      kTopEarlgreyAlertIdKeymgrRecovOperationErr));
  abs_mmio_write32(kBase + KEYMGR_KEY_VERSION_REG_OFFSET, 2u);
  uint32_t gen_sw_ctrl =
      KEYMGR_CONTROL_SHADOWED_OPERATION_VALUE_GENERATE_SW_OUTPUT
      << KEYMGR_CONTROL_SHADOWED_OPERATION_OFFSET;
  abs_mmio_write32(kBase + KEYMGR_CONTROL_SHADOWED_REG_OFFSET, gen_sw_ctrl);
  abs_mmio_write32(kBase + KEYMGR_CONTROL_SHADOWED_REG_OFFSET, gen_sw_ctrl);
  abs_mmio_write32(kBase + KEYMGR_START_REG_OFFSET, 0x1u);

  uint32_t st = wait_for_op_done();
  CHECK(st == KEYMGR_OP_STATUS_STATUS_VALUE_DONE_ERROR);
  uint32_t err = abs_mmio_read32(kBase + KEYMGR_ERR_CODE_REG_OFFSET);
  uint32_t dbg = abs_mmio_read32(kBase + KEYMGR_DEBUG_REG_OFFSET);
  CHECK(err == (1u << KEYMGR_ERR_CODE_INVALID_KMAC_INPUT_BIT));
  CHECK(dbg == (1u << KEYMGR_DEBUG_INVALID_KEY_VERSION_BIT));
  CHECK(abs_mmio_read32(kBase + KEYMGR_WORKING_STATE_REG_OFFSET) ==
        KEYMGR_WORKING_STATE_STATE_VALUE_OWNER_INTERMEDIATE_KEY);
  // Word 0 (cleared to 0 before the failing GEN_SW_OUT) must still be 0 (not
  // overwritten with a new KMAC digest), and Word 1 (unread before the failing
  // GEN_SW_OUT) must still hold its prior non-zero share (not wiped).
  CHECK(abs_mmio_read32(kBase + KEYMGR_SW_SHARE0_OUTPUT_0_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kBase + KEYMGR_SW_SHARE0_OUTPUT_1_REG_OFFSET) != 0u);
  abs_mmio_write32(kBase + KEYMGR_ERR_CODE_REG_OFFSET, err);
  abs_mmio_write32(kBase + KEYMGR_DEBUG_REG_OFFSET, dbg);
  abs_mmio_write32(kBase + KEYMGR_OP_STATUS_REG_OFFSET, st);
  CHECK_STATUS_OK(ottf_alerts_expect_alert_finish(
      kTopEarlgreyAlertIdKeymgrRecovOperationErr));

  // Advance OwnerIntKey (3) -> OwnerKey (4).
  CHECK_STATUS_OK(
      keymgr_testutils_advance_state(&keymgr, &kOwnerRootKeyParams));
  CHECK(abs_mmio_read32(kBase + KEYMGR_WORKING_STATE_REG_OFFSET) ==
        KEYMGR_WORKING_STATE_STATE_VALUE_OWNER_KEY);

  // [keymgr_ctrl.sv:154] & Companion Item 1/3: Unenumerated OPERATION = 6 acts
  // as OP_DISABLE (`dis_op`), transitioning WORKING_STATE to Disabled (5) with
  // OP_STATUS = DONE_SUCCESS (2) and ERR_CODE = 0.
  uint32_t unenum_dis_ctrl = 6u << KEYMGR_CONTROL_SHADOWED_OPERATION_OFFSET;
  abs_mmio_write32(kBase + KEYMGR_CONTROL_SHADOWED_REG_OFFSET, unenum_dis_ctrl);
  abs_mmio_write32(kBase + KEYMGR_CONTROL_SHADOWED_REG_OFFSET, unenum_dis_ctrl);
  abs_mmio_write32(kBase + KEYMGR_START_REG_OFFSET, 0x1u);
  st = wait_for_op_done();
  CHECK(st == KEYMGR_OP_STATUS_STATUS_VALUE_DONE_SUCCESS);
  CHECK(abs_mmio_read32(kBase + KEYMGR_ERR_CODE_REG_OFFSET) == 0x0u);
  abs_mmio_write32(kBase + KEYMGR_OP_STATUS_REG_OFFSET, st);
  CHECK(abs_mmio_read32(kBase + KEYMGR_WORKING_STATE_REG_OFFSET) ==
        KEYMGR_WORKING_STATE_STATE_VALUE_DISABLED);

  // In Disabled (5), CFG_REGWEN remains 1 and START = 1 runs a dummy KMAC
  // operation that completes with DONE_ERROR (3), ERR_CODE.INVALID_OP (0x1),
  // and FAULT_STATUS = 0.
  CHECK(abs_mmio_read32(kBase + KEYMGR_CFG_REGWEN_REG_OFFSET) == 0x1u);
  CHECK_STATUS_OK(ottf_alerts_expect_alert_start(
      kTopEarlgreyAlertIdKeymgrRecovOperationErr));
  abs_mmio_write32(kBase + KEYMGR_START_REG_OFFSET, 0x1u);
  st = wait_for_op_done();
  CHECK(st == KEYMGR_OP_STATUS_STATUS_VALUE_DONE_ERROR);
  err = abs_mmio_read32(kBase + KEYMGR_ERR_CODE_REG_OFFSET);
  CHECK(err == (1u << KEYMGR_ERR_CODE_INVALID_OP_BIT));
  CHECK(abs_mmio_read32(kBase + KEYMGR_FAULT_STATUS_REG_OFFSET) == 0x0u);
  abs_mmio_write32(kBase + KEYMGR_ERR_CODE_REG_OFFSET, err);
  abs_mmio_write32(kBase + KEYMGR_OP_STATUS_REG_OFFSET, st);
  CHECK_STATUS_OK(ottf_alerts_expect_alert_finish(
      kTopEarlgreyAlertIdKeymgrRecovOperationErr));

  // [keymgr_reg_pkg.sv:472-536]: Sub-word write (`sb`) to `CONTROL_SHADOWED`
  // (4-byte permit mask `4'b1111`) and word read from unmapped offset `0xfc`
  // trigger synchronous TL-UL `d_error = 1` exceptions (`mcause = 7` and
  // `mcause = 5`).
  g_fault_seen = false;
  g_fault_mcause = 0;
  abs_mmio_write8(kBase + KEYMGR_CONTROL_SHADOWED_REG_OFFSET, 0x01u);
  CHECK(g_fault_seen);
  CHECK(g_fault_mcause == kIbexExcStoreAccessFault);

  g_fault_seen = false;
  g_fault_mcause = 0;
  (void)abs_mmio_read32(kBase + 0xfcu);
  CHECK(g_fault_seen);
  CHECK(g_fault_mcause == kIbexExcLoadAccessFault);
}

bool test_main(void) {
  const dif_rstmgr_reset_info_bitfield_t reset_info =
      rstmgr_testutils_reason_get();
  uint32_t phase_magic = retention_sram_get()->creator.reserved[0];

  if (reset_info == kDifRstmgrResetInfoPor) {
    // On first boot (POR), program valid CreatorSecret (Info Page 1) and
    // all-zero OwnerSecret (Info Page 2), lock OTP SECRET2, and reboot into
    // Phase 1 to empirically verify [keymgr.sv:436-464] & [keymgr_ctrl.sv:185].
    program_flash_secrets_and_reboot(&kZeroSecret, kBootPhase1BadOwnerSeed);
    return false;
  }

  if (phase_magic == kBootPhase1BadOwnerSeed) {
    run_phase1_keymgr_cdi_and_zero_inputs();
    return false;
  }

  CHECK(phase_magic == kBootPhase2GoodOwnerSeed);
  retention_sram_get()->creator.reserved[0] = 0u;
  run_phase2_remaining_errata();

  LOG_INFO("keymgr_errata_test passed on all errata items");
  return true;
}
