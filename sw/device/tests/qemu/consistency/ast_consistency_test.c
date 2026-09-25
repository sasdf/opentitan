// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file ast_errata_v2_test.c
 * @brief Physical CW340 FPGA verification test for Earlgrey v2 (`trunk-v2`)
 *        `ast` errata:
 *   - (CONFIRMED_PRESENT_ON_V2):
 *     `REGA0` (`0x00`), `REGA1` (`0x04`), and `REGA28` (`0x70`) are described
 *     as `"for OTP/ROM Write Testing"` yet have `SwAccessRO` (`we = 0`) and
 *     always read back `0x00`, `0x01`, and `0x1c`, while `REGAL` (`0xd4` in v2,
 *     `resval: 0x35`) has `assign hw2reg.regal.d = regal;` in `ast_main.sv:154`
 *     yet `u_regal.qs()` is left unconnected (`SwAccessWO`), so `REGAL`
 * (`0xd4`) always reads back `0x00000000` while asserting `ast_init_done_o =
 *     MuBi4True` (`SENSOR_CTRL.STATUS.AST_INIT_DONE = 1`).
 *   - (CONFIRMED_PRESENT_ON_V2):
 *     `AST_PERMIT[59]` is `4'b1111` for all 59 AST registers (`REGA0..REGA52`,
 *     `REGAL` at `0xd4`, `REGB0..REGB4` at `0x200..0x210`), rejecting any
 *     sub-word write (`sb` or `sh`, even to `REGA0` or `REGAL`) with a
 *     synchronous Store Access Fault (`mcause = 7`), and unmapped holes
 *     `0xd8..0x1fc` and `0x214..0x3fc` raise `addrmiss` (`mcause = 5` / `7`).
 *   - (NEW_IN_V2):
 *     In `trunk-v2`, `REGA` expanded by 15 registers (`REGA38..REGA52` at
 *     `0x98..0xd0`, `rw`, `resval = 0x26..0x34`) and `REGAL` shifted from
 *     `0x98` (`resval = 0x26`) to `0xd4` (`resval = 0x35`). Offset `0x98`
 *     (formerly `REGAL` in v1, which read `0x0` as `wo`) is now `REGA38` (`rw`,
 *     storing full 32-bit values), while `REGAL` at `0xd4` still leaves
 *     `hw2reg.regal.d` (`ast_main.sv:154`) dead and unconnected in
 *     `ast_reg_top.sv` (`addr_hit[53]` hardwired to `'0`).
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top/ast_regs.h"
#include "hw/top/sensor_ctrl_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG(.ignore_alerts = true);

enum {
  kAstBase = TOP_EARLGREY_AST_BASE_ADDR,
  kSensorCtrlBase = TOP_EARLGREY_SENSOR_CTRL_BASE_ADDR,
};

static volatile uint32_t g_fault_count = 0;
static volatile uint32_t g_last_mcause = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = ibex_mcause_read();
  g_last_mcause = mcause;
  g_fault_count++;

  uint32_t mepc;
  CSR_READ(CSR_REG_MEPC, &mepc);
  uint16_t insn_first_half = *(const volatile uint16_t *)mepc;
  uint32_t step = ((insn_first_half & 0x3u) != 0x3u) ? 2u : 4u;
  CSR_WRITE(CSR_REG_MEPC, mepc + step);
}

static inline void mmio_write8(uint32_t addr, uint8_t val) {
  asm volatile("sb %0, 0(%1)" : : "r"(val), "r"(addr) : "memory");
}

static inline void mmio_write16(uint32_t addr, uint16_t val) {
  asm volatile("sh %0, 0(%1)" : : "r"(val), "r"(addr) : "memory");
}

