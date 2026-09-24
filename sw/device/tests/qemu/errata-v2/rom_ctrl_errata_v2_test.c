// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file rom_ctrl_errata_v2_test.c
 * @brief Physical CW340 FPGA verification of Earlgrey v2 (`trunk-v2`) hardware,
 *        specification, and DIF discrepancies in `rom_ctrl` (`hw/ip/rom_ctrl`,
 *        `hw/top_earlgrey/rtl/autogen/earlgrey_pd_main.sv`,
 *        `hw/top_earlgrey/ip/xbar_main/rtl/autogen/xbar_main.sv`, and
 *        `sw/device/lib/dif/dif_rom_ctrl.h`).
 *
 * Verified behaviors:
 * 1. `hw/ip/rom_ctrl/rtl/rom_ctrl_reg_pkg.sv:122-141` &
 *    `hw/ip/rom_ctrl/rtl/rom_ctrl_regs_reg_top.sv:709-736`:
 *    `ROM_CTRL_REGS_PERMIT` rejects sub-word writes (`sb`/`sh`) to read-only
 *    `DIGEST_0..7` and `EXP_DIGEST_0..7` (`PERMIT = 4'b1111`) with a
 *    synchronous Store Access Fault (`d_error = 1`, `mcause = 7`), while
 *    accepting 32-bit writes (`sw`) to `DIGEST`/`EXP_DIGEST` and byte-0 writes
 *    (`sb`) to read-only `FATAL_ALERT_CAUSE` (`0x04`, `PERMIT = 4'b0001`).
 * 2. `hw/ip/rom_ctrl/rtl/rom_ctrl.sv:503-519` vs
 *    `hw/ip/rom_ctrl/data/rom_ctrl.hjson:98-100`:
 *    Writing `1` to `ALERT_TEST.fatal` (`0x411e0000`) emits a single one-shot
 *    alert handshake (`kTopEarlgreyAlertIdRomCtrlFatal = 59` in `trunk-v2`)
 *    rather than latching until hard reset, leaving `FATAL_ALERT_CAUSE == 0`
 *    and allowing `ALERT_CAUSE_59` to be cleared via RW1C.
 * 3. `sw/device/lib/dif/dif_rom_ctrl.h:28-48` &
 *    `sw/device/lib/dif/dif_rom_ctrl.c:24-35`:
 *    `dif_rom_ctrl_fatal_alert_cause_t` defines
 *    `kDifRomCtrlFatalAlertCauseNoError = 0`,
 *    `kDifRomCtrlFatalAlertCauseCheckerError =
 *     ROM_CTRL_FATAL_ALERT_CAUSE_CHECKER_ERROR_BIT` (`0`), and
 *    `kDifRomCtrlFatalAlertCauseIntegrityError =
 *     ROM_CTRL_FATAL_ALERT_CAUSE_INTEGRITY_ERROR_BIT` (`1`, bit index rather
 *    than bitmask `1 << 1`), so `kDifRomCtrlFatalAlertCauseCheckerError`
 *    collides with `kDifRomCtrlFatalAlertCauseNoError` (`0 == 0`) and compares
 *    equal to the `alert_causes` (`0`) returned by
 *    `dif_rom_ctrl_get_fatal_alert_cause()` on a healthy post-boot chip.
 * 4. `hw/ip/rom_ctrl/rtl/rom_ctrl.sv:194-224` &
 *    `hw/ip/rom_ctrl/util/scramble_image.py`:
 *    Reading the top 8 `EXP_DIGEST` words (`0x0006ffe0..0x0006fffc`, word
 *    indices `49144..49151` of the 192 KiB `trunk-v2` ROM window
 *    `0x00040000..0x0006ffff`) passes inverted Hsiao `(39,32)` ECC bits through
 *    `u_tl_adapter_rom` (`.EnableDataIntgPt(1)`) with `d_error = 0`, firing an
 *    asynchronous Ibex Internal Load Integrity NMI (`mcause = 0xffffffe0`,
 *    `mtval = 0x0006ffe0..0x0006fffc`) and setting
 *    `RV_CORE_IBEX.ERR_STATUS.FATAL_INTG_ERR` (bit 8) while leaving
 *    `ROM_CTRL_FATAL_ALERT_CAUSE == 0`.
 * 5. `hw/top_earlgrey/rtl/autogen/earlgrey_pd_main.sv:2516`,
 *    `hw/ip/rom_ctrl/rtl/rom_ctrl.sv:197`, and
 *    `hw/top_earlgrey/ip/xbar_main/rtl/autogen/xbar_main.sv:823-825`:
 *    In `trunk-v2`, `MemSizeRom` is `196608` bytes (`192 KiB = 0x30000`,
 *    non-power-of-2 `49,152` words, `SramAw = 16`), and `u_tl_adapter_rom`
 *    instantiates `.SramDepth(49152)` (`addr_miss_error =
 *    tl_i.a_address[17:2] >= 49152`). Because `xbar_main` restricts routing to
 *    `tl_rom_ctrl__rom_o` to `0x00040000 <= a_address < 0x00070000`, reading
 *    word `49152` (`0x00070000`, 4 bytes above `0x0006fffc`) is intercepted by
 *    `xbar_main`'s error responder (`d_error = 1`), raising a synchronous Load
 *    Access Fault (`mcause = 5`, with valid response data integrity) instead of
 *    reaching `u_tl_adapter_rom` or triggering a Load Integrity NMI.
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_rom_ctrl.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top/alert_handler_regs.h"
#include "hw/top/rom_ctrl_regs.h"
#include "hw/top/rv_core_ibex_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

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
  LOG_INFO("Starting rom_ctrl Earlgrey v2 FPGA verification test...");

  // 1. Verify ROM_CTRL_REGS_PERMIT sub-word write wr_err on read-only CSRs
  //    (hw/ip/rom_ctrl/rtl/rom_ctrl_reg_pkg.sv:122-141).
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
  // d_error=0, whereas bytes 1..3 (+1, +2, +3) raise StoreAccessFault
  // (mcause = 7).
  store_access_fault_seen = false;
  abs_mmio_write8(kRomCtrlRegsBase + ROM_CTRL_FATAL_ALERT_CAUSE_REG_OFFSET,
                  0x3u);
  CHECK(!store_access_fault_seen);

  for (uint32_t byte_off = 1u; byte_off <= 3u; ++byte_off) {
    store_access_fault_seen = false;
    abs_mmio_write8(
        kRomCtrlRegsBase + ROM_CTRL_FATAL_ALERT_CAUSE_REG_OFFSET + byte_off,
        0x1u);
    CHECK(store_access_fault_seen);
  }

  // 8-bit (`sb`) and 16-bit (`sh`) writes to RO DIGEST_0 and EXP_DIGEST_0
  // (PERMIT = 4'b1111) raise StoreAccessFault (mcause = 7).
  store_access_fault_seen = false;
  abs_mmio_write8(kRomCtrlRegsBase + ROM_CTRL_DIGEST_0_REG_OFFSET, 0xaau);
  CHECK(store_access_fault_seen);

  store_access_fault_seen = false;
  *(volatile uint16_t *)(uintptr_t)(kRomCtrlRegsBase +
                                    ROM_CTRL_DIGEST_0_REG_OFFSET) = 0xbbccu;
  CHECK(store_access_fault_seen);

  store_access_fault_seen = false;
  abs_mmio_write8(kRomCtrlRegsBase + ROM_CTRL_EXP_DIGEST_0_REG_OFFSET, 0x55u);
  CHECK(store_access_fault_seen);

  store_access_fault_seen = false;
  *(volatile uint16_t *)(uintptr_t)(kRomCtrlRegsBase +
                                    ROM_CTRL_EXP_DIGEST_0_REG_OFFSET) = 0xddeeu;
  CHECK(store_access_fault_seen);

  CHECK(abs_mmio_read32(kRomCtrlRegsBase + ROM_CTRL_DIGEST_0_REG_OFFSET) == d0);
  CHECK(abs_mmio_read32(kRomCtrlRegsBase + ROM_CTRL_EXP_DIGEST_0_REG_OFFSET) ==
        e0);
  LOG_INFO("Confirmed ROM_CTRL_REGS_PERMIT sub-word write faults.");

  // 2. Verify ALERT_TEST.fatal one-shot handshake & RW1C clear on trunk-v2
  //    (kTopEarlgreyAlertIdRomCtrlFatal = 59, rom_ctrl.sv:503-519).
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
  CHECK(abs_mmio_read32(kRomCtrlRegsBase +
                        ROM_CTRL_FATAL_ALERT_CAUSE_REG_OFFSET) == 0u);

  abs_mmio_write32(cause_addr, 1u);
  CHECK(abs_mmio_read32(cause_addr) == 0u);
  shadow_write32(class_addr,
                 ALERT_HANDLER_ALERT_CLASS_SHADOWED_0_CLASS_A_0_VALUE_CLASSD);
  LOG_INFO("Confirmed ALERT_TEST.fatal one-shot pulse (Alert ID %d).",
           kRomCtrlAlertId);

  // 3. Verify dif_rom_ctrl_fatal_alert_cause_t enum collision & unshifted bit
  //    indices vs dif_rom_ctrl_get_fatal_alert_cause() (dif_rom_ctrl.h:28-48).
  dif_rom_ctrl_t rom_ctrl;
  CHECK_DIF_OK(
      dif_rom_ctrl_init(mmio_region_from_addr(kRomCtrlRegsBase), &rom_ctrl));
  dif_rom_ctrl_fatal_alert_causes_t alert_causes = 0xffu;
  CHECK_DIF_OK(dif_rom_ctrl_get_fatal_alert_cause(&rom_ctrl, &alert_causes));
  CHECK(alert_causes == 0u);
  CHECK((uint32_t)kDifRomCtrlFatalAlertCauseNoError == 0u);
  CHECK((uint32_t)kDifRomCtrlFatalAlertCauseCheckerError == 0u);
  CHECK((uint32_t)kDifRomCtrlFatalAlertCauseCheckerError ==
        (uint32_t)kDifRomCtrlFatalAlertCauseNoError);
  CHECK(alert_causes == (uint32_t)kDifRomCtrlFatalAlertCauseCheckerError);
  CHECK((uint32_t)kDifRomCtrlFatalAlertCauseIntegrityError ==
        (1u << ROM_CTRL_FATAL_ALERT_CAUSE_CHECKER_ERROR_BIT));
  CHECK((((1u << ROM_CTRL_FATAL_ALERT_CAUSE_CHECKER_ERROR_BIT) &
          (uint32_t)kDifRomCtrlFatalAlertCauseCheckerError) == 0u));
  CHECK((((1u << ROM_CTRL_FATAL_ALERT_CAUSE_INTEGRITY_ERROR_BIT) &
          (uint32_t)kDifRomCtrlFatalAlertCauseIntegrityError) == 0u));
  CHECK((((1u << ROM_CTRL_FATAL_ALERT_CAUSE_CHECKER_ERROR_BIT) &
          (uint32_t)kDifRomCtrlFatalAlertCauseIntegrityError) != 0u));

  dif_rom_ctrl_digest_t digest;
  dif_rom_ctrl_digest_t exp_digest;
  CHECK_DIF_OK(dif_rom_ctrl_get_digest(&rom_ctrl, &digest));
  CHECK_DIF_OK(dif_rom_ctrl_get_expected_digest(&rom_ctrl, &exp_digest));
  for (size_t i = 0; i < ROM_CTRL_DIGEST_MULTIREG_COUNT; ++i) {
    CHECK(digest.digest[i] == exp_digest.digest[i]);
  }
  LOG_INFO("Confirmed dif_rom_ctrl_fatal_alert_cause_t enum collision.");

  // 4. Verify reading the top 8 EXP_DIGEST words of the 192 KiB v2 ROM
  //    (0x0006ffe0..0x0006fffc, word indices 49144..49151) raises Ibex Internal
  //    Load Integrity NMI (mcause = 0xffffffe0) and sets
  //    RV_CORE_IBEX.ERR_STATUS.FATAL_INTG_ERR, whereas word 49143 (0x0006ffdc)
  //    immediately below EXP_DIGEST reads cleanly without fault.
  CHECK(kRomCtrlRomBase == 0x00040000u);
  CHECK(kRomCtrlRomSize == 0x00030000u);

  uint32_t ibex_fatal_hw_en_addr =
      kAlertHandlerBase + ALERT_HANDLER_ALERT_EN_SHADOWED_0_REG_OFFSET +
      kIbexFatalHwErrAlertId * sizeof(uint32_t);
  shadow_write32(ibex_fatal_hw_en_addr, 0u);

  const uint32_t kExpDigestBase = kRomCtrlRomBase + kRomCtrlRomSize - 32u;
  CHECK(kExpDigestBase == 0x0006ffe0u);

  load_integrity_fault_seen = false;
  load_access_fault_seen = false;
  (void)abs_mmio_read32(kExpDigestBase - sizeof(uint32_t));
  busy_spin_micros(5);
  CHECK(!load_integrity_fault_seen);
  CHECK(!load_access_fault_seen);

  for (uint32_t i = 0u; i < 8u; ++i) {
    uint32_t word_addr = kExpDigestBase + i * sizeof(uint32_t);
    load_integrity_fault_seen = false;
    load_access_fault_seen = false;
    load_integrity_fault_mtval = 0u;
    (void)abs_mmio_read32(word_addr);
    busy_spin_micros(5);
    CHECK(load_integrity_fault_seen);
    CHECK(!load_access_fault_seen);
    CHECK(load_integrity_fault_mtval == word_addr);

    uint32_t ibex_err_status =
        abs_mmio_read32(kIbexBase + RV_CORE_IBEX_ERR_STATUS_REG_OFFSET);
    CHECK((ibex_err_status &
           (1u << RV_CORE_IBEX_ERR_STATUS_FATAL_INTG_ERR_BIT)) != 0u);
    abs_mmio_write32(kIbexBase + RV_CORE_IBEX_ERR_STATUS_REG_OFFSET,
                     1u << RV_CORE_IBEX_ERR_STATUS_FATAL_INTG_ERR_BIT);
    CHECK(abs_mmio_read32(kRomCtrlRegsBase +
                          ROM_CTRL_FATAL_ALERT_CAUSE_REG_OFFSET) == 0u);
  }
  LOG_INFO("Confirmed Ibex Load Integrity NMI (0xffffffe0) at 0x%08x..0x%08x.",
           kExpDigestBase, kExpDigestBase + 28u);

  // 5. Verify reading 0x00070000 (word index 49152, 4 bytes above 0x0006fffc
  //    inside the 16-bit SramAw=16 index space) is intercepted by xbar_main
  //    (ADDR_SIZE_ROM_CTRL__ROM = 0x30000) with a synchronous Load Access
  //    Fault (mcause = 5, d_error = 1) rather than Load Integrity NMI.
  uint32_t orig_pmpaddr7 = 0;
  uint32_t orig_pmpcfg1 = 0;
  CSR_READ(CSR_REG_PMPADDR7, &orig_pmpaddr7);
  CSR_READ(CSR_REG_PMPCFG1, &orig_pmpcfg1);

  const uint32_t kTail64kBase = kRomCtrlRomBase + kRomCtrlRomSize;
  const uint32_t kTail64kSize = 0x00010000u;
  const uint32_t kTailNapotAddr =
      (kTail64kBase >> 2) | ((kTail64kSize - 1u) >> 3);
  CSR_WRITE(CSR_REG_PMPADDR7, kTailNapotAddr);
  // Configure pmp7cfg (bits 31:24 of pmpcfg1) as NAPOT + LRW (0x9b).
  CSR_WRITE(CSR_REG_PMPCFG1, (orig_pmpcfg1 & 0x00ffffffu) | (0x9bu << 24));

  load_access_fault_seen = false;
  load_integrity_fault_seen = false;
  (void)abs_mmio_read32(kTail64kBase);
  busy_spin_micros(5);
  CHECK(load_access_fault_seen);
  CHECK(!load_integrity_fault_seen);
  LOG_INFO("Confirmed synchronous Load Access Fault (mcause=5) at 0x%08x.",
           kTail64kBase);

  LOG_INFO("All rom_ctrl Earlgrey v2 FPGA checks PASSED!");
  return true;
}
