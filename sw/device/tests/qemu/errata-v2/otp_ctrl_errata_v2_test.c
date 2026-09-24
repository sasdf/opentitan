// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file
 * @brief Earlgrey v2 (trunk-v2) OTP_CTRL hardware & DIF errata verification
 * test for the physical CW340 FPGA.
 *
 * Verifies:
 * 1. Legacy `otp_ctrl.prim` (`0x40132000`) removal in `trunk-v2` raising
 *    unmapped `xbar_peri` Load/Store Access Faults (`mcause=5`/`7`), contrasted
 *    with `rram_macro.prim` (`0x41018000`) where `rram_macro.sv:63` ties
 *    `lc_nvm_debug_en_i` to `unused_lc_nvm_debug_en` without `u_tlul_lc_gate`.
 * 2. `otp_ctrl.core` sub-word (8-bit/16-bit) CSR write rejection (`wr_err` in
 *    `otp_ctrl_core_reg_top.sv` via `OTP_CTRL_CORE_PERMIT == 4'b1111`) raising
 *    synchronous Store Access Fault (`mcause=7`) without `BUS_INTEG_ERROR`.
 * 3. `SW_CFG_WINDOW` write rejection (`otp_ctrl.sv:395-404`
 *    `u_tlul_adapter_sram_sw_cfg` + `tlul_err.sv:53`) raising ONLY a
 * synchronous Store Access Fault (`mcause=7`) without setting partition
 * `ERR_CODE_*`, `STATUS.PARTITION_ERROR`, `PARTITION_STATUS_0`, or
 * `INTR_STATE.otp_error`.
 * 4. v2 `DIRECT_ACCESS_CMD.ZEROIZE` (`bit 3`, `otp_ctrl.hjson`,
 *    `otp_ctrl_dai.sv:323-325, 720-741`) unconditional `AccessError` (`0x5`) on
 *    all Earlgrey partitions (`zeroizable: 1'b0` in
 * `otp_ctrl_part_pkg.sv:100-285`), asserting both
 * `INTR_STATE.otp_operation_done` and `INTR_STATE.otp_error` while blanking
 * `DIRECT_ACCESS_RDATA_0/1` to `0`.
 * 5. `dif_otp_ctrl_read_blocking`
 * (`sw/device/lib/dif/dif_otp_ctrl.c:1021-1028`) word-vs-byte bounds check
 * mismatch (`address + len >= part.len`), allowing reads to cross partition
 * boundaries in `SW_CFG_WINDOW` and trigger a hardware Load Access Fault
 * (`mcause=5`) + `AccessError` when the adjacent partition is read-locked.
 * 6. Read-locked `SW_CFG_WINDOW` dual error reporting (`mcause=5` +
 *    `ERR_CODE_0 = AccessError` + v2 `STATUS.PARTITION_ERROR` +
 *    `PARTITION_STATUS_0.VENDOR_TEST_ERROR` + `INTR_STATE.otp_error`), plus
 *    unbuffered partition SW digest (`0x38`) read-lock blocking on both
 *    `SW_CFG_WINDOW` and `DAI` (`otp_ctrl_part_unbuf.sv:395-409`,
 *    `otp_ctrl_dai.sv:336-342`).
 */

#include <stdbool.h>
#include <stdint.h>

#include "hw/top/dt/otp_ctrl.h"
#include "sw/device/lib/arch/device.h"
#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_alert_handler.h"
#include "sw/device/lib/dif/dif_otp_ctrl.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top/otp_ctrl_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kOtpCoreBase = TOP_EARLGREY_OTP_CTRL_CORE_BASE_ADDR,
  kOtpSwCfgBase =
      TOP_EARLGREY_OTP_CTRL_CORE_BASE_ADDR + OTP_CTRL_SW_CFG_WINDOW_REG_OFFSET,
  // In trunk-v2, otp_ctrl.prim (0x40132000) was removed from otp_ctrl when
  // otp_ctrl.otp_macro was wired to rram_ctrl.otp_macro, leaving 0x40132000
  // unmapped on xbar_peri and placing the macro prim bus at
  // TOP_EARLGREY_RRAM_MACRO_PRIM_BASE_ADDR (0x41018000).
  kOtpPrimLegacyBase = 0x40132000u,
  kRramMacroPrimBase = TOP_EARLGREY_RRAM_MACRO_PRIM_BASE_ADDR,
  kAlertHandlerBase = TOP_EARLGREY_ALERT_HANDLER_BASE_ADDR,

  kVendorTestOffset = OTP_CTRL_PARAM_VENDOR_TEST_OFFSET,
  kVendorTestSize = OTP_CTRL_PARAM_VENDOR_TEST_SIZE,
  kVendorTestDigestOffset =
      kVendorTestOffset + kVendorTestSize - sizeof(uint64_t),
  kCreatorSwCfgOffset = OTP_CTRL_PARAM_CREATOR_SW_CFG_OFFSET,
  kOwnerSwCfgOffset = OTP_CTRL_PARAM_OWNER_SW_CFG_OFFSET,
  kHwCfg0Offset = OTP_CTRL_PARAM_HW_CFG0_OFFSET,

  kRiscvExcLoadAccessFault = 5,
  kRiscvExcStoreAccessFault = 7,
};

