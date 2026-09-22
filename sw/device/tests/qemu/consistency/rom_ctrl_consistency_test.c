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

  // 4. Verify ROM_CTRL_REGS_PERMIT sub-word write wr_err
  // (rom_ctrl_reg_pkg.sv:115-134, rom_ctrl_regs_reg_top.sv:713-733):
  // - ALERT_TEST (0x00) and FATAL_ALERT_CAUSE (0x04) have PERMIT = 4'b0001:
  //   8-bit write at offset +0 (reg_be = 4'b0001) succeeds without fault, while
  //   8-bit write at offset +1 (reg_be = 4'b0010) fails with StoreAccessFault.
  // - DIGEST_0..7 (0x08..0x24) and EXP_DIGEST_0..7 (0x28..0x44) have PERMIT =
  // 4'b1111:
  //   8-bit (sb) and 16-bit (sh) writes fail with StoreAccessFault (wr_err =
  //   1).
  store_access_fault_seen = false;
  abs_mmio_write8(kRomCtrlRegsBase + ROM_CTRL_ALERT_TEST_REG_OFFSET, 0u);
  CHECK(!store_access_fault_seen,
        "Expected no StoreAccessFault on 8-bit write to ALERT_TEST+0");

  store_access_fault_seen = false;
  abs_mmio_write8(kRomCtrlRegsBase + ROM_CTRL_FATAL_ALERT_CAUSE_REG_OFFSET,
                  0x3u);
  CHECK(!store_access_fault_seen,
        "Expected no StoreAccessFault on 8-bit write to FATAL_ALERT_CAUSE+0");

  store_access_fault_seen = false;
  abs_mmio_write8(kRomCtrlRegsBase + ROM_CTRL_ALERT_TEST_REG_OFFSET + 1u, 1u);
  CHECK(store_access_fault_seen,
        "Expected StoreAccessFault on 8-bit write to ALERT_TEST+1 "
        "(PERMIT=4'b0001)");

  store_access_fault_seen = false;
  abs_mmio_write8(kRomCtrlRegsBase + ROM_CTRL_DIGEST_0_REG_OFFSET, 0xaau);
  CHECK(
      store_access_fault_seen,
      "Expected StoreAccessFault on 8-bit write to DIGEST_0 (PERMIT=4'b1111)");

  store_access_fault_seen = false;
  abs_mmio_write8(kRomCtrlRegsBase + ROM_CTRL_EXP_DIGEST_0_REG_OFFSET, 0x55u);
  CHECK(store_access_fault_seen,
        "Expected StoreAccessFault on 8-bit write to EXP_DIGEST_0 "
        "(PERMIT=4'b1111)");

  // Sub-word (size < 4) CSR reads succeed and return the corresponding byte
  // slices of the 32-bit register value.
  uint32_t d0 =
      abs_mmio_read32(kRomCtrlRegsBase + ROM_CTRL_DIGEST_0_REG_OFFSET);
  for (uint32_t b = 0; b < 4u; ++b) {
    uint8_t byte_val =
        abs_mmio_read8(kRomCtrlRegsBase + ROM_CTRL_DIGEST_0_REG_OFFSET + b);
    CHECK(byte_val == (uint8_t)((d0 >> (b * 8u)) & 0xffu));
  }

  // Out-of-bounds CSR read (offset 0x48 >= REGS_COUNT * 4) raises
  // LoadAccessFault (MEMTX_DECODE_ERROR).
  load_access_fault_seen = false;
  (void)abs_mmio_read32(kRomCtrlRegsBase + 0x48u);
  CHECK(load_access_fault_seen,
        "Expected LoadAccessFault on out-of-bounds CSR read at offset 0x48");
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
  shadow_write32(class_addr,
                 ALERT_HANDLER_ALERT_CLASS_SHADOWED_0_CLASS_A_0_VALUE_CLASSA);
  shadow_write32(en_addr, 1u);
  abs_mmio_write32(cause_addr, 1u);
  CHECK(abs_mmio_read32(cause_addr) == 0u,
        "Expected ALERT_CAUSE[60] to be 0 before ALERT_TEST");

  // Verify 1-bit write mask on ALERT_TEST: writing 0xfffffffe (bit 0 = 0)
  // must be ignored and NOT trigger ALERT_CAUSE[60].
  abs_mmio_write32(kRomCtrlRegsBase + ROM_CTRL_ALERT_TEST_REG_OFFSET,
                   0xfffffffeu);
  CHECK(abs_mmio_read32(cause_addr) == 0u,
        "Expected ALERT_CAUSE[60] == 0 after writing 0xfffffffe to ALERT_TEST");

  // Trigger ALERT_TEST = 1 -> should pulse once, setting ALERT_CAUSE[60] = 1.
  abs_mmio_write32(kRomCtrlRegsBase + ROM_CTRL_ALERT_TEST_REG_OFFSET,
                   1u << ROM_CTRL_ALERT_TEST_FATAL_BIT);
  CHECK(abs_mmio_read32(cause_addr) == 1u,
        "Expected ALERT_CAUSE[60] == 1 after first ALERT_TEST write");

  // Clear ALERT_CAUSE[60] (rw1c). Because ALERT_TEST is a pulse (not latched),
  // ALERT_CAUSE[60] must remain 0 after clearing.
  abs_mmio_write32(cause_addr, 1u);
  CHECK(
      abs_mmio_read32(cause_addr) == 0u,
      "ALERT_CAUSE[60] remained 1 after rw1c clear (ALERT_TEST was latched!)");

  // Trigger ALERT_TEST = 0xffffffff (masked to bit 0 = 1) a second time
  // without writing 0 first -> must fire again.
  abs_mmio_write32(kRomCtrlRegsBase + ROM_CTRL_ALERT_TEST_REG_OFFSET,
                   0xffffffffu);
  CHECK(abs_mmio_read32(cause_addr) == 1u,
        "Expected ALERT_CAUSE[60] == 1 after second ALERT_TEST write");

  // Clean up and restore Class D routing.
  abs_mmio_write32(cause_addr, 1u);
  CHECK(abs_mmio_read32(cause_addr) == 0u,
        "Expected ALERT_CAUSE[60] == 0 after final clear");
  shadow_write32(class_addr,
                 ALERT_HANDLER_ALERT_CLASS_SHADOWED_0_CLASS_A_0_VALUE_CLASSD);
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
        "Expected StoreAccessFault when writing to ROM window base");
  CHECK(abs_mmio_read32(kRomCtrlRomBase) == orig_word,
        "ROM contents modified by rejected write");

  // Last non-digest ROM word (0x0000ffdc, immediately preceding the top 8
  // EXP_DIGEST words at 0x0000ffe0..0x0000fffc) has valid ECC and can be read
  // normally, while writes trigger StoreAccessFault.
  uint32_t last_code_word_addr =
      kRomCtrlRomBase + kRomCtrlRomSize - 32u - sizeof(uint32_t);
  uint32_t orig_last_code_word = abs_mmio_read32(last_code_word_addr);
  store_access_fault_seen = false;
  abs_mmio_write32(last_code_word_addr, ~orig_last_code_word);
  CHECK(store_access_fault_seen,
        "Expected StoreAccessFault when writing to last non-digest ROM word");
  CHECK(abs_mmio_read32(last_code_word_addr) == orig_last_code_word,
        "ROM last non-digest word modified by rejected write");

  // Wave 2 Deep-Pass Check:
  // In Earlgrey RTL (rom_ctrl.sv:184 EnableDataIntgPt=1 & scramble_image.py
  // lines 319-350), the top 8 words (0x0000ffe0..0x0000fffc) store the raw
  // EXP_DIGEST with ECC bits specifically chosen so that after PRINCE keystream
  // unscrambling (clr_rdata_o), every word in 0x0000ffe0..0x0000fffc has an
  // invalid Hsiao (39,32) ECC checksum. Reading any of the top 8 words over
  // the bus raises an Ibex Load Integrity NMI (MCAUSE=0xffffffe0,
  // MTVAL=top_word_addr) and sets RV_CORE_IBEX.ERR_STATUS.FATAL_INTG_ERR (bit
  // 8).
  uint32_t ibex_fatal_hw_en_addr =
      kAlertHandlerBase + ALERT_HANDLER_ALERT_EN_SHADOWED_0_REG_OFFSET +
      kIbexFatalHwErrAlertId * sizeof(uint32_t);
  shadow_write32(ibex_fatal_hw_en_addr, 0u);

  uint32_t top_word_addr = kRomCtrlRomBase + kRomCtrlRomSize - sizeof(uint32_t);
  store_access_fault_seen = false;
  abs_mmio_write32(top_word_addr, 0xdeadbeefu);
  CHECK(store_access_fault_seen,
        "Expected StoreAccessFault when writing to top ROM digest region");

  load_integrity_fault_seen = false;
  load_integrity_fault_mtval = 0u;
  (void)abs_mmio_read32(top_word_addr);
  busy_spin_micros(5);
  if (!load_integrity_fault_seen ||
      load_integrity_fault_mtval != top_word_addr) {
    LOG_ERROR(
        "RTL_MISMATCH: reading top 8 EXP_DIGEST ROM word 0x%08x did not raise "
        "Load Integrity NMI (seen=%d, mtval=0x%08x)",
        top_word_addr, load_integrity_fault_seen, load_integrity_fault_mtval);
    CHECK(false);
  }
  uint32_t ibex_err_status = abs_mmio_read32(kIbexBase + 0x54u);
  if ((ibex_err_status & (1u << 8)) == 0u) {
    LOG_ERROR(
        "RTL_MISMATCH: reading top 8 EXP_DIGEST ROM word did not set "
        "RV_CORE_IBEX.ERR_STATUS.FATAL_INTG_ERR (got 0x%08x)",
        ibex_err_status);
    CHECK(false);
  }
  // Clear ERR_STATUS.FATAL_INTG_ERR (RW1C).
  abs_mmio_write32(kIbexBase + 0x54u, 1u << 8);

  CSR_WRITE(CSR_REG_PMPCFG0, orig_pmpcfg0);
  CSR_WRITE(CSR_REG_PMPADDR0, orig_pmpaddr0);
}

bool test_main(void) {
  test_csr_semantics_and_digests();
  test_alert_test_pulse_and_clear();
  test_rom_write_protection();
  return true;
}
