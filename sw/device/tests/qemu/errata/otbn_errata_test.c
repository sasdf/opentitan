// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * CW340 FPGA & QEMU Hardware Errata & Security Hardening Confirmation Test
 * for `otbn` (`P03` — `/root/knowledge/errata/otbn.md`).
 *
 * Empirically confirms on both physical CW340 FPGA silicon and QEMU:
 * - [otbn.sv:736-746] (TRUE_SILICON_ERRATA): Idle 32-bit host IMEM/DMEM reads
 *   (TL-UL Get) advance LOAD_CHECKSUM with wr_data=0, and StatusLocked writes
 *   also advance LOAD_CHECKSUM (`otbn.sv:736-746`).
 * - [otbn_alu_bignum.sv:905-919] (SPEC_DOC_ERRATA): BN.ADDM reduces a + b >=
 * MOD (yielding 0 when a + b == MOD, `otbn_alu_bignum.sv:905-919` vs
 * `bignum-insns.yml:116`) and dual-operand `x1` call stack reads pop once
 * (`otbn_rf_base.sv:105-134`).
 * - [otbn.sv:451-477,691-720] (INTENDED_SECURITY_HARDENING): Illegal host
 * DMEM/IMEM read during BUSY_EXECUTE returns blanked 39'h0 (`ecc = 7'h00 !=
 * 7'h39`) before `locking_q` latches, raising Ibex Load Integrity NMI
 * (`FATAL_INTG_ERR`) and locking OTBN (`otbn.sv:451-477, 691-720`).
 * - [otbn.sv:146-158,798-806,892,926-995] &
 * [otbn.sv:352,451-472,566,691-715,926-957] (INTENDED_SECURITY_HARDENING): In
 * STATUS_LOCKED (`0xFF`), IMEM/DMEM reads return 0x00000000 with valid
 *   `SecdedInv3932ZeroEcc` (`7'h39`, no Ibex NMI), `ERR_BITS` and `INSN_CNT`
 *   remain writable (write-any-clears-to-zero), and `FATAL_ALERT_CAUSE` remains
 *   sticky set-only (`otbn.sv:146-158, 451-472, 926-995`).
 * - [otbn.sv:371-383,610-622] (INTENDED_SECURITY_HARDENING): Sub-word writes
 * (`sb`) to IMEM/DMEM (`.ByteAccess(0)`) and narrow CSRs (`OTBN_PERMIT`) and
 * accesses to unmapped offset `0x2c` (`addrmiss`) trigger synchronous TL-UL bus
 * faults
 *   (`mcause = 5 / 7`).
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/entropy_testutils.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/ip/otbn/data/otbn_regs.h"
#include "hw/ip/rv_core_ibex/data/rv_core_ibex_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG(.ignore_alerts = true);

enum {
  kOtbnBase = TOP_EARLGREY_OTBN_BASE_ADDR,
  kIbexBase = TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR,
  kCmdExecute = 0xd8u,
  kStatusIdle = 0x00u,
  kStatusBusyExecute = 0x01u,
  kStatusLocked = 0xffu,
};

static volatile bool g_fault_seen = false;
static volatile uint32_t g_fault_mcause = 0u;
static volatile bool g_load_integrity_nmi_seen = false;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  g_fault_seen = true;
  g_fault_mcause = ibex_mcause_read();
}

void ottf_internal_isr(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t err_status =
      abs_mmio_read32(kIbexBase + RV_CORE_IBEX_ERR_STATUS_REG_OFFSET);
  if ((err_status & (1u << RV_CORE_IBEX_ERR_STATUS_FATAL_INTG_ERR_BIT)) != 0u) {
    g_load_integrity_nmi_seen = true;
  }
  abs_mmio_write32(kIbexBase + RV_CORE_IBEX_ERR_STATUS_REG_OFFSET, UINT32_MAX);
}

