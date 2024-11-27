// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#![no_std]

ot_bindgen::include!();

/// Creates a reordering barrier for `_val`.
///
/// This function prevents the compiler from reordering instructions around the
/// barrier. This is useful for ensuring that instructions are executed in a
/// specific order, even in the presence of compiler optimizations.
///
/// See the original C code comments for more details and examples.
#[macro_export]
macro_rules! barrier {
  ($val:ident) => (
    {
      #[cfg(not(any(ot_build_for_static_analyzer, ot_disable_hardening)))]
      // Safety: This inline assembly is a no-op and does not have any side
      // effects.
      unsafe {
        #[cfg(ot_platform_rv32)]
        core::arch::asm!( "# {val}", val = inout(reg) $val );

        #[cfg(not(ot_platform_rv32))]
        core::arch::asm!( "# {val:r}", val = inout(reg) $val );
      };
    }
  );
}
pub use barrier as barrier32;
pub use barrier as barrierw;

// Launders the 32-bit value `val`.
//
// This function returns a value identical to the input but prevents the
// compiler from making optimizations based on the laundered value. This is
// useful for preventing the compiler from removing or reordering instructions
// that are intended to be redundant for fault tolerance.
//
// See the original C code comments for more details and examples.
//
// Confirmed with https://godbolt.org/z/qovzeG4z6
#[macro_export]
macro_rules! launder32 {
    ($val:expr) => {{
        let mut val = $val as u32;
        $crate::barrier!(val);
        val
    }};
}

/// Launders the pointer-sized value `val`.
///
/// See `launder32()` for detailed semantics.
#[macro_export]
macro_rules! launderw {
    ($val:expr) => {{
        let mut val = $val as usize;
        $crate::barrier!(val);
        val
    }};
}

#[macro_use]
#[cfg(all(ot_platform_rv32, not(ot_disable_hardening)))]
mod platform_hardened_impl {
    /// This string can be tuned to be longer or shorter as desired, for
    /// fault-hardening purposes.
    #[macro_export]
    macro_rules! hardened_trap {
        () => {
            // Safety: This inline assembly is a trap instruction and will
            // terminate the program.
            unsafe {
                core::arch::asm!("unimp; unimp; unimp;");
            }
        };
    }

    // Don't use #[rustfmt::skip] since it produces macro-defined-macro.
    #[cfg_attr(rustfmt, rustfmt_skip)]
    #[macro_export]
    macro_rules! hardened_check_op {
        (eq, $e:expr) => { concat!("beq " , $e) };
        (ne, $e:expr) => { concat!("bne " , $e) };
        (lt, $e:expr) => { concat!("bltu ", $e) };
        (gt, $e:expr) => { concat!("bgtu ", $e) };
        (le, $e:expr) => { concat!("bleu ", $e) };
        (ge, $e:expr) => { concat!("bgeu ", $e) };
    }

    /// The inverse opcodes test the opposite condition of their name
    /// (e.g. EQ checks for not equal, etc).
    #[cfg_attr(rustfmt, rustfmt_skip)]
    #[macro_export]
    macro_rules! hardened_check_inv {
        (eq, $e:expr) => { concat!("bne " , $e) };
        (ne, $e:expr) => { concat!("beq " , $e) };
        (lt, $e:expr) => { concat!("bgeu ", $e) };
        (gt, $e:expr) => { concat!("bleu ", $e) };
        (le, $e:expr) => { concat!("bgtu ", $e) };
        (ge, $e:expr) => { concat!("bltu ", $e) };
    }

    // rustfmt is not good at high-order macro.
    #[cfg_attr(rustfmt, rustfmt_skip)]
    macro_rules! define_hardened_check {
        ($name: ident, $op:ident) => {
            #[macro_export]
            macro_rules! $name {
                ($a:expr, $b:expr) => {
                    let (a, b) = ($a as usize, $b as usize);

                    // Safety: This inline assembly performs a conditional trap
                    // and does not have any side effects if the condition is
                    // met.
                    unsafe {
                        core::arch::asm!(
                            $crate::hardened_check_op!($op, "{a}, {b}, 1f;"),
                            "0:",
                            "unimp;",
                            "1:",
                            $crate::hardened_check_inv!($op, "{a}, {b}, 0b;"),
                            a = in(reg) a,
                            b = in(reg) b,
                        );
                    };
                };
            }
        };
    }
}

#[macro_use]
#[cfg(all(not(ot_platform_rv32), not(ot_disable_hardening)))]
mod platform_hardened_impl {
    #[macro_export]
    macro_rules! hardened_trap {
        () => {
            panic!("hardened trap");
        };
    }

    #[cfg_attr(rustfmt, rustfmt_skip)]
    #[macro_export]
    macro_rules! hardened_check_op {
        (eq, $a:tt, $b:tt) => { $a == $b };
        (ne, $a:tt, $b:tt) => { $a != $b };
        (lt, $a:tt, $b:tt) => { $a <  $b };
        (gt, $a:tt, $b:tt) => { $a >  $b };
        (le, $a:tt, $b:tt) => { $a <= $b };
        (ge, $a:tt, $b:tt) => { $a >= $b };
    }

    macro_rules! define_hardened_check {
        ($name: ident, $op:ident) => {
            #[macro_export]
            macro_rules! $name {
                ($a:expr, $b:expr) => {
                    let (a, b) = ($a as usize, $b as usize);
                    assert!($crate::hardened_check_op!($op, a, b));
                };
            }
        };
    }
}

#[macro_use]
#[cfg(ot_disable_hardening)]
mod platform_hardened_impl {
    #[macro_export]
    macro_rules! hardened_trap {
        () => {};
    }

    macro_rules! define_hardened_check {
        ($name: ident, $op:ident) => {
            #[macro_export]
            macro_rules! $name {
                ($a:expr, $b:expr) => {};
            }
        };
    }
}

define_hardened_check!(hardened_check_eq, eq);
define_hardened_check!(hardened_check_ne, ne);
define_hardened_check!(hardened_check_lt, lt);
define_hardened_check!(hardened_check_gt, gt);
define_hardened_check!(hardened_check_le, le);
define_hardened_check!(hardened_check_ge, ge);
