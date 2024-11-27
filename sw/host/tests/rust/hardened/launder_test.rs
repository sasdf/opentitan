use ot_sw_device_lib_base_hardened::launder32;
use std::error::Error;

fn main() -> Result<(), Box<dyn Error>> {
    let a = 0x1337;
    let b = 0x1337;

    if a != 0x1337 {
        println!("RemovedCheckNotFound");
    };

    if launder32!(b) != 0x1337 {
        println!("LaunderedCheckFound");
    };

    Ok(())
}
