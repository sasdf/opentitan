// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"
#include "sw/device/silicon_creator/lib/drivers/retention_sram.h"

#include "hw/top/lc_ctrl_regs.h"
#include "hw/top/rstmgr_regs.h"
#include "hw/top/rv_core_ibex_regs.h"
#include "hw/top/sram_ctrl_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kSramMainRegsBase = TOP_EARLGREY_SRAM_CTRL_MAIN_REGS_BASE_ADDR,
  kSramMainRamBase = TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_BASE_ADDR,
  kSramSecRegsBase = TOP_EARLGREY_SRAM_CTRL_SEC_REGS_BASE_ADDR,
  kSramSecRamBase = TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR,
  kSramMetaRegsBase = TOP_EARLGREY_SRAM_CTRL_META_REGS_BASE_ADDR,
  kSramRetRegsBase = TOP_EARLGREY_SRAM_CTRL_RET_REGS_BASE_ADDR,
  kSramRetRamBase = TOP_EARLGREY_SRAM_CTRL_RET_RAM_BASE_ADDR,
  kCheriotRegsBase = TOP_EARLGREY_CHERIOT_REGS_BASE_ADDR,
  kCheriotRevbmBase = TOP_EARLGREY_CHERIOT_REVBM_BASE_ADDR,
  kRvCoreIbexBase = TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR,
  kSpiHost0Base = TOP_EARLGREY_SPI_HOST0_BASE_ADDR,
  kSysrstCtrlBase = TOP_EARLGREY_SYSRST_CTRL_BASE_ADDR,
  kRstmgrBase = TOP_EARLGREY_RSTMGR_BASE_ADDR,
  kLcCtrlBase = TOP_EARLGREY_LC_CTRL_REGS_BASE_ADDR,
  kRiscvInstrAccessFault = 1,
  kRiscvLoadAccessFault = 5,
  kRiscvStoreAccessFault = 7,
  kRiscvRetInsn32 = 0x00008067u,
};

static volatile uint32_t g_fault_count = 0;
static volatile uint32_t g_last_mcause = 0;
static volatile uint32_t g_last_mepc = 0;
static volatile uintptr_t g_saved_return_pc = 0;

void ottf_exception_handler(uint32_t *exc_info) {
  uint32_t mcause = ibex_mcause_read();
  uint32_t mepc = ibex_mepc_read();
  g_fault_count++;
  g_last_mcause = mcause;
  g_last_mepc = mepc;

  if (mcause == kRiscvInstrAccessFault && g_saved_return_pc != 0) {
    exc_info[0] = (uint32_t)g_saved_return_pc;
    g_saved_return_pc = 0;
    return;
  }
  if (mcause == kRiscvLoadAccessFault || mcause == kRiscvStoreAccessFault) {
    return;
  }
  ottf_generic_fault_print(exc_info, "Unhandled exception", mcause);
  abort();
}

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

/**
 * Test 1 (Part A - v1 Item 1 on v2 + Part B - v2 4-Instance sram_ctrl):
 * Verify `top_earlgrey.sv:48, 95, 102, 155`, `earlgrey_pd_aon.sv:518-519`,
 * `earlgrey_pd_main.sv:2451-2452, 2497-2498, 2713-2714`, and
 * `xbar_main.sv:793-817`:
 * - All 4 `sram_ctrl` instances (`main`, `sec`, `meta`, `ret`) expose writable
 *   `EXEC` (`0x1c`) CSRs accepting `kMultiBitBool4True` (`0x6`).
 * - `u_sram_ctrl_main` (`0x10000000`) and `u_sram_ctrl_sec` (`0x10020000`)
 *   permit instruction fetch (`mcause = 0`) when `EXEC == 0x6`, and block
 *   instruction fetch (`mcause = 1`) when `EXEC == 0x9`
 * (`kMultiBitBool4False`).
 * - `u_sram_ctrl_ret` (`0x40600000`) ALWAYS faults with `mcause = 1`
 *   (`Instruction Access Fault`) even when `SRAM_CTRL_RET.EXEC == 0x6`, due to
 *   `SramCtrlRetInstrExec = 0`, `.lc_hw_debug_en_i(Off)`,
 *   `.otp_en_sram_ifetch_i(MuBi8False)`, and `xbar_main.sv:793-817` (`corei`
 *   omitting `TlPeri`).
 */
