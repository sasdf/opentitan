// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file rom_ctrl_errata_test.c
 * @brief CW340 FPGA & QEMU Errata Confirmation Test for `rom_ctrl` (P33).
 *
 * Empirically confirms on both physical CW340 FPGA silicon and QEMU:
 * - [rom_ctrl.sv:437-458] (INTENDED_SECURITY_HARDENING / SPEC_DOC_ERRATA):
 *   `ALERT_TEST.fatal` (`0x00`) emits a single one-shot alert handshake
 *   (`alert_test_set_d` clears on `alert_clr` in `prim_alert_sender.sv:175`)
 *   so `ALERT_CAUSE[60]` stays `0` after `RW1C` clear, rather than remaining
 *   permanently asserted until reset as `rom_ctrl.hjson:81` implies.
 * - [rom_ctrl.sv:183-197] (INTENDED_SECURITY_HARDENING):
 *   Reading the top 8 `EXP_DIGEST` words of the ROM window
 * (`0x0000ffe0..0x0000fffc`, scrambled with inverted Hsiao ECC bits by
 * `scramble_image.py:319-350`) passes corrupted `data_intg` through
 * `u_tl_adapter_rom` (`.EnableDataIntgPt(1)`) with `d_error = 0`, raising an
 * Ibex Internal Load Integrity NMI
 *   (`mcause = 0xffffffe0`, `mtval = 0x0000fffc`) and setting
 *   `RV_CORE_IBEX.ERR_STATUS.fatal_intg_err` (`bit 8`).
 * - [rom_ctrl_reg_pkg.sv:115-134] (INTENDED_SECURITY_HARDENING):
 *   `ROM_CTRL_REGS_PERMIT` rejects sub-word writes (`sb`/`sh`) to read-only
 *   `DIGEST_0..7` and `EXP_DIGEST_0..7` (`PERMIT = 4'b1111`) with Store Access
 *   Fault (`d_error = 1`, `mcause = 7`), while accepting byte-0 `sb` writes to
 *   read-only `FATAL_ALERT_CAUSE` (`0x04`, `PERMIT = 4'b0001`).
 */

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_alert_handler.h"
#include "sw/device/lib/dif/dif_rom_ctrl.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "alert_handler_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "rom_ctrl_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kRomCtrlRegsBase = TOP_EARLGREY_ROM_CTRL_REGS_BASE_ADDR,
  kRomCtrlRomBase = TOP_EARLGREY_ROM_CTRL_ROM_BASE_ADDR,
  kRomCtrlRomSize = TOP_EARLGREY_ROM_CTRL_ROM_SIZE_BYTES,
  kAlertHandlerBase = TOP_EARLGREY_ALERT_HANDLER_BASE_ADDR,
  kIbexBase = TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR,
  kRomCtrlAlertId = kTopEarlgreyAlertIdRomCtrlFatal,
  kIbexFatalHwErrAlertId = kTopEarlgreyAlertIdRvCoreIbexFatalHwErr,
};

static volatile bool store_access_fault_seen = false;
static volatile bool load_access_fault_seen = false;
static volatile bool load_integrity_fault_seen = false;
static volatile uint32_t load_integrity_fault_mtval = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  uint32_t mcause = ibex_mcause_read();
  if ((ibex_exc_t)(mcause & kIbexExcMax) == kIbexExcStoreAccessFault) {
    store_access_fault_seen = true;
  } else if ((ibex_exc_t)(mcause & kIbexExcMax) == kIbexExcLoadAccessFault) {
    load_access_fault_seen = true;
  } else {
    ottf_generic_fault_print(exc_info, "Unexpected Load/Store Fault", mcause);
    abort();
  }
}

void ottf_internal_isr(uint32_t *exc_info) {
  uint32_t mcause = ibex_mcause_read();
  if (mcause == 0xffffffe0u) {
    load_integrity_fault_seen = true;
    load_integrity_fault_mtval = ibex_mtval_read();
  } else {
    ottf_generic_fault_print(exc_info, "Unexpected Internal IRQ", mcause);
    abort();
  }
}