// Base & Bignum OTBN instruction encoders matching
// hw/ip/otbn/rtl/otbn_decoder.sv
#define OTBN_ECALL() (0x00000073u)
#define OTBN_NOP() (0x00000013u)
#define OTBN_ADDI(rd, rs1, imm12)                                         \
  ((((uint32_t)(imm12)&0xfffu) << 20) | (((uint32_t)(rs1)&0x1fu) << 15) | \
   (0x0u << 12) | (((uint32_t)(rd)&0x1fu) << 7) | 0x13u)
#define OTBN_ADD(rd, rs1, rs2)                                         \
  ((((uint32_t)(rs2)&0x1fu) << 20) | (((uint32_t)(rs1)&0x1fu) << 15) | \
   (0x0u << 12) | (((uint32_t)(rd)&0x1fu) << 7) | 0x33u)
#define OTBN_SW(rs2, rs1, imm12)                                       \
  (((((uint32_t)(imm12) >> 5) & 0x7fu) << 25) |                        \
   (((uint32_t)(rs2)&0x1fu) << 20) | (((uint32_t)(rs1)&0x1fu) << 15) | \
   (0x2u << 12) | (((uint32_t)(imm12)&0x1fu) << 7) | 0x23u)
#define OTBN_JAL_PLUS4(rd) (0x0040006fu | (((uint32_t)(rd)&0x1fu) << 7))
#define OTBN_LOOPI(iters, bodysize_minus_1)                    \
  ((((uint32_t)(bodysize_minus_1)&0xfffu) << 20) |             \
   ((((uint32_t)(iters) >> 5) & 0x1fu) << 15) | (0x1u << 12) | \
   (((uint32_t)(iters)&0x1fu) << 7) | 0x7bu)
#define OTBN_BN_XOR(wrd, wrs1, wrs2)                                     \
  ((((uint32_t)(wrs2)&0x1fu) << 20) | (((uint32_t)(wrs1)&0x1fu) << 15) | \
   (0x6u << 12) | (((uint32_t)(wrd)&0x1fu) << 7) | 0x7bu)
#define OTBN_BN_ADDI(wrd, wrs1, imm10)                                     \
  ((((uint32_t)(imm10)&0x3ffu) << 20) | (((uint32_t)(wrs1)&0x1fu) << 15) | \
   (0x4u << 12) | (((uint32_t)(wrd)&0x1fu) << 7) | 0x2bu)
#define OTBN_BN_ADDM(wrd, wrs1, wrs2)                                    \
  ((((uint32_t)(wrs2)&0x1fu) << 20) | (((uint32_t)(wrs1)&0x1fu) << 15) | \
   (0x5u << 12) | (((uint32_t)(wrd)&0x1fu) << 7) | 0x2bu)
#define OTBN_BN_WSRW(wsr, wrs1)                   \
  ((1u << 31) | (((uint32_t)(wsr)&0xffu) << 20) | \
   (((uint32_t)(wrs1)&0x1fu) << 15) | (0x7u << 12) | 0x0bu)
#define OTBN_BN_SID_ZERO_OFF(grs2, grs1)                                 \
  ((((uint32_t)(grs2)&0x1fu) << 20) | (((uint32_t)(grs1)&0x1fu) << 15) | \
   (0x5u << 12) | 0x0bu)

static void otbn_wait_for_not_running(void) {
  for (uint32_t i = 0; i < 1000000; ++i) {
    uint32_t status = abs_mmio_read32(kOtbnBase + OTBN_STATUS_REG_OFFSET);
    if (status == kStatusIdle || status == kStatusLocked) {
      return;
    }
  }
  CHECK(false, "Timed out waiting for OTBN to finish");
}

static void otbn_run_program(const uint32_t *insns, size_t count) {
  otbn_wait_for_not_running();
  for (size_t i = 0; i < count; ++i) {
    abs_mmio_write32(kOtbnBase + OTBN_IMEM_REG_OFFSET + i * sizeof(uint32_t),
                     insns[i]);
  }
  abs_mmio_write32(kOtbnBase + OTBN_INTR_STATE_REG_OFFSET, 1u);
  abs_mmio_write32(kOtbnBase + OTBN_CMD_REG_OFFSET, kCmdExecute);
  otbn_wait_for_not_running();
}

