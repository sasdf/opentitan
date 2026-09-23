// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file rv_dm_errata_test.c
 * @brief CW340 FPGA & QEMU Errata Confirmation Test for `rv_dm` (P21).
 *
 * Empirically confirms on both physical CW340 FPGA silicon and QEMU:
 * - [dm_mem.sv:71-86,389-403] (SPEC_DOC_ERRATA):
 *   `Halted..Exception` are 8-byte spaced at `0x100, 0x108, 0x110, 0x118`
 *   (accessing `0x104` or `0x10c` raises a TL-UL bus fault, contrary to
 *   `theory_of_operation.md:10`), and `0x338..0x387` (`ABSTRACTCMD`,
 *   `PROGRAM_BUFFER`, `DATA0/1`) are implemented (contrary to
 *   `theory_of_operation.md:11`).
 * - [dm_mem.sv:378-402] (TRUE_SILICON_ERRATA):
 *   Read-only (`swaccess: "ro"`) regions `WHERETO` (`0x300`), `ABSTRACTCMD`
 *   (`0x338..0x35c`), `PROGRAM_BUFFER` (`0x360..0x37c`), and `FLAGS`
 *   (`0x400..0x7fc`) use `gen_wr_err(we_i, be_i, FullRegMask)`
 * (`dm_mem.sv:374`) instead of `err_d = we_i`, so 32-bit writes (`sw`, `be_i =
 * 4'b1111`) succeed with `d_error = 0` while sub-word writes (`sb`/`sh`)
 * bus-fault
 *   (`d_error = 1`, `mcause = 7`), whereas `ROM` (`0x800..0xffc`) rejects
 *   32-bit writes (`err_d = we_i`).
 * - [rv_dm.sv:238-243,505-520] (INTENDED_SECURITY_HARDENING):
 *   `RV_DM.REGS` (`0x41200000`) remains accessible independent of `RV_DM.MEM`
 *   (`0x00010000`) lifecycle gating (`u_tlul_lc_gate_rom`), allowing software
 *   to write `LATE_DEBUG_ENABLE = kMultiBitBool32True` (`0x96969696`) to ungate
 *   `RV_DM.MEM`.
 * - [rv_dm_reg_pkg.sv:53-57] (INTENDED_SECURITY_HARDENING):
 *   `RV_DM.REGS` (`BlockAw = 4`) asserts `addrmiss` (`d_error = 1`) at unmapped
 *   offset `0x0c`, and `RV_DM_REGS_PERMIT` rejects sub-word writes to
 *   `LATE_DEBUG_ENABLE` (`0x08`, `PERMIT = 4'b1111`) with `mcause = 7` while
 *   accepting byte-0 `sb` writes to `LATE_DEBUG_ENABLE_REGWEN` (`0x04`,
 *   `PERMIT = 4'b0001`).
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/dif/dif_lc_ctrl.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "rv_dm_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kRvDmRegsBase = TOP_EARLGREY_RV_DM_REGS_BASE_ADDR,
  kRvDmMemBase = TOP_EARLGREY_RV_DM_MEM_BASE_ADDR,
  kLcCtrlBase = TOP_EARLGREY_LC_CTRL_BASE_ADDR,
};

static volatile bool g_load_fault_seen = false;
static volatile bool g_store_fault_seen = false;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = ibex_mcause_read();
  if (mcause == kIbexExcLoadAccessFault) {
    g_load_fault_seen = true;
  } else if (mcause == kIbexExcStoreAccessFault) {
    g_store_fault_seen = true;
  } else {
    CHECK(false, "Unexpected exception mcause=0x%x", mcause);
  }
}

static bool try_read32(uint32_t addr, uint32_t *out_val) {
  g_load_fault_seen = false;
  uint32_t val = abs_mmio_read32(addr);
  if (g_load_fault_seen) {
    return false;
  }
  if (out_val != NULL) {
    *out_val = val;
  }
  return true;
}

