// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * CW340 FPGA & QEMU Empirical Errata Confirmation Test for `top_earlgrey`
 * (P37).
 *
 * Exercises and confirms all documented silicon / spec / architectural
 * behaviors in `/root/knowledge/errata/top_earlgrey.md` and
 * `/root/knowledge/errata/reggen_wo_read_and_ro_write_no_tlul_error.md`:
 * - [top_earlgrey.sv:2083-2085] (`INTENDED_SECURITY_HARDENING` /
 * `SPEC_DOC_ERRATA`): Although `u_sram_ctrl_ret_aon` (`0x40500000`)
 * instantiates the identical `sram_ctrl_regs_reg_top.sv` CSR block as
 * `u_sram_ctrl_main` (`0x411c0000`) with writable `EXEC_REGWEN` (`0x08`) and
 * `EXEC` (`0x0c`) registers that accept and read back `kMultiBitBool4True`
 * (`0x6`), `top_earlgrey.sv:2084-2085` permanently ties
 * `.lc_hw_debug_en_i(lc_ctrl_pkg::Off)`,
 *   `.otp_en_sram_ifetch_i(prim_mubi_pkg::MuBi8False)`, and `.InstrExec(0)` on
 *   `u_sram_ctrl_ret_aon`. Consequently, instruction fetch from Main SRAM
 *   succeeds when `SRAM_CTRL_MAIN.EXEC == 0x6`, whereas instruction fetch from
 *   Retention SRAM (`0x40600000`) synchronously faults with Instruction Access
 *   Fault (`mcause = 1`) even when `SRAM_CTRL_RET_AON.EXEC == 0x6`!
 * - [top_earlgrey.sv:1499-1502] (`BENIGN_RTL_IMPL_DETAIL` / `E2`):
 *   `reggen`-generated `*_reg_top.sv` across Earlgrey peripherals returns `0`
 *   with `d_error = 0` (`mcause = 0`) on `SwAccessWO` reads (`CTRL`,
 *   `ALERT_TEST`) and silently ignores 32-bit full-word writes to `SwAccessRO`
 *   CSRs (`STATUS`) with `d_error = 0` (`mcause = 0`), whereas accesses to
 *   unmapped CSR offsets (`0x24`) synchronously trap with `mcause = 5 / 7`.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"
#include "sw/device/silicon_creator/lib/drivers/retention_sram.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "lc_ctrl_regs.h"
#include "rstmgr_regs.h"
#include "sram_ctrl_regs.h"

OTTF_DEFINE_TEST_CONFIG();

#define CHECK_EQ(a, b, ...) CHECK((a) == (b), __VA_ARGS__)

enum {
  kSramMainRegsBase = TOP_EARLGREY_SRAM_CTRL_MAIN_REGS_BASE_ADDR,
  kSramRetRegsBase = TOP_EARLGREY_SRAM_CTRL_RET_AON_REGS_BASE_ADDR,
  kSramRetRamBase = TOP_EARLGREY_SRAM_CTRL_RET_AON_RAM_BASE_ADDR,
  kRstmgrBase = TOP_EARLGREY_RSTMGR_AON_BASE_ADDR,
  kLcCtrlBase = TOP_EARLGREY_LC_CTRL_BASE_ADDR,
  kRiscvInstrAccessFault = 1,
  kRiscvLoadAccessFault = 5,
  kRiscvStoreAccessFault = 7,
  kRiscvRetInsn32 = 0x00008067u,  // jalr x0, 0(x1) (`ret`)
};

static volatile uint32_t g_fault_count = 0;
static volatile uint32_t g_last_mcause = 0;
static volatile uint32_t g_last_mepc = 0;
static volatile uint32_t g_saved_return_pc = 0;

void ottf_exception_handler(uint32_t *exc_info) {
  uint32_t mcause = 0;
  uint32_t mepc = 0;
  CSR_READ(CSR_REG_MCAUSE, &mcause);
  CSR_READ(CSR_REG_MEPC, &mepc);
  g_fault_count++;
  g_last_mcause = mcause;
  g_last_mepc = mepc;

  if (mcause == kRiscvInstrAccessFault) {
    // Instruction Access Fault on jump to non-executable Retention SRAM:
    // ottf_isrs.S restores mepc from exc_info[0] (0(sp)) before mret.
    exc_info[0] = g_saved_return_pc;
    return;
  }
  if (mcause == kRiscvLoadAccessFault || mcause == kRiscvStoreAccessFault) {
    // compute_mepc_on_synchronous_irq in ottf_isrs.S already advanced
    // exc_info[0] past the faulting load/store instruction.
    return;
  }
  ottf_generic_fault_print(exc_info, "Unhandled exception", mcause);
  abort();
}

// Execute `jalr ra, target_fn` while recording the exact post-call return PC
// in `g_saved_return_pc` so `ottf_exception_handler` can recover cleanly from
// `mcause = 1` (`kRiscvInstrAccessFault`).
static void call_fn_catching_instr_fault(uintptr_t target_fn) {
  asm volatile(
      "la   t0, 1f\n"
      "sw   t0, %[ret_pc]\n"
      "mv   t1, %[fn]\n"
      "jalr ra, t1, 0\n"
      "1:\n"
      : [ret_pc] "=m"(g_saved_return_pc)
      : [fn] "r"(target_fn)
      : "t0", "t1", "ra", "memory");
}

