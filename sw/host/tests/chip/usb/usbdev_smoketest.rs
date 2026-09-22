// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

use anyhow::Context;
use anyhow::{Result, bail, ensure};
use clap::Parser;
use std::time::Duration;

use opentitanlib::app::TransportWrapper;
use opentitanlib::execute_test;
use opentitanlib::io::uart::Uart;
use opentitanlib::test_utils::init::InitializeTest;
use opentitanlib::transport::common::uart::SerialPortUart;
use opentitanlib::uart::console::UartConsole;

use usb::UsbOpts;

#[derive(Debug, Parser)]
struct Opts {
    #[command(flatten)]
    init: InitializeTest,

    /// Console/USB timeout.
    #[arg(long, value_parser = humantime::parse_duration, default_value = "10s")]
    timeout: Duration,

    /// USB options.
    #[command(flatten)]
    usb: UsbOpts,
}

fn usbdev_echo_qemu(opts: &Opts, transport: &TransportWrapper, uart: &dyn Uart) -> Result<()> {
    log::info!("waiting for device...");
    let usb_ctx = transport.usb().context("Cannot get USB context")?;
    let device =
        usb_ctx.device_by_id_with_timeout(opts.usb.vid, opts.usb.pid, None, opts.timeout)?;
    let mut buffer = [0u8; 256];
    let mut total_read = 0;
    let deadline = std::time::Instant::now() + opts.timeout;
    while total_read < 6 && std::time::Instant::now() < deadline {
        match device.read_bulk_timeout(0x81, &mut buffer[total_read..], Duration::from_millis(500))
        {
            Ok(n) if n > 0 => total_read += n,
            _ => {}
        }
    }
    ensure!(
        total_read > 0,
        "Bulk read from EP1 returned 0 bytes on QEMU USB device"
    );
    device.write_bulk_timeout(0x01, &buffer[0..total_read], opts.timeout)?;
    let _ = UartConsole::wait_for(uart, r"PASS!", opts.timeout)?;
    Ok(())
}

fn usbdev_echo(opts: &Opts, uart: &dyn Uart) -> Result<()> {
    log::info!("waiting for device...");
    let devices = opts.usb.wait_for_device(opts.timeout)?;
    if devices.is_empty() {
        bail!("no USB device found");
    }

    let ports = serialport::available_ports()?;
    //  In CI it is possible for several devices to match because we don't use a unique
    //  VID and PID, and all devices are visible (but not accessible) to every program.
    //  So with filter all vid:pid matches to try to open them: the ones belonging to
    //  other containers cannot be opened.
    let usb_uart = ports
        .iter()
        .find_map(|port| {
            let serialport::SerialPortType::UsbPort(info) = &port.port_type else {
                return None;
            };
            if info.vid != opts.usb.vid || info.pid != opts.usb.pid {
                return None;
            }
            SerialPortUart::open(&port.port_name, 115200).ok()
        })
        .ok_or(anyhow::anyhow!("Could not open any tty port"))?;

    let mut buffer = [0u8; 256];
    let len = usb_uart.read(&mut buffer)?;

    usb_uart.write(&buffer[0..len])?;

    let _ = UartConsole::wait_for(uart, r"PASS!", opts.timeout)?;
    Ok(())
}

fn main() -> Result<()> {
    let opts = Opts::parse();
    opts.init.init_logging();
    let transport = opts.init.init_target()?;

    let uart = transport.uart("console")?;
    let _ = UartConsole::wait_for(&*uart, r"Running [^\r\n]*", opts.timeout)?;

    // Enable VBUS sense on the board if necessary.
    if opts.usb.vbus_control_available() {
        opts.usb.enable_vbus(&transport, true)?;
    }
    // Sense VBUS if available.
    if opts.usb.vbus_sense_available() {
        ensure!(
            opts.usb.vbus_present(&transport)?,
            "OT USB does not appear to be connected to a host (VBUS not detected)"
        );
    }

    if opts.init.backend_opts.interface == "qemu" {
        execute_test!(usbdev_echo_qemu, &opts, &transport, &*uart);
        return Ok(());
    }
    execute_test!(usbdev_echo, &opts, &*uart);

    Ok(())
}
