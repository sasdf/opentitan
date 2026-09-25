// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// CW340 FPGA Verification Test for OpenTitan Earlgrey v2 (`trunk-v2`) `usbdev`:
//
// 1. `hw/ip/usbdev/rtl/usbdev.sv:264-269, 728-729`, `usbdev_usbif.sv:156`,
//    `hw/ip/usbdev/data/usbdev.hjson:441-449, 607-614`:
//    `USBSTAT.rx_empty` (`bit 31`) reads `0` (`!= resval: "1"`) and
//    `USBCTRL.device_address` (`bits 22:16`) is forced to `0` whenever
//    `USBCTRL.enable == 0` (`connect_en == 0`), and writing `enable = 1` +
//    `device_address` in the same CSR write loses `device_address` across the
//    48 MHz CDC latency.
// 2. `hw/ip/usbdev/rtl/usbdev_linkstate.sv:183-188`, `usb_fs_tx.sv:135-145`,
//    `hw/ip/usbdev/data/usbdev.hjson:430-439`:
//    Writing `USBCTRL.resume_link_active = 1` in `LinkPowered (1)` advances
//    through `LinkResuming (6)` and settles in `LinkActiveNoSOF (5)` (not
//    `LinkActive (3)`) on the next idle `J` cycle, and
//    `PHY_CONFIG.tx_osc_test_mode`
//    (`bit 5`) drives `PHY_PINS_SENSE.tx_oe_o = 1` (`bit 12`).
// 3. `hw/ip/usbdev/rtl/usbdev.sv:780-783`, `tlul_adapter_sram.sv:144-166`,
//    `hw/ip/usbdev/data/usbdev.hjson:340-345`:
//    `USBDEV.BUFFER` (`0x800..0xFFF`, `tlul_adapter_sram #(.ByteAccess(0))`)
//    permits sub-word reads (`lb`/`lh`, `d_error = 0`) while rejecting
//    sub-word writes (`sb`/`sh` -> `mcause = 7`), alongside `USBDEV_PERMIT`
//    sub-word CSR write faults and `0x0AC..0x7FC` `addrmiss` faults.
// 4. `hw/ip/usbdev/data/usbdev.hjson:720-752`,
// `hw/ip/usbdev/rtl/usbdev.sv:488-510`,
//    `hw/ip/usbdev/rtl/usbdev_reg_pkg.sv:1034`,
//    `hw/ip/usbdev/rtl/usbdev_reg_top.sv:3470-3504`,
//    `sw/device/lib/dif/dif_usbdev.c:182-197, 309-342` (New in `trunk-v2`
//    commit `73f2f7ac5311`): `RXENABLE_OUT` (`0x28`) was converted from a
//    12-bit `multireg` (`4'b0011`) to a 28-bit `hwext: "true"`, `hwqe: "true"`
//    register with a write-only `preserve` field (`bits [27:16]`, `swaccess:
//    "wo"`):
//    - `preserve` uses inverted write-enable polarity (`rxenable_out_we =
//    ~preserve`,
//      `0` = update, `1` = preserve), opposite to `OUT_DATA_TOGGLE.mask`
//      (`0x3C`) and `IN_DATA_TOGGLE.mask` (`0x40`) (`1` = update, `0` =
//      preserve); writing the `DATA_TOGGLE` idiom `((1u << ep) << 16) | (1u <<
//      ep)` leaves `ep` unchanged (`0`) while wiping all 11 other endpoints to
//      `0`.
//    - Because `preserve` is `wo` (`qs == 0`), a standard read-modify-write on
//      `RXENABLE_OUT` reads `preserve == 0` and writes back `preserve == 0`,
//      forcing `rxenable_out_we = 12'hFFF` across all 12 endpoints.
//    - Because `USBDEV_PERMIT[12]` changed from `4'b0011` to `4'b1111` and
//      `rxenable_out_qe = &rxenable_out_flds_we`, a 16-bit halfword write
//      (`sh`) to `RXENABLE_OUT` (`0x28`) now traps with `mcause = 7` (unlike
//      `RXENABLE_SETUP` `0x24` and `SET_NAK_OUT` `0x2C`, which retain `4'b0011`
//      and accept `sh`).

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_pinmux.h"
#include "sw/device/lib/dif/dif_usbdev.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top/usbdev_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kUsbdevBase = TOP_EARLGREY_USBDEV_BASE_ADDR,
  kPinmuxBase = TOP_EARLGREY_PINMUX_BASE_ADDR,
};