static uint32_t s_main_sram_code_buf[4] __attribute__((aligned(16)));

static void test_top_earlgrey_asymmetric_sram_ifetch_tieoff(void) {
  LOG_INFO(
      "Verifying [top_earlgrey.sv:2083-2085]: Asymmetric sram_ctrl_ret_aon vs "
      "sram_ctrl_main ifetch tie-off despite writable EXEC=0x6 CSRs");

  // Unlock the address space for RWX in ePMP (PMP entry 7: NAPOT | L | R | W |
  // X = 0x9f) so Ibex ePMP permits instruction fetch from both Main SRAM and
  // Retention SRAM, isolating the hardware sram_ctrl InstrExec / en_ifetch
  // gate.
  CSR_WRITE(CSR_REG_PMPADDR7, 0x7fffffffu);
  CSR_SET_BITS(CSR_REG_PMPCFG1, 0x9fu << 24);
  icache_invalidate();

  // 1. Both sram_ctrl_main and sram_ctrl_ret_aon expose writable EXEC_REGWEN
  // (0x08) and EXEC (0x0c) CSRs that accept kMultiBitBool4True (0x6).
  uint32_t orig_main_exec =
      abs_mmio_read32(kSramMainRegsBase + SRAM_CTRL_EXEC_REG_OFFSET);
  uint32_t orig_ret_exec =
      abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_EXEC_REG_OFFSET);

  abs_mmio_write32(kSramMainRegsBase + SRAM_CTRL_EXEC_REG_OFFSET,
                   kMultiBitBool4True);
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_EXEC_REG_OFFSET,
                   kMultiBitBool4True);

  CHECK_EQ(abs_mmio_read32(kSramMainRegsBase + SRAM_CTRL_EXEC_REG_OFFSET),
           (uint32_t)kMultiBitBool4True,
           "[top_earlgrey.sv:2083-2085] Expected SRAM_CTRL_MAIN.EXEC == 0x6");
  CHECK_EQ(abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_EXEC_REG_OFFSET),
           (uint32_t)kMultiBitBool4True,
           "[top_earlgrey.sv:2083-2085] Expected SRAM_CTRL_RET_AON.EXEC == 0x6 "
           "(writable CSR despite InstrExec=0)");

  // 2. Write `ret` (0x00008067) into Main SRAM and verify instruction fetch
  // succeeds (0 faults) when SRAM_CTRL_MAIN.EXEC == 0x6.
  s_main_sram_code_buf[0] = kRiscvRetInsn32;
  s_main_sram_code_buf[1] = kRiscvRetInsn32;
  icache_invalidate();

  g_fault_count = 0;
  g_last_mcause = 0;
  call_fn_catching_instr_fault((uintptr_t)&s_main_sram_code_buf[0]);
  CHECK_EQ(
      g_fault_count, 0u,
      "[top_earlgrey.sv:2083-2085] Expected Main SRAM instruction fetch to "
      "succeed when SRAM_CTRL_MAIN.EXEC == 0x6");

  // 3. Write the exact same `ret` (0x00008067) into Retention SRAM and verify
  // instruction fetch synchronously faults with Instruction Access Fault
  // (mcause = 1) EVEN THOUGH SRAM_CTRL_RET_AON.EXEC == 0x6, because
  // top_earlgrey.sv:2084-2085 permanently ties off lc_hw_debug_en_i=Off,
  // otp_en_sram_ifetch_i=MuBi8False, and InstrExec=0!
  uintptr_t ret_sram_code_addr =
      kSramRetRamBase + offsetof(retention_sram_t, owner);
  abs_mmio_write32(ret_sram_code_addr, kRiscvRetInsn32);
  abs_mmio_write32(ret_sram_code_addr + 4u, kRiscvRetInsn32);
  icache_invalidate();

  g_fault_count = 0;
  g_last_mcause = 0;
  g_last_mepc = 0;
  call_fn_catching_instr_fault(ret_sram_code_addr);
  CHECK_EQ(g_fault_count, 1u,
           "[top_earlgrey.sv:2083-2085] Expected Retention SRAM instruction "
           "fetch to fault even with SRAM_CTRL_RET_AON.EXEC == 0x6");
  CHECK_EQ(
      g_last_mcause, (uint32_t)kRiscvInstrAccessFault,
      "[top_earlgrey.sv:2083-2085] Expected mcause == 1 (Instruction Access "
      "Fault), got %u",
      g_last_mcause);
  CHECK_EQ(g_last_mepc, (uint32_t)ret_sram_code_addr,
           "[top_earlgrey.sv:2083-2085] Expected mepc == 0x%08x, got 0x%08x",
           (uint32_t)ret_sram_code_addr, g_last_mepc);

  // Restore original EXEC CSR values.
  abs_mmio_write32(kSramMainRegsBase + SRAM_CTRL_EXEC_REG_OFFSET,
                   orig_main_exec);
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_EXEC_REG_OFFSET, orig_ret_exec);
}

