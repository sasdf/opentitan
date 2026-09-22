// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "lc_ctrl_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kLcCtrlBase = TOP_EARLGREY_LC_CTRL_BASE_ADDR,
};

static volatile bool load_access_fault_seen = false;
static volatile bool store_access_fault_seen = false;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  uint32_t mcause = ibex_mcause_read();
  if ((ibex_exc_t)(mcause & kIbexExcMax) == kIbexExcLoadAccessFault) {
    load_access_fault_seen = true;
  } else if ((ibex_exc_t)(mcause & kIbexExcMax) == kIbexExcStoreAccessFault) {
    store_access_fault_seen = true;
  } else {
    ottf_generic_fault_print(exc_info, "Unexpected Load/Store Fault", mcause);
    abort();
  }
}

bool test_main(void) {
  // 1. W/O ALERT_TEST (0x00) reads back as 0.
  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_ALERT_TEST_REG_OFFSET) == 0u);

  // 2. Ensure hardware mutex is unclaimed (CLAIM_TRANSITION_IF = MuBi8False,
  // TRANSITION_REGWEN == 0).
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_CLAIM_TRANSITION_IF_REG_OFFSET,
                   kMultiBitBool8False);
  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_CLAIM_TRANSITION_IF_REG_OFFSET) ==
        kMultiBitBool8False);
  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_REGWEN_REG_OFFSET) ==
        0u);

  // 3. While mutex is unclaimed, OTP_VENDOR_TEST_STATUS reads 0, and writes to
  // TRANSITION_CMD, TRANSITION_CTRL, TRANSITION_TOKEN_0..3, TRANSITION_TARGET,
  // and OTP_VENDOR_TEST_CTRL must be ignored.
  CHECK(abs_mmio_read32(kLcCtrlBase +
                        LC_CTRL_OTP_VENDOR_TEST_STATUS_REG_OFFSET) == 0u);
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_TRANSITION_CMD_REG_OFFSET, 1u);
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_TRANSITION_CTRL_REG_OFFSET, 1u);
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_TRANSITION_TOKEN_0_REG_OFFSET,
                   0x11223344u);
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_TRANSITION_TOKEN_1_REG_OFFSET,
                   0x55667788u);
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_TRANSITION_TOKEN_2_REG_OFFSET,
                   0x99aabbcCu);
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_TRANSITION_TOKEN_3_REG_OFFSET,
                   0xddeeff00u);
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_TRANSITION_TARGET_REG_OFFSET,
                   0x12345678u);
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_OTP_VENDOR_TEST_CTRL_REG_OFFSET,
                   0xabcdef01u);

  // 4. Claim hardware mutex (CLAIM_TRANSITION_IF = MuBi8True) and verify all
  // un-claimed writes were ignored (all protected registers still read 0).
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_CLAIM_TRANSITION_IF_REG_OFFSET,
                   kMultiBitBool8True);
  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_CLAIM_TRANSITION_IF_REG_OFFSET) ==
        kMultiBitBool8True);
  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_REGWEN_REG_OFFSET) ==
        1u);

  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_CTRL_REG_OFFSET) ==
        0u);
  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_TOKEN_0_REG_OFFSET) ==
        0u);
  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_TOKEN_1_REG_OFFSET) ==
        0u);
  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_TOKEN_2_REG_OFFSET) ==
        0u);
  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_TOKEN_3_REG_OFFSET) ==
        0u);
  CHECK(abs_mmio_read32(kLcCtrlBase + LC_CTRL_TRANSITION_TARGET_REG_OFFSET) ==
        0u);
  CHECK(abs_mmio_read32(kLcCtrlBase +
                        LC_CTRL_OTP_VENDOR_TEST_CTRL_REG_OFFSET) == 0u);

  // Read OTP_VENDOR_TEST_STATUS while holding the mutex, and verify
  // OTP_VENDOR_TEST_CTRL write/read under mutex.
  uint32_t vendor_status =
      abs_mmio_read32(kLcCtrlBase + LC_CTRL_OTP_VENDOR_TEST_STATUS_REG_OFFSET);
  CHECK(vendor_status == 0u);
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_OTP_VENDOR_TEST_CTRL_REG_OFFSET,
                   0x5a5a5a5au);
  CHECK(
      abs_mmio_read32(kLcCtrlBase + LC_CTRL_OTP_VENDOR_TEST_CTRL_REG_OFFSET) ==
      0x5a5a5a5au);

  // Release hardware mutex -> OTP_VENDOR_TEST_CTRL reads 0 when not owner.
  abs_mmio_write32(kLcCtrlBase + LC_CTRL_CLAIM_TRANSITION_IF_REG_OFFSET,
                   kMultiBitBool8False);
  CHECK(abs_mmio_read32(kLcCtrlBase +
                        LC_CTRL_OTP_VENDOR_TEST_CTRL_REG_OFFSET) == 0u);

  // 5. Out-of-bounds CSR read/write at offset 0x8c raises Load/Store Access
  // Fault.
  load_access_fault_seen = false;
  (void)abs_mmio_read32(kLcCtrlBase + 0x8cu);
  CHECK(load_access_fault_seen);

  store_access_fault_seen = false;
  abs_mmio_write32(kLcCtrlBase + 0x8cu, 0xdeadbeefu);
  CHECK(store_access_fault_seen);

  return true;
}