static volatile uint32_t g_fault_count = 0;
static volatile uint32_t g_last_mcause = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = 0;
  uint32_t mepc = 0;
  CSR_READ(CSR_REG_MCAUSE, &mcause);
  CSR_READ(CSR_REG_MEPC, &mepc);
  g_last_mcause = mcause;
  g_fault_count++;
  uint16_t insn_half = *(const uint16_t *)mepc;
  uint32_t step = ((insn_half & 0x3u) != 0x3u) ? 2u : 4u;
  CSR_WRITE(CSR_REG_MEPC, mepc + step);
}

static void test_usbdev_rx_empty_and_devaddr_gated_by_enable(void) {
  LOG_INFO(
      "Testing [usbdev.sv:264-269,728-729]: USBSTAT.rx_empty & "
      "USBCTRL.device_address gated by USBCTRL.enable");

  abs_mmio_write32(kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET, 0x0u);
  abs_mmio_write32(kUsbdevBase + USBDEV_FIFO_CTRL_REG_OFFSET,
                   (1u << USBDEV_FIFO_CTRL_AVOUT_RST_BIT) |
                       (1u << USBDEV_FIFO_CTRL_AVSETUP_RST_BIT) |
                       (1u << USBDEV_FIFO_CTRL_RX_RST_BIT));
  busy_spin_micros(20);

  uint32_t usbstat = abs_mmio_read32(kUsbdevBase + USBDEV_USBSTAT_REG_OFFSET);
  uint32_t rx_depth = (usbstat >> USBDEV_USBSTAT_RX_DEPTH_OFFSET) &
                      USBDEV_USBSTAT_RX_DEPTH_MASK;
  uint32_t rx_empty = (usbstat >> USBDEV_USBSTAT_RX_EMPTY_BIT) & 1u;
  CHECK(rx_depth == 0u);
  CHECK(rx_empty == 0u,
        "Expected USBSTAT.rx_empty == 0 when USBCTRL.enable == 0");

  const uint32_t kTestAddr = 0x2au;
  abs_mmio_write32(kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET,
                   kTestAddr << USBDEV_USBCTRL_DEVICE_ADDRESS_OFFSET);
  busy_spin_micros(20);
  uint32_t usbctrl = abs_mmio_read32(kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET);
  uint32_t devaddr = (usbctrl >> USBDEV_USBCTRL_DEVICE_ADDRESS_OFFSET) &
                     USBDEV_USBCTRL_DEVICE_ADDRESS_MASK;
  CHECK(devaddr == 0u,
        "Expected device_address forced to 0 when USBCTRL.enable == 0");

  // Writing enable = 1 and device_address in the same CSR write when enable
  // was 0 loses device_address across the 48 MHz CDC latency.
  abs_mmio_write32(kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET,
                   (1u << USBDEV_USBCTRL_ENABLE_BIT) |
                       (kTestAddr << USBDEV_USBCTRL_DEVICE_ADDRESS_OFFSET));
  busy_spin_micros(20);
  usbctrl = abs_mmio_read32(kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET);
  devaddr = (usbctrl >> USBDEV_USBCTRL_DEVICE_ADDRESS_OFFSET) &
            USBDEV_USBCTRL_DEVICE_ADDRESS_MASK;
  CHECK(devaddr == 0u,
        "Expected simultaneous enable=1 + device_address write to be cleared "
        "by CDC clr_devaddr");

  // Once enable == 1 has settled across CDC, USBSTAT.rx_empty == 1 and
  // writing device_address latches 0x2a.
  usbstat = abs_mmio_read32(kUsbdevBase + USBDEV_USBSTAT_REG_OFFSET);
  rx_empty = (usbstat >> USBDEV_USBSTAT_RX_EMPTY_BIT) & 1u;
  CHECK(rx_empty == 1u,
        "Expected USBSTAT.rx_empty == 1 when USBCTRL.enable == 1");

  abs_mmio_write32(kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET,
                   (1u << USBDEV_USBCTRL_ENABLE_BIT) |
                       (kTestAddr << USBDEV_USBCTRL_DEVICE_ADDRESS_OFFSET));
  busy_spin_micros(20);
  usbctrl = abs_mmio_read32(kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET);
  devaddr = (usbctrl >> USBDEV_USBCTRL_DEVICE_ADDRESS_OFFSET) &
            USBDEV_USBCTRL_DEVICE_ADDRESS_MASK;
  CHECK(devaddr == kTestAddr,
        "Expected device_address == 0x2a when USBCTRL.enable == 1");

  // Clearing USBCTRL.enable = 0 clears both device_address -> 0 and
  // USBSTAT.rx_empty -> 0.
  abs_mmio_write32(kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET,
                   kTestAddr << USBDEV_USBCTRL_DEVICE_ADDRESS_OFFSET);
  busy_spin_micros(20);
  usbctrl = abs_mmio_read32(kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET);
  devaddr = (usbctrl >> USBDEV_USBCTRL_DEVICE_ADDRESS_OFFSET) &
            USBDEV_USBCTRL_DEVICE_ADDRESS_MASK;
  usbstat = abs_mmio_read32(kUsbdevBase + USBDEV_USBSTAT_REG_OFFSET);
  rx_empty = (usbstat >> USBDEV_USBSTAT_RX_EMPTY_BIT) & 1u;
  CHECK(devaddr == 0u);
  CHECK(rx_empty == 0u);
}

