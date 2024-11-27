#![no_std]
#![feature(core_intrinsics)]

use core::intrinsics;
use core::panic::PanicInfo;

use __sw_device_silicon_creator_imm_rom_ext_rust_base_hardened::hardened_trap;


#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
  hardened_trap!();
  intrinsics::abort()
}
