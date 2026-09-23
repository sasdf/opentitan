// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * CW340 FPGA & QEMU Empirical Errata Confirmation Test for `usbdev` (P17).
 *
 * Exercises and confirms all 3 silicon / spec / architectural behaviors in
 * `/root/knowledge/errata/usbdev.md`:
 * - [usbdev.sv:263-268,709-710]: `USBSTAT.rx_empty` reads `0` at reset and
 * whenever `USBCTRL.enable == 0` (`connect_en & ~rx_fifo_rvalid` in
 * `usbdev.sv:268`, contradicting `resval: "1"` in `usbdev.hjson`), and
 * `USBCTRL.device_address` is held at `0` whenever `USBCTRL.enable == 0`
 * (`clr_devaddr_o = ~connect_en_i | link_reset`).
 * - [usbdev_linkstate.sv:144-200]: Writing `USBCTRL.resume_link_active = 1`
 * from `LinkPowered` transitions `USBSTAT.link_state` through `LinkResuming
 * (6)` into `LinkActiveNoSOF (5)` (not `LinkActive (3)`), and
 *   `PHY_CONFIG.tx_osc_test_mode` forces `PHY_PINS_SENSE.tx_oe_o = 1`.
 * - [usbdev.sv:760-763]: `USBDEV.BUFFER` (`0x800..0xFFF`, `tlul_adapter_sram
 *   #(.ByteAccess(0))`) permits sub-word reads (`lb`/`lh`, `d_error = 0`)
 *   while rejecting sub-word writes (`sb`/`sh`) with a Store Access Fault
 *   (`mcause = 7`); sub-word writes to multi-byte CSRs (`USBCTRL`) and accesses
 *   to the unmapped CSR gap (`0x0AC..0x7FC`) also fault (`mcause = 5 / 7`).
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_pinmux.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "usbdev_regs.h"

OTTF_DEFINE_TEST_CONFIG();

#define CHECK_EQ(a, b, ...) CHECK((a) == (b), __VA_ARGS__)

enum {
  kUsbdevBase = TOP_EARLGREY_USBDEV_BASE_ADDR,
  kPinmuxBase = TOP_EARLGREY_PINMUX_AON_BASE_ADDR,
  kRiscvLoadAccessFault = 5,
  kRiscvStoreAccessFault = 7,
};

static volatile uint32_t g_fault_count = 0;
static volatile uint32_t g_last_mcause = 0;

void ottf_exception_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = 0;
  uint32_t mepc = 0;
  CSR_READ(CSR_REG_MCAUSE, &mcause);
  CSR_READ(CSR_REG_MEPC, &mepc);
  if (mcause == kRiscvLoadAccessFault || mcause == kRiscvStoreAccessFault) {
    g_fault_count++;
    g_last_mcause = mcause;
    uint16_t insn16 = *(const volatile uint16_t *)mepc;
    uint32_t step = ((insn16 & 0x3u) != 0x3u) ? 2u : 4u;
    CSR_WRITE(CSR_REG_MEPC, mepc + step);
    return;
  }
  ottf_generic_fault_print(exc_info, "Unhandled exception", mcause);
  abort();
}

