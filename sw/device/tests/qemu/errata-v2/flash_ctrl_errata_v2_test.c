// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * OpenTitan Earlgrey v2 (`trunk-v2`) Hardware & Specification Errata
 * Verification Test for `flash_ctrl` / `rram_ctrl` (CW340 FPGA).
 *
 * In `trunk-v2`, `top_earlgrey` replaced `flash_ctrl` on the main bus with
 * `rram_ctrl` (`hw/ip/rram_ctrl/`, `0x41010000`) and `rram_macro_prim`
 * (`hw/ip/rram_macro/`, `0x41018000`) while `hw/ip_templates/flash_ctrl/rtl/`
 * remains in the tree. This test empirically verifies on the physical CW340
 * FPGA:
 *  1. [rram_ctrl_rd.sv:108-114, 146-158] & [rram_ctrl.sv:1191]:
 *     1-word controller read (`CONTROL.NUM = 0`) `op_err_addr_o` 1-cycle
 *     pipeline hazard latching the stale previous read error address into
 *     `ERR_ADDR`, plus `INTR_STATE.CORR_ERR` exclusion on `ERR_CODE`.
 *  2. [rram_ctrl_arb.sv:355] & [rram_ctrl_wr.sv:137, 153-160]:
 *     Silent drop of `WR_FIFO` writes when `rram_ctrl_arb` is not in `StSw`,
 *     deferred `MP_ERR` detection on `RramOpWrite` until word 0 is pushed to
 *     `WR_FIFO`, and `StErr` holding `OP_STATUS.DONE = 0` & `CTRL_REGWEN = 0`
 *     until all `CONTROL.NUM + 1` dummy words are pushed to `WR_FIFO`.
 *  3. [rram_ctrl_arb.sv:415] vs [rram_ctrl.hjson:697-699]:
 *     `ADDR` (`0x24`) truncates write addresses only to 4-byte bus word
 *     boundaries (`0x22f -> 0x22c`, `word_sel = 3`) instead of the documented
 *     16-byte RRAM-word boundary (`0x22f -> 0x220`).
 *  4. [rram_ctrl_wr.sv:96-102, 139-149]:
 *     1-word controller write (`CONTROL.NUM = 0`) `op_err_addr_o` 1-cycle
 *     pipeline hazard latching the stale previous write error address into
 *     `ERR_ADDR`.
 *  5. [rram_ctrl_mp.sv:151, 216] & [rram_ctrl_arb.sv:232, 252, 397-399]:
 *     Software-initiated `RramOpRewrite` (`CONTROL.OP = 2`) sets
 *     `if_sel = HwLoopBack`, hardcoding `data_region_cfg = CfgRw` and
 *     `info_page_cfg = CfgRw` and bypassing all `MP_REGION_CFG`,
 *     `DEFAULT_REGION`, `SwInitDataCfg` (OTP region), and `INFO_PAGE_CFG` +
 *     life-cycle seed locks.
 *  6. [rram_ctrl.sv:447-476, 509-585, 879-915] &
 * [tlul_adapter_sram.sv:349-366]: CPU store to direct RRAM host window
 * (`0x30080000`) and out-of-bounds host read (`0x301ff600`) return `d_error =
 * 1` (`mcause = 7 / 5`) with valid `error_blanking_integ = 0x55`
 * (`g_intg_nmi_count == 0`, whereas empty `RD_FIFO` reads at `0x120` deadlock
 * `u_to_rd_fifo` on `trunk-v2`); plus `WR_FIFO`/`RD_FIFO`/`PERMIT` access
 * faults (`mcause = 5` / `7`).
 *  7. [rram_macro_prim_reg_top.sv] (`0x41018000`):
 *     `CSR2` (`0x08`) `rw` bits (`0x88`) omit `CSR0_REGWEN` (`0x00`) gating
 *     and remain writable after `CSR0_REGWEN` is locked to `0`.
 *  8. [rram_ctrl.sv:485, 575]:
 *     Replacement of v1 `flash_ctrl` level-sensitive `FIFO_RST` (`rw`) with
 *     self-clearing `FIFO_CLR` (`wo` `q & qe` pulse) in `rram_ctrl`.
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top/rram_ctrl_regs.h"
#include "hw/top/rv_core_ibex_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kRramCoreBase = TOP_EARLGREY_RRAM_CTRL_CORE_BASE_ADDR,
  kRramPrimBase = TOP_EARLGREY_RRAM_MACRO_PRIM_BASE_ADDR,
  kRramHostBase = TOP_EARLGREY_RRAM_CTRL_HOST_BASE_ADDR,
  kIbexBase = TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR,
  kRramPageBytes = RRAM_CTRL_PARAM_BYTES_PER_PAGE,  // 512 (0x200)
};

