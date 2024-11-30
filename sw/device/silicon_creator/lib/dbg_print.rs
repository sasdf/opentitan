// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#![no_std]

ot_bindgen::include!();

use core::fmt::{Arguments, Result, Write};

#[macro_export]
macro_rules! dbg_printf {
    ($fmt:literal) => (
        $crate::dbg_printf(
            concat!($fmt, "\0").as_ptr() as *const i8
        )
    );
    ($fmt:literal, $($arg:tt)*) => (
        $crate::dbg_printf(
            concat!($fmt, "\0").as_ptr() as *const i8,
            $($arg)*
        )
    );
}

pub fn dbg_print(s: &str) {
    for byte in s.bytes() {
        unsafe {
            uart_putchar(byte);
        }
    }
}

struct UartWriter;

impl Write for UartWriter {
    fn write_str(&mut self, s: &str) -> Result {
        dbg_print(s);
        Ok(())
    }
}

#[doc(hidden)]
pub fn _print(args: Arguments) {
    let mut writer = UartWriter;
    writer.write_fmt(args).unwrap();
}

#[doc(hidden)]
pub fn _println(args: Arguments) {
    _print(args);
    dbg_print("\r\n");
}

#[macro_export]
macro_rules! println {
    () => ($crate::dbg_print("\r\n"));
    ($($arg:tt)*) => ({
        $crate::_println(format_args!($($arg)*));
    });
}

#[macro_export]
macro_rules! print {
    ($($arg:tt)*) => ($crate::_print(format_args!($($arg)*)));
}
