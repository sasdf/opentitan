// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_COVERAGE_RUNTIME_H_
#define OPENTITAN_SW_DEVICE_COVERAGE_RUNTIME_H_

#include <stdint.h>

#include "sw/device/lib/base/macros.h"

#ifdef OT_COVERAGE_ENABLED

void coverage_init(void);
void coverage_report(void);
void coverage_invalidate(void);

#else  // OT_COVERAGE_ENABLED

#define coverage_init(...) do {} while(0)
#define coverage_report(...) do {} while(0)
#define coverage_invalidate(...) do {} while(0)

#endif  // OT_COVERAGE_ENABLED


#endif  // OPENTITAN_SW_DEVICE_COVERAGE_RUNTIME_H_
