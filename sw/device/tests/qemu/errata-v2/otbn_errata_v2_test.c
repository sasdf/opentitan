// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * CW340 FPGA Earlgrey v2 (`trunk-v2`) Hardware & Specification Errata
 * Verification Test for `otbn` (`hw/ip/otbn/rtl/otbn.sv`).
 *
 * Empirically verifies on physical CW340 FPGA silicon (`trunk-v2` bitstream):
 * 1. `otbn_alu_bignum.sv` / `otbn_mod_result_selector.sv` vs
 *    `bignum-insns.yml:116` & `otbn_rf_base.sv:108-134`:
 *    - `BN.ADDM` subtracts `MOD` when `wrs1 + wrs2 >= MOD` (returning `0` at
 *      equality `wrs1 + wrs2 == MOD`, whereas `bignum-insns.yml:116` states
 *      strict `> MOD`).
 *    - Dual-operand `x1` call-stack read (`add x2, x1, x1`) pops `x1` once
 *      and orders pop before push.
 * 2. `otbn.sv:419, 674` & `otbn_reg_pkg.sv:376-392`:
 *    - Sub-word (`sb`) writes to `LOAD_CHECKSUM`, `SCRATCH_0` (`0x2000`),
 *      `IMEM` (`.ByteAccess(0)`), and `DMEM` (`.ByteAccess(0)`), and accesses
 *      to unmapped offset `0x2c` (`addrmiss`) trigger synchronous TL-UL bus
 *      faults (`d_error = 1`, `mcause = 5 / 7`).
 * 3. `otbn.sv:788-801` vs `otbn.hjson:798-802` & `theory_of_operation.md`:
 *    - `mem_crc_data_in_valid` checks `imem_byte_mask_bus == 4'hf` /
 *      `dmem_byte_mask_bus == 4'hf` without gating on `imem_write_bus` /
 *      `dmem_write_bus` or `~locking`. Consequently, any 32-bit host TL-UL
 *      `Get` (read) from `IMEM`/`DMEM` while idle advances `LOAD_CHECKSUM`
 *      with `wr_data = 0`.
 * 4. [NEW IN V2] `otbn_rnd.sv:257-282` vs `csr.yml:102-122, 257-274`:
 *    - When `CTRL.URND_CTRL_ENABLED == 1` (`bit 2`), writing `START | STOP`
 *      (`0x3`) simultaneously to `URND_CTRL` (`0x7d9`) while the Bivium PRNG
 *      is running (`urnd_stopped_q == 0`) keeps the PRNG running
 *      (`urnd_stopped_d = 0`, `URND_STATUS.STOPPED = 0` because `START` has
 *      priority over `STOP`), yet `otbn_rnd.sv:280`
 *      (`!urnd_stopped_q && stop_cmd && urnd_advance`) falsely latches
 *      `URND_STATUS.USED_WHILE_STOPPED = 1` (`bit 3`) because `urnd_advance`
 *      remains `1` (`!urnd_stopped_d == 1`).
 * 5. [NEW IN V2] `otbn_mai.sv:456-459` vs `csr.yml:217-235` &
 *    `dif_otbn.h:55-85`:
 *    - Writing `MAI_CTRL` (`0x7e0`) with `START = 0` and an invalid
 *      `OPERATION` (`0`) is ignored (`invalid_op` is gated by `& ma_start` in
 *      `otbn_mai.sv:458-459`, contradicting `csr.yml:228`).
 *    - Conversely, writing `MAI_CTRL` (`0x7e0`) with `START = 0`, valid
 *      `OPERATION = 23` (`SecAdd`), and any reserved bit `[31:6]` set
 *      (`0x6e`) is NOT ignored (`csr.yml:234` states `"31-6: Reserved. Any
 *      write is ignored"`): `rsvd_csr_write` (`otbn_mai.sv:456`) immediately
 *      aborts OTBN execution and latches `ERR_BITS.MAI_SOFTWARE_ERROR` (`bit
 * 8`, `0x100`), which is also omitted from `dif_otbn_err_bits_t` in
 *      `sw/device/lib/dif/dif_otbn.h:55-85`.
 * 6. [NEW IN V2 + V1] `otbn.sv:169-175, 451, 484-522, 705, 788-792, 997-1066`:
 *    - When paused by `WFI` (`STATUS_PAUSED = 0x05`, `wfi_pending = 1`):
 *      (a) `is_not_running_q` is `0`, so host writes to `INSN_CNT` and
 *          `ERR_BITS` are ignored while paused (unlike `STATUS_LOCKED` `0xFF`
 *          where `is_not_running_q == 1` allows clearing both);
 *      (b) `dmem_access_core` is `0` while `imem_access_core` is `1`
 *          (`wfi_pending == 1`), which forces `mem_crc_data_in_valid` to `0`
 *          for both reads and writes, so host 32-bit `DMEM` writes while paused
 *          modify `DMEM` in SRAM while bypassing `LOAD_CHECKSUM`;
 *      (c) `imem_access_core` is `1` (`wfi_pending == 1`), so a host `IMEM`
 *          read while `STATUS_PAUSED` (`0x05`) returns blanked `39'h0`
 *          (`ecc = 7'h00 != SecdedInv3932ZeroEcc = 7'h39`) before `locking_q`
 *          latches, raising an Ibex `FATAL_INTG_ERR` NMI and transitioning
 *          OTBN directly from `STATUS_PAUSED` (`0x05`) to `STATUS_LOCKED`
 *          (`0xFF`).
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_otbn.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/entropy_testutils.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top/otbn_regs.h"
#include "hw/top/rv_core_ibex_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG(.ignore_alerts = true);

