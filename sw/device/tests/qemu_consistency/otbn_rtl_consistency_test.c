// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "otbn_regs.h"

OTTF_DEFINE_TEST_CONFIG(.ignore_alerts = true);

enum {
  kOtbnBase = TOP_EARLGREY_OTBN_BASE_ADDR,
  kCmdExecute = 0xd8u,
  kStatusIdle = 0x00u,
  kStatusBusyExecute = 0x01u,
  kStatusLocked = 0xffu,
};

#define EXPECT_RTL(cond, ...)                  \
  do {                                         \
    if (!(cond)) {                             \
      LOG_ERROR("RTL_MISMATCH: " __VA_ARGS__); \
      all_ok = false;                          \
    }                                          \
  } while (0)

// Base & Bignum OTBN instruction encoders matching hw/ip/otbn/rtl/otbn_decoder.sv
#define OTBN_ECALL() (0x00000073u)
#define OTBN_NOP() (0x00000013u)
#define OTBN_ADDI(rd, rs1, imm12)                                \
  ((((uint32_t)(imm12) & 0xfffu) << 20) |                        \
   (((uint32_t)(rs1) & 0x1fu) << 15) | (0x0u << 12) |            \
   (((uint32_t)(rd) & 0x1fu) << 7) | 0x13u)
#define OTBN_ADD(rd, rs1, rs2)                                   \
  ((((uint32_t)(rs2) & 0x1fu) << 20) |                           \
   (((uint32_t)(rs1) & 0x1fu) << 15) | (0x0u << 12) |            \
   (((uint32_t)(rd) & 0x1fu) << 7) | 0x33u)
#define OTBN_SW(rs2, rs1, imm12)                                 \
  (((((uint32_t)(imm12) >> 5) & 0x7fu) << 25) |                  \
   (((uint32_t)(rs2) & 0x1fu) << 20) |                           \
   (((uint32_t)(rs1) & 0x1fu) << 15) | (0x2u << 12) |            \
   (((uint32_t)(imm12) & 0x1fu) << 7) | 0x23u)
#define OTBN_JAL_PLUS4(rd) (0x0040006fu | (((uint32_t)(rd) & 0x1fu) << 7))
#define OTBN_BNE_PLUS4(rs1, rs2)                                 \
  ((((uint32_t)(rs2) & 0x1fu) << 20) |                           \
   (((uint32_t)(rs1) & 0x1fu) << 15) | (0x1u << 12) |            \
   (0x4u << 7) | 0x63u)
#define OTBN_LOOPI(iters, bodysize_minus_1)                      \
  ((((uint32_t)(bodysize_minus_1) & 0xfffu) << 20) |             \
   ((((uint32_t)(iters) >> 5) & 0x1fu) << 15) | (0x1u << 12) |   \
   (((uint32_t)(iters) & 0x1fu) << 7) | 0x7bu)
#define OTBN_BN_XOR(wrd, wrs1, wrs2)                             \
  ((((uint32_t)(wrs2) & 0x1fu) << 20) |                          \
   (((uint32_t)(wrs1) & 0x1fu) << 15) | (0x6u << 12) |           \
   (((uint32_t)(wrd) & 0x1fu) << 7) | 0x7bu)
#define OTBN_BN_ADDI(wrd, wrs1, imm10)                           \
  ((((uint32_t)(imm10) & 0x3ffu) << 20) |                        \
   (((uint32_t)(wrs1) & 0x1fu) << 15) | (0x4u << 12) |           \
   (((uint32_t)(wrd) & 0x1fu) << 7) | 0x2bu)
#define OTBN_BN_ADDM(wrd, wrs1, wrs2)                            \
  ((((uint32_t)(wrs2) & 0x1fu) << 20) |                          \
   (((uint32_t)(wrs1) & 0x1fu) << 15) | (0x5u << 12) |           \
   (((uint32_t)(wrd) & 0x1fu) << 7) | 0x2bu)
#define OTBN_BN_WSRW(wsr, wrs1)                                  \
  ((1u << 31) | (((uint32_t)(wsr) & 0xffu) << 20) |              \
   (((uint32_t)(wrs1) & 0x1fu) << 15) | (0x7u << 12) | 0x0bu)
#define OTBN_BN_SID_ZERO_OFF(grs2, grs1)                         \
  ((((uint32_t)(grs2) & 0x1fu) << 20) |                          \
   (((uint32_t)(grs1) & 0x1fu) << 15) | (0x5u << 12) | 0x0bu)

