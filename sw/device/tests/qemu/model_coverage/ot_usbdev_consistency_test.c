// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_pinmux.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "usbdev_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kUsbdevBase = TOP_EARLGREY_USBDEV_BASE_ADDR,
  kPinmuxBase = TOP_EARLGREY_PINMUX_AON_BASE_ADDR,
};

bool test_main(void) {
  LOG_INFO("Starting ot_usbdev FPGA/QEMU consistency test");

  // Connect UsbdevSense pinmux input to ConstantOne so VBUS SENSE == 1.
  dif_pinmux_t pinmux;
  CHECK_DIF_OK(dif_pinmux_init(mmio_region_from_addr(kPinmuxBase), &pinmux));
  CHECK_DIF_OK(dif_pinmux_input_select(
      &pinmux, kTopEarlgreyPinmuxPeripheralInUsbdevSense,
      kTopEarlgreyPinmuxInselConstantOne));
  busy_spin_micros(20);

  // Ensure USBCTRL is initially disabled.
  abs_mmio_write32(kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET, 0x0u);

  // 1. PHY_PINS_DRIVE -> PHY_PINS_SENSE loopback (usbdev_iomux.sv + CW340
  // TUSB1106 transceiver wiring).
  const uint32_t kDriveEnOe = (1u << USBDEV_PHY_PINS_DRIVE_EN_BIT) |
                              (1u << USBDEV_PHY_PINS_DRIVE_OE_O_BIT);
  abs_mmio_write32(kUsbdevBase + USBDEV_PHY_CONFIG_REG_OFFSET, 0x0u);

  // (a) Drive DP_O = 1, DN_O = 0 with RX_ENABLE_O = 0:
  // RX_DP_I = 1, RX_DN_I = 0, and RX_D_I = 0 (differential receiver suspended
  // when usb_rx_enable_o == 0).
  abs_mmio_write32(kUsbdevBase + USBDEV_PHY_PINS_DRIVE_REG_OFFSET,
                   kDriveEnOe | (1u << USBDEV_PHY_PINS_DRIVE_DP_O_BIT));
  busy_spin_micros(20);
  CHECK(abs_mmio_read32(kUsbdevBase + USBDEV_PHY_PINS_SENSE_REG_OFFSET) ==
        0x00010501u);

  // (b) Drive DP_O = 1, DN_O = 0 with RX_ENABLE_O = 1:
  // RX_DP_I = 1, RX_DN_I = 0, and RX_D_I = 1 (differential receiver active).
  abs_mmio_write32(kUsbdevBase + USBDEV_PHY_PINS_DRIVE_REG_OFFSET,
                   kDriveEnOe | (1u << USBDEV_PHY_PINS_DRIVE_RX_ENABLE_O_BIT) |
                       (1u << USBDEV_PHY_PINS_DRIVE_DP_O_BIT));
  busy_spin_micros(20);
  CHECK(abs_mmio_read32(kUsbdevBase + USBDEV_PHY_PINS_SENSE_REG_OFFSET) ==
        0x00010505u);

  // (c) Drive DP_O = 0, DN_O = 1 with RX_ENABLE_O = 1:
  // RX_DP_I = 0, RX_DN_I = 1, RX_D_I = 0.
  abs_mmio_write32(kUsbdevBase + USBDEV_PHY_PINS_DRIVE_REG_OFFSET,
                   kDriveEnOe | (1u << USBDEV_PHY_PINS_DRIVE_RX_ENABLE_O_BIT) |
                       (1u << USBDEV_PHY_PINS_DRIVE_DN_O_BIT));
  busy_spin_micros(20);
  CHECK(abs_mmio_read32(kUsbdevBase + USBDEV_PHY_PINS_SENSE_REG_OFFSET) ==
        0x00010502u);

  // (d) With TX_USE_D_SE0 = 1 and D_O = 1 (while DP_O = 0, DN_O = 0):
  // i_mux_tx_dp / i_mux_tx_dn in usbdev_iomux.sv still select DP_O / DN_O
  // (0 / 0), so RX_DP_I = 0, RX_DN_I = 0, RX_D_I = 0.
  abs_mmio_write32(kUsbdevBase + USBDEV_PHY_CONFIG_REG_OFFSET,
                   1u << USBDEV_PHY_CONFIG_TX_USE_D_SE0_BIT);
  abs_mmio_write32(kUsbdevBase + USBDEV_PHY_PINS_DRIVE_REG_OFFSET,
                   kDriveEnOe | (1u << USBDEV_PHY_PINS_DRIVE_RX_ENABLE_O_BIT) |
                       (1u << USBDEV_PHY_PINS_DRIVE_D_O_BIT));
  busy_spin_micros(20);
  CHECK(abs_mmio_read32(kUsbdevBase + USBDEV_PHY_PINS_SENSE_REG_OFFSET) ==
        0x00010500u);

  // Restore PHY_PINS_DRIVE = 0 and PHY_CONFIG = 0.
  abs_mmio_write32(kUsbdevBase + USBDEV_PHY_PINS_DRIVE_REG_OFFSET, 0x0u);
  abs_mmio_write32(kUsbdevBase + USBDEV_PHY_CONFIG_REG_OFFSET, 0x0u);

  // 2. Empty RXFIFO readback returns 0 when USBSTAT.RX_DEPTH == 0.
  abs_mmio_write32(kUsbdevBase + USBDEV_FIFO_CTRL_REG_OFFSET,
                   1u << USBDEV_FIFO_CTRL_RX_RST_BIT);
  uint32_t usbstat = abs_mmio_read32(kUsbdevBase + USBDEV_USBSTAT_REG_OFFSET);
  CHECK(((usbstat >> USBDEV_USBSTAT_RX_DEPTH_OFFSET) &
         USBDEV_USBSTAT_RX_DEPTH_MASK) == 0u);
  CHECK(abs_mmio_read32(kUsbdevBase + USBDEV_RXFIFO_REG_OFFSET) == 0x0u);

  // 3. AVOUTBUFFER FIFO 8-entry fill (AV_OUT_FULL == 1) and 9th entry overflow
  // setting INTR_STATE.AV_OVERFLOW == 1.
  abs_mmio_write32(kUsbdevBase + USBDEV_FIFO_CTRL_REG_OFFSET,
                   1u << USBDEV_FIFO_CTRL_AVOUT_RST_BIT);
  abs_mmio_write32(kUsbdevBase + USBDEV_INTR_STATE_REG_OFFSET,
                   1u << USBDEV_INTR_STATE_AV_OVERFLOW_BIT);
  CHECK((abs_mmio_read32(kUsbdevBase + USBDEV_INTR_STATE_REG_OFFSET) &
         (1u << USBDEV_INTR_STATE_AV_OVERFLOW_BIT)) == 0u);

  for (uint32_t i = 0; i < 8; ++i) {
    abs_mmio_write32(kUsbdevBase + USBDEV_AVOUTBUFFER_REG_OFFSET, i);
  }
  usbstat = abs_mmio_read32(kUsbdevBase + USBDEV_USBSTAT_REG_OFFSET);
  CHECK(((usbstat >> USBDEV_USBSTAT_AV_OUT_DEPTH_OFFSET) &
         USBDEV_USBSTAT_AV_OUT_DEPTH_MASK) == 8u);
  CHECK((usbstat & (1u << USBDEV_USBSTAT_AV_OUT_FULL_BIT)) != 0u);
  CHECK((abs_mmio_read32(kUsbdevBase + USBDEV_INTR_STATE_REG_OFFSET) &
         (1u << USBDEV_INTR_STATE_AV_OVERFLOW_BIT)) == 0u);

  // Push 9th buffer ID while AV_OUT_FULL == 1 -> sets AV_OVERFLOW.
  abs_mmio_write32(kUsbdevBase + USBDEV_AVOUTBUFFER_REG_OFFSET, 9u);
  CHECK((abs_mmio_read32(kUsbdevBase + USBDEV_INTR_STATE_REG_OFFSET) &
         (1u << USBDEV_INTR_STATE_AV_OVERFLOW_BIT)) != 0u);
  usbstat = abs_mmio_read32(kUsbdevBase + USBDEV_USBSTAT_REG_OFFSET);
  CHECK(((usbstat >> USBDEV_USBSTAT_AV_OUT_DEPTH_OFFSET) &
         USBDEV_USBSTAT_AV_OUT_DEPTH_MASK) == 8u);

  // Reset AVOUTBUFFER and clear AV_OVERFLOW.
  abs_mmio_write32(kUsbdevBase + USBDEV_FIFO_CTRL_REG_OFFSET,
                   1u << USBDEV_FIFO_CTRL_AVOUT_RST_BIT);
  abs_mmio_write32(kUsbdevBase + USBDEV_INTR_STATE_REG_OFFSET,
                   1u << USBDEV_INTR_STATE_AV_OVERFLOW_BIT);

  // 4. AON Wake Control (WAKE_CONTROL & WAKE_EVENTS):
  // (a) Drive SE0 via PHY_PINS_DRIVE (EN=1, OE_O=1, DP_O=0, DN_O=0,
  // DP_PULLUP_EN_O=0) and request SUSPEND_REQ = 1 -> WAKE_EVENTS sets
  // MODULE_ACTIVE | BUS_RESET (0x9) while BUS_NOT_IDLE is 0 (usb_dp_i ==
  // aon_dppullup_en == 0 and usb_dn_i == aon_dnpullup_en == 0).
  abs_mmio_write32(kUsbdevBase + USBDEV_PHY_PINS_DRIVE_REG_OFFSET, kDriveEnOe);
  busy_spin_micros(20);
  abs_mmio_write32(kUsbdevBase + USBDEV_WAKE_CONTROL_REG_OFFSET,
                   1u << USBDEV_WAKE_CONTROL_SUSPEND_REQ_BIT);
  busy_spin_micros(60);

  uint32_t wake_events =
      abs_mmio_read32(kUsbdevBase + USBDEV_WAKE_EVENTS_REG_OFFSET);
  CHECK(wake_events == ((1u << USBDEV_WAKE_EVENTS_MODULE_ACTIVE_BIT) |
                        (1u << USBDEV_WAKE_EVENTS_BUS_RESET_BIT)));

  // (b) While MODULE_ACTIVE == 1 (aon_dppullup_en latched at 0), drive DP_O = 1
  // -> usb_dp_i != aon_dppullup_en, latching BUS_NOT_IDLE alongside sticky
  // BUS_RESET (0xd).
  abs_mmio_write32(kUsbdevBase + USBDEV_PHY_PINS_DRIVE_REG_OFFSET,
                   kDriveEnOe | (1u << USBDEV_PHY_PINS_DRIVE_DP_O_BIT));
  busy_spin_micros(60);
  wake_events = abs_mmio_read32(kUsbdevBase + USBDEV_WAKE_EVENTS_REG_OFFSET);
  CHECK(wake_events == ((1u << USBDEV_WAKE_EVENTS_MODULE_ACTIVE_BIT) |
                        (1u << USBDEV_WAKE_EVENTS_BUS_NOT_IDLE_BIT) |
                        (1u << USBDEV_WAKE_EVENTS_BUS_RESET_BIT)));

  // Clear PHY_PINS_DRIVE = 0 and acknowledge wake (WAKE_ACK = 1) -> clears
  // WAKE_EVENTS to 0.
  abs_mmio_write32(kUsbdevBase + USBDEV_PHY_PINS_DRIVE_REG_OFFSET, 0x0u);
  abs_mmio_write32(kUsbdevBase + USBDEV_WAKE_CONTROL_REG_OFFSET,
                   1u << USBDEV_WAKE_CONTROL_WAKE_ACK_BIT);
  busy_spin_micros(60);
  CHECK(abs_mmio_read32(kUsbdevBase + USBDEV_WAKE_EVENTS_REG_OFFSET) == 0x0u);

  // (c) Hold D+ pullup (idle J state: usb_dp_i = 1, usb_dn_i = 0) and request
  // SUSPEND_REQ = 1 -> WAKE_EVENTS == MODULE_ACTIVE (0x1, no BUS_RESET or
  // BUS_NOT_IDLE). Then enable USBCTRL.ENABLE = 1, release PHY_PINS_DRIVE = 0,
  // and send WAKE_ACK = 1 -> WAKE_EVENTS clears to 0 and USBSTAT.LINK_STATE ==
  // POWERED (1).
  abs_mmio_write32(kUsbdevBase + USBDEV_PHY_PINS_DRIVE_REG_OFFSET,
                   (1u << USBDEV_PHY_PINS_DRIVE_EN_BIT) |
                       (1u << USBDEV_PHY_PINS_DRIVE_DP_PULLUP_EN_O_BIT));
  busy_spin_micros(20);
  abs_mmio_write32(kUsbdevBase + USBDEV_WAKE_CONTROL_REG_OFFSET,
                   1u << USBDEV_WAKE_CONTROL_SUSPEND_REQ_BIT);
  busy_spin_micros(60);
  CHECK(abs_mmio_read32(kUsbdevBase + USBDEV_WAKE_EVENTS_REG_OFFSET) ==
        (1u << USBDEV_WAKE_EVENTS_MODULE_ACTIVE_BIT));

  abs_mmio_write32(kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET,
                   1u << USBDEV_USBCTRL_ENABLE_BIT);
  abs_mmio_write32(kUsbdevBase + USBDEV_PHY_PINS_DRIVE_REG_OFFSET, 0x0u);
  abs_mmio_write32(kUsbdevBase + USBDEV_WAKE_CONTROL_REG_OFFSET,
                   1u << USBDEV_WAKE_CONTROL_WAKE_ACK_BIT);
  busy_spin_micros(60);
  CHECK(abs_mmio_read32(kUsbdevBase + USBDEV_WAKE_EVENTS_REG_OFFSET) == 0x0u);
  usbstat = abs_mmio_read32(kUsbdevBase + USBDEV_USBSTAT_REG_OFFSET);
  CHECK(((usbstat >> USBDEV_USBSTAT_LINK_STATE_OFFSET) &
         USBDEV_USBSTAT_LINK_STATE_MASK) ==
        USBDEV_USBSTAT_LINK_STATE_VALUE_POWERED);

  // Clean up USBCTRL.
  abs_mmio_write32(kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET, 0x0u);

  LOG_INFO("ot_usbdev FPGA/QEMU consistency test passed");
  return true;
}
