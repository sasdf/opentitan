// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file keymgr_errata_v2_test.c
 * @brief CW340 FPGA verification test for Earlgrey v2 (`trunk-v2`) `keymgr_dpe`
 * hardware, specification, and DIF errata:
 *
 * Confirmed v1 Errata Still Present on v2 (`keymgr_dpe`):
 *   - Undocumented dynamic gating of `SW_BINDING_REGWEN`,
 *     `SLOT_POLICY_REGWEN`, and `MAX_KEY_VER_REGWEN` by `CFG_REGWEN` while
 *     `OP_STATUS == WIP` (`hw/ip/keymgr_dpe/rtl/keymgr_dpe.sv:476-478`).
 *   - `SIDELOAD_CLEAR` continuous level priority over
 *     `GEN_HW_OUT` (`OP_STATUS = DONE_SUCCESS` while holding `valid_q = 0`,
 *     causing KMAC sideload to fail with `ErrKeyNotValid = 0x1`,
 *     `hw/ip/keymgr_dpe/rtl/keymgr_dpe_sideload_key.sv:32-48`).
 *   - `data_valid_o` gating (`~invalid_op` at
 *     `hw/ip/keymgr_dpe/rtl/keymgr_dpe_ctrl.sv:284`) preserves existing
 *     `SW_SHARE0/1_OUTPUT` and sideload keys when `KEY_VERSION >
 *     slot.max_key_version` raises `ERR_CODE = INVALID_OP |
 *     INVALID_KMAC_INPUT (0x3)`.
 *   - Failed `ADVANCE` (`op_err == 1`) preserves
 *     `WORKING_STATE = Available` and existing DPE key slots (`SlotUpdateIdle`)
 *     while holding `unlock_after_advance_o = 0` (`SW_BINDING_REGWEN` stays
 *     locked at `0`, `hw/ip/keymgr_dpe/rtl/keymgr_dpe_ctrl.sv:174, 212`).
 *   - Synchronous TL-UL bus error (`d_error = 1`, Ibex
 *     `mcause = 7 / 5`) on narrow sub-word writes violating
 *     `KEYMGR_DPE_PERMIT[54]` (`CONTROL_SHADOWED = 4'b0111`,
 *     `SW_BINDING_0 = 4'b1111`) and unmapped offset accesses (`>= 0xd8`).
 *
 * v1 Errata Fixed in v2 RTL (`keymgr_dpe`):
 *   - `adv_dvalid` and `hw2reg.debug` are now aligned with
 *     `adv_matrix` (`hw/ip/keymgr_dpe/rtl/keymgr_dpe.sv:543-570, 649-656`);
 *     advancing from `BootStageCreator (0)` checks `devid_vld &
 *     health_state_vld & rom_digest_vld` without checking `creator_seed_vld`
 *     one stage early.
 *   - `StCtrlDpeDisabled` (`WORKING_STATE = Disabled (2)`)
 *     sets `op_req = 0` and `invalid_op = op_start_i`
 *     (`hw/ip/keymgr_dpe/rtl/keymgr_dpe_ctrl.sv:580-587`), completing in 1
 *     cycle with `OP_STATUS = DONE_ERROR (3)` and `ERR_CODE.INVALID_OP = 1`
 *     without streaming dummy KMAC transactions.
 *
 * Newly Discovered Earlgrey v2 (`trunk-v2`) Errata (`NEW_IN_V2`):
 *   - Unenumerated `CONTROL_SHADOWED.OPERATION` (`6` or
 *     `7`) in `StCtrlDpeAvailable` sets `op_req = 1` with `adv_req = gen_req =
 *     erase_req = dis_req = load_req = 0` and `invalid_op = 0`
 *     (`hw/ip/keymgr_dpe/rtl/keymgr_dpe_ctrl.sv:155-160, 408, 534-543`),
 *     leaving `u_op_state` in `StIdle` (`op_ack = 0`) and permanently
 *     deadlocking `keymgr_dpe` in `OP_STATUS = WIP (1)` with `START = 1` and
 *     `CFG_REGWEN = 0` until hardware reset.
 *   - `u_sw_binding_regwen`, `u_slot_policy_regwen`, and
 *     `u_max_key_ver_regwen` set `.NonInitClr(1'b1)`
 *     (`hw/ip/keymgr_dpe/rtl/keymgr_dpe.sv:433-474`, contradicting
 *     `keymgr_dpe.hjson:736`), while `unlock_after_advance_o = adv_req &
 *     op_ack & ~(op_err | op_fault_err)` (`keymgr_dpe_ctrl.sv:174`) omits
 *     `init_o` and `load_req`, so UDS loading fails to unlock `*_REGWEN` and
 *     causes `dif_keymgr_dpe_advance_state()` to return `kDifLocked`
 *     (`TODO(#30666)` / `TODO(#30667)`).
 *   - 3-bit `CONTROL_SHADOWED.SLOT_SRC_SEL` (`[16:14]`)
 *     and `SLOT_DST_SEL` (`[20:18]`) silently tie off bit `[2]` and truncate
 *     modulo 4 (`slot & 0x3`, `hw/ip/keymgr_dpe/rtl/keymgr_dpe.sv:319-336`,
 *     `TODO(#30682)`) instead of raising `ERR_CODE.INVALID_OP`, allowing
 *     out-of-bounds `SLOT_SRC_SEL = 6` / `SLOT_DST_SEL = 6` to alias and erase
 *     hardware slot `2`.
 *   - `ROM`
 * (`sw/device/silicon_creator/rom/rom.c:665-735`) and
 * `sc_keymgr_dpe_advance_creator()`
 *     (`sw/device/silicon_creator/lib/drivers/keymgr_dpe.c:509-534`) never
 *     invoke `sc_keymgr_dpe_lock_uds()`, leaving `LOAD_KEY_LOCK == 0` (`0xd4`)
 *     after `ROM` boot so post-ROM firmware can execute `OpDpeLoadRootKey`
 *     (`OPERATION = 5`) to reload the raw OTP `UDS` (`BootStageCreator = 0`)
 *     into any free slot.
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_keymgr_dpe.h"
#include "sw/device/lib/dif/dif_kmac.h"
#include "sw/device/lib/dif/dif_rstmgr.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/irq.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/entropy_testutils.h"
#include "sw/device/lib/testing/kmac_testutils.h"
#include "sw/device/lib/testing/rstmgr_testutils.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top/keymgr_dpe_regs.h"
#include "hw/top/kmac_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kKeymgrBase = TOP_EARLGREY_KEYMGR_DPE_BASE_ADDR,
  kKmacBase = TOP_EARLGREY_KMAC_BASE_ADDR,
  kRstmgrBase = TOP_EARLGREY_RSTMGR_BASE_ADDR,
};

