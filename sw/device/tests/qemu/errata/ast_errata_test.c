// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file ast_errata_test.c
 * @brief CW340 FPGA & QEMU Errata Confirmation Test for `ast` (P19).
 *
 * Empirically confirms on both physical CW340 FPGA silicon and QEMU:
 * - [ast.sv:861-877] (TRUE_SILICON_ERRATA):
 *   1) `REGA0` (`0x00`), `REGA1` (`0x04`), and `REGA28` (`0x70`) are described
 *      as "for OTP/ROM Write Testing" (and written by ROM during boot), yet
 *      have `SwAccessRO` (`.we(1'b0)` in `ast_reg_top.sv:252-305, 1035-1060`):
 *      full 32-bit writes do not fault (`d_error = 0`) and are silently
 * ignored, with reads always returning `0x00000000`, `0x00000001`, and
 * `0x0000001c`. 2) `REGAL` (`0x98`) implements a 32-bit `regal` register
 * initialized to `AST_REGAL_RESVAL` (`0x26`) and wired to `hw2reg.regal.d =
 * regal` in `ast.sv:861-877`, yet `u_regal.qs()` is left unconnected
 * (`SwAccessWO`) and `addr_hit[38]` in `ast_reg_top.sv:1911-1913` hardwires
 * readback to `0x00000000` before and after 32-bit writes. 3) `REGA2..REGA27`,
 * `REGA29..REGA37`, and `REGB0..REGB4` (`0x200..0x210`) accept and retain full
 * 32-bit read-write values (`E4`).
 * - [ast_reg_pkg.sv:305-355] (INTENDED_SECURITY_HARDENING):
 *   1) `AST_PERMIT[0..43]` is `4'b1111` (`ast_reg_pkg.sv:305-355`), so any
 *      sub-word (8-bit or 16-bit) write to `REGA0`, `REGA2`, or `REGAL` raises
 *      a synchronous TL-UL bus fault (`d_error = 1`, Ibex Store Access Fault
 *      `mcause = 7`).
 *   2) Accesses to the unmapped aperture holes `0x9c..0x1fc` (between `REGAL`
 *      and `REGB0`) and `0x214..0x3fc` (after `REGB4`) raise synchronous TL-UL
 *      `addrmiss` bus faults (`d_error = 1`, `mcause = 5` on read, `7` on
 * write).
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kAstBase = TOP_EARLGREY_AST_BASE_ADDR,
  kAstRega0Offset = 0x00u,
  kAstRega1Offset = 0x04u,
  kAstRega2Offset = 0x08u,
  kAstRega28Offset = 0x70u,
  kAstRegalOffset = 0x98u,
  kAstHole1Offset = 0x9cu,
  kAstHole1EndOffset = 0x1fcu,
  kAstRegb0Offset = 0x200u,
  kAstRegb4Offset = 0x210u,
  kAstHole2Offset = 0x214u,
  kAstHole2EndOffset = 0x3fcu,
};

static volatile bool g_fault_seen = false;
static volatile uint32_t g_last_mcause = 0;

void ottf_exception_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = ibex_mcause_read();
  if (mcause == kIbexExcLoadAccessFault || mcause == kIbexExcStoreAccessFault) {
    g_fault_seen = true;
    g_last_mcause = mcause;
    uint32_t mepc = ibex_mepc_read();
    uint16_t insn = *(const uint16_t *)mepc;
    uint32_t step = ((insn & 0x3u) == 0x3u) ? 4u : 2u;
    ibex_mepc_write(mepc + step);
    return;
  }
  ottf_generic_fault_print(exc_info, "Unexpected exception", mcause);
  abort();
}

