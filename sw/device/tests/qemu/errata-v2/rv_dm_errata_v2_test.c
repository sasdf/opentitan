// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * Earlgrey v2 RV_DM Hardware Behavior & RTL Verification Test on CW340 FPGA.
 *
 * Verifies five RV_DM hardware behaviors in Earlgrey v2 (`trunk-v2`):
 *
 * 1. `RV_DM.REGS` (`0x41200000`) `RegsAw = 4` Address Decode (`0x00..0x08`
 *    mapped, `0x0c` `addrmiss = 1`) and `RV_DM_REGS_PERMIT = '{4'b0001,
 *    4'b0001, 4'b1111}` Byte-Enable Enforcement
 * (`hw/ip/rv_dm/rtl/rv_dm_reg_pkg.sv:14, 1001-1007`,
 * `hw/ip/rv_dm/rtl/rv_dm_regs_reg_top.sv:269-290`):
 *    - Offsets `0x00` (`ALERT_TEST`), `0x04` (`LATE_DEBUG_ENABLE_REGWEN`), and
 *      `0x08` (`LATE_DEBUG_ENABLE`) are mapped (`reg_we_check` /
 * `reg_re_check`), whereas offset `0x0c` asserts `addrmiss = 1`, raising
 * synchronous Load/Store Access Faults (`mcause = 5/7`).
 *    - `LATE_DEBUG_ENABLE` (`0x08`) requires `RV_DM_REGS_PERMIT[2] = 4'b1111`
 *      so sub-word writes (`sb` / `sh`) fault with `mcause = 7` without
 *      modifying the register, whereas `LATE_DEBUG_ENABLE_REGWEN` (`0x04`)
 *      has `RV_DM_REGS_PERMIT[1] = 4'b0001` and accepts a byte-0 `sb` write.
 *
 * 2. Asymmetric `RV_DM.MEM` (`0x00010000`) Lifecycle & `LATE_DEBUG_ENABLE`
 *    Bus Gating via `u_tlul_lc_gate_rom` and `OTP_CTRL.HW_CFG1`
 *    `DIS_RV_DM_LATE_DEBUG` Override (`hw/ip/rv_dm/rtl/rv_dm.sv:260-265,
 *    646-659`):
 *    - `RV_DM.REGS` (`0x41200000`) is directly connected to the peripheral bus
 *      without a `tlul_lc_gate`, whereas `RV_DM.MEM` (`0x00010000`) is gated by
 *      `u_tlul_lc_gate_rom` (`lc_hw_debug_en_gated_ndm[LcEnRom]`).
 *    - In `rv_dm.sv:260-265`, `lc_hw_debug_en_gated_raw[k]` ORs
 *      `mubi8_test_true_strict(otp_dis_rv_dm_late_debug[k])` with
 *      `mubi32_test_true_strict(late_debug_enable[k])`. When
 *      `HW_CFG1.DIS_RV_DM_LATE_DEBUG` is fused to `kMultiBitBool8True` (`0x96`,
 *      as in `trunk-v2` `hw/top_earlgrey/data/otp/BUILD:232`), writing
 *      `LATE_DEBUG_ENABLE = kMultiBitBool32False` (`0x69696969`) is silently
 *      overridden by `otp_dis_rv_dm_late_debug` (`RV_DM.MEM` remains open with
 *      `mcause = 0`).
 *
 * 3. `RV_DM.MEM` (`0x00010000`) 8-Byte Control Register Stride (`0x100, 0x108,
 *    0x110, 0x118`) and Implemented `ABSTRACTCMD` / `PROGRAM_BUFFER` / `DATA`
 *    Regions vs. `theory_of_operation.md`
 * (`hw/ip/rv_dm/doc/theory_of_operation.md:10-11`,
 *    `hw/vendor/pulp_riscv_dbg/src/dm_mem.sv:71-86, 389-403`):
 *    - `Halted`, `Going`, `Resuming`, `Exception` are decoded on an 8-byte
 *      stride (`0x100`, `0x108`, `0x110`, `0x118`), whereas the 4-byte odd
 *      slots (`0x104`, `0x10c`, `0x114`, `0x11c`) hit the `default: err_d =
 * 1'b1` arm (`mcause = 5/7`).
 *    - `ABSTRACTCMD` (`0x338..0x35f`), `PROGRAM_BUFFER` (`0x360..0x37f`), and
 *      `DATA0/1` (`0x380..0x387`) are mapped and readable (`mcause = 0`),
 *      whereas unmapped gaps (`0x000`, `0x304`, `0x388`) fault (`mcause =
 * 5/7`).
 *
 * 4. `RV_DM.MEM` Read-Only (`swaccess: "ro"`) Regions (`WHERETO` `0x300`,
 *    `ABSTRACTCMD` `0x338..0x35c`, `PROGRAM_BUFFER` `0x360..0x37c`, `FLAGS`
 *    `0x400..0x7fc`) `gen_wr_err(we_i, be_i, FullRegMask)` vs. `ROM`
 * (`0x800..0xffc`) `err_d = we_i`
 * (`hw/vendor/pulp_riscv_dbg/src/dm_mem.sv:374-406`,
 *    `hw/ip/rv_dm/data/rv_dm.hjson:103-690`):
 *    - Full-word 32-bit `sw` writes (`be_i = 4'b1111`) to `WHERETO`,
 *      `ABSTRACTCMD`, `PROGRAM_BUFFER`, and `FLAGS` evaluate
 *      `gen_wr_err(1'b1, 4'b1111, 4'b1111) == 1'b0` and complete without bus
 *      error (`mcause = 0`) while ignoring the write data, whereas sub-word
 *      `sb`/`sh` writes fault with `mcause = 7`.
 *    - By contrast, `ROM` (`0x800..0xffc`) assigns `err_d = we_i`, faulting
 *      even on 32-bit full-word `sw` (`mcause = 7`).
 *
 * 5. `DATAADDR_0`/`DATAADDR_1` (`0x380..0x387`) Synchronous Zero-Clamp Under
 *    `!dmcontrol_q.dmactive` (`hw/vendor/pulp_riscv_dbg/src/dm_csrs.sv:612,
 *    627, 647`) and `ABSTRACTCMD_1..9` (`0x33c..0x35c`) Non-Zero Reset
 *    Instructions (`hw/vendor/pulp_riscv_dbg/src/dm_mem.sv:425-437` vs.
 *    `hw/ip/rv_dm/data/rv_dm.hjson` `resval: "0"`):
 *    - Although `DATAADDR_0` and `DATAADDR_1` are declared `swaccess: "rw"` in
 *      `rv_dm.hjson:249-280` and 32-bit `sw` stores return `err_d = 0`
 *      (`mcause = 0`), `dm_csrs.sv:647` clamps `data_q <= '0` on every clock
 *      cycle whenever `!dmcontrol_q.dmactive` (which resets to `0` and cannot
 *      be set by the CPU on `top_earlgrey` when `UseDmiInterface = 1`),
 *      discarding all 32-bit CPU writes while sub-word `sb`/`sh` stores fault
 *      with `mcause = 7`.
 *    - `ABSTRACTCMD_1..9` (`0x33c..0x35c`) specify `resval: "0"` in
 *      `rv_dm.hjson`, but `dm_mem.sv:425-437` combinationally drives non-zero
 *      RISC-V instructions (`0x00000517`, `0x00000013`, `0x00100073`).
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top/lc_ctrl_regs.h"
#include "hw/top/otp_ctrl_regs.h"
#include "hw/top/rv_dm_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kRvDmRegsBase = TOP_EARLGREY_RV_DM_REGS_BASE_ADDR,
  kRvDmMemBase = TOP_EARLGREY_RV_DM_MEM_BASE_ADDR,
  kOtpCtrlBase = TOP_EARLGREY_OTP_CTRL_CORE_BASE_ADDR,
  kLcCtrlBase = TOP_EARLGREY_LC_CTRL_REGS_BASE_ADDR,

  kRvDmData0Offset = RV_DM_DATAADDR_0_REG_OFFSET,
  kRvDmData1Offset = RV_DM_DATAADDR_1_REG_OFFSET,
  kRvDmRom0Offset = 0x800u,

  kMcauseLoadAccessFault = 5u,
  kMcauseStoreAccessFault = 7u,
};