static volatile bool g_saw_bus_fault = false;
static volatile uint32_t g_last_mcause = 0;

void ottf_external_isr(uint32_t *exc_info) {
  (void)exc_info;
  irq_external_ctrl(false);
}

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  g_last_mcause = ibex_mcause_read();
  g_saw_bus_fault = true;
}

static void keymgr_write_shadowed(uint32_t offset, uint32_t val) {
  abs_mmio_write32(kKeymgrBase + offset, val);
  abs_mmio_write32(kKeymgrBase + offset, val);
}

static uint32_t keymgr_make_ctrl(uint32_t op, uint32_t dest_sel,
                                 uint32_t src_slot, uint32_t dst_slot,
                                 bool sw_binding_only) {
  uint32_t reg = 0;
  reg = bitfield_field32_write(reg, KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_FIELD,
                               op);
  reg = bitfield_field32_write(reg, KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_FIELD,
                               dest_sel);
  reg = bitfield_field32_write(
      reg, KEYMGR_DPE_CONTROL_SHADOWED_SLOT_SRC_SEL_FIELD, src_slot);
  reg = bitfield_field32_write(
      reg, KEYMGR_DPE_CONTROL_SHADOWED_SLOT_DST_SEL_FIELD, dst_slot);
  reg = bitfield_bit32_write(
      reg, KEYMGR_DPE_CONTROL_SHADOWED_SW_BINDING_ONLY_BIT, sw_binding_only);
  return reg;
}

static uint32_t keymgr_wait_done(uint32_t *err_code_out) {
  uint32_t st;
  do {
    st = abs_mmio_read32(kKeymgrBase + KEYMGR_DPE_OP_STATUS_REG_OFFSET);
  } while (st == KEYMGR_DPE_OP_STATUS_STATUS_VALUE_IDLE ||
           st == KEYMGR_DPE_OP_STATUS_STATUS_VALUE_WIP);
  uint32_t err = abs_mmio_read32(kKeymgrBase + KEYMGR_DPE_ERR_CODE_REG_OFFSET);
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_OP_STATUS_REG_OFFSET, st);
  if (err != 0) {
    abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_ERR_CODE_REG_OFFSET, err);
  }
  if (err_code_out != NULL) {
    *err_code_out = err;
  }
  return st;
}

/**
 * Test 1: (CONFIRMED_PRESENT_ON_V2)
 * Narrow sub-word writes violating `KEYMGR_DPE_PERMIT[54]` and unmapped offset
 * accesses (`>= 0xd8`) raise a synchronous TL-UL bus error (`d_error = 1`,
 * Ibex `mcause = 7 / 5`).
 */
static void test_errata_keymgr_007_permit_and_addrmiss(void) {
  LOG_INFO("Testing KEYMGR_DPE_PERMIT and addrmiss faults");

  uint32_t ctrl_before =
      abs_mmio_read32(kKeymgrBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET);
  g_saw_bus_fault = false;
  g_last_mcause = 0;
  *((volatile uint8_t *)(kKeymgrBase +
                         KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET)) = 0x00;
  CHECK(g_saw_bus_fault,
        "Expected Store Access Fault on 1-byte write to CONTROL_SHADOWED");
  CHECK(g_last_mcause == 7, "Expected mcause == 7, got 0x%x", g_last_mcause);
  CHECK(abs_mmio_read32(kKeymgrBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET) ==
        ctrl_before);

  g_saw_bus_fault = false;
  g_last_mcause = 0;
  *((volatile uint16_t *)(kKeymgrBase +
                          KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET)) = 0x0000;
  CHECK(g_saw_bus_fault,
        "Expected Store Access Fault on 2-byte write to CONTROL_SHADOWED");
  CHECK(g_last_mcause == 7, "Expected mcause == 7, got 0x%x", g_last_mcause);
  CHECK(abs_mmio_read32(kKeymgrBase + KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET) ==
        ctrl_before);

  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_SW_BINDING_0_REG_OFFSET,
                   0x13572468u);
  g_saw_bus_fault = false;
  g_last_mcause = 0;
  *((volatile uint16_t *)(kKeymgrBase + KEYMGR_DPE_SW_BINDING_0_REG_OFFSET)) =
      0xdead;
  CHECK(g_saw_bus_fault,
        "Expected Store Access Fault on 2-byte write to SW_BINDING_0");
  CHECK(g_last_mcause == 7, "Expected mcause == 7, got 0x%x", g_last_mcause);
  CHECK(abs_mmio_read32(kKeymgrBase + KEYMGR_DPE_SW_BINDING_0_REG_OFFSET) ==
        0x13572468u);

  const uint32_t kPermit0011Regs[] = {
      KEYMGR_DPE_RESEED_INTERVAL_SHADOWED_REG_OFFSET,
      KEYMGR_DPE_FAULT_STATUS_REG_OFFSET,
      KEYMGR_DPE_DEBUG_REG_OFFSET,
  };
  for (size_t i = 0; i < ARRAYSIZE(kPermit0011Regs); ++i) {
    g_saw_bus_fault = false;
    g_last_mcause = 0;
    *((volatile uint8_t *)(kKeymgrBase + kPermit0011Regs[i])) = 0x00;
    CHECK(g_saw_bus_fault,
          "Expected Store Access Fault on 1-byte write to offset 0x%x",
          kPermit0011Regs[i]);
    CHECK(g_last_mcause == 7, "Expected mcause == 7, got 0x%x", g_last_mcause);
  }

  g_saw_bus_fault = false;
  g_last_mcause = 0;
  (void)*((volatile uint32_t *)(kKeymgrBase + 0xd8u));
  CHECK(g_saw_bus_fault,
        "Expected Load Access Fault on unmapped offset 0xd8 (addrmiss)");
  CHECK(g_last_mcause == 5, "Expected mcause == 5, got 0x%x", g_last_mcause);

  g_saw_bus_fault = false;
  g_last_mcause = 0;
  *((volatile uint32_t *)(kKeymgrBase + 0xd8u)) = 0xdeadbeefu;
  CHECK(g_saw_bus_fault,
        "Expected Store Access Fault on unmapped offset 0xd8 (addrmiss)");
  CHECK(g_last_mcause == 7, "Expected mcause == 7, got 0x%x", g_last_mcause);
}

