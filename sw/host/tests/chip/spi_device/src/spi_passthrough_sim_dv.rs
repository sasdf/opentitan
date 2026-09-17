// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

use anyhow::{Result, bail, ensure};
use clap::Parser;
use std::rc::Rc;
use std::time::{Duration, Instant};

use opentitanlib::app::TransportWrapper;
use opentitanlib::execute_test;
use opentitanlib::io::spi::{Target, Transfer};
use opentitanlib::spiflash::SpiFlash;
use opentitanlib::test_utils::init::InitializeTest;
use opentitanlib::uart::console::UartConsole;

const FLASH_STATUS_WIP: u8 = 0x01;

#[derive(Debug, Parser)]
struct Opts {
    #[command(flatten)]
    init: InitializeTest,

    /// Console receive timeout.
    #[arg(long, value_parser = humantime::parse_duration, default_value = "600s")]
    timeout: Duration,

    /// Name of the debugger's SPI interface.
    #[arg(long, default_value = "BOOTSTRAP")]
    spi: String,
}

fn wait_until_not_busy(spi: &Rc<dyn Target>, timeout: Duration) -> Result<()> {
    let start = Instant::now();
    loop {
        let mut status = [0u8; 1];
        spi.run_transaction(&mut [
            Transfer::Write(&[SpiFlash::READ_STATUS]),
            Transfer::Read(&mut status),
        ])?;
        if status[0] & FLASH_STATUS_WIP == 0 {
            return Ok(());
        }
        if start.elapsed() > timeout {
            bail!("Timed out waiting for SPI flash WIP bit to clear");
        }
        std::thread::sleep(Duration::from_millis(5));
    }
}

fn test_spi_passthrough_sim_dv(opts: &Opts, transport: &TransportWrapper) -> Result<()> {
    let uart = transport.uart("console")?;
    let spi = transport.spi(&opts.spi)?;

    let _ = UartConsole::wait_for(&*uart, r"Test setup complete\.", opts.timeout)?;

    // 1. Uploaded command 1: WriteStatus1 (0x01)
    log::info!("Testing uploaded WriteStatus1");
    spi.run_transaction(&mut [Transfer::Write(&[SpiFlash::WRITE_ENABLE])])?;
    spi.run_transaction(&mut [Transfer::Write(&[SpiFlash::WRITE_STATUS, 0x00])])?;
    wait_until_not_busy(&spi, opts.timeout)?;

    // 2. Uploaded command 2: SectorErase (0x20) at address 0x000000
    log::info!("Testing uploaded SectorErase + passthrough ReadNormal");
    spi.run_transaction(&mut [Transfer::Write(&[SpiFlash::WRITE_ENABLE])])?;
    spi.run_transaction(&mut [Transfer::Write(&[0x20, 0x00, 0x00, 0x00])])?;
    wait_until_not_busy(&spi, opts.timeout)?;

    let mut read_buf = [0u8; 16];
    spi.run_transaction(&mut [
        Transfer::Write(&[SpiFlash::READ, 0x00, 0x00, 0x00]),
        Transfer::Read(&mut read_buf),
    ])?;
    ensure!(
        read_buf == [0xffu8; 16],
        "Expected erased sector (all 0xFF), got {:02x?}",
        read_buf
    );

    // 3. Uploaded command 3: PageProgram (0x02) at address 0x000000
    log::info!("Testing uploaded PageProgram + passthrough ReadNormal & ReadFast");
    let expected_data: [u8; 16] = [
        0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae,
        0xaf,
    ];
    let mut prog_cmd = vec![SpiFlash::PAGE_PROGRAM, 0x00, 0x00, 0x00];
    prog_cmd.extend_from_slice(&expected_data);

    spi.run_transaction(&mut [Transfer::Write(&[SpiFlash::WRITE_ENABLE])])?;
    spi.run_transaction(&mut [Transfer::Write(&prog_cmd)])?;
    wait_until_not_busy(&spi, opts.timeout)?;

    // Verify via ReadNormal (0x03)
    read_buf.fill(0);
    spi.run_transaction(&mut [
        Transfer::Write(&[SpiFlash::READ, 0x00, 0x00, 0x00]),
        Transfer::Read(&mut read_buf),
    ])?;
    ensure!(
        read_buf == expected_data,
        "ReadNormal mismatch: expected {:02x?}, got {:02x?}",
        expected_data,
        read_buf
    );

    // Verify via ReadFast (0x0B) with 8 dummy cycles (1 dummy byte)
    read_buf.fill(0);
    spi.run_transaction(&mut [
        Transfer::Write(&[SpiFlash::FAST_READ, 0x00, 0x00, 0x00, 0x00]),
        Transfer::Read(&mut read_buf),
    ])?;
    ensure!(
        read_buf == expected_data,
        "ReadFast mismatch: expected {:02x?}, got {:02x?}",
        expected_data,
        read_buf
    );

    // 4. Uploaded command 4: ChipErase (0xC7)
    log::info!("Testing uploaded ChipErase + passthrough ReadNormal");
    spi.run_transaction(&mut [Transfer::Write(&[SpiFlash::WRITE_ENABLE])])?;
    spi.run_transaction(&mut [Transfer::Write(&[SpiFlash::CHIP_ERASE])])?;
    wait_until_not_busy(&spi, opts.timeout)?;

    read_buf.fill(0);
    spi.run_transaction(&mut [
        Transfer::Write(&[SpiFlash::READ, 0x00, 0x00, 0x00]),
        Transfer::Read(&mut read_buf),
    ])?;
    ensure!(
        read_buf == [0xffu8; 16],
        "Expected chip erased (all 0xFF), got {:02x?}",
        read_buf
    );

    // 5. Uploaded command 5: Final WriteStatus1 to signal completion to firmware
    log::info!("Sending final WriteStatus1 completion command");
    spi.run_transaction(&mut [Transfer::Write(&[SpiFlash::WRITE_ENABLE])])?;
    spi.run_transaction(&mut [Transfer::Write(&[SpiFlash::WRITE_STATUS, 0x00])])?;

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

    execute_test!(test_spi_passthrough_sim_dv, &opts, &transport);
    Ok(())
}