static volatile uint32_t fault_count = 0;
static volatile uint32_t last_mcause = 0;

void ottf_external_nmi_handler(uint32_t *exc_info) { (void)exc_info; }

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = ibex_mcause_read();
  fault_count++;
  last_mcause = mcause;

  uint32_t mepc;
  CSR_READ(CSR_REG_MEPC, &mepc);
  uint16_t insn16 = *(const volatile uint16_t *)mepc;
  uint32_t step = ((insn16 & 0x3u) == 0x3u) ? 4u : 2u;
  CSR_WRITE(CSR_REG_MEPC, mepc + step);
}

static uint32_t probe_read32(uint32_t addr, uint32_t *out_val) {
  uint32_t before = fault_count;
  last_mcause = 0;
  uint32_t val = 0;
  asm volatile("lw %0, 0(%1)" : "=r"(val) : "r"(addr) : "memory");
  if (out_val != NULL) {
    *out_val = val;
  }
  return (fault_count > before) ? last_mcause : 0u;
}

static uint32_t probe_write32(uint32_t addr, uint32_t val) {
  uint32_t before = fault_count;
  last_mcause = 0;
  asm volatile("sw %0, 0(%1)" : : "r"(val), "r"(addr) : "memory");
  return (fault_count > before) ? last_mcause : 0u;
}

