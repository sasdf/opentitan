#![no_std]
#![feature(core_intrinsics)]

use core::intrinsics;
use core::panic::PanicInfo;


#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    intrinsics::abort()
}

#[macro_export]
macro_rules! dbg_printf {
    ($fmt:literal, $($arg:tt)*) => (
      use __sw_device_silicon_creator_imm_rom_ext_rust_ot_bindgen as ot;
      unsafe { ot::dbg_printf(concat!($fmt, "\0").as_ptr() as *const i8, $($arg)*) }
    );
}

#[macro_export]
macro_rules! static_assert {
    ($($tt:tt)*) => {
        const _: () = assert!($($tt)*);
    }
}
