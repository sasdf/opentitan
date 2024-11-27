#![no_std]

use __sw_device_silicon_creator_imm_rom_ext_rust_dbg_print::dbg_printf;
use __sw_device_silicon_creator_imm_rom_ext_rust_base_error::*;
use __sw_device_silicon_creator_imm_rom_ext_rust_base_hardened::*;
use __sw_device_silicon_creator_imm_rom_ext_rust_ot_bindgen as ot;

#[no_mangle]
pub extern "C" fn imm_rom_ext_start() -> ot::rom_error_t {
    // Check the ePMP state.
    hardened_return_if_error!(unsafe { ot::epmp_state_check() });

    // Check sec_mmio expectations.
    // We don't check the counters since we don't want to tie ROM_EXT to a
    // specific ROM version.
    unsafe { ot::sec_mmio_check_values(ot::rnd_uint32()) };

    // Initialize Immutable ROM EXT.
    unsafe { ot::sec_mmio_next_stage_init() };
    hardened_return_if_error!(unsafe { ot::imm_rom_ext_epmp_reconfigure() });

    // Configure UART0 as stdout.
    unsafe { ot::pinmux_init_uart0_tx() };
    unsafe { ot::uart_init(ot::kUartNCOValue) };

    dbg_printf!("IMM_ROM_EXT v0.1\r\n", 0);
    unsafe { ot::dbg_print_epmp() };

    // Establish our identity.
    let rom_ext = unsafe { ot::rom_ext_manifest() };
    hardened_return_if_error!(unsafe { ot::dice_chain_init() });
    // TODO: Move UDS cert check to mutable ROM_EXT.
    hardened_return_if_error!(unsafe { ot::dice_chain_attestation_silicon() });
    hardened_return_if_error!(unsafe {
        ot::dice_chain_attestation_creator(&mut ot::boot_measurements.rom_ext, rom_ext)
    });

    // Write the DICE certs to flash if they have been updated.
    hardened_return_if_error!(unsafe { ot::dice_chain_flush_flash() });

    // Make mutable part executable.
    hardened_return_if_error!(unsafe { ot::imm_rom_ext_epmp_mutable_rx(rom_ext) });

    ot::kErrorOk
}

#[no_mangle]
pub extern "C" fn imm_rom_ext_main() {
  let error = imm_rom_ext_start();

  dbg_printf!("Hello Rust %d !!\r\n", error);

  if launder32!(error) != ot::kErrorOk {
    unsafe { ot::shutdown_finalize(error); }
    hardened_trap!();
  }
  hardened_check_eq!(error, ot::kErrorOk);
}