static void test_usbdev_rx_empty_and_devaddr_gated_by_enable(void) {
  LOG_INFO(
      "Testing [usbdev.sv:263-268,709-710]: rx_empty & device_address gated by "
      "USBCTRL.enable");

  // Drive idle J state (DP_O = 1, DN_O = 0) via PHY_PINS_DRIVE so the bus is
  // not in SE0 link_reset while testing connect_en gating.
  const uint32_t kDriveIdleJ = (1u << USBDEV_PHY_PINS_DRIVE_EN_BIT) |
                               (1u << USBDEV_PHY_PINS_DRIVE_OE_O_BIT) |
                               (1u << USBDEV_PHY_PINS_DRIVE_RX_ENABLE_O_BIT) |
                               (1u << USBDEV_PHY_PINS_DRIVE_DP_O_BIT);
  abs_mmio_write32(kUsbdevBase + USBDEV_PHY_CONFIG_REG_OFFSET, 0x0u);
  abs_mmio_write32(kUsbdevBase + USBDEV_PHY_PINS_DRIVE_REG_OFFSET, kDriveIdleJ);
  busy_spin_micros(20);

  // Ensure USBCTRL is disabled and RXFIFO is flushed.
  abs_mmio_write32(kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET, 0x0u);
  abs_mmio_write32(kUsbdevBase + USBDEV_FIFO_CTRL_REG_OFFSET,
                   1u << USBDEV_FIFO_CTRL_RX_RST_BIT);
  busy_spin_micros(20);

  // 1. With USBCTRL.enable == 0 and RX_DEPTH == 0, USBSTAT.rx_empty reads 0
  // (contradicting usbdev.hjson resval: "1") because usbdev.sv:268 assigns
  // `hw2reg.usbstat.rx_empty.d = connect_en & ~rx_fifo_rvalid`.
  uint32_t usbstat = abs_mmio_read32(kUsbdevBase + USBDEV_USBSTAT_REG_OFFSET);
  uint32_t rx_depth = (usbstat >> USBDEV_USBSTAT_RX_DEPTH_OFFSET) &
                      USBDEV_USBSTAT_RX_DEPTH_MASK;
  uint32_t rx_empty = (usbstat >> USBDEV_USBSTAT_RX_EMPTY_BIT) & 1u;
  CHECK_EQ(rx_depth, 0u, "Expected RX_DEPTH == 0 after RX_RST");
  CHECK_EQ(rx_empty, 0u,
           "[usbdev.sv:263-268,709-710] Expected USBSTAT.rx_empty == 0 when "
           "USBCTRL.enable == 0");

  // 2. Writing USBCTRL.device_address = 0x2a with USBCTRL.enable = 0 is
  // cleared to 0 because `clr_devaddr_o = ~connect_en_i | link_reset`.
  const uint32_t kTestAddr = 0x2au;
  abs_mmio_write32(kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET,
                   kTestAddr << USBDEV_USBCTRL_DEVICE_ADDRESS_OFFSET);
  busy_spin_micros(10);
  uint32_t usbctrl = abs_mmio_read32(kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET);
  uint32_t devaddr = (usbctrl >> USBDEV_USBCTRL_DEVICE_ADDRESS_OFFSET) &
                     USBDEV_USBCTRL_DEVICE_ADDRESS_MASK;
  CHECK_EQ(devaddr, 0u,
           "[usbdev.sv:263-268,709-710] Expected device_address == 0 when "
           "USBCTRL.enable == 0");

  // 3. Enable USBCTRL.enable = 1 first so connect_en propagates and
  // clr_devaddr_o deasserts, driving USBSTAT.rx_empty == 1, then write
  // USBCTRL.device_address = 0x2a with USBCTRL.enable = 1.
  abs_mmio_write32(kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET,
                   1u << USBDEV_USBCTRL_ENABLE_BIT);
  busy_spin_micros(20);
  usbstat = abs_mmio_read32(kUsbdevBase + USBDEV_USBSTAT_REG_OFFSET);
  rx_empty = (usbstat >> USBDEV_USBSTAT_RX_EMPTY_BIT) & 1u;
  CHECK_EQ(rx_empty, 1u,
           "[usbdev.sv:263-268,709-710] Expected USBSTAT.rx_empty == 1 when "
           "USBCTRL.enable == 1");

  abs_mmio_write32(kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET,
                   (kTestAddr << USBDEV_USBCTRL_DEVICE_ADDRESS_OFFSET) |
                       (1u << USBDEV_USBCTRL_ENABLE_BIT));
  busy_spin_micros(10);
  usbctrl = abs_mmio_read32(kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET);
  devaddr = (usbctrl >> USBDEV_USBCTRL_DEVICE_ADDRESS_OFFSET) &
            USBDEV_USBCTRL_DEVICE_ADDRESS_MASK;
  CHECK_EQ(devaddr, kTestAddr,
           "[usbdev.sv:263-268,709-710] Expected device_address == 0x2a when "
           "USBCTRL.enable == 1");

  // 4. Clearing USBCTRL.enable = 0 clears both device_address -> 0 and
  // USBSTAT.rx_empty -> 0.
  abs_mmio_write32(kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET,
                   kTestAddr << USBDEV_USBCTRL_DEVICE_ADDRESS_OFFSET);
  busy_spin_micros(20);
  usbctrl = abs_mmio_read32(kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET);
  devaddr = (usbctrl >> USBDEV_USBCTRL_DEVICE_ADDRESS_OFFSET) &
            USBDEV_USBCTRL_DEVICE_ADDRESS_MASK;
  usbstat = abs_mmio_read32(kUsbdevBase + USBDEV_USBSTAT_REG_OFFSET);
  rx_empty = (usbstat >> USBDEV_USBSTAT_RX_EMPTY_BIT) & 1u;
  CHECK_EQ(devaddr, 0u,
           "[usbdev.sv:263-268,709-710] Expected device_address cleared to 0 "
           "on disable");
  CHECK_EQ(rx_empty, 0u,
           "[usbdev.sv:263-268,709-710] Expected USBSTAT.rx_empty cleared to 0 "
           "on disable");
}

