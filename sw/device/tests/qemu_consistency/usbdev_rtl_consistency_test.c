// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/dif/dif_rstmgr.h"
#include "sw/device/lib/dif/dif_usbdev.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "usbdev_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kUsbdevBase = TOP_EARLGREY_USBDEV_BASE_ADDR,
  kRstmgrBase = TOP_EARLGREY_RSTMGR_AON_BASE_ADDR,
};

#define EXPECT_RTL(cond, ...)             \
  do {                                    \
    if (!(cond)) {                        \
      LOG_ERROR("RTL_MISMATCH: " __VA_ARGS__); \
      all_ok = false;                     \
    }                                     \
  } while (0)

bool test_main(void) {
  bool all_ok = true;
  dif_rstmgr_t rstmgr;
  CHECK_DIF_OK(
      dif_rstmgr_init(mmio_region_from_addr(kRstmgrBase), &rstmgr));

  // Issue a peripheral software reset to USBDEV to ensure clean post-reset CSR
  // state regardless of any ROM/ROM_EXT initialization.
  CHECK_DIF_OK(dif_rstmgr_software_reset(&rstmgr,
                                         kTopEarlgreyResetManagerSwResetsUsb,
                                         kDifRstmgrSoftwareReset));

  // 1. PHY_CONFIG reset value & valid field mask (0xe7).
  // In RTL (usbdev.hjson), eop_single_bit (bit 2) has resval = 1 (0x4).
  uint32_t phy_cfg = abs_mmio_read32(kUsbdevBase + USBDEV_PHY_CONFIG_REG_OFFSET);
  EXPECT_RTL((phy_cfg & (1u << USBDEV_PHY_CONFIG_EOP_SINGLE_BIT_BIT)) != 0,
             "PHY_CONFIG.eop_single_bit reset value expected 1, got 0x%08x",
             phy_cfg);
  abs_mmio_write32(kUsbdevBase + USBDEV_PHY_CONFIG_REG_OFFSET, 0xffffffffu);
  phy_cfg = abs_mmio_read32(kUsbdevBase + USBDEV_PHY_CONFIG_REG_OFFSET);
  EXPECT_RTL(
      phy_cfg == 0xe7u,
      "PHY_CONFIG reserved bits should read as 0 (expected 0xe7, got 0x%08x)",
      phy_cfg);
  abs_mmio_write32(kUsbdevBase + USBDEV_PHY_CONFIG_REG_OFFSET,
                   1u << USBDEV_PHY_CONFIG_EOP_SINGLE_BIT_BIT);

  // 2. USBSTAT.rx_empty is gated by connect_en (USBCTRL.enable) in RTL:
  // `assign hw2reg.usbstat.rx_empty.d = connect_en & ~rx_fifo_rvalid;`
  uint32_t usbstat = abs_mmio_read32(kUsbdevBase + USBDEV_USBSTAT_REG_OFFSET);
  EXPECT_RTL(bitfield_bit32_read(usbstat, USBDEV_USBSTAT_RX_EMPTY_BIT) == false,
             "USBSTAT.rx_empty should be 0 when USBCTRL.enable == 0 (got "
             "0x%08x)",
             usbstat);

  // 3. USBCTRL.device_address is cleared by hardware (`clr_devaddr_o =
  // ~connect_en_i | link_reset`) whenever USBCTRL.enable == 0.
  abs_mmio_write32(
      kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET,
      bitfield_field32_write(0, USBDEV_USBCTRL_DEVICE_ADDRESS_FIELD, 0x2au));
  uint32_t usbctrl = abs_mmio_read32(kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET);
  EXPECT_RTL(
      bitfield_field32_read(usbctrl, USBDEV_USBCTRL_DEVICE_ADDRESS_FIELD) == 0u,
      "USBCTRL.device_address must be cleared to 0 when USBCTRL.enable == 0 "
      "(got 0x%08x)",
      usbctrl);

  // 4. Write-only registers (AVOUTBUFFER, AVSETUPBUFFER, FIFO_CTRL) must read
  // back as 0, and overflowing AVSETUPBUFFER (depth 4) / AVOUTBUFFER (depth 8)
  // must assert INTR_STATE.av_overflow (bit 9).
  abs_mmio_write32(kUsbdevBase + USBDEV_INTR_STATE_REG_OFFSET, 0xffffffffu);
  for (uint32_t i = 0; i < 4; ++i) {
    abs_mmio_write32(kUsbdevBase + USBDEV_AVSETUPBUFFER_REG_OFFSET, i + 1);
  }
  EXPECT_RTL(abs_mmio_read32(kUsbdevBase + USBDEV_AVSETUPBUFFER_REG_OFFSET) ==
                 0u,
             "AVSETUPBUFFER is WO and must read back as 0");
  uint32_t intr_state =
      abs_mmio_read32(kUsbdevBase + USBDEV_INTR_STATE_REG_OFFSET);
  EXPECT_RTL(
      bitfield_bit32_read(intr_state, USBDEV_INTR_COMMON_AV_OVERFLOW_BIT) ==
          false,
      "AV_OVERFLOW should not be set at depth 4 (intr_state=0x%08x)",
      intr_state);

  // 5th push to AVSETUPBUFFER overflows the 4-entry FIFO.
  abs_mmio_write32(kUsbdevBase + USBDEV_AVSETUPBUFFER_REG_OFFSET, 5u);
  intr_state = abs_mmio_read32(kUsbdevBase + USBDEV_INTR_STATE_REG_OFFSET);
  EXPECT_RTL(
      bitfield_bit32_read(intr_state, USBDEV_INTR_COMMON_AV_OVERFLOW_BIT) ==
          true,
      "AV_OVERFLOW must be set after pushing to full AVSETUPBUFFER "
      "(intr_state=0x%08x)",
      intr_state);

  // Clear AV_OVERFLOW and reset FIFOs via FIFO_CTRL.
  abs_mmio_write32(kUsbdevBase + USBDEV_INTR_STATE_REG_OFFSET,
                   1u << USBDEV_INTR_COMMON_AV_OVERFLOW_BIT);
  abs_mmio_write32(kUsbdevBase + USBDEV_FIFO_CTRL_REG_OFFSET, 0x7u);
  EXPECT_RTL(abs_mmio_read32(kUsbdevBase + USBDEV_FIFO_CTRL_REG_OFFSET) == 0u,
             "FIFO_CTRL is WO and must read back as 0");

  // 5. WAKE_EVENTS is RO (`swaccess: "ro"`): software writes must be ignored.
  uint32_t wake_events_before =
      abs_mmio_read32(kUsbdevBase + USBDEV_WAKE_EVENTS_REG_OFFSET);
  abs_mmio_write32(kUsbdevBase + USBDEV_WAKE_EVENTS_REG_OFFSET, 0x00000701u);
  uint32_t wake_events_after =
      abs_mmio_read32(kUsbdevBase + USBDEV_WAKE_EVENTS_REG_OFFSET);
  EXPECT_RTL(wake_events_after == wake_events_before,
             "WAKE_EVENTS is RO; write modified value from 0x%08x to 0x%08x",
             wake_events_before, wake_events_after);

  // 6. Event counters (COUNT_OUT, COUNT_IN, COUNT_NODATA_IN, COUNT_ERRORS):
  // - count[7:0] is RO (software writes do not overwrite count)
  // - rst[31] is WO (reads back as 0)
  // - reserved bits read back as 0
  abs_mmio_write32(kUsbdevBase + USBDEV_COUNT_OUT_REG_OFFSET, 0xffffffffu);
  uint32_t count_out =
      abs_mmio_read32(kUsbdevBase + USBDEV_COUNT_OUT_REG_OFFSET);
  EXPECT_RTL(count_out == 0x0ffff000u,
             "COUNT_OUT RO/WO/reserved field mask mismatch: expected "
             "0x0ffff000, got 0x%08x",
             count_out);

  abs_mmio_write32(kUsbdevBase + USBDEV_COUNT_IN_REG_OFFSET, 0xffffffffu);
  uint32_t count_in = abs_mmio_read32(kUsbdevBase + USBDEV_COUNT_IN_REG_OFFSET);
  EXPECT_RTL(count_in == 0x0fffe000u,
             "COUNT_IN RO/WO/reserved field mask mismatch: expected "
             "0x0fffe000, got 0x%08x",
             count_in);

  abs_mmio_write32(kUsbdevBase + USBDEV_COUNT_NODATA_IN_REG_OFFSET,
                   0xffffffffu);
  uint32_t count_nodata_in =
      abs_mmio_read32(kUsbdevBase + USBDEV_COUNT_NODATA_IN_REG_OFFSET);
  EXPECT_RTL(count_nodata_in == 0x0fff0000u,
             "COUNT_NODATA_IN RO/WO/reserved field mask mismatch: expected "
             "0x0fff0000, got 0x%08x",
             count_nodata_in);

  abs_mmio_write32(kUsbdevBase + USBDEV_COUNT_ERRORS_REG_OFFSET, 0xffffffffu);
  uint32_t count_errors =
      abs_mmio_read32(kUsbdevBase + USBDEV_COUNT_ERRORS_REG_OFFSET);
  EXPECT_RTL(count_errors == 0x78000000u,
             "COUNT_ERRORS RO/WO/reserved field mask mismatch: expected "
             "0x78000000, got 0x%08x",
             count_errors);

  // 7. CONFIGIN_0 and SET_NAK_OUT reserved bit masking.
  abs_mmio_write32(kUsbdevBase + USBDEV_CONFIGIN_0_REG_OFFSET, 0x1fffffffu);
  uint32_t configin0 =
      abs_mmio_read32(kUsbdevBase + USBDEV_CONFIGIN_0_REG_OFFSET);
  EXPECT_RTL(
      configin0 == 0x00007f1fu,
      "CONFIGIN_0 reserved bits should read as 0 (expected 0x00007f1f, got "
      "0x%08x)",
      configin0);

  abs_mmio_write32(kUsbdevBase + USBDEV_SET_NAK_OUT_REG_OFFSET, 0xffffffffu);
  uint32_t set_nak_out =
      abs_mmio_read32(kUsbdevBase + USBDEV_SET_NAK_OUT_REG_OFFSET);
  EXPECT_RTL(
      set_nak_out == 0x00000fffu,
      "SET_NAK_OUT reserved bits should read as 0 (expected 0xfff, got "
      "0x%08x)",
      set_nak_out);

  return all_ok;
}
