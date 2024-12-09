// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#![no_std]
#![feature(concat_idents)]

ot_bindgen::include!();

use core::convert::TryInto;
use core::mem::size_of;

pub(crate) use ot_sw_device_lib_testing_json_provisioning_data::*;
pub(crate) use ot_sw_device_silicon_creator_lib_cert_cert::*;
pub(crate) use ot_sw_device_silicon_creator_lib_error::*;

macro_rules! perso_tlv_get_field {
    ($typ:ident, $field: ident, $full_value: ident) => {{
        let mask: u16 = concat_idents!(k, $typ, $field, FieldMask) as u16;
        let shift: u16 = concat_idents!(k, $typ, $field, FieldShift) as u16;
        (($full_value.swap_bytes() >> shift) & mask)
    }};
}

pub fn perso_tlv_get_cert_obj_rust(buf: &[u8], obj: &mut perso_tlv_cert_obj_t) -> RomError {
    // Extract LTV object header
    if buf.len() < size_of::<perso_tlv_object_header_t>() {
        // Not enough data for header
        return RomError(kErrorPersoTlvCertObjNotFound);
    }
    let (objh_bytes, buf) = buf.split_at(size_of::<perso_tlv_object_header_t>());

    let objh = unsafe { *(objh_bytes.as_ptr() as *const perso_tlv_object_header_t) };

    let obj_size = perso_tlv_get_field!(Objh, Size, objh) as usize;
    if obj_size < size_of::<perso_tlv_object_header_t>() {
        return RomError(kErrorPersoTlvCertObjNotFound); // Object is empty
    }
    let obj_size = obj_size - size_of::<perso_tlv_object_header_t>();

    if obj_size > buf.len() {
        return RomError(kErrorPersoTlvInternal); // Object exceeds buffer size
    }
    let (buf, _) = buf.split_at(obj_size);
    obj.obj_size = obj_size;

    let obj_type = perso_tlv_get_field!(Objh, Type, objh) as perso_tlv_object_type_t;
    obj.obj_type = obj_type;
    if obj_type != kPersoObjectTypeX509Cert && obj_type != kPersoObjectTypeCwtCert {
        return RomError(kErrorPersoTlvCertObjNotFound);
    }

    // Extract certificate object header
    if buf.len() < size_of::<perso_tlv_cert_header_t>() {
        // Not enough data for header
        return RomError(kErrorPersoTlvCertObjNotFound);
    }
    let (crth_bytes, buf) = buf.split_at(size_of::<perso_tlv_cert_header_t>());
    let crth = unsafe { *(crth_bytes.as_ptr() as *const perso_tlv_cert_header_t) };
    let name_len = perso_tlv_get_field!(Crth, NameSize, crth) as usize;
    let wrapped_cert_size = perso_tlv_get_field!(Crth, Size, crth) as usize;

    if (wrapped_cert_size < (size_of::<perso_tlv_cert_header_t>() + name_len))
        || (wrapped_cert_size - size_of::<perso_tlv_cert_header_t>() > buf.len())
    {
        return RomError(kErrorPersoTlvInternal); // Something is really screwed up.
    }

    // Extract certificate name string
    if name_len > buf.len() {
        return RomError(kErrorPersoTlvInternal); // Something is really screwed up.
    }
    let (name_bytes, buf) = buf.split_at(name_len);
    let name_bytes: &[i8] = unsafe { &*(name_bytes as *const [u8] as *const [i8]) };
    obj.name[..name_len].copy_from_slice(name_bytes);
    obj.name[name_len] = 0;

    // Set pointer to certificate body
    obj.cert_body_size = buf.len();
    obj.cert_body_p = buf.as_ptr() as *mut u8;

    // Sanity check on the certificate body size.
    // TODO(24281): add sanity check on CWT certificate body size.
    if obj_type == kPersoObjectTypeX509Cert {
        let decoded_cert_size = unsafe { cert_x509_asn1_decode_size_header(buf.as_ptr()) };
        if decoded_cert_size != obj.cert_body_size.try_into().unwrap() {
            return RomError(kErrorPersoTlvInternal);
        }
    }

    OkError()
}

#[no_mangle]
pub unsafe extern "C" fn perso_tlv_get_cert_obj(
    buf: *mut u8,
    ltv_buf_size: usize,
    obj: *mut perso_tlv_cert_obj_t,
) -> rom_error_t {
    let buf = unsafe { core::slice::from_raw_parts_mut(buf, ltv_buf_size) };
    let obj = unsafe { &mut *obj };
    perso_tlv_get_cert_obj_rust(buf, obj).into()
}
