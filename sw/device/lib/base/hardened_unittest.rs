use ot_sw_device_lib_base_hardened::{
    hardened_check_eq, hardened_check_ne, hardened_trap, launder32,
};

#[test]
fn test_launder32_same() {
    let val = 0x1337;
    assert_eq!(launder32!(val), 0x1337);
    assert_ne!(launder32!(val), 0x1338);
}

#[test]
#[should_panic]
fn test_trap() {
    hardened_trap!();
}

#[test]
#[should_panic]
fn test_hardened_check_panic() {
    let val = 0x1337;
    hardened_check_eq!(val, 1338);
}

#[test]
#[should_panic]
fn test_hardened_check_pass() {
    let val = 0x1337;
    hardened_check_ne!(val, 1338);
    hardened_check_eq!(val, 1337);
}
