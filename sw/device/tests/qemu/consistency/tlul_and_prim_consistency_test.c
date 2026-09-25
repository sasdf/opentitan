// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file tlul_errata_v2_test.c
 * @brief Physical CW340 FPGA verification of Earlgrey v2 (`trunk-v2` /
 *        `mainline-v2`) hardware, specification, and adapter behaviors for
 *        `tlul` (`hw/ip/tlul/rtl/`):
 *
 * 1. `reggen` `*_reg_top.sv` (`tlul_adapter_reg`) vs. `tlul_adapter_sram`:
 *    - Reading a `reggen` `wo` CSR (`SRAM_CTRL_RET_AON.CTRL` `0x40500014`)
 *      returns `0x00000000` with `d_error = 0` (`mcause = 0`), and writing a
 *      32-bit word to a `reggen` `ro` CSR (`SRAM_CTRL_RET_AON.STATUS`
 *      `0x40500004`) is silently ignored with `d_error = 0`.
 *    - By contrast, reading a `tlul_adapter_sram` `ErrOnRead = 1` window
 *      (`RRAM_CTRL.WR_FIFO` `0x4101011c`) synchronously faults with
 *      `LoadAccessFault` (`mcause = 5`), and writing a `tlul_adapter_sram`
 *      `ErrOnWrite = 1` window (`RRAM_CTRL.RD_FIFO` `0x41010120` or
 *      `ROM_CTRL.ROM` `0x00008000`) faults with `StoreAccessFault` (`mcause =
 *      7`) and `0` Integrity NMIs (`tlul_adapter_sram.sv:349-366`
 *      `u_tlul_data_integ_enc_data` supplies `error_blanking_integ = 0x55` on
 *      `reqfifo_rdata.error == 1`).
 *
 * 2. `tlul_adapter_reg` & `*_PERMIT` sub-word byte-enable enforcement
 *    (`wr_err = reg_we & |(PERMIT[i] & ~reg_be)`):
 *    - On `SRAM_CTRL_RET_AON.READBACK` (`0x40500020`, `PERMIT = 4'b0001`), an
 *      8-bit store (`sb`) to byte lane 0 (`+0`, `reg_be = 4'b0001`) succeeds
 *      with `d_error = 0`, whereas an 8-bit store (`sb`) to byte lane 1 (`+1`,
 *      `reg_be = 4'b0010`) faults synchronously with `StoreAccessFault`
 *      (`mcause = 7`).
 *    - On `RRAM_CTRL.ADDR` (`0x41010024`, `PERMIT = 4'b0111`), both 8-bit
 *      (`sb`) and 16-bit (`sh`) stores to byte lane 0 (`+0`) fault
 *      synchronously with `StoreAccessFault` (`mcause = 7`) and leave `ADDR`
 *      unmodified.
 *
 * 3. `tlul_lc_gate.sv` + `tlul_err_resp.sv` (`ReturnBlankResp = 0`):
 *    - Accessing `OTP_CTRL.PRIM` (`0x40138000`) when `lc_dft_en != On` routes
 *      requests through `tlul_lc_gate` to `u_tlul_err_resp`, returning
 *      `d_error = 1` (`mcause = 5` on `lw`, `mcause = 7` on `sw`) with valid
 *      `data_intg = 0x55` (`0` Integrity NMIs).
 *
 * 4. `tlul_sram_byte.sv` (`SEC_CM: MEM.READBACK`):
 *    - `mubi4_test_true_loose(rdback_en_q)` (`!= 4'h9`) activates the
 *      multi-cycle readback FSM (`StWrReadBackInit`, `StRdReadBack`,
 *      `StByteWrReadBackInit`).
 *    - Although `tlul_sram_byte.sv:176, 223-232` added `u_rdback_data_exp_intg`
 *      (`rdback_data_exp_intg_q`) and captures expected `data_intg` in
 *      `StWriteCmd`/`StWrReadBackInit`/`StRdReadBack`/`StRdReadBackDWait`,
 *      `rdback_chk_ok_unbuf` (`tlul_sram_byte.sv:181`:
 *      `rdback_data_exp_q == tl_sram_i.d_data`) never compares
 *      `rdback_data_exp_intg_q` against `tl_sram_i.d_user.data_intg`.
 *    - Consequently, even with `READBACK = kMultiBitBool4True` (`0x6`) active
 *      on `SRAM_CTRL_RET_AON`:
 *      (a) Reading a stale-key SRAM word whose 7-bit `data_intg` ECC is
 *          corrupted triggers 1 Ibex Load Integrity NMI (`mcause = 0xffffffe0`)
 *          on the primary read (`StRdReadBack`), while the internal readback
 *          check (`StPassThru`) reports `rdback_chk_ok = 1`
 * (`STATUS.READBACK_ERROR == 0`). (b) Performing a 1-byte sub-word write (`sb`
 * of `0x5au`) to that corrupted word while `READBACK = kMultiBitBool4True` is
 * active
 *          (`StWaitRd -> StWriteCmd -> StByteWrReadBackInit -> StWrReadBack`)
 *          discards the invalid `d_user.data_intg` during `StWaitRd`, computes
 *          a fresh valid `SecdedInv3932` ECC over the 3 corrupted bytes
 * `[31:8]`
 *          + `0x5au`, and passes its own readback comparison with
 *          `STATUS.READBACK_ERROR == 0` and `0` NMIs!
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
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
#include "hw/top/sram_ctrl_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kSramRetRegsBase = 0x40500000u,
  kSramRetRamBase = 0x40600000u,
  kRramCoreBase = TOP_EARLGREY_RRAM_CTRL_CORE_BASE_ADDR,
  kRomBase = TOP_EARLGREY_ROM_CTRL_ROM_BASE_ADDR,
  kOtpPrimBase = 0x40138000u,
  kIbexBase = TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR,
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