static uint32_t probe_write16(uint32_t addr, uint16_t val) {
  uint32_t before = fault_count;
  last_mcause = 0;
  asm volatile("sh %0, 0(%1)" : : "r"(val), "r"(addr) : "memory");
  return (fault_count > before) ? last_mcause : 0u;
}

static uint32_t probe_write8(uint32_t addr, uint8_t val) {
  uint32_t before = fault_count;
  last_mcause = 0;
  asm volatile("sb %0, 0(%1)" : : "r"(val), "r"(addr) : "memory");
  return (fault_count > before) ? last_mcause : 0u;
}

/**
 * Test 1: `RV_DM.REGS` (`0x41200000`) `RegsAw = 4` Address Decode (`0x0c`
 * `addrmiss`) and `RV_DM_REGS_PERMIT` Byte-Enable Enforcement.
 */
static void test_rv_dm_regs_decode_and_permit(void) {
  LOG_INFO(
      "Test 1: RV_DM.REGS decode and RV_DM_REGS_PERMIT byte-enable checks");

  uint32_t regwen = 0;
  uint32_t late_dbg = 0;
  CHECK(probe_read32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REGWEN_REG_OFFSET,
                     &regwen) == 0u);
  CHECK(regwen == 1u);
  CHECK(probe_read32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET,
                     &late_dbg) == 0u);

  // Offset 0x0c within the 16-byte RegsAw=4 window is unmapped (addrmiss = 1).
  CHECK(probe_read32(kRvDmRegsBase + 0x0cu, NULL) == kMcauseLoadAccessFault);
  CHECK(probe_write32(kRvDmRegsBase + 0x0cu, 0u) == kMcauseStoreAccessFault);

  // LATE_DEBUG_ENABLE (0x08) has RV_DM_REGS_PERMIT[2] = 4'b1111: sub-word
  // writes (sb/sh) must fault with Store Access Fault (mcause = 7) and leave
  // the register unmodified.
  CHECK(probe_write8(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET,
                     0x96u) == kMcauseStoreAccessFault);
  CHECK(probe_write16(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET,
                      0x9696u) == kMcauseStoreAccessFault);
  CHECK(abs_mmio_read32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET) ==
        late_dbg);

  // LATE_DEBUG_ENABLE_REGWEN (0x04) has RV_DM_REGS_PERMIT[1] = 4'b0001: a
  // byte-0 `sb` write of 1 succeeds without bus error (keeping REGWEN = 1),
  // whereas a byte-1 `sb` write (be = 4'b0010) faults with mcause = 7.
  CHECK(probe_write8(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REGWEN_REG_OFFSET,
                     0x01u) == 0u);
  CHECK(probe_write8(
            kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REGWEN_REG_OFFSET + 1u,
            0x01u) == kMcauseStoreAccessFault);
}

