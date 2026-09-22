// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "spi_device_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kSpiDeviceBase = TOP_EARLGREY_SPI_DEVICE_BASE_ADDR,
};

static volatile bool g_expect_load_fault = false;
static volatile uint32_t g_load_fault_count = 0;
static volatile uint32_t g_last_mcause = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  if (g_expect_load_fault) {
    g_last_mcause = ibex_mcause_read();
    ++g_load_fault_count;
    return;
  }
  ottf_generic_fault_print(exc_info, "Unhandled Load/Store Fault",
                           ibex_mcause_read());
  abort();
}

bool test_main(void) {
  // ---------------------------------------------------------------------------
  // 1. CONTROL.FLASH_READ_BUFFER_CLR & CONTROL.MODE transitions
  //    (ot_spi_device.c Chunks 36, 38, 40)
  // ---------------------------------------------------------------------------
  const uint32_t kFlashModeAndClr =
      bitfield_field32_write(0u, SPI_DEVICE_CONTROL_MODE_FIELD,
                             SPI_DEVICE_CONTROL_MODE_VALUE_FLASHMODE) |
      bitfield_bit32_write(0u, SPI_DEVICE_CONTROL_FLASH_READ_BUFFER_CLR_BIT,
                           true) |
      bitfield_bit32_write(0u, SPI_DEVICE_CONTROL_FLASH_STATUS_FIFO_CLR_BIT,
                           true);
  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_CONTROL_REG_OFFSET,
                   kFlashModeAndClr);
  uint32_t ctrl =
      abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_CONTROL_REG_OFFSET);
  CHECK(ctrl == bitfield_field32_write(0u, SPI_DEVICE_CONTROL_MODE_FIELD,
                                       SPI_DEVICE_CONTROL_MODE_VALUE_FLASHMODE),
        "CONTROL readback in FlashMode mismatch: got 0x%x", ctrl);

  // Transition to Passthrough mode, then to mode 0 (FwMode / disabled).
  abs_mmio_write32(
      kSpiDeviceBase + SPI_DEVICE_CONTROL_REG_OFFSET,
      bitfield_field32_write(0u, SPI_DEVICE_CONTROL_MODE_FIELD,
                             SPI_DEVICE_CONTROL_MODE_VALUE_PASSTHROUGH));
  CHECK(abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_CONTROL_REG_OFFSET) ==
            bitfield_field32_write(0u, SPI_DEVICE_CONTROL_MODE_FIELD,
                                   SPI_DEVICE_CONTROL_MODE_VALUE_PASSTHROUGH),
        "CONTROL readback in Passthrough mismatch");

  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_CONTROL_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_CONTROL_REG_OFFSET) == 0u,
        "CONTROL readback in mode 0 mismatch");

  // Restore FlashMode and test READ_THRESHOLD (Chunk 41).
  abs_mmio_write32(
      kSpiDeviceBase + SPI_DEVICE_CONTROL_REG_OFFSET,
      bitfield_field32_write(0u, SPI_DEVICE_CONTROL_MODE_FIELD,
                             SPI_DEVICE_CONTROL_MODE_VALUE_FLASHMODE));
  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_READ_THRESHOLD_REG_OFFSET,
                   0xffffffffu);
  CHECK(abs_mmio_read32(kSpiDeviceBase +
                        SPI_DEVICE_READ_THRESHOLD_REG_OFFSET) == 0x3ffu,
        "READ_THRESHOLD 10-bit mask mismatch");
  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_READ_THRESHOLD_REG_OFFSET, 0u);

  // ---------------------------------------------------------------------------
  // 2. STATUS CSB & TPM_CSB polling & INTR_STATE / LAST_READ_ADDR /
  //    UPLOAD_STATUS reads (ot_spi_device.c Chunks 27, 28, 29, 30)
  // ---------------------------------------------------------------------------
  const uint32_t kPinmuxTpmCsbInselAddr =
      TOP_EARLGREY_PINMUX_AON_BASE_ADDR + 0xe8u +
      (uint32_t)kTopEarlgreyPinmuxPeripheralInSpiDeviceTpmCsb * 4u;

  // At reset, PINMUX_MIO_PERIPH_INSEL_31 == 0 (ConstantZero), so TPM_CSB == 0
  // and CSB == 1 (0x20).
  uint32_t st = abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_STATUS_REG_OFFSET);
  CHECK(st == (1u << SPI_DEVICE_STATUS_CSB_BIT),
        "Expected CSB=1, TPM_CSB=0 (0x20) when PINMUX INSEL=ConstantZero, "
        "got 0x%x",
        st);

  // Set PINMUX_MIO_PERIPH_INSEL_31 = 1 (ConstantOne) so TPM_CSB == 1 and
  // CSB == 1 (0x60).
  abs_mmio_write32(kPinmuxTpmCsbInselAddr, kTopEarlgreyPinmuxInselConstantOne);
  const uint32_t kExpectedIdleStatus =
      (1u << SPI_DEVICE_STATUS_CSB_BIT) | (1u << SPI_DEVICE_STATUS_TPM_CSB_BIT);

  // Sequence A: Read STATUS (enables csb_poll_enabled), then read INTR_STATE.
  st = abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_STATUS_REG_OFFSET);
  CHECK(st == kExpectedIdleStatus,
        "Expected CSB=1 and TPM_CSB=1 (0x60) in STATUS, got 0x%x", st);
  CHECK(
      abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_INTR_STATE_REG_OFFSET) == 0u,
      "Expected INTR_STATE == 0");

  // Sequence B: Read STATUS, then read LAST_READ_ADDR.
  st = abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_STATUS_REG_OFFSET);
  CHECK(st == kExpectedIdleStatus,
        "Expected CSB=1 and TPM_CSB=1 (0x60) in STATUS, got 0x%x", st);
  CHECK(abs_mmio_read32(kSpiDeviceBase +
                        SPI_DEVICE_LAST_READ_ADDR_REG_OFFSET) == 0u,
        "Expected LAST_READ_ADDR == 0");

  // Sequence C: Read STATUS, then read UPLOAD_STATUS.
  st = abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_STATUS_REG_OFFSET);
  CHECK(st == kExpectedIdleStatus,
        "Expected CSB=1 and TPM_CSB=1 (0x60) in STATUS, got 0x%x", st);
  CHECK(abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_UPLOAD_STATUS_REG_OFFSET) ==
            0u,
        "Expected UPLOAD_STATUS == 0");

  // ---------------------------------------------------------------------------
  // 3. Empty UPLOAD_CMDFIFO / UPLOAD_ADDRFIFO reads, W/O reads, and R/O writes
  //    (ot_spi_device.c Chunks 31, 32, 33, 43, 45)
  // ---------------------------------------------------------------------------
  CHECK(abs_mmio_read32(kSpiDeviceBase +
                        SPI_DEVICE_UPLOAD_CMDFIFO_REG_OFFSET) == 0u,
        "Empty UPLOAD_CMDFIFO read must return 0");
  CHECK(abs_mmio_read32(kSpiDeviceBase +
                        SPI_DEVICE_UPLOAD_ADDRFIFO_REG_OFFSET) == 0u,
        "Empty UPLOAD_ADDRFIFO read must return 0");

  // W/O registers (INTR_TEST, ALERT_TEST, TPM_READ_FIFO) read back as 0.
  CHECK(abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_INTR_TEST_REG_OFFSET) == 0u,
        "W/O INTR_TEST must read as 0");
  CHECK(
      abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_ALERT_TEST_REG_OFFSET) == 0u,
      "W/O ALERT_TEST must read as 0");
  CHECK(abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_TPM_READ_FIFO_REG_OFFSET) ==
            0u,
        "W/O TPM_READ_FIFO must read as 0");

  // R/O registers (STATUS, LAST_READ_ADDR, UPLOAD_CMDFIFO, TPM_CAP,
  // TPM_CMD_ADDR) ignore writes.
  uint32_t tpm_cap_orig =
      abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_TPM_CAP_REG_OFFSET);
  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_STATUS_REG_OFFSET, 0u);
  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_LAST_READ_ADDR_REG_OFFSET,
                   0xdeadbeefu);
  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_UPLOAD_CMDFIFO_REG_OFFSET,
                   0xffffu);
  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_TPM_CAP_REG_OFFSET, 0u);
  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_TPM_CMD_ADDR_REG_OFFSET,
                   0xdeadbeefu);

  CHECK(abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_STATUS_REG_OFFSET) ==
            kExpectedIdleStatus,
        "R/O STATUS write must be ignored");
  abs_mmio_write32(kPinmuxTpmCsbInselAddr, kTopEarlgreyPinmuxInselConstantZero);
  CHECK(abs_mmio_read32(kSpiDeviceBase +
                        SPI_DEVICE_LAST_READ_ADDR_REG_OFFSET) == 0u,
        "R/O LAST_READ_ADDR write must be ignored");
  CHECK(abs_mmio_read32(kSpiDeviceBase +
                        SPI_DEVICE_UPLOAD_CMDFIFO_REG_OFFSET) == 0u,
        "R/O UPLOAD_CMDFIFO write must be ignored");
  CHECK(abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_TPM_CAP_REG_OFFSET) ==
            tpm_cap_orig,
        "R/O TPM_CAP write must be ignored");
  CHECK(abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_TPM_CMD_ADDR_REG_OFFSET) ==
            0u,
        "R/O TPM_CMD_ADDR write must be ignored");

  // ---------------------------------------------------------------------------
  // 4. Egress SRAM Buffer Read Fault (ErrOnRead = 1 -> mcause == 5)
  //    (ot_spi_device.c Chunk 46 vs spi_device.sv u_tlul2sram_egress)
  // ---------------------------------------------------------------------------
  g_load_fault_count = 0;
  g_last_mcause = 0;
  g_expect_load_fault = true;
  (void)abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_EGRESS_BUFFER_REG_OFFSET);
  g_expect_load_fault = false;
  CHECK(g_load_fault_count == 1u && g_last_mcause == 5u,
        "Reading egress buffer (0x1000) must raise Load Access Fault "
        "(mcause=5), got count=%u mcause=%u",
        g_load_fault_count, g_last_mcause);

  return true;
}
