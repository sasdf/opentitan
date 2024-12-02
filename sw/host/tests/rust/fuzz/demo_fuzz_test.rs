// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#[no_mangle]
pub extern "C" fn LLVMFuzzerTestOneInput(data: *const u8, size: usize) -> u32 {
    let data = unsafe { std::slice::from_raw_parts(data, size) };

    let mut a = 0;
    if data.len() > 42 && data[0] == 42 {
        a += 1;
    }
    let _ = a;

    return 0; // Values other than 0 and -1 are reserved for future use.
}