static dif_otp_ctrl_t otp;
static dif_alert_handler_t alert_handler;

static volatile bool expect_access_fault = false;
static volatile uint32_t access_fault_count = 0;
static volatile uint32_t last_fault_mcause = 0;
static volatile uint32_t last_fault_mtval = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  uint32_t mcause = ibex_mcause_read();
  uint32_t mtval = ibex_mtval_read();
  CHECK(expect_access_fault,
        "Unexpected Load/Store Access Fault: mcause=0x%x mtval=0x%08x", mcause,
        mtval);
  access_fault_count++;
  last_fault_mcause = mcause;
  last_fault_mtval = mtval;
}

static void check_no_otp_alerts(void) {
  for (dif_alert_handler_alert_t a = kTopEarlgreyAlertIdOtpCtrlFatalMacroError;
       a <= kTopEarlgreyAlertIdOtpCtrlRecovPrimOtpAlert; ++a) {
    bool is_cause = false;
    CHECK_DIF_OK(
        dif_alert_handler_alert_is_cause(&alert_handler, a, &is_cause));
    CHECK(!is_cause, "Unexpected OTP_CTRL alert %u fired!", (uint32_t)a);
  }
}

static void wait_for_dai_idle(void) {
  while (true) {
    uint32_t status =
        abs_mmio_read32(kOtpCoreBase + OTP_CTRL_STATUS_REG_OFFSET);
    if (bitfield_bit32_read(status, OTP_CTRL_STATUS_DAI_IDLE_BIT)) {
      return;
    }
  }
}

static void clear_otp_irqs(void) {
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET,
                   (1u << OTP_CTRL_INTR_STATE_OTP_OPERATION_DONE_BIT) |
                       (1u << OTP_CTRL_INTR_STATE_OTP_ERROR_BIT));
}

static void test_prim_bus_lc_gate_access_faults(void) {
  LOG_INFO(
      "Test 1: legacy otp_ctrl.prim (0x40132000) fault vs rram_macro.prim "
      "(0x41018000)");
  wait_for_dai_idle();
  clear_otp_irqs();

  // 1a. Legacy v1 `otp_ctrl.prim` (0x40132000) was removed from `otp_ctrl` in
  // trunk-v2 (`top_earlgrey.hjson:408-410`), leaving 0x40132000 unmapped on
  // xbar_peri: reads/writes raise synchronous Load/Store Access Faults (5/7).
  access_fault_count = 0;
  expect_access_fault = true;
  uint32_t dummy = *(volatile uint32_t *)kOtpPrimLegacyBase;
  (void)dummy;
  asm volatile("" ::: "memory");
  expect_access_fault = false;

  CHECK(access_fault_count == 1,
        "Expected 1 Load Access Fault on 0x40132000 read, got %u",
        access_fault_count);
  CHECK(last_fault_mcause == kRiscvExcLoadAccessFault,
        "Expected mcause=5, got %u", last_fault_mcause);

  access_fault_count = 0;
  expect_access_fault = true;
  *(volatile uint32_t *)kOtpPrimLegacyBase = 0xdeadbeef;
  asm volatile("" ::: "memory");
  expect_access_fault = false;

  CHECK(access_fault_count == 1,
        "Expected 1 Store Access Fault on 0x40132000 write, got %u",
        access_fault_count);
  CHECK(last_fault_mcause == kRiscvExcStoreAccessFault,
        "Expected mcause=7, got %u", last_fault_mcause);

  // 1b. In trunk-v2, `otp_ctrl.otp_macro` is wired to `rram_ctrl.otp_macro`
  // (`top_earlgrey.hjson:1332`), and `rram_macro.prim` (`0x41018000`) ties off
  // `assign unused_lc_nvm_debug_en = lc_nvm_debug_en_i;` (`rram_macro.sv:63`)
  // without `u_tlul_lc_gate`, allowing CSR0_REGWEN (0x0) and CSR1 (0x4)
  // reads/writes without access faults.
  access_fault_count = 0;
  expect_access_fault = false;
  uint32_t rram_regwen = *(volatile uint32_t *)(kRramMacroPrimBase + 0x0u);
  *(volatile uint32_t *)(kRramMacroPrimBase + 0x4u) = 0x5a5aa5a5u;
  uint32_t rram_csr1 = *(volatile uint32_t *)(kRramMacroPrimBase + 0x4u);
  CHECK(
      access_fault_count == 0 && rram_regwen == 1u &&
          rram_csr1 == (0x5a5aa5a5u & 0x1fffu),
      "Expected rram_macro.prim (0x41018000) to be RW without u_tlul_lc_gate, "
      "got regwen=0x%x csr1=0x%08x faults=%u",
      rram_regwen, rram_csr1, access_fault_count);

  uint32_t status = abs_mmio_read32(kOtpCoreBase + OTP_CTRL_STATUS_REG_OFFSET);
  uint32_t part_status =
      abs_mmio_read32(kOtpCoreBase + OTP_CTRL_PARTITION_STATUS_0_REG_OFFSET);
  uint32_t intr_state =
      abs_mmio_read32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET);
  CHECK(status == (1u << OTP_CTRL_STATUS_DAI_IDLE_BIT),
        "Expected STATUS=0x100 after prim fault, got 0x%08x", status);
  CHECK(part_status == 0,
        "Expected PARTITION_STATUS_0=0 after prim fault, got 0x%08x",
        part_status);
  CHECK(intr_state == 0, "Expected INTR_STATE=0 after prim fault, got 0x%08x",
        intr_state);
  check_no_otp_alerts();
}

