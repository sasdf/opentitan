#![no_std]
#![feature(try_trait_v2)]

ot_bindgen::include!();

pub use ot_sw_device_lib_base_hardened::hardened_check_eq;
pub use ot_sw_device_lib_base_hardened::launder32;

// Evaluate an expression and return if the result is an error.
//
// @param expr_ An expression which results in a `rom_error_t`.
//
#[macro_export]
macro_rules! return_if_error {
    ($expr:expr) => {{
        let error: $crate::rom_error_t = $expr;
        if error != $crate::kErrorOk {
            return error;
        }
    };};
}

// Hardened version of `RETURN_IF_ERROR()`.
//
// See `launder32()` and `HARDENED_CHECK_EQ()` in
// `sw/device/lib/base/hardened.h` for more details.
//
// @param expr An expression which results in a `rom_error_t`.
//
#[macro_export]
macro_rules! hardened_return_if_error {
    ($expr:expr) => {{
        let error: $crate::rom_error_t = $expr;
        if $crate::launder32!(error) != $crate::kErrorOk {
            return error;
        }
        $crate::hardened_check_eq!(error, $crate::kErrorOk);
    };};
}

// Rust type for rom_error_t with Try trait.
// It supports "?" as RETURN_IF_ERROR.
#[repr(C)]
pub struct RomError(pub rom_error_t);

// Hardened version of RomError.
#[repr(C)]
pub struct HardenedError(pub rom_error_t);

// Error constant shortcuts.
#[allow(non_snake_case)]
#[inline]
pub fn OkError<T: core::convert::From<u32>>() -> T {
    kErrorOk.into()
}

// Error status interface.
pub trait ErrorTrait {
    fn is_ok(&self) -> bool;
    fn is_err(&self) -> bool;
    fn is_code(&self, code: rom_error_t) -> bool;
    fn unwrap(self);
    fn harden(self) -> HardenedError;
    fn soften(self) -> RomError;
    fn code(&self) -> rom_error_t;
}

// Generic class implementation.
macro_rules! define_error_class {
    ($Type: ident, $launder: ident, $hardened: ident, $check: ident) => {
        impl ErrorTrait for $Type {
            #[inline]
            fn code(&self) -> rom_error_t {
                self.0
            }
            #[inline]
            fn is_ok(&self) -> bool {
                $launder!(self.code()) == kErrorOk
            }
            #[inline]
            fn is_err(&self) -> bool {
                !self.is_ok()
            }
            #[inline]
            fn is_code(&self, code: rom_error_t) -> bool {
                $launder!(self.code()) == code
            }
            #[inline]
            fn unwrap(self) {
                $check!(self.code(), kErrorOk);
            }
            #[inline]
            fn harden(self) -> HardenedError {
                self.code().into()
            }
            #[inline]
            fn soften(self) -> RomError {
                self.code().into()
            }
        }

        impl From<rom_error_t> for $Type {
            #[inline]
            fn from(err: rom_error_t) -> Self {
                $Type(err)
            }
        }

        impl From<$Type> for rom_error_t {
            #[inline]
            fn from(err: $Type) -> Self {
                err.code()
            }
        }

        impl core::ops::Try for $Type {
            type Output = ();
            type Residual = $Type;
            #[inline]
            fn from_output(_: Self::Output) -> Self {
                OkError()
            }
            #[inline]
            fn branch(self) -> core::ops::ControlFlow<Self::Residual, Self::Output> {
                if self.is_err() {
                    return core::ops::ControlFlow::Break(self);
                }
                $hardened!(self.code(), kErrorOk);
                core::ops::ControlFlow::Continue(())
            }
        }

        impl<T: ErrorTrait> core::ops::FromResidual<T> for $Type {
            #[inline]
            fn from_residual(residual: T) -> $Type {
                $hardened!(residual.code(), kErrorOk);
                residual.code().into()
            }
        }
    };
}

macro_rules! nop_launder {
    ($t:expr) => {
        $t
    };
}

macro_rules! nop_check {
    ($t:expr, $u:expr) => {};
}

define_error_class!(RomError, nop_launder, assert_eq, nop_check);
define_error_class!(
    HardenedError,
    launder32,
    hardened_check_eq,
    hardened_check_eq
);