/**
 * Test 2: UDS Reload & REGWEN Lock (NEW_IN_V2)
 * - `ROM` (`rom.c:665-735`) and
 *   `sc_keymgr_dpe_advance_creator()` leave `LOAD_KEY_LOCK == 0` (`0xd4`),
 *   allowing post-ROM firmware to execute `OpDpeLoadRootKey` (`OPERATION = 5`)
 *   and reload the raw OTP `UDS` (`BootStageCreator = 0`) into slot 2.
 * - Locking `SW_BINDING_REGWEN = 0`,
 *   `SLOT_POLICY_REGWEN = 0`, and `MAX_KEY_VER_REGWEN = 0` before UDS load
 *   leaves all three `*_REGWEN` registers locked at `0` after UDS load succeeds
 *   (`unlock_after_advance_o = adv_req & op_ack & ... == 0`), causing
 *   `dif_keymgr_dpe_advance_state()` to return `kDifLocked` (`TODO(#30667)`).
 */
static void test_errata_v2_04_and_v2_02_uds_reload_and_regwen_lock(
    const dif_keymgr_dpe_t *keymgr_dpe) {
  LOG_INFO(
      "Testing UDS reload LOAD_KEY_LOCK "
      "and UDS REGWEN lock");

  uint32_t load_lock =
      abs_mmio_read32(kKeymgrBase + KEYMGR_DPE_LOAD_KEY_LOCK_REG_OFFSET);
  CHECK(load_lock == 0,
        "Expected LOAD_KEY_LOCK == 0 after ROM boot, "
        "got 0x%x",
        load_lock);

  // Set MAX_KEY_VER_SHADOWED = 5 and lock all three *_REGWEN registers (rw0c)
  // before loading UDS into slot 2.
  keymgr_write_shadowed(KEYMGR_DPE_MAX_KEY_VER_SHADOWED_REG_OFFSET, 5u);
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_MAX_KEY_VER_REGWEN_REG_OFFSET, 0u);
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_SW_BINDING_REGWEN_REG_OFFSET, 0u);
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_SLOT_POLICY_REGWEN_REG_OFFSET, 0u);

  CHECK(abs_mmio_read32(kKeymgrBase +
                        KEYMGR_DPE_MAX_KEY_VER_REGWEN_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kKeymgrBase +
                        KEYMGR_DPE_SW_BINDING_REGWEN_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kKeymgrBase +
                        KEYMGR_DPE_SLOT_POLICY_REGWEN_REG_OFFSET) == 0u);

  // Execute OpDpeLoadRootKey (OPERATION = 5) targeting free slot 2.
  keymgr_write_shadowed(
      KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      keymgr_make_ctrl(
          KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_LOAD_ROOT_KEY,
          KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, 0, 2, false));
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_START_REG_OFFSET, 1u);

  uint32_t err = 0;
  uint32_t st = keymgr_wait_done(&err);
  CHECK(st == KEYMGR_DPE_OP_STATUS_STATUS_VALUE_DONE_SUCCESS,
        "Expected DONE_SUCCESS on OpDpeLoadRootKey into slot 2, got 0x%x", st);
  CHECK(err == 0, "Expected ERR_CODE == 0, got 0x%x", err);

  // Because `unlock_after_advance_o = adv_req & op_ack
  // & ~(op_err | op_fault_err)` is 0 during UDS load (both in StCtrlDpeRootKey
  // and OpDpeLoadRootKey), all three *_REGWEN registers remain locked at 0!
  CHECK(abs_mmio_read32(kKeymgrBase +
                        KEYMGR_DPE_MAX_KEY_VER_REGWEN_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kKeymgrBase +
                        KEYMGR_DPE_SW_BINDING_REGWEN_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kKeymgrBase +
                        KEYMGR_DPE_SLOT_POLICY_REGWEN_REG_OFFSET) == 0u);

  // Consequently, calling dif_keymgr_dpe_advance_state() returns kDifLocked!
  dif_keymgr_dpe_advance_params_t adv_params = {
      .binding_value = {0},
      .max_key_version = 5,
      .slot_src_sel = 2,
      .slot_dst_sel = 2,
      .slot_policy = 1,
  };
  CHECK(dif_keymgr_dpe_advance_state(keymgr_dpe, &adv_params) == kDifLocked,
        "Expected dif_keymgr_dpe_advance_state() to fail with kDifLocked ");
}

/**
 * Test 3: (FIXED_IN_V2_RTL)
 * Advancing slot 2 (`BootStageCreator (0) -> BootStageOwnerInt (1)`) with
 * `SW_BINDING_ONLY = 0` checks `devid_vld & health_state_vld & rom_digest_vld`
 * (`adv_dvalid[BootStageCreator]`) and does NOT check `creator_seed_vld` one
 * stage early (`DEBUG.INVALID_CREATOR_SEED == 0`). Also verifies that this
 * KMAC-backed `Advance` (`adv_req & op_ack == 1`) unlocks `SW_BINDING_REGWEN`,
 * `SLOT_POLICY_REGWEN`, and `MAX_KEY_VER_REGWEN`.
 */
