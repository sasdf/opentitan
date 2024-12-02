// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#![no_std]

ot_bindgen::include!();

use ot_sw_device_silicon_creator_lib_drivers_rnd::*;

impl nonce_t {
  pub fn new(&mut self) {
      self.value[0] = unsafe { rnd_uint32() };
      self.value[1] = unsafe { rnd_uint32() };
  }
}

#[no_mangle]
pub unsafe extern "C" fn nonce_new(nonce: *mut nonce_t) {
    // Safety: Assume non-null ptr from C interface.
    unsafe { &mut *nonce }.new();
}
