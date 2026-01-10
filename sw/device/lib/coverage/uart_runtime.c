// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/arch/device.h"
#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/coverage/printer.h"
#include "sw/device/silicon_creator/lib/dbg_print.h"
#include "sw/device/silicon_creator/lib/drivers/pinmux.h"
#include "sw/device/silicon_creator/lib/drivers/uart.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "pinmux_regs.h"

static void _dbg_puts(const char *str) {
  while (*str) {
    uart_putchar(*str++);
  }
}

static void _pinmux_init_uart0_tx(void) {
  abs_mmio_write32(TOP_EARLGREY_PINMUX_AON_BASE_ADDR +
                       PINMUX_MIO_OUTSEL_0_REG_OFFSET +
                       kTopEarlgreyPinmuxMioOutIoc4 * sizeof(uint32_t),
                   kTopEarlgreyPinmuxOutselUart0Tx);
}

void coverage_transport_init(void) {
  _pinmux_init_uart0_tx();
  uart_init(kUartNCOValue);

  _dbg_puts("COVERAGE:UART\r\n");
  while (!uart_tx_idle()) {
  }
}

void coverage_report(void) {
  if (coverage_is_valid()) {
    _dbg_puts("== COVERAGE PROFILE START ==\r\n");
    coverage_printer_run();
    _dbg_puts("== COVERAGE PROFILE END ==\r\n");
  } else {
    _dbg_puts("== COVERAGE PROFILE INVALID ==\r\n");
  }

  // Wait until the report is sent.
  while (!uart_tx_idle()) {
  }
}

void coverage_printer_sink(const void *data, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    uart_write_hex(((uint8_t *)data)[i], 1, 0);
  }
}
