#![no_std]
#![feature(core_intrinsics)]

use core::intrinsics;
use core::panic::PanicInfo;

use ot_sw_device_lib_base_hardened::hardened_trap;

#[inline]
#[panic_handler]
#[allow(unreachable_code)]
fn panic(_info: &PanicInfo) -> ! {
    hardened_trap!();
    intrinsics::abort()
}
