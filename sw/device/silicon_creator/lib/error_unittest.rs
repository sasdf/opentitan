#![feature(try_trait_v2)]

pub use ot_sw_device_silicon_creator_lib_error::kErrorOk;
pub use ot_sw_device_silicon_creator_lib_error::kErrorUnknown;
pub use ot_sw_device_silicon_creator_lib_error::ErrorTrait;
pub use ot_sw_device_silicon_creator_lib_error::HardenedError;
pub use ot_sw_device_silicon_creator_lib_error::OkError;
pub use ot_sw_device_silicon_creator_lib_error::RomError;

pub use ot_sw_device_silicon_creator_lib_error::hardened_return_if_error;
pub use ot_sw_device_silicon_creator_lib_error::return_if_error;
pub use ot_sw_device_silicon_creator_lib_error::rom_error_t;

#[test]
fn test_class_return_ok() {
    fn test() -> RomError {
        let a: HardenedError = OkError();
        a?;
        OkError()
    }
    assert!(test().is_ok());
}

#[test]
#[should_panic]
fn test_class_return_error() {
    fn test() -> RomError {
        let a: HardenedError = kErrorUnknown.into();
        a.soften()?;
        OkError()
    }
    assert!(test().is_ok());
}

#[test]
fn test_macro_return_ok() {
    fn test() -> rom_error_t {
        let a: rom_error_t = kErrorOk;
        return_if_error!(a);
        kErrorOk
    }
    assert_eq!(test(), kErrorOk);
}

#[test]
#[should_panic]
fn test_macro_return_error() {
    fn test() -> rom_error_t {
        let a: rom_error_t = kErrorUnknown;
        hardened_return_if_error!(a);
        kErrorOk
    }
    assert_eq!(test(), kErrorOk);
}
