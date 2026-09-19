// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/dif/dif_pinmux.h"
#include "sw/device/lib/dif/dif_rstmgr.h"
#include "sw/device/lib/dif/dif_usbdev.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "usbdev_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kUsbdevBase = TOP_EARLGREY_USBDEV_BASE_ADDR,
  kRstmgrBase = TOP_EARLGREY_RSTMGR_AON_BASE_ADDR,
  kPinmuxBase = TOP_EARLGREY_PINMUX_AON_BASE_ADDR,
};

#define EXPECT_RTL(cond, ...)             \
  do {                                    \
    if (!(cond)) {                        \
      LOG_ERROR("RTL_MISMATCH: " __VA_ARGS__); \
      all_ok = false;                     \
    }                                     \
  } while (0)

static volatile bool g_fault_seen = false;
static volatile uint32_t g_fault_mcause = 0u;

void ottf_exception_handler(uint32_t *exc_info) {
  (void)exc_info;
  g_fault_seen = true;
  g_fault_mcause = ibex_mcause_read();
}

bool test_main(void) {
  bool all_ok = true;
  dif_rstmgr_t rstmgr;
  CHECK_DIF_OK(
      dif_rstmgr_init(mmio_region_from_addr(kRstmgrBase), &rstmgr));
  dif_pinmux_t pinmux;
  CHECK_DIF_OK(
      dif_pinmux_init(mmio_region_from_addr(kPinmuxBase), &pinmux));

  // Tie USBDEV sense input to ConstantZero via PINMUX and issue a peripheral
  // software reset to USBDEV to ensure clean post-reset CSR state.
  CHECK_DIF_OK(dif_pinmux_input_select(
      &pinmux, kTopEarlgreyPinmuxPeripheralInUsbdevSense,
      kTopEarlgreyPinmuxInselConstantZero));
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

  // 8. Wave 2: Hold D+ pullup via PHY_PINS_DRIVE (EN=1, DP_PULLUP_EN_O=1) so
  // the unattached USB pads are in J state (see_se0=0, link_reset=0).
  // Then PHY_CONFIG.tx_osc_test_mode (bit 5) drives usb_oe_o = 1
  // (reflected in PHY_PINS_SENSE.tx_oe_o, bit 12) in usb_fs_tx.sv (`OscTest`).
  abs_mmio_write32(kUsbdevBase + USBDEV_PHY_PINS_DRIVE_REG_OFFSET,
                   (1u << USBDEV_PHY_PINS_DRIVE_EN_BIT) |
                       (1u << USBDEV_PHY_PINS_DRIVE_DP_PULLUP_EN_O_BIT));
  busy_spin_micros(5);
  abs_mmio_write32(kUsbdevBase + USBDEV_PHY_CONFIG_REG_OFFSET,
                   (1u << USBDEV_PHY_CONFIG_EOP_SINGLE_BIT_BIT) |
                       (1u << USBDEV_PHY_CONFIG_TX_OSC_TEST_MODE_BIT));
  busy_spin_micros(5);
  uint32_t pins_sense =
      abs_mmio_read32(kUsbdevBase + USBDEV_PHY_PINS_SENSE_REG_OFFSET);
  EXPECT_RTL(
      bitfield_bit32_read(pins_sense, USBDEV_PHY_PINS_SENSE_TX_OE_O_BIT) ==
          true,
      "PHY_PINS_SENSE.tx_oe_o must be 1 when PHY_CONFIG.tx_osc_test_mode == 1 "
      "(got 0x%08x)",
      pins_sense);
  abs_mmio_write32(kUsbdevBase + USBDEV_PHY_CONFIG_REG_OFFSET,
                   1u << USBDEV_PHY_CONFIG_EOP_SINGLE_BIT_BIT);
  busy_spin_micros(5);

  // 9. Wave 2: PINMUX MioInUsbdevSense (`kTopEarlgreyPinmuxPeripheralInUsbdevSense`)
  // drives `cio_usbdev_sense_p2d` (`USBSTAT.sense`, `PHY_PINS_SENSE.pwr_sense`),
  // sets `INTR_STATE.powered`, transitions `USBSTAT.link_state` to `Powered` (1)
  // when `USBCTRL.enable == 1`, and `USBCTRL.resume_link_active` transitions
  // `USBSTAT.link_state` to `ActiveNoSOF` (5) (`usbdev_linkstate.sv:140-163`).
  // Furthermore, when VBUS disconnects while `USBCTRL.enable == 1`,
  // `USBCTRL.device_address` and `AVSETUPBUFFER`/`AVOUTBUFFER` FIFOs are NOT
  // cleared (`usbdev_usbif.sv:155`, `usbdev.sv:257-325`).
  abs_mmio_write32(kUsbdevBase + USBDEV_INTR_STATE_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kUsbdevBase + USBDEV_AVSETUPBUFFER_REG_OFFSET, 3u);
  abs_mmio_write32(kUsbdevBase + USBDEV_AVOUTBUFFER_REG_OFFSET, 7u);
  CHECK_DIF_OK(dif_pinmux_input_select(
      &pinmux, kTopEarlgreyPinmuxPeripheralInUsbdevSense,
      kTopEarlgreyPinmuxInselConstantOne));
  busy_spin_micros(2);

  usbstat = abs_mmio_read32(kUsbdevBase + USBDEV_USBSTAT_REG_OFFSET);
  pins_sense = abs_mmio_read32(kUsbdevBase + USBDEV_PHY_PINS_SENSE_REG_OFFSET);
  intr_state = abs_mmio_read32(kUsbdevBase + USBDEV_INTR_STATE_REG_OFFSET);
  EXPECT_RTL(bitfield_bit32_read(usbstat, USBDEV_USBSTAT_SENSE_BIT) == true &&
                 bitfield_bit32_read(
                     pins_sense, USBDEV_PHY_PINS_SENSE_PWR_SENSE_BIT) == true &&
                 bitfield_bit32_read(
                     intr_state, USBDEV_INTR_COMMON_POWERED_BIT) == true,
             "PINMUX ConstantOne on UsbdevSense must set USBSTAT.sense=1, "
             "PHY_PINS_SENSE.pwr_sense=1, INTR_STATE.powered=1 "
             "(usbstat=0x%08x, pins_sense=0x%08x, intr_state=0x%08x)",
             usbstat, pins_sense, intr_state);

  // Enable USBCTRL with device_address = 0x2a -> LINK_STATE becomes Powered (1).
  uint32_t ctrl_val =
      bitfield_bit32_write(0u, USBDEV_USBCTRL_ENABLE_BIT, true);
  ctrl_val = bitfield_field32_write(
      ctrl_val, USBDEV_USBCTRL_DEVICE_ADDRESS_FIELD, 0x2au);
  abs_mmio_write32(kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET, ctrl_val);
  busy_spin_micros(2);

  usbstat = abs_mmio_read32(kUsbdevBase + USBDEV_USBSTAT_REG_OFFSET);
  uint32_t link_state =
      bitfield_field32_read(usbstat, USBDEV_USBSTAT_LINK_STATE_FIELD);
  EXPECT_RTL(link_state == USBDEV_USBSTAT_LINK_STATE_VALUE_POWERED,
             "USBSTAT.link_state expected Powered (1), got %u (usbstat=0x%08x)",
             link_state, usbstat);

  // Write RESUME_LINK_ACTIVE=1 while in LinkPowered -> transitions to
  // LinkActiveNoSOF (5) and sets INTR_STATE.link_resume.
  abs_mmio_write32(
      kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET,
      bitfield_bit32_write(ctrl_val, USBDEV_USBCTRL_RESUME_LINK_ACTIVE_BIT,
                           true));
  busy_spin_micros(2);

  usbstat = abs_mmio_read32(kUsbdevBase + USBDEV_USBSTAT_REG_OFFSET);
  link_state = bitfield_field32_read(usbstat, USBDEV_USBSTAT_LINK_STATE_FIELD);
  intr_state = abs_mmio_read32(kUsbdevBase + USBDEV_INTR_STATE_REG_OFFSET);
  EXPECT_RTL(link_state == USBDEV_USBSTAT_LINK_STATE_VALUE_ACTIVE_NOSOF &&
                 bitfield_bit32_read(
                     intr_state, USBDEV_INTR_COMMON_LINK_RESUME_BIT) == true,
             "RESUME_LINK_ACTIVE from Powered must transition LINK_STATE to "
             "ActiveNoSOF (5) and set INTR_STATE.link_resume "
             "(link_state=%u, intr_state=0x%08x)",
             link_state, intr_state);

  // Disconnect VBUS via PINMUX ConstantZero while USBCTRL.enable == 1:
  // LINK_STATE becomes Disconnected (0), INTR_STATE.disconnected=1,
  // but USBCTRL.device_address (0x2a) and AVSETUP/AVOUT FIFO depths (1, 1)
  // MUST be retained.
  CHECK_DIF_OK(dif_pinmux_input_select(
      &pinmux, kTopEarlgreyPinmuxPeripheralInUsbdevSense,
      kTopEarlgreyPinmuxInselConstantZero));
  busy_spin_micros(2);

  usbstat = abs_mmio_read32(kUsbdevBase + USBDEV_USBSTAT_REG_OFFSET);
  usbctrl = abs_mmio_read32(kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET);
  EXPECT_RTL(
      bitfield_field32_read(usbstat, USBDEV_USBSTAT_LINK_STATE_FIELD) ==
              USBDEV_USBSTAT_LINK_STATE_VALUE_DISCONNECTED &&
          bitfield_field32_read(usbctrl, USBDEV_USBCTRL_DEVICE_ADDRESS_FIELD) ==
              0x2au &&
          bitfield_field32_read(usbstat, USBDEV_USBSTAT_AV_SETUP_DEPTH_FIELD) ==
              1u &&
          bitfield_field32_read(usbstat, USBDEV_USBSTAT_AV_OUT_DEPTH_FIELD) ==
              1u,
      "VBUS loss with USBCTRL.enable=1 must not clear DEVICE_ADDRESS or "
      "AVSETUP/AVOUT FIFOs (usbstat=0x%08x, usbctrl=0x%08x)",
      usbstat, usbctrl);

  // 10. Wave 5: USBDEV_PERMIT sub-word wr_err, addrmiss (0xac), and buffer SRAM ByteAccess=0.
  abs_mmio_write32(kUsbdevBase + USBDEV_INTR_ENABLE_REG_OFFSET, 0u);
  g_fault_seen = false;
  abs_mmio_write8(kUsbdevBase + USBDEV_INTR_ENABLE_REG_OFFSET, 0x5u);
  EXPECT_RTL(g_fault_seen && g_fault_mcause == kIbexExcStoreAccessFault,
             "sb to INTR_ENABLE (PERMIT=0x7) must fault with MCAUSE=7");
  EXPECT_RTL(abs_mmio_read32(kUsbdevBase + USBDEV_INTR_ENABLE_REG_OFFSET) == 0u,
             "sb to INTR_ENABLE must not modify register");

  g_fault_seen = false;
  (void)abs_mmio_read32(kUsbdevBase + 0xacu);
  EXPECT_RTL(g_fault_seen && g_fault_mcause == kIbexExcLoadAccessFault,
             "addrmiss read at 0xac must fault with MCAUSE=5");

  g_fault_seen = false;
  abs_mmio_write32(kUsbdevBase + 0xacu, 0x1u);
  EXPECT_RTL(g_fault_seen && g_fault_mcause == kIbexExcStoreAccessFault,
             "addrmiss write at 0xac must fault with MCAUSE=7");

  abs_mmio_write32(kUsbdevBase + USBDEV_BUFFER_REG_OFFSET, 0x11223344u);
  g_fault_seen = false;
  abs_mmio_write8(kUsbdevBase + USBDEV_BUFFER_REG_OFFSET, 0xffu);
  EXPECT_RTL(g_fault_seen && g_fault_mcause == kIbexExcStoreAccessFault,
             "sb to USBDEV_BUFFER (ByteAccess=0) must fault with MCAUSE=7");
  EXPECT_RTL(abs_mmio_read32(kUsbdevBase + USBDEV_BUFFER_REG_OFFSET) == 0x11223344u,
             "sb to USBDEV_BUFFER must not modify word");

  return all_ok;
}
