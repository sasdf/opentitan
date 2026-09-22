// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stddef.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"
#include "sw/device/silicon_creator/lib/drivers/retention_sram.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sram_ctrl_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kSramRetRegsBase = TOP_EARLGREY_SRAM_CTRL_RET_AON_REGS_BASE_ADDR,
  kSramRetRamBase = TOP_EARLGREY_SRAM_CTRL_RET_AON_RAM_BASE_ADDR,
};

static volatile bool kLoadStoreFault = false;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  kLoadStoreFault = true;
}

bool test_main(void) {
  uintptr_t ret_buf = kSramRetRamBase + offsetof(retention_sram_t, owner);
  const uint32_t kMarker = 0x6b4abfaeu;

  // 1. Write marker word, trigger CTRL.INIT, and immediately read from
  // retention SRAM without polling STATUS.INIT_DONE first.
  // In RTL, the TL-UL read stalls until initialization completes; in QEMU,
  // ot_sram_ctrl_mem_init_read_with_attrs expedites initialization and returns
  // the newly initialized word without fault.
  abs_mmio_write32(ret_buf, kMarker);
  CHECK(abs_mmio_read32(ret_buf) == kMarker);

  kLoadStoreFault = false;
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_CTRL_REG_OFFSET,
                   1u << SRAM_CTRL_CTRL_INIT_BIT);
  uint32_t imm_read_word = abs_mmio_read32(ret_buf);
  CHECK(!kLoadStoreFault);
  CHECK(imm_read_word != kMarker);

  uint32_t status =
      abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_STATUS_REG_OFFSET);
  CHECK(bitfield_bit32_read(status, SRAM_CTRL_STATUS_INIT_DONE_BIT));

  // 2. Out-of-bounds CSR read at offset 0x24 raises LoadAccessFault.
  kLoadStoreFault = false;
  (void)abs_mmio_read32(kSramRetRegsBase + 0x24u);
  CHECK(kLoadStoreFault);

  return true;
}
