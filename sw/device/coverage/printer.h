// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_COVERAGE_PRINTER_H_
#define OPENTITAN_SW_DEVICE_COVERAGE_PRINTER_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// console sink provided by the actual runtime.
extern void coverage_printer_sink(const void *data, size_t size);

// Constructs the profile report and call coverage_printer_sink.
void coverage_printer_run(void);

void coverage_printer_init(void);

// Check if the report has been invalidated.
int coverage_is_valid(void);

/* Internal APIs */
void coverage_printer_sink_with_crc(const void *data, size_t size);

void coverage_printer_contents(void);

void coverage_printer_init_cnts(void);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // OPENTITAN_SW_DEVICE_COVERAGE_PRINTER_H_
