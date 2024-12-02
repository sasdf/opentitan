// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

use anyhow::{bail, Result};
pub use ot_sw_device_silicon_creator_manuf_base_perso_tlv_data as perso_tlv_objects;

// Types of objects which can come from the device in the perso blob.
#[repr(C)]
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum ObjType {
    UnendorsedX509Cert = perso_tlv_objects::kPersoObjectTypeX509Tbs as isize,
    EndorsedX509Cert = perso_tlv_objects::kPersoObjectTypeX509Cert as isize,
    DevSeed = perso_tlv_objects::kPersoObjectTypeDevSeed as isize,
    EndorsedCwtCert = perso_tlv_objects::kPersoObjectTypeCwtCert as isize,
}

impl ObjType {
    pub fn from_usize(value: usize) -> Result<ObjType> {
        match value {
            0 => Ok(ObjType::UnendorsedX509Cert),
            1 => Ok(ObjType::EndorsedX509Cert),
            2 => Ok(ObjType::DevSeed),
            3 => Ok(ObjType::EndorsedCwtCert),
            _ => bail!("incorrect input value of {value} for ObjType"),
        }
    }
}

// LTV header of the transferred object, on the wire packed in u16.
pub type ObjHeaderType = perso_tlv_objects::perso_tlv_object_header_t;
pub struct ObjHeader {
    pub obj_size: usize,
    pub obj_type: ObjType,
}

// Header of the certificate payload of the LTV object, on the wire packed in
// u16.
pub type CertHeaderType = perso_tlv_objects::perso_tlv_cert_header_t;
pub struct CertHeader<'a> {
    // Total size the certificate takes in the buffer, header + hame length +
    // cert size.
    pub wrapped_size: usize,
    pub cert_name: &'a str,
    pub cert_body: Vec<u8>,
}

// A helper macro for quick unpacking  LTV object and Certificate payload
// headers. First parameter indicates which header is being parsed, the second
// parameter is the field which needs to be extracted, the third parameter is
// the packed u16 header value.
#[macro_export]
macro_rules! perso_tlv_get_field {
    ($type:expr, $name:expr, $intv:expr) => {{
        let input = $intv as u32;
        let v = match ($type, $name) {
            ("obj", "size") => {
                (input >> $crate::perso_tlv_objects::kObjhSizeFieldShift)
                    & $crate::perso_tlv_objects::kObjhSizeFieldMask
            }

            ("obj", "type") => {
                (input >> $crate::perso_tlv_objects::kObjhTypeFieldShift)
                    & $crate::perso_tlv_objects::kObjhTypeFieldMask
            }

            ("crth", "size") => {
                (input >> $crate::perso_tlv_objects::kCrthSizeFieldShift)
                    & $crate::perso_tlv_objects::kCrthSizeFieldMask
            }

            ("crth", "name") => {
                (input >> $crate::perso_tlv_objects::kCrthNameSizeFieldShift)
                    & $crate::perso_tlv_objects::kCrthNameSizeFieldMask
            }
            (&_, _) => bail!("Unexpected macro invocation"),
        };
        v as usize
    }};
}

// Helper functions used to pack LTV object and Certificate payload headers.
pub fn make_obj_header(size: usize, otype: ObjType) -> Result<ObjHeaderType> {
    if size as u32 > perso_tlv_objects::kObjhSizeFieldMask {
        bail!("Can't create object of size {size}")
    }

    Ok((((size as u32 & perso_tlv_objects::kObjhSizeFieldMask)
        << perso_tlv_objects::kObjhSizeFieldShift)
        + (((otype as u32) & perso_tlv_objects::kObjhTypeFieldMask)
            << perso_tlv_objects::kObjhTypeFieldShift)) as u16)
}

pub fn make_cert_wrapper_header(cert_size: usize, cert_name: &str) -> Result<CertHeaderType> {
    if cert_size as u32 > perso_tlv_objects::kCrthSizeFieldMask {
        bail!("Can't create a certificate wraper of size {cert_size}")
    }

    let name_len = cert_name.len();
    if name_len as u32 > perso_tlv_objects::kCrthNameSizeFieldMask {
        bail!(
            "Can't create certificate wrapper for name \"{}\"",
            cert_name
        )
    }
    let wrapped_size = (cert_size + name_len + std::mem::size_of::<CertHeaderType>()) as u32;
    Ok((((wrapped_size & perso_tlv_objects::kCrthSizeFieldMask)
        << perso_tlv_objects::kCrthSizeFieldShift)
        + ((name_len as u32 & perso_tlv_objects::kCrthNameSizeFieldMask)
            << perso_tlv_objects::kCrthNameSizeFieldShift)) as u16)
}
