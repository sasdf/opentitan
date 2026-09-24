// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/dif/dif_sram_ctrl.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"
#include "sw/device/silicon_creator/lib/drivers/retention_sram.h"

#include "hw/top/sram_ctrl_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kSramRetRegsBase = TOP_EARLGREY_SRAM_CTRL_RET_REGS_BASE_ADDR,
  kSramRetRamBase = TOP_EARLGREY_SRAM_CTRL_RET_RAM_BASE_ADDR,
  kSramMetaRegsBase = TOP_EARLGREY_SRAM_CTRL_META_REGS_BASE_ADDR,
  kSramMetaRamBase = TOP_EARLGREY_SRAM_CTRL_META_RAM_BASE_ADDR,
  kIbexLoadIntegrityNmiMcause = 0xffffffe0u,
};

static volatile bool g_load_store_fault = false;
static volatile uint32_t g_last_mcause = 0;
static volatile uint32_t g_nmi_load_integ_count = 0;

static void advance_mepc_over_faulting_insn(void) {
  uint32_t mepc = 0;
  CSR_READ(CSR_REG_MEPC, &mepc);
  uint16_t insn_half = *(const volatile uint16_t *)mepc;
  uint32_t step = ((insn_half & 0x3u) == 0x3u) ? 4u : 2u;
  CSR_WRITE(CSR_REG_MEPC, mepc + step);
}

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = 0;
  CSR_READ(CSR_REG_MCAUSE, &mcause);
  g_load_store_fault = true;
  g_last_mcause = mcause;
  advance_mepc_over_faulting_insn();
}

void ottf_internal_isr(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = 0;
  CSR_READ(CSR_REG_MCAUSE, &mcause);
  if (mcause == kIbexLoadIntegrityNmiMcause) {
    g_nmi_load_integ_count++;
  }
}

static void sram_renew_key(uintptr_t regs_base) {
  abs_mmio_write32(regs_base + SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET,
                   kMultiBitBool4True);
  abs_mmio_write32(regs_base + SRAM_CTRL_CTRL_REG_OFFSET,
                   1u << SRAM_CTRL_CTRL_RENEW_SCR_KEY_BIT);
  while (!bitfield_bit32_read(
      abs_mmio_read32(regs_base + SRAM_CTRL_STATUS_REG_OFFSET),
      SRAM_CTRL_STATUS_SCR_KEY_VALID_BIT)) {
  }
}

static void sram_init_mem(uintptr_t regs_base) {
  abs_mmio_write32(regs_base + SRAM_CTRL_CTRL_REG_OFFSET,
                   1u << SRAM_CTRL_CTRL_INIT_BIT);
  while (!bitfield_bit32_read(
      abs_mmio_read32(regs_base + SRAM_CTRL_STATUS_REG_OFFSET),
      SRAM_CTRL_STATUS_INIT_DONE_BIT)) {
  }
}