static void test_errata_keymgr_001_fixed_in_v2_and_regwen_unlock(void) {
  LOG_INFO(
      "Testing (FIXED_IN_V2_RTL): BootStageCreator "
      "adv_dvalid alignment");

  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_DEBUG_REG_OFFSET, 0u);
  keymgr_write_shadowed(
      KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      keymgr_make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_ADVANCE,
                       KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, 2, 2,
                       /*sw_binding_only=*/false));
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_START_REG_OFFSET, 1u);

  uint32_t err = 0;
  uint32_t st = keymgr_wait_done(&err);
  uint32_t dbg = abs_mmio_read32(kKeymgrBase + KEYMGR_DPE_DEBUG_REG_OFFSET);
  CHECK(st == KEYMGR_DPE_OP_STATUS_STATUS_VALUE_DONE_SUCCESS,
        "Expected DONE_SUCCESS advancing slot 2 (BootStageCreator -> "
        "BootStageOwnerInt), got st=0x%x err=0x%x dbg=0x%x",
        st, err, dbg);
  CHECK(err == 0);
  CHECK((dbg & (1u << KEYMGR_DPE_DEBUG_INVALID_CREATOR_SEED_BIT)) == 0u,
        "Expected INVALID_CREATOR_SEED == 0 on BootStageCreator advance");

  // Verify that `unlock_after_advance_o = adv_req & op_ack & ...` unlocked all
  // three *_REGWEN registers upon successful KMAC Advance.
  CHECK(abs_mmio_read32(kKeymgrBase +
                        KEYMGR_DPE_MAX_KEY_VER_REGWEN_REG_OFFSET) == 1u);
  CHECK(abs_mmio_read32(kKeymgrBase +
                        KEYMGR_DPE_SW_BINDING_REGWEN_REG_OFFSET) == 1u);
  CHECK(abs_mmio_read32(kKeymgrBase +
                        KEYMGR_DPE_SLOT_POLICY_REGWEN_REG_OFFSET) == 1u);

  // Erase slot 2, reload UDS into slot 2 (which sets allow_child = 1,
  // retain_parent = 0), and advance slot 2 -> 2 with SLOT_POLICY = 0x5
  // (retain_parent = 1, allow_child = 1) and MAX_KEY_VER_SHADOWED = 10 so
  // slot 2 can derive children and versioned keys in subsequent tests.

  keymgr_write_shadowed(
      KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      keymgr_make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_ERASE_SLOT,
                       KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, 0, 2,
                       false));
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_START_REG_OFFSET, 1u);
  CHECK(keymgr_wait_done(&err) ==
        KEYMGR_DPE_OP_STATUS_STATUS_VALUE_DONE_SUCCESS);

  keymgr_write_shadowed(KEYMGR_DPE_MAX_KEY_VER_SHADOWED_REG_OFFSET, 10u);
  keymgr_write_shadowed(
      KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      keymgr_make_ctrl(
          KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_LOAD_ROOT_KEY,
          KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, 0, 2, false));
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_START_REG_OFFSET, 1u);
  CHECK(keymgr_wait_done(&err) ==
        KEYMGR_DPE_OP_STATUS_STATUS_VALUE_DONE_SUCCESS);

  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_SLOT_POLICY_REG_OFFSET, 0x5u);
  keymgr_write_shadowed(KEYMGR_DPE_MAX_KEY_VER_SHADOWED_REG_OFFSET, 10u);
  keymgr_write_shadowed(
      KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      keymgr_make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_ADVANCE,
                       KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, 2, 2,
                       /*sw_binding_only=*/false));
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_START_REG_OFFSET, 1u);
  CHECK(keymgr_wait_done(&err) ==
        KEYMGR_DPE_OP_STATUS_STATUS_VALUE_DONE_SUCCESS);
  CHECK(err == 0);
}

/**
 * Test 4: (CONFIRMED_PRESENT_ON_V2)
 * While an operation is in progress (`OP_STATUS == WIP (1)`, `CFG_REGWEN ==
 * 0`), `SW_BINDING_REGWEN`, `SLOT_POLICY_REGWEN`, and `MAX_KEY_VER_REGWEN`
 * dynamically read `0` (`hw2reg.*_regwen.d = *_regwen & cfg_regwen` at
 * `keymgr_dpe.sv:476-478`) and block writes to `SW_BINDING_0` and
 * `SLOT_POLICY`, then return to `1` when `OP_STATUS == DONE_SUCCESS`.
 */
static void test_errata_keymgr_002_cfg_regwen_dynamic_gating(void) {
  LOG_INFO("Testing Dynamic gating of *_REGWEN by CFG_REGWEN");

  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_SW_BINDING_0_REG_OFFSET,
                   0x11223344u);
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_SLOT_POLICY_REG_OFFSET, 0x5u);
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_KEY_VERSION_REG_OFFSET, 0u);

  keymgr_write_shadowed(
      KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      keymgr_make_ctrl(
          KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_GENERATE_SW_OUTPUT,
          KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, 2, 2, false));

  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_START_REG_OFFSET, 1u);
  uint32_t cfg_wen =
      abs_mmio_read32(kKeymgrBase + KEYMGR_DPE_CFG_REGWEN_REG_OFFSET);
  uint32_t swb_wen =
      abs_mmio_read32(kKeymgrBase + KEYMGR_DPE_SW_BINDING_REGWEN_REG_OFFSET);
  uint32_t pol_wen =
      abs_mmio_read32(kKeymgrBase + KEYMGR_DPE_SLOT_POLICY_REGWEN_REG_OFFSET);
  uint32_t ver_wen =
      abs_mmio_read32(kKeymgrBase + KEYMGR_DPE_MAX_KEY_VER_REGWEN_REG_OFFSET);

  // Attempt to overwrite SW_BINDING_0, SLOT_POLICY, and MAX_KEY_VER_SHADOWED
  // while WIP (CFG_REGWEN==0).
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_SW_BINDING_0_REG_OFFSET,
                   0xdeadbeefu);
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_SLOT_POLICY_REG_OFFSET, 0x0u);
  keymgr_write_shadowed(KEYMGR_DPE_MAX_KEY_VER_SHADOWED_REG_OFFSET, 99u);

  uint32_t err = 0;
  uint32_t st = keymgr_wait_done(&err);
  CHECK(st == KEYMGR_DPE_OP_STATUS_STATUS_VALUE_DONE_SUCCESS);
  CHECK(err == 0);

  CHECK(cfg_wen == 0u, "Expected CFG_REGWEN == 0 while WIP");
  CHECK(swb_wen == 0u,
        "Expected SW_BINDING_REGWEN == 0 while CFG_REGWEN == 0 ");
  CHECK(pol_wen == 0u,
        "Expected SLOT_POLICY_REGWEN == 0 while CFG_REGWEN == 0");
  CHECK(ver_wen == 0u,
        "Expected MAX_KEY_VER_REGWEN == 0 while CFG_REGWEN == 0");
  CHECK(abs_mmio_read32(kKeymgrBase + KEYMGR_DPE_SW_BINDING_0_REG_OFFSET) ==
        0x11223344u);
  CHECK(abs_mmio_read32(kKeymgrBase + KEYMGR_DPE_SLOT_POLICY_REG_OFFSET) ==
        0x5u);
  CHECK(abs_mmio_read32(kKeymgrBase +
                        KEYMGR_DPE_MAX_KEY_VER_SHADOWED_REG_OFFSET) == 10u);
  CHECK(abs_mmio_read32(kKeymgrBase +
                        KEYMGR_DPE_SW_BINDING_REGWEN_REG_OFFSET) == 1u);
  CHECK(abs_mmio_read32(kKeymgrBase +
                        KEYMGR_DPE_SLOT_POLICY_REGWEN_REG_OFFSET) == 1u);
  CHECK(abs_mmio_read32(kKeymgrBase +
                        KEYMGR_DPE_MAX_KEY_VER_REGWEN_REG_OFFSET) == 1u);
}

