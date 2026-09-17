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

fn test_pwrmgr_sysrst_ctrl(opts: &Opts, transport: &TransportWrapper) -> Result<()> {
    let uart = transport.uart("console")?;

    // Drive IOR13 (host pin 13) high initially to mimic external pull-up on key0_in.
    let key0_pin = transport.gpio_pin("IOR13")?;
    key0_pin.set_mode(PinMode::PushPull)?;
    key0_pin.write(true)?;
    std::thread::sleep(Duration::from_millis(10));

    // Wait for Round 0 to configure sysrst_ctrl and log that SourceOne / round 0 is ready.
    let _ = UartConsole::wait_for(
        &*uart,
        r"Reset Request SourceOne is set|Test round 0",
        opts.timeout,
    )?;

    // Pulse IOR13 low to trigger combo 0 self-reset.
    key0_pin.write(false)?;
    std::thread::sleep(Duration::from_millis(2));
    key0_pin.write(true)?;

    // Wait for final PASS! or FAIL! after Round 1 (watchdog bite) and Round 2 (finish).
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

    execute_test!(test_pwrmgr_sysrst_ctrl, &opts, &transport);
    Ok(())
}
