// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_COVERAGE_RUNTIME_ASM_H_
#define OPENTITAN_SW_DEVICE_COVERAGE_RUNTIME_ASM_H_

#ifdef OT_COVERAGE_INSTRUMENTED

#define COVERAGE_ASM_INIT() call coverage_init;

#define COVERAGE_ASM_TRANSPORT_INIT() call coverage_transport_init;

#define COVERAGE_ASM_BACKUP_COUNTERS(kReg0, kReg1) \
  li a0, 0;                                        \
  call coverage_backup_asm_counters;               \
  mv kReg0, a0;                                    \
  li a0, 32;                                       \
  call coverage_backup_asm_counters;               \
  mv kReg1, a0;

#define COVERAGE_ASM_RESTORE_COUNTERS(kReg0, kReg1) \
  mv a0, kReg0;                                     \
  mv a1, kReg1;                                     \
  call coverage_restore_asm_counters;

#define COVERAGE_ASM_REPORT() call coverage_report;

#define COVERAGE_ASM_AUTOGEN_MARK(kTemp, kIndex) \
  lui kTemp, % hi(.L__asm_profc + kIndex);       \
  sb zero, % lo(.L__asm_profc + kIndex)(kTemp)

#else  // OT_COVERAGE_INSTRUMENTED

#define COVERAGE_ASM_INIT(...)
#define COVERAGE_ASM_TRANSPORT_INIT(...)
#define COVERAGE_ASM_BACKUP_COUNTERS(...)
#define COVERAGE_ASM_RESTORE_COUNTERS(...)
#define COVERAGE_ASM_REPORT(...)
#define COVERAGE_ASM_AUTOGEN_MARK(...)

#endif  // OT_COVERAGE_INSTRUMENTED

#define COVERAGE_ASM_MANUAL_MARK COVERAGE_ASM_AUTOGEN_MARK

#endif  // OPENTITAN_SW_DEVICE_COVERAGE_RUNTIME_ASM_H_