/**
 * Test 5: (CONFIRMED_PRESENT_ON_V2)
 * `SIDELOAD_CLEAR` continuous level priority over `GEN_HW_OUT` reports
 * `OP_STATUS = DONE_SUCCESS` while holding `valid_q = 0` (`u_kmac_key`),
 * causing KMAC sideload operations to fail with `ErrKeyNotValid (0x1)`.
 */
static void test_errata_keymgr_004_sideload_clear_level_priority(
    dif_kmac_t *kmac) {
  LOG_INFO("Testing SIDELOAD_CLEAR continuous level priority");

  // Hold SIDELOAD_CLEAR = KMAC (2) while running GEN_HW_OUT (DEST_SEL = KMAC).
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_SIDELOAD_CLEAR_REG_OFFSET,
                   KEYMGR_DPE_SIDELOAD_CLEAR_VAL_VALUE_KMAC);
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_KEY_VERSION_REG_OFFSET, 0u);
  keymgr_write_shadowed(
      KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      keymgr_make_ctrl(
          KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_GENERATE_HW_OUTPUT,
          KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_KMAC, 2, 2, false));
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_START_REG_OFFSET, 1u);

  uint32_t err = 0;
  uint32_t st = keymgr_wait_done(&err);
  CHECK(st == KEYMGR_DPE_OP_STATUS_STATUS_VALUE_DONE_SUCCESS);
  CHECK(err == 0);

  // Release SIDELOAD_CLEAR = None (0); slot valid_q remains 0!
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_SIDELOAD_CLEAR_REG_OFFSET,
                   KEYMGR_DPE_SIDELOAD_CLEAR_VAL_VALUE_NONE);

  // Attempt KMAC with sideload=true -> fails with ErrKeyNotValid (0x1 /
  // kFailedPrecondition).
  static const dif_kmac_key_t kDummySwKey = {
      .share0 = {0x43424140, 0x47464544, 0x4B4A4948, 0x4F4E4D4C, 0x53525150,
                 0x57565554, 0x5B5A5958, 0x5F5E5D5C},
      .share1 = {0},
      .length = kDifKmacKeyLen256,
  };
  uint32_t kmac_out[8] = {0};
  CHECK_STATUS_OK(kmac_testutils_config(kmac, /*sideload=*/true));
  status_t kmac_res =
      kmac_testutils_kmac(kmac, kDifKmacModeKmacLen128, &kDummySwKey, NULL, 0,
                          "test", 4, ARRAYSIZE(kmac_out), kmac_out, NULL);
  uint32_t kmac_err = abs_mmio_read32(kKmacBase + KMAC_ERR_CODE_REG_OFFSET);
  CHECK(status_err(kmac_res) == kFailedPrecondition,
        "Expected kFailedPrecondition (ErrKeyNotValid) when sideload slot was "
        "held clear by SIDELOAD_CLEAR");
  CHECK((kmac_err >> 24) == 0x1u,
        "Expected KMAC ErrKeyNotValid (0x1), got 0x%x", kmac_err);

  // Wait for KMAC to return to sha3_idle after dif_kmac_err_processed()
  while ((abs_mmio_read32(kKmacBase + KMAC_STATUS_REG_OFFSET) &
          (1u << KMAC_STATUS_SHA3_IDLE_BIT)) == 0u) {
  }
  CHECK_STATUS_OK(kmac_testutils_config(kmac, /*sideload=*/true));

  // Now run GEN_HW_OUT (DEST_SEL = KMAC) with SIDELOAD_CLEAR == 0 and verify
  // KMAC sideload succeeds without ErrKeyNotValid!
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_START_REG_OFFSET, 1u);
  CHECK(keymgr_wait_done(&err) ==
        KEYMGR_DPE_OP_STATUS_STATUS_VALUE_DONE_SUCCESS);
  CHECK(err == 0);

  CHECK_STATUS_OK(kmac_testutils_kmac(kmac, kDifKmacModeKmacLen128,
                                      &kDummySwKey, NULL, 0, "test", 4,
                                      ARRAYSIZE(kmac_out), kmac_out, NULL));
  // Reset KMAC_CFG_SHADOWED.KMAC_EN back to 0 so subsequent keymgr_dpe KMAC
  // App-Interface requests are not blocked by KMAC software mode.
  CHECK_STATUS_OK(kmac_testutils_config(kmac, /*sideload=*/true));
}

/**
 * Test 6: (CONFIRMED_PRESENT_ON_V2)
 * `data_valid_o` gating (`~invalid_op` at `keymgr_dpe_ctrl.sv:284`) preserves
 * existing `SW_SHARE0/1_OUTPUT` and sideload keys when `KEY_VERSION >
 * slot.max_key_version` fails with `ERR_CODE = INVALID_OP |
 * INVALID_KMAC_INPUT (0x3)`.
 */
