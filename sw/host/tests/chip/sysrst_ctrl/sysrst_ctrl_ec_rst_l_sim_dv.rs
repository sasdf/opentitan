// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

use anyhow::{Result, ensure};
use clap::Parser;
use once_cell::sync::Lazy;
use std::fs;
use std::path::PathBuf;
use std::time::Duration;

use object::{Object, ObjectSymbol};
use opentitanlib::app::TransportWrapper;
use opentitanlib::execute_test;
use opentitanlib::io::uart::Uart;
use opentitanlib::test_utils::init::InitializeTest;
use opentitanlib::test_utils::mem::MemWriteReq;
use opentitanlib::test_utils::test_status::TestStatus;
use opentitanlib::uart::console::UartConsole;

use sysrst_ctrl::{Config, read_pins, set_pins, setup_pins};

#[derive(Debug, Parser)]
struct Opts {
    #[command(flatten)]
    init: InitializeTest,

    /// Console receive timeout.
    #[arg(long, value_parser = humantime::parse_duration, default_value = "10s")]
    timeout: Duration,

    /// Path to the firmware's ELF file, for querying symbol addresses.
    #[arg(value_name = "FIRMWARE_ELF")]
    firmware_elf: PathBuf,
}

struct Params<'a> {
    opts: &'a Opts,
    transport: &'a TransportWrapper,
    uart: &'a dyn Uart,
    config: &'a Config,
    test_phase_addr: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
enum TestPhase {
    OverrideSetup = 2,
    OverrideZeros = 3,
    OverrideOnes = 4,
    Done = 5,
}

static CONFIG: Lazy<Config> = Lazy::new(|| {
    Config {
        // key0_in, key1_in, key2_in, pwrb_in
        output_pins: vec!["IOR10", "IOR11", "IOR12", "IOR5"],
        open_drain: vec![false, false, false, false],
        // key0_out, key1_out, key2_out, pwrb_out, bat_dis_out, z3_wakeup_out, ec_rst, flash_wp
        input_pins: vec![
            "IOR6",
            "IOR7",
            "IOB0",
            "IOB1",
            "IOB2",
            "IOB3",
            "SYSRST_CTRL_EC_RST_L",
            "SYSRST_CTRL_FLASH_WP_L",
        ],
        pullup_pins: vec!["SYSRST_CTRL_EC_RST_L", "SYSRST_CTRL_FLASH_WP_L"],
    }
});

const OUTPUT_ALL_SET: u8 = 0b11111111;
const OUTPUT_NONE_SET: u8 = 0b00000000;

fn sync_with_sw(params: &Params) -> Result<()> {
    UartConsole::wait_for(
        params.uart,
        &TestStatus::InWfi.wait_pattern(),
        params.opts.timeout,
    )?;
    Ok(())
}

fn chip_sw_sysrst_ctrl_ec_rst_l_sim_dv(params: &Params) -> Result<()> {
    // Setup transport pins and drive key0..pwrb high initially.
    setup_pins(params.transport, params.config)?;
    set_pins(params.transport, params.config, 0b1111)?;

    // Wait for Boot #0 to configure combo reset and enter WFI.
    sync_with_sw(params)?;

    // Drive key0_in (bit 0) and key1_in (bit 1) low to trigger combo 0 self-reset + ec_rst_l.
    set_pins(params.transport, params.config, 0b1100)?;
    std::thread::sleep(Duration::from_millis(10));
    set_pins(params.transport, params.config, 0b1111)?;

    // Wait for Boot #1 to check reset reason (kDifRstmgrResetInfoSysRstCtrl) and enter sync_with_testbench.
    sync_with_sw(params)?;

    // Phase 2: OverrideSetup
    MemWriteReq::execute(
        params.uart,
        params.test_phase_addr,
        &[TestPhase::OverrideSetup as u8],
    )?;
    sync_with_sw(params)?;

    // Phase 3: OverrideZeros
    MemWriteReq::execute(
        params.uart,
        params.test_phase_addr,
        &[TestPhase::OverrideZeros as u8],
    )?;
    sync_with_sw(params)?;
    ensure!(read_pins(params.transport, params.config)? == OUTPUT_NONE_SET);

    // Phase 4: OverrideOnes
    MemWriteReq::execute(
        params.uart,
        params.test_phase_addr,
        &[TestPhase::OverrideOnes as u8],
    )?;
    sync_with_sw(params)?;
    ensure!(read_pins(params.transport, params.config)? == OUTPUT_ALL_SET);

    // Phase 5: Done
    MemWriteReq::execute(
        params.uart,
        params.test_phase_addr,
        &[TestPhase::Done as u8],
    )?;

    let _ = UartConsole::wait_for(params.uart, r"PASS!", params.opts.timeout)?;
    Ok(())
}

fn main() -> Result<()> {
    let opts = Opts::parse();
    opts.init.init_logging();
    let transport = opts.init.init_target()?;

    let elf_binary = fs::read(&opts.firmware_elf)?;
    let elf_file = object::File::parse(&*elf_binary)?;
    let symbol = elf_file
        .symbols()
        .find(|symbol| symbol.name() == Ok("kTestPhaseReal"))
        .expect("Provided ELF missing 'kTestPhaseReal' symbol");
    assert_eq!(
        symbol.size(),
        1,
        "symbol 'kTestPhaseReal' does not have the expected size"
    );

    let uart = transport.uart("console")?;
    uart.set_flow_control(true)?;

    execute_test!(
        chip_sw_sysrst_ctrl_ec_rst_l_sim_dv,
        &Params {
            opts: &opts,
            transport: &transport,
            uart: &*uart,
            config: &CONFIG,
            test_phase_addr: symbol.address() as u32,
        }
    );
    Ok(())
}
