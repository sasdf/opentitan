#![no_std]

use ot_sw_device_lib_arch_device::kUartNCOValue;
use ot_sw_device_lib_base_hardened::*;
use ot_sw_device_silicon_creator_imm_rom_ext_imm_rom_ext_epmp::imm_rom_ext_epmp_mutable_rx;
use ot_sw_device_silicon_creator_imm_rom_ext_imm_rom_ext_epmp::imm_rom_ext_epmp_reconfigure;
use ot_sw_device_silicon_creator_lib_base_boot_measurements::boot_measurements;
use ot_sw_device_silicon_creator_lib_base_sec_mmio::*;
use ot_sw_device_silicon_creator_lib_cert_dice_chain::dice_chain_attestation_creator;
use ot_sw_device_silicon_creator_lib_cert_dice_chain::dice_chain_attestation_silicon;
use ot_sw_device_silicon_creator_lib_cert_dice_chain::dice_chain_flush_flash;
use ot_sw_device_silicon_creator_lib_cert_dice_chain::dice_chain_init;
use ot_sw_device_silicon_creator_lib_dbg_print::dbg_print_epmp;
use ot_sw_device_silicon_creator_lib_dbg_print::dbg_printf;
use ot_sw_device_silicon_creator_lib_drivers_pinmux::pinmux_init_uart0_tx;
use ot_sw_device_silicon_creator_lib_drivers_rnd::rnd_uint32;
use ot_sw_device_silicon_creator_lib_drivers_uart::uart_init;
use ot_sw_device_silicon_creator_lib_epmp_state::epmp_state_check;
use ot_sw_device_silicon_creator_lib_error::*;
use ot_sw_device_silicon_creator_lib_shutdown::shutdown_finalize;
use ot_sw_device_silicon_creator_rom_ext_rom_ext_manifest::rom_ext_manifest;

#[no_mangle]
pub extern "C" fn imm_rom_ext_start() -> HardenedError {
    // Check the ePMP state.
    // Safety: porting WIP.
    HardenedError(unsafe { epmp_state_check() })?;

    // Check sec_mmio expectations.
    // We don't check the counters since we don't want to tie ROM_EXT to a
    // specific ROM version.
    // Safety: porting WIP.
    unsafe { sec_mmio_check_values(rnd_uint32()) };

    // Initialize Immutable ROM EXT.
    // Safety: porting WIP.
    unsafe { sec_mmio_next_stage_init() };
    // Safety: porting WIP.
    HardenedError(unsafe { imm_rom_ext_epmp_reconfigure() })?;

    // Configure UART0 as stdout.
    // Safety: porting WIP.
    unsafe { pinmux_init_uart0_tx() };
    // Safety: porting WIP.
    unsafe { uart_init(kUartNCOValue) };

    unsafe { dbg_printf!("IMM_ROM_EXT v0.1\r\n") };
    // Safety: porting WIP.
    unsafe { dbg_print_epmp() };

    // Establish our identity.
    // Safety: porting WIP.
    let rom_ext = unsafe { rom_ext_manifest() };
    // Safety: porting WIP.
    HardenedError(unsafe { dice_chain_init() })?;
    // TODO: Move UDS cert check to mutable ROM_EXT.
    // Safety: porting WIP.
    HardenedError(unsafe { dice_chain_attestation_silicon() })?;
    // Safety: porting WIP.
    HardenedError(unsafe {
        dice_chain_attestation_creator(&mut boot_measurements.rom_ext, rom_ext)
    })?;

    // Write the DICE certs to flash if they have been updated.
    // Safety: porting WIP.
    HardenedError(unsafe { dice_chain_flush_flash() })?;

    // Make mutable part executable.
    // Safety: porting WIP.
    HardenedError(unsafe { imm_rom_ext_epmp_mutable_rx(rom_ext) })?;

    OkError()
}

#[no_mangle]
pub extern "C" fn imm_rom_ext_main() {
    let error = imm_rom_ext_start();

    unsafe { dbg_printf!("Hello Rust %d !!\r\n", error.code()) };

    if error.is_err() {
        // Safety: porting WIP.
        unsafe {
            shutdown_finalize(error.code());
        }
        hardened_trap!();
    }
    error.unwrap();
}