enum {
  kOtbnBase = TOP_EARLGREY_OTBN_BASE_ADDR,
  kIbexBase = TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR,
  kCmdExecute = 0xd8u,
  kCmdResume = 0xa6u,
  kStatusIdle = 0x00u,
  kStatusBusyExecute = 0x01u,
  kStatusPaused = 0x05u,
  kStatusLocked = 0xffu,
  // OTBN CSR addresses from hw/ip/otbn/data/csr.yml
  kOtbnCsrUrndCtrl = 0x7d9u,
  kOtbnCsrMaiCtrl = 0x7e0u,
  kOtbnCsrUrndStatus = 0xfc2u,
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
#define OTBN_WFI() (0x10500073u)
#define OTBN_NOP() (0x00000013u)
#define OTBN_ADDI(rd, rs1, imm12)                                             \
  ((((uint32_t)(imm12) & 0xfffu) << 20) | (((uint32_t)(rs1) & 0x1fu) << 15) | \
   (0x0u << 12) | (((uint32_t)(rd) & 0x1fu) << 7) | 0x13u)
#define OTBN_ADD(rd, rs1, rs2)                                             \
  ((((uint32_t)(rs2) & 0x1fu) << 20) | (((uint32_t)(rs1) & 0x1fu) << 15) | \
   (0x0u << 12) | (((uint32_t)(rd) & 0x1fu) << 7) | 0x33u)
#define OTBN_SW(rs2, rs1, imm12)                                           \
  (((((uint32_t)(imm12) >> 5) & 0x7fu) << 25) |                            \
   (((uint32_t)(rs2) & 0x1fu) << 20) | (((uint32_t)(rs1) & 0x1fu) << 15) | \
   (0x2u << 12) | (((uint32_t)(imm12) & 0x1fu) << 7) | 0x23u)
#define OTBN_JAL_PLUS4(rd) (0x0040006fu | (((uint32_t)(rd) & 0x1fu) << 7))
#define OTBN_CSRRW(rd, csr, rs1)                                            \
  ((((uint32_t)(csr) & 0xfffu) << 20) | (((uint32_t)(rs1) & 0x1fu) << 15) | \
   (0x1u << 12) | (((uint32_t)(rd) & 0x1fu) << 7) | 0x73u)
#define OTBN_CSRRS(rd, csr, rs1)                                            \
  ((((uint32_t)(csr) & 0xfffu) << 20) | (((uint32_t)(rs1) & 0x1fu) << 15) | \
   (0x2u << 12) | (((uint32_t)(rd) & 0x1fu) << 7) | 0x73u)
#define OTBN_BN_XOR(wrd, wrs1, wrs2)                                         \
  ((((uint32_t)(wrs2) & 0x1fu) << 20) | (((uint32_t)(wrs1) & 0x1fu) << 15) | \
   (0x6u << 12) | (((uint32_t)(wrd) & 0x1fu) << 7) | 0x7bu)
#define OTBN_BN_ADDI(wrd, wrs1, imm10)                                         \
  ((((uint32_t)(imm10) & 0x3ffu) << 20) | (((uint32_t)(wrs1) & 0x1fu) << 15) | \
   (0x4u << 12) | (((uint32_t)(wrd) & 0x1fu) << 7) | 0x2bu)
#define OTBN_BN_ADDM(wrd, wrs1, wrs2)                                        \
  ((((uint32_t)(wrs2) & 0x1fu) << 20) | (((uint32_t)(wrs1) & 0x1fu) << 15) | \
   (0x5u << 12) | (((uint32_t)(wrd) & 0x1fu) << 7) | 0x2bu)
#define OTBN_BN_WSRW(wsr, wrs1)                     \
  ((1u << 31) | (((uint32_t)(wsr) & 0xffu) << 20) | \
   (((uint32_t)(wrs1) & 0x1fu) << 15) | (0x7u << 12) | 0x0bu)
#define OTBN_BN_SID_ZERO_OFF(grs2, grs1)                                     \
  ((((uint32_t)(grs2) & 0x1fu) << 20) | (((uint32_t)(grs1) & 0x1fu) << 15) | \
   (0x5u << 12) | 0x0bu)

static void otbn_wait_for_settled_status(void) {
  for (uint32_t i = 0; i < 1000000; ++i) {
    uint32_t status = abs_mmio_read32(kOtbnBase + OTBN_STATUS_REG_OFFSET);
    if (status == kStatusIdle || status == kStatusPaused ||
        status == kStatusLocked) {
      return;
    }
  }
  CHECK(false, "Timed out waiting for OTBN status to settle");
}

static void otbn_run_program(const uint32_t *insns, size_t count) {
  otbn_wait_for_settled_status();
  for (size_t i = 0; i < count; ++i) {
    abs_mmio_write32(kOtbnBase + OTBN_IMEM_REG_OFFSET + i * sizeof(uint32_t),
                     insns[i]);
  }
  abs_mmio_write32(kOtbnBase + OTBN_INTR_STATE_REG_OFFSET, 1u);
  abs_mmio_write32(kOtbnBase + OTBN_CMD_REG_OFFSET, kCmdExecute);
  otbn_wait_for_settled_status();
}

bool test_main(void) {
  CHECK_STATUS_OK(entropy_testutils_auto_mode_init());
  otbn_wait_for_settled_status();

  dif_otbn_t otbn;
  CHECK_DIF_OK(dif_otbn_init(mmio_region_from_addr(kOtbnBase), &otbn));
  abs_mmio_write32(kOtbnBase + OTBN_CTRL_REG_OFFSET, 0u);

  // -------------------------------------------------------------------------
  // 1. [otbn_alu_bignum.sv / otbn_mod_result_selector.sv vs
  // bignum-insns.yml:116
  //     & otbn_rf_base.sv:108-134]:
  //    (a) BN.ADDM reduces wrs1 + wrs2 >= MOD (returning 0 when 60 + 40 == 100
  //        == MOD, whereas bignum-insns.yml:116 states strict `> MOD`).
  //    (b) Dual-operand `x1` call stack read (`add x2, x1, x1`) pops only once
  //        and orders pop before push.
  // -------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [otbn_alu_bignum.sv vs bignum-insns.yml:116]: BN.ADDM >= MOD "
      "equality reduction & x1 dual-operand single-pop...");
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
  CHECK(addm_res == 0u, "BN.ADDM (60 + 40 == 100 == MOD) expected 0, got %u",
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
  CHECK(err_bits == 0u && x2_val == 28u,
        "Expected x2=28 and ERR_BITS=0, got x2=%u, ERR_BITS=0x%08x", x2_val,
        err_bits);

  // -------------------------------------------------------------------------
  // 2. [otbn.sv:419, 674 & otbn_reg_pkg.sv:376-392]:
  //    Sub-word writes (`sb`) to IMEM/DMEM (`.ByteAccess(0)`), narrow CSRs
  //    (`OTBN_PERMIT`), and v2 `SCRATCH_0` (`0x2000`), plus accesses to
  //    unmapped offset `0x2c` (`addrmiss`) assert TL-UL `d_error = 1`
  //    (`mcause = 5 / 7`).
  // -------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [otbn.sv:419,674 & otbn_reg_pkg.sv:376-392]: .ByteAccess(0), "
      "OTBN_PERMIT, and addrmiss TL-UL bus faults...");
  abs_mmio_write32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET, 0x12345678u);
  g_fault_seen = false;
  abs_mmio_write8(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET, 0xffu);
  CHECK(g_fault_seen && g_fault_mcause == kIbexExcStoreAccessFault,
        "sb to LOAD_CHECKSUM must raise MCAUSE=7");
  CHECK(
      abs_mmio_read32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET) == 0x12345678u,
      "sb to LOAD_CHECKSUM must not modify register");

  abs_mmio_write32(kOtbnBase + OTBN_SCRATCH_0_REG_OFFSET, 0xcafebabeu);
  g_fault_seen = false;
  abs_mmio_write8(kOtbnBase + OTBN_SCRATCH_0_REG_OFFSET, 0x00u);
  CHECK(g_fault_seen && g_fault_mcause == kIbexExcStoreAccessFault,
        "sb to SCRATCH_0 (OTBN_PERMIT=4'b1111) must raise MCAUSE=7");
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_SCRATCH_0_REG_OFFSET) == 0xcafebabeu,
        "sb to SCRATCH_0 must not modify register");

  g_fault_seen = false;
  (void)abs_mmio_read32(kOtbnBase + 0x2cu);
  CHECK(g_fault_seen && g_fault_mcause == kIbexExcLoadAccessFault,
        "Read at unmapped 0x2c must raise MCAUSE=5");

  g_fault_seen = false;
  abs_mmio_write32(kOtbnBase + 0x2cu, 0u);
  CHECK(g_fault_seen && g_fault_mcause == kIbexExcStoreAccessFault,
        "Write at unmapped 0x2c must raise MCAUSE=7");

  abs_mmio_write32(kOtbnBase + OTBN_DMEM_REG_OFFSET, 0x89abcdefu);
  abs_mmio_write32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET, 0u);
  g_fault_seen = false;
  abs_mmio_write8(kOtbnBase + OTBN_IMEM_REG_OFFSET, 0x11u);
  CHECK(g_fault_seen && g_fault_mcause == kIbexExcStoreAccessFault,
        "sb to IMEM (.ByteAccess(0)) must raise MCAUSE=7");
  g_fault_seen = false;
  abs_mmio_write8(kOtbnBase + OTBN_DMEM_REG_OFFSET, 0x22u);
  CHECK(g_fault_seen && g_fault_mcause == kIbexExcStoreAccessFault,
        "sb to DMEM (.ByteAccess(0)) must raise MCAUSE=7");
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET) == 0u,
        "Sub-word write faults must not advance LOAD_CHECKSUM");
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_DMEM_REG_OFFSET) == 0x89abcdefu,
        "sb to DMEM must not modify word");

  // -------------------------------------------------------------------------
  // 3. [otbn.sv:788-801]:
  //    Idle 32-bit host reads (`TL-UL Get`) from IMEM[0] and DMEM[0] advance
  //    `LOAD_CHECKSUM` identically to writing `32'h00000000`.
  // -------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [otbn.sv:788-801]: Idle 32-bit host IMEM/DMEM reads advance "
      "LOAD_CHECKSUM...");
  abs_mmio_write32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET, 0u);
  abs_mmio_write32(kOtbnBase + OTBN_DMEM_REG_OFFSET, 0u);
  const uint32_t crc_single_dmem_zero_write =
      abs_mmio_read32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET);
  CHECK(crc_single_dmem_zero_write != 0u);

  abs_mmio_write32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET, 0u);
  abs_mmio_write32(kOtbnBase + OTBN_IMEM_REG_OFFSET, 0u);
  abs_mmio_write32(kOtbnBase + OTBN_DMEM_REG_OFFSET, 0u);
  const uint32_t crc_zero_writes =
      abs_mmio_read32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET);
  CHECK(crc_zero_writes != 0u);

  abs_mmio_write32(kOtbnBase + OTBN_IMEM_REG_OFFSET, 0xa5a5a5a5u);
  abs_mmio_write32(kOtbnBase + OTBN_DMEM_REG_OFFSET, 0x5a5a5a5au);
  abs_mmio_write32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET, 0u);
  uint32_t imem0_rd = abs_mmio_read32(kOtbnBase + OTBN_IMEM_REG_OFFSET);
  uint32_t dmem0_rd = abs_mmio_read32(kOtbnBase + OTBN_DMEM_REG_OFFSET);
  uint32_t crc_after_reads =
      abs_mmio_read32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET);
  CHECK(imem0_rd == 0xa5a5a5a5u && dmem0_rd == 0x5a5a5a5au);
  CHECK(crc_after_reads == crc_zero_writes,
        "Idle IMEM[0]+DMEM[0] reads advanced LOAD_CHECKSUM to 0x%08x (expected "
        "0x%08x)",
        crc_after_reads, crc_zero_writes);

  abs_mmio_write32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET, 0u);
  abs_mmio_write32(kOtbnBase + OTBN_DMEM_REG_OFFSET, 0x12345678u);
  abs_mmio_write32(kOtbnBase + OTBN_IMEM_REG_OFFSET, 0x12345678u);
  const uint32_t expected_crc_12345678 =
      abs_mmio_read32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET);
  CHECK(expected_crc_12345678 != 0u);

  // -------------------------------------------------------------------------
  // 4. [NEW IN V2] [otbn_rnd.sv:257-282 vs csr.yml:102-122, 257-274]:
  //    When CTRL.URND_CTRL_ENABLED == 1 (bit 2), writing `START | STOP` (0x3)
  //    to `URND_CTRL` (0x7d9) while the Bivium PRNG is running keeps the PRNG
  //    running (`URND_STATUS.STOPPED == 0` because `START` has priority over
  //    `STOP`), yet `otbn_rnd.sv:280` (`!urnd_stopped_q && stop_cmd &&
  //    urnd_advance`) falsely sets `URND_STATUS.USED_WHILE_STOPPED = 1`
  //    (bit 3)!
  // -------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [NEW IN V2: otbn_rnd.sv:257-282 vs csr.yml:102-122]: "
      "URND_CTRL START|STOP simultaneous write sets USED_WHILE_STOPPED=1 while "
      "STOPPED=0...");
  abs_mmio_write32(kOtbnBase + OTBN_CTRL_REG_OFFSET,
                   1u << OTBN_CTRL_URND_CTRL_ENABLED_BIT);
  abs_mmio_write32(kOtbnBase + OTBN_DMEM_REG_OFFSET, 0u);
  abs_mmio_write32(kOtbnBase + OTBN_DMEM_REG_OFFSET + 4u, 0u);
  const uint32_t prog_urnd_start_stop_bug[] = {
      OTBN_CSRRS(3, kOtbnCsrUrndStatus, 0),  // x3 = initial URND_STATUS
      OTBN_ADDI(2, 0, 3),                    // x2 = 0x3 (START | STOP)
      OTBN_CSRRW(0, kOtbnCsrUrndCtrl, 2),    // URND_CTRL = 0x3
      OTBN_NOP(),                            // No URND read!
      OTBN_CSRRS(4, kOtbnCsrUrndStatus,
                 0),     // x4 = URND_STATUS after START|STOP
      OTBN_SW(3, 0, 0),  // DMEM[0] = initial URND_STATUS
      OTBN_SW(4, 0, 4),  // DMEM[4] = post URND_STATUS
      OTBN_ECALL(),
  };
  otbn_run_program(prog_urnd_start_stop_bug,
                   ARRAYSIZE(prog_urnd_start_stop_bug));
  uint32_t urnd_status_init = abs_mmio_read32(kOtbnBase + OTBN_DMEM_REG_OFFSET);
  uint32_t urnd_status_post =
      abs_mmio_read32(kOtbnBase + OTBN_DMEM_REG_OFFSET + 4u);
  LOG_INFO("URND_STATUS init=0x%08x, post(START|STOP)=0x%08x", urnd_status_init,
           urnd_status_post);
  // Verify Bivium parameters exposed in URND_STATUS[31:16]:
  // URND_STATE_WIDTH = 177 (bits 25:16), URND_RESTORE_WIDTH = 32 (bits 31:26)
  CHECK(((urnd_status_init >> 16) & 0x3ffu) == 177u,
        "Expected URND_STATE_WIDTH=177, got %u",
        (urnd_status_init >> 16) & 0x3ffu);
  CHECK(((urnd_status_init >> 26) & 0x3fu) == 32u,
        "Expected URND_RESTORE_WIDTH=32, got %u",
        (urnd_status_init >> 26) & 0x3fu);
  // Before START|STOP: URND_CTRL_ENABLED=1 (bit 0), STOPPED=0 (bit 1),
  // USED_WHILE_STOPPED=0 (bit 3) -> low nibble == 0x1.
  CHECK((urnd_status_init & 0xfu) == 0x1u,
        "Expected initial URND_STATUS[3:0]=0x1, got 0x%x",
        urnd_status_init & 0xfu);
  // After START|STOP (0x3): STOPPED (bit 1) is STILL 0 (never stopped!),
  // YET USED_WHILE_STOPPED (bit 3) is falsely latched to 1 -> low nibble ==
  // 0x9!
  CHECK((urnd_status_post & 0xfu) == 0x9u,
        "Expected post-START|STOP URND_STATUS[3:0]=0x9 (STOPPED=0, "
        "USED_WHILE_STOPPED=1), got 0x%x",
        urnd_status_post & 0xfu);

  // -------------------------------------------------------------------------
  // 5. [NEW IN V2] [otbn_mai.sv:456-459 vs csr.yml:217-235 & dif_otbn.h:55-85]:
  //    (a) Writing MAI_CTRL (0x7e0) with START=0 and an invalid OPERATION (0)
  //        does NOT raise MAI_SOFTWARE_ERROR (`invalid_op` is gated by
  //        `& ma_start` in `otbn_mai.sv:458-459`, contradicting `csr.yml:228`).
  //    (b) Writing MAI_CTRL (0x7e0) with START=0, valid OPERATION=23 (SecAdd),
  //        and reserved bit 6 set (`0x6e = (1<<6) | (23<<1)`) is NOT ignored
  //        (contradicting `csr.yml:234` `"31-6: Reserved. Any write is
  //        ignored"`): `rsvd_csr_write` (`otbn_mai.sv:456`) immediately raises
  //        `ERR_BITS.MAI_SOFTWARE_ERROR` (`bit 8`, `0x100`), which is also
  //        missing from `dif_otbn_err_bits_t` in `dif_otbn.h:55-85`.
  // -------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [NEW IN V2: otbn_mai.sv:456-459 vs csr.yml:217-235]: MAI_CTRL "
      "START=0 invalid op vs reserved bit [31:6] MAI_SOFTWARE_ERROR...");
  abs_mmio_write32(kOtbnBase + OTBN_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kOtbnBase + OTBN_ERR_BITS_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kOtbnBase + OTBN_DMEM_REG_OFFSET, 0xffffffffu);
  const uint32_t prog_mai_invalid_op_no_start[] = {
      OTBN_ADDI(2, 0, 0),                 // x2 = 0 (START=0, invalid op=0)
      OTBN_CSRRW(0, kOtbnCsrMaiCtrl, 2),  // MAI_CTRL = 0x0
      OTBN_CSRRS(3, kOtbnCsrMaiCtrl, 0),  // x3 = readback MAI_CTRL
      OTBN_SW(3, 0, 0),                   // DMEM[0] = MAI_CTRL readback
      OTBN_ECALL(),
  };
  otbn_run_program(prog_mai_invalid_op_no_start,
                   ARRAYSIZE(prog_mai_invalid_op_no_start));
  uint32_t mai_ctrl_rb = abs_mmio_read32(kOtbnBase + OTBN_DMEM_REG_OFFSET);
  uint32_t mai_err_no_start =
      abs_mmio_read32(kOtbnBase + OTBN_ERR_BITS_REG_OFFSET);
  CHECK(mai_ctrl_rb == 0u && mai_err_no_start == 0u,
        "Expected MAI_CTRL=0 and ERR_BITS=0 on START=0 invalid op=0 write, got "
        "MAI_CTRL=0x%x ERR_BITS=0x%x",
        mai_ctrl_rb, mai_err_no_start);

  const uint32_t prog_mai_rsvd_bit_err[] = {
      // x2 = (1 << 6) | (23 << 1) = 0x6e (START=0, valid op=23 SecAdd, rsvd bit
      // 6=1)
      OTBN_ADDI(2, 0, 0x6e),
      OTBN_CSRRW(0, kOtbnCsrMaiCtrl, 2),
      OTBN_ECALL(),
  };
  otbn_run_program(prog_mai_rsvd_bit_err, ARRAYSIZE(prog_mai_rsvd_bit_err));
  dif_otbn_err_bits_t dif_err_bits = kDifOtbnErrBitsNoError;
  CHECK_DIF_OK(dif_otbn_get_err_bits(&otbn, &dif_err_bits));
  CHECK((uint32_t)dif_err_bits == (1u << OTBN_ERR_BITS_MAI_SOFTWARE_ERROR_BIT),
        "Expected ERR_BITS=0x100 (MAI_SOFTWARE_ERROR bit 8) on MAI_CTRL "
        "reserved bit write, got 0x%08x",
        (uint32_t)dif_err_bits);
  abs_mmio_write32(kOtbnBase + OTBN_ERR_BITS_REG_OFFSET, 0xffffffffu);

  // -------------------------------------------------------------------------
  // 6. [NEW IN V2 + V1] [otbn.sv:169-175, 451, 484-522, 705, 788-792]:
  //    WFI Pause (`STATUS_PAUSED = 0x05`) & `STATUS_LOCKED` (`0xFF`):
  //    - Execute a program with `WFI` when `CTRL.WFI_ENABLED == 1`.
  //    - Verify `STATUS == kStatusPaused` (`0x05`) and `INTR_STATE.done == 1`.
  //    - While paused (`wfi_pending == 1`, `is_not_running_q == 0`), verify
  //      host writes to `INSN_CNT` are ignored (`INSN_CNT` remains `2`).
  //    - While paused (`dmem_access_core == 0` so DMEM is unlocked for host
  //      writes, but `imem_access_core == 1` because `wfi_pending == 1`),
  //      `mem_crc_data_in_valid = ~(dmem_access_core | imem_access_core) & ...`
  //      (`otbn.sv:789`) is forced to `0`! Consequently, a 32-bit host WRITE to
  //      `DMEM[0]` (`0x99887766`) while paused modifies `DMEM[0]` in SRAM, yet
  //      completely bypasses `u_mem_load_crc32` (`LOAD_CHECKSUM` stays `0`)!
  //    - Resume via `CMD = kCmdResume` (`0xa6`) and verify completion.
  //    - Re-enter `STATUS_PAUSED` (`0x05`) and perform a 32-bit host read from
  //      `IMEM[0]` (`imem_access_core == 1`). Verify it returns blanked `39'h0`
  //      (`ecc = 7'h00`) before `locking_q` latches, raising an Ibex
  //      `FATAL_INTG_ERR` NMI and transitioning OTBN directly from
  //      `STATUS_PAUSED` (`0x05`) to `STATUS_LOCKED` (`0xFF`) with
  //      `ERR_BITS.ILLEGAL_BUS_ACCESS = 1` and
  //      `FATAL_ALERT_CAUSE.ILLEGAL_BUS_ACCESS = 1`.
  //    - In `STATUS_LOCKED` (`0xFF`), verify 32-bit host writes still advance
  //      `LOAD_CHECKSUM`, and `INSN_CNT` & `ERR_BITS` ARE writable and clear to
  //      `0` (`is_not_running_q == 1` in `STATUS_LOCKED`), while
  //      `FATAL_ALERT_CAUSE` stays sticky.
  // -------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [NEW IN V2 + V1: otbn.sv:169-175, 451, 705, 788-792]: "
      "STATUS_PAUSED (0x05) DMEM write LOAD_CHECKSUM bypass, INSN_CNT write "
      "lockout, and IMEM read Ibex Integrity NMI -> STATUS_LOCKED (0xFF)...");
  CHECK_DIF_OK(dif_otbn_set_ctrl_wfi_enable(&otbn, true));
  abs_mmio_write32(kOtbnBase + OTBN_INSN_CNT_REG_OFFSET, 0u);
  const uint32_t prog_wfi_pause[] = {
      OTBN_ADDI(2, 0, 0x42),  // insn 1: x2 = 0x42
      OTBN_SW(2, 0, 0),       // insn 2: DMEM[0] = 0x42
      OTBN_WFI(),             // insn 3: pause in STATUS_PAUSED (0x05)
      OTBN_ADDI(2, 2, 1),     // insn 4: x2 = 0x43
      OTBN_SW(2, 0, 4),       // insn 5: DMEM[4] = 0x43
      OTBN_ECALL(),           // insn 6: finish
  };
  for (size_t i = 0; i < ARRAYSIZE(prog_wfi_pause); ++i) {
    abs_mmio_write32(kOtbnBase + OTBN_IMEM_REG_OFFSET + i * sizeof(uint32_t),
                     prog_wfi_pause[i]);
  }
  abs_mmio_write32(kOtbnBase + OTBN_INTR_STATE_REG_OFFSET, 1u);
  abs_mmio_write32(kOtbnBase + OTBN_CMD_REG_OFFSET, kCmdExecute);
  otbn_wait_for_settled_status();

  CHECK(abs_mmio_read32(kOtbnBase + OTBN_STATUS_REG_OFFSET) == kStatusPaused,
        "Expected STATUS=0x05 (PAUSED) on WFI");
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_INTR_STATE_REG_OFFSET) == 1u,
        "Expected INTR_STATE.done=1 on WFI transition to STATUS_PAUSED");
  uint32_t paused_insn_cnt =
      abs_mmio_read32(kOtbnBase + OTBN_INSN_CNT_REG_OFFSET);
  CHECK(paused_insn_cnt == 2u, "Expected INSN_CNT=2 before WFI pause, got %u",
        paused_insn_cnt);

  // Attempt to clear INSN_CNT while STATUS == PAUSED (0x05): ignored because
  // is_not_running_d includes wfi_pending (otbn.sv:171-175).
  abs_mmio_write32(kOtbnBase + OTBN_INSN_CNT_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_INSN_CNT_REG_OFFSET) == 2u,
        "Host write to INSN_CNT in STATUS_PAUSED must be ignored");

  // Clear LOAD_CHECKSUM, read DMEM[0] (0x42), and write 0x99887766 to DMEM[0]
  // while STATUS == PAUSED (0x05): because imem_access_core == 1 (wfi_pending),
  // mem_crc_data_in_valid = ~(dmem_access_core | imem_access_core) & ... is 0,
  // so DMEM[0] is updated to 0x99887766 while LOAD_CHECKSUM remains 0!
  abs_mmio_write32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET, 0u);
  uint32_t paused_dmem0 = abs_mmio_read32(kOtbnBase + OTBN_DMEM_REG_OFFSET);
  CHECK(paused_dmem0 == 0x42u, "Expected DMEM[0]=0x42 while paused, got 0x%x",
        paused_dmem0);
  abs_mmio_write32(kOtbnBase + OTBN_DMEM_REG_OFFSET, 0x99887766u);
  uint32_t paused_dmem0_after_wr =
      abs_mmio_read32(kOtbnBase + OTBN_DMEM_REG_OFFSET);
  uint32_t paused_crc =
      abs_mmio_read32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET);
  CHECK(paused_dmem0_after_wr == 0x99887766u,
        "Expected DMEM[0]=0x99887766 after host write in STATUS_PAUSED, got "
        "0x%08x",
        paused_dmem0_after_wr);
  CHECK(paused_crc == 0u,
        "Host DMEM[0] write in STATUS_PAUSED bypassed LOAD_CHECKSUM: expected "
        "0x00000000 (imem_access_core=1 disables mem_crc_data_in_valid), got "
        "0x%08x",
        paused_crc);

  // Resume from WFI via CMD.RESUME (0xa6) and verify normal completion.
  abs_mmio_write32(kOtbnBase + OTBN_INTR_STATE_REG_OFFSET, 1u);
  abs_mmio_write32(kOtbnBase + OTBN_CMD_REG_OFFSET, kCmdResume);
  otbn_wait_for_settled_status();
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_STATUS_REG_OFFSET) == kStatusIdle);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_DMEM_REG_OFFSET) == 0x99887766u);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_DMEM_REG_OFFSET + 4u) == 0x43u);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_INSN_CNT_REG_OFFSET) == 6u);

  // Re-enter STATUS_PAUSED (0x05) and perform an illegal host IMEM[0] read.
  abs_mmio_write32(kOtbnBase + OTBN_INTR_STATE_REG_OFFSET, 1u);
  abs_mmio_write32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET, 0u);
  abs_mmio_write32(kOtbnBase + OTBN_CMD_REG_OFFSET, kCmdExecute);
  otbn_wait_for_settled_status();
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_STATUS_REG_OFFSET) == kStatusPaused);

  g_load_integrity_nmi_seen = false;
  (void)abs_mmio_read32(kOtbnBase + OTBN_IMEM_REG_OFFSET);
  otbn_wait_for_settled_status();
  for (volatile int i = 0; i < 500; ++i) {
  }

  CHECK(g_load_integrity_nmi_seen,
        "Illegal IMEM read during STATUS_PAUSED must raise Ibex "
        "FATAL_INTG_ERR NMI");
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_STATUS_REG_OFFSET) == kStatusLocked);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_ERR_BITS_REG_OFFSET) ==
        (1u << OTBN_ERR_BITS_ILLEGAL_BUS_ACCESS_BIT));
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_FATAL_ALERT_CAUSE_REG_OFFSET) ==
        (1u << OTBN_FATAL_ALERT_CAUSE_ILLEGAL_BUS_ACCESS_BIT));

  // Verify [otbn.sv:788-801]: 32-bit writes in STATUS_LOCKED advance
  // LOAD_CHECKSUM even though SRAM writes are blocked by `locking`.
  abs_mmio_write32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET, 0u);
  abs_mmio_write32(kOtbnBase + OTBN_DMEM_REG_OFFSET, 0x12345678u);
  abs_mmio_write32(kOtbnBase + OTBN_IMEM_REG_OFFSET, 0x12345678u);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET) ==
        expected_crc_12345678);

  // Verify [otbn.sv:169-175, 997-1066]: unlike STATUS_PAUSED, in STATUS_LOCKED
  // (0xFF) is_not_running_q is 1, so INTR_STATE, INSN_CNT, and ERR_BITS are
  // writable and clear to 0, while FATAL_ALERT_CAUSE remains sticky.
  abs_mmio_write32(kOtbnBase + OTBN_INTR_STATE_REG_OFFSET, 1u);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_INTR_STATE_REG_OFFSET) == 0u);

  abs_mmio_write32(kOtbnBase + OTBN_INSN_CNT_REG_OFFSET, 0xffffffffu);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_INSN_CNT_REG_OFFSET) == 0u);

  abs_mmio_write32(kOtbnBase + OTBN_ERR_BITS_REG_OFFSET, 0xffffffffu);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_ERR_BITS_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_FATAL_ALERT_CAUSE_REG_OFFSET) ==
        (1u << OTBN_FATAL_ALERT_CAUSE_ILLEGAL_BUS_ACCESS_BIT));

  LOG_INFO("All Earlgrey v2 otbn errata checks confirmed on CW340 FPGA!");
  return true;
}