static void test_v1_001_and_v2_001_rega_ro_and_regal_dead_readback(void) {
  LOG_INFO(
      "Testing AST SwAccessRO REGA0/1/28, v2 REGA38..52/REGB0..4 RW, and REGAL "
      "readback...");

  // 1. REGA0 (0x00), REGA1 (0x04), and REGA28 (0x70) are read-only (SwAccessRO)
  // and silently ignore 32-bit writes, returning 0x00, 0x01, and 0x1c.
  abs_mmio_write32(kAstBase + AST_REGA0_REG_OFFSET, 0xa5a5a5a5u);
  abs_mmio_write32(kAstBase + AST_REGA1_REG_OFFSET, 0x5a5a5a5au);
  abs_mmio_write32(kAstBase + AST_REGA28_REG_OFFSET, 0xdeadbeefu);

  CHECK(abs_mmio_read32(kAstBase + AST_REGA0_REG_OFFSET) == 0x00u,
        "Expected REGA0 to remain 0x00 (SwAccessRO)");
  CHECK(abs_mmio_read32(kAstBase + AST_REGA1_REG_OFFSET) == 0x01u,
        "Expected REGA1 to remain 0x01 (SwAccessRO)");
  CHECK(abs_mmio_read32(kAstBase + AST_REGA28_REG_OFFSET) == 0x1cu,
        "Expected REGA28 to remain 0x1c (SwAccessRO)");

  // 2. New v2 REGA38..REGA52 (0x98..0xd0, 15 registers) and REGB_0..REGB_4
  // (0x200..0x210, 5 registers) are full 32-bit RW registers!
  // Note offset 0x98 was REGAL (WO, reading 0) in v1, and is now REGA38 (RW).
  CHECK(AST_REGA38_REG_OFFSET == 0x98u, "Expected REGA38 at 0x98");
  CHECK(AST_REGA52_REG_OFFSET == 0xd0u, "Expected REGA52 at 0xd0");
  CHECK(AST_REGAL_REG_OFFSET == 0xd4u, "Expected REGAL at 0xd4 in trunk-v2");

  for (uint32_t i = 0u; i < 15u; ++i) {
    uint32_t off = AST_REGA38_REG_OFFSET + i * sizeof(uint32_t);
    uint32_t orig = abs_mmio_read32(kAstBase + off);
    uint32_t test_val = 0x12345670u + i;
    abs_mmio_write32(kAstBase + off, test_val);
    CHECK(abs_mmio_read32(kAstBase + off) == test_val,
          "Expected REGA%u (0x%x) to read back 32-bit RW value", 38u + i, off);
    abs_mmio_write32(kAstBase + off, orig);
  }

  for (uint32_t i = 0u; i < 5u; ++i) {
    uint32_t off = AST_REGB_0_REG_OFFSET + i * sizeof(uint32_t);
    uint32_t orig = abs_mmio_read32(kAstBase + off);
    uint32_t test_val = 0x89abcde0u + i;
    abs_mmio_write32(kAstBase + off, test_val);
    CHECK(abs_mmio_read32(kAstBase + off) == test_val,
          "Expected REGB_%u (0x%x) to read back 32-bit RW value", i, off);
    abs_mmio_write32(kAstBase + off, orig);
  }

  // 3. REGAL (0xd4) has hw2reg.regal.d = regal in ast_main.sv:154, yet
  // u_regal.qs() is unconnected (SwAccessWO), so REGAL always reads 0x00000000
  // while asserting ast_init_done_o = MuBi4True!
  abs_mmio_write32(kAstBase + AST_REGAL_REG_OFFSET, 0xcafebabeu);
  uint32_t regal_rb = abs_mmio_read32(kAstBase + AST_REGAL_REG_OFFSET);
  CHECK(regal_rb == 0x00000000u,
        "Expected REGAL (0xd4) readback to be 0x0 due to disconnected qs, got "
        "0x%x",
        regal_rb);

  uint32_t sc_status =
      abs_mmio_read32(kSensorCtrlBase + SENSOR_CTRL_STATUS_REG_OFFSET);
  CHECK(bitfield_bit32_read(sc_status, SENSOR_CTRL_STATUS_AST_INIT_DONE_BIT),
        "Expected SENSOR_CTRL.STATUS.AST_INIT_DONE == 1 after writing REGAL");
}