/**
 * Test 2: `RV_DM.MEM` (`0x00010000`) `u_tlul_lc_gate_rom` Gating Multiplexer
 * (`rv_dm.sv:260-265, 646-659`): Interaction of `OTP_CTRL.HW_CFG1`
 * `DIS_RV_DM_LATE_DEBUG` (`otp_dis_rv_dm_late_debug_i`) and
 * `LATE_DEBUG_ENABLE`.
 */
static void test_rv_dm_mem_late_debug_gate(void) {
  LOG_INFO("Test 2: RV_DM.MEM u_tlul_lc_gate_rom vs OTP DIS_RV_DM_LATE_DEBUG");

  uint32_t lc_state =
      abs_mmio_read32(kLcCtrlBase + LC_CTRL_LC_STATE_REG_OFFSET);
  while ((abs_mmio_read32(kOtpCtrlBase + OTP_CTRL_STATUS_REG_OFFSET) &
          (1u << OTP_CTRL_STATUS_DAI_IDLE_BIT)) == 0u) {
  }
  abs_mmio_write32(kOtpCtrlBase + OTP_CTRL_DIRECT_ACCESS_ADDRESS_REG_OFFSET,
                   OTP_CTRL_PARAM_HW_CFG1_OFFSET);
  abs_mmio_write32(kOtpCtrlBase + OTP_CTRL_DIRECT_ACCESS_CMD_REG_OFFSET,
                   1u << OTP_CTRL_DIRECT_ACCESS_CMD_RD_BIT);
  while ((abs_mmio_read32(kOtpCtrlBase + OTP_CTRL_STATUS_REG_OFFSET) &
          (1u << OTP_CTRL_STATUS_DAI_IDLE_BIT)) == 0u) {
  }
  uint32_t hw_cfg1_word0 =
      abs_mmio_read32(kOtpCtrlBase + OTP_CTRL_DIRECT_ACCESS_RDATA_0_REG_OFFSET);
  uint32_t dis_rv_dm_late_debug =
      (hw_cfg1_word0 >> ((OTP_CTRL_PARAM_DIS_RV_DM_LATE_DEBUG_OFFSET -
                          OTP_CTRL_PARAM_HW_CFG1_OFFSET) *
                         8u)) &
      0xffu;
  LOG_INFO("LC_STATE=0x%08x, HW_CFG1[0]=0x%08x, DIS_RV_DM_LATE_DEBUG=0x%02x",
           lc_state, hw_cfg1_word0, dis_rv_dm_late_debug);

  // Set LATE_DEBUG_ENABLE to kMultiBitBool32False (0x69696969).
  CHECK(probe_write32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET,
                      kMultiBitBool32False) == 0u);
  CHECK(abs_mmio_read32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET) ==
        (uint32_t)kMultiBitBool32False);

  // In `rv_dm.sv:260-265`:
  //   lc_hw_debug_en_gated_raw[k] =
  //     (mubi8_test_true_strict(otp_dis_rv_dm_late_debug[k]) ||
  //      mubi32_test_true_strict(late_debug_enable[k])) ?
  //     lc_hw_debug_en_raw[k] : lc_dft_en_raw[k];
  // When HW_CFG1.DIS_RV_DM_LATE_DEBUG is kMultiBitBool8True (0x96, as fused in
  // trunk-v2 `hw/top_earlgrey/data/otp/BUILD:232`), `otp_dis_rv_dm_late_debug`
  // overrides `LATE_DEBUG_ENABLE = kMultiBitBool32False` and keeps
  // `u_tlul_lc_gate_rom` open (`mcause = 0`).
  uint32_t whereto = 0;
  if (dis_rv_dm_late_debug == (uint32_t)kMultiBitBool8True) {
    CHECK(probe_read32(kRvDmMemBase + RV_DM_WHERETO_REG_OFFSET, &whereto) ==
          0u);
  } else {
    CHECK(probe_read32(kRvDmMemBase + RV_DM_WHERETO_REG_OFFSET, NULL) ==
          kMcauseLoadAccessFault);
  }

  // Enable LATE_DEBUG_ENABLE = kMultiBitBool32True (0x96969696) so RV_DM.MEM is
  // unconditionally open regardless of OTP DIS_RV_DM_LATE_DEBUG.
  CHECK(probe_write32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET,
                      kMultiBitBool32True) == 0u);
  CHECK(abs_mmio_read32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET) ==
        (uint32_t)kMultiBitBool32True);
  CHECK(probe_read32(kRvDmMemBase + RV_DM_WHERETO_REG_OFFSET, &whereto) == 0u);
}

