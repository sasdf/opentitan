// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

use anyhow::{Result, bail};
use clap::Parser;
use std::time::Duration;

use opentitanlib::app::TransportWrapper;
use opentitanlib::execute_test;
use opentitanlib::io::gpio::PinMode;
use opentitanlib::test_utils::init::InitializeTest;
use opentitanlib::uart::console::UartConsole;

#[derive(Debug, Parser)]
struct Opts {
    #[command(flatten)]
    init: InitializeTest,

    /// Console receive timeout.
    #[arg(long, value_parser = humantime::parse_duration, default_value = "600s")]
    timeout: Duration,
}

fn test_gpio_sim_dv(opts: &Opts, transport: &TransportWrapper) -> Result<()> {
    let uart = transport.uart("console")?;

    // Initialize all 32 host GPIO pins to PushPull driven low (0) before the
    // device switches GPIO direction to input mode.
    for i in 0..32 {
        let pin = transport.gpio_pin(&i.to_string())?;
        pin.set_mode(PinMode::PushPull)?;
        pin.write(false)?;
    }

    let _ = UartConsole::wait_for(&*uart, r"Starting GPIO input test", opts.timeout)?;

    // Allow Ibex to finish configuring interrupts and enter wait_for_interrupt().
    std::thread::sleep(Duration::from_millis(50));

    // Drive rising thermometer pattern (0 -> 1 on pins 0..31 sequentially).
    for i in 0..32 {
        transport.gpio_pin(&i.to_string())?.write(true)?;
        std::thread::sleep(Duration::from_millis(10));
    }

    // Allow Ibex to verify read_all == 0xffffffff and enter the falling edge loop.
    std::thread::sleep(Duration::from_millis(30));

    // Drive falling reverse thermometer pattern (1 -> 0 on pins 0..31 sequentially).
    for i in 0..32 {
        transport.gpio_pin(&i.to_string())?.write(false)?;
        std::thread::sleep(Duration::from_millis(10));
    }

    let res = UartConsole::wait_for(&*uart, r"PASS!|FAIL!", opts.timeout)?;
    if !res[0].contains("PASS!") {
        bail!("Test failed: {:?}", res);
    }
    Ok(())
}

fn main() -> Result<()> {
    let opts = Opts::parse();
    opts.init.init_logging();
    let transport = opts.init.init_target()?;

    let uart = transport.uart("console")?;
    uart.set_flow_control(true)?;

    execute_test!(test_gpio_sim_dv, &opts, &transport);
    Ok(())
}
