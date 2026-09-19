// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_alert_handler.h"
#include "sw/device/lib/dif/dif_rom_ctrl.h"
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
  kRomCtrlAlertId = kTopEarlgreyAlertIdRomCtrlFatal,
};

static volatile bool store_access_fault_seen = false;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  uint32_t mcause = ibex_mcause_read();
  if ((ibex_exc_t)(mcause & kIbexExcMax) == kIbexExcStoreAccessFault) {
    store_access_fault_seen = true;
  } else {
    ottf_generic_fault_print(exc_info, "Unexpected Load/Store Fault", mcause);
    abort();
  }
}

static void shadow_write32(uint32_t addr, uint32_t val) {
  abs_mmio_write32(addr, val);
  abs_mmio_write32(addr, val);
}

static void test_csr_semantics_and_digests(void) {
  dif_rom_ctrl_t rom_ctrl;
  CHECK_DIF_OK(
      dif_rom_ctrl_init(mmio_region_from_addr(kRomCtrlRegsBase), &rom_ctrl));

  // 1. Verify FATAL_ALERT_CAUSE is 0 after healthy boot and is read-only.
  dif_rom_ctrl_fatal_alert_causes_t causes;
  CHECK_DIF_OK(dif_rom_ctrl_get_fatal_alert_cause(&rom_ctrl, &causes));
  CHECK(causes == 0, "Expected FATAL_ALERT_CAUSE == 0, got 0x%x", causes);

  abs_mmio_write32(kRomCtrlRegsBase + ROM_CTRL_FATAL_ALERT_CAUSE_REG_OFFSET,
                   0x3u);
  CHECK_DIF_OK(dif_rom_ctrl_get_fatal_alert_cause(&rom_ctrl, &causes));
  CHECK(causes == 0, "FATAL_ALERT_CAUSE should be RO, got 0x%x", causes);

  // 2. Verify ALERT_TEST is write-only (reads return 0) and writing 0 is a
  // no-op.
  abs_mmio_write32(kRomCtrlRegsBase + ROM_CTRL_ALERT_TEST_REG_OFFSET, 0u);
  uint32_t alert_test_read =
      abs_mmio_read32(kRomCtrlRegsBase + ROM_CTRL_ALERT_TEST_REG_OFFSET);
  CHECK(alert_test_read == 0u, "ALERT_TEST should read as 0, got 0x%x",
        alert_test_read);

  // 3. Verify DIGEST_0..7 and EXP_DIGEST_0..7 match, are non-trivial (neither
  // all-zeros nor all-ones), and are strictly read-only.
  dif_rom_ctrl_digest_t digest;
  dif_rom_ctrl_digest_t exp_digest;
  CHECK_DIF_OK(dif_rom_ctrl_get_digest(&rom_ctrl, &digest));
  CHECK_DIF_OK(dif_rom_ctrl_get_expected_digest(&rom_ctrl, &exp_digest));
  CHECK_ARRAYS_EQ(digest.digest, exp_digest.digest,
                  ROM_CTRL_DIGEST_MULTIREG_COUNT);

  bool any_nonzero = false;
  bool any_non_ones = false;
  for (size_t i = 0; i < ROM_CTRL_DIGEST_MULTIREG_COUNT; ++i) {
    if (digest.digest[i] != 0u) {
      any_nonzero = true;
    }
    if (digest.digest[i] != 0xffffffffu) {
      any_non_ones = true;
    }
    uint32_t d_off =
        ROM_CTRL_DIGEST_0_REG_OFFSET + (uint32_t)(i * sizeof(uint32_t));
    uint32_t e_off =
        ROM_CTRL_EXP_DIGEST_0_REG_OFFSET + (uint32_t)(i * sizeof(uint32_t));
    abs_mmio_write32(kRomCtrlRegsBase + d_off, ~digest.digest[i]);
    abs_mmio_write32(kRomCtrlRegsBase + e_off, ~exp_digest.digest[i]);
    CHECK(abs_mmio_read32(kRomCtrlRegsBase + d_off) == digest.digest[i],
          "DIGEST_%u should be RO", i);
    CHECK(abs_mmio_read32(kRomCtrlRegsBase + e_off) == exp_digest.digest[i],
          "EXP_DIGEST_%u should be RO", i);
  }
  CHECK(any_nonzero && any_non_ones,
        "ROM digest must be neither all-zeros nor all-ones");
}

