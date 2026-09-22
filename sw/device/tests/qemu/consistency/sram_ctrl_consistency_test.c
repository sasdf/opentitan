// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/dif/dif_sram_ctrl.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
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
  kSramMainRegsBase = TOP_EARLGREY_SRAM_CTRL_MAIN_REGS_BASE_ADDR,
  kTestWords = 8,
};

static const uint32_t kTestPattern[kTestWords] = {
    0x6b4abfaeu, 0x63bdb6e7u, 0x87f99b1au, 0xa214dffeu,
    0xb12291f9u, 0xd0cd1abeu, 0x5c95e716u, 0xe887aab1u,
};

static volatile bool kLoadStoreFault = false;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  kLoadStoreFault = true;
  uint32_t mepc = ibex_mepc_read();
  uint16_t inst = *(volatile uint16_t *)mepc;
  uint32_t step = ((inst & 0x3u) == 0x3u) ? 4u : 2u;
  CSR_WRITE(CSR_REG_MEPC, mepc + step);
}

#define EXPECT_RTL(cond, ...)                  \
  do {                                         \
    if (!(cond)) {                             \
      LOG_ERROR("RTL_MISMATCH: " __VA_ARGS__); \
      all_ok = false;                          \
    }                                          \
  } while (0)

bool test_main(void) {
  bool all_ok = true;

  // 1. ALERT_TEST (0x00) is WO: reads back as 0.
  EXPECT_RTL(
      abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_ALERT_TEST_REG_OFFSET) == 0u,
      "ALERT_TEST must read back as 0");
  EXPECT_RTL(abs_mmio_read32(kSramMainRegsBase +
                             SRAM_CTRL_ALERT_TEST_REG_OFFSET) == 0u,
             "Main SRAM ALERT_TEST must read back as 0");

  // 2. STATUS (0x04) is RO: software writes must be ignored and reserved bits
  // [31:6] read as 0.
  uint32_t status_before =
      abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_STATUS_REG_OFFSET);
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_STATUS_REG_OFFSET, 0xffffffffu);
  uint32_t status_after =
      abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_STATUS_REG_OFFSET);
  EXPECT_RTL(status_after == status_before && (status_after & ~0x3fu) == 0u,
             "STATUS is RO (6-bit); before=0x%08x, after=0x%08x", status_before,
             status_after);

  // 3. EXEC_REGWEN (0x08), CTRL_REGWEN (0x10), READBACK_REGWEN (0x1c):
  // 1-bit W0C registers with reset value 1; writing 1 preserves 1 and reserved
  // bits read as 0.
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_EXEC_REGWEN_REG_OFFSET,
                   0xffffffffu);
  EXPECT_RTL(abs_mmio_read32(kSramRetRegsBase +
                             SRAM_CTRL_EXEC_REGWEN_REG_OFFSET) == 1u,
             "EXEC_REGWEN expected 1 after writing 0xffffffff");

  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_CTRL_REGWEN_REG_OFFSET,
                   0xffffffffu);
  EXPECT_RTL(abs_mmio_read32(kSramRetRegsBase +
                             SRAM_CTRL_CTRL_REGWEN_REG_OFFSET) == 1u,
             "CTRL_REGWEN expected 1 after writing 0xffffffff");

  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_READBACK_REGWEN_REG_OFFSET,
                   0xffffffffu);
  EXPECT_RTL(abs_mmio_read32(kSramRetRegsBase +
                             SRAM_CTRL_READBACK_REGWEN_REG_OFFSET) == 1u,
             "READBACK_REGWEN expected 1 after writing 0xffffffff");

  // 4. EXEC (0x0c) and READBACK (0x20) 4-bit MuBi4 RW registers:
  // Reserved bits [31:4] must read back as 0.
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_EXEC_REG_OFFSET,
                   0xfffffff0u | kMultiBitBool4True);
  EXPECT_RTL(abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_EXEC_REG_OFFSET) ==
                 kMultiBitBool4True,
             "EXEC expected 0x6 (MuBi4True) with upper bits masked");
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_EXEC_REG_OFFSET,
                   kMultiBitBool4False);
  EXPECT_RTL(abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_EXEC_REG_OFFSET) ==
                 kMultiBitBool4False,
             "EXEC expected 0x9 (MuBi4False)");

  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_READBACK_REG_OFFSET,
                   0xfffffff0u | kMultiBitBool4True);
  EXPECT_RTL(
      abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_READBACK_REG_OFFSET) ==
          kMultiBitBool4True,
      "READBACK expected 0x6 (MuBi4True) with upper bits masked");
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_READBACK_REG_OFFSET,
                   kMultiBitBool4False);
  EXPECT_RTL(
      abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_READBACK_REG_OFFSET) ==
          kMultiBitBool4False,
      "READBACK expected 0x9 (MuBi4False)");

  // 5. SCR_KEY_ROTATED (0x18) MuBi4 W1C & CTRL.RENEW_SCR_KEY (0x14 bit 0):
  // Clear SCR_KEY_ROTATED by writing MuBi4True (0x6) -> reads MuBi4False (0x9).
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET,
                   kMultiBitBool4True);
  EXPECT_RTL(abs_mmio_read32(kSramRetRegsBase +
                             SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET) ==
                 kMultiBitBool4False,
             "SCR_KEY_ROTATED expected 0x9 (MuBi4False) after W1C clear");

  uintptr_t ret_buf = kSramRetRamBase + offsetof(retention_sram_t, owner);
  for (size_t i = 0; i < kTestWords; ++i) {
    abs_mmio_write32(ret_buf + i * sizeof(uint32_t), kTestPattern[i]);
  }
  for (size_t i = 0; i < kTestWords; ++i) {
    EXPECT_RTL(
        abs_mmio_read32(ret_buf + i * sizeof(uint32_t)) == kTestPattern[i],
        "Initial retention SRAM write/read mismatch at word %u", i);
  }

  // Trigger CTRL.RENEW_SCR_KEY.
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_CTRL_REG_OFFSET,
                   1u << SRAM_CTRL_CTRL_RENEW_SCR_KEY_BIT);
  uint32_t status = 0;
  do {
    status = abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_STATUS_REG_OFFSET);
  } while (!bitfield_bit32_read(status, SRAM_CTRL_STATUS_SCR_KEY_VALID_BIT));

  EXPECT_RTL(
      bitfield_bit32_read(status, SRAM_CTRL_STATUS_SCR_KEY_SEED_VALID_BIT),
      "STATUS.SCR_KEY_SEED_VALID should be set after key renewal (0x%08x)",
      status);
  EXPECT_RTL(abs_mmio_read32(kSramRetRegsBase +
                             SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET) ==
                 kMultiBitBool4True,
             "SCR_KEY_ROTATED should be 0x6 (MuBi4True) after key renewal");
  EXPECT_RTL(
      abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_CTRL_REG_OFFSET) == 0u,
      "CTRL is WO and must read back as 0");

  // Verify MuBi4 W1C (mubi4_and_hi) semantics on SCR_KEY_ROTATED (q = 0x6):
  // - Writing 0x9 (~wd = 0x6) preserves 0x6.
  // - Writing 0x0 (~wd = 0xf) sets q = 0xf.
  // - Writing 0x6 (~wd = 0x9) clears q = 0x9 (MuBi4False).
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET,
                   kMultiBitBool4False);
  EXPECT_RTL(abs_mmio_read32(kSramRetRegsBase +
                             SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET) ==
                 kMultiBitBool4True,
             "SCR_KEY_ROTATED (0x6) should remain 0x6 after writing 0x9");
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET,
                   0x0u);
  EXPECT_RTL(abs_mmio_read32(kSramRetRegsBase +
                             SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET) == 0xfu,
             "SCR_KEY_ROTATED (0x6) should become 0xf after writing 0x0");
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET,
                   kMultiBitBool4True);
  EXPECT_RTL(
      abs_mmio_read32(kSramRetRegsBase +
                      SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET) ==
          kMultiBitBool4False,
      "SCR_KEY_ROTATED should clear to 0x9 (MuBi4False) after writing 0x6");

  // 6. CTRL.INIT (0x14 bit 1): initializes SRAM, CTRL reads as 0 immediately,
  // and sets STATUS.INIT_DONE.
  for (size_t i = 0; i < kTestWords; ++i) {
    abs_mmio_write32(ret_buf + i * sizeof(uint32_t), kTestPattern[i]);
  }
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_CTRL_REG_OFFSET,
                   1u << SRAM_CTRL_CTRL_INIT_BIT);
  EXPECT_RTL(
      abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_CTRL_REG_OFFSET) == 0u,
      "CTRL is WO and must read back as 0 even immediately after CTRL.INIT "
      "write");
  do {
    status = abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_STATUS_REG_OFFSET);
  } while (!bitfield_bit32_read(status, SRAM_CTRL_STATUS_INIT_DONE_BIT));

  bool any_changed = false;
  for (size_t i = 0; i < kTestWords; ++i) {
    if (abs_mmio_read32(ret_buf + i * sizeof(uint32_t)) != kTestPattern[i]) {
      any_changed = true;
      break;
    }
  }
  EXPECT_RTL(any_changed, "CTRL.INIT must wipe/re-initialize SRAM contents");

  // 6a-2. Trigger CTRL.INIT and immediately read from retention SRAM before
  // polling STATUS.INIT_DONE: TL-UL read stalls until init finishes and
  // succeeds without bus fault.
  abs_mmio_write32(ret_buf, kTestPattern[0]);
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_CTRL_REG_OFFSET,
                   1u << SRAM_CTRL_CTRL_INIT_BIT);
  uint32_t imm_read_word = abs_mmio_read32(ret_buf);
  EXPECT_RTL(!kLoadStoreFault && imm_read_word != kTestPattern[0],
             "Immediate SRAM read during CTRL.INIT must succeed and return "
             "re-initialized word");
  status = abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_STATUS_REG_OFFSET);
  EXPECT_RTL(bitfield_bit32_read(status, SRAM_CTRL_STATUS_INIT_DONE_BIT),
             "STATUS.INIT_DONE must be 1 after immediate SRAM read completes");

  // 6b. Writing CTRL.RENEW_SCR_KEY while CTRL.INIT is in progress (init_q == 1)
  // must be ignored (sram_ctrl.sv: key_req gated by !init_q).
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET,
                   kMultiBitBool4True);
  EXPECT_RTL(abs_mmio_read32(kSramRetRegsBase +
                             SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET) ==
                 kMultiBitBool4False,
             "SCR_KEY_ROTATED must be 0x9 before CTRL.INIT test");
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_CTRL_REG_OFFSET,
                   1u << SRAM_CTRL_CTRL_INIT_BIT);
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_CTRL_REG_OFFSET,
                   1u << SRAM_CTRL_CTRL_RENEW_SCR_KEY_BIT);
  do {
    status = abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_STATUS_REG_OFFSET);
  } while (!bitfield_bit32_read(status, SRAM_CTRL_STATUS_INIT_DONE_BIT));
  EXPECT_RTL(
      abs_mmio_read32(kSramRetRegsBase +
                      SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET) ==
          kMultiBitBool4False,
      "CTRL.RENEW_SCR_KEY written while CTRL.INIT is active must be "
      "ignored (SCR_KEY_ROTATED expected 0x9, got 0x%x)",
      abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET));

  // Simultaneous CTRL.RENEW_SCR_KEY | CTRL.INIT in a single write when idle
  // must trigger both key renewal and initialization.
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_CTRL_REG_OFFSET,
                   (1u << SRAM_CTRL_CTRL_RENEW_SCR_KEY_BIT) |
                       (1u << SRAM_CTRL_CTRL_INIT_BIT));
  do {
    status = abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_STATUS_REG_OFFSET);
  } while (!bitfield_bit32_read(status, SRAM_CTRL_STATUS_INIT_DONE_BIT) ||
           !bitfield_bit32_read(status, SRAM_CTRL_STATUS_SCR_KEY_VALID_BIT));
  EXPECT_RTL(abs_mmio_read32(kSramRetRegsBase +
                             SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET) ==
                 kMultiBitBool4True,
             "Simultaneous RENEW_SCR_KEY | INIT must rotate key (0x6)");
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET,
                   kMultiBitBool4True);

  // 6c. Wave 5: Verify SRAM_CTRL_REGS_PERMIT sub-word write error (wr_err) and
  // addrmiss decode error (sram_ctrl_reg_pkg.sv:152-162,
  // sram_ctrl_regs_reg_top.sv:656-670).
  kLoadStoreFault = false;
  abs_mmio_write8(kSramRetRegsBase + SRAM_CTRL_EXEC_REG_OFFSET + 1u, 0x06u);
  EXPECT_RTL(kLoadStoreFault,
             "Expected Store Access Fault on 8-bit write to byte 1 of "
             "SRAM_CTRL_EXEC (PERMIT=4'b0001)");
  EXPECT_RTL(abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_EXEC_REG_OFFSET) ==
                 kMultiBitBool4False,
             "EXEC must remain 0x9 after rejected sub-word write to byte 1");

  kLoadStoreFault = false;
  abs_mmio_write8(kSramRetRegsBase + SRAM_CTRL_EXEC_REG_OFFSET,
                  kMultiBitBool4True);
  EXPECT_RTL(!kLoadStoreFault && abs_mmio_read32(kSramRetRegsBase +
                                                 SRAM_CTRL_EXEC_REG_OFFSET) ==
                                     kMultiBitBool4True,
             "8-bit write to byte 0 of SRAM_CTRL_EXEC (PERMIT=4'b0001) must "
             "succeed");
  abs_mmio_write8(kSramRetRegsBase + SRAM_CTRL_EXEC_REG_OFFSET,
                  kMultiBitBool4False);

  kLoadStoreFault = false;
  (void)abs_mmio_read32(kSramRetRegsBase + 0x24u);
  EXPECT_RTL(kLoadStoreFault,
             "Expected Load Access Fault on unmapped offset 0x24 (addrmiss)");

  // 7. Lock REGWEN registers (W0C) and verify locked registers ignore writes.
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_EXEC_REGWEN_REG_OFFSET, 0u);
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_EXEC_REGWEN_REG_OFFSET, 1u);
  EXPECT_RTL(abs_mmio_read32(kSramRetRegsBase +
                             SRAM_CTRL_EXEC_REGWEN_REG_OFFSET) == 0u,
             "EXEC_REGWEN is W0C and must stay 0 once cleared");
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_EXEC_REG_OFFSET,
                   kMultiBitBool4True);
  EXPECT_RTL(abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_EXEC_REG_OFFSET) ==
                 kMultiBitBool4False,
             "EXEC write must be ignored when EXEC_REGWEN == 0");

  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_READBACK_REGWEN_REG_OFFSET, 0u);
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_READBACK_REGWEN_REG_OFFSET, 1u);
  EXPECT_RTL(abs_mmio_read32(kSramRetRegsBase +
                             SRAM_CTRL_READBACK_REGWEN_REG_OFFSET) == 0u,
             "READBACK_REGWEN is W0C and must stay 0 once cleared");
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_READBACK_REG_OFFSET,
                   kMultiBitBool4True);
  EXPECT_RTL(
      abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_READBACK_REG_OFFSET) ==
          kMultiBitBool4False,
      "READBACK write must be ignored when READBACK_REGWEN == 0");

  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_CTRL_REGWEN_REG_OFFSET, 0u);
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_CTRL_REGWEN_REG_OFFSET, 1u);
  EXPECT_RTL(abs_mmio_read32(kSramRetRegsBase +
                             SRAM_CTRL_CTRL_REGWEN_REG_OFFSET) == 0u,
             "CTRL_REGWEN is W0C and must stay 0 once cleared");
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_CTRL_REG_OFFSET,
                   1u << SRAM_CTRL_CTRL_RENEW_SCR_KEY_BIT);
  EXPECT_RTL(abs_mmio_read32(kSramRetRegsBase +
                             SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET) ==
                 kMultiBitBool4False,
             "CTRL.RENEW_SCR_KEY must be ignored when CTRL_REGWEN == 0");

  return all_ok;
}
