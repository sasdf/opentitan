// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/dif/dif_flash_ctrl.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "flash_ctrl_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kFlashCoreBase = TOP_EARLGREY_FLASH_CTRL_CORE_BASE_ADDR,
  kFlashPrimBase = TOP_EARLGREY_FLASH_CTRL_PRIM_BASE_ADDR,
};

#define EXPECT_RTL(cond, ...)                   \
  do {                                          \
    if (!(cond)) {                              \
      LOG_ERROR("RTL_MISMATCH: " __VA_ARGS__);  \
      all_ok = false;                           \
    }                                           \
  } while (0)

bool test_main(void) {
  bool all_ok = true;

  // 1. WO registers (ALERT_TEST, PROG_FIFO) must read back as 0.
  EXPECT_RTL(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_ALERT_TEST_REG_OFFSET) == 0u,
             "ALERT_TEST is WO and must read back as 0");
  EXPECT_RTL(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_PROG_FIFO_REG_OFFSET) == 0u,
             "PROG_FIFO is WO and must read back as 0");

  // 2. RO registers (STATUS, PHY_STATUS, CTRL_REGWEN) ignore software writes.
  uint32_t status_before =
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_STATUS_REG_OFFSET);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_STATUS_REG_OFFSET, 0xffffffffu);
  uint32_t status_after =
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_STATUS_REG_OFFSET);
  EXPECT_RTL(status_after == status_before,
             "STATUS is RO; before=0x%08x, after=0x%08x", status_before,
             status_after);

  uint32_t phy_status =
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_PHY_STATUS_REG_OFFSET);
  EXPECT_RTL((phy_status & 0x6u) == 0x6u,
             "PHY_STATUS.prog_normal_avail and prog_repair_avail must be 1 (0x%08x)",
             phy_status);

  uint32_t ctrl_regwen =
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_CTRL_REGWEN_REG_OFFSET);
  EXPECT_RTL(ctrl_regwen == 1u,
             "CTRL_REGWEN should be 1 when idle (got 0x%08x)", ctrl_regwen);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_CTRL_REGWEN_REG_OFFSET, 0u);
  EXPECT_RTL(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_CTRL_REGWEN_REG_OFFSET) == 1u,
             "CTRL_REGWEN is RO (HW-managed) and must ignore software writes");

  // 3. SCRATCH (0x1a0) full 32-bit RW and FIFO_LVL (0x1a4) reserved bit masking (0x1f1f).
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_SCRATCH_REG_OFFSET, 0xdeadbeefu);
  EXPECT_RTL(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_SCRATCH_REG_OFFSET) ==
                 0xdeadbeefu,
             "SCRATCH readback mismatch");

  uint32_t fifo_lvl_orig =
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_FIFO_LVL_REG_OFFSET);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_FIFO_LVL_REG_OFFSET, 0xffffffffu);
  EXPECT_RTL(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_FIFO_LVL_REG_OFFSET) ==
                 0x00001f1fu,
             "FIFO_LVL reserved bits must read as 0 (expected 0x1f1f)");
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_FIFO_LVL_REG_OFFSET,
                   fifo_lvl_orig);

  // 4. Region configuration & REGION_CFG_REGWEN_7 W0C lock test (using region 7).
  uint32_t regwen7 =
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_REGION_CFG_REGWEN_7_REG_OFFSET);
  if (regwen7 == 1u) {
    uint32_t test_cfg =
        bitfield_field32_write(0u, FLASH_CTRL_MP_REGION_CFG_7_EN_7_FIELD,
                               kMultiBitBool4False) |
        bitfield_field32_write(0u, FLASH_CTRL_MP_REGION_CFG_7_RD_EN_7_FIELD,
                               kMultiBitBool4True) |
        bitfield_field32_write(0u, FLASH_CTRL_MP_REGION_CFG_7_PROG_EN_7_FIELD,
                               kMultiBitBool4False) |
        bitfield_field32_write(0u, FLASH_CTRL_MP_REGION_CFG_7_ERASE_EN_7_FIELD,
                               kMultiBitBool4False) |
        bitfield_field32_write(
            0u, FLASH_CTRL_MP_REGION_CFG_7_SCRAMBLE_EN_7_FIELD,
            kMultiBitBool4False) |
        bitfield_field32_write(0u, FLASH_CTRL_MP_REGION_CFG_7_ECC_EN_7_FIELD,
                               kMultiBitBool4False) |
        bitfield_field32_write(0u, FLASH_CTRL_MP_REGION_CFG_7_HE_EN_7_FIELD,
                               kMultiBitBool4False);
    abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_MP_REGION_CFG_7_REG_OFFSET,
                     0xf0000000u | test_cfg);
    EXPECT_RTL(abs_mmio_read32(kFlashCoreBase +
                               FLASH_CTRL_MP_REGION_CFG_7_REG_OFFSET) == test_cfg,
               "MP_REGION_CFG_7 upper bits [31:28] must be masked to 0");

    // Lock REGION_CFG_REGWEN_7 via W0C and verify it cannot be re-enabled.
    abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_REGION_CFG_REGWEN_7_REG_OFFSET,
                     0u);
    abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_REGION_CFG_REGWEN_7_REG_OFFSET,
                     1u);
    EXPECT_RTL(abs_mmio_read32(kFlashCoreBase +
                               FLASH_CTRL_REGION_CFG_REGWEN_7_REG_OFFSET) == 0u,
               "REGION_CFG_REGWEN_7 is W0C and must remain 0 once cleared");

    // Attempt to modify MP_REGION_CFG_7 while locked -> must be ignored.
    abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_MP_REGION_CFG_7_REG_OFFSET,
                     0x9999999u);
    EXPECT_RTL(abs_mmio_read32(kFlashCoreBase +
                               FLASH_CTRL_MP_REGION_CFG_7_REG_OFFSET) == test_cfg,
               "MP_REGION_CFG_7 write must be ignored when REGION_CFG_REGWEN_7 == 0");
  }

  // 5. Invalid CONTROL.OP (0x3) sets OP_STATUS.ERR, OP_STATUS.DONE, ERR_CODE.OP_ERR, and pulses recov_err alert.
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_OP_STATUS_REG_OFFSET, 0u);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ERR_CODE_REG_OFFSET, 0xffu);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_INTR_STATE_REG_OFFSET,
                   (1u << FLASH_CTRL_INTR_STATE_OP_DONE_BIT) |
                       (1u << FLASH_CTRL_INTR_STATE_CORR_ERR_BIT));

  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdFlashCtrlRecovErr));

  uint32_t bad_ctrl =
      bitfield_field32_write(0u, FLASH_CTRL_CONTROL_OP_FIELD, 0x3u) |
      bitfield_bit32_write(0u, FLASH_CTRL_CONTROL_START_BIT, true);
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_CONTROL_REG_OFFSET, bad_ctrl);

  uint32_t op_status =
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_OP_STATUS_REG_OFFSET);
  uint32_t err_code =
      abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_ERR_CODE_REG_OFFSET);
  EXPECT_RTL(op_status == ((1u << FLASH_CTRL_OP_STATUS_DONE_BIT) |
                           (1u << FLASH_CTRL_OP_STATUS_ERR_BIT)),
             "Invalid CONTROL.OP (0x3) must set OP_STATUS.DONE | OP_STATUS.ERR (got 0x%08x)",
             op_status);
  EXPECT_RTL((err_code & (1u << FLASH_CTRL_ERR_CODE_OP_ERR_BIT)) != 0u,
             "Invalid CONTROL.OP (0x3) must set ERR_CODE.OP_ERR (got 0x%08x)",
             err_code);

  // Clear ERR_CODE (W1C) and OP_STATUS (RW), and verify alert was caught.
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_ERR_CODE_REG_OFFSET, err_code);
  EXPECT_RTL(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_ERR_CODE_REG_OFFSET) == 0u,
             "ERR_CODE must clear to 0 on W1C write");
  abs_mmio_write32(kFlashCoreBase + FLASH_CTRL_OP_STATUS_REG_OFFSET, 0u);
  EXPECT_RTL(abs_mmio_read32(kFlashCoreBase + FLASH_CTRL_OP_STATUS_REG_OFFSET) == 0u,
             "OP_STATUS must clear to 0 on RW write");

  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdFlashCtrlRecovErr));

  return all_ok;
}