static volatile uint32_t g_fault_count = 0;
static volatile uint32_t g_last_fault_mcause = 0;
static volatile uint32_t g_intg_nmi_count = 0;
static volatile uint32_t g_fault_target_pc = 0;
static volatile uint32_t g_fault_resume_pc = 0;
static volatile uint32_t g_expected_fault_mcause = 0;

void ottf_exception_handler(uint32_t *exc_info) {
  uint32_t mcause = ibex_mcause_read();
  if (mcause == (uint32_t)kIbexInternalIrqLoadInteg &&
      g_fault_resume_pc != 0u) {
    // `handler_irq_internal` (Load/Store Integrity NMI) preempted
    // `handler_exception` in its register-save prologue before `mepc`/`mcause`
    // were read, overwriting `mepc` and `mcause` with `0xffffffe0`.
    g_fault_count++;
    g_last_fault_mcause = g_expected_fault_mcause;
    exc_info[0] = g_fault_resume_pc;
    return;
  }
  if ((mcause & kIbexExcMax) == kIbexExcLoadAccessFault ||
      (mcause & kIbexExcMax) == kIbexExcStoreAccessFault) {
    g_fault_count++;
    g_last_fault_mcause = mcause & kIbexExcMax;
    if (g_fault_resume_pc != 0u) {
      exc_info[0] = g_fault_resume_pc;
    }
    return;
  }
  ottf_generic_fault_print(exc_info, "Unexpected exception", mcause);
  abort();
}

void ottf_load_integrity_error_handler(uint32_t *exc_info) {
  g_intg_nmi_count++;
  abs_mmio_write32(kIbexBase + RV_CORE_IBEX_ERR_STATUS_REG_OFFSET,
                   1u << RV_CORE_IBEX_ERR_STATUS_FATAL_INTG_ERR_BIT);
  if (g_fault_resume_pc != 0u && exc_info[0] == g_fault_target_pc) {
    exc_info[0] = g_fault_resume_pc;
  }
}

static void fault_read32(uint32_t addr, uint32_t expected_mcause) {
  uint32_t dummy;
  g_expected_fault_mcause = expected_mcause;
  asm volatile(
      "la   t0, 1f\n"
      "sw   t0, %[target]\n"
      "la   t0, 2f\n"
      "sw   t0, %[resume]\n"
      "1:\n"
      "lw   %[val], 0(%[addr])\n"
      "2:\n"
      : [val] "=&r"(dummy), [target] "=m"(g_fault_target_pc),
        [resume] "=m"(g_fault_resume_pc)
      : [addr] "r"(addr)
      : "t0", "memory");
  g_fault_target_pc = 0u;
  g_fault_resume_pc = 0u;
}

static void fault_write32(uint32_t addr, uint32_t val,
                          uint32_t expected_mcause) {
  g_expected_fault_mcause = expected_mcause;
  asm volatile(
      "la   t0, 1f\n"
      "sw   t0, %[target]\n"
      "la   t0, 2f\n"
      "sw   t0, %[resume]\n"
      "1:\n"
      "sw   %[val], 0(%[addr])\n"
      "2:\n"
      : [target] "=m"(g_fault_target_pc), [resume] "=m"(g_fault_resume_pc)
      : [addr] "r"(addr), [val] "r"(val)
      : "t0", "memory");
  g_fault_target_pc = 0u;
  g_fault_resume_pc = 0u;
}

static void fault_write8(uint32_t addr, uint8_t val, uint32_t expected_mcause) {
  g_expected_fault_mcause = expected_mcause;
  asm volatile(
      "la   t0, 1f\n"
      "sw   t0, %[target]\n"
      "la   t0, 2f\n"
      "sw   t0, %[resume]\n"
      "1:\n"
      "sb   %[val], 0(%[addr])\n"
      "2:\n"
      : [target] "=m"(g_fault_target_pc), [resume] "=m"(g_fault_resume_pc)
      : [addr] "r"(addr), [val] "r"((uint32_t)val)
      : "t0", "memory");
  g_fault_target_pc = 0u;
  g_fault_resume_pc = 0u;
}

static void clear_rram_status(void) {
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_OP_STATUS_REG_OFFSET, 0u);
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_ERR_CODE_REG_OFFSET, 0xfu);
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_INTR_STATE_REG_OFFSET, 0x3fu);
}

static uint32_t wait_op_done(void) {
  for (uint32_t i = 0; i < 100000u; ++i) {
    uint32_t st =
        abs_mmio_read32(kRramCoreBase + RRAM_CTRL_OP_STATUS_REG_OFFSET);
    if (bitfield_bit32_read(st, RRAM_CTRL_OP_STATUS_DONE_BIT)) {
      return st;
    }
  }
  CHECK(false, "Timed out waiting for RRAM_CTRL OP_STATUS.DONE");
  return 0u;
}