static void test_core_subword_write_faults(void) {
  LOG_INFO(
      "Test 2: otp_ctrl.core sub-word CSR write rejection "
      "(OTP_CTRL_CORE_PERMIT)");
  wait_for_dai_idle();
  clear_otp_irqs();

  const uint32_t kWdata0Addr =
      kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_WDATA_0_REG_OFFSET;
  const uint32_t kSentinel = 0xa5a55a5au;
  abs_mmio_write32(kWdata0Addr, kSentinel);
  CHECK(abs_mmio_read32(kWdata0Addr) == kSentinel, "WDATA_0 setup failed");

  // 8-bit store (`sb`) to full-word CSR (`OTP_CTRL_CORE_PERMIT == 4'b1111`).
  access_fault_count = 0;
  expect_access_fault = true;
  *(volatile uint8_t *)kWdata0Addr = 0x11u;
  asm volatile("" ::: "memory");
  expect_access_fault = false;

  CHECK(access_fault_count == 1,
        "Expected Store Access Fault on 8-bit CSR write, got %u",
        access_fault_count);
  CHECK(last_fault_mcause == kRiscvExcStoreAccessFault,
        "Expected mcause=7 on 8-bit CSR write, got %u", last_fault_mcause);
  CHECK(abs_mmio_read32(kWdata0Addr) == kSentinel,
        "8-bit CSR write must not mutate WDATA_0");

  // 16-bit store (`sh`) to full-word CSR (`OTP_CTRL_CORE_PERMIT == 4'b1111`).
  access_fault_count = 0;
  expect_access_fault = true;
  *(volatile uint16_t *)kWdata0Addr = 0x2233u;
  asm volatile("" ::: "memory");
  expect_access_fault = false;

  CHECK(access_fault_count == 1,
        "Expected Store Access Fault on 16-bit CSR write, got %u",
        access_fault_count);
  CHECK(last_fault_mcause == kRiscvExcStoreAccessFault,
        "Expected mcause=7 on 16-bit CSR write, got %u", last_fault_mcause);
  CHECK(abs_mmio_read32(kWdata0Addr) == kSentinel,
        "16-bit CSR write must not mutate WDATA_0");

  uint32_t status = abs_mmio_read32(kOtpCoreBase + OTP_CTRL_STATUS_REG_OFFSET);
  CHECK(!bitfield_bit32_read(status, OTP_CTRL_STATUS_BUS_INTEG_ERROR_BIT),
        "Sub-word CSR write must not set STATUS.BUS_INTEG_ERROR (0x%08x)",
        status);
  check_no_otp_alerts();
}