bool test_main(void) {
  LOG_INFO("=== OpenTitan Earlgrey v2 (trunk-v2) SRAM_CTRL Errata Suite ===");
  CHECK_STATUS_OK(
      ottf_alerts_ignore_alert(kTopEarlgreyAlertIdRvCoreIbexFatalHwErr));
  CHECK_STATUS_OK(
      ottf_alerts_ignore_alert(kTopEarlgreyAlertIdSramCtrlRetFatalError));

  uintptr_t ret_buf = kSramRetRamBase + offsetof(retention_sram_t, owner);

  // ---------------------------------------------------------------------------
  // 1. [sram_ctrl_regs_reg_top.sv:566-590, prim_subreg_arb.sv:89-93]:
  //    SCR_KEY_ROTATED (0x18) MuBi4 SwAccessW1C bitwise mubi4_and_hi(q, ~wd)
  //    behavior: writing 0x9 preserves 0x6 (true no-op), writing 0x0 corrupts
  //    0x6 into 0xf, writing 0xf corrupts 0x6 into 0x0, and writing 0x6 clears
  //    0x6 to 0x9.
  // ---------------------------------------------------------------------------
  sram_renew_key(kSramRetRegsBase);
  uint32_t rot =
      abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET);
  CHECK(rot == kMultiBitBool4True,
        "Expected SCR_KEY_ROTATED == 0x6 after RENEW_SCR_KEY, got 0x%x", rot);

  // Writing 0x9 (kMultiBitBool4False) is the true no-op: preserves 0x6.
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET,
                   kMultiBitBool4False);
  rot =
      abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET);
  CHECK(rot == kMultiBitBool4True,
        "[sram_ctrl_regs_reg_top.sv:566-590] Expected writing 0x9 to preserve "
        "SCR_KEY_ROTATED == 0x6, got 0x%x",
        rot);

  // Writing 0x0 (standard rw1c no-op) corrupts 0x6 into 0xf!
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET,
                   0x0u);
  rot =
      abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET);
  CHECK(rot == 0xfu,
        "[sram_ctrl_regs_reg_top.sv:566-590] Expected writing 0x0 to corrupt "
        "SCR_KEY_ROTATED from 0x6 to 0xf, got 0x%x",
        rot);

  // Rotate again (0x6) and write 0xf (standard rw1c clear) -> corrupts to 0x0!
  sram_renew_key(kSramRetRegsBase);
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET,
                   0xfu);
  rot =
      abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET);
  CHECK(rot == 0x0u,
        "[sram_ctrl_regs_reg_top.sv:566-590] Expected writing 0xf to corrupt "
        "SCR_KEY_ROTATED from 0x6 to 0x0, got 0x%x",
        rot);

  // Rotate again (0x6) after sram_init_mem so STATUS == 0x38 (INIT_DONE=1,
  // SCR_KEY_SEED_VALID=1, SCR_KEY_VALID=1, ESCALATED=0), and write 0x6
  // (kMultiBitBool4True) -> clears SCR_KEY_ROTATED cleanly to 0x9 while STATUS
  // remains 0x38:
  sram_init_mem(kSramRetRegsBase);
  sram_renew_key(kSramRetRegsBase);
  uint32_t status_before_w1c =
      abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_STATUS_REG_OFFSET);
  CHECK(status_before_w1c == 0x38u,
        "Expected STATUS == 0x38 (INIT_DONE=1, SCR_KEY_SEED_VALID=1, "
        "SCR_KEY_VALID=1, ESCALATED=0) before clearing SCR_KEY_ROTATED, got "
        "0x%x",
        status_before_w1c);
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET,
                   kMultiBitBool4True);
  rot =
      abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET);
  uint32_t status_after_w1c =
      abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_STATUS_REG_OFFSET);
  CHECK(rot == kMultiBitBool4False && status_after_w1c == 0x38u,
        "[sram_ctrl_regs_reg_top.sv:566-590] Expected writing 0x6 to clear "
        "SCR_KEY_ROTATED to 0x9 while preserving STATUS == 0x38, got rot=0x%x "
        "status=0x%x",
        rot, status_after_w1c);
  LOG_INFO("Test 1 (SCR_KEY_ROTATED mubi4_and_hi W1C semantics) PASSED");

  // ---------------------------------------------------------------------------
  // 2. [sram_ctrl_reg_pkg.sv:159-169, sram_ctrl_regs_reg_top.sv:721-732]:
  //    SRAM_CTRL_REGS_PERMIT = 4'b0001 accepts sb/sh to +0, faults sb to +1..+3
  //    (mcause=7), and faults unmapped read/write at 0x24 (mcause=5/7).
  // ---------------------------------------------------------------------------
  CHECK(abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_CTRL_REG_OFFSET) == 0u,
        "Expected WO register CTRL (0x14) to read back 0");

  g_load_store_fault = false;
  abs_mmio_write8(kSramRetRegsBase + SRAM_CTRL_EXEC_REGWEN_REG_OFFSET, 1u);
  CHECK(!g_load_store_fault,
        "[sram_ctrl_reg_pkg.sv:159-169] Expected sb to EXEC_REGWEN+0 "
        "(PERMIT=4'b0001) to succeed");

  g_load_store_fault = false;
  asm volatile("sh %0, 0(%1)"
               :
               : "r"((uint16_t)1u),
                 "r"(kSramRetRegsBase + SRAM_CTRL_EXEC_REGWEN_REG_OFFSET)
               : "memory");
  CHECK(!g_load_store_fault,
        "[sram_ctrl_reg_pkg.sv:159-169] Expected sh to EXEC_REGWEN+0 "
        "(reg_be=4'b0011, PERMIT=4'b0001) to succeed");

  for (uint32_t off = 1u; off <= 3u; ++off) {
    g_load_store_fault = false;
    g_last_mcause = 0;
    abs_mmio_write8(kSramRetRegsBase + SRAM_CTRL_EXEC_REGWEN_REG_OFFSET + off,
                    1u);
    CHECK(g_load_store_fault && g_last_mcause == 7u,
          "[sram_ctrl_reg_pkg.sv:159-169] Expected sb to EXEC_REGWEN+%u "
          "(reg_be[0]==0) to fault with mcause=7",
          off);
  }

  g_load_store_fault = false;
  g_last_mcause = 0;
  (void)abs_mmio_read32(kSramRetRegsBase + 0x24u);
  CHECK(g_load_store_fault && g_last_mcause == 5u,
        "[sram_ctrl_reg_pkg.sv:159-169] Expected unmapped CSR read at 0x24 to "
        "fault with mcause=5");

  g_load_store_fault = false;
  g_last_mcause = 0;
  abs_mmio_write32(kSramRetRegsBase + 0x24u, 0u);
  CHECK(g_load_store_fault && g_last_mcause == 7u,
        "[sram_ctrl_reg_pkg.sv:159-169] Expected unmapped CSR write at 0x24 to "
        "fault with mcause=7");
  LOG_INFO("Test 2 (SRAM_CTRL_REGS_PERMIT = 4'b0001 sub-word CSR) PASSED");

  // ---------------------------------------------------------------------------
  // 3. [sram_ctrl.sv:326-365, prim_ram_1p_scr.sv:198-265]:
  //    CTRL.RENEW_SCR_KEY = 1 without CTRL.INIT = 1 preserves STATUS.INIT_DONE
  //    == 1 while invalidating prior SRAM words, firing Ibex Load Integrity
  //    NMI (mcause = 0xffffffe0) on reads.
  // ---------------------------------------------------------------------------
  sram_init_mem(kSramRetRegsBase);
  uint32_t status =
      abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_STATUS_REG_OFFSET);
  CHECK(bitfield_bit32_read(status, SRAM_CTRL_STATUS_INIT_DONE_BIT) &&
            bitfield_bit32_read(status, SRAM_CTRL_STATUS_SCR_KEY_VALID_BIT),
        "Expected INIT_DONE=1 and SCR_KEY_VALID=1 after CTRL.INIT, got 0x%x",
        status);

  for (uint32_t i = 0; i < 8; ++i) {
    abs_mmio_write32(ret_buf + i * 4u, 0xa5a50000u + i);
  }
  abs_mmio_write8(ret_buf + 1u, 0x5au);
  g_load_store_fault = false;
  asm volatile("sh %0, 0(%1)"
               :
               : "r"((uint16_t)0x1234u), "r"(ret_buf + 2u)
               : "memory");
  CHECK(!g_load_store_fault && abs_mmio_read32(ret_buf) == 0x12345a00u,
        "Expected sub-word sb/sh RMW on SRAM data window to produce "
        "0x12345a00");

  g_nmi_load_integ_count = 0;
  sram_renew_key(kSramRetRegsBase);
  status = abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_STATUS_REG_OFFSET);
  CHECK(bitfield_bit32_read(status, SRAM_CTRL_STATUS_INIT_DONE_BIT),
        "[sram_ctrl.sv:326-365] Expected STATUS.INIT_DONE to remain 1 after "
        "CTRL.RENEW_SCR_KEY=1 without CTRL.INIT (status=0x%x)",
        status);

  // Find a stale word in ret_buf[0..7] that triggers Ibex Load Integrity NMI:
  uintptr_t corrupt_word_addr = 0;
  for (uint32_t i = 0; i < 8; ++i) {
    uint32_t before = g_nmi_load_integ_count;
    (void)abs_mmio_read32(ret_buf + i * 4u);
    if (g_nmi_load_integ_count > before) {
      corrupt_word_addr = ret_buf + i * 4u;
      break;
    }
  }
  CHECK(corrupt_word_addr != 0,
        "[sram_ctrl.sv:326-365] Expected reading stale SRAM words after "
        "CTRL.RENEW_SCR_KEY=1 to trigger Ibex Load Integrity NMI "
        "(mcause=0xffffffe0)");
  LOG_INFO("Test 3 (RENEW_SCR_KEY status independence & stale-key NMI) PASSED");

  // ---------------------------------------------------------------------------
  // 4. NEW_IN_V2 [sram_ctrl.sv:593-670, top_earlgrey.sv:51,98,105,158,
  //    tlul_sram_byte.sv:667-694]:
  //    Because EccCorrection = 0 on all Earlgrey v2 sram_ctrl instances
  //    (gen_no_ecc_correction ties sram_rerror = 2'b00) and tlul_sram_byte
  //    discards tl_sram_i.d_user.data_intg during the internal read phase
  //    (StWaitRd) of a sub-word write (sb/sh) before re-encoding a fresh
  //    7-bit SecdedInv3932 syndrome via u_tlul_data_integ_enc, performing a
  //    single 1-byte write (sb) to a corrupted/stale-key word
  //    (corrupt_word_addr, proven above to trigger Load Integrity NMI on read)
  //    silently launders a valid 39-bit SECDED ECC syndrome over the 3
  //    corrupted bytes [31:8]!
  // ---------------------------------------------------------------------------
  uint32_t nmi_before_sb = g_nmi_load_integ_count;
  g_load_store_fault = false;
  abs_mmio_write8(corrupt_word_addr, 0xccu);
  CHECK(!g_load_store_fault && g_nmi_load_integ_count == nmi_before_sb,
        "[sram_ctrl.sv:660-670, tlul_sram_byte.sv:667-694] Expected sb to "
        "corrupted SRAM word to complete without NMI or bus fault");

  uint32_t laundered_word = abs_mmio_read32(corrupt_word_addr);
  CHECK(g_nmi_load_integ_count == nmi_before_sb,
        "[sram_ctrl.sv:660-670, tlul_sram_byte.sv:667-694] Expected 32-bit "
        "read of word after 1-byte sb to NOT trigger Load Integrity NMI "
        "(valid ECC syndrome laundered over 3 stale bytes), laundered=0x%08x",
        laundered_word);
  CHECK((laundered_word & 0xffu) == 0xccu,
        "Expected byte 0 of laundered word to equal 0xcc, got 0x%08x",
        laundered_word);

  // Restore clean ECC across retention SRAM via CTRL.INIT = 1:
  sram_init_mem(kSramRetRegsBase);
  LOG_INFO(
      "Test 4 (Sub-word sb RMW ECC laundering on EccCorrection=0: "
      "laundered_word=0x%08x) PASSED",
      laundered_word);

  // ---------------------------------------------------------------------------
  // 5. NEW_IN_V2 [top_earlgrey.h:925-930, earlgrey_pd_main.sv:2678-2722,
  //    tl_main_pkg.sv:40, cheriot_access_check.sv:61-73]:
  //    top_earlgrey.h advertises TOP_EARLGREY_SRAM_CTRL_META_RAM_BASE_ADDR =
  //    0x11000000u (size 0x9800 = 38 KiB) alongside
  //    TOP_EARLGREY_SRAM_CTRL_META_REGS_BASE_ADDR = 0x411A0000u. Although
  //    sram_ctrl_meta CSRs at 0x411A0000u accept CTRL.RENEW_SCR_KEY = 1 and
  //    CTRL.INIT = 1 and report STATUS.INIT_DONE = 1 and SCR_KEY_VALID = 1,
  //    u_sram_ctrl_meta.ram_tl_i is NOT connected to xbar_main (instead
  //    0x11000000u on xbar_main routes to u_cheriot.revbm_tl_d_i, which
  //    rejects CPU reads/writes with d_error = 1 / mcause = 5 & 7).
  // ---------------------------------------------------------------------------
  sram_renew_key(kSramMetaRegsBase);
  sram_init_mem(kSramMetaRegsBase);
  uint32_t meta_status =
      abs_mmio_read32(kSramMetaRegsBase + SRAM_CTRL_STATUS_REG_OFFSET);
  CHECK(
      bitfield_bit32_read(meta_status, SRAM_CTRL_STATUS_INIT_DONE_BIT) &&
          bitfield_bit32_read(meta_status, SRAM_CTRL_STATUS_SCR_KEY_VALID_BIT),
      "Expected sram_ctrl_meta STATUS to report INIT_DONE=1 and "
      "SCR_KEY_VALID=1 (0x18), got 0x%x",
      meta_status);

  g_load_store_fault = false;
  g_last_mcause = 0;
  abs_mmio_write32(kSramMetaRamBase, 0x12345678u);
  CHECK(g_load_store_fault && g_last_mcause == 7u,
        "[top_earlgrey.h:925-930, earlgrey_pd_main.sv:2720] Expected 32-bit "
        "store to TOP_EARLGREY_SRAM_CTRL_META_RAM_BASE_ADDR (0x11000000) to "
        "fault with mcause=7 even after sram_ctrl_meta INIT_DONE=1");

  g_load_store_fault = false;
  g_last_mcause = 0;
  (void)abs_mmio_read32(kSramMetaRamBase);
  CHECK(g_load_store_fault && g_last_mcause == 5u,
        "[top_earlgrey.h:925-930, earlgrey_pd_main.sv:2720] Expected 32-bit "
        "load from TOP_EARLGREY_SRAM_CTRL_META_RAM_BASE_ADDR (0x11000000) to "
        "fault with mcause=5 even after sram_ctrl_meta INIT_DONE=1");
  LOG_INFO(
      "Test 5 (sram_ctrl_meta 0x411A0000 INIT_DONE=1 vs unmapped/blocked "
      "0x11000000 RAM window) PASSED");

  LOG_INFO("=== ALL SRAM_CTRL ERRATA-V2 CHECKS PASSED ===");
  return true;
}
