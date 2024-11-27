// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#![no_std]

extern "C" {
    pub fn lifecycle_state_get() -> LifecycleState;
    pub fn lifecycle_raw_state_get() -> u32;
    pub fn lifecycle_device_id_get(device_id: *mut LifecycleDeviceId);
    pub fn lifecycle_hw_rev_get(hw_rev: *mut LifecycleHwRev);
}

// Lifecycle states.
//
// This is a condensed version of the 24 possible life cycle states where
// TEST_UNLOCKED_* states are mapped to `kLcStateTest` and invalid states where
// CPU execution is disabled are omitted.
//
// Encoding generated with
// $ ./util/design/sparse-fsm-encode.py -d 6 -m 5 -n 32 \
//     -s 2447090565 --language=c
//
// Minimum Hamming distance: 13
// Maximum Hamming distance: 19
// Minimum Hamming weight: 15
// Maximum Hamming weight: 20
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum LifecycleState {
    // Unlocked test state where debug functions are enabled.
    //
    // Corresponds to TEST_UNLOCKED_* life cycle states.
    Test = 0xb2865fbb,
    // Development life cycle state where limited debug functionality is
    // available.
    Dev = 0x0b5a75e0,
    // Production life cycle state.
    Prod = 0x65f2520f,
    // Same as PROD, but transition into RMA is not possible from this state.
    ProdEnd = 0x91b9b68a,
    // RMA life cycle state.
    Rma = 0xcf8cfaab,
}

// Size of the device identifier in words.
pub const LIFECYCLE_DEVICE_ID_NUM_WORDS: usize = 8;

// 256-bit device identifier that is stored in the `HW_CFG0` partition in OTP.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct LifecycleDeviceId {
    pub device_id: [u32; LIFECYCLE_DEVICE_ID_NUM_WORDS],
}

// Hardware revision.
//
// Consists of a 16-bit silicon creator ID,
// a 16-bit product ID and an 8bit revision ID.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct LifecycleHwRev {
    pub silicon_creator_id: u16,
    pub product_id: u16,
    pub revision_id: u8,
}