/**
 * Test 3: `RV_DM.MEM` (`dm_mem.sv`) 8-Byte Control Register Stride (`0x100,
 * 0x108, 0x110, 0x118`) and Implemented `ABSTRACTCMD`, `PROGRAM_BUFFER`, `DATA`
 * Regions vs. `theory_of_operation.md`.
 */
static void test_rv_dm_mem_stride_and_regions(void) {
  LOG_INFO("Test 3: RV_DM.MEM 8-byte stride (0x100..0x118) and mapped regions");

  // 8-byte aligned slots (0x100, 0x108, 0x110, 0x118) are mapped in dm_mem.sv.
  uint32_t val = 0;
  CHECK(probe_read32(kRvDmMemBase + RV_DM_HALTED_REG_OFFSET, &val) == 0u);
  CHECK(probe_read32(kRvDmMemBase + RV_DM_GOING_REG_OFFSET, &val) == 0u);
  CHECK(probe_read32(kRvDmMemBase + RV_DM_RESUMING_REG_OFFSET, &val) == 0u);
  CHECK(probe_read32(kRvDmMemBase + RV_DM_EXCEPTION_REG_OFFSET, &val) == 0u);

  // Odd 4-byte slots (0x104, 0x10c, 0x114, 0x11c) hit default: err_d = 1'b1.
  CHECK(probe_read32(kRvDmMemBase + 0x104u, NULL) == kMcauseLoadAccessFault);
  CHECK(probe_write32(kRvDmMemBase + 0x104u, 0u) == kMcauseStoreAccessFault);
  CHECK(probe_read32(kRvDmMemBase + 0x10cu, NULL) == kMcauseLoadAccessFault);
  CHECK(probe_write32(kRvDmMemBase + 0x10cu, 0u) == kMcauseStoreAccessFault);

  // ABSTRACTCMD (0x338..0x35c), PROGRAM_BUFFER (0x360..0x37c), and DATA0/1
  // (0x380..0x384) are mapped and readable without fault.
  CHECK(probe_read32(kRvDmMemBase + RV_DM_ABSTRACTCMD_0_REG_OFFSET, &val) ==
        0u);
  CHECK(probe_read32(kRvDmMemBase + RV_DM_PROGRAM_BUFFER_0_REG_OFFSET, &val) ==
        0u);
  CHECK(probe_read32(kRvDmMemBase + kRvDmData0Offset, &val) == 0u);
  CHECK(probe_read32(kRvDmMemBase + kRvDmData1Offset, &val) == 0u);

  // Unmapped gaps (0x000, 0x304, 0x388) fault with Load/Store Access Fault.
  CHECK(probe_read32(kRvDmMemBase + 0x000u, NULL) == kMcauseLoadAccessFault);
  CHECK(probe_read32(kRvDmMemBase + 0x304u, NULL) == kMcauseLoadAccessFault);
  CHECK(probe_read32(kRvDmMemBase + 0x388u, NULL) == kMcauseLoadAccessFault);
}

