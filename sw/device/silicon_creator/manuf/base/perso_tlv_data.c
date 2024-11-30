// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/silicon_creator/manuf/base/perso_tlv_data.h"

#include "sw/device/silicon_creator/lib/cert/cert.h"
#include "sw/device/silicon_creator/lib/error.h"

rom_error_t perso_tlv_cert_obj_build(const char *name,
                                     const perso_tlv_object_type_t obj_type,
                                     const uint8_t *cert, size_t cert_size,
                                     uint8_t *buf, size_t *buf_size) {
  perso_tlv_object_header_t obj_header = 0;
  perso_tlv_cert_header_t cert_header = 0;
  size_t obj_size;
  size_t wrapped_cert_size;

  // Compute the name length (strlen() is not available).
  size_t name_len = 0;
  while (name[name_len])
    name_len++;
  if (name_len > kCrthNameSizeFieldMask)
    return kErrorPersoTlvCertNameTooLong;

  // Compute the wrapped certificate object (cert header + cert data) and perso
  // LTV object sizes.
  wrapped_cert_size = sizeof(perso_tlv_cert_header_t) + name_len + cert_size;
  obj_size = wrapped_cert_size + sizeof(perso_tlv_object_header_t);

  // Check there is enough room in the buffer to store the perso LTV object.
  if (obj_size > *buf_size)
    return kErrorPersoTlvOutputBufTooSmall;

  // Setup the perso LTV object header.
  PERSO_TLV_SET_FIELD(Objh, Type, obj_header, obj_type);
  PERSO_TLV_SET_FIELD(Objh, Size, obj_header, obj_size);

  // Setup the cert object header.
  PERSO_TLV_SET_FIELD(Crth, Size, cert_header, wrapped_cert_size);
  PERSO_TLV_SET_FIELD(Crth, NameSize, cert_header, name_len);

  // Push the cert perso LTV object to the buffer.
  // Return the size of the buffer that was used up by this perso LTV object.
  *buf_size = 0;
  memcpy(buf + *buf_size, &obj_header, sizeof(perso_tlv_object_header_t));
  *buf_size += sizeof(perso_tlv_object_header_t);
  memcpy(buf + *buf_size, &cert_header, sizeof(perso_tlv_cert_header_t));
  *buf_size += sizeof(perso_tlv_cert_header_t);
  memcpy(buf + *buf_size, name, name_len);
  *buf_size += name_len;
  memcpy(buf + *buf_size, cert, cert_size);
  *buf_size += cert_size;

  return kErrorOk;
}

rom_error_t perso_tlv_push_cert_to_perso_blob(
    const char *name, bool needs_endorsement,
    const dice_cert_format_t dice_format, const uint8_t *cert, size_t cert_size,
    perso_blob_t *pb) {
  // Build the perso TLV cert object and push it to the perso blob.
  size_t obj_size = sizeof(pb->body) - pb->next_free;
  perso_tlv_object_type_t obj_type = kPersoObjectTypeCwtCert;
  if (dice_format == kDiceCertFormatX509TcbInfo) {
    if (needs_endorsement) {
      obj_type = kPersoObjectTypeX509Tbs;
    } else {
      obj_type = kPersoObjectTypeX509Cert;
    }
  }
  HARDENED_RETURN_IF_ERROR(perso_tlv_cert_obj_build(
      name, obj_type, cert, cert_size, pb->body + pb->next_free, &obj_size));

  // Update the perso blob offset and object count.
  pb->next_free += obj_size;
  pb->num_objs++;

  return kErrorOk;
}

rom_error_t perso_tlv_push_to_perso_blob(const void *data, size_t size,
                                         perso_blob_t *perso_blob) {
  size_t room = sizeof(perso_blob->body) - perso_blob->next_free;
  if (room < size)
    return kErrorPersoTlvOutputBufTooSmall;
  memcpy(perso_blob->body + perso_blob->next_free, data, size);
  perso_blob->next_free += size;
  return kErrorOk;
}
