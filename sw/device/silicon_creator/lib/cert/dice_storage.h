// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_SILICON_CREATOR_LIB_CERT_DICE_STORAGE_H_
#define OPENTITAN_SW_DEVICE_SILICON_CREATOR_LIB_CERT_DICE_STORAGE_H_

#include "sw/device/silicon_creator/lib/drivers/flash_ctrl.h"
#include "sw/device/silicon_creator/manuf/base/perso_tlv_data.h"

#ifdef __cplusplus
extern "C" {
#endif


typedef struct __attribute__((packed)) dice_cert_header {
  uint16_t object_header;  // Big Endian
  uint16_t cert_header;    // Big Endian
  char name[12];           // "CDI_0" or "CDI_1", padded with 0
} dice_cert_header_t;

typedef struct dice_cert_layout {
  const flash_ctrl_info_page_t *info_page;
  uint32_t page_offset;
  dice_cert_header_t header;  // Template
  uint8_t header_size;
  uint16_t data_size;
} dice_cert_layout_t;

#define DICE_CERT_LAYOUT(name_str, info_page_ptr, offset_val, slot_size_val,   \
                         type_val)                                             \
  {                                                                            \
    .info_page = info_page_ptr, .page_offset = offset_val,                     \
    .header =                                                                  \
        {                                                                      \
            .object_header = TLV_OBJ_HEADER(type_val, slot_size_val),          \
            .cert_header = TLV_CERT_HEADER(sizeof(name_str) - 1, 0),           \
            .name = name_str,                                                  \
        },                                                                     \
    .header_size = 4 + sizeof(name_str) - 1,                                   \
    .data_size = slot_size_val - (4 + sizeof(name_str) - 1),                   \
  }

extern const dice_cert_layout_t kCdi0EcdsaStorage;
extern const dice_cert_layout_t kCdi1EcdsaStorage;

#ifdef __cplusplus
}
#endif

#endif  // OPENTITAN_SW_DEVICE_SILICON_CREATOR_LIB_CERT_DICE_STORAGE_H_
