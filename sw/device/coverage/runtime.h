// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_COVERAGE_RUNTIME_H_
#define OPENTITAN_SW_DEVICE_COVERAGE_RUNTIME_H_

#include "sw/device/lib/base/macros.h"

#ifdef OT_COVERAGE_ENABLED

void coverage_init(void);
void coverage_report(void);
#define COVERAGE_REPORT coverage_report

#else  // OT_COVERAGE_ENABLED

#define coverage_init(...)
#define coverage_report(...)
#define COVERAGE_REPORT(...)

#endif  // OT_COVERAGE_ENABLED

#endif  // OPENTITAN_SW_DEVICE_COVERAGE_RUNTIME_H_