static void test_errata_keymgr_005_data_valid_gating_on_invalid_version(void) {
  LOG_INFO("Testing data_valid_o gating on invalid version");

  // Generate a valid SW key on slot 2 (version 1 <= max_key_version 10).
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_KEY_VERSION_REG_OFFSET, 1u);
  keymgr_write_shadowed(
      KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      keymgr_make_ctrl(
          KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_GENERATE_SW_OUTPUT,
          KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, 2, 2, false));
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_START_REG_OFFSET, 1u);
  uint32_t err = 0;
  CHECK(keymgr_wait_done(&err) ==
        KEYMGR_DPE_OP_STATUS_STATUS_VALUE_DONE_SUCCESS);
  uint32_t expected_sh0_w1 =
      abs_mmio_read32(kKeymgrBase + KEYMGR_DPE_SW_SHARE0_OUTPUT_1_REG_OFFSET);
  uint32_t expected_sh1_w1 =
      abs_mmio_read32(kKeymgrBase + KEYMGR_DPE_SW_SHARE1_OUTPUT_1_REG_OFFSET);
  uint32_t expected_unmasked_w1 = expected_sh0_w1 ^ expected_sh1_w1;
  CHECK((expected_sh0_w1 | expected_sh1_w1) != 0u);

  // Re-generate the same valid SW key (so SW_SHARE0/1_OUTPUT_1 hold
  // valid shares whose XOR is expected_unmasked_w1 prior to the failing call,
  // since SW_SHARE*_OUTPUT is `rc` read-clear!).
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_START_REG_OFFSET, 1u);
  CHECK(keymgr_wait_done(&err) ==
        KEYMGR_DPE_OP_STATUS_STATUS_VALUE_DONE_SUCCESS);

  // Now invoke GEN_SW_OUT with unauthorized KEY_VERSION = 99 (> 10).
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_DEBUG_REG_OFFSET, 0u);
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_KEY_VERSION_REG_OFFSET, 99u);
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_START_REG_OFFSET, 1u);
  uint32_t st = keymgr_wait_done(&err);
  uint32_t dbg = abs_mmio_read32(kKeymgrBase + KEYMGR_DPE_DEBUG_REG_OFFSET);
  CHECK(st == KEYMGR_DPE_OP_STATUS_STATUS_VALUE_DONE_ERROR);
  CHECK(err == ((1u << KEYMGR_DPE_ERR_CODE_INVALID_OP_BIT) |
                (1u << KEYMGR_DPE_ERR_CODE_INVALID_KMAC_INPUT_BIT)),
        "Expected ERR_CODE == 0x3 (INVALID_OP | INVALID_KMAC_INPUT), got 0x%x",
        err);
  CHECK((dbg & (1u << KEYMGR_DPE_DEBUG_INVALID_KEY_VERSION_BIT)) != 0u);

  // Verify SW_SHARE0_OUTPUT_1 and SW_SHARE1_OUTPUT_1 were preserved untouched!
  uint32_t preserved_sh0_w1 =
      abs_mmio_read32(kKeymgrBase + KEYMGR_DPE_SW_SHARE0_OUTPUT_1_REG_OFFSET);
  uint32_t preserved_sh1_w1 =
      abs_mmio_read32(kKeymgrBase + KEYMGR_DPE_SW_SHARE1_OUTPUT_1_REG_OFFSET);
  CHECK((preserved_sh0_w1 | preserved_sh1_w1) != 0u);
  CHECK((preserved_sh0_w1 ^ preserved_sh1_w1) == expected_unmasked_w1);
}

/**
 * Test 7: (CONFIRMED_PRESENT_ON_V2)
 * Failed `ADVANCE` preserves `WORKING_STATE = Available` and existing DPE key
 * slots (`SlotUpdateIdle`), while holding `unlock_after_advance_o = 0` so
 * `SW_BINDING_REGWEN` stays locked at `0` until a subsequent valid `ADVANCE`.
 */
static void test_errata_keymgr_006_failed_advance_locks_sw_binding(void) {
  LOG_INFO(
      "Testing Failed ADVANCE preserves slot but keeps "
      "SW_BINDING_REGWEN == 0");

  // Lock SW_BINDING_REGWEN = 0.
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_SW_BINDING_REGWEN_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kKeymgrBase +
                        KEYMGR_DPE_SW_BINDING_REGWEN_REG_OFFSET) == 0u);

  // Slot 2 has retain_parent = 1 (`SLOT_POLICY = 0x5`), so advancing in-place
  // (`src = 2, dst = 2`) violates `invalid_retain_parent` (`slot_src_sel ==
  // slot_dst_sel` when `retain_parent == 1` in `keymgr_dpe_ctrl.sv:686-688`)!
  keymgr_write_shadowed(
      KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      keymgr_make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_ADVANCE,
                       KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, 2, 2,
                       /*sw_binding_only=*/true));
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_START_REG_OFFSET, 1u);

  uint32_t err = 0;
  uint32_t st = keymgr_wait_done(&err);
  CHECK(st == KEYMGR_DPE_OP_STATUS_STATUS_VALUE_DONE_ERROR);
  CHECK(err == ((1u << KEYMGR_DPE_ERR_CODE_INVALID_OP_BIT) |
                (1u << KEYMGR_DPE_ERR_CODE_INVALID_KMAC_INPUT_BIT)));
  CHECK(abs_mmio_read32(kKeymgrBase + KEYMGR_DPE_WORKING_STATE_REG_OFFSET) ==
        KEYMGR_DPE_WORKING_STATE_STATE_VALUE_AVAILABLE);

  // Verify SW_BINDING_REGWEN is STILL locked at 0 after the failed ADVANCE!
  CHECK(abs_mmio_read32(kKeymgrBase +
                        KEYMGR_DPE_SW_BINDING_REGWEN_REG_OFFSET) == 0u);

  // Now perform a valid ADVANCE from slot 2 -> slot 3 (`src = 2 != dst = 3`
  // satisfies `retain_parent == 1`) and verify SW_BINDING_REGWEN unlocks to 1!
  keymgr_write_shadowed(
      KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      keymgr_make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_ADVANCE,
                       KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, 2, 3,
                       /*sw_binding_only=*/true));
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_START_REG_OFFSET, 1u);
  st = keymgr_wait_done(&err);
  CHECK(st == KEYMGR_DPE_OP_STATUS_STATUS_VALUE_DONE_SUCCESS);
  CHECK(err == 0);
  CHECK(abs_mmio_read32(kKeymgrBase +
                        KEYMGR_DPE_SW_BINDING_REGWEN_REG_OFFSET) == 1u);
}

/**
 * Test 8: (NEW_IN_V2, `TODO(#30682)`)
 * Out-of-bounds 3-bit `SLOT_SRC_SEL` / `SLOT_DST_SEL` (`4..7`) in
 * `keymgr_dpe.sv:319-336` silently truncates modulo 4 (`slot & 0x3`) instead
 * of raising `ERR_CODE.INVALID_OP`.
 */