static void test_sram_ifetch_and_v2_sec_sram(void) {
  LOG_INFO(
      "Test 1: 4-instance sram_ctrl EXEC (0x1c) vs main/sec/ret ifetch gating");

  CSR_WRITE(CSR_REG_PMPADDR7, 0x7fffffffu);
  CSR_SET_BITS(CSR_REG_PMPCFG1, 0x9fu << 24);
  icache_invalidate();

  uint32_t orig_main_exec =
      abs_mmio_read32(kSramMainRegsBase + SRAM_CTRL_EXEC_REG_OFFSET);
  uint32_t orig_sec_exec =
      abs_mmio_read32(kSramSecRegsBase + SRAM_CTRL_EXEC_REG_OFFSET);
  uint32_t orig_meta_exec =
      abs_mmio_read32(kSramMetaRegsBase + SRAM_CTRL_EXEC_REG_OFFSET);
  uint32_t orig_ret_exec =
      abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_EXEC_REG_OFFSET);

  // All 4 sram_ctrl instances accept kMultiBitBool4True (0x6) in EXEC (0x1c).
  abs_mmio_write32(kSramMainRegsBase + SRAM_CTRL_EXEC_REG_OFFSET,
                   kMultiBitBool4True);
  abs_mmio_write32(kSramSecRegsBase + SRAM_CTRL_EXEC_REG_OFFSET,
                   kMultiBitBool4True);
  abs_mmio_write32(kSramMetaRegsBase + SRAM_CTRL_EXEC_REG_OFFSET,
                   kMultiBitBool4True);
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_EXEC_REG_OFFSET,
                   kMultiBitBool4True);

  CHECK(abs_mmio_read32(kSramMainRegsBase + SRAM_CTRL_EXEC_REG_OFFSET) ==
            kMultiBitBool4True,
        "Expected SRAM_CTRL_MAIN.EXEC (0x1c) == 0x6");
  CHECK(abs_mmio_read32(kSramSecRegsBase + SRAM_CTRL_EXEC_REG_OFFSET) ==
            kMultiBitBool4True,
        "Expected SRAM_CTRL_SEC.EXEC (0x1c) == 0x6");
  CHECK(abs_mmio_read32(kSramMetaRegsBase + SRAM_CTRL_EXEC_REG_OFFSET) ==
            kMultiBitBool4True,
        "Expected SRAM_CTRL_META.EXEC (0x1c) == 0x6");
  CHECK(abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_EXEC_REG_OFFSET) ==
            kMultiBitBool4True,
        "Expected SRAM_CTRL_RET.EXEC (0x1c) == 0x6");

  // 1a. Main SRAM (`0x10000000`) executes `ret` with 0 faults when EXEC=0x6,
  // and faults with `mcause = 1` when `SRAM_CTRL_MAIN.EXEC == 0x9`.
  s_main_sram_code_buf[0] = kRiscvRetInsn32;
  s_main_sram_code_buf[1] = kRiscvRetInsn32;
  icache_invalidate();
  g_fault_count = 0;
  call_fn_catching_instr_fault((uintptr_t)&s_main_sram_code_buf[0]);
  CHECK(g_fault_count == 0u,
        "Expected Main SRAM instruction fetch to succeed with EXEC=0x6");

  abs_mmio_write32(kSramMainRegsBase + SRAM_CTRL_EXEC_REG_OFFSET,
                   kMultiBitBool4False);
  icache_invalidate();
  g_fault_count = 0;
  g_last_mcause = 0;
  call_fn_catching_instr_fault((uintptr_t)&s_main_sram_code_buf[0]);
  CHECK(g_fault_count == 1u && g_last_mcause == kRiscvInstrAccessFault,
        "Expected Main SRAM ifetch to fault with mcause=1 when EXEC=0x9");
  abs_mmio_write32(kSramMainRegsBase + SRAM_CTRL_EXEC_REG_OFFSET,
                   kMultiBitBool4True);
  icache_invalidate();

  // 1b. New in v2: Sec SRAM (`0x10020000`, `SramCtrlSecInstrExec=1`) executes
  // `ret` when `SRAM_CTRL_SEC.EXEC == 0x6`, and traps with `mcause = 1` when
  // `SRAM_CTRL_SEC.EXEC == 0x9` (`kMultiBitBool4False`).
  // First trigger CTRL.INIT on `u_sram_ctrl_sec` (`0x411d0014`) so all 64 KiB
  // have valid 39-bit ECC integrity bits when Ibex prefetches cachelines.
  abs_mmio_write32(kSramSecRegsBase + SRAM_CTRL_CTRL_REG_OFFSET,
                   1u << SRAM_CTRL_CTRL_INIT_BIT);
  for (int i = 0; i < 10000; ++i) {
    if ((abs_mmio_read32(kSramSecRegsBase + SRAM_CTRL_STATUS_REG_OFFSET) &
         (1u << SRAM_CTRL_STATUS_INIT_DONE_BIT)) != 0u) {
      break;
    }
  }
  CHECK((abs_mmio_read32(kSramSecRegsBase + SRAM_CTRL_STATUS_REG_OFFSET) &
         (1u << SRAM_CTRL_STATUS_INIT_DONE_BIT)) != 0u,
        "Expected SRAM_CTRL_SEC.STATUS.INIT_DONE == 1 after CTRL.INIT");

  uintptr_t sec_sram_code_addr = kSramSecRamBase + 0x100u;
  for (uint32_t off = 0; off < 64u; off += 4u) {
    abs_mmio_write32(sec_sram_code_addr + off, kRiscvRetInsn32);
  }
  icache_invalidate();

  g_fault_count = 0;
  call_fn_catching_instr_fault(sec_sram_code_addr);
  CHECK(g_fault_count == 0u,
        "Expected Sec SRAM (0x10020100) ifetch to succeed with EXEC=0x6");

  abs_mmio_write32(kSramSecRegsBase + SRAM_CTRL_EXEC_REG_OFFSET,
                   kMultiBitBool4False);
  icache_invalidate();
  g_fault_count = 0;
  g_last_mcause = 0;
  call_fn_catching_instr_fault(sec_sram_code_addr);
  CHECK(g_fault_count == 1u && g_last_mcause == kRiscvInstrAccessFault,
        "Expected Sec SRAM ifetch to fault with mcause=1 when EXEC=0x9");

  // 1c. Retention SRAM (`0x40600000`) ALWAYS faults with `mcause = 1` even
  // though `SRAM_CTRL_RET.EXEC == 0x6`.
  uintptr_t ret_sram_code_addr =
      kSramRetRamBase + offsetof(retention_sram_t, owner);
  abs_mmio_write32(ret_sram_code_addr, kRiscvRetInsn32);
  abs_mmio_write32(ret_sram_code_addr + 4u, kRiscvRetInsn32);
  icache_invalidate();

  g_fault_count = 0;
  g_last_mcause = 0;
  call_fn_catching_instr_fault(ret_sram_code_addr);
  CHECK(
      g_fault_count == 1u && g_last_mcause == kRiscvInstrAccessFault,
      "Expected Retention SRAM ifetch to fault with mcause=1 despite EXEC=0x6");

  abs_mmio_write32(kSramMainRegsBase + SRAM_CTRL_EXEC_REG_OFFSET,
                   orig_main_exec);
  abs_mmio_write32(kSramSecRegsBase + SRAM_CTRL_EXEC_REG_OFFSET, orig_sec_exec);
  abs_mmio_write32(kSramMetaRegsBase + SRAM_CTRL_EXEC_REG_OFFSET,
                   orig_meta_exec);
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_EXEC_REG_OFFSET, orig_ret_exec);
}

