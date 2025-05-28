// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_COVERAGE_RUNTIME_H_
#define OPENTITAN_SW_DEVICE_COVERAGE_RUNTIME_H_

#ifdef OT_COVERAGE_INSTRUMENTED

void coverage_init(void);
void coverage_report(void);
#define COVERAGE_REPORT coverage_report

#else  // OT_COVERAGE_INSTRUMENTED

#define coverage_init(...)
#define coverage_report(...)
#define COVERAGE_REPORT(...)

#endif  // OT_COVERAGE_INSTRUMENTED

#endif  // OPENTITAN_SW_DEVICE_COVERAGE_RUNTIME_H_