static void test_errata_v2_03_out_of_bounds_slot_truncation_aliasing(void) {
  LOG_INFO(
      "Testing Out-of-bounds slot 6 silently aliases "
      "and erases slot 2");

  // Generate SW key from valid slot 2 (`SLOT_SRC_SEL = 2`).
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_KEY_VERSION_REG_OFFSET, 0u);
  keymgr_write_shadowed(
      KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      keymgr_make_ctrl(
          KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_GENERATE_SW_OUTPUT,
          KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, 2, 2, false));
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_START_REG_OFFSET, 1u);
  uint32_t err = 0;
  CHECK(keymgr_wait_done(&err) ==
        KEYMGR_DPE_OP_STATUS_STATUS_VALUE_DONE_SUCCESS);
  uint32_t slot2_key_w0 =
      abs_mmio_read32(kKeymgrBase + KEYMGR_DPE_SW_SHARE0_OUTPUT_0_REG_OFFSET) ^
      abs_mmio_read32(kKeymgrBase + KEYMGR_DPE_SW_SHARE1_OUTPUT_0_REG_OFFSET);

  // Now generate SW key using out-of-bounds `SLOT_SRC_SEL = 6` (`6 & 0x3 ==
  // 2`)!
  keymgr_write_shadowed(
      KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      keymgr_make_ctrl(
          KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_GENERATE_SW_OUTPUT,
          KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, 6, 6, false));
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_START_REG_OFFSET, 1u);
  uint32_t st = keymgr_wait_done(&err);
  CHECK(st == KEYMGR_DPE_OP_STATUS_STATUS_VALUE_DONE_SUCCESS,
        "Expected out-of-bounds SLOT_SRC_SEL=6 to silently succeed (alias "
        "slot 2)");
  CHECK(err == 0);
  uint32_t slot6_key_w0 =
      abs_mmio_read32(kKeymgrBase + KEYMGR_DPE_SW_SHARE0_OUTPUT_0_REG_OFFSET) ^
      abs_mmio_read32(kKeymgrBase + KEYMGR_DPE_SW_SHARE1_OUTPUT_0_REG_OFFSET);
  CHECK(slot6_key_w0 == slot2_key_w0,
        "Expected SLOT_SRC_SEL=6 output (0x%x) to equal slot 2 output (0x%x)",
        slot6_key_w0, slot2_key_w0);

  // Now invoke OpDpeErase (`OPERATION = 1`) on out-of-bounds `SLOT_DST_SEL = 6`
  // (`6 & 0x3 == 2`) -> silently erases hardware slot 2!
  keymgr_write_shadowed(
      KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      keymgr_make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_ERASE_SLOT,
                       KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, 0, 6,
                       false));
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_START_REG_OFFSET, 1u);
  CHECK(keymgr_wait_done(&err) ==
        KEYMGR_DPE_OP_STATUS_STATUS_VALUE_DONE_SUCCESS);
  CHECK(err == 0);

  // Prove hardware slot 2 (`SLOT_SRC_SEL = 2`) was wiped by the out-of-bounds
  // erase on slot 6!
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_DEBUG_REG_OFFSET, 0u);
  keymgr_write_shadowed(
      KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      keymgr_make_ctrl(
          KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_GENERATE_SW_OUTPUT,
          KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, 2, 2, false));
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_START_REG_OFFSET, 1u);
  st = keymgr_wait_done(&err);
  uint32_t dbg = abs_mmio_read32(kKeymgrBase + KEYMGR_DPE_DEBUG_REG_OFFSET);
  CHECK(st == KEYMGR_DPE_OP_STATUS_STATUS_VALUE_DONE_ERROR,
        "Expected slot 2 to be invalid after out-of-bounds erase on slot 6");
  CHECK(err == (1u << KEYMGR_DPE_ERR_CODE_INVALID_OP_BIT),
        "Expected ERR_CODE == 0x1 (INVALID_OP) on erased slot 2, got 0x%x",
        err);
  // Because SlotErase fills key_slots_q[2].key with PRNG entropy and
  // keymgr_dpe_input_checks.sv:88 ignores key_i.valid (unused_key_vld),
  // key_vld == 1 so INVALID_KMAC_INPUT and DEBUG.INVALID_KEY remain 0 even
  // though active_key_slot.valid == 0 triggers ERR_CODE.INVALID_OP (0x1).
  CHECK((dbg & (1u << KEYMGR_DPE_DEBUG_INVALID_KEY_BIT)) == 0u);

  // Also invoke ADVANCE on erased slot 2 (SLOT_SRC_SEL = 2, SLOT_DST_SEL = 2):
  // because invalid_data[OpDpeAdvance] = ~key_vld | invalid_advance | ...
  // (keymgr_dpe.sv:677) includes invalid_advance (~active_key_slot_o.valid),
  // ERR_CODE is 0x3 (INVALID_OP | INVALID_KMAC_INPUT), while DEBUG.INVALID_KEY
  // (hw2reg.debug.invalid_key.d = ~key_vld) still remains 0!
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_DEBUG_REG_OFFSET, 0u);
  keymgr_write_shadowed(
      KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      keymgr_make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_ADVANCE,
                       KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, 2, 2,
                       /*sw_binding_only=*/true));
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_START_REG_OFFSET, 1u);
  st = keymgr_wait_done(&err);
  dbg = abs_mmio_read32(kKeymgrBase + KEYMGR_DPE_DEBUG_REG_OFFSET);
  CHECK(st == KEYMGR_DPE_OP_STATUS_STATUS_VALUE_DONE_ERROR);
  CHECK(err == ((1u << KEYMGR_DPE_ERR_CODE_INVALID_OP_BIT) |
                (1u << KEYMGR_DPE_ERR_CODE_INVALID_KMAC_INPUT_BIT)));
  CHECK((dbg & (1u << KEYMGR_DPE_DEBUG_INVALID_KEY_BIT)) == 0u);
}

/**
 * Test 9A (Boot 1): (NEW_IN_V2)
 * Programming unenumerated `CONTROL_SHADOWED.OPERATION = 6` (`3'b110`) in
 * `StCtrlDpeAvailable` (`WORKING_STATE == 1`) sets `op_req = 1` with all
 * `*_req = 0` and `invalid_op = 0`, permanently deadlocking `keymgr_dpe` in
 * `OP_STATUS = WIP (1)` (`START = 1`, `CFG_REGWEN = 0`) until hardware reset!
 */
