// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "uart_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kUart1Base = TOP_EARLGREY_UART1_BASE_ADDR,
};

bool test_main(void) {
  LOG_INFO("Starting ot_uart FPGA/QEMU consistency test on UART1");

  // Reset UART1 control and FIFOs before starting.
  abs_mmio_write32(kUart1Base + UART_CTRL_REG_OFFSET, 0x0u);
  abs_mmio_write32(
      kUart1Base + UART_FIFO_CTRL_REG_OFFSET,
      (1u << UART_FIFO_CTRL_RXRST_BIT) | (1u << UART_FIFO_CTRL_TXRST_BIT));
  abs_mmio_write32(kUart1Base + UART_INTR_STATE_REG_OFFSET, 0x1ffu);

  // 1. Write-only (WDATA, INTR_TEST, ALERT_TEST) readback returns 0, and
  // read-only (STATUS, RDATA, FIFO_STATUS, VAL) writes are ignored without
  // bus fault.
  CHECK(abs_mmio_read32(kUart1Base + UART_WDATA_REG_OFFSET) == 0x0u);
  CHECK(abs_mmio_read32(kUart1Base + UART_INTR_TEST_REG_OFFSET) == 0x0u);
  CHECK(abs_mmio_read32(kUart1Base + UART_ALERT_TEST_REG_OFFSET) == 0x0u);

  abs_mmio_write32(kUart1Base + UART_STATUS_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kUart1Base + UART_RDATA_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kUart1Base + UART_FIFO_STATUS_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kUart1Base + UART_VAL_REG_OFFSET, 0xffffffffu);

  CHECK(abs_mmio_read32(kUart1Base + UART_FIFO_STATUS_REG_OFFSET) == 0x0u);
  uint32_t status = abs_mmio_read32(kUart1Base + UART_STATUS_REG_OFFSET);
  CHECK((status & (1u << UART_STATUS_TXEMPTY_BIT)) != 0u);
  CHECK((status & (1u << UART_STATUS_RXEMPTY_BIT)) != 0u);

  // 2. CTRL architectural bitmask readback (0xffff03f7: bits 3 and 15:10
  // reserved/unimplemented in uart_reg_top.sv).
  abs_mmio_write32(kUart1Base + UART_CTRL_REG_OFFSET, 0xffffffffu);
  CHECK(abs_mmio_read32(kUart1Base + UART_CTRL_REG_OFFSET) == 0xffff03f7u);

  // Restore CTRL = 0 (TX = 0, RX = 0) and reset TX/RX FIFOs.
  abs_mmio_write32(kUart1Base + UART_CTRL_REG_OFFSET, 0x0u);
  abs_mmio_write32(
      kUart1Base + UART_FIFO_CTRL_REG_OFFSET,
      (1u << UART_FIFO_CTRL_RXRST_BIT) | (1u << UART_FIFO_CTRL_TXRST_BIT));
  abs_mmio_write32(kUart1Base + UART_INTR_STATE_REG_OFFSET, 0x1ffu);

  // 3. 32-byte TX FIFO full (TxFifoDepth = 32 in uart_reg_pkg.sv:
  // STATUS.TXFULL == 1, FIFO_STATUS.TXLVL == 32) and 33rd byte overflow drop
  // while CTRL.TX == 0.
  for (uint32_t i = 0; i < 32; ++i) {
    abs_mmio_write32(kUart1Base + UART_WDATA_REG_OFFSET, (i ^ 0x5au) & 0xffu);
  }

  uint32_t fifo_status =
      abs_mmio_read32(kUart1Base + UART_FIFO_STATUS_REG_OFFSET);
  uint32_t txlvl = (fifo_status >> UART_FIFO_STATUS_TXLVL_OFFSET) &
                   UART_FIFO_STATUS_TXLVL_MASK;
  CHECK(txlvl == 32u);
  status = abs_mmio_read32(kUart1Base + UART_STATUS_REG_OFFSET);
  CHECK((status & (1u << UART_STATUS_TXFULL_BIT)) != 0u);
  CHECK((status & (1u << UART_STATUS_TXEMPTY_BIT)) == 0u);

  // Write 33rd byte while TXFULL == 1; must be dropped without altering TXLVL.
  abs_mmio_write32(kUart1Base + UART_WDATA_REG_OFFSET, 0xffu);
  fifo_status = abs_mmio_read32(kUart1Base + UART_FIFO_STATUS_REG_OFFSET);
  txlvl = (fifo_status >> UART_FIFO_STATUS_TXLVL_OFFSET) &
          UART_FIFO_STATUS_TXLVL_MASK;
  CHECK(txlvl == 32u);
  status = abs_mmio_read32(kUart1Base + UART_STATUS_REG_OFFSET);
  CHECK((status & (1u << UART_STATUS_TXFULL_BIT)) != 0u);

  // 4. Enable UART1 system loopback at maximum baud rate (NCO = 0xffff) so the
  // first 32 bytes drain into the 64-byte RX FIFO (RxFifoDepth = 64 in
  // uart_reg_pkg.sv), then send 32 more bytes (32..63) to fill RX FIFO to 64.
  const uint32_t kCtrlLoopback =
      (0xffffu << UART_CTRL_NCO_OFFSET) | (1u << UART_CTRL_SLPBK_BIT) |
      (1u << UART_CTRL_TX_BIT) | (1u << UART_CTRL_RX_BIT);
  abs_mmio_write32(kUart1Base + UART_CTRL_REG_OFFSET, kCtrlLoopback);

  while ((abs_mmio_read32(kUart1Base + UART_STATUS_REG_OFFSET) &
          (1u << UART_STATUS_TXIDLE_BIT)) == 0u) {
  }
  for (uint32_t i = 32; i < 64; ++i) {
    abs_mmio_write32(kUart1Base + UART_WDATA_REG_OFFSET, (i ^ 0x5au) & 0xffu);
  }

  // Poll STATUS until TXIDLE (bit 3) and RXFULL (bit 1) both assert.
  const uint32_t kWaitMask =
      (1u << UART_STATUS_TXIDLE_BIT) | (1u << UART_STATUS_RXFULL_BIT);
  while ((abs_mmio_read32(kUart1Base + UART_STATUS_REG_OFFSET) & kWaitMask) !=
         kWaitMask) {
  }

  status = abs_mmio_read32(kUart1Base + UART_STATUS_REG_OFFSET);
  CHECK((status & (1u << UART_STATUS_RXFULL_BIT)) != 0u);
  CHECK((status & (1u << UART_STATUS_RXEMPTY_BIT)) == 0u);

  fifo_status = abs_mmio_read32(kUart1Base + UART_FIFO_STATUS_REG_OFFSET);
  txlvl = (fifo_status >> UART_FIFO_STATUS_TXLVL_OFFSET) &
          UART_FIFO_STATUS_TXLVL_MASK;
  uint32_t rxlvl = (fifo_status >> UART_FIFO_STATUS_RXLVL_OFFSET) &
                   UART_FIFO_STATUS_RXLVL_MASK;
  CHECK(txlvl == 0u);
  CHECK(rxlvl == 64u);
  CHECK((abs_mmio_read32(kUart1Base + UART_INTR_STATE_REG_OFFSET) &
         (1u << UART_INTR_STATE_RX_OVERFLOW_BIT)) == 0u);

  // Transmit 65th byte (0xee) while RXFULL == 1 to trigger RX_OVERFLOW.
  abs_mmio_write32(kUart1Base + UART_WDATA_REG_OFFSET, 0xeeu);
  while (((abs_mmio_read32(kUart1Base + UART_STATUS_REG_OFFSET) &
           (1u << UART_STATUS_TXIDLE_BIT)) == 0u) ||
         ((abs_mmio_read32(kUart1Base + UART_INTR_STATE_REG_OFFSET) &
           (1u << UART_INTR_STATE_RX_OVERFLOW_BIT)) == 0u)) {
  }

  CHECK((abs_mmio_read32(kUart1Base + UART_INTR_STATE_REG_OFFSET) &
         (1u << UART_INTR_STATE_RX_OVERFLOW_BIT)) != 0u);
  status = abs_mmio_read32(kUart1Base + UART_STATUS_REG_OFFSET);
  CHECK((status & (1u << UART_STATUS_RXFULL_BIT)) != 0u);
  fifo_status = abs_mmio_read32(kUart1Base + UART_FIFO_STATUS_REG_OFFSET);
  rxlvl = (fifo_status >> UART_FIFO_STATUS_RXLVL_OFFSET) &
          UART_FIFO_STATUS_RXLVL_MASK;
  CHECK(rxlvl == 64u);

  // Read all 64 bytes from RDATA and verify FIFO data integrity.
  for (uint32_t i = 0; i < 64; ++i) {
    uint32_t rdata =
        abs_mmio_read32(kUart1Base + UART_RDATA_REG_OFFSET) & 0xffu;
    CHECK(rdata == ((i ^ 0x5au) & 0xffu));
  }

  status = abs_mmio_read32(kUart1Base + UART_STATUS_REG_OFFSET);
  CHECK((status & (1u << UART_STATUS_RXEMPTY_BIT)) != 0u);
  CHECK((status & (1u << UART_STATUS_RXFULL_BIT)) == 0u);
  CHECK(abs_mmio_read32(kUart1Base + UART_FIFO_STATUS_REG_OFFSET) == 0x0u);

  // Clean up UART1 state.
  abs_mmio_write32(kUart1Base + UART_CTRL_REG_OFFSET, 0x0u);
  abs_mmio_write32(kUart1Base + UART_INTR_STATE_REG_OFFSET, 0x1ffu);

  LOG_INFO("ot_uart FPGA/QEMU consistency test passed");
  return true;
}
