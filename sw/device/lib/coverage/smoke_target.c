// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/coverage/smoke_target.h"

void covfunc_nested_covered(void) {}

void covfunc_covered(void) { covfunc_nested_covered(); }

void covfunc_nested_uncovered(void) {}

void covfunc_uncovered(void) { covfunc_nested_uncovered(); }