static void test_errata_v2_01_unenumerated_op_deadlock_in_available(void) {
  LOG_INFO(
      "Testing Unenumerated OPERATION=6 permanent WIP "
      "deadlock in Available state");

  keymgr_write_shadowed(
      KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      keymgr_make_ctrl(/*op=*/6u,
                       KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, 3, 3,
                       false));
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_START_REG_OFFSET, 1u);

  // Spin for 100us (thousands of clock cycles — far longer than any
  // single-cycle or KMAC operation) and verify `keymgr_dpe` is permanently
  // stuck in WIP!
  busy_spin_micros(100);

  uint32_t op_status =
      abs_mmio_read32(kKeymgrBase + KEYMGR_DPE_OP_STATUS_REG_OFFSET);
  uint32_t start_reg =
      abs_mmio_read32(kKeymgrBase + KEYMGR_DPE_START_REG_OFFSET);
  uint32_t cfg_regwen =
      abs_mmio_read32(kKeymgrBase + KEYMGR_DPE_CFG_REGWEN_REG_OFFSET);
  uint32_t err_code =
      abs_mmio_read32(kKeymgrBase + KEYMGR_DPE_ERR_CODE_REG_OFFSET);

  CHECK(op_status == KEYMGR_DPE_OP_STATUS_STATUS_VALUE_WIP,
        "Expected permanent OP_STATUS == WIP (1) deadlock on OPERATION=6, "
        "got 0x%x",
        op_status);
  CHECK(start_reg == 1u, "Expected START == 1 during deadlock, got 0x%x",
        start_reg);
  CHECK(cfg_regwen == 0u, "Expected CFG_REGWEN == 0 during deadlock, got 0x%x",
        cfg_regwen);
  CHECK(err_code == 0u, "Expected ERR_CODE == 0 during deadlock, got 0x%x",
        err_code);
}

/**
 * Test 9B (Boot 2 after SW reset): (FIXED_IN_V2_RTL)
 * In `StCtrlDpeDisabled` (`WORKING_STATE = Disabled (2)`), `keymgr_dpe` sets
 * `op_req = 0` and `invalid_op = op_start_i` (`keymgr_dpe_ctrl.sv:580-587`),
 * completing in 1 cycle with `OP_STATUS = DONE_ERROR (3)` and
 * `ERR_CODE.INVALID_OP = 1` without invoking KMAC.
 */
static void test_errata_keymgr_003_fixed_in_v2_disabled_state(void) {
  LOG_INFO(
      "Testing (FIXED_IN_V2_RTL): Disabled state fast "
      "rejection without KMAC");

  // Transition from Available (1) to Disabled (2).
  keymgr_write_shadowed(
      KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      keymgr_make_ctrl(KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_VALUE_DISABLE,
                       KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, 0, 0,
                       false));
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_START_REG_OFFSET, 1u);
  uint32_t err = 0;
  CHECK(keymgr_wait_done(&err) ==
        KEYMGR_DPE_OP_STATUS_STATUS_VALUE_DONE_SUCCESS);
  CHECK(abs_mmio_read32(kKeymgrBase + KEYMGR_DPE_WORKING_STATE_REG_OFFSET) ==
        KEYMGR_DPE_WORKING_STATE_STATE_VALUE_DISABLED);

  // In Disabled state (2), CFG_REGWEN is still 1; issuing START=1 immediately
  // finishes in 1 cycle (`op_done_o = invalid_op = 1`) with DONE_ERROR (3) and
  // ERR_CODE.INVALID_OP = 1 (even for unenumerated OPERATION=6!).
  keymgr_write_shadowed(
      KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET,
      keymgr_make_ctrl(/*op=*/6u,
                       KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_VALUE_NONE, 0, 0,
                       false));
  abs_mmio_write32(kKeymgrBase + KEYMGR_DPE_START_REG_OFFSET, 1u);
  uint32_t st = keymgr_wait_done(&err);
  CHECK(st == KEYMGR_DPE_OP_STATUS_STATUS_VALUE_DONE_ERROR);
  CHECK(err == (1u << KEYMGR_DPE_ERR_CODE_INVALID_OP_BIT));
  CHECK(abs_mmio_read32(kKeymgrBase + KEYMGR_DPE_CFG_REGWEN_REG_OFFSET) == 1u);
}

bool test_main(void) {
  irq_external_ctrl(false);
  dif_rstmgr_t rstmgr;
  dif_keymgr_dpe_t keymgr_dpe;
  dif_kmac_t kmac;

  CHECK_DIF_OK(dif_rstmgr_init(mmio_region_from_addr(kRstmgrBase), &rstmgr));
  CHECK_DIF_OK(
      dif_keymgr_dpe_init(mmio_region_from_addr(kKeymgrBase), &keymgr_dpe));
  CHECK_DIF_OK(dif_kmac_init(mmio_region_from_addr(kKmacBase), &kmac));
  CHECK_STATUS_OK(entropy_testutils_auto_mode_init());
  CHECK_STATUS_OK(kmac_testutils_config(&kmac, /*sideload=*/true));

  dif_rstmgr_reset_info_bitfield_t reset_info = rstmgr_testutils_reason_get();
  if ((reset_info & kDifRstmgrResetInfoSw) == 0) {
    LOG_INFO(
        "=== Boot 1 (POR): Running keymgr_dpe v2 errata suite (1..9A) ===");
    test_errata_keymgr_007_permit_and_addrmiss();
    test_errata_v2_04_and_v2_02_uds_reload_and_regwen_lock(&keymgr_dpe);
    test_errata_keymgr_001_fixed_in_v2_and_regwen_unlock();
    test_errata_keymgr_002_cfg_regwen_dynamic_gating();
    test_errata_keymgr_004_sideload_clear_level_priority(&kmac);
    test_errata_keymgr_005_data_valid_gating_on_invalid_version();
    test_errata_keymgr_006_failed_advance_locks_sw_binding();
    test_errata_v2_03_out_of_bounds_slot_truncation_aliasing();
    test_errata_v2_01_unenumerated_op_deadlock_in_available();

    // Recover from the permanent hardware deadlock via
    // software device reset to run Boot 2 (Test 9B).
    rstmgr_testutils_reason_clear();
    CHECK_DIF_OK(dif_rstmgr_software_device_reset(&rstmgr));
    wait_for_interrupt();
    return false;
  }

  LOG_INFO("=== Boot 2 (SW Reset): Running Test 9B ===");
  test_errata_keymgr_003_fixed_in_v2_disabled_state();
  LOG_INFO("=== All keymgr_dpe v2 errata tests PASSED on CW340 FPGA! ===");
  return true;
}