static void test_sw_cfg_window_write_rejection(void) {
  LOG_INFO(
      "Test 3: SW_CFG_WINDOW write rejection via u_tlul_adapter_sram_sw_cfg");
  wait_for_dai_idle();
  clear_otp_irqs();

  access_fault_count = 0;
  expect_access_fault = true;
  *(volatile uint32_t *)(kOtpSwCfgBase + kVendorTestOffset) = 0x12345678u;
  asm volatile("" ::: "memory");
  expect_access_fault = false;

  CHECK(access_fault_count == 1,
        "Expected Store Access Fault on SW_CFG_WINDOW write, got %u",
        access_fault_count);
  CHECK(last_fault_mcause == kRiscvExcStoreAccessFault,
        "Expected mcause=7 on SW_CFG_WINDOW write, got %u", last_fault_mcause);

  uint32_t err0 =
      abs_mmio_read32(kOtpCoreBase + OTP_CTRL_ERR_CODE_0_REG_OFFSET);
  uint32_t status = abs_mmio_read32(kOtpCoreBase + OTP_CTRL_STATUS_REG_OFFSET);
  uint32_t part_status =
      abs_mmio_read32(kOtpCoreBase + OTP_CTRL_PARTITION_STATUS_0_REG_OFFSET);
  uint32_t intr_state =
      abs_mmio_read32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET);
  CHECK(err0 == OTP_CTRL_ERR_CODE_0_ERR_CODE_0_VALUE_NO_ERROR,
        "SW_CFG_WINDOW write must not set ERR_CODE_0, got 0x%x", err0);
  CHECK(status == (1u << OTP_CTRL_STATUS_DAI_IDLE_BIT),
        "SW_CFG_WINDOW write must not set STATUS.PARTITION_ERROR, got 0x%08x",
        status);
  CHECK(part_status == 0,
        "SW_CFG_WINDOW write must not set PARTITION_STATUS_0, got 0x%08x",
        part_status);
  CHECK(intr_state == 0,
        "SW_CFG_WINDOW write must not set INTR_STATE.otp_error, got 0x%08x",
        intr_state);
  check_no_otp_alerts();
}

static void test_v2_zeroize_cmd_access_error_and_rdata_blanking(void) {
  LOG_INFO(
      "Test 4: v2 DIRECT_ACCESS_CMD.ZEROIZE (bit 3) AccessError & RDATA "
      "blanking");
  wait_for_dai_idle();
  clear_otp_irqs();

  // First read HW_CFG0 (0x678) via DAI so DIRECT_ACCESS_RDATA_0 holds a
  // non-zero value.
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_ADDRESS_REG_OFFSET,
                   kHwCfg0Offset);
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_CMD_REG_OFFSET,
                   (1u << OTP_CTRL_DIRECT_ACCESS_CMD_RD_BIT));
  wait_for_dai_idle();

  uint32_t rdata0_before =
      abs_mmio_read32(kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_RDATA_0_REG_OFFSET);
  CHECK(
      rdata0_before != 0,
      "Expected non-zero HW_CFG0 word in DIRECT_ACCESS_RDATA_0 before ZEROIZE");
  clear_otp_irqs();

  // Issue v2 DIRECT_ACCESS_CMD.ZEROIZE (bit 3) targeting unlocked VENDOR_TEST
  // (0x0). Because all Earlgrey partitions have `zeroizable: 1'b0` in
  // `otp_ctrl_part_pkg.sv`, `otp_ctrl_dai.sv` in `ZerSt` (`L720-741`) clears
  // `data_q` (`data_clr = 1'b1`), sets `error_d = AccessError`, and asserts
  // `dai_cmd_done_o = 1'b1`.
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_ADDRESS_REG_OFFSET,
                   kVendorTestOffset);
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_CMD_REG_OFFSET,
                   (1u << OTP_CTRL_DIRECT_ACCESS_CMD_ZEROIZE_BIT));
  wait_for_dai_idle();

  uint32_t status = abs_mmio_read32(kOtpCoreBase + OTP_CTRL_STATUS_REG_OFFSET);
  uint32_t part_status =
      abs_mmio_read32(kOtpCoreBase + OTP_CTRL_PARTITION_STATUS_0_REG_OFFSET);
  uint32_t dai_err =
      abs_mmio_read32(kOtpCoreBase + OTP_CTRL_ERR_CODE_11_REG_OFFSET);
  uint32_t intr_state =
      abs_mmio_read32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET);
  uint32_t rdata0_after =
      abs_mmio_read32(kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_RDATA_0_REG_OFFSET);
  uint32_t rdata1_after =
      abs_mmio_read32(kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_RDATA_1_REG_OFFSET);

  CHECK(dai_err == OTP_CTRL_ERR_CODE_0_ERR_CODE_0_VALUE_ACCESS_ERROR,
        "Expected ERR_CODE_11 (DAI) = AccessError (5) after ZEROIZE, got %u",
        dai_err);
  CHECK(status == ((1u << OTP_CTRL_STATUS_DAI_ERROR_BIT) |
                   (1u << OTP_CTRL_STATUS_DAI_IDLE_BIT)),
        "Expected STATUS = 0x102 (DAI_ERROR | DAI_IDLE) after ZEROIZE, got "
        "0x%08x",
        status);
  CHECK(part_status == 0,
        "Expected PARTITION_STATUS_0 = 0 after DAI ZEROIZE error, got 0x%08x",
        part_status);
  CHECK(intr_state == ((1u << OTP_CTRL_INTR_STATE_OTP_OPERATION_DONE_BIT) |
                       (1u << OTP_CTRL_INTR_STATE_OTP_ERROR_BIT)),
        "Expected both otp_operation_done and otp_error IRQs (0x3) on failed "
        "ZEROIZE, got 0x%x",
        intr_state);
  CHECK(rdata0_after == 0 && rdata1_after == 0,
        "Expected DIRECT_ACCESS_RDATA_0/1 blanked to 0 by failed ZEROIZE, got "
        "0x%08x / 0x%08x",
        rdata0_after, rdata1_after);

  dif_otp_ctrl_status_t dif_status;
  CHECK_DIF_OK(dif_otp_ctrl_get_status(&otp, &dif_status));
  CHECK(dif_status.codes == ((1u << kDifOtpCtrlStatusCodeDaiError) |
                             (1u << kDifOtpCtrlStatusCodeDaiIdle)),
        "Unexpected dif_status.codes: 0x%08x", dif_status.codes);
  CHECK(dif_status.causes[kDifOtpCtrlPartitionDaiError] ==
            kDifOtpCtrlErrorLockedAccess,
        "Expected kDifOtpCtrlErrorLockedAccess for DAI error cause, got %u",
        dif_status.causes[kDifOtpCtrlPartitionDaiError]);

  // Recover DAI error by issuing a valid DAI read of HW_CFG0: verify that
  // otp_ctrl_dai.sv:285 (IdleSt) clears ERR_CODE_11 (0) and STATUS.DAI_ERROR
  // (STATUS == 0x100) while sticky W1C INTR_STATE.otp_error (0x3) remains set.
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_ADDRESS_REG_OFFSET,
                   kHwCfg0Offset);
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_CMD_REG_OFFSET,
                   (1u << OTP_CTRL_DIRECT_ACCESS_CMD_RD_BIT));
  wait_for_dai_idle();
  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_ERR_CODE_11_REG_OFFSET) ==
            OTP_CTRL_ERR_CODE_0_ERR_CODE_0_VALUE_NO_ERROR,
        "Valid DAI read must clear ERR_CODE_11 back to NoError");
  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_STATUS_REG_OFFSET) ==
            (1u << OTP_CTRL_STATUS_DAI_IDLE_BIT),
        "Valid DAI read must clear STATUS.DAI_ERROR back to 0 (0x100)");
  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET) ==
            ((1u << OTP_CTRL_INTR_STATE_OTP_OPERATION_DONE_BIT) |
             (1u << OTP_CTRL_INTR_STATE_OTP_ERROR_BIT)),
        "Sticky W1C INTR_STATE.otp_error must remain 1 after valid DAI read");
  clear_otp_irqs();
  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET) == 0u,
        "Expected INTR_STATE == 0 after W1C clear");
  check_no_otp_alerts();
}

