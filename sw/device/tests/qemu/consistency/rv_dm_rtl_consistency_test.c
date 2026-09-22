// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/dif/dif_lc_ctrl.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "rv_dm_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kRvDmRegsBase = TOP_EARLGREY_RV_DM_REGS_BASE_ADDR,
  kRvDmMemBase = TOP_EARLGREY_RV_DM_MEM_BASE_ADDR,
  kLcCtrlBase = TOP_EARLGREY_LC_CTRL_BASE_ADDR,
};

static volatile bool load_fault_seen;
static volatile bool store_fault_seen;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = ibex_mcause_read();
  if (mcause == kIbexExcLoadAccessFault) {
    load_fault_seen = true;
  } else if (mcause == kIbexExcStoreAccessFault) {
    store_fault_seen = true;
  } else {
    CHECK(false, "Unexpected exception mcause=0x%x", mcause);
  }
}

static bool try_read32(uint32_t addr, uint32_t *out_val) {
  load_fault_seen = false;
  uint32_t val = abs_mmio_read32(addr);
  if (load_fault_seen) {
    return false;
  }
  if (out_val != NULL) {
    *out_val = val;
  }
  return true;
}

static bool try_write32(uint32_t addr, uint32_t val) {
  store_fault_seen = false;
  abs_mmio_write32(addr, val);
  return !store_fault_seen;
}

static bool try_write16(uint32_t addr, uint16_t val) {
  store_fault_seen = false;
  *(volatile uint16_t *)addr = val;
  return !store_fault_seen;
}

static bool try_write8(uint32_t addr, uint8_t val) {
  store_fault_seen = false;
  abs_mmio_write8(addr, val);
  return !store_fault_seen;
}