/**
 * Test 4: `RV_DM.MEM` Read-Only (`swaccess: "ro"`) Regions (`WHERETO`,
 * `ABSTRACTCMD`, `PROGRAM_BUFFER`, `FLAGS`) `gen_wr_err(we_i, be_i,
 * FullRegMask)` vs. `ROM` (`0x800..0xffc`) `err_d = we_i`.
 */
static void test_rv_dm_mem_ro_write_err_behavior(void) {
  LOG_INFO(
      "Test 4: RV_DM.MEM ro regions 32-bit sw (err_d=0) vs sb/sh (err_d=1) vs "
      "ROM");

  uint32_t whereto_before = 0;
  uint32_t flags0_before = 0;
  uint32_t rom0_before = 0;
  CHECK(probe_read32(kRvDmMemBase + RV_DM_WHERETO_REG_OFFSET,
                     &whereto_before) == 0u);
  CHECK(probe_read32(kRvDmMemBase + RV_DM_FLAGS_0_REG_OFFSET, &flags0_before) ==
        0u);
  CHECK(probe_read32(kRvDmMemBase + kRvDmRom0Offset, &rom0_before) == 0u);

  // Full-word 32-bit `sw` (`be_i = 4'b1111`) to WHERETO (0x300), ABSTRACTCMD
  // (0x338), PROGRAM_BUFFER (0x360), and FLAGS_0 (0x400) completes with
  // d_error = 0 (mcause = 0) while ignoring the write data.
  CHECK(probe_write32(kRvDmMemBase + RV_DM_WHERETO_REG_OFFSET, 0xdeadbeefu) ==
        0u);
  CHECK(probe_write32(kRvDmMemBase + RV_DM_ABSTRACTCMD_0_REG_OFFSET,
                      0xdeadbeefu) == 0u);
  CHECK(probe_write32(kRvDmMemBase + RV_DM_PROGRAM_BUFFER_0_REG_OFFSET,
                      0xdeadbeefu) == 0u);
  CHECK(probe_write32(kRvDmMemBase + RV_DM_FLAGS_0_REG_OFFSET, 0xdeadbeefu) ==
        0u);
  CHECK(abs_mmio_read32(kRvDmMemBase + RV_DM_WHERETO_REG_OFFSET) ==
        whereto_before);
  CHECK(abs_mmio_read32(kRvDmMemBase + RV_DM_FLAGS_0_REG_OFFSET) ==
        flags0_before);

  // Sub-word `sb`/`sh` writes (`be_i != 4'b1111`) to those same regions fault
  // with Store Access Fault (mcause = 7).
  CHECK(probe_write8(kRvDmMemBase + RV_DM_WHERETO_REG_OFFSET, 0x55u) ==
        kMcauseStoreAccessFault);
  CHECK(probe_write16(kRvDmMemBase + RV_DM_PROGRAM_BUFFER_0_REG_OFFSET,
                      0x5555u) == kMcauseStoreAccessFault);
  CHECK(probe_write8(kRvDmMemBase + RV_DM_FLAGS_0_REG_OFFSET, 0x55u) ==
        kMcauseStoreAccessFault);

  // By contrast, ROM (0x800..0xffc) sets `err_d = we_i`, faulting even on a
  // full-word 32-bit `sw` (`be_i = 4'b1111`).
  CHECK(probe_write32(kRvDmMemBase + kRvDmRom0Offset, 0xdeadbeefu) ==
        kMcauseStoreAccessFault);
  CHECK(abs_mmio_read32(kRvDmMemBase + kRvDmRom0Offset) == rom0_before);
}

/**
 * Test 5: `DATAADDR_0`/`DATAADDR_1` (`0x380..0x387`) Synchronous Zero-Clamp
 * Under `!dmcontrol_q.dmactive` (`dm_csrs.sv:612, 627, 647`) + Sub-Word Write
 * Fault (`dm_mem.sv:396`), and `ABSTRACTCMD_1..9` (`0x33c..0x35c`) Non-Zero
 * Reset Instructions (`dm_mem.sv:425-437` vs. `rv_dm.hjson` `resval: "0"`).
 */
