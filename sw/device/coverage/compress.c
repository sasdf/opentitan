
// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdint.h>

#include "sw/device/coverage/printer.h"

void coverage_compress_zeros(uint8_t tag, uint32_t size) {
  uint32_t buf[2] = {0, size};
  if (size <= 0xfd) {
    // 00XX
    // [tag][size]
    buf[0] = 0x00000000 | ((uint32_t)tag << 24);
    coverage_printer_sink_with_crc((uint8_t *)buf + 3, 2);
  } else if (size <= 0xffff) {
    // 00feXXXX
    // [tag][fe][size]
    buf[0] = 0xfe000000 | ((uint32_t)tag << 16);
    coverage_printer_sink_with_crc((uint8_t *)buf + 2, 4);
  } else {
    // 00ffXXXXXXXX
    // [tag][fe][size]
    buf[0] = 0xff000000 | ((uint32_t)tag << 16);
    coverage_printer_sink_with_crc((uint8_t *)buf + 2, 6);
  }
}

void coverage_compress(unsigned char *data, size_t size) {
  size_t i = 0;
  while (i < size) {
    // Non-zero span
    {
      size_t start = i;
      while (i < size && data[i] != 0 && data[i] != 0xff)
        i++;
      if (i > start) {
        coverage_printer_sink_with_crc(&data[start], i - start);
      }
    }

    // FF span
    {
      size_t start = i;
      while (i < size && data[i] == 0xff)
        i++;
      if (i > start) {
        coverage_compress_zeros(0xff, (uint32_t)(i - start));
      }
    }

    // Zero span
    {
      size_t start = i;
      while (i < size && data[i] == 0)
        i++;
      if (i > start) {
        coverage_compress_zeros(0x00, (uint32_t)(i - start));
      }
    }
  }
}