static bool try_write32(uint32_t addr, uint32_t val) {
  g_store_fault_seen = false;
  abs_mmio_write32(addr, val);
  return !g_store_fault_seen;
}

static bool try_write16(uint32_t addr, uint16_t val) {
  g_store_fault_seen = false;
  *(volatile uint16_t *)addr = val;
  return !g_store_fault_seen;
}

static bool try_write8(uint32_t addr, uint8_t val) {
  g_store_fault_seen = false;
  abs_mmio_write8(addr, val);
  return !g_store_fault_seen;
}

bool test_main(void) {
  // 1. Verify [rv_dm_reg_pkg.sv:53-57] & [rv_dm.sv:238-243,505-520]: RV_DM.REGS
  // (0x41200000)
  //    is always open on the peripheral bus, faults on unmapped 0x0c, and
  //    enforces RV_DM_REGS_PERMIT on LATE_DEBUG_ENABLE (0x08) vs REGWEN (0x04).
  LOG_INFO(
      "Verifying [rv_dm.sv:238-243,505-520..004] "
      "(INTENDED_SECURITY_HARDENING): "
      "RV_DM.REGS 0x0c addrmiss, RV_DM_REGS_PERMIT, and LATE_DEBUG_ENABLE...");

  CHECK(!try_read32(kRvDmRegsBase + 0x0cu, NULL),
        "Unmapped RV_DM.REGS offset 0x0c must raise Load Access Fault");
  CHECK(!try_write32(kRvDmRegsBase + 0x0cu, 0x12345678u),
        "Unmapped RV_DM.REGS offset 0x0c must raise Store Access Fault");

  uint32_t orig_late_dbg =
      abs_mmio_read32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET);
  CHECK(!try_write8(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET, 0x12u),
        "8-bit write to LATE_DEBUG_ENABLE must raise Store Access Fault");
  CHECK(
      !try_write16(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET, 0x1234u),
      "16-bit write to LATE_DEBUG_ENABLE must raise Store Access Fault");
  CHECK(abs_mmio_read32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET) ==
        orig_late_dbg);

  CHECK(
      try_write8(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REGWEN_REG_OFFSET,
                 0xffu),
      "8-bit write to LATE_DEBUG_ENABLE_REGWEN (PERMIT=4'b0001) must succeed");
  CHECK(abs_mmio_read32(kRvDmRegsBase +
                        RV_DM_LATE_DEBUG_ENABLE_REGWEN_REG_OFFSET) == 1u);

  // Enable LATE_DEBUG_ENABLE (kMultiBitBool32True = 0x96969696) to ensure
  // u_tlul_lc_gate_rom ungates RV_DM.MEM (0x00010000).
  CHECK(try_write32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET,
                    kMultiBitBool32True));
  CHECK(abs_mmio_read32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET) ==
        kMultiBitBool32True);

  // 2. Verify [dm_mem.sv:71-86,389-403] (SPEC_DOC_ERRATA):
  //    Halted..Exception at 8-byte stride (0x100, 0x108, 0x110, 0x118) vs
  //    faulting 0x104/0x10c, and implemented 0x338..0x387 vs unmapped gaps.
  LOG_INFO(
      "Verifying [dm_mem.sv:71-86,389-403] (SPEC_DOC_ERRATA): 8-byte stride "
      "0x100..0x118 (0x104/0x10c fault) & implemented 0x338..0x387...");

  uint32_t val = 0;
  CHECK(try_read32(kRvDmMemBase + 0x100u, &val) && val == 0x00000000u);
  CHECK(try_read32(kRvDmMemBase + 0x108u, &val) && val == 0x00000000u);
  CHECK(try_read32(kRvDmMemBase + 0x110u, &val) && val == 0x00000000u);
  CHECK(try_read32(kRvDmMemBase + 0x118u, &val) && val == 0x00000000u);

  CHECK(!try_read32(kRvDmMemBase + 0x104u, &val),
        "0x104 must raise Load Access Fault (8-byte stride in dm_mem.sv)");
  CHECK(!try_read32(kRvDmMemBase + 0x10cu, &val),
        "0x10c must raise Load Access Fault (8-byte stride in dm_mem.sv)");
  CHECK(!try_read32(kRvDmMemBase + 0x000u, &val));
  CHECK(!try_read32(kRvDmMemBase + 0x304u, &val));
  CHECK(!try_read32(kRvDmMemBase + 0x388u, &val));

  CHECK(try_read32(kRvDmMemBase + 0x33cu, &val) && val == 0x00000517u,
        "ABSTRACTCMD[1] (0x33c) must be implemented");
  CHECK(try_read32(kRvDmMemBase + 0x360u, &val) && val == 0x00000000u,
        "PROGRAM_BUFFER[0] (0x360) must be implemented");
  CHECK(try_read32(kRvDmMemBase + 0x380u, &val) && val == 0x00000000u,
        "DATA0 (0x380) must be implemented");

  // 3. Verify [dm_mem.sv:378-402] (TRUE_SILICON_ERRATA):
  //    Read-only regions WhereTo (0x300), AbstractCmd (0x338..0x35c),
  //    ProgBuf (0x360..0x37c), and Flags (0x400..0x7fc) succeed with d_error=0
  //    on 32-bit writes (be_i = 4'b1111) while ignoring the write data, but
  //    fault with d_error=1 on sub-word writes (sb/sh), whereas Debug ROM
  //    (0x800..0xffc) faults even on 32-bit writes (err_d = we_i).
  LOG_INFO(
      "Verifying [dm_mem.sv:378-402] (TRUE_SILICON_ERRATA): RO regions succeed "
      "on 32-bit sw (d_error=0) but fault on sub-word sb/sh (d_error=1)...");

  CHECK(try_write32(kRvDmMemBase + 0x300u, 0xdeadbeefu),
        "32-bit write to RO WhereTo (0x300) must succeed with d_error=0");
  CHECK(try_read32(kRvDmMemBase + 0x300u, &val) && val == 0x00000000u);

  CHECK(try_write32(kRvDmMemBase + 0x33cu, 0xdeadbeefu),
        "32-bit write to RO AbstractCmd (0x33c) must succeed with d_error=0");
  CHECK(try_read32(kRvDmMemBase + 0x33cu, &val) && val == 0x00000517u);

  CHECK(try_write32(kRvDmMemBase + 0x360u, 0xdeadbeefu),
        "32-bit write to RO ProgBuf (0x360) must succeed with d_error=0");
  CHECK(try_read32(kRvDmMemBase + 0x360u, &val) && val == 0x00000000u);

  CHECK(try_write32(kRvDmMemBase + 0x400u, 0xdeadbeefu),
        "32-bit write to RO Flags (0x400) must succeed with d_error=0");
  CHECK(try_read32(kRvDmMemBase + 0x400u, &val) && val == 0x00000000u);

  // Sub-word writes to those exact same RO regions must fault with d_error=1!
  CHECK(!try_write8(kRvDmMemBase + 0x300u, 0x11u));
  CHECK(!try_write16(kRvDmMemBase + 0x338u, 0x2222u));
  CHECK(!try_write8(kRvDmMemBase + 0x360u, 0x33u));
  CHECK(!try_write8(kRvDmMemBase + 0x400u, 0x55u));

  // Whereas Debug ROM (0x800) rejects even 32-bit writes (err_d = we_i).
  CHECK(try_read32(kRvDmMemBase + 0x800u, &val) && val == 0x0180006fu);
  CHECK(!try_write32(kRvDmMemBase + 0x800u, 0xdeadbeefu),
        "32-bit write to Debug ROM (0x800) must fault with err_d = we_i");
  CHECK(try_read32(kRvDmMemBase + 0x800u, &val) && val == 0x0180006fu);

  // Restore original LATE_DEBUG_ENABLE.
  abs_mmio_write32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET,
                   orig_late_dbg);

  LOG_INFO("All [dm_mem.sv:71-86,389-403..004] checks confirmed!");
  return true;
}