static void otbn_wait_for_not_running(void) {
  while (true) {
    uint32_t status = abs_mmio_read32(kOtbnBase + OTBN_STATUS_REG_OFFSET);
    if (status == kStatusIdle || status == kStatusLocked) {
      break;
    }
  }
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
  bool all_ok = true;
  otbn_wait_for_not_running();
  // Ensure CTRL.software_errs_fatal is 0 (ROM sets it to 1 during sigverify).
  abs_mmio_write32(kOtbnBase + OTBN_CTRL_REG_OFFSET, 0u);

  // 1. INSN_CNT write-any-clears-to-zero semantics (otbn.sv:988-996).
  // Execute a 1-instruction program (ECALL) so INSN_CNT == 1, then write
  // 0xffffffff to INSN_CNT while idle. RTL ignores write data and zeroes
  // INSN_CNT.
  const uint32_t prog_ecall[] = {OTBN_ECALL()};
  otbn_run_program(prog_ecall, ARRAYSIZE(prog_ecall));
  uint32_t insn_cnt = abs_mmio_read32(kOtbnBase + OTBN_INSN_CNT_REG_OFFSET);
  EXPECT_RTL(insn_cnt == 1u, "Expected INSN_CNT == 1 after ECALL, got %u",
             insn_cnt);
  abs_mmio_write32(kOtbnBase + OTBN_INSN_CNT_REG_OFFSET, 0xffffffffu);
  insn_cnt = abs_mmio_read32(kOtbnBase + OTBN_INSN_CNT_REG_OFFSET);
  EXPECT_RTL(insn_cnt == 0u,
             "INSN_CNT write of 0xffffffff must clear INSN_CNT to 0 (got "
             "0x%08x)",
             insn_cnt);

  // 2. ERR_BITS write-any-clears-to-zero semantics (otbn.sv:926-957).
  // Trigger a recoverable CALL_STACK underflow error (`add x2, x1, x0`), then
  // write 0xffffffff to ERR_BITS while idle. RTL ignores write data and clears
  // ERR_BITS to 0.
  const uint32_t prog_callstack_err[] = {
      OTBN_ADD(2, 1, 0),
      OTBN_ECALL(),
  };
  otbn_run_program(prog_callstack_err, ARRAYSIZE(prog_callstack_err));
  uint32_t err_bits = abs_mmio_read32(kOtbnBase + OTBN_ERR_BITS_REG_OFFSET);
  EXPECT_RTL(err_bits == (1u << OTBN_ERR_BITS_CALL_STACK_BIT),
             "Expected ERR_BITS == CALL_STACK (0x4), got 0x%08x", err_bits);
  abs_mmio_write32(kOtbnBase + OTBN_ERR_BITS_REG_OFFSET, 0xffffffffu);
  err_bits = abs_mmio_read32(kOtbnBase + OTBN_ERR_BITS_REG_OFFSET);
  EXPECT_RTL(err_bits == 0u,
             "ERR_BITS write of 0xffffffff must clear ERR_BITS to 0 (got "
             "0x%08x)",
             err_bits);
  // Restore clean ERR_BITS = 0 in case QEMU stored 0xffffffff.
  abs_mmio_write32(kOtbnBase + OTBN_ERR_BITS_REG_OFFSET, 0u);

  // 3. BN.ADDM boundary condition when a + b == MOD (otbn_alu_bignum.sv:905).
  // When MOD = 100, w2 = 60, w3 = 40, `bn.addm w4, w2, w3` must subtract MOD
  // and produce 0 (since a + b >= MOD).
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
  EXPECT_RTL(addm_res == 0u,
             "BN.ADDM with a + b == MOD (60 + 40 == 100) must return 0, got %u",
             addm_res);

  // 4. Call stack (x1) simultaneous pop+push & dual-read port semantics
  // (otbn_rf_base.sv:101-135, otbn_stack.sv:8-15).
  // Push one entry onto x1 (`jal x1, +4` at PC=0 pushes 4), then execute
  // `addi x1, x1, 10` (pop 4 ordered before push 14 -> top of stack is 14),
  // then `add x2, x1, x1` (both read ports read 14 and pop ONCE at commit ->
  // x2 = 28, ERR_BITS == 0).
  abs_mmio_write32(kOtbnBase + OTBN_DMEM_REG_OFFSET, 0u);
  const uint32_t prog_x1_dual_read[] = {
      OTBN_JAL_PLUS4(1),   // PC=0: pushes 4 onto x1 call stack, jumps to PC=4
      OTBN_ADDI(1, 1, 10), // PC=4: pops 4 and pushes 14 onto x1 call stack
      OTBN_ADD(2, 1, 1),   // PC=8: reads x1 on both ports (14 + 14 = 28), pops once
      OTBN_SW(2, 0, 0),    // DMEM[0] = x2
      OTBN_ECALL(),
  };
  otbn_run_program(prog_x1_dual_read, ARRAYSIZE(prog_x1_dual_read));
  err_bits = abs_mmio_read32(kOtbnBase + OTBN_ERR_BITS_REG_OFFSET);
  uint32_t x2_val = abs_mmio_read32(kOtbnBase + OTBN_DMEM_REG_OFFSET);
  EXPECT_RTL(err_bits == 0u && x2_val == 28u,
             "Simultaneous pop+push `addi x1, x1, 10` and dual read `add x2, "
             "x1, x1` with 1 stack entry must yield x2=28 and ERR_BITS=0 "
             "(got x2=%u, ERR_BITS=0x%08x)",
             x2_val, err_bits);

  // 5a. LOOPI at the end instruction of an outer loop (otbn_loop_controller.sv:162).
  // Outer `loopi 2, 0` has body size 1 (end instruction at PC=4). Placing
  // `loopi 1, 0` at PC=4 must trigger `loop_at_end_err` (ERR_BITS.LOOP).
  const uint32_t prog_loopi_at_end[] = {
      OTBN_LOOPI(2, 0),  // PC=0: loop 2 times, body size = 1 insn (end = PC=4)
      OTBN_LOOPI(1, 0),  // PC=4: LOOPI at outer loop end -> ERR_BITS.LOOP
      OTBN_NOP(),
      OTBN_ECALL(),
  };
  otbn_run_program(prog_loopi_at_end, ARRAYSIZE(prog_loopi_at_end));
  err_bits = abs_mmio_read32(kOtbnBase + OTBN_ERR_BITS_REG_OFFSET);
  EXPECT_RTL(err_bits == (1u << OTBN_ERR_BITS_LOOP_BIT),
             "LOOPI at end of active outer loop must set ERR_BITS.LOOP "
             "(expected 0x10, got 0x%08x)",
             err_bits);

  // 5b. Jump (JAL/JALR) at the end instruction of an active loop
  // (otbn_loop_controller.sv:160, otbn_controller.sv:402).
  // `loopi 2, 0` has end instruction at PC=4. Placing `jal x0, +4` at PC=4
  // must trigger `loop_branch_err` (ERR_BITS.LOOP).
  const uint32_t prog_jump_at_end[] = {
      OTBN_LOOPI(2, 0),    // PC=0: loop 2 times, body size = 1 insn (end = PC=4)
      OTBN_JAL_PLUS4(0),   // PC=4: JAL at loop end -> ERR_BITS.LOOP
      OTBN_ECALL(),
  };
  otbn_run_program(prog_jump_at_end, ARRAYSIZE(prog_jump_at_end));
  err_bits = abs_mmio_read32(kOtbnBase + OTBN_ERR_BITS_REG_OFFSET);
  EXPECT_RTL(err_bits == (1u << OTBN_ERR_BITS_LOOP_BIT),
             "JAL at end of active loop must set ERR_BITS.LOOP "
             "(expected 0x10, got 0x%08x)",
             err_bits);

  // 6. Host IMEM read during BUSY_EXECUTE triggers ILLEGAL_BUS_ACCESS, locks
  // OTBN (STATUS == 0xff), latches FATAL_ALERT_CAUSE.ILLEGAL_BUS_ACCESS, and
  // still permits RW1C/RW/clear writes to INTR_STATE, LOAD_CHECKSUM, INSN_CNT,
  // and ERR_BITS while STATUS == LOCKED (otbn.sv:666, 926-996, 1168-1213).
  const uint32_t prog_long_loop[] = {
      OTBN_LOOPI(500, 0),
      OTBN_NOP(),
      OTBN_ECALL(),
  };
  for (size_t i = 0; i < ARRAYSIZE(prog_long_loop); ++i) {
    abs_mmio_write32(kOtbnBase + OTBN_IMEM_REG_OFFSET + i * sizeof(uint32_t),
                     prog_long_loop[i]);
  }
  abs_mmio_write32(kOtbnBase + OTBN_INTR_STATE_REG_OFFSET, 1u);
  abs_mmio_write32(kOtbnBase + OTBN_CMD_REG_OFFSET, kCmdExecute);

  // Wait for OTBN to finish pre-start URND refresh and enter Running state.
  for (int i = 0;
       i < 100 && abs_mmio_read32(kOtbnBase + OTBN_INSN_CNT_REG_OFFSET) == 0u;
       ++i) {
  }

  // Write IMEM while OTBN is in BUSY_EXECUTE (a read returns blanked 39'b0
  // without ZeroEcc before locking_q asserts, causing an Ibex load integrity
  // fault, whereas a write triggers imem_illegal_bus_access cleanly).
  abs_mmio_write32(kOtbnBase + OTBN_IMEM_REG_OFFSET, 0u);
  otbn_wait_for_not_running();
  // Wait for the 2-round post-lock internal secure wipe (~130 cycles) to finish
  // so `is_not_running_q` asserts in StatusLocked.
  for (volatile int i = 0; i < 500; ++i) {
  }

  uint32_t status = abs_mmio_read32(kOtbnBase + OTBN_STATUS_REG_OFFSET);
  err_bits = abs_mmio_read32(kOtbnBase + OTBN_ERR_BITS_REG_OFFSET);
  uint32_t fatal_cause =
      abs_mmio_read32(kOtbnBase + OTBN_FATAL_ALERT_CAUSE_REG_OFFSET);
  uint32_t intr_state = abs_mmio_read32(kOtbnBase + OTBN_INTR_STATE_REG_OFFSET);

  EXPECT_RTL(status == kStatusLocked,
             "Host IMEM access during BUSY_EXECUTE must lock OTBN "
             "(expected STATUS=0xff, got 0x%08x)",
             status);
  EXPECT_RTL(err_bits == (1u << OTBN_ERR_BITS_ILLEGAL_BUS_ACCESS_BIT),
             "Expected ERR_BITS.ILLEGAL_BUS_ACCESS (0x%08x), got 0x%08x",
             1u << OTBN_ERR_BITS_ILLEGAL_BUS_ACCESS_BIT, err_bits);
  EXPECT_RTL(
      fatal_cause == (1u << OTBN_FATAL_ALERT_CAUSE_ILLEGAL_BUS_ACCESS_BIT),
      "Expected FATAL_ALERT_CAUSE.ILLEGAL_BUS_ACCESS (0x%08x), got 0x%08x",
      1u << OTBN_FATAL_ALERT_CAUSE_ILLEGAL_BUS_ACCESS_BIT, fatal_cause);
  EXPECT_RTL(intr_state == 1u,
             "Expected INTR_STATE.done == 1 on lock completion, got 0x%08x",
             intr_state);

  // Verify INTR_STATE (RW1C), ERR_BITS (clear), and INSN_CNT (clear) remain
  // writable when STATUS == LOCKED, while FATAL_ALERT_CAUSE stays sticky.
  abs_mmio_write32(kOtbnBase + OTBN_INTR_STATE_REG_OFFSET, 1u);
  intr_state = abs_mmio_read32(kOtbnBase + OTBN_INTR_STATE_REG_OFFSET);
  EXPECT_RTL(intr_state == 0u,
             "INTR_STATE must be clearable (RW1C) when STATUS == LOCKED "
             "(got 0x%08x)",
             intr_state);

  abs_mmio_write32(kOtbnBase + OTBN_INSN_CNT_REG_OFFSET, 0xffffffffu);
  insn_cnt = abs_mmio_read32(kOtbnBase + OTBN_INSN_CNT_REG_OFFSET);
  EXPECT_RTL(insn_cnt == 0u,
             "INSN_CNT must be clearable when STATUS == LOCKED (got 0x%08x)",
             insn_cnt);

  abs_mmio_write32(kOtbnBase + OTBN_ERR_BITS_REG_OFFSET, 0xffffffffu);
  err_bits = abs_mmio_read32(kOtbnBase + OTBN_ERR_BITS_REG_OFFSET);
  fatal_cause = abs_mmio_read32(kOtbnBase + OTBN_FATAL_ALERT_CAUSE_REG_OFFSET);
  EXPECT_RTL(err_bits == 0u,
             "ERR_BITS must be clearable when STATUS == LOCKED (got 0x%08x)",
             err_bits);
  EXPECT_RTL(
      fatal_cause == (1u << OTBN_FATAL_ALERT_CAUSE_ILLEGAL_BUS_ACCESS_BIT),
      "FATAL_ALERT_CAUSE must remain sticky after clearing ERR_BITS "
      "(got 0x%08x)",
      fatal_cause);

  return all_ok;
}