/**
 * Test 2 (Part A - v1 Item 2 & Item 3 on v2):
 * Verify:
 * - `reggen` `SwAccessWO` reads return `0` with `d_error = 0` (`CTRL`,
 *   `ALERT_TEST`), and full-word `SwAccessRO` writes are ignored with
 *   `d_error = 0` (`STATUS`).
 * - In `trunk-v2`, `sram_ctrl` added `READBACK_REGWEN` (`0x20`) and `READBACK`
 *   (`0x24`), shifting the first unmapped `addrmiss` offset inside the 64-byte
 *   `ADDR_MASK = 0x3f` window from `0x24` to `0x28`.
 * - Two-stage crossbar `ADDR_MASK` vs. `addrmiss` faults (`SPI_HOST0` at
 *   `0x40300038` within `ADDR_MASK = 0x3f`, `SYSRST_CTRL` at `0x404300AC`
 *   within `ADDR_MASK = 0xff`), plus removed v1 peripherals (`PATTGEN` at
 *   `0x400e0000`, `PWM` at `0x40450000`, `OTP_CTRL__PRIM` at `0x40132000`)
 *   routing through `ADDR_MASK_PERI` to `xbar_peri`'s `tlul_err_resp`
 *   (`mcause = 5 / 7`).
 */
static void test_reggen_wo_ro_and_xbar_decode_windows(void) {
  LOG_INFO(
      "Test 2: reggen WO/RO d_error=0, sram_ctrl 0x20/0x24 READBACK vs 0x28 "
      "addrmiss, and xbar ADDR_MASK / removed v1 aperture faults");

  // 2a. SwAccessWO reads return 0 without bus fault.
  g_fault_count = 0;
  CHECK(abs_mmio_read32(kSramMainRegsBase + SRAM_CTRL_CTRL_REG_OFFSET) == 0u,
        "Expected SRAM_CTRL_MAIN.CTRL (WO) read == 0");
  CHECK(abs_mmio_read32(kSramMetaRegsBase + SRAM_CTRL_CTRL_REG_OFFSET) == 0u,
        "Expected SRAM_CTRL_META.CTRL (WO) read == 0");
  CHECK(abs_mmio_read32(kRstmgrBase + RSTMGR_ALERT_TEST_REG_OFFSET) == 0u,
        "Expected RSTMGR.ALERT_TEST (WO) read == 0");
  CHECK(abs_mmio_read32(kCheriotRegsBase) == 0u,
        "Expected CHERIOT.ALERT_TEST (0x411b0000, WO) read == 0");
  CHECK(g_fault_count == 0u, "SwAccessWO reads must not fault");

  // 2b. Full-word writes to SwAccessRO registers are ignored without bus fault.
  uint32_t main_status =
      abs_mmio_read32(kSramMainRegsBase + SRAM_CTRL_STATUS_REG_OFFSET);
  uint32_t lc_status = abs_mmio_read32(kLcCtrlBase + LC_CTRL_STATUS_REG_OFFSET);
  abs_mmio_write32(kSramMainRegsBase + SRAM_CTRL_STATUS_REG_OFFSET,
                   0xFFFFFFFFu);
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_STATUS_REG_OFFSET, 0xFFFFFFFFu);
  CHECK(g_fault_count == 0u, "SwAccessRO full-word writes must not fault");
  CHECK(abs_mmio_read32(kSramMainRegsBase + SRAM_CTRL_STATUS_REG_OFFSET) ==
            main_status,
        "Expected SRAM_CTRL_MAIN.STATUS unchanged");
  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_STATUS_REG_OFFSET) == lc_status,
        "Expected LC_CTRL.STATUS unchanged");

  // 2c. In trunk-v2 sram_ctrl, 0x20 (READBACK_REGWEN, reset 0x1) and 0x24
  // (READBACK, reset 0x9) are mapped, while 0x28 is the first unmapped offset
  // in the 64-byte ADDR_MASK=0x3f window and raises addrmiss (mcause=5/7).
  CHECK(abs_mmio_read32(kSramMainRegsBase +
                        SRAM_CTRL_READBACK_REGWEN_REG_OFFSET) == 1u,
        "Expected SRAM_CTRL_MAIN.READBACK_REGWEN (0x20) == 1");
  CHECK(abs_mmio_read32(kSramMainRegsBase + SRAM_CTRL_READBACK_REG_OFFSET) ==
            kMultiBitBool4False,
        "Expected SRAM_CTRL_MAIN.READBACK (0x24) == 0x9");
  CHECK(g_fault_count == 0u, "0x20 and 0x24 must not fault in v2 sram_ctrl");

  g_fault_count = 0;
  g_last_mcause = 0;
  (void)abs_mmio_read32(kSramMainRegsBase + 0x28u);
  CHECK(g_fault_count == 1u && g_last_mcause == kRiscvLoadAccessFault,
        "Expected unmapped sram_ctrl offset 0x28 to fault with mcause=5");

  g_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write32(kSramMainRegsBase + 0x28u, 0u);
  CHECK(g_fault_count == 1u && g_last_mcause == kRiscvStoreAccessFault,
        "Expected unmapped sram_ctrl offset 0x28 store to fault with mcause=7");

  // 2d. Verify mapped boundary CSRs (`0x40300034` in SPI_HOST0 and
  // `0x404300A8` in SYSRST_CTRL) succeed with 0 faults, whereas `0x40300038`,
  // `0x404300AC`, and removed v1 `xbar_peri` apertures (`0x400e0000`,
  // `0x40450000`, `0x40132000`) fault with mcause=5 on load and mcause=7 on
  // store.
  g_fault_count = 0;
  (void)abs_mmio_read32(kSpiHost0Base + 0x34u);
  (void)abs_mmio_read32(kSysrstCtrlBase + 0xA8u);
  CHECK(g_fault_count == 0u,
        "Expected mapped SPI_HOST0+0x34 and SYSRST_CTRL+0xA8 reads to succeed");

  const uintptr_t fault_addrs[] = {
      kSpiHost0Base + 0x38u, kSysrstCtrlBase + 0xACu,
      0x400e0000u,  // Removed v1 PATTGEN aperture in xbar_peri
      0x40450000u,  // Removed v1 PWM_AON aperture in xbar_peri
      0x40132000u,  // Removed v1 OTP_CTRL__PRIM aperture in xbar_peri
  };
  for (size_t i = 0; i < sizeof(fault_addrs) / sizeof(fault_addrs[0]); ++i) {
    g_fault_count = 0;
    g_last_mcause = 0;
    (void)abs_mmio_read32(fault_addrs[i]);
    CHECK(g_fault_count == 1u && g_last_mcause == kRiscvLoadAccessFault,
          "Expected load fault (mcause=5) at 0x%08x", (uint32_t)fault_addrs[i]);

    g_fault_count = 0;
    g_last_mcause = 0;
    abs_mmio_write32(fault_addrs[i], 0u);
    CHECK(g_fault_count == 1u && g_last_mcause == kRiscvStoreAccessFault,
          "Expected store fault (mcause=7) at 0x%08x",
          (uint32_t)fault_addrs[i]);
  }
}

