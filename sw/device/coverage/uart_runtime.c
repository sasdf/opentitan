// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/coverage/printer.h"
#include "sw/device/lib/base/macros.h"
#include "sw/device/silicon_creator/lib/drivers/uart.h"

enum {
    kCoverageNotDumped = 0x19285721,
};

static uint32_t coverage_status;


OT_NO_COVERAGE
void coverage_init(void) {
  // python3 sw/device/coverage/uart_hex.py 'COVERAGE:UART\r\n'
  uart_write_imm(0x4547415245564f43);
  uart_write_imm(0x000a0d545241553a);

  coverage_printer_init();

  coverage_status = kCoverageNotDumped;
}

OT_NO_COVERAGE
void coverage_report(void) {
  // Wait until idle.
  while (!uart_tx_idle());

#if defined(OT_COVERAGE_INSTRUMENTED)

  if (coverage_status != kCoverageNotDumped) {
    // python3 sw/device/coverage/uart_hex.py '== COVERAGE PROFILE DUMPED ==\r\n'
    uart_write_imm(0x5245564f43203d3d);
    uart_write_imm(0x464f525020454741);
    uart_write_imm(0x504d554420454c49);
    uart_write_imm(0x000a0d3d3d204445);
  } else {
    coverage_status = 0;

    // python3 sw/device/coverage/uart_hex.py '== COVERAGE PROFILE START ==\r\n'
    uart_write_imm(0x5245564f43203d3d);
    uart_write_imm(0x464f525020454741);
    uart_write_imm(0x5241545320454c49);
    uart_write_imm(0x00000a0d3d3d2054);

    coverage_printer_run();

    // python3 sw/device/coverage/uart_hex.py '== COVERAGE PROFILE END ==\r\n'
    uart_write_imm(0x5245564f43203d3d);
    uart_write_imm(0x464f525020454741);
    uart_write_imm(0x20444e4520454c49);
    uart_write_imm(0x000000000a0d3d3d);
  }

#elif defined(OT_COVERAGE_ENABLED)

  // python3 sw/device/coverage/uart_hex.py '== COVERAGE PROFILE SKIP ==\r\n'
  uart_write_imm(0x5245564f43203d3d);
  uart_write_imm(0x464f525020454741);
  uart_write_imm(0x50494b5320454c49);
  uart_write_imm(0x0000000a0d3d3d20);

#endif

  // Wait until the report is sent.
  while (!uart_tx_idle());
}

OT_NO_COVERAGE
void coverage_printer_sink(const void *data, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    uart_write_hex(((uint8_t *)data)[i], 1, 0);
  }
}