static void test_v1_002_permit_subword_and_addrmiss_holes(void) {
  LOG_INFO(
      "Testing AST_PERMIT[59]=4'b1111 sub-word faults & unmapped holes...");

  // 1. Sub-word writes (sb/sh) to all 59 AST registers (REGA0..REGA52 at
  // 0x00..0xd0, REGAL at 0xd4, and REGB0..REGB4 at 0x200..0x210) fault with
  // mcause = 7 (AST_PERMIT = 4'b1111 for all 59 registers) and block we.
  uint32_t orig_rega2 = abs_mmio_read32(kAstBase + AST_REGA2_REG_OFFSET);
  uint32_t orig_regb0 = abs_mmio_read32(kAstBase + AST_REGB_0_REG_OFFSET);
  abs_mmio_write32(kAstBase + AST_REGA2_REG_OFFSET, 0x55aa55aau);
  abs_mmio_write32(kAstBase + AST_REGB_0_REG_OFFSET, 0xaa55aa55u);

  for (uint32_t idx = 0u; idx < 59u; ++idx) {
    uint32_t off =
        (idx < 54u) ? (idx * sizeof(uint32_t))
                    : (AST_REGB_0_REG_OFFSET + (idx - 54u) * sizeof(uint32_t));
    g_fault_count = 0;
    g_last_mcause = 0;
    mmio_write8(kAstBase + off, 0x11u);
    CHECK(g_fault_count == 1u && g_last_mcause == 7u,
          "Expected sb to AST offset 0x%x to fault with mcause=7", off);

    g_fault_count = 0;
    g_last_mcause = 0;
    mmio_write16(kAstBase + off, 0x2233u);
    CHECK(g_fault_count == 1u && g_last_mcause == 7u,
          "Expected sh to AST offset 0x%x to fault with mcause=7", off);
  }

  CHECK(abs_mmio_read32(kAstBase + AST_REGA2_REG_OFFSET) == 0x55aa55aau,
        "Faulting sb/sh must block write-enable on REGA2");
  CHECK(abs_mmio_read32(kAstBase + AST_REGB_0_REG_OFFSET) == 0xaa55aa55u,
        "Faulting sb/sh must block write-enable on REGB_0");
  abs_mmio_write32(kAstBase + AST_REGA2_REG_OFFSET, orig_rega2);
  abs_mmio_write32(kAstBase + AST_REGB_0_REG_OFFSET, orig_regb0);

  // 2. Unmapped aperture holes 0xd8..0x1fc (between REGAL at 0xd4 and REGB0 at
  // 0x200) and 0x214..0x3fc (after REGB4 at 0x210) raise addrmiss faults.
  const uint32_t kHoleOffsets[] = {0xd8u, 0x1fcu, 0x214u, 0x3fcu};
  for (size_t i = 0; i < 4u; ++i) {
    g_fault_count = 0;
    g_last_mcause = 0;
    (void)abs_mmio_read32(kAstBase + kHoleOffsets[i]);
    CHECK(g_fault_count == 1u && g_last_mcause == 5u,
          "Expected lw at AST hole 0x%x to fault with mcause=5",
          kHoleOffsets[i]);

    g_fault_count = 0;
    g_last_mcause = 0;
    abs_mmio_write32(kAstBase + kHoleOffsets[i], 0u);
    CHECK(g_fault_count == 1u && g_last_mcause == 7u,
          "Expected sw at AST hole 0x%x to fault with mcause=7",
          kHoleOffsets[i]);
  }
}

bool test_main(void) {
  LOG_INFO("=== AST Earlgrey v2 (trunk-v2) Errata Verification Test ===");
  test_v1_001_and_v2_001_rega_ro_and_regal_dead_readback();
  test_v1_002_permit_subword_and_addrmiss_holes();
  LOG_INFO("=== ALL AST Earlgrey v2 Errata Checks PASSED on CW340 FPGA! ===");
  return true;
}