/**
 * Test 3 (Part B - NEW IN V2):
 * Verify `u_sram_ctrl_meta` (`0x411a0000`) & `u_cheriot` (`0x411b0000` /
 * `0x11000000`) top-level integration in `earlgrey_pd_main.sv:2642-2722` and
 * `tl_main_pkg.sv:37-40, 70-73`:
 * 1. Trigger `CTRL.INIT` (`0x2`) on `u_sram_ctrl_meta` (`0x411a0014`) so
 *    `STATUS.INIT_DONE` (`bit 5`) asserts (`0x20`), and verify `EXEC = 0x6`
 *    (`0x1c`) and `READBACK = 0x6` (`0x24`) are writable, yet its `38,912`-byte
 *    RAM port is wired exclusively to `u_cheriot.meta_sram_tl_o`
 *    (`earlgrey_pd_main.sv:2720`).
 * 2. When `RV_CORE_IBEX.CHERIOT_ENA` (`0x411f0060`) is `MuBi4False` (`0x9`),
 *    `u_cheriot.u_cheriot_access_check_sys` (`cheriot_access_check.sv:61`)
 *    blocks all `xbar_main` loads/stores to `CHERIOT__REVBM`
 *    (`0x11000000..0x11000bff`, `3 KiB` of `u_sram_ctrl_meta`) with
 *    `mcause = 5 / 7`, and instruction fetch (`jalr`) to `0x11000000` traps
 *    with `mcause = 1` (`xbar_main.sv:793-817` omits `CHERIOT__REVBM` from
 *    `corei`).
 * 3. The remaining `35 KiB` of `u_sram_ctrl_meta` (`0x11000c00..0x110097ff`) is
 *    unmapped on `xbar_main` (`ADDR_SIZE_CHERIOT__REVBM = 0x00000c00` in
 *    `tl_main_pkg.sv:73`), faulting with `mcause = 5 / 7`.
 * 4. `ADDR_SPACE_CHERIOT__REGS` (`0x411b0000`) has `ADDR_MASK = 0x00000003`
 *    (`tl_main_pkg.sv:72`, 4 bytes total), so offset `0x411b0004` misses
 *    `~(32'h3)` in `xbar_main` and faults with `mcause = 5 / 7`.
 */