static void test_usbdev_resume_link_active_no_sof_and_tx_osc(void) {
  LOG_INFO(
      "Testing [usbdev_linkstate.sv:144-200]: resume_link_active -> "
      "LinkActiveNoSOF(5) & "
      "tx_osc_test_mode");

  // Drive idle J state (DP_O = 1, DN_O = 0) via PHY_PINS_DRIVE so bus is not
  // in SE0 reset when enabled.
  const uint32_t kDriveIdleJ = (1u << USBDEV_PHY_PINS_DRIVE_EN_BIT) |
                               (1u << USBDEV_PHY_PINS_DRIVE_OE_O_BIT) |
                               (1u << USBDEV_PHY_PINS_DRIVE_RX_ENABLE_O_BIT) |
                               (1u << USBDEV_PHY_PINS_DRIVE_DP_O_BIT);
  abs_mmio_write32(kUsbdevBase + USBDEV_PHY_CONFIG_REG_OFFSET, 0x0u);
  abs_mmio_write32(kUsbdevBase + USBDEV_PHY_PINS_DRIVE_REG_OFFSET, kDriveIdleJ);
  busy_spin_micros(20);

  // 1. Enable USBCTRL first so connect_en transitions link_state from
  // LinkDisconnected (0) -> LinkPowered (1).
  abs_mmio_write32(kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET,
                   1u << USBDEV_USBCTRL_ENABLE_BIT);
  busy_spin_micros(20);
  uint32_t usbstat = abs_mmio_read32(kUsbdevBase + USBDEV_USBSTAT_REG_OFFSET);
  uint32_t link_state = (usbstat >> USBDEV_USBSTAT_LINK_STATE_OFFSET) &
                        USBDEV_USBSTAT_LINK_STATE_MASK;
  CHECK_EQ(link_state, 1u,
           "[usbdev_linkstate.sv:144-200] Expected link_state == LinkPowered "
           "(1), got %u",
           link_state);

  // 2. Trigger RESUME_LINK_ACTIVE (wo pulse) from LinkPowered (1) ->
  // transitions through LinkResuming (6) and settles in LinkActiveNoSOF (5).
  abs_mmio_write32(kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET,
                   (1u << USBDEV_USBCTRL_ENABLE_BIT) |
                       (1u << USBDEV_USBCTRL_RESUME_LINK_ACTIVE_BIT));
  busy_spin_micros(20);

  usbstat = abs_mmio_read32(kUsbdevBase + USBDEV_USBSTAT_REG_OFFSET);
  link_state = (usbstat >> USBDEV_USBSTAT_LINK_STATE_OFFSET) &
               USBDEV_USBSTAT_LINK_STATE_MASK;
  CHECK_EQ(link_state, 5u,
           "[usbdev_linkstate.sv:144-200] Expected link_state == "
           "LinkActiveNoSOF (5), got %u",
           link_state);

  // Release PHY_PINS_DRIVE override and verify PHY_CONFIG.tx_osc_test_mode
  // forces PHY_PINS_SENSE.tx_oe_o == 1.
  abs_mmio_write32(kUsbdevBase + USBDEV_PHY_PINS_DRIVE_REG_OFFSET, 0x0u);
  abs_mmio_write32(kUsbdevBase + USBDEV_PHY_CONFIG_REG_OFFSET,
                   1u << USBDEV_PHY_CONFIG_TX_OSC_TEST_MODE_BIT);
  busy_spin_micros(20);
  uint32_t pins_sense =
      abs_mmio_read32(kUsbdevBase + USBDEV_PHY_PINS_SENSE_REG_OFFSET);
  CHECK_EQ(
      (pins_sense >> USBDEV_PHY_PINS_SENSE_TX_OE_O_BIT) & 1u, 1u,
      "[usbdev_linkstate.sv:144-200] Expected PHY_PINS_SENSE.tx_oe_o == 1 in "
      "tx_osc_test_mode");

  // Restore defaults.
  abs_mmio_write32(kUsbdevBase + USBDEV_PHY_CONFIG_REG_OFFSET, 0x0u);
  abs_mmio_write32(kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET, 0x0u);
}