static void test_usbdev_resume_link_active_no_sof_and_tx_osc(
    const dif_pinmux_t *pinmux) {
  LOG_INFO(
      "Testing [usbdev_linkstate.sv:183-188, usb_fs_tx.sv:135-145]: "
      "resume_link_active -> LinkActiveNoSOF(5), VBUS loss preservation & "
      "tx_osc_test_mode");

  const uint32_t kDriveIdleJ = (1u << USBDEV_PHY_PINS_DRIVE_EN_BIT) |
                               (1u << USBDEV_PHY_PINS_DRIVE_OE_O_BIT) |
                               (1u << USBDEV_PHY_PINS_DRIVE_RX_ENABLE_O_BIT) |
                               (1u << USBDEV_PHY_PINS_DRIVE_DP_O_BIT);
  abs_mmio_write32(kUsbdevBase + USBDEV_PHY_CONFIG_REG_OFFSET, 0x0u);
  abs_mmio_write32(kUsbdevBase + USBDEV_PHY_PINS_DRIVE_REG_OFFSET, kDriveIdleJ);
  busy_spin_micros(20);

  abs_mmio_write32(kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET,
                   1u << USBDEV_USBCTRL_ENABLE_BIT);
  busy_spin_micros(20);
  uint32_t usbstat = abs_mmio_read32(kUsbdevBase + USBDEV_USBSTAT_REG_OFFSET);
  uint32_t link_state = (usbstat >> USBDEV_USBSTAT_LINK_STATE_OFFSET) &
                        USBDEV_USBSTAT_LINK_STATE_MASK;
  CHECK(link_state == 1u, "Expected link_state == LinkPowered (1), got %u",
        link_state);

  const uint32_t kTestAddr = 0x2au;
  abs_mmio_write32(kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET,
                   (1u << USBDEV_USBCTRL_ENABLE_BIT) |
                       (1u << USBDEV_USBCTRL_RESUME_LINK_ACTIVE_BIT) |
                       (kTestAddr << USBDEV_USBCTRL_DEVICE_ADDRESS_OFFSET));
  abs_mmio_write32(kUsbdevBase + USBDEV_AVOUTBUFFER_REG_OFFSET, 0x3u);
  busy_spin_micros(20);

  usbstat = abs_mmio_read32(kUsbdevBase + USBDEV_USBSTAT_REG_OFFSET);
  link_state = (usbstat >> USBDEV_USBSTAT_LINK_STATE_OFFSET) &
               USBDEV_USBSTAT_LINK_STATE_MASK;
  CHECK(link_state == 5u, "Expected link_state == LinkActiveNoSOF (5), got %u",
        link_state);
  CHECK(((usbstat >> USBDEV_USBSTAT_AV_OUT_DEPTH_OFFSET) &
         USBDEV_USBSTAT_AV_OUT_DEPTH_MASK) == 1u);

  // Simulate VBUS loss (UsbdevSense -> ConstantZero) while USBCTRL.enable == 1:
  // link_state drops to LinkDisconnected (0) and sense == 0, while FIFOs
  // (av_out_depth == 1) and USBCTRL.device_address (0x2a) are preserved.
  CHECK_DIF_OK(
      dif_pinmux_input_select(pinmux, kTopEarlgreyPinmuxPeripheralInUsbdevSense,
                              kTopEarlgreyPinmuxInselConstantZero));
  busy_spin_micros(20);
  usbstat = abs_mmio_read32(kUsbdevBase + USBDEV_USBSTAT_REG_OFFSET);
  link_state = (usbstat >> USBDEV_USBSTAT_LINK_STATE_OFFSET) &
               USBDEV_USBSTAT_LINK_STATE_MASK;
  uint32_t usbctrl = abs_mmio_read32(kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET);
  uint32_t devaddr = (usbctrl >> USBDEV_USBCTRL_DEVICE_ADDRESS_OFFSET) &
                     USBDEV_USBCTRL_DEVICE_ADDRESS_MASK;
  CHECK(((usbstat >> USBDEV_USBSTAT_SENSE_BIT) & 1u) == 0u);
  CHECK(link_state == 0u, "Expected LinkDisconnected (0) on VBUS loss, got %u",
        link_state);
  CHECK(devaddr == kTestAddr,
        "Expected VBUS loss to preserve device_address == 0x2a, got 0x%x",
        devaddr);
  CHECK(((usbstat >> USBDEV_USBSTAT_AV_OUT_DEPTH_OFFSET) &
         USBDEV_USBSTAT_AV_OUT_DEPTH_MASK) == 1u,
        "Expected VBUS loss to preserve AVOUTBUFFER depth == 1");

  // Restore UsbdevSense -> ConstantOne and clear FIFO.
  CHECK_DIF_OK(
      dif_pinmux_input_select(pinmux, kTopEarlgreyPinmuxPeripheralInUsbdevSense,
                              kTopEarlgreyPinmuxInselConstantOne));
  abs_mmio_write32(kUsbdevBase + USBDEV_FIFO_CTRL_REG_OFFSET,
                   (1u << USBDEV_FIFO_CTRL_AVOUT_RST_BIT) |
                       (1u << USBDEV_FIFO_CTRL_AVSETUP_RST_BIT) |
                       (1u << USBDEV_FIFO_CTRL_RX_RST_BIT));
  busy_spin_micros(20);

  abs_mmio_write32(kUsbdevBase + USBDEV_PHY_PINS_DRIVE_REG_OFFSET, 0x0u);
  abs_mmio_write32(kUsbdevBase + USBDEV_PHY_CONFIG_REG_OFFSET,
                   1u << USBDEV_PHY_CONFIG_TX_OSC_TEST_MODE_BIT);
  busy_spin_micros(20);
  uint32_t pins_sense =
      abs_mmio_read32(kUsbdevBase + USBDEV_PHY_PINS_SENSE_REG_OFFSET);
  CHECK(((pins_sense >> USBDEV_PHY_PINS_SENSE_TX_OE_O_BIT) & 1u) == 1u,
        "Expected PHY_PINS_SENSE.tx_oe_o == 1 in tx_osc_test_mode");

  abs_mmio_write32(kUsbdevBase + USBDEV_PHY_CONFIG_REG_OFFSET, 0x4u);
  abs_mmio_write32(kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET, 0x0u);
}

