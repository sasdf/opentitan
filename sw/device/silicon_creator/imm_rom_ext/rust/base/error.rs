#![no_std]

// Hardened version of `RETURN_IF_ERROR()`.
//
// See `launder32()` and `HARDENED_CHECK_EQ()` in
// `sw/device/lib/base/hardened.h` for more details.
//
// @param expr An expression which results in a `rom_error_t`.
//
#[macro_export]
macro_rules! hardened_return_if_error {
  ($expr:expr) => (
    {
      use __sw_device_silicon_creator_imm_rom_ext_rust_base_hardened::hardened_check_eq;
      use __sw_device_silicon_creator_imm_rom_ext_rust_base_hardened::launder32;
      use __sw_device_silicon_creator_imm_rom_ext_rust_ot_bindgen as ot;

      let error: ot::rom_error_t = $expr;
      if launder32!(error) != ot::kErrorOk {
        return error;
      }
      hardened_check_eq!(error, ot::kErrorOk);
    }
  );
}
