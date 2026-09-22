// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "rom_ctrl_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kRomCtrlRegsBase = TOP_EARLGREY_ROM_CTRL_REGS_BASE_ADDR,
  kRomCtrlRomBase = TOP_EARLGREY_ROM_CTRL_ROM_BASE_ADDR,
  kRomCtrlRomSize = TOP_EARLGREY_ROM_CTRL_ROM_SIZE_BYTES,
};

static volatile bool store_access_fault_seen = false;
static volatile bool load_access_fault_seen = false;

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

bool test_main(void) {
  // 1. Sub-word (size < 4) CSR reads from DIGEST_0 and EXP_DIGEST_0.
  uint32_t d0 =
      abs_mmio_read32(kRomCtrlRegsBase + ROM_CTRL_DIGEST_0_REG_OFFSET);
  uint32_t e0 =
      abs_mmio_read32(kRomCtrlRegsBase + ROM_CTRL_EXP_DIGEST_0_REG_OFFSET);
  CHECK(d0 == e0);
  for (uint32_t b = 0; b < 4u; ++b) {
    uint8_t db =
        abs_mmio_read8(kRomCtrlRegsBase + ROM_CTRL_DIGEST_0_REG_OFFSET + b);
    uint8_t eb =
        abs_mmio_read8(kRomCtrlRegsBase + ROM_CTRL_EXP_DIGEST_0_REG_OFFSET + b);
    CHECK(db == (uint8_t)((d0 >> (b * 8u)) & 0xffu));
    CHECK(eb == (uint8_t)((e0 >> (b * 8u)) & 0xffu));
  }

  // 2. Out-of-bounds CSR read (offset 0x48 >= REGS_COUNT * 4) raises
  // LoadAccessFault.
  load_access_fault_seen = false;
  (void)abs_mmio_read32(kRomCtrlRegsBase + 0x48u);
  CHECK(load_access_fault_seen);

  // 3. ROM digest memory window (top 32 bytes) StoreAccessFault via TL-UL.
  uint32_t orig_pmpaddr0 = 0;
  uint32_t orig_pmpcfg0 = 0;
  CSR_READ(CSR_REG_PMPADDR0, &orig_pmpaddr0);
  CSR_READ(CSR_REG_PMPCFG0, &orig_pmpcfg0);

  const uint32_t kRomNapotAddr =
      (kRomCtrlRomBase >> 2) | ((kRomCtrlRomSize - 1u) >> 3);
  const uint32_t kPmp0CfgRwLocked = 0x9bu;
  const uint32_t kPmp0CfgRwUnlocked = 0x1bu;
  CSR_WRITE(CSR_REG_PMPADDR0, kRomNapotAddr);
  CSR_WRITE(CSR_REG_PMPCFG0, (orig_pmpcfg0 & ~0xffu) | kPmp0CfgRwLocked);
  uint32_t active_pmpcfg0 = 0;
  CSR_READ(CSR_REG_PMPCFG0, &active_pmpcfg0);
  if ((active_pmpcfg0 & 0xffu) != kPmp0CfgRwLocked) {
    CSR_WRITE(CSR_REG_PMPCFG0, (orig_pmpcfg0 & ~0xffu) | kPmp0CfgRwUnlocked);
  }

  uint32_t top_word_addr = kRomCtrlRomBase + kRomCtrlRomSize - sizeof(uint32_t);
  store_access_fault_seen = false;
  abs_mmio_write32(top_word_addr, 0xdeadbeefu);
  CHECK(store_access_fault_seen);

  CSR_WRITE(CSR_REG_PMPCFG0, orig_pmpcfg0);
  CSR_WRITE(CSR_REG_PMPADDR0, orig_pmpaddr0);

  return true;
}