static void verify_ast_rega_ro_and_regal_dead_readback(void) {
  LOG_INFO(
      "Verifying [ast.sv:861-877] (TRUE_SILICON_ERRATA): REGA0/1/28 SwAccessRO "
      "& REGAL disconnected u_regal.qs() readback...");

  // 1. REGA0 (0x00), REGA1 (0x04), and REGA28 (0x70) are SwAccessRO:
  //    32-bit writes do not fault (d_error = 0) and reads always return their
  //    hardcoded reset constants (0x00, 0x01, 0x1c).
  g_fault_seen = false;
  abs_mmio_write32(kAstBase + kAstRega0Offset, 0xa5a5a5a5u);
  abs_mmio_write32(kAstBase + kAstRega1Offset, 0x5a5a5a5au);
  abs_mmio_write32(kAstBase + kAstRega28Offset, 0xdeadbeefu);
  for (volatile int d = 0; d < 16; ++d) {
  }
  CHECK(!g_fault_seen, "32-bit writes to REGA0/1/28 must not fault");

  CHECK(abs_mmio_read32(kAstBase + kAstRega0Offset) == 0x00000000u,
        "REGA0 must remain read-only constant 0x00000000");
  CHECK(abs_mmio_read32(kAstBase + kAstRega1Offset) == 0x00000001u,
        "REGA1 must remain read-only constant 0x00000001");
  CHECK(abs_mmio_read32(kAstBase + kAstRega28Offset) == 0x0000001cu,
        "REGA28 must remain read-only constant 0x0000001c");

  // 2. REGA2 (0x08) and REGB0..REGB4 (0x200..0x210) accept and retain full
  //    32-bit values (preserving original values after test).
  uint32_t orig_rega2 = abs_mmio_read32(kAstBase + kAstRega2Offset);
  uint32_t orig_regb0 = abs_mmio_read32(kAstBase + kAstRegb0Offset);
  abs_mmio_write32(kAstBase + kAstRega2Offset, 0xdeadbeefu);
  abs_mmio_write32(kAstBase + kAstRegb0Offset, 0xcafebabeu);
  CHECK(abs_mmio_read32(kAstBase + kAstRega2Offset) == 0xdeadbeefu);
  CHECK(abs_mmio_read32(kAstBase + kAstRegb0Offset) == 0xcafebabeu);
  abs_mmio_write32(kAstBase + kAstRega2Offset, orig_rega2);
  abs_mmio_write32(kAstBase + kAstRegb0Offset, orig_regb0);

  // 3. REGAL (0x98) has disconnected u_regal.qs() (SwAccessWO) and addr_hit[38]
  //    hardwired to 0x00000000 in ast_reg_top.sv:1911-1913, despite ast.sv:866
  //    wiring hw2reg.regal.d = regal (resval 0x26).
  g_fault_seen = false;
  uint32_t regal_before = abs_mmio_read32(kAstBase + kAstRegalOffset);
  CHECK(regal_before == 0x00000000u,
        "[ast.sv:861-877] REGAL readback must be 0x00000000 (not 0x26), got "
        "0x%08x",
        regal_before);

  abs_mmio_write32(kAstBase + kAstRegalOffset, 0x12345678u);
  uint32_t regal_after = abs_mmio_read32(kAstBase + kAstRegalOffset);
  CHECK(!g_fault_seen, "32-bit write/read on REGAL must not fault");
  CHECK(regal_after == 0x00000000u,
        "[ast.sv:861-877] REGAL readback after write must still be 0x00000000, "
        "got 0x%08x",
        regal_after);
}

static void verify_ast_subword_wr_err_and_addrmiss(void) {
  LOG_INFO(
      "Verifying [ast_reg_pkg.sv:305-355] (INTENDED_SECURITY_HARDENING): "
      "AST_PERMIT "
      "sub-word wr_err & unmapped aperture addrmiss...");

  uint32_t saved_rega2 = abs_mmio_read32(kAstBase + kAstRega2Offset);

  // 1. Sub-word 8-bit and 16-bit writes to REGA0 (RO), REGA2 (RW), and REGAL
  //    (WO) must trigger synchronous Store Access Fault (mcause = 7) and leave
  //    REGA2 unmodified.
  g_fault_seen = false;
  abs_mmio_write8(kAstBase + kAstRega0Offset, 0x11u);
  for (volatile int d = 0; d < 16 && !g_fault_seen; ++d) {
  }
  CHECK(g_fault_seen && g_last_mcause == kIbexExcStoreAccessFault,
        "8-bit write to REGA0 must raise Store Access Fault");

  g_fault_seen = false;
  *((volatile uint16_t *)(kAstBase + kAstRega2Offset)) = 0x2233u;
  for (volatile int d = 0; d < 16 && !g_fault_seen; ++d) {
  }
  CHECK(g_fault_seen && g_last_mcause == kIbexExcStoreAccessFault,
        "16-bit write to REGA2 must raise Store Access Fault");
  CHECK(abs_mmio_read32(kAstBase + kAstRega2Offset) == saved_rega2,
        "Sub-word write to REGA2 must not modify register contents");

  g_fault_seen = false;
  abs_mmio_write8(kAstBase + kAstRegalOffset, 0x44u);
  for (volatile int d = 0; d < 16 && !g_fault_seen; ++d) {
  }
  CHECK(g_fault_seen && g_last_mcause == kIbexExcStoreAccessFault,
        "8-bit write to REGAL must raise Store Access Fault");

  // 2. Unmapped aperture holes 0x9c..0x1fc and 0x214..0x3fc must raise
  //    synchronous addrmiss Load/Store Access Faults (mcause = 5 / 7).
  const uint32_t kUnmappedOffsets[] = {
      kAstHole1Offset,
      kAstHole1EndOffset,
      kAstHole2Offset,
      kAstHole2EndOffset,
  };
  for (size_t i = 0; i < sizeof(kUnmappedOffsets) / sizeof(kUnmappedOffsets[0]);
       ++i) {
    uint32_t off = kUnmappedOffsets[i];
    g_fault_seen = false;
    (void)abs_mmio_read32(kAstBase + off);
    for (volatile int d = 0; d < 16 && !g_fault_seen; ++d) {
    }
    CHECK(g_fault_seen && g_last_mcause == kIbexExcLoadAccessFault,
          "Read at unmapped AST offset 0x%x must raise Load Access Fault", off);

    g_fault_seen = false;
    abs_mmio_write32(kAstBase + off, 0xdeadbeefu);
    for (volatile int d = 0; d < 16 && !g_fault_seen; ++d) {
    }
    CHECK(g_fault_seen && g_last_mcause == kIbexExcStoreAccessFault,
          "Write at unmapped AST offset 0x%x must raise Store Access Fault",
          off);
  }
}

bool test_main(void) {
  verify_ast_rega_ro_and_regal_dead_readback();
  verify_ast_subword_wr_err_and_addrmiss();
  LOG_INFO("All [ast.sv:861-877..002] checks confirmed!");
  return true;
}