bool test_main(void) {
  // 1. Verify ALERT_TEST (0x00) is WO (reads back 0) and generates a transient
  // alert pulse on every write of 1 (testing two consecutive pulses).
  CHECK(abs_mmio_read32(kRvDmRegsBase + RV_DM_ALERT_TEST_REG_OFFSET) == 0u);

  for (int i = 0; i < 2; ++i) {
    CHECK_STATUS_OK(
        ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdRvDmFatalFault));
    abs_mmio_write32(kRvDmRegsBase + RV_DM_ALERT_TEST_REG_OFFSET,
                     1u << RV_DM_ALERT_TEST_FATAL_FAULT_BIT);
    CHECK(abs_mmio_read32(kRvDmRegsBase + RV_DM_ALERT_TEST_REG_OFFSET) == 0u);
    CHECK_STATUS_OK(
        ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdRvDmFatalFault));
    busy_spin_micros(10);
  }

  // 2. Determine life cycle state to check RV_DM.MEM gating and memory map.
  dif_lc_ctrl_t lc_ctrl;
  CHECK_DIF_OK(dif_lc_ctrl_init(mmio_region_from_addr(kLcCtrlBase), &lc_ctrl));
  dif_lc_ctrl_state_t lc_state;
  CHECK_DIF_OK(dif_lc_ctrl_get_state(&lc_ctrl, &lc_state));

  bool lc_hw_debug_en =
      (lc_state == kDifLcCtrlStateTestUnlocked0 ||
       lc_state == kDifLcCtrlStateTestUnlocked1 ||
       lc_state == kDifLcCtrlStateTestUnlocked2 ||
       lc_state == kDifLcCtrlStateTestUnlocked3 ||
       lc_state == kDifLcCtrlStateTestUnlocked4 ||
       lc_state == kDifLcCtrlStateTestUnlocked5 ||
       lc_state == kDifLcCtrlStateTestUnlocked6 ||
       lc_state == kDifLcCtrlStateTestUnlocked7 ||
       lc_state == kDifLcCtrlStateDev || lc_state == kDifLcCtrlStateRma);

  // 3. Verify LATE_DEBUG_ENABLE_REGWEN (0x04, rw0c) and LATE_DEBUG_ENABLE
  // (0x08), as well as RV_DM.MEM (0x00010000) gating and dm_mem.sv /
  // debug_rom.sv semantics.
  uint32_t regwen = abs_mmio_read32(kRvDmRegsBase +
                                    RV_DM_LATE_DEBUG_ENABLE_REGWEN_REG_OFFSET);
  CHECK((regwen & ~1u) == 0u);

  // 2b. Verify unmapped offset 0x0c in RV_DM.REGS (RegsAw=4, 16-byte aperture)
  // returns addrmiss TL-UL bus error (Load/Store Access Fault).
  CHECK(!try_read32(kRvDmRegsBase + 0x0cu, NULL));
  CHECK(!try_write32(kRvDmRegsBase + 0x0cu, 0x12345678u));

  if (regwen == 1u) {
    uint32_t orig_val =
        abs_mmio_read32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET);
    CHECK(orig_val == kMultiBitBool32False || orig_val == kMultiBitBool32True);

    // Sub-word (8-bit and 16-bit) writes to LATE_DEBUG_ENABLE
    // (RV_DM_REGS_PERMIT[2] = 4'b1111) must assert wr_err (Store Access Fault)
    // and preserve LATE_DEBUG_ENABLE.
    store_fault_seen = false;
    abs_mmio_write8(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET, 0x12u);
    CHECK(store_fault_seen);
    store_fault_seen = false;
    *(volatile uint16_t *)(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET) =
        0x1234u;
    CHECK(store_fault_seen);
    CHECK(abs_mmio_read32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET) ==
          orig_val);

    // Sub-word 8-bit write to LATE_DEBUG_ENABLE_REGWEN (RV_DM_REGS_PERMIT[1] =
    // 4'b0001) must succeed without bus error.
    store_fault_seen = false;
    abs_mmio_write8(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REGWEN_REG_OFFSET,
                    0xffu);
    CHECK(!store_fault_seen);

    // Writing 1 to rw0c LATE_DEBUG_ENABLE_REGWEN must keep it 1, and upper bits
    // [31:1] must be masked to 0.
    abs_mmio_write32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REGWEN_REG_OFFSET,
                     0xffffffffu);
    CHECK(abs_mmio_read32(kRvDmRegsBase +
                          RV_DM_LATE_DEBUG_ENABLE_REGWEN_REG_OFFSET) == 1u);

    // Enable LATE_DEBUG_ENABLE (0x96969696) so lc_hw_debug_en_gated selects
    // lc_hw_debug_en (rv_dm.sv:238-243).
    abs_mmio_write32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET,
                     kMultiBitBool32True);
    CHECK(abs_mmio_read32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET) ==
          kMultiBitBool32True);

    if (lc_hw_debug_en) {
      uint32_t val = 0;

      // 3a. Verify Debug ROM (0x10800..0x1089c) matches debug_rom.sv:
      // HaltAddress (0x800) = j 0x818 (0x0180006f), 0x804 = nop (0x00000013)
      // ResumeAddress (0x808) = j 0x88c (0x0840006f), 0x80c = nop (0x00000013)
      // ExceptionAddress (0x810) = j 0x860 (0x0500006f), 0x814 = nop
      // (0x00000013)
      CHECK(try_read32(kRvDmMemBase + 0x800u, &val) && val == 0x0180006fu);
      CHECK(try_read32(kRvDmMemBase + 0x804u, &val) && val == 0x00000013u);
      CHECK(try_read32(kRvDmMemBase + 0x808u, &val) && val == 0x0840006fu);
      CHECK(try_read32(kRvDmMemBase + 0x80cu, &val) && val == 0x00000013u);
      CHECK(try_read32(kRvDmMemBase + 0x810u, &val) && val == 0x0500006fu);
      CHECK(try_read32(kRvDmMemBase + 0x814u, &val) && val == 0x00000013u);
      CHECK(try_read32(kRvDmMemBase + 0x818u, &val) && val == 0x0ff0000fu);
      CHECK(try_read32(kRvDmMemBase + 0x89cu, &val) && val == 0x7b200073u);
      CHECK(try_read32(kRvDmMemBase + 0x8a0u, &val) && val == 0x00000000u);

      // 3b. Writing to Debug ROM (0x800..0xfff) must return a TL-UL bus error
      // (err_d = we_i in dm_mem.sv:400) and leave ROM contents intact.
      CHECK(!try_write32(kRvDmMemBase + 0x800u, 0xdeadbeefu));
      CHECK(try_read32(kRvDmMemBase + 0x800u, &val) && val == 0x0180006fu);

      // 3c. Verify default Abstract Command ROM (0x338..0x35c,
      // dm_mem.sv:425-436) and verify CPU writes to it are ignored without bus
      // error.
      CHECK(try_read32(kRvDmMemBase + 0x338u, &val) && val == 0x00000000u);
      CHECK(try_read32(kRvDmMemBase + 0x33cu, &val) && val == 0x00000517u);
      CHECK(try_read32(kRvDmMemBase + 0x340u, &val) && val == 0x00c55513u);
      CHECK(try_read32(kRvDmMemBase + 0x344u, &val) && val == 0x00c51513u);
      CHECK(try_read32(kRvDmMemBase + 0x348u, &val) && val == 0x00000013u);
      CHECK(try_read32(kRvDmMemBase + 0x358u, &val) && val == 0x7b302573u);
      CHECK(try_read32(kRvDmMemBase + 0x35cu, &val) && val == 0x00100073u);
      CHECK(try_write32(kRvDmMemBase + 0x33cu, 0xdeadbeefu));
      CHECK(try_read32(kRvDmMemBase + 0x33cu, &val) && val == 0x00000517u);

      // 3d. Verify WhereTo (0x300), ProgBuf (0x360..0x37c), and Flags
      // (0x400..0x7fc) read 0 and ignore CPU writes without bus error.
      CHECK(try_read32(kRvDmMemBase + 0x300u, &val) && val == 0x00000000u);
      CHECK(try_write32(kRvDmMemBase + 0x300u, 0x12345678u));
      CHECK(try_read32(kRvDmMemBase + 0x300u, &val) && val == 0x00000000u);

      CHECK(try_read32(kRvDmMemBase + 0x360u, &val) && val == 0x00000000u);
      CHECK(try_write32(kRvDmMemBase + 0x360u, 0x12345678u));
      CHECK(try_read32(kRvDmMemBase + 0x360u, &val) && val == 0x00000000u);

      CHECK(try_read32(kRvDmMemBase + 0x400u, &val) && val == 0x00000000u);
      CHECK(try_write32(kRvDmMemBase + 0x400u, 0x12345678u));
      CHECK(try_read32(kRvDmMemBase + 0x7fcu, &val) && val == 0x00000000u);
      CHECK(try_write32(kRvDmMemBase + 0x7fcu, 0x12345678u));
      CHECK(try_read32(kRvDmMemBase + 0x7fcu, &val) && val == 0x00000000u);

      // 3e. Verify Data0 (0x380) and Data1 (0x384) accept writes without bus
      // error, and read back 0 while dmcontrol.dmactive == 0
      // (dm_csrs.sv:627,647).
      CHECK(try_write32(kRvDmMemBase + 0x380u, 0xa5a55a5au));
      CHECK(try_write32(kRvDmMemBase + 0x384u, 0x12345678u));
      CHECK(try_read32(kRvDmMemBase + 0x380u, &val) && val == 0x00000000u);
      CHECK(try_read32(kRvDmMemBase + 0x384u, &val) && val == 0x00000000u);

      // 3f. Verify 8-byte aligned Action registers (0x100, 0x108, 0x110, 0x118)
      // read 0 without fault, while unmapped holes (0x000, 0x104, 0x304, 0x388)
      // return TL-UL bus errors (dm_mem.sv:390-402).
      CHECK(try_read32(kRvDmMemBase + 0x100u, &val) && val == 0x00000000u);
      CHECK(try_read32(kRvDmMemBase + 0x108u, &val) && val == 0x00000000u);
      CHECK(try_read32(kRvDmMemBase + 0x110u, &val) && val == 0x00000000u);
      CHECK(try_read32(kRvDmMemBase + 0x118u, &val) && val == 0x00000000u);

      CHECK(!try_read32(kRvDmMemBase + 0x000u, &val));
      CHECK(!try_read32(kRvDmMemBase + 0x104u, &val));
      CHECK(!try_read32(kRvDmMemBase + 0x304u, &val));
      CHECK(!try_read32(kRvDmMemBase + 0x388u, &val));

      // Wave 7: dm_mem.sv:374-402 gen_wr_err enforces FullRegMask (4'b1111)
      // on writes to WhereTo (0x300), AbstractCmd (0x338..0x35f),
      // ProgBuf (0x360..0x37f), Data0/1 (0x380..0x387), and Flags
      // (0x400..0x7ff), and HartSelMask/OneBitMask (4'b0001) on
      // Halted/Going/Resuming/Exception (0x100/0x108/0x110/0x118), while
      // sub-word reads succeed (we_i == 0).
      CHECK(!try_write8(kRvDmMemBase + 0x300u, 0x11u));
      CHECK(!try_write16(kRvDmMemBase + 0x338u, 0x2222u));
      CHECK(!try_write8(kRvDmMemBase + 0x360u, 0x33u));
      CHECK(!try_write8(kRvDmMemBase + 0x380u, 0x44u));
      CHECK(!try_write8(kRvDmMemBase + 0x400u, 0x55u));
      CHECK(!try_write8(kRvDmMemBase + 0x101u, 0x00u));

      // Sub-word 8-bit reads from AbstractCmd[2] (0x340 = 0x00c55513) and
      // Flags[0]+1 (0x401 = 0x00) must succeed without fault.
      load_fault_seen = false;
      uint8_t ac2_b1 = abs_mmio_read8(kRvDmMemBase + 0x340u + 1u);
      uint8_t flag_b1 = abs_mmio_read8(kRvDmMemBase + 0x401u);
      CHECK(!load_fault_seen && ac2_b1 == 0x55u && flag_b1 == 0x00u);
    }

    // Restore original LATE_DEBUG_ENABLE.
    abs_mmio_write32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET,
                     orig_val);
    CHECK(abs_mmio_read32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET) ==
          orig_val);

    // Lock LATE_DEBUG_ENABLE_REGWEN (rw0c) by writing 0 and verify it cannot be
    // re-enabled by writing 1.
    abs_mmio_write32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REGWEN_REG_OFFSET,
                     0u);
    abs_mmio_write32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REGWEN_REG_OFFSET,
                     1u);
    CHECK(abs_mmio_read32(kRvDmRegsBase +
                          RV_DM_LATE_DEBUG_ENABLE_REGWEN_REG_OFFSET) == 0u);
  }

  // Once LATE_DEBUG_ENABLE_REGWEN == 0, writes to LATE_DEBUG_ENABLE must be
  // ignored.
  uint32_t locked_val =
      abs_mmio_read32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET);
  abs_mmio_write32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET,
                   locked_val ^ 0xffffffffu);
  CHECK(abs_mmio_read32(kRvDmRegsBase + RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET) ==
        locked_val);

  return true;
}