static void test_usbdev_buffer_asymmetric_subword_and_csr_faults(void) {
  LOG_INFO(
      "Testing [usbdev.sv:780-783]: BUFFER sub-word read OK / write fault & "
      "CSR TL-UL faults");

  const uint32_t kBufferWord0 = kUsbdevBase + USBDEV_BUFFER_REG_OFFSET;
  const uint32_t kPattern = 0x11223344u;

  g_fault_count = 0;
  abs_mmio_write32(kBufferWord0, kPattern);
  CHECK(g_fault_count == 0u);
  CHECK(abs_mmio_read32(kBufferWord0) == kPattern);

  uint8_t b0 = abs_mmio_read8(kBufferWord0 + 0u);
  uint8_t b1 = abs_mmio_read8(kBufferWord0 + 1u);
  uint8_t b2 = abs_mmio_read8(kBufferWord0 + 2u);
  uint8_t b3 = abs_mmio_read8(kBufferWord0 + 3u);
  uint16_t h1 = *(const volatile uint16_t *)(uintptr_t)(kBufferWord0 + 2u);
  CHECK(g_fault_count == 0u);
  CHECK(b0 == 0x44u);
  CHECK(b1 == 0x33u);
  CHECK(b2 == 0x22u);
  CHECK(b3 == 0x11u);
  CHECK(h1 == 0x1122u);

  g_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write8(kBufferWord0, 0xAAu);
  CHECK(g_fault_count == 1u);
  CHECK(g_last_mcause == (uint32_t)kIbexExcStoreAccessFault);

  g_fault_count = 0;
  g_last_mcause = 0;
  *(volatile uint16_t *)(uintptr_t)(kBufferWord0 + 2u) = 0xBBCCu;
  CHECK(g_fault_count == 1u);
  CHECK(g_last_mcause == (uint32_t)kIbexExcStoreAccessFault);
  CHECK(abs_mmio_read32(kBufferWord0) == kPattern);

  g_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write8(kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET, 0x01u);
  CHECK(g_fault_count == 1u);
  CHECK(g_last_mcause == (uint32_t)kIbexExcStoreAccessFault);

  g_fault_count = 0;
  g_last_mcause = 0;
  (void)abs_mmio_read32(kUsbdevBase + 0x100u);
  CHECK(g_fault_count == 1u);
  CHECK(g_last_mcause == (uint32_t)kIbexExcLoadAccessFault);

  g_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write32(kUsbdevBase + 0x100u, 0xDEADBEEFu);
  CHECK(g_fault_count == 1u);
  CHECK(g_last_mcause == (uint32_t)kIbexExcStoreAccessFault);
}