static void test_dif_read_blocking_word_vs_byte_bounds_bypass(void) {
  LOG_INFO(
      "Test 5: dif_otp_ctrl_read_blocking word-vs-byte bounds check mismatch");
  wait_for_dai_idle();
  clear_otp_irqs();

  uint32_t buf[4] = {0};

  // In dif_otp_ctrl.c:1021, `if (address + len >= kPartitions[partition].len)`
  // adds `address` (bytes) to `len` (words) and compares against `len` (64
  // bytes). Thus address=0x3c (60) + len=4 (words) = 64 >= 64 returns
  // kDifOutOfRange, whereas address=0x38 (56) + len=4 (words, 16 bytes ->
  // 0x38..0x47) = 60 < 64 bypasses kDifOutOfRange and reads 8 bytes into
  // CREATOR_SW_CFG (0x40, 0x44)!
  CHECK(dif_otp_ctrl_read_blocking(&otp, kDifOtpCtrlPartitionVendorTest,
                                   /*address=*/0x3c, buf,
                                   /*len=*/4) == kDifOutOfRange,
        "Expected kDifOutOfRange at address=0x3c, len=4");

  uint32_t creator_word0 =
      abs_mmio_read32(kOtpSwCfgBase + kCreatorSwCfgOffset + 0);
  uint32_t creator_word1 =
      abs_mmio_read32(kOtpSwCfgBase + kCreatorSwCfgOffset + 4);

  CHECK_DIF_OK(dif_otp_ctrl_read_blocking(&otp, kDifOtpCtrlPartitionVendorTest,
                                          /*address=*/0x38, buf, /*len=*/4));
  CHECK(
      buf[2] == creator_word0 && buf[3] == creator_word1,
      "Expected out-of-bounds dif_otp_ctrl_read_blocking words [2..3] to match "
      "CREATOR_SW_CFG words (0x%08x, 0x%08x vs 0x%08x, 0x%08x)",
      buf[2], buf[3], creator_word0, creator_word1);

  // Now lock CREATOR_SW_CFG_READ_LOCK (0) while keeping VENDOR_TEST unlocked
  // (1). Calling dif_otp_ctrl_read_blocking(&otp,
  // kDifOtpCtrlPartitionVendorTest, 0x38, buf, 4) bypasses the DIF bounds check
  // (`56 + 4 = 60 < 64`) and faults in hardware on the 3rd word (`0x40` in
  // read-locked CREATOR_SW_CFG)!
  CHECK_DIF_OK(
      dif_otp_ctrl_lock_reading(&otp, kDifOtpCtrlPartitionCreatorSwCfg));

  access_fault_count = 0;
  expect_access_fault = true;
  dif_result_t res = dif_otp_ctrl_read_blocking(
      &otp, kDifOtpCtrlPartitionVendorTest, /*address=*/0x38, buf, /*len=*/4);
  expect_access_fault = false;

  CHECK(res == kDifOk,
        "dif_otp_ctrl_read_blocking bypassed kDifOutOfRange and returned %d",
        (int)res);
  CHECK(access_fault_count == 2,
        "Expected 2 Load Access Faults on words at 0x40 and 0x44 in locked "
        "CREATOR_SW_CFG, got %u",
        access_fault_count);
  CHECK(last_fault_mcause == kRiscvExcLoadAccessFault,
        "Expected mcause=5, got %u", last_fault_mcause);

  uint32_t status = abs_mmio_read32(kOtpCoreBase + OTP_CTRL_STATUS_REG_OFFSET);
  uint32_t part_status =
      abs_mmio_read32(kOtpCoreBase + OTP_CTRL_PARTITION_STATUS_0_REG_OFFSET);
  uint32_t err1 =
      abs_mmio_read32(kOtpCoreBase + OTP_CTRL_ERR_CODE_1_REG_OFFSET);
  CHECK(err1 == OTP_CTRL_ERR_CODE_0_ERR_CODE_0_VALUE_ACCESS_ERROR,
        "Expected ERR_CODE_1 (CREATOR_SW_CFG) = AccessError (5), got %u", err1);
  CHECK(bitfield_bit32_read(status, OTP_CTRL_STATUS_PARTITION_ERROR_BIT),
        "Expected STATUS.PARTITION_ERROR=1, got 0x%08x", status);
  CHECK(bitfield_bit32_read(
            part_status, OTP_CTRL_PARTITION_STATUS_0_CREATOR_SW_CFG_ERROR_BIT),
        "Expected PARTITION_STATUS_0.CREATOR_SW_CFG_ERROR=1, got 0x%08x",
        part_status);

  clear_otp_irqs();
  check_no_otp_alerts();
}