static void test_alert_test_pulse_and_clear(void) {
  uint32_t en_addr = kAlertHandlerBase +
                     ALERT_HANDLER_ALERT_EN_SHADOWED_0_REG_OFFSET +
                     kRomCtrlAlertId * sizeof(uint32_t);
  uint32_t class_addr = kAlertHandlerBase +
                        ALERT_HANDLER_ALERT_CLASS_SHADOWED_0_REG_OFFSET +
                        kRomCtrlAlertId * sizeof(uint32_t);
  uint32_t cause_addr = kAlertHandlerBase +
                        ALERT_HANDLER_ALERT_CAUSE_0_REG_OFFSET +
                        kRomCtrlAlertId * sizeof(uint32_t);

  // Route rom_ctrl fatal alert (ID 60) to Class A (which is disabled in OTTF)
  // while keeping ALERT_EN_SHADOWED[60] = 1 so ALERT_CAUSE[60] records alerts.
  shadow_write32(class_addr, ALERT_HANDLER_ALERT_CLASS_SHADOWED_0_CLASS_A_0_VALUE_CLASSA);
  shadow_write32(en_addr, 1u);
  abs_mmio_write32(cause_addr, 1u);
  CHECK(abs_mmio_read32(cause_addr) == 0u,
        "Expected ALERT_CAUSE[60] to be 0 before ALERT_TEST");

  // Trigger ALERT_TEST = 1 -> should pulse once, setting ALERT_CAUSE[60] = 1.
  abs_mmio_write32(kRomCtrlRegsBase + ROM_CTRL_ALERT_TEST_REG_OFFSET,
                   1u << ROM_CTRL_ALERT_TEST_FATAL_BIT);
  CHECK(abs_mmio_read32(cause_addr) == 1u,
        "Expected ALERT_CAUSE[60] == 1 after first ALERT_TEST write");

  // Clear ALERT_CAUSE[60] (rw1c). Because ALERT_TEST is a pulse (not latched),
  // ALERT_CAUSE[60] must remain 0 after clearing.
  abs_mmio_write32(cause_addr, 1u);
  CHECK(abs_mmio_read32(cause_addr) == 0u,
        "ALERT_CAUSE[60] remained 1 after rw1c clear (ALERT_TEST was latched!)");

  // Trigger ALERT_TEST = 1 a second time without writing 0 first -> must fire
  // again.
  abs_mmio_write32(kRomCtrlRegsBase + ROM_CTRL_ALERT_TEST_REG_OFFSET,
                   1u << ROM_CTRL_ALERT_TEST_FATAL_BIT);
  CHECK(abs_mmio_read32(cause_addr) == 1u,
        "Expected ALERT_CAUSE[60] == 1 after second ALERT_TEST write");

  // Clean up and restore Class D routing.
  abs_mmio_write32(cause_addr, 1u);
  CHECK(abs_mmio_read32(cause_addr) == 0u,
        "Expected ALERT_CAUSE[60] == 0 after final clear");
  shadow_write32(class_addr, ALERT_HANDLER_ALERT_CLASS_SHADOWED_0_CLASS_A_0_VALUE_CLASSD);
}

static void test_rom_write_protection(void) {
  // Under sival_rom_ext, ROM_EXT clears ePMP entries 0..7 before jumping to
  // BL0 while MSECCFG.MMWP == 1 denies unmatched addresses; under
  // rom_with_fake_keys, ROM maps the ROM window as read-only in ePMP entry 2.
  // Configure highest-priority PMP entry 0 as NAPOT Read+Write covering the
  // ROM window so CPU loads succeed and CPU stores reach rom_ctrl over TL-UL
  // (testing rom_ctrl's ErrOnWrite TL-UL rejection rather than CPU ePMP).
  uint32_t orig_pmpaddr0 = 0;
  uint32_t orig_pmpcfg0 = 0;
  CSR_READ(CSR_REG_PMPADDR0, &orig_pmpaddr0);
  CSR_READ(CSR_REG_PMPCFG0, &orig_pmpcfg0);

  const uint32_t kRomNapotAddr =
      (kRomCtrlRomBase >> 2) | ((kRomCtrlRomSize - 1u) >> 3);
  // L=1 (bit 7), A=NAPOT (3 << 3), X=0, W=1 (bit 1), R=1 (bit 0) -> 0x9b.
  // Also set L=0 fallback (0x1b) in case RLB is cleared.
  const uint32_t kPmp0CfgRwLocked = 0x9bu;
  const uint32_t kPmp0CfgRwUnlocked = 0x1bu;
  CSR_WRITE(CSR_REG_PMPADDR0, kRomNapotAddr);
  CSR_WRITE(CSR_REG_PMPCFG0, (orig_pmpcfg0 & ~0xffu) | kPmp0CfgRwLocked);
  uint32_t active_pmpcfg0 = 0;
  CSR_READ(CSR_REG_PMPCFG0, &active_pmpcfg0);
  if ((active_pmpcfg0 & 0xffu) != kPmp0CfgRwLocked) {
    CSR_WRITE(CSR_REG_PMPCFG0, (orig_pmpcfg0 & ~0xffu) | kPmp0CfgRwUnlocked);
  }

  uint32_t orig_word = abs_mmio_read32(kRomCtrlRomBase);
  store_access_fault_seen = false;
  abs_mmio_write32(kRomCtrlRomBase, ~orig_word);
  CHECK(store_access_fault_seen,
        "Expected StoreAccessFault when writing to ROM window");
  CHECK(abs_mmio_read32(kRomCtrlRomBase) == orig_word,
        "ROM contents modified by rejected write");

  CSR_WRITE(CSR_REG_PMPCFG0, orig_pmpcfg0);
  CSR_WRITE(CSR_REG_PMPADDR0, orig_pmpaddr0);
}

bool test_main(void) {
  test_csr_semantics_and_digests();
  test_alert_test_pulse_and_clear();
  test_rom_write_protection();
  return true;
}