static void test_v2_sram_ctrl_meta_and_cheriot_xbar_integration(void) {
  LOG_INFO(
      "Test 3 [NEW_IN_V2]: u_sram_ctrl_meta (0x411a0000) vs u_cheriot "
      "(0x11000000 / 0x411b0000) top-level integration");

  // Verify RV_CORE_IBEX.CHERIOT_ENA (0x411f0060) is at its reset default 0x9.
  CHECK(
      abs_mmio_read32(kRvCoreIbexBase + RV_CORE_IBEX_CHERIOT_ENA_REG_OFFSET) ==
          kMultiBitBool4False,
      "Expected RV_CORE_IBEX.CHERIOT_ENA == 0x9 (MuBi4False)");

  // 3a. Trigger CTRL.INIT on u_sram_ctrl_meta (`0x411a0014`) and wait for
  // STATUS.INIT_DONE (bit 5 = 0x20), then verify READBACK (0x24) is writable.
  abs_mmio_write32(kSramMetaRegsBase + SRAM_CTRL_CTRL_REG_OFFSET,
                   1u << SRAM_CTRL_CTRL_INIT_BIT);
  uint32_t meta_status = 0;
  for (int i = 0; i < 10000; ++i) {
    meta_status =
        abs_mmio_read32(kSramMetaRegsBase + SRAM_CTRL_STATUS_REG_OFFSET);
    if ((meta_status & (1u << SRAM_CTRL_STATUS_INIT_DONE_BIT)) != 0u) {
      break;
    }
  }
  CHECK((meta_status & (1u << SRAM_CTRL_STATUS_INIT_DONE_BIT)) != 0u,
        "Expected SRAM_CTRL_META.STATUS.INIT_DONE == 1 after CTRL.INIT, got "
        "0x%08x",
        meta_status);

  abs_mmio_write32(kSramMetaRegsBase + SRAM_CTRL_READBACK_REG_OFFSET,
                   kMultiBitBool4True);
  CHECK(abs_mmio_read32(kSramMetaRegsBase + SRAM_CTRL_READBACK_REG_OFFSET) ==
            kMultiBitBool4True,
        "Expected SRAM_CTRL_META.READBACK == 0x6");
  abs_mmio_write32(kSramMetaRegsBase + SRAM_CTRL_READBACK_REG_OFFSET,
                   kMultiBitBool4False);

  // 3b. Despite SRAM_CTRL_META.STATUS.INIT_DONE == 1 and writable CSRs,
  // accessing `0x11000000` (`CHERIOT__REVBM`, backed by `u_sram_ctrl_meta`)
  // traps with mcause=5 (load), mcause=7 (store), and mcause=1 (ifetch).
  g_fault_count = 0;
  g_last_mcause = 0;
  (void)abs_mmio_read32(kCheriotRevbmBase);
  CHECK(
      g_fault_count == 1u && g_last_mcause == kRiscvLoadAccessFault,
      "Expected load from CHERIOT__REVBM (0x11000000) to fault with mcause=5");

  g_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write32(kCheriotRevbmBase, 0x12345678u);
  CHECK(g_fault_count == 1u && g_last_mcause == kRiscvStoreAccessFault,
        "Expected store to CHERIOT__REVBM (0x11000000) to fault with mcause=7");

  g_fault_count = 0;
  g_last_mcause = 0;
  call_fn_catching_instr_fault(kCheriotRevbmBase);
  CHECK(
      g_fault_count == 1u && g_last_mcause == kRiscvInstrAccessFault,
      "Expected ifetch at CHERIOT__REVBM (0x11000000) to fault with mcause=1");

  // 3c. Offset 0x11000c00 (`MetaNvmTagBase`, byte 3,072 of the 38,912-byte
  // `u_sram_ctrl_meta`) lies outside `ADDR_SIZE_CHERIOT__REVBM` (`0xc00`) and
  // faults at `xbar_main` (`dev_sel_s1n_38 = 5'd29`).
  g_fault_count = 0;
  g_last_mcause = 0;
  (void)abs_mmio_read32(kCheriotRevbmBase + 0x0c00u);
  CHECK(g_fault_count == 1u && g_last_mcause == kRiscvLoadAccessFault,
        "Expected load at unmapped MetaNvmTagBase (0x11000c00) to fault with "
        "mcause=5");

  g_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write32(kCheriotRevbmBase + 0x0c00u, 0x12345678u);
  CHECK(g_fault_count == 1u && g_last_mcause == kRiscvStoreAccessFault,
        "Expected store at unmapped MetaNvmTagBase (0x11000c00) to fault with "
        "mcause=7");

  // 3d. `ADDR_SPACE_CHERIOT__REGS` (`0x411b0000`) has `ADDR_MASK = 0x3` (4
  // bytes), so `0x411b0004` faults with `mcause = 5` on load and `mcause = 7`
  // on store.
  g_fault_count = 0;
  g_last_mcause = 0;
  (void)abs_mmio_read32(kCheriotRegsBase + 4u);
  CHECK(g_fault_count == 1u && g_last_mcause == kRiscvLoadAccessFault,
        "Expected load at 0x411b0004 (outside CHERIOT__REGS ADDR_MASK=0x3) to "
        "fault with mcause=5");

  g_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write32(kCheriotRegsBase + 4u, 0u);
  CHECK(g_fault_count == 1u && g_last_mcause == kRiscvStoreAccessFault,
        "Expected store at 0x411b0004 (outside CHERIOT__REGS ADDR_MASK=0x3) to "
        "fault with mcause=7");
}

bool test_main(void) {
  LOG_INFO("=== top_earlgrey Earlgrey v2 Errata Verification Test (P37) ===");
  test_sram_ifetch_and_v2_sec_sram();
  test_reggen_wo_ro_and_xbar_decode_windows();
  test_v2_sram_ctrl_meta_and_cheriot_xbar_integration();
  LOG_INFO("=== All top_earlgrey v2 errata checks PASSED ===");
  return true;
}