static void test_usbdev_buffer_asymmetric_subword_and_csr_faults(void) {
  LOG_INFO(
      "Testing [usbdev.sv:760-763]: BUFFER sub-word read OK / write fault & "
      "CSR TL-UL faults");

  const uint32_t kBufferWord0 = kUsbdevBase + USBDEV_BUFFER_REG_OFFSET;
  const uint32_t kPattern = 0x11223344u;

  // 1. 32-bit word write to USBDEV.BUFFER succeeds.
  g_fault_count = 0;
  abs_mmio_write32(kBufferWord0, kPattern);
  CHECK_EQ(g_fault_count, 0u, "32-bit write to BUFFER must not fault");
  CHECK_EQ(abs_mmio_read32(kBufferWord0), kPattern, "32-bit readback mismatch");

  // 2. Sub-word byte/halfword reads (`lb`/`lbu`/`lh`/`lhu`) from USBDEV.BUFFER
  // succeed (`tlul_err.sv:53` sets `err_o = 0` on reads when `ByteAccess ==
  // 0`).
  uint8_t b0 = abs_mmio_read8(kBufferWord0 + 0u);
  uint8_t b1 = abs_mmio_read8(kBufferWord0 + 1u);
  uint8_t b2 = abs_mmio_read8(kBufferWord0 + 2u);
  uint8_t b3 = abs_mmio_read8(kBufferWord0 + 3u);
  uint16_t h1 = *(const volatile uint16_t *)(uintptr_t)(kBufferWord0 + 2u);
  CHECK_EQ(
      g_fault_count, 0u,
      "[usbdev.sv:760-763] Sub-word reads from USBDEV.BUFFER must not fault");
  CHECK_EQ(b0, 0x44u, "Byte 0 mismatch");
  CHECK_EQ(b1, 0x33u, "Byte 1 mismatch");
  CHECK_EQ(b2, 0x22u, "Byte 2 mismatch");
  CHECK_EQ(b3, 0x11u, "Byte 3 mismatch");
  CHECK_EQ(h1, 0x1122u, "Halfword 1 mismatch");

  // 3. Sub-word writes (`sb`/`sh`) to USBDEV.BUFFER trap with Store Access
  // Fault (`mcause = 7`) and do not modify the SRAM word.
  g_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write8(kBufferWord0, 0xAAu);
  CHECK_EQ(g_fault_count, 1u,
           "[usbdev.sv:760-763] Expected sb to USBDEV.BUFFER to trap");
  CHECK_EQ(g_last_mcause, (uint32_t)kRiscvStoreAccessFault,
           "Expected Store Access Fault (mcause=7) on sb to BUFFER");

  g_fault_count = 0;
  g_last_mcause = 0;
  *(volatile uint16_t *)(uintptr_t)(kBufferWord0 + 2u) = 0xBBCCu;
  CHECK_EQ(g_fault_count, 1u,
           "[usbdev.sv:760-763] Expected sh to USBDEV.BUFFER to trap");
  CHECK_EQ(g_last_mcause, (uint32_t)kRiscvStoreAccessFault,
           "Expected Store Access Fault (mcause=7) on sh to BUFFER");
  CHECK_EQ(
      abs_mmio_read32(kBufferWord0), kPattern,
      "[usbdev.sv:760-763] Rejected sub-word writes must not corrupt BUFFER");

  // 4. Sub-word write (`sb`) to multi-byte CSR (`USBCTRL`, permit `4'b0111`)
  // traps with Store Access Fault (`mcause = 7`).
  g_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write8(kUsbdevBase + USBDEV_USBCTRL_REG_OFFSET, 0x01u);
  CHECK_EQ(g_fault_count, 1u,
           "[usbdev.sv:760-763] Expected sb to USBCTRL to trap");
  CHECK_EQ(g_last_mcause, (uint32_t)kRiscvStoreAccessFault,
           "Expected Store Access Fault (mcause=7) on sb to USBCTRL");

  // 5. Unmapped CSR gap (`0x0AC..0x7FC`) traps with Load/Store Access Fault.
  g_fault_count = 0;
  g_last_mcause = 0;
  (void)abs_mmio_read32(kUsbdevBase + 0x100u);
  CHECK_EQ(g_fault_count, 1u,
           "[usbdev.sv:760-763] Expected read from unmapped gap 0x100 to trap");
  CHECK_EQ(g_last_mcause, (uint32_t)kRiscvLoadAccessFault,
           "Expected Load Access Fault (mcause=5) on gap read");

  g_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write32(kUsbdevBase + 0x100u, 0xDEADBEEFu);
  CHECK_EQ(g_fault_count, 1u,
           "[usbdev.sv:760-763] Expected write to unmapped gap 0x100 to trap");
  CHECK_EQ(g_last_mcause, (uint32_t)kRiscvStoreAccessFault,
           "Expected Store Access Fault (mcause=7) on gap write");
}

bool test_main(void) {
  LOG_INFO("Starting usbdev CW340 FPGA & QEMU errata confirmation test (P17)");

  // Connect UsbdevSense pinmux input to ConstantOne so VBUS SENSE == 1.
  dif_pinmux_t pinmux;
  CHECK_DIF_OK(dif_pinmux_init(mmio_region_from_addr(kPinmuxBase), &pinmux));
  CHECK_DIF_OK(dif_pinmux_input_select(
      &pinmux, kTopEarlgreyPinmuxPeripheralInUsbdevSense,
      kTopEarlgreyPinmuxInselConstantOne));
  busy_spin_micros(20);

  test_usbdev_rx_empty_and_devaddr_gated_by_enable();
  test_usbdev_resume_link_active_no_sof_and_tx_osc();
  test_usbdev_buffer_asymmetric_subword_and_csr_faults();

  LOG_INFO("All usbdev errata checks PASSED");
  return true;
}