static uint32_t safe_read32(uint32_t addr, uint32_t expected_mcause) {
  uint32_t val = 0x11223344u;
  g_expected_fault_mcause = expected_mcause;
  asm volatile(
      "la   t0, 1f\n"
      "sw   t0, %[target]\n"
      "la   t0, 2f\n"
      "sw   t0, %[resume]\n"
      "1:\n"
      "lw   %[val], 0(%[addr])\n"
      "2:\n"
      : [val] "+r"(val), [target] "=m"(g_fault_target_pc),
        [resume] "=m"(g_fault_resume_pc)
      : [addr] "r"(addr)
      : "t0", "memory");
  g_fault_target_pc = 0u;
  g_fault_resume_pc = 0u;
  return val;
}

static void safe_write32(uint32_t addr, uint32_t val,
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

static void safe_write16(uint32_t addr, uint16_t val,
                         uint32_t expected_mcause) {
  g_expected_fault_mcause = expected_mcause;
  asm volatile(
      "la   t0, 1f\n"
      "sw   t0, %[target]\n"
      "la   t0, 2f\n"
      "sw   t0, %[resume]\n"
      "1:\n"
      "sh   %[val], 0(%[addr])\n"
      "2:\n"
      : [target] "=m"(g_fault_target_pc), [resume] "=m"(g_fault_resume_pc)
      : [addr] "r"(addr), [val] "r"((uint32_t)val)
      : "t0", "memory");
  g_fault_target_pc = 0u;
  g_fault_resume_pc = 0u;
}

static void safe_write8(uint32_t addr, uint8_t val, uint32_t expected_mcause) {
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

bool test_main(void) {
  LOG_INFO("=== Starting TLUL Earlgrey v2 Errata Test ===");
  CHECK_STATUS_OK(
      ottf_alerts_ignore_alert(kTopEarlgreyAlertIdRvCoreIbexFatalHwErr));

  // ---------------------------------------------------------------------------
  // 1. `reggen` `*_reg_top.sv` (`tlul_adapter_reg`) vs. `tlul_adapter_sram`
  //    access direction and `error_blanking_integ` (`tlul_adapter_sram.sv:366`)
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [tlul_adapter_reg.sv] vs [tlul_adapter_sram.sv:349-366]: "
      "reggen WO/RO no-fault vs tlul_adapter_sram ErrOnRead/ErrOnWrite faults "
      "with valid error_blanking_integ (0x55)");

  // Reading a reggen WO register returns 0 with 0 faults.
  g_fault_count = 0;
  uint32_t wo_val =
      safe_read32(kSramRetRegsBase + SRAM_CTRL_CTRL_REG_OFFSET, 0u);
  CHECK(g_fault_count == 0u);
  CHECK(wo_val == 0u);

  // Writing a full 32-bit word to a reggen RO register is ignored with 0
  // faults.
  uint32_t status_before =
      abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_STATUS_REG_OFFSET);
  g_fault_count = 0;
  safe_write32(kSramRetRegsBase + SRAM_CTRL_STATUS_REG_OFFSET, 0xffffffffu, 0u);
  CHECK(g_fault_count == 0u);
  CHECK(abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_STATUS_REG_OFFSET) ==
        status_before);

  // Reading a tlul_adapter_sram ErrOnRead=1 window (RRAM_CTRL.WR_FIFO) faults
  // with LoadAccessFault (mcause = 5) and 0 Integrity NMIs.
  g_fault_count = 0;
  g_intg_nmi_count = 0;
  (void)safe_read32(kRramCoreBase + RRAM_CTRL_WR_FIFO_REG_OFFSET,
                    kIbexExcLoadAccessFault);
  CHECK(g_fault_count == 1u);
  CHECK(g_last_fault_mcause == kIbexExcLoadAccessFault);
  CHECK(g_intg_nmi_count == 0u);

  // Writing a tlul_adapter_sram ErrOnWrite=1 window (ROM_CTRL.ROM 0x00008000
  // and RRAM_CTRL.RD_FIFO 0x41010120) faults with StoreAccessFault (mcause = 7)
  // and 0 Integrity NMIs (`error_blanking_integ = 0x55`).
  g_fault_count = 0;
  g_intg_nmi_count = 0;
  safe_write32(kRomBase + 0x100u, 0xdeadbeefu, kIbexExcStoreAccessFault);
  CHECK(g_fault_count == 1u);
  CHECK(g_last_fault_mcause == kIbexExcStoreAccessFault);
  CHECK(g_intg_nmi_count == 0u);

  g_fault_count = 0;
  g_intg_nmi_count = 0;
  safe_write32(kRramCoreBase + RRAM_CTRL_RD_FIFO_REG_OFFSET, 0x12345678u,
               kIbexExcStoreAccessFault);
  CHECK(g_fault_count == 1u);
  CHECK(g_last_fault_mcause == kIbexExcStoreAccessFault);
  CHECK(g_intg_nmi_count == 0u);

  // ---------------------------------------------------------------------------
  // 2. `tlul_adapter_reg` & `*_PERMIT` sub-word byte-enable mask enforcement
  //    (`wr_err = reg_we & |(PERMIT[i] & ~reg_be)`)
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [tlul_adapter_reg.sv] & [*_PERMIT]: Sub-word write permit "
      "mask on 1-byte PERMIT (4'b0001) vs multi-byte PERMIT (4'b0111)");

  // SRAM_CTRL_RET_AON.READBACK (0x20) has PERMIT = 4'b0001:
  // sb to +0 (reg_be = 4'b0001) succeeds; sb to +1 (reg_be = 4'b0010) faults!
  g_fault_count = 0;
  safe_write8(kSramRetRegsBase + SRAM_CTRL_READBACK_REG_OFFSET,
              kMultiBitBool4True, 0u);
  CHECK(g_fault_count == 0u);
  CHECK(abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_READBACK_REG_OFFSET) ==
        kMultiBitBool4True);

  g_fault_count = 0;
  safe_write8(kSramRetRegsBase + SRAM_CTRL_READBACK_REG_OFFSET + 1u,
              kMultiBitBool4False, kIbexExcStoreAccessFault);
  CHECK(g_fault_count == 1u);
  CHECK(g_last_fault_mcause == kIbexExcStoreAccessFault);
  CHECK(abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_READBACK_REG_OFFSET) ==
        kMultiBitBool4True);

  // RRAM_CTRL.ADDR (0x24) has PERMIT = 4'b0111:
  // Even sb/sh to byte lane 0 (+0) faults with StoreAccessFault (mcause = 7)!
  abs_mmio_write32(kRramCoreBase + RRAM_CTRL_ADDR_REG_OFFSET, 0x123400u);
  g_fault_count = 0;
  safe_write8(kRramCoreBase + RRAM_CTRL_ADDR_REG_OFFSET, 0x56u,
              kIbexExcStoreAccessFault);
  CHECK(g_fault_count == 1u);
  CHECK(g_last_fault_mcause == kIbexExcStoreAccessFault);
  CHECK(abs_mmio_read32(kRramCoreBase + RRAM_CTRL_ADDR_REG_OFFSET) ==
        0x123400u);

  g_fault_count = 0;
  safe_write16(kRramCoreBase + RRAM_CTRL_ADDR_REG_OFFSET, 0x5678u,
               kIbexExcStoreAccessFault);
  CHECK(g_fault_count == 1u);
  CHECK(g_last_fault_mcause == kIbexExcStoreAccessFault);
  CHECK(abs_mmio_read32(kRramCoreBase + RRAM_CTRL_ADDR_REG_OFFSET) ==
        0x123400u);

  // ---------------------------------------------------------------------------
  // 3. `tlul_lc_gate.sv` & `tlul_err_resp.sv` (`ReturnBlankResp = 0`)
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [tlul_lc_gate.sv] & [tlul_err_resp.sv]: OTP_CTRL.PRIM "
      "(0x40138000) returns d_error = 1 with valid data_intg = 0x55");
  g_fault_count = 0;
  g_intg_nmi_count = 0;
  (void)safe_read32(kOtpPrimBase, kIbexExcLoadAccessFault);
  CHECK(g_fault_count == 1u);
  CHECK(g_last_fault_mcause == kIbexExcLoadAccessFault);
  CHECK(g_intg_nmi_count == 0u);

  g_fault_count = 0;
  g_intg_nmi_count = 0;
  safe_write32(kOtpPrimBase, 0x1u, kIbexExcStoreAccessFault);
  CHECK(g_fault_count == 1u);
  CHECK(g_last_fault_mcause == kIbexExcStoreAccessFault);
  CHECK(g_intg_nmi_count == 0u);

  // ---------------------------------------------------------------------------
  // 4. `tlul_sram_byte.sv:176, 181, 223-232, 667-694` (`SEC_CM: MEM.READBACK`):
  //    With `READBACK = kMultiBitBool4True` (`0x6`) enabled on
  //    `sram_ctrl_ret_aon`:
  //    (a) Reading a stale-key SRAM word with corrupted `data_intg` ECC
  //        triggers 1 Ibex Load Integrity NMI on the primary read, while
  //        `tlul_sram_byte`'s internal readback comparison
  //        (`rdback_chk_ok_unbuf = (rdback_data_exp_q == tl_sram_i.d_data)`)
  //        ignores `rdback_data_exp_intg_q` and reports `STATUS.READBACK_ERROR
  //        == 0`.
  //    (b) Executing a 1-byte sub-word write (`sb` of `0x5au`) to that
  //        corrupted word while `READBACK = kMultiBitBool4True` is active
  //        discards the invalid `d_user.data_intg` during `StWaitRd`, splices
  //        `0x5au` with the 3 corrupted bytes `[31:8]`, computes a fresh valid
  //        `SecdedInv3932` ECC (`u_tlul_data_integ_enc`), and passes its own
  //        `StByteWrReadBackInit -> StWrReadBack` check with
  //        `STATUS.READBACK_ERROR == 0` and `0` NMIs!
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [tlul_sram_byte.sv:181, 223-232, 667-694]: Dead "
      "u_rdback_data_exp_intg and RMW ECC laundering under active READBACK = "
      "kMultiBitBool4True");

  // Initialize retention SRAM and write 16 seed words at offset 0x200.
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_READBACK_REG_OFFSET,
                   kMultiBitBool4True);
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_CTRL_REG_OFFSET,
                   (1u << SRAM_CTRL_CTRL_RENEW_SCR_KEY_BIT) |
                       (1u << SRAM_CTRL_CTRL_INIT_BIT));
  while ((abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_STATUS_REG_OFFSET) &
          ((1u << SRAM_CTRL_STATUS_INIT_DONE_BIT) |
           (1u << SRAM_CTRL_STATUS_SCR_KEY_VALID_BIT))) !=
         ((1u << SRAM_CTRL_STATUS_INIT_DONE_BIT) |
          (1u << SRAM_CTRL_STATUS_SCR_KEY_VALID_BIT))) {
  }
  for (uint32_t i = 0; i < 16u; ++i) {
    abs_mmio_write32(kSramRetRamBase + 0x200u + (i * 4u),
                     0xA5A50000u ^ (i * 0x01010101u));
  }

  // Rotate scrambling key WITHOUT INIT so all 16 words at 0x200..0x23c have
  // invalid 39-bit SecdedInv3932 ECC syndromes.
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_CTRL_REG_OFFSET,
                   1u << SRAM_CTRL_CTRL_RENEW_SCR_KEY_BIT);
  while ((abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_STATUS_REG_OFFSET) &
          (1u << SRAM_CTRL_STATUS_SCR_KEY_VALID_BIT)) == 0u) {
  }

  // Locate a word in 0x200..0x23c whose stale-key bits trigger an Ibex Load
  // Integrity NMI while READBACK = kMultiBitBool4True is active.
  uint32_t corrupt_addr = 0u;
  uint32_t orig_word = 0u;
  uint32_t fault_rd_val = 0u;
  for (uint32_t i = 0; i < 16u; ++i) {
    uint32_t candidate = kSramRetRamBase + 0x200u + (i * 4u);
    g_intg_nmi_count = 0;
    uint32_t rd = safe_read32(candidate, 0u);
    if (g_intg_nmi_count == 1u) {
      corrupt_addr = candidate;
      orig_word = 0xa5a50000u | i;
      fault_rd_val = rd;
      break;
    }
  }
  CHECK(corrupt_addr != 0u);
  CHECK(fault_rd_val == 0x11223344u);

  // Even though `corrupt_addr` just failed ECC on read and triggered an Ibex
  // Load Integrity NMI while `READBACK = kMultiBitBool4True` was enabled,
  // `tlul_sram_byte.sv:181` ignored `rdback_data_exp_intg_q`, so
  // `STATUS.READBACK_ERROR` (bit 5) and `STATUS.BUS_INTEG_ERROR` (bit 0) are 0!
  uint32_t sram_status =
      abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_STATUS_REG_OFFSET);
  CHECK((sram_status & (1u << SRAM_CTRL_STATUS_READBACK_ERROR_BIT)) == 0u);
  CHECK((sram_status & (1u << SRAM_CTRL_STATUS_BUS_INTEG_ERROR_BIT)) == 0u);

  // Now perform a 1-byte sub-word write (`sb` of `0x5au`) to `corrupt_addr`
  // while `READBACK = kMultiBitBool4True` is STILL active!
  g_fault_count = 0;
  g_intg_nmi_count = 0;
  safe_write8(corrupt_addr, 0x5au, 0u);
  CHECK(g_fault_count == 0u);
  CHECK(g_intg_nmi_count == 0u);

  // Read back the full 32-bit word at `corrupt_addr` with `READBACK =
  // kMultiBitBool4True`: zero Load Integrity NMIs, upper 3 stale-key garbage
  // bytes [31:8] laundered with a fresh valid ECC (`!= orig_word` and `!= 0`),
  // low byte == 0x5a, and `STATUS.READBACK_ERROR == 0`!
  uint32_t laundered_word = safe_read32(corrupt_addr, 0u);
  CHECK(g_fault_count == 0u);
  CHECK(g_intg_nmi_count == 0u);
  CHECK((laundered_word & 0xffu) == 0x5au);
  CHECK((laundered_word & 0xffffff00u) != (orig_word & 0xffffff00u));
  CHECK((laundered_word & 0xffffff00u) != 0u);
  sram_status = abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_STATUS_REG_OFFSET);
  CHECK((sram_status & (1u << SRAM_CTRL_STATUS_READBACK_ERROR_BIT)) == 0u);
  CHECK((sram_status & (1u << SRAM_CTRL_STATUS_BUS_INTEG_ERROR_BIT)) == 0u);

  // Also verify loose mubi4_test_true_loose(rdback_en_q) (rdback_en_q != 4'h9)
  // with non-strict READBACK = 0x0 (tlul_sram_byte.sv:302):
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_READBACK_REG_OFFSET, 0x0u);
  CHECK((abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_READBACK_REG_OFFSET) &
         0xfu) == 0x0u);
  safe_write8(corrupt_addr, 0xa5u, 0u);
  uint32_t loose_rdback_word = safe_read32(corrupt_addr, 0u);
  CHECK(g_intg_nmi_count == 0u);
  CHECK((loose_rdback_word & 0xffu) == 0xa5u);
  CHECK((loose_rdback_word & 0xffffff00u) == (laundered_word & 0xffffff00u));
  sram_status = abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_STATUS_REG_OFFSET);
  CHECK((sram_status & (1u << SRAM_CTRL_STATUS_READBACK_ERROR_BIT)) == 0u);

  // Restore READBACK = kMultiBitBool4False.
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_READBACK_REG_OFFSET,
                   kMultiBitBool4False);

  LOG_INFO("=== ALL TLUL V2 ERRATA CHECKS PASSED ===");
  return true;
}