static void test_rv_dm_data_dmactive_clamp_and_abstractcmd_resval(void) {
  LOG_INFO(
      "Test 5: DATA0/1 !dmactive zero-clamp + sb/sh fault and ABSTRACTCMD "
      "non-zero resvals");

  // 1. In `rv_dm.hjson:249-280`, DATAADDR_0 (0x380) and DATAADDR_1 (0x384) are
  // declared `swaccess: "rw"`. On the TL-UL `mem` bus (`dm_mem.sv:277-295,
  // 396`), 32-bit `sw` writes (`be_i = 4'b1111`) complete without bus error
  // (`err_d = 0`, `mcause = 0`), whereas sub-word `sb`/`sh` writes (`be_i !=
  // 4'b1111`) fault with Store Access Fault (`err_d = 1`, `mcause = 7`).
  // However, inside `dm_csrs.sv:612, 627, 647`, `if (!dmcontrol_q.dmactive)
  // data_q <= '0;` synchronously clamps `data_q` to 0 on every clock cycle
  // whenever `dmcontrol.dmactive == 0` (which cannot be set by the CPU on
  // `top_earlgrey` because `UseDmiInterface = 1` omits `rv_dm_dbg_reg_top`),
  // discarding all 32-bit CPU writes to DATAADDR_0/1!
  CHECK(probe_write32(kRvDmMemBase + kRvDmData0Offset, 0x11223344u) == 0u);
  CHECK(probe_write32(kRvDmMemBase + kRvDmData1Offset, 0xaabbccddu) == 0u);
  CHECK(abs_mmio_read32(kRvDmMemBase + kRvDmData0Offset) == 0x00000000u);
  CHECK(abs_mmio_read32(kRvDmMemBase + kRvDmData1Offset) == 0x00000000u);

  CHECK(probe_write8(kRvDmMemBase + kRvDmData0Offset, 0x99u) ==
        kMcauseStoreAccessFault);
  CHECK(probe_write16(kRvDmMemBase + kRvDmData0Offset + 2u, 0xeeffu) ==
        kMcauseStoreAccessFault);
  CHECK(abs_mmio_read32(kRvDmMemBase + kRvDmData0Offset) == 0x00000000u);

  // 2. In `rv_dm.hjson:122-205`, `ABSTRACTCMD_0..9` (`0x338..0x35c`) are all
  // specified with `resval: "0"`. However, `dm_mem.sv:425-437` combinationally
  // drives `abstract_cmd` with non-zero RISC-V instructions (`auipc`, `srli`,
  // `slli`, `nop` = 0x00000013, `csrr`, `ebreak` = 0x00100073).
  uint32_t acmd1 =
      abs_mmio_read32(kRvDmMemBase + RV_DM_ABSTRACTCMD_1_REG_OFFSET);
  uint32_t acmd4 =
      abs_mmio_read32(kRvDmMemBase + RV_DM_ABSTRACTCMD_4_REG_OFFSET);
  uint32_t acmd9 =
      abs_mmio_read32(kRvDmMemBase + RV_DM_ABSTRACTCMD_9_REG_OFFSET);
  LOG_INFO("ABSTRACTCMD_1=0x%08x, ABSTRACTCMD_4=0x%08x, ABSTRACTCMD_9=0x%08x",
           acmd1, acmd4, acmd9);
  CHECK(acmd1 == 0x00000517u);  // auipc a0, 0
  CHECK(acmd4 == 0x00000013u);  // nop (addi x0, x0, 0)
  CHECK(acmd9 == 0x00100073u);  // ebreak
}

bool test_main(void) {
  test_rv_dm_regs_decode_and_permit();
  test_rv_dm_mem_late_debug_gate();
  test_rv_dm_mem_stride_and_regions();
  test_rv_dm_mem_ro_write_err_behavior();
  test_rv_dm_data_dmactive_clamp_and_abstractcmd_resval();
  return true;
}