static void test_top_earlgrey_reggen_wo_read_and_ro_write(void) {
  LOG_INFO(
      "Verifying [top_earlgrey.sv:1499-1502]: reggen SwAccessWO reads return 0 "
      "& "
      "SwAccessRO full-word writes ignored with d_error=0 vs unmapped "
      "addrmiss");

  // 1. Reading SwAccessWO registers (SRAM_CTRL_MAIN.CTRL,
  // SRAM_CTRL_RET_AON.CTRL, RSTMGR_AON.ALERT_TEST) returns 0x00000000 without
  // faulting (d_error = 0).
  g_fault_count = 0;
  uint32_t main_ctrl =
      abs_mmio_read32(kSramMainRegsBase + SRAM_CTRL_CTRL_REG_OFFSET);
  uint32_t ret_ctrl =
      abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_CTRL_REG_OFFSET);
  uint32_t rstmgr_alert_test =
      abs_mmio_read32(kRstmgrBase + RSTMGR_ALERT_TEST_REG_OFFSET);
  CHECK_EQ(
      g_fault_count, 0u,
      "[top_earlgrey.sv:1499-1502] Reading SwAccessWO CSRs must not fault");
  CHECK_EQ(main_ctrl, 0u, "Expected SRAM_CTRL_MAIN.CTRL (WO) read == 0");
  CHECK_EQ(ret_ctrl, 0u, "Expected SRAM_CTRL_RET_AON.CTRL (WO) read == 0");
  CHECK_EQ(rstmgr_alert_test, 0u,
           "Expected RSTMGR_AON.ALERT_TEST (WO) read == 0");

  // 2. Full-word writes (reg_be == 4'b1111) to SwAccessRO registers
  // (SRAM_CTRL_MAIN.STATUS, SRAM_CTRL_RET_AON.STATUS, LC_CTRL.STATUS) are
  // silently ignored with d_error = 0 (mcause = 0) and preserve register state.
  uint32_t main_status_before =
      abs_mmio_read32(kSramMainRegsBase + SRAM_CTRL_STATUS_REG_OFFSET);
  uint32_t ret_status_before =
      abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_STATUS_REG_OFFSET);
  uint32_t lc_status_before =
      abs_mmio_read32(kLcCtrlBase + LC_CTRL_STATUS_REG_OFFSET);

  g_fault_count = 0;
  abs_mmio_write32(kSramMainRegsBase + SRAM_CTRL_STATUS_REG_OFFSET,
                   0xFFFFFFFFu);
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_STATUS_REG_OFFSET, 0xFFFFFFFFu);
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_STATUS_REG_OFFSET, 0xFFFFFFFFu);
  CHECK_EQ(
      g_fault_count, 0u,
      "[top_earlgrey.sv:1499-1502] Full-word writes to SwAccessRO CSRs must "
      "not fault");
  CHECK_EQ(abs_mmio_read32(kSramMainRegsBase + SRAM_CTRL_STATUS_REG_OFFSET),
           main_status_before,
           "Expected SRAM_CTRL_MAIN.STATUS (RO) to remain unchanged");
  CHECK_EQ(abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_STATUS_REG_OFFSET),
           ret_status_before,
           "Expected SRAM_CTRL_RET_AON.STATUS (RO) to remain unchanged");
  CHECK_EQ(abs_mmio_read32(kLcCtrlBase + LC_CTRL_STATUS_REG_OFFSET),
           lc_status_before,
           "Expected LC_CTRL.STATUS (RO) to remain unchanged");

  // 3. Conversely, reading or writing an unmapped CSR offset (0x24 in
  // sram_ctrl_main / sram_ctrl_ret_aon) triggers addrmiss = 1 -> mcause = 5
  // / 7.
  g_fault_count = 0;
  g_last_mcause = 0;
  (void)abs_mmio_read32(kSramMainRegsBase + 0x24u);
  CHECK_EQ(g_fault_count, 1u, "Expected read at unmapped 0x24 to trap");
  CHECK_EQ(g_last_mcause, (uint32_t)kRiscvLoadAccessFault, "Expected mcause=5");

  g_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write32(kSramRetRegsBase + 0x24u, 0x12345678u);
  CHECK_EQ(g_fault_count, 1u, "Expected write at unmapped 0x24 to trap");
  CHECK_EQ(g_last_mcause, (uint32_t)kRiscvStoreAccessFault,
           "Expected mcause=7");
}

bool test_main(void) {
  LOG_INFO(
      "Starting top_earlgrey CW340 FPGA & QEMU errata confirmation test (P37)");

  test_top_earlgrey_asymmetric_sram_ifetch_tieoff();
  test_top_earlgrey_reggen_wo_read_and_ro_write();

  LOG_INFO("All top_earlgrey errata checks PASSED");
  return true;
}
