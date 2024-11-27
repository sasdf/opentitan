use crate::bindings::*;
use crate::base::error::*;
use crate::*;


type Manifest = u8;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct KeymgrBindingValue {
    pub data: [u32; 8],
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct BootMeasurements {
    /// ROM_EXT digest value calculated in ROM. Stored in a format that can be
    /// consumed by the key manager.
    ///
    /// If OTP `ROM_KEYMGR_ROM_EXT_MEAS_EN` is set to `kHardenedBoolTrue`, the
    /// rom will use this value to configure the key manager attestation
    /// binding registers.
    pub rom_ext: KeymgrBindingValue,
    /// BL0 firmware + owner configuration block digest value calculated in
    /// ROM_EXT. Stored in a format that can be consumed by the key manager.
    pub bl0: KeymgrBindingValue,
}

static_assert!(core::mem::size_of::<BootMeasurements>() == 64);
static_assert!(core::mem::align_of::<BootMeasurements>() == 4);
static_assert!(core::mem::offset_of!(BootMeasurements, rom_ext) == 0);
static_assert!(core::mem::offset_of!(BootMeasurements, bl0) == 32);

extern "C" {
    pub fn shutdown_finalize(error: RomError);
    pub fn epmp_state_check() -> RomError;
    pub fn sec_mmio_check_values(rnd_offset: u32);
    pub fn sec_mmio_next_stage_init();
    pub fn imm_rom_ext_epmp_reconfigure() -> RomError;
    pub fn pinmux_init_uart0_tx();
    pub fn uart_init(precalculated_nco: u32);
    pub fn dbg_print_epmp();
    pub fn rom_ext_manifest() -> *const Manifest;
    pub fn dice_chain_init() -> RomError;
    pub fn dice_chain_attestation_silicon() -> RomError;
    pub fn dice_chain_attestation_creator(a: *const KeymgrBindingValue, b: *const Manifest)
        -> RomError;
    pub fn dice_chain_flush_flash() -> RomError;
    pub fn imm_rom_ext_epmp_mutable_rx(a: *const Manifest) -> RomError;
    pub fn rnd_uint32() -> u32;
    pub static boot_measurements: BootMeasurements;
    pub static kUartNCOValue: u32;
}
