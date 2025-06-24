// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_COVERAGE_RUNTIME_ASM_H_
#define OPENTITAN_SW_DEVICE_COVERAGE_RUNTIME_ASM_H_

#ifdef OT_COVERAGE_INSTRUMENTED


#define COVERAGE_ASM_COUNTER_INIT() \
  li s10, 0; \
  li s11, 0;

#define COVERAGE_ASM_MARK_MANUAL_REG(kReg, kOffset) \
  li s9, (1<<kOffset); \
  or kReg, kReg, s9;

#define COVERAGE_ASM_MARK_MANUAL_PRF(kTemp, kIndex) \
  lui kTemp, %hi(_prf_cnts_asm+kIndex); \
  sb zero, %lo(_prf_cnts_asm+kIndex)(kTemp)

#define COVERAGE_ASM_MARK_AUTOGEN_REG(kReg, kOffset) \
  COVERAGE_ASM_MARK_MANUAL_REG(kReg, kOffset)

#define COVERAGE_ASM_MARK_AUTOGEN_PRF(kTemp, kIndex) \
  COVERAGE_ASM_MARK_MANUAL_PRF(kTemp, kIndex)

#else // OT_COVERAGE_INSTRUMENTED

#define COVERAGE_ASM_COUNTER_INIT(...)
#define COVERAGE_ASM_MARK_MANUAL_PRF(...)
#define COVERAGE_ASM_MARK_AUTOGEN_PRF(...)
#define COVERAGE_ASM_MARK_MANUAL_REG(...)
#define COVERAGE_ASM_MARK_AUTOGEN_REG(...)

#endif // OT_COVERAGE_INSTRUMENTED

#endif  // OPENTITAN_SW_DEVICE_COVERAGE_RUNTIME_ASM_H_