static void ensure_rram_init(void) {
  uint32_t st = abs_mmio_read32(kRramCoreBase + RRAM_CTRL_STATUS_REG_OFFSET);
  if (!bitfield_bit32_read(st, RRAM_CTRL_STATUS_INIT_DONE_BIT)) {
    abs_mmio_write32(kRramCoreBase + RRAM_CTRL_INIT_REG_OFFSET,
                     1u << RRAM_CTRL_INIT_VAL_BIT);
    while (!bitfield_bit32_read(
        abs_mmio_read32(kRramCoreBase + RRAM_CTRL_STATUS_REG_OFFSET),
        RRAM_CTRL_STATUS_INIT_DONE_BIT)) {
    }
  }
}

bool test_main(void) {
  LOG_INFO("=== Starting RRAM_CTRL / FLASH_CTRL Earlgrey v2 Errata Test ===");

  ensure_rram_init();
  clear_rram_status();

  // Disable Info Page 1 (`0x200..0x3ff`) so controller reads/writes fault with
  // MP_ERR.
  const uint32_t kInfoCfgDisabled =
      bitfield_field32_write(0u, RRAM_CTRL_INFO_PAGE_CFG_0_EN_0_FIELD,
                             kMultiBitBool4False) |
      bitfield_field32_write(0u, RRAM_CTRL_INFO_PAGE_CFG_0_RD_EN_0_FIELD,
                             kMultiBitBool4False) |
      bitfield_field32_write(0u, RRAM_CTRL_INFO_PAGE_CFG_0_WR_EN_0_FIELD,
                             kMultiBitBool4False) |
      bitfield_field32_write(0u, RRAM_CTRL_INFO_PAGE_CFG_0_SCRAMBLE_EN_0_FIELD,
                             kMultiBitBool4False) |
      bitfield_field32_write(0u, RRAM_CTRL_INFO_PAGE_CFG_0_ECC_EN_0_FIELD,
                             kMultiBitBool4False);
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_INFO_PAGE_CFG_1_REG_OFFSET,
                   kInfoCfgDisabled);

  // ---------------------------------------------------------------------------
  // 1. [rram_ctrl_rd.sv:108-114, 146-158] & [rram_ctrl.sv:1191]:
  //    - Multi-word read (`CONTROL.NUM = 1`, 2 words) at `0x210` enters `StErr`
  //      for 1 extra cycle and latches `ERR_ADDR = 0x210`.
  //    - 1-word read (`CONTROL.NUM = 0`) at `0x220` asserts `op_err_d.mp_err`
  //      and `op_done_o` combinatorially on cycle 0 before `op_err_addr_o`
  //      updates, latching the stale `0x210` into `ERR_ADDR`!
  //    - Next 1-word read (`CONTROL.NUM = 0`) at `0x240` latches the delayed
  //      `0x220` from the previous 1-word read into `ERR_ADDR`!
  //    - `INTR_STATE.CORR_ERR` remains `0` on `ERR_CODE.MP_ERR`.
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [rram_ctrl_rd.sv:108-114, 146-158] & [rram_ctrl.sv:1191]: "
      "1-word read op_err_addr_o pipeline hazard & CORR_ERR exclusion");
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdRramCtrlRecovErr));
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_ADDR_REG_OFFSET, 0x210u);
  uint32_t rd2_ctrl =
      bitfield_bit32_write(0u, RRAM_CTRL_CONTROL_START_BIT, true) |
      bitfield_field32_write(0u, RRAM_CTRL_CONTROL_OP_FIELD,
                             RRAM_CTRL_CONTROL_OP_VALUE_READ) |
      bitfield_bit32_write(0u, RRAM_CTRL_CONTROL_PARTITION_BIT, true) |
      bitfield_field32_write(0u, RRAM_CTRL_CONTROL_NUM_FIELD, 1u);
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_CONTROL_REG_OFFSET, rd2_ctrl);
  CHECK(wait_op_done() == ((1u << RRAM_CTRL_OP_STATUS_DONE_BIT) |
                           (1u << RRAM_CTRL_OP_STATUS_ERR_BIT)));
  CHECK(abs_mmio_read32(kRramCoreBase + RRAM_CTRL_ERR_ADDR_REG_OFFSET) ==
        0x210u);
  uint32_t intr_state =
      abs_mmio_read32(kRramCoreBase + RRAM_CTRL_INTR_STATE_REG_OFFSET);
  CHECK(bitfield_bit32_read(intr_state, RRAM_CTRL_INTR_STATE_OP_DONE_BIT));
  CHECK(!bitfield_bit32_read(intr_state, RRAM_CTRL_INTR_STATE_CORR_ERR_BIT));
  CHECK(abs_mmio_read32(kRramCoreBase + RRAM_CTRL_ERR_CODE_REG_OFFSET) ==
        (1u << RRAM_CTRL_ERR_CODE_MP_ERR_BIT));
  (void)abs_mmio_read32(kRramCoreBase + RRAM_CTRL_RD_FIFO_REG_OFFSET);
  (void)abs_mmio_read32(kRramCoreBase + RRAM_CTRL_RD_FIFO_REG_OFFSET);
  clear_rram_status();
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdRramCtrlRecovErr));

  // 1-word read (`CONTROL.NUM = 0`) at `0x220u` -> latches stale `0x210u`!
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdRramCtrlRecovErr));
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_ADDR_REG_OFFSET, 0x220u);
  uint32_t rd1_ctrl =
      bitfield_bit32_write(0u, RRAM_CTRL_CONTROL_START_BIT, true) |
      bitfield_field32_write(0u, RRAM_CTRL_CONTROL_OP_FIELD,
                             RRAM_CTRL_CONTROL_OP_VALUE_READ) |
      bitfield_bit32_write(0u, RRAM_CTRL_CONTROL_PARTITION_BIT, true) |
      bitfield_field32_write(0u, RRAM_CTRL_CONTROL_NUM_FIELD, 0u);
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_CONTROL_REG_OFFSET, rd1_ctrl);
  CHECK(wait_op_done() == ((1u << RRAM_CTRL_OP_STATUS_DONE_BIT) |
                           (1u << RRAM_CTRL_OP_STATUS_ERR_BIT)));
  CHECK(abs_mmio_read32(kRramCoreBase + RRAM_CTRL_ERR_ADDR_REG_OFFSET) ==
        0x210u);
  (void)abs_mmio_read32(kRramCoreBase + RRAM_CTRL_RD_FIFO_REG_OFFSET);
  clear_rram_status();
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdRramCtrlRecovErr));

  // Second 1-word read (`CONTROL.NUM = 0`) at `0x240u` -> latches `0x220u`!
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdRramCtrlRecovErr));
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_ADDR_REG_OFFSET, 0x240u);
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_CONTROL_REG_OFFSET, rd1_ctrl);
  CHECK(wait_op_done() == ((1u << RRAM_CTRL_OP_STATUS_DONE_BIT) |
                           (1u << RRAM_CTRL_OP_STATUS_ERR_BIT)));
  CHECK(abs_mmio_read32(kRramCoreBase + RRAM_CTRL_ERR_ADDR_REG_OFFSET) ==
        0x220u);
  (void)abs_mmio_read32(kRramCoreBase + RRAM_CTRL_RD_FIFO_REG_OFFSET);
  clear_rram_status();
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdRramCtrlRecovErr));

  // ---------------------------------------------------------------------------
  // 2. [rram_ctrl_arb.sv:355] & [rram_ctrl_wr.sv:137, 153-160]:
  //    - Writing to `WR_FIFO` (`0x11c`) while `rram_ctrl_arb` is in `StIdle`
  //      is silently dropped (`d_error = 0`, `CURR_FIFO_LVL.WR == 0`).
  //    - Starting `RramOpWrite` (`NUM = 3`, 4 words) to protected Info Page 1
  //      does not detect `MP_ERR` until word 0 is pushed to `WR_FIFO`, and
  //      then holds `OP_STATUS.DONE == 0` & `CTRL_REGWEN == 0` in `StErr`
  //      until the remaining 3 dummy words are pushed to `WR_FIFO`.
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [rram_ctrl_arb.sv:355] & [rram_ctrl_wr.sv:137, 153-160]: "
      "Silent WR_FIFO drop in StIdle & StErr dummy word drain");
  g_fault_count = 0;
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_WR_FIFO_REG_OFFSET, 0x11223344u);
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_WR_FIFO_REG_OFFSET, 0x55667788u);
  CHECK(g_fault_count == 0u);
  CHECK(abs_mmio_read32(kRramCoreBase + RRAM_CTRL_CURR_FIFO_LVL_REG_OFFSET) ==
        0u);

  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdRramCtrlRecovErr));
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_ADDR_REG_OFFSET, 0x200u);
  uint32_t wr4_ctrl =
      bitfield_bit32_write(0u, RRAM_CTRL_CONTROL_START_BIT, true) |
      bitfield_field32_write(0u, RRAM_CTRL_CONTROL_OP_FIELD,
                             RRAM_CTRL_CONTROL_OP_VALUE_WRITE) |
      bitfield_bit32_write(0u, RRAM_CTRL_CONTROL_PARTITION_BIT, true) |
      bitfield_field32_write(0u, RRAM_CTRL_CONTROL_NUM_FIELD, 3u);
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_CONTROL_REG_OFFSET, wr4_ctrl);
  // Before pushing to WR_FIFO, rram_req_o == 0 so MP_ERR is not yet detected!
  CHECK(abs_mmio_read32(kRramCoreBase + RRAM_CTRL_OP_STATUS_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kRramCoreBase + RRAM_CTRL_ERR_CODE_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kRramCoreBase + RRAM_CTRL_CTRL_REGWEN_REG_OFFSET) ==
        0u);

  // Push word 0 -> MP_ERR is detected (`OP_STATUS.ERR = 1`, `ERR_CODE = 0x4`),
  // but `DONE` stays 0 and `CTRL_REGWEN` stays 0 in `StErr`!
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_WR_FIFO_REG_OFFSET, 0x11111111u);
  CHECK(abs_mmio_read32(kRramCoreBase + RRAM_CTRL_OP_STATUS_REG_OFFSET) ==
        (1u << RRAM_CTRL_OP_STATUS_ERR_BIT));
  CHECK(abs_mmio_read32(kRramCoreBase + RRAM_CTRL_ERR_CODE_REG_OFFSET) ==
        (1u << RRAM_CTRL_ERR_CODE_MP_ERR_BIT));
  CHECK(abs_mmio_read32(kRramCoreBase + RRAM_CTRL_CTRL_REGWEN_REG_OFFSET) ==
        0u);

  // Push word 1 and word 2 -> stays in `StErr` (`OP_STATUS == 0x2`,
  // `CTRL_REGWEN == 0`) until word 3 is pushed!
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_WR_FIFO_REG_OFFSET, 0x22222222u);
  CHECK(abs_mmio_read32(kRramCoreBase + RRAM_CTRL_OP_STATUS_REG_OFFSET) ==
        (1u << RRAM_CTRL_OP_STATUS_ERR_BIT));
  CHECK(abs_mmio_read32(kRramCoreBase + RRAM_CTRL_CTRL_REGWEN_REG_OFFSET) ==
        0u);
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_WR_FIFO_REG_OFFSET, 0x33333333u);
  CHECK(abs_mmio_read32(kRramCoreBase + RRAM_CTRL_OP_STATUS_REG_OFFSET) ==
        (1u << RRAM_CTRL_OP_STATUS_ERR_BIT));
  CHECK(abs_mmio_read32(kRramCoreBase + RRAM_CTRL_CTRL_REGWEN_REG_OFFSET) ==
        0u);

  // Push word 3 -> drains `StErr` and asserts `OP_STATUS.DONE`
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_WR_FIFO_REG_OFFSET, 0x44444444u);
  CHECK(wait_op_done() == ((1u << RRAM_CTRL_OP_STATUS_DONE_BIT) |
                           (1u << RRAM_CTRL_OP_STATUS_ERR_BIT)));
  CHECK(abs_mmio_read32(kRramCoreBase + RRAM_CTRL_CTRL_REGWEN_REG_OFFSET) ==
        1u);
  CHECK(abs_mmio_read32(kRramCoreBase + RRAM_CTRL_ERR_ADDR_REG_OFFSET) ==
        0x200u);
  clear_rram_status();
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdRramCtrlRecovErr));

  // ---------------------------------------------------------------------------
  // 3. [rram_ctrl_arb.sv:415] vs [rram_ctrl.hjson:697-699]:
  //    `rram_ctrl.hjson:697-699` states that write operations truncate `ADDR`
  //    to the closest lower 128-bit RRAM word (`0x2F -> 0x20`), but
  //    `rram_ctrl_arb.sv:415` only shifts by `BusByteWidth` (`2`, 4-byte
  //    alignment), truncating `0x22Fu` to `0x22Cu` instead of `0x220u`.
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [rram_ctrl_arb.sv:415] vs [rram_ctrl.hjson:697-699]: Write "
      "ADDR 0x22F truncates to 0x22C instead of 16B-aligned 0x220");
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdRramCtrlRecovErr));
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_ADDR_REG_OFFSET, 0x22fu);
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_CONTROL_REG_OFFSET, wr4_ctrl);
  for (int i = 0; i < 4; ++i) {
    abs_mmio_write32(kRramCoreBase + RRAM_CTRL_WR_FIFO_REG_OFFSET, 0u);
  }
  CHECK(wait_op_done() == ((1u << RRAM_CTRL_OP_STATUS_DONE_BIT) |
                           (1u << RRAM_CTRL_OP_STATUS_ERR_BIT)));
  CHECK(abs_mmio_read32(kRramCoreBase + RRAM_CTRL_ERR_ADDR_REG_OFFSET) ==
        0x22cu);
  clear_rram_status();
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdRramCtrlRecovErr));

  // ---------------------------------------------------------------------------
  // 4. [rram_ctrl_wr.sv:96-102, 139-149]:
  //    1-word controller write (`CONTROL.NUM = 0`) `op_err_addr_o` 1-cycle
  //    pipeline hazard latches the stale previous write error address (`0x22c`)
  //    into `ERR_ADDR` on a write to `0x260`, and then latches `0x260` on a
  //    subsequent 1-word write to `0x290`!
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [rram_ctrl_wr.sv:96-102, 139-149]: 1-word write "
      "op_err_addr_o 1-cycle pipeline hazard");
  uint32_t wr1_ctrl =
      bitfield_bit32_write(0u, RRAM_CTRL_CONTROL_START_BIT, true) |
      bitfield_field32_write(0u, RRAM_CTRL_CONTROL_OP_FIELD,
                             RRAM_CTRL_CONTROL_OP_VALUE_WRITE) |
      bitfield_bit32_write(0u, RRAM_CTRL_CONTROL_PARTITION_BIT, true) |
      bitfield_field32_write(0u, RRAM_CTRL_CONTROL_NUM_FIELD, 0u);

  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdRramCtrlRecovErr));
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_ADDR_REG_OFFSET, 0x260u);
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_CONTROL_REG_OFFSET, wr1_ctrl);
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_WR_FIFO_REG_OFFSET, 0u);
  CHECK(wait_op_done() == ((1u << RRAM_CTRL_OP_STATUS_DONE_BIT) |
                           (1u << RRAM_CTRL_OP_STATUS_ERR_BIT)));
  CHECK(abs_mmio_read32(kRramCoreBase + RRAM_CTRL_ERR_ADDR_REG_OFFSET) ==
        0x22cu);
  clear_rram_status();
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdRramCtrlRecovErr));

  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdRramCtrlRecovErr));
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_ADDR_REG_OFFSET, 0x290u);
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_CONTROL_REG_OFFSET, wr1_ctrl);
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_WR_FIFO_REG_OFFSET, 0u);
  CHECK(wait_op_done() == ((1u << RRAM_CTRL_OP_STATUS_DONE_BIT) |
                           (1u << RRAM_CTRL_OP_STATUS_ERR_BIT)));
  CHECK(abs_mmio_read32(kRramCoreBase + RRAM_CTRL_ERR_ADDR_REG_OFFSET) ==
        0x260u);
  clear_rram_status();
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdRramCtrlRecovErr));

  // ---------------------------------------------------------------------------
  // 5. [rram_ctrl_mp.sv:151, 216] & [rram_ctrl_arb.sv:232, 252]:
  //    Software-initiated `RramOpRewrite` (`CONTROL.OP = 2`) sets
  //    `if_sel = HwLoopBack`, which hardcodes `info_page_cfg = CfgRw` and
  //    `data_region_cfg = CfgRw`, completely bypassing `INFO_PAGE_CFG_1 = 0`
  //    (disabled page) and `SwInitDataCfg` (reserved OTP pages `>= 4091`,
  //    byte address `0x1ff600`) and completing with `OP_STATUS.DONE = 1` and
  //    `OP_STATUS.ERR = 0`!
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [rram_ctrl_mp.sv:151, 216]: RramOpRewrite HwLoopBack "
      "bypasses INFO_PAGE_CFG_1 and SwInitDataCfg OTP protection");
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_ADDR_REG_OFFSET, 0x200u);
  uint32_t rw_info_ctrl =
      bitfield_bit32_write(0u, RRAM_CTRL_CONTROL_START_BIT, true) |
      bitfield_field32_write(0u, RRAM_CTRL_CONTROL_OP_FIELD,
                             RRAM_CTRL_CONTROL_OP_VALUE_REWRITE) |
      bitfield_bit32_write(0u, RRAM_CTRL_CONTROL_PARTITION_BIT, true);
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_CONTROL_REG_OFFSET, rw_info_ctrl);
  CHECK(wait_op_done() == (1u << RRAM_CTRL_OP_STATUS_DONE_BIT));
  CHECK(abs_mmio_read32(kRramCoreBase + RRAM_CTRL_ERR_CODE_REG_OFFSET) == 0u);
  clear_rram_status();

  // Also verify on reserved OTP Data Partition page 4091 (`0x1ff600`):
  // First confirm normal Read (`OP = 0`) to `0x1ff600` is blocked by
  // `SwInitDataCfg` with `MP_ERR`:
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdRramCtrlRecovErr));
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_ADDR_REG_OFFSET, 0x1ff600u);
  uint32_t rd_otp_ctrl =
      bitfield_bit32_write(0u, RRAM_CTRL_CONTROL_START_BIT, true) |
      bitfield_field32_write(0u, RRAM_CTRL_CONTROL_OP_FIELD,
                             RRAM_CTRL_CONTROL_OP_VALUE_READ) |
      bitfield_bit32_write(0u, RRAM_CTRL_CONTROL_PARTITION_BIT, false) |
      bitfield_field32_write(0u, RRAM_CTRL_CONTROL_NUM_FIELD, 0u);
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_CONTROL_REG_OFFSET, rd_otp_ctrl);
  CHECK(wait_op_done() == ((1u << RRAM_CTRL_OP_STATUS_DONE_BIT) |
                           (1u << RRAM_CTRL_OP_STATUS_ERR_BIT)));
  CHECK(bitfield_bit32_read(
      abs_mmio_read32(kRramCoreBase + RRAM_CTRL_ERR_CODE_REG_OFFSET),
      RRAM_CTRL_ERR_CODE_MP_ERR_BIT));
  (void)abs_mmio_read32(kRramCoreBase + RRAM_CTRL_RD_FIFO_REG_OFFSET);
  clear_rram_status();
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdRramCtrlRecovErr));

  // Yet `RramOpRewrite` (`OP = 2`) to `0x1ff600` bypasses `SwInitDataCfg` and
  // succeeds with `ERR == 0`!
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_ADDR_REG_OFFSET, 0x1ff600u);
  uint32_t rw_otp_ctrl =
      bitfield_bit32_write(0u, RRAM_CTRL_CONTROL_START_BIT, true) |
      bitfield_field32_write(0u, RRAM_CTRL_CONTROL_OP_FIELD,
                             RRAM_CTRL_CONTROL_OP_VALUE_REWRITE) |
      bitfield_bit32_write(0u, RRAM_CTRL_CONTROL_PARTITION_BIT, false);
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_CONTROL_REG_OFFSET, rw_otp_ctrl);
  CHECK(wait_op_done() == (1u << RRAM_CTRL_OP_STATUS_DONE_BIT));
  CHECK(abs_mmio_read32(kRramCoreBase + RRAM_CTRL_ERR_CODE_REG_OFFSET) == 0u);
  clear_rram_status();

  // ---------------------------------------------------------------------------
  // 6. [rram_ctrl.sv:447-476, 509-585, 879-915] &
  // [tlul_adapter_sram.sv:349-366]:
  //    - CPU store to RRAM host window (`0x30080000`) and out-of-bounds read
  //      (`0x301ff600`) return `d_error = 1` (`mcause = 7 / 5`) with valid
  //      `error_blanking_integ = 0x55` (`g_intg_nmi_count == 0`), whereas an
  //      empty `RD_FIFO` read (`0x120`) deadlocks `u_to_rd_fifo` on `trunk-v2`.
  //    - `RD_FIFO` write (`mcause = 7`), `WR_FIFO` read (`mcause = 5`), and
  //      sub-word writes (`sb`) to `WR_FIFO` and `CONTROL` (`mcause = 7`).
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [rram_ctrl.sv:447-585, 879-915]: Host window access faults "
      "(error_blanking_integ=0x55) and WR_FIFO/RD_FIFO/CSR PERMIT faults");
  CHECK_STATUS_OK(
      ottf_alerts_ignore_alert(kTopEarlgreyAlertIdRvCoreIbexFatalHwErr));

  g_fault_count = 0;
  g_intg_nmi_count = 0;
  fault_write32(kRramHostBase + 0x80000u, 0xdeadbeefu,
                kIbexExcStoreAccessFault);
  CHECK(g_fault_count == 1u);
  CHECK(g_last_fault_mcause == kIbexExcStoreAccessFault);
  CHECK(g_intg_nmi_count == 0u);

  g_fault_count = 0;
  g_intg_nmi_count = 0;
  fault_read32(kRramHostBase + (4091u * kRramPageBytes),
               kIbexExcLoadAccessFault);
  CHECK(g_fault_count == 1u);
  CHECK(g_last_fault_mcause == kIbexExcLoadAccessFault);
  CHECK(g_intg_nmi_count == 0u);

  g_fault_count = 0;
  g_intg_nmi_count = 0;
  fault_write32(kRramCoreBase + RRAM_CTRL_RD_FIFO_REG_OFFSET, 0x12345678u,
                kIbexExcStoreAccessFault);
  CHECK(g_fault_count == 1u);
  CHECK(g_last_fault_mcause == kIbexExcStoreAccessFault);
  CHECK(g_intg_nmi_count == 0u);

  g_fault_count = 0;
  fault_read32(kRramCoreBase + RRAM_CTRL_WR_FIFO_REG_OFFSET,
               kIbexExcLoadAccessFault);
  CHECK(g_fault_count == 1u);
  CHECK(g_last_fault_mcause == kIbexExcLoadAccessFault);

  g_fault_count = 0;
  fault_write8(kRramCoreBase + RRAM_CTRL_WR_FIFO_REG_OFFSET, 0x11u,
               kIbexExcStoreAccessFault);
  CHECK(g_fault_count == 1u);
  CHECK(g_last_fault_mcause == kIbexExcStoreAccessFault);

  g_fault_count = 0;
  fault_write8(kRramCoreBase + RRAM_CTRL_CONTROL_REG_OFFSET, 0x01u,
               kIbexExcStoreAccessFault);
  CHECK(g_fault_count == 1u);
  CHECK(g_last_fault_mcause == kIbexExcStoreAccessFault);

  g_fault_count = 0;
  fault_write8(kRramCoreBase + RRAM_CTRL_ADDR_REG_OFFSET, 0x00u,
               kIbexExcStoreAccessFault);
  CHECK(g_fault_count == 1u);
  CHECK(g_last_fault_mcause == kIbexExcStoreAccessFault);

  g_fault_count = 0;
  fault_write8(kRramCoreBase + RRAM_CTRL_CTRL_REGWEN_REG_OFFSET + 1u, 0x00u,
               kIbexExcStoreAccessFault);
  CHECK(g_fault_count == 1u);
  CHECK(g_last_fault_mcause == kIbexExcStoreAccessFault);

  g_fault_count = 0;
  abs_mmio_write8(kRramCoreBase + RRAM_CTRL_CTRL_REGWEN_REG_OFFSET, 1u);
  CHECK(g_fault_count == 0u);

  // ---------------------------------------------------------------------------
  // 7. [rram_macro_prim_reg_top.sv] (`0x41018000`):
  //    `CSR2` (`0x08`) `rw` bits (`0x88`) omit `CSR0_REGWEN` (`0x00`) gating
  //    and remain writable after `CSR0_REGWEN` is locked to `0`.
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [rram_macro_prim_reg_top.sv]: CSR2 (0x08) bypasses "
      "CSR0_REGWEN (0x00)");
  abs_mmio_write32(kRramPrimBase + 0x04u, 0x123u);  // CSR1
  CHECK(abs_mmio_read32(kRramPrimBase + 0x04u) == 0x123u);
  abs_mmio_write32(kRramPrimBase + 0x00u, 0u);  // Lock CSR0_REGWEN = 0
  CHECK(abs_mmio_read32(kRramPrimBase + 0x00u) == 0u);
  abs_mmio_write32(kRramPrimBase + 0x04u, 0x456u);  // CSR1 write ignored
  CHECK(abs_mmio_read32(kRramPrimBase + 0x04u) == 0x123u);
  abs_mmio_write32(kRramPrimBase + 0x08u,
                   0x88u);  // CSR2 rw bits [7,3] bypass lock!
  CHECK((abs_mmio_read32(kRramPrimBase + 0x08u) & 0x88u) == 0x88u);
  abs_mmio_write32(kRramPrimBase + 0x08u, 0x00u);
  CHECK((abs_mmio_read32(kRramPrimBase + 0x08u) & 0x88u) == 0x00u);

  // ---------------------------------------------------------------------------
  // 8. [rram_ctrl.sv:485, 575]:
  //    Verify that `FIFO_CLR` (`0x114`, `wo` pulse) in `rram_ctrl` fixes v1
  //    `flash_ctrl`'s level-sensitive `FIFO_RST`: writing `1 << 1` to
  //    `FIFO_CLR` pulses a 1-cycle clear and does not hold `RD_FIFO` empty on
  //    subsequent reads.
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [rram_ctrl.sv:485, 575]: Self-clearing FIFO_CLR wo pulse "
      "allows subsequent OP_READ to fill RD_FIFO without clearing FIFO_CLR");
  const uint32_t kInfoCfgAllowRead =
      bitfield_field32_write(0u, RRAM_CTRL_INFO_PAGE_CFG_0_EN_0_FIELD,
                             kMultiBitBool4True) |
      bitfield_field32_write(0u, RRAM_CTRL_INFO_PAGE_CFG_0_RD_EN_0_FIELD,
                             kMultiBitBool4True) |
      bitfield_field32_write(0u, RRAM_CTRL_INFO_PAGE_CFG_0_WR_EN_0_FIELD,
                             kMultiBitBool4False) |
      bitfield_field32_write(0u, RRAM_CTRL_INFO_PAGE_CFG_0_SCRAMBLE_EN_0_FIELD,
                             kMultiBitBool4False) |
      bitfield_field32_write(0u, RRAM_CTRL_INFO_PAGE_CFG_0_ECC_EN_0_FIELD,
                             kMultiBitBool4False);
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_INFO_PAGE_CFG_1_REG_OFFSET,
                   kInfoCfgAllowRead);
  abs_mmio_write32(
      kRramCoreBase + RRAM_CTRL_FIFO_CLR_REG_OFFSET,
      (1u << RRAM_CTRL_FIFO_CLR_RD_BIT) | (1u << RRAM_CTRL_FIFO_CLR_WR_BIT));
  clear_rram_status();
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_ADDR_REG_OFFSET, 0x200u);
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_CONTROL_REG_OFFSET, rd2_ctrl);
  CHECK(wait_op_done() == (1u << RRAM_CTRL_OP_STATUS_DONE_BIT));
  CHECK((abs_mmio_read32(kRramCoreBase + RRAM_CTRL_CURR_FIFO_LVL_REG_OFFSET) >>
         8) == 2u);
  (void)abs_mmio_read32(kRramCoreBase + RRAM_CTRL_RD_FIFO_REG_OFFSET);
  (void)abs_mmio_read32(kRramCoreBase + RRAM_CTRL_RD_FIFO_REG_OFFSET);

  LOG_INFO("=== ALL RRAM_CTRL / FLASH_CTRL V2 ERRATA CHECKS PASSED ===");
  return true;
}
