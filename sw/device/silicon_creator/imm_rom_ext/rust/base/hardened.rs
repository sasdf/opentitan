#![no_std]

// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0


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
  ($val:expr) => (
    {
      let mut val = $val as u32;
      // Safety: This inline assembly is a no-op and does not have any side effects.
      #[cfg(not(any(ot_build_for_static_analyzer, ot_disable_hardening)))]
      unsafe {
        core::arch::asm!( "# {val}", val = inout(reg) val );
      };
      val
    }
  );
}

// Launders the pointer-sized value `val`.
//
// See `launder32()` for detailed semantics.
#[macro_export]
macro_rules! launderw {
  ($val:expr) => (
    {
      let mut val = $val as usize;
      // Safety: This inline assembly is a no-op and does not have any side effects.
      #[cfg(not(any(ot_build_for_static_analyzer, ot_disable_hardening)))]
      unsafe {
        core::arch::asm!( "# {val}", val = inout(reg) val );
      };
      val
    }
  );
}

// Creates a reordering barrier for `_val`.
//
// This function prevents the compiler from reordering instructions around the
// barrier. This is useful for ensuring that instructions are executed in a
// specific order, even in the presence of compiler optimizations.
//
// See the original C code comments for more details and examples.
#[inline]
pub fn barrier32(mut _val: u32) {
  launder32!(_val);
}

// Creates a reordering barrier for `_val`.
//
// See `barrier32()` for detailed semantics.
#[inline]
pub fn barrierw(mut _val: usize) {
  launderw!(_val);
}

// Executes a trap instruction.
//
// This macro is intended to be used in situations where the program has
// detected an error or unexpected condition that indicates a potential
// attack.
#[macro_export]
macro_rules! hardened_trap {
  () => {
    // Safety: This inline assembly is a trap instruction and will terminate the program.
    unsafe {
      core::arch::asm!("unimp; unimp; unimp;");
    }
  };
}

// Compares two values and traps if they are not equal.
//
// This macro performs a constant-time comparison of two values and executes a
// trap instruction if they are not equal. This is useful for enforcing
// invariants in hardened code.
#[macro_export]
macro_rules! hardened_check_eq {
  ($a:expr, $b:expr) => {
      let (a, b) = ($a as usize, $b as usize);
  
    // Safety: This inline assembly performs a conditional trap and does not have any side
    // effects if the condition is met.
    unsafe {
      core::arch::asm!(
        "beq {a}, {b}, 1f;",
        "unimp;",
        "1:",
        "bne {a}, {b}, 0f;",
        "0:",
        a = in(reg) a,
        b = in(reg) b,
      );
    }
  };
}

// Compares two values and traps if they are equal.
//
// This macro performs a constant-time comparison of two values and executes a
// trap instruction if they are equal. This is useful for enforcing invariants
// in hardened code.
#[macro_export]
macro_rules! hardened_check_ne {
  ($a:expr, $b:expr) => {
    let (a, b) = ($a as usize, $b as usize);

    // Safety: This inline assembly performs a conditional trap and does not have any side
    // effects if the condition is met.
    unsafe {
      core::arch::asm!(
        "bne {a}, {b}, 1f;",
        "unimp;",
        "1:",
        "beq {a}, {b}, 0f;",
        "0:",
        a = in(reg) a,
        b = in(reg) b,
      );
    }
  };
}

// Compares two values and traps if the first is not less than the second.
//
// This macro performs a constant-time comparison of two values and executes a
// trap instruction if the first value is not less than the second value. This
// is useful for enforcing invariants in hardened code.
#[macro_export]
macro_rules! hardened_check_lt {
  ($a:expr, $b:expr) => {
    let (a, b) = ($a as usize, $b as usize);

    // Safety: This inline assembly performs a conditional trap and does not have any side
    // effects if the condition is met.
    unsafe {
      core::arch::asm!(
        "bltu {a}, {b}, 1f;",
        "unimp;",
        "1:",
        "bgeu {a}, {b}, 0f;",
        "0:",
        a = in(reg) a,
        b = in(reg) b,
      );
    }
  };
}

// Compares two values and traps if the first is not greater than the second.
//
// This macro performs a constant-time comparison of two values and executes a
// trap instruction if the first value is not greater than the second value.
// This is useful for enforcing invariants in hardened code.
#[macro_export]
macro_rules! hardened_check_gt {
  ($a:expr, $b:expr) => {
    let (a, b) = ($a as usize, $b as usize);

    // Safety: This inline assembly performs a conditional trap and does not have any side
    // effects if the condition is met.
    unsafe {
      core::arch::asm!(
        "bgtu {a}, {b}, 1f;",
        "unimp;",
        "1:",
        "bleu {a}, {b}, 0f;",
        "0:",
        a = in(reg) a,
        b = in(reg) b,
      );
    }
  };
}

// Compares two values and traps if the first is not less than or equal to the
// second.
//
// This macro performs a constant-time comparison of two values and executes a
// trap instruction if the first value is not less than or equal to the second
// value. This is useful for enforcing invariants in hardened code.
#[macro_export]
macro_rules! hardened_check_le {
  ($a:expr, $b:expr) => {
    let (a, b) = ($a as usize, $b as usize);

    // Safety: This inline assembly performs a conditional trap and does not have any side
    // effects if the condition is met.
    unsafe {
      core::arch::asm!(
        "bleu {a}, {b}, 1f;",
        "unimp;",
        "1:",
        "bgtu {a}, {b}, 0f;",
        "0:",
        a = in(reg) a,
        b = in(reg) b,
      );
    }
  };
}

// Compares two values and traps if the first is not greater than or equal to
// the second.
//
// This macro performs a constant-time comparison of two values and executes a
// trap instruction if the first value is not greater than or equal to the
// second value. This is useful for enforcing invariants in hardened code.
#[macro_export]
macro_rules! hardened_check_ge {
  ($a:expr, $b:expr) => {
    let (a, b) = ($a as usize, $b as usize);

    // Safety: This inline assembly performs a conditional trap and does not have any side
    // effects if the condition is met.
    unsafe {
      core::arch::asm!(
        "bgeu {a}, {b}, 1f;",
        "unimp;",
        "1:",
        "bltu {a}, {b}, 0f;",
        "0:",
        a = in(reg) a,
        b = in(reg) b,
      );
    }
  };
}
