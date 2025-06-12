// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdint.h>

#include "sw/device/coverage/printer.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/runtime/print.h"
#include "sw/device/lib/base/macros.h"


OT_NO_COVERAGE
void coverage_init(void) {
  base_printf("COVERAGE:OTTF\r\n");

  coverage_printer_init();
}

/**
 * Sends the given buffer as a hex string over dif console.
 */
OT_NO_COVERAGE
void coverage_report(void) {
#if defined(OT_COVERAGE_INSTRUMENTED)

  if (coverage_is_valid()) {
    base_printf("== COVERAGE PROFILE START ==\r\n");
    coverage_printer_run();
    base_printf("== COVERAGE PROFILE END ==\r\n");
  } else {
    base_printf("== COVERAGE PROFILE DUMPED ==\r\n");
  }

#elif defined(OT_COVERAGE_ENABLED)

  base_printf("== COVERAGE PROFILE SKIP ==\r\n");

#endif

}

OT_NO_COVERAGE
void coverage_printer_sink(const void *data, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    base_printf("%02x", ((uint8_t *)data)[i]);
  }
}
