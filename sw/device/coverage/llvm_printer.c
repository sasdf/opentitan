// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdint.h>

#include "external/llvm_compiler_rt/lib/profile/InstrProfiling.h"
#include "external/llvm_compiler_rt/lib/profile/InstrProfilingInternal.h"
#include "sw/device/coverage/compress.h"
#include "sw/device/coverage/printer.h"
#include "sw/device/lib/base/macros.h"

extern char _init_array_start[];
extern char _init_array_end[];

void coverage_printer_contents(void) {
  void (**func)(void) = (void (**)(void))_init_array_start;
  void (**func_end)(void) = (void (**)(void))_init_array_end;
  for (; func < func_end; ++func) {
    (*func)();
  }

  __llvm_profile_write_buffer(NULL);
}

uint32_t lprofBufferWriter(ProfDataWriter *This, ProfDataIOVec *IOVecs,
                           uint32_t NumIOVecs) {
  OT_DISCARD(This);
  for (size_t i = 0; i < NumIOVecs; i++) {
    size_t len = IOVecs[i].ElmSize * IOVecs[i].NumElm;
    unsigned char *data = (unsigned char *)IOVecs[i].Data;
    if (data) {
      coverage_compress(data, len);
    } else if (IOVecs[i].UseZeroPadding) {
      coverage_compress_zeros(0x00, len);
    }
  }
  return 0;
}

void coverage_printer_init(void) { coverage_printer_init_cnts(); }