static void test_read_locked_window_and_unbuf_sw_digest_and_dai_errata(void) {
  LOG_INFO(
      "Test 6: Read-locked SW_CFG_WINDOW + SW digest blocking + v2 "
      "PARTITION_STATUS_0");
  wait_for_dai_idle();
  clear_otp_irqs();

  // Read VENDOR_TEST_DIGEST_0/1 CSRs before locking VENDOR_TEST_READ_LOCK.
  uint32_t csr_dig0_before =
      abs_mmio_read32(kOtpCoreBase + OTP_CTRL_VENDOR_TEST_DIGEST_0_REG_OFFSET);
  uint32_t csr_dig1_before =
      abs_mmio_read32(kOtpCoreBase + OTP_CTRL_VENDOR_TEST_DIGEST_1_REG_OFFSET);

  // Lock VENDOR_TEST_READ_LOCK (W0C -> 0).
  CHECK_DIF_OK(dif_otp_ctrl_lock_reading(&otp, kDifOtpCtrlPartitionVendorTest));

  // 6a. Reading VENDOR_TEST data (0x0) via SW_CFG_WINDOW raises Load Access
  // Fault AND sets ERR_CODE_0 = AccessError (5),
  // PARTITION_STATUS_0.VENDOR_TEST_ERROR = 1, STATUS.PARTITION_ERROR = 1, and
  // INTR_STATE.otp_error = 1.
  access_fault_count = 0;
  expect_access_fault = true;
  uint32_t val = *(volatile uint32_t *)(kOtpSwCfgBase + kVendorTestOffset);
  (void)val;
  asm volatile("" ::: "memory");
  expect_access_fault = false;

  CHECK(
      access_fault_count == 1 && last_fault_mcause == kRiscvExcLoadAccessFault,
      "Expected Load Access Fault on locked VENDOR_TEST window read");
  uint32_t err0 =
      abs_mmio_read32(kOtpCoreBase + OTP_CTRL_ERR_CODE_0_REG_OFFSET);
  uint32_t status = abs_mmio_read32(kOtpCoreBase + OTP_CTRL_STATUS_REG_OFFSET);
  uint32_t part_status =
      abs_mmio_read32(kOtpCoreBase + OTP_CTRL_PARTITION_STATUS_0_REG_OFFSET);
  uint32_t intr_state =
      abs_mmio_read32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET);

  CHECK(err0 == OTP_CTRL_ERR_CODE_0_ERR_CODE_0_VALUE_ACCESS_ERROR,
        "Expected ERR_CODE_0 = AccessError (5), got %u", err0);
  CHECK(bitfield_bit32_read(status, OTP_CTRL_STATUS_PARTITION_ERROR_BIT),
        "Expected STATUS.PARTITION_ERROR = 1, got 0x%08x", status);
  CHECK(bitfield_bit32_read(part_status,
                            OTP_CTRL_PARTITION_STATUS_0_VENDOR_TEST_ERROR_BIT),
        "Expected PARTITION_STATUS_0.VENDOR_TEST_ERROR = 1, got 0x%08x",
        part_status);
  CHECK(bitfield_bit32_read(intr_state, OTP_CTRL_INTR_STATE_OTP_ERROR_BIT),
        "Expected INTR_STATE.otp_error = 1, got 0x%08x", intr_state);

  dif_otp_ctrl_status_t dif_status;
  CHECK_DIF_OK(dif_otp_ctrl_get_status(&otp, &dif_status));
  CHECK(bitfield_bit32_read(dif_status.codes,
                            kDifOtpCtrlStatusCodePartitionError),
        "Expected kDifOtpCtrlStatusCodePartitionError in dif_status.codes");
  CHECK(dif_status.causes[kDifOtpCtrlPartitionVendorTest] ==
            kDifOtpCtrlErrorLockedAccess,
        "Expected kDifOtpCtrlErrorLockedAccess for VENDOR_TEST");

  // 6b. Reading VENDOR_TEST's 64-bit SW digest (0x38) via SW_CFG_WINDOW is ALSO
  // blocked by VENDOR_TEST_READ_LOCK = 0 (`otp_ctrl_part_unbuf.sv:395-409`).
  clear_otp_irqs();
  access_fault_count = 0;
  expect_access_fault = true;
  uint32_t dig_win =
      *(volatile uint32_t *)(kOtpSwCfgBase + kVendorTestDigestOffset);
  (void)dig_win;
  asm volatile("" ::: "memory");
  expect_access_fault = false;

  CHECK(
      access_fault_count == 1 && last_fault_mcause == kRiscvExcLoadAccessFault,
      "Expected Load Access Fault on locked VENDOR_TEST SW digest window read");
  CHECK(bitfield_bit32_read(
            abs_mmio_read32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET),
            OTP_CTRL_INTR_STATE_OTP_ERROR_BIT),
        "Expected INTR_STATE.otp_error = 1 on SW digest window read");

  // Verify that reading unlocked OWNER_SW_CFG via SW_CFG_WINDOW succeeds with
  // 0 faults and ERR_CODE_2 == 0, while VENDOR_TEST's ERR_CODE_0 == 5,
  // PARTITION_STATUS_0.VENDOR_TEST_ERROR == 1, and STATUS.PARTITION_ERROR == 1
  // remain set.
  access_fault_count = 0;
  expect_access_fault = false;
  uint32_t owner_word =
      *(volatile uint32_t *)(kOtpSwCfgBase + kOwnerSwCfgOffset);
  (void)owner_word;
  CHECK(access_fault_count == 0,
        "Unlocked OWNER_SW_CFG window read must not fault");
  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_ERR_CODE_2_REG_OFFSET) ==
            OTP_CTRL_ERR_CODE_0_ERR_CODE_0_VALUE_NO_ERROR,
        "Expected ERR_CODE_2 (OWNER_SW_CFG) == 0");
  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_ERR_CODE_0_REG_OFFSET) ==
            OTP_CTRL_ERR_CODE_0_ERR_CODE_0_VALUE_ACCESS_ERROR,
        "Expected ERR_CODE_0 (VENDOR_TEST) to remain AccessError (5)");
  CHECK(bitfield_bit32_read(
            abs_mmio_read32(kOtpCoreBase +
                            OTP_CTRL_PARTITION_STATUS_0_REG_OFFSET),
            OTP_CTRL_PARTITION_STATUS_0_VENDOR_TEST_ERROR_BIT),
        "Expected PARTITION_STATUS_0.VENDOR_TEST_ERROR to remain 1");
  CHECK(bitfield_bit32_read(
            abs_mmio_read32(kOtpCoreBase + OTP_CTRL_STATUS_REG_OFFSET),
            OTP_CTRL_STATUS_PARTITION_ERROR_BIT),
        "Expected STATUS.PARTITION_ERROR to remain 1");

  // 6c. Reading VENDOR_TEST's 64-bit SW digest (0x38) via DAI is ALSO blocked
  // by VENDOR_TEST_READ_LOCK = 0 (`otp_ctrl_dai.sv:336-342` only exempts
  // `hw_digest` and `zeroizable`, not `sw_digest`), setting ERR_CODE_11 =
  // AccessError (5), asserting BOTH otp_operation_done and otp_error IRQs, and
  // blanking RDATA_0/1! Prime RDATA_0 with a valid HW_CFG0 DAI read first.
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_ADDRESS_REG_OFFSET,
                   kHwCfg0Offset);
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_CMD_REG_OFFSET,
                   (1u << OTP_CTRL_DIRECT_ACCESS_CMD_RD_BIT));
  wait_for_dai_idle();
  CHECK(abs_mmio_read32(kOtpCoreBase +
                        OTP_CTRL_DIRECT_ACCESS_RDATA_0_REG_OFFSET) != 0,
        "Expected non-zero RDATA_0 after HW_CFG0 DAI read");
  clear_otp_irqs();

  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_ADDRESS_REG_OFFSET,
                   kVendorTestDigestOffset);
  abs_mmio_write32(kOtpCoreBase + OTP_CTRL_DIRECT_ACCESS_CMD_REG_OFFSET,
                   (1u << OTP_CTRL_DIRECT_ACCESS_CMD_RD_BIT));
  wait_for_dai_idle();

  CHECK(
      abs_mmio_read32(kOtpCoreBase + OTP_CTRL_ERR_CODE_11_REG_OFFSET) ==
          OTP_CTRL_ERR_CODE_0_ERR_CODE_0_VALUE_ACCESS_ERROR,
      "Expected ERR_CODE_11 = AccessError (5) on DAI read of locked SW digest");
  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET) ==
            ((1u << OTP_CTRL_INTR_STATE_OTP_OPERATION_DONE_BIT) |
             (1u << OTP_CTRL_INTR_STATE_OTP_ERROR_BIT)),
        "Expected both otp_operation_done and otp_error on failed DAI SW "
        "digest read");
  CHECK(abs_mmio_read32(kOtpCoreBase +
                        OTP_CTRL_DIRECT_ACCESS_RDATA_0_REG_OFFSET) == 0 &&
            abs_mmio_read32(kOtpCoreBase +
                            OTP_CTRL_DIRECT_ACCESS_RDATA_1_REG_OFFSET) == 0,
        "Expected RDATA_0/1 blanked to 0 after failed DAI SW digest read");

  // 6d. Dedicated CSRs VENDOR_TEST_DIGEST_0/1 remain readable without fault or
  // error.
  clear_otp_irqs();
  uint32_t csr_dig0_after =
      abs_mmio_read32(kOtpCoreBase + OTP_CTRL_VENDOR_TEST_DIGEST_0_REG_OFFSET);
  uint32_t csr_dig1_after =
      abs_mmio_read32(kOtpCoreBase + OTP_CTRL_VENDOR_TEST_DIGEST_1_REG_OFFSET);
  CHECK(csr_dig0_after == csr_dig0_before && csr_dig1_after == csr_dig1_before,
        "Dedicated VENDOR_TEST_DIGEST_0/1 CSRs must remain readable when "
        "read-locked");
  CHECK(abs_mmio_read32(kOtpCoreBase + OTP_CTRL_INTR_STATE_REG_OFFSET) == 0,
        "Reading VENDOR_TEST_DIGEST_0/1 CSRs must not trigger otp_error IRQ");
  check_no_otp_alerts();
}

bool test_main(void) {
  CHECK_DIF_OK(dif_otp_ctrl_init_from_dt((dt_otp_ctrl_t)0, &otp));
  CHECK_DIF_OK(dif_alert_handler_init(mmio_region_from_addr(kAlertHandlerBase),
                                      &alert_handler));

  test_prim_bus_lc_gate_access_faults();
  test_core_subword_write_faults();
  test_sw_cfg_window_write_rejection();
  test_v2_zeroize_cmd_access_error_and_rdata_blanking();
  test_dif_read_blocking_word_vs_byte_bounds_bypass();
  test_read_locked_window_and_unbuf_sw_digest_and_dai_errata();

  LOG_INFO("All Earlgrey v2 OTP_CTRL errata checks passed on CW340 FPGA!");
  return true;
}