static void shadow_write32(uint32_t addr, uint32_t val) {
  abs_mmio_write32(addr, val);
  abs_mmio_write32(addr, val);
}

bool test_main(void) {
  // 1. Verify [rom_ctrl_reg_pkg.sv:115-134] (INTENDED_SECURITY_HARDENING):
  //    ROM_CTRL_REGS_PERMIT sub-word write wr_err on read-only CSRs.
  LOG_INFO(
      "Verifying [rom_ctrl_reg_pkg.sv:115-134] (INTENDED_SECURITY_HARDENING): "
      "ROM_CTRL_REGS_PERMIT sub-word write faults on RO DIGEST/EXP_DIGEST...");

  uint32_t d0 =
      abs_mmio_read32(kRomCtrlRegsBase + ROM_CTRL_DIGEST_0_REG_OFFSET);
  uint32_t e0 =
      abs_mmio_read32(kRomCtrlRegsBase + ROM_CTRL_EXP_DIGEST_0_REG_OFFSET);
  CHECK(d0 == e0 && d0 != 0u);

  // 32-bit writes to RO DIGEST_0 and EXP_DIGEST_0 succeed with d_error=0.
  store_access_fault_seen = false;
  abs_mmio_write32(kRomCtrlRegsBase + ROM_CTRL_DIGEST_0_REG_OFFSET, ~d0);
  abs_mmio_write32(kRomCtrlRegsBase + ROM_CTRL_EXP_DIGEST_0_REG_OFFSET, ~e0);
  CHECK(!store_access_fault_seen);

  // 8-bit write to RO FATAL_ALERT_CAUSE+0 (PERMIT = 4'b0001) succeeds with
  // d_error=0, whereas +1 raises StoreAccessFault!
  store_access_fault_seen = false;
  abs_mmio_write8(kRomCtrlRegsBase + ROM_CTRL_FATAL_ALERT_CAUSE_REG_OFFSET,
                  0x3u);
  CHECK(!store_access_fault_seen);

  store_access_fault_seen = false;
  abs_mmio_write8(kRomCtrlRegsBase + ROM_CTRL_FATAL_ALERT_CAUSE_REG_OFFSET + 1u,
                  0x1u);
  CHECK(store_access_fault_seen);

  // 8-bit write to RO DIGEST_0 and EXP_DIGEST_0 (PERMIT = 4'b1111) raises
  // StoreAccessFault!
  store_access_fault_seen = false;
  abs_mmio_write8(kRomCtrlRegsBase + ROM_CTRL_DIGEST_0_REG_OFFSET, 0xaau);
  CHECK(store_access_fault_seen);

  store_access_fault_seen = false;
  abs_mmio_write8(kRomCtrlRegsBase + ROM_CTRL_EXP_DIGEST_0_REG_OFFSET, 0x55u);
  CHECK(store_access_fault_seen);

  // 2. Verify [rom_ctrl.sv:437-458] (INTENDED_SECURITY_HARDENING):
  //    ALERT_TEST.fatal emits a one-shot alert handshake (does not stay latched
  //    after RW1C clear of ALERT_CAUSE[60]).
  LOG_INFO(
      "Verifying [rom_ctrl.sv:437-458] (INTENDED_SECURITY_HARDENING): "
      "ALERT_TEST.fatal one-shot handshake & RW1C clear...");

  uint32_t en_addr = kAlertHandlerBase +
                     ALERT_HANDLER_ALERT_EN_SHADOWED_0_REG_OFFSET +
                     kRomCtrlAlertId * sizeof(uint32_t);
  uint32_t class_addr = kAlertHandlerBase +
                        ALERT_HANDLER_ALERT_CLASS_SHADOWED_0_REG_OFFSET +
                        kRomCtrlAlertId * sizeof(uint32_t);
  uint32_t cause_addr = kAlertHandlerBase +
                        ALERT_HANDLER_ALERT_CAUSE_0_REG_OFFSET +
                        kRomCtrlAlertId * sizeof(uint32_t);

  shadow_write32(class_addr,
                 ALERT_HANDLER_ALERT_CLASS_SHADOWED_0_CLASS_A_0_VALUE_CLASSA);
  shadow_write32(en_addr, 1u);
  abs_mmio_write32(cause_addr, 1u);
  CHECK(abs_mmio_read32(cause_addr) == 0u);

  abs_mmio_write32(kRomCtrlRegsBase + ROM_CTRL_ALERT_TEST_REG_OFFSET,
                   1u << ROM_CTRL_ALERT_TEST_FATAL_BIT);
  CHECK(abs_mmio_read32(cause_addr) == 1u);

  abs_mmio_write32(cause_addr, 1u);
  CHECK(abs_mmio_read32(cause_addr) == 0u,
        "ALERT_CAUSE[60] must stay 0 after RW1C clear");
  shadow_write32(class_addr,
                 ALERT_HANDLER_ALERT_CLASS_SHADOWED_0_CLASS_A_0_VALUE_CLASSD);

  // 3. Verify [rom_ctrl.sv:183-197] (INTENDED_SECURITY_HARDENING):
  //    Reading top 8 ROM words (0x0000ffe0..0x0000fffc) raises Ibex Load
  //    Integrity NMI (mcause = 0xffffffe0) and sets ERR_STATUS.fatal_intg_err.
  LOG_INFO(
      "Verifying [rom_ctrl.sv:183-197] (INTENDED_SECURITY_HARDENING): Top 8 "
      "ROM words 0x0000ffe0..0x0000fffc raise Ibex Load Integrity NMI...");

  uint32_t orig_pmpaddr0 = 0;
  uint32_t orig_pmpcfg0 = 0;
  CSR_READ(CSR_REG_PMPADDR0, &orig_pmpaddr0);
  CSR_READ(CSR_REG_PMPCFG0, &orig_pmpcfg0);

  const uint32_t kRomNapotAddr =
      (kRomCtrlRomBase >> 2) | ((kRomCtrlRomSize - 1u) >> 3);
  CSR_WRITE(CSR_REG_PMPADDR0, kRomNapotAddr);
  CSR_WRITE(CSR_REG_PMPCFG0, (orig_pmpcfg0 & ~0xffu) | 0x9bu);
  uint32_t active_pmpcfg0 = 0;
  CSR_READ(CSR_REG_PMPCFG0, &active_pmpcfg0);
  if ((active_pmpcfg0 & 0xffu) != 0x9bu) {
    CSR_WRITE(CSR_REG_PMPCFG0, (orig_pmpcfg0 & ~0xffu) | 0x1bu);
  }

  uint32_t ibex_fatal_hw_en_addr =
      kAlertHandlerBase + ALERT_HANDLER_ALERT_EN_SHADOWED_0_REG_OFFSET +
      kIbexFatalHwErrAlertId * sizeof(uint32_t);
  shadow_write32(ibex_fatal_hw_en_addr, 0u);

  uint32_t top_word_addr = kRomCtrlRomBase + kRomCtrlRomSize - sizeof(uint32_t);
  load_integrity_fault_seen = false;
  load_integrity_fault_mtval = 0u;
  (void)abs_mmio_read32(top_word_addr);
  busy_spin_micros(5);
  CHECK(
      load_integrity_fault_seen && load_integrity_fault_mtval == top_word_addr,
      "Expected Load Integrity NMI on 0x%08x", top_word_addr);

  uint32_t ibex_err_status = abs_mmio_read32(kIbexBase + 0x54u);
  CHECK((ibex_err_status & (1u << 8)) != 0u,
        "Expected RV_CORE_IBEX.ERR_STATUS.fatal_intg_err set");
  abs_mmio_write32(kIbexBase + 0x54u, 1u << 8);

  CSR_WRITE(CSR_REG_PMPCFG0, orig_pmpcfg0);
  CSR_WRITE(CSR_REG_PMPADDR0, orig_pmpaddr0);

  LOG_INFO("All [rom_ctrl.sv:437-458..003] checks confirmed!");
  return true;
}