static void test_usbdev_v2_rxenable_out_preserve_inversion_and_permit(void) {
  LOG_INFO(
      "Testing [usbdev.sv:488-510, usbdev_reg_pkg.sv:1034, "
      "usbdev_reg_top.sv:3470-3504]: v2 RXENABLE_OUT.preserve inversion, "
      "wo readback RMW hazard, and 16-bit sh fault");

  dif_usbdev_t usbdev;
  CHECK_DIF_OK(dif_usbdev_init(mmio_region_from_addr(kUsbdevBase), &usbdev));

  // Drive idle J state (DP_O = 1, DN_O = 0) and enable USBCTRL so the 48 MHz
  // link state machine is not in SE0 `link_reset` (which holds `data_toggle_q`
  // at 0 in `usb_fs_nb_out_pe.sv:365-366`).
  const uint32_t kDriveIdleJ = (1u << USBDEV_PHY_PINS_DRIVE_EN_BIT) |
                               (1u << USBDEV_PHY_PINS_DRIVE_OE_O_BIT) |
                               (1u << USBDEV_PHY_PINS_DRIVE_RX_ENABLE_O_BIT) |
                               (1u << USBDEV_PHY_PINS_DRIVE_DP_O_BIT);
  abs_mmio_write32(kUsbdevBase + USBDEV_PHY_PINS_DRIVE_REG_OFFSET, kDriveIdleJ);
  abs_mmio_write32(kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET,
                   1u << USBDEV_USBCTRL_ENABLE_BIT);
  busy_spin_micros(20);

  // 1. Compare OUT_DATA_TOGGLE (0x3C) and IN_DATA_TOGGLE (0x40) positive `mask`
  // ([27:16] = 1 updates) against RXENABLE_OUT (0x28) inverted `preserve`
  // ([27:16] = 1 preserves, 0 updates):
  // Initialize OUT_DATA_TOGGLE and IN_DATA_TOGGLE to 0x005 (endpoints 0 and 2 =
  // 1) using mask = 0xFFF.
  CHECK_DIF_OK(dif_usbdev_data_toggle_out_write(&usbdev, 0x0FFFu, 0x0005u));
  CHECK_DIF_OK(dif_usbdev_data_toggle_in_write(&usbdev, 0x0FFFu, 0x0005u));
  busy_spin_micros(10);
  uint16_t toggles = 0;
  uint16_t in_toggles = 0;
  CHECK_DIF_OK(dif_usbdev_data_toggle_out_read(&usbdev, &toggles));
  CHECK_DIF_OK(dif_usbdev_data_toggle_in_read(&usbdev, &in_toggles));
  CHECK(toggles == 0x0005u, "Expected toggles == 0x0005, got 0x%04x", toggles);
  CHECK(in_toggles == 0x0005u, "Expected in_toggles == 0x0005, got 0x%04x",
        in_toggles);

  // Set endpoint 5 in OUT_DATA_TOGGLE and IN_DATA_TOGGLE using mask = (1 << 5),
  // state = (1 << 5): endpoints 0 and 2 are preserved, and endpoint 5 becomes 1
  // (`0x0025`).
  const uint32_t kMaskAndBit5 = ((1u << 5) << 16) | (1u << 5);
  abs_mmio_write32(kUsbdevBase + USBDEV_OUT_DATA_TOGGLE_REG_OFFSET,
                   kMaskAndBit5);
  abs_mmio_write32(kUsbdevBase + USBDEV_IN_DATA_TOGGLE_REG_OFFSET,
                   kMaskAndBit5);
  busy_spin_micros(10);
  CHECK_DIF_OK(dif_usbdev_data_toggle_out_read(&usbdev, &toggles));
  CHECK_DIF_OK(dif_usbdev_data_toggle_in_read(&usbdev, &in_toggles));
  CHECK(toggles == 0x0025u,
        "OUT_DATA_TOGGLE.mask (1=update) must set ep5 and preserve ep0,ep2");
  CHECK(in_toggles == 0x0025u,
        "IN_DATA_TOGGLE.mask (1=update) must set ep5 and preserve ep0,ep2");

  // Now initialize RXENABLE_OUT to 0x005 (endpoints 0 and 2 enabled) using
  // preserve = 0x000 (update all 12 endpoints).
  abs_mmio_write32(kUsbdevBase + USBDEV_RXENABLE_OUT_REG_OFFSET, 0x00000005u);
  CHECK(abs_mmio_read32(kUsbdevBase + USBDEV_RXENABLE_OUT_REG_OFFSET) ==
        0x00000005u);

  // Write the exact same `((1u << 5) << 16) | (1u << 5)` word to RXENABLE_OUT:
  // Because `rxenable_out_we = ~preserve`, bit 5 in [27:16] (`preserve[5] = 1`)
  // PREVENTS endpoint 5 from being set (`out[5]` stays `0`), while `preserve =
  // 0` on all 11 other bits WIPES endpoints 0 and 2 to `0` (`out` becomes
  // `0x000`)!
  abs_mmio_write32(kUsbdevBase + USBDEV_RXENABLE_OUT_REG_OFFSET, kMaskAndBit5);
  uint32_t rxenable_out =
      abs_mmio_read32(kUsbdevBase + USBDEV_RXENABLE_OUT_REG_OFFSET);
  CHECK(rxenable_out == 0x00000000u,
        "RXENABLE_OUT.preserve (0=update, 1=preserve) blocks ep5 and wipes "
        "ep0,ep2 to 0, got 0x%08x",
        rxenable_out);

  // 2. Verify that `dif_usbdev_endpoint_out_enable()` writes `preserve = 0xFDF`
  // to enable ep5 while preserving ep0 and ep2, and that `preserve` (`wo`)
  // always reads back as `0` in `[27:16]`.
  abs_mmio_write32(kUsbdevBase + USBDEV_RXENABLE_OUT_REG_OFFSET, 0x00000005u);
  abs_mmio_write32(kUsbdevBase + USBDEV_RXENABLE_OUT_REG_OFFSET, 0x0FFF0000u);
  CHECK((abs_mmio_read32(kUsbdevBase + USBDEV_RXENABLE_OUT_REG_OFFSET) >> 16) ==
            0u,
        "Expected RXENABLE_OUT.preserve (wo) [27:16] to read back 0");
  CHECK_DIF_OK(dif_usbdev_endpoint_out_enable(&usbdev, 5u, kDifToggleEnabled));
  rxenable_out = abs_mmio_read32(kUsbdevBase + USBDEV_RXENABLE_OUT_REG_OFFSET);
  CHECK(rxenable_out == 0x00000025u,
        "Expected RXENABLE_OUT == 0x00000025 (with wo preserve [27:16] == 0), "
        "got 0x%08x",
        rxenable_out);

  // 3. Verify that whereas 16-bit halfword writes (`sh`) to the adjacent 12-bit
  // endpoint bitmask CSRs `RXENABLE_SETUP` (`0x24`, `USBDEV_PERMIT[11] =
  // 4'b0011`) and `SET_NAK_OUT` (`0x2C`, `USBDEV_PERMIT[13] = 4'b0011`) succeed
  // with `fault_count == 0`, a 16-bit halfword write (`sh`) to `RXENABLE_OUT`
  // (`0x28`, `USBDEV_PERMIT[12] = 4'b1111` in `trunk-v2`) traps with a
  // synchronous Store Access Fault (`mcause = 7`) and leaves `RXENABLE_OUT`
  // unchanged (`0x00000025`).
  g_fault_count = 0;
  *(volatile uint16_t *)(uintptr_t)(kUsbdevBase +
                                    USBDEV_RXENABLE_SETUP_REG_OFFSET) = 0x0015u;
  *(volatile uint16_t *)(uintptr_t)(kUsbdevBase +
                                    USBDEV_SET_NAK_OUT_REG_OFFSET) = 0x000Au;
  CHECK(g_fault_count == 0u,
        "16-bit halfword writes to RXENABLE_SETUP and SET_NAK_OUT (4'b0011) "
        "must not fault");
  CHECK(abs_mmio_read32(kUsbdevBase + USBDEV_RXENABLE_SETUP_REG_OFFSET) ==
        0x0015u);
  CHECK(abs_mmio_read32(kUsbdevBase + USBDEV_SET_NAK_OUT_REG_OFFSET) ==
        0x000Au);

  g_fault_count = 0;
  g_last_mcause = 0;
  *(volatile uint16_t *)(uintptr_t)(kUsbdevBase +
                                    USBDEV_RXENABLE_OUT_REG_OFFSET) = 0x0FFFu;
  CHECK(g_fault_count == 1u,
        "Expected 16-bit sh to RXENABLE_OUT (4'b1111 in trunk-v2) to trap");
  CHECK(g_last_mcause == (uint32_t)kIbexExcStoreAccessFault);
  CHECK(abs_mmio_read32(kUsbdevBase + USBDEV_RXENABLE_OUT_REG_OFFSET) ==
        0x00000025u);

  // Clean up CSRs.
  abs_mmio_write32(kUsbdevBase + USBDEV_RXENABLE_SETUP_REG_OFFSET, 0u);
  abs_mmio_write32(kUsbdevBase + USBDEV_SET_NAK_OUT_REG_OFFSET, 0u);
  abs_mmio_write32(kUsbdevBase + USBDEV_RXENABLE_OUT_REG_OFFSET, 0u);
  CHECK_DIF_OK(dif_usbdev_data_toggle_out_write(&usbdev, 0x0FFFu, 0x0000u));
  CHECK_DIF_OK(dif_usbdev_data_toggle_in_write(&usbdev, 0x0FFFu, 0x0000u));
}

bool test_main(void) {
  LOG_INFO("Starting usbdev CW340 FPGA errata-v2 verification test (P31)");

  dif_pinmux_t pinmux;
  CHECK_DIF_OK(dif_pinmux_init(mmio_region_from_addr(kPinmuxBase), &pinmux));
  CHECK_DIF_OK(dif_pinmux_input_select(
      &pinmux, kTopEarlgreyPinmuxPeripheralInUsbdevSense,
      kTopEarlgreyPinmuxInselConstantOne));
  busy_spin_micros(20);

  test_usbdev_rx_empty_and_devaddr_gated_by_enable();
  test_usbdev_resume_link_active_no_sof_and_tx_osc(&pinmux);
  test_usbdev_buffer_asymmetric_subword_and_csr_faults();
  test_usbdev_v2_rxenable_out_preserve_inversion_and_permit();

  LOG_INFO("All usbdev errata-v2 checks PASSED on CW340 FPGA");
  return true;
}
