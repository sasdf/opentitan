// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_COVERAGE_COMPRESS_H_
#define OPENTITAN_SW_DEVICE_COVERAGE_COMPRESS_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

void coverage_compress_zeros(uint8_t tag, uint32_t size);

void coverage_compress(unsigned char *data, size_t size);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // OPENTITAN_SW_DEVICE_COVERAGE_COMPRESS_H_