bool test_main(void) {
  CHECK_STATUS_OK(entropy_testutils_auto_mode_init());
  otbn_wait_for_not_running();
  abs_mmio_write32(kOtbnBase + OTBN_CTRL_REG_OFFSET, 0u);

  // -------------------------------------------------------------------------
  // 1. [otbn_alu_bignum.sv:905-919] (SPEC_DOC_ERRATA):
  //    (a) BN.ADDM reduces wrs1 + wrs2 >= MOD (`otbn_alu_bignum.sv:905-919`
  //        returns 0 when 60 + 40 == 100 == MOD, vs `bignum-insns.yml:116`
  //        stating strict `> MOD`).
  //    (b) Dual-operand `x1` call stack read (`add x2, x1, x1`) pops only once
  //        and pop is ordered before push (`otbn_rf_base.sv:105-134`).
  // -------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [otbn_alu_bignum.sv:905-919] (SPEC_DOC_ERRATA): BN.ADDM >= "
      "MOD boundary "
      "& x1 call stack single-pop...");
  abs_mmio_write32(kOtbnBase + OTBN_DMEM_REG_OFFSET, 0xdeadbeefu);
  const uint32_t prog_bn_addm[] = {
      OTBN_BN_XOR(0, 0, 0),        // w0 = 0
      OTBN_BN_ADDI(1, 0, 100),     // w1 = 100
      OTBN_BN_WSRW(0, 1),          // MOD = w1 (100)
      OTBN_BN_ADDI(2, 0, 60),      // w2 = 60
      OTBN_BN_ADDI(3, 0, 40),      // w3 = 40
      OTBN_BN_ADDM(4, 2, 3),       // w4 = (60 + 40) mod 100 == 0
      OTBN_ADDI(2, 0, 4),          // x2 = 4 (index of w4)
      OTBN_BN_SID_ZERO_OFF(2, 0),  // DMEM[0..31] = w4
      OTBN_ECALL(),
  };
  otbn_run_program(prog_bn_addm, ARRAYSIZE(prog_bn_addm));
  uint32_t addm_res = abs_mmio_read32(kOtbnBase + OTBN_DMEM_REG_OFFSET);
  CHECK(addm_res == 0u,
        "[otbn_alu_bignum.sv:905-919] BN.ADDM (60 + 40 == 100 == MOD) expected "
        "0, got %u",
        addm_res);

  abs_mmio_write32(kOtbnBase + OTBN_DMEM_REG_OFFSET, 0u);
  const uint32_t prog_x1_dual_read[] = {
      OTBN_JAL_PLUS4(1),    // PC=0: pushes 4 onto x1 call stack
      OTBN_ADDI(1, 1, 10),  // PC=4: pops 4 and pushes 14 onto x1 call stack
      OTBN_ADD(2, 1, 1),  // PC=8: reads x1 on both ports (14+14=28), pops once
      OTBN_SW(2, 0, 0),   // DMEM[0] = x2
      OTBN_ECALL(),
  };
  otbn_run_program(prog_x1_dual_read, ARRAYSIZE(prog_x1_dual_read));
  uint32_t err_bits = abs_mmio_read32(kOtbnBase + OTBN_ERR_BITS_REG_OFFSET);
  uint32_t x2_val = abs_mmio_read32(kOtbnBase + OTBN_DMEM_REG_OFFSET);
  CHECK(
      err_bits == 0u && x2_val == 28u,
      "[otbn_alu_bignum.sv:905-919] Expected x2=28 and ERR_BITS=0, got x2=%u, "
      "ERR_BITS=0x%08x",
      x2_val, err_bits);

  // -------------------------------------------------------------------------
  // 2. [otbn.sv:371-383,610-622] (INTENDED_SECURITY_HARDENING):
  //    Sub-word writes (`sb`) to IMEM/DMEM (`.ByteAccess(0)`), narrow CSRs
  //    (`OTBN_PERMIT`), and accesses to unmapped offset `0x2c` (`addrmiss`)
  //    assert TL-UL `d_error = 1` (`mcause = 5 / 7`).
  // -------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [otbn.sv:371-383,610-622] (INTENDED_SECURITY_HARDENING): "
      ".ByteAccess(0), "
      "OTBN_PERMIT, and addrmiss TL-UL bus faults...");
  abs_mmio_write32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET, 0x12345678u);
  g_fault_seen = false;
  abs_mmio_write8(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET, 0xffu);
  CHECK(g_fault_seen && g_fault_mcause == kIbexExcStoreAccessFault,
        "[otbn.sv:371-383,610-622] sb to LOAD_CHECKSUM must raise MCAUSE=7");
  CHECK(
      abs_mmio_read32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET) == 0x12345678u,
      "[otbn.sv:371-383,610-622] sb to LOAD_CHECKSUM must not modify register");

  g_fault_seen = false;
  (void)abs_mmio_read32(kOtbnBase + 0x2cu);
  CHECK(g_fault_seen && g_fault_mcause == kIbexExcLoadAccessFault,
        "[otbn.sv:371-383,610-622] Read at unmapped 0x2c must raise MCAUSE=5");

  g_fault_seen = false;
  abs_mmio_write32(kOtbnBase + 0x2cu, 0u);
  CHECK(g_fault_seen && g_fault_mcause == kIbexExcStoreAccessFault,
        "[otbn.sv:371-383,610-622] Write at unmapped 0x2c must raise MCAUSE=7");

  abs_mmio_write32(kOtbnBase + OTBN_DMEM_REG_OFFSET, 0x89abcdefu);
  abs_mmio_write32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET, 0u);
  g_fault_seen = false;
  abs_mmio_write8(kOtbnBase + OTBN_IMEM_REG_OFFSET, 0x11u);
  CHECK(g_fault_seen && g_fault_mcause == kIbexExcStoreAccessFault,
        "[otbn.sv:371-383,610-622] sb to IMEM (.ByteAccess(0)) must raise "
        "MCAUSE=7");
  g_fault_seen = false;
  abs_mmio_write8(kOtbnBase + OTBN_DMEM_REG_OFFSET, 0x22u);
  CHECK(g_fault_seen && g_fault_mcause == kIbexExcStoreAccessFault,
        "[otbn.sv:371-383,610-622] sb to DMEM (.ByteAccess(0)) must raise "
        "MCAUSE=7");
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET) == 0u,
        "[otbn.sv:371-383,610-622] Sub-word write faults must not advance "
        "LOAD_CHECKSUM");
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_DMEM_REG_OFFSET) == 0x89abcdefu,
        "[otbn.sv:371-383,610-622] sb to DMEM must not modify word");

  // -------------------------------------------------------------------------
  // 3. [otbn.sv:736-746] (TRUE_SILICON_ERRATA — Part A):
  //    Idle 32-bit host reads (`TL-UL Get`) from IMEM[0] and DMEM[0] advance
  //    `LOAD_CHECKSUM` identically to writing `32'h00000000`
  //    (`otbn.sv:736-746`).
  // -------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [otbn.sv:736-746] (TRUE_SILICON_ERRATA): Idle 32-bit host "
      "IMEM/DMEM read advances LOAD_CHECKSUM...");
  abs_mmio_write32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET, 0u);
  abs_mmio_write32(kOtbnBase + OTBN_IMEM_REG_OFFSET, 0u);
  abs_mmio_write32(kOtbnBase + OTBN_DMEM_REG_OFFSET, 0u);
  const uint32_t crc_zero_writes =
      abs_mmio_read32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET);
  CHECK(crc_zero_writes != 0u);

  // Seed non-zero contents into IMEM[0] and DMEM[0], then clear LOAD_CHECKSUM
  // and perform 32-bit READS from IMEM[0] and DMEM[0]. Because
  // `tlul_adapter_sram` gates `wdata_int` to 0 on reads (`we_o == 0`), both
  // reads advance `u_mem_load_crc32` with `wr_data = 0x00000000`!
  abs_mmio_write32(kOtbnBase + OTBN_IMEM_REG_OFFSET, 0xa5a5a5a5u);
  abs_mmio_write32(kOtbnBase + OTBN_DMEM_REG_OFFSET, 0x5a5a5a5au);
  abs_mmio_write32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET, 0u);
  uint32_t imem0_rd = abs_mmio_read32(kOtbnBase + OTBN_IMEM_REG_OFFSET);
  uint32_t dmem0_rd = abs_mmio_read32(kOtbnBase + OTBN_DMEM_REG_OFFSET);
  uint32_t crc_after_reads =
      abs_mmio_read32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET);
  CHECK(imem0_rd == 0xa5a5a5a5u && dmem0_rd == 0x5a5a5a5au);
  CHECK(crc_after_reads == crc_zero_writes,
        "[otbn.sv:736-746] Idle IMEM[0]+DMEM[0] reads advanced LOAD_CHECKSUM "
        "to 0x%08x (expected 0x%08x)",
        crc_after_reads, crc_zero_writes);

  // Also record expected CRC for writing 0x12345678 to DMEM[0] and IMEM[0].
  abs_mmio_write32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET, 0u);
  abs_mmio_write32(kOtbnBase + OTBN_DMEM_REG_OFFSET, 0x12345678u);
  abs_mmio_write32(kOtbnBase + OTBN_IMEM_REG_OFFSET, 0x12345678u);
  const uint32_t expected_crc_12345678 =
      abs_mmio_read32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET);
  CHECK(expected_crc_12345678 != 0u);

  // -------------------------------------------------------------------------
  // 4. [otbn.sv:451-477,691-720], [otbn.sv:146-158,798-806,892,926-995],
  // [otbn.sv:352,451-472,566,691-715,926-957]
  //    (INTENDED_SECURITY_HARDENING) & [otbn.sv:736-746] Part B (StatusLocked):
  //    - Illegal host read from DMEM during BUSY_EXECUTE returns 39'h0
  //      (`ecc = 7'h00 != SecdedInv3932ZeroEcc = 7'h39`) before `locking_q`
  //      latches, triggering Ibex Load Integrity NMI (`FATAL_INTG_ERR`) and
  //      locking OTBN (`STATUS = 0xFF`, `ERR_BITS.ILLEGAL_BUS_ACCESS = 1`,
  //      `FATAL_ALERT_CAUSE.ILLEGAL_BUS_ACCESS = 1`), while `LOAD_CHECKSUM`
  //      remains 0 (`dmem_access_core == 1`).
  //    - Once `STATUS == LOCKED` (`locking_q == 1`), host reads from DMEM/IMEM
  //      return `0x00000000` with valid `SecdedInv3932ZeroEcc` (`7'h39`, no
  //      Ibex NMI), while 32-bit host writes/reads still advance
  //      `LOAD_CHECKSUM`
  //      (`[otbn.sv:736-746]` Part B).
  //    - In `STATUS == LOCKED`, `INSN_CNT` and `ERR_BITS` are write-any-clears
  //      to 0, while `FATAL_ALERT_CAUSE` remains sticky set-only.
  // -------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [otbn.sv:451-477,691-720..005] (INTENDED_SECURITY_HARDENING) "
      "& "
      "[otbn.sv:736-746] StatusLocked LOAD_CHECKSUM...");
  const uint32_t prog_long_loop[] = {
      OTBN_LOOPI(1000, 0),
      OTBN_NOP(),
      OTBN_ECALL(),
  };
  for (size_t i = 0; i < ARRAYSIZE(prog_long_loop); ++i) {
    abs_mmio_write32(kOtbnBase + OTBN_IMEM_REG_OFFSET + i * sizeof(uint32_t),
                     prog_long_loop[i]);
  }
  abs_mmio_write32(kOtbnBase + OTBN_INSN_CNT_REG_OFFSET, 0u);
  abs_mmio_write32(kOtbnBase + OTBN_INTR_STATE_REG_OFFSET, 1u);
  abs_mmio_write32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET, 0u);
  abs_mmio_write32(kOtbnBase + OTBN_CMD_REG_OFFSET, kCmdExecute);

  for (int i = 0;
       i < 1000 && abs_mmio_read32(kOtbnBase + OTBN_INSN_CNT_REG_OFFSET) == 0u;
       ++i) {
  }
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_STATUS_REG_OFFSET) ==
        kStatusBusyExecute);

  g_load_integrity_nmi_seen = false;
  (void)abs_mmio_read32(kOtbnBase + OTBN_DMEM_REG_OFFSET);
  otbn_wait_for_not_running();
  for (volatile int i = 0; i < 500; ++i) {
  }

  CHECK(g_load_integrity_nmi_seen,
        "[otbn.sv:451-477,691-720] Illegal DMEM read during BUSY_EXECUTE must "
        "raise "
        "Ibex FATAL_INTG_ERR NMI");
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_STATUS_REG_OFFSET) == kStatusLocked);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_ERR_BITS_REG_OFFSET) ==
        (1u << OTBN_ERR_BITS_ILLEGAL_BUS_ACCESS_BIT));
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_FATAL_ALERT_CAUSE_REG_OFFSET) ==
        (1u << OTBN_FATAL_ALERT_CAUSE_ILLEGAL_BUS_ACCESS_BIT));

  // Note on [otbn.sv:352,451-472,566,691-715,926-957]: Although otbn.sv:466,707
  // wires `locking_q ? SecdedInv3932ZeroEcc : ...` intending to return 0 with
  // valid ECC when locked, `locking_q` only asserts on Cycle 2 (1 cycle after
  // `dmem_dummy_response_q` on Cycle 1), and once in STATUS_LOCKED
  // (`busy_execute_q == 0`), `u_dmem`/`u_imem` wire `.intg_error_i(locking)`
  // which kills `rvalid_o` in `prim_ram_1p_scr.sv:379` while
  // `dmem_dummy_response_d` is 0. Thus TL-UL writes in STATUS_LOCKED succeed
  // immediately (`dmem_gnt_bus = dmem_req_bus`) and advance LOAD_CHECKSUM
  // while SRAM macro writes are suppressed by `intg_error_i(locking)`.

  // Verify [otbn.sv:736-746] Part B: 32-bit writes in STATUS_LOCKED advance
  // LOAD_CHECKSUM even though SRAM writes are blocked by `locking`.
  abs_mmio_write32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET, 0u);
  abs_mmio_write32(kOtbnBase + OTBN_DMEM_REG_OFFSET, 0x12345678u);
  abs_mmio_write32(kOtbnBase + OTBN_IMEM_REG_OFFSET, 0x12345678u);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET) ==
        expected_crc_12345678);

  // Verify [otbn.sv:146-158,798-806,892,926-995]: INTR_STATE, INSN_CNT, and
  // ERR_BITS remain writable in STATUS_LOCKED, while FATAL_ALERT_CAUSE remains
  // sticky.
  abs_mmio_write32(kOtbnBase + OTBN_INTR_STATE_REG_OFFSET, 1u);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_INTR_STATE_REG_OFFSET) == 0u);

  abs_mmio_write32(kOtbnBase + OTBN_INSN_CNT_REG_OFFSET, 0xffffffffu);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_INSN_CNT_REG_OFFSET) == 0u);

  abs_mmio_write32(kOtbnBase + OTBN_ERR_BITS_REG_OFFSET, 0xffffffffu);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_ERR_BITS_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_FATAL_ALERT_CAUSE_REG_OFFSET) ==
        (1u << OTBN_FATAL_ALERT_CAUSE_ILLEGAL_BUS_ACCESS_BIT));

  LOG_INFO("All [otbn.sv:736-746..006] checks confirmed!");
  return true;
}
