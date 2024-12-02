// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stddef.h>
#include <stdint.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  int a = 0;
  if (size > 42 && data[0] == 42) {
    a += 1;
  }
  (void)a;

  return 0;  // Values other than 0 and -1 are reserved for future use.
}
