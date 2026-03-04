// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_LIB_COVERAGE_SMOKE_TARGET_H_
#define OPENTITAN_SW_DEVICE_LIB_COVERAGE_SMOKE_TARGET_H_

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

void covfunc_covered(void);

static inline void inline_covfunc_covered(void) {}

void covfunc_uncovered(void);

static inline void inline_covfunc_uncovered(void) {}

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // OPENTITAN_SW_DEVICE_LIB_COVERAGE_SMOKE_TARGET_H_
