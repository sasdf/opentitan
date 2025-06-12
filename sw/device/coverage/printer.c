// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/coverage/printer.h"

#include <stdint.h>

#include "sw/device/coverage/compress.h"
#include "sw/device/lib/base/crc32.h"

#define BUILD_ID_SIZE 20
#define VARIANT_MASK_BYTE_COVERAGE (1ULL << 60)

/**
 * When the linker finds a definition of this symbol, it knows to skip loading
 * the object which contains the profiling runtime's static initializer. See
 * https://clang.llvm.org/docs/SourceBasedCodeCoverage.html#using-the-profiling-runtime-without-static-initializers
 * for more information.
 */
int __llvm_profile_runtime;

static uint32_t coverage_status;

uint32_t coverage_crc;

static char send_buf[0x100];

void coverage_printer_sink_with_crc(const void *buf, size_t size) {
  const uint8_t *ptr = (const uint8_t *)buf;
  while (size) {
    size_t chunk_size = size > sizeof(send_buf) ? sizeof(send_buf) : size;
    for (int i = 0; i < chunk_size; ++i) {
      send_buf[i] = ptr[i];
    }
    crc32_add(&coverage_crc, send_buf, chunk_size);
    coverage_printer_sink(send_buf, chunk_size);
    size -= chunk_size;
    ptr += chunk_size;
  }
}

extern char __llvm_prf_cnts_start[];
extern char __llvm_prf_cnts_end[];
extern char __llvm_prf_cnts_values_end[];
extern char _build_id_start[];
extern char _build_id_end[];

#define BUILD_ID ((uint8_t*)_build_id_end - BUILD_ID_SIZE)

// The variable is defined as weak so that compiler can emit an override.
// See also LLVM's `compiler-rt/lib/profile/InstrProfiling.h`.
__attribute__((weak))
const uint64_t __llvm_profile_raw_version = 8;
extern const uint64_t __llvm_profile_raw_version;

void coverage_printer_init_cnts(void) {
  uint32_t *ptr = (uint32_t *)__llvm_prf_cnts_start;
  while (ptr < (uint32_t *)__llvm_prf_cnts_end) {
    if (__llvm_profile_raw_version & VARIANT_MASK_BYTE_COVERAGE) {
      *ptr++ = 0xffffffff;
    } else {
      *ptr++ = 0x00000000;
    }
  }

  // Set the report as valid.
  coverage_status = *(uint32_t*) BUILD_ID;

  // Dry run the crc to bump their counters.
  // crc32_add(&coverage_crc, send_buf, 1);
  // crc32_add(&coverage_crc, send_buf, 4);
}

void coverage_printer_run(void) {
  crc32_init(&coverage_crc);


  if (_build_id_end - _build_id_start >= BUILD_ID_SIZE) {
    coverage_compress((unsigned char *)BUILD_ID, BUILD_ID_SIZE);
  } else {
    coverage_compress_zeros(0x00, BUILD_ID_SIZE);
  }

  coverage_printer_contents();

  coverage_crc = crc32_finish(&coverage_crc);
  coverage_printer_sink(&coverage_crc, sizeof(coverage_crc));
}

void coverage_invalidate(void) {
  coverage_status = 0x42;
}

int coverage_is_valid(void) {
  return coverage_status == *(uint32_t*) BUILD_ID;
}
