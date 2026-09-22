// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

use anyhow::{Context, Result, bail, ensure};
use clap::Parser;
use std::path::PathBuf;
use std::process::Command;
use std::time::Duration;

use opentitanlib::test_utils::init::InitializeTest;
use opentitanlib::transport::Capability;
use opentitanlib::uart::console::UartConsole;

use usb::{UsbDeviceHandle, UsbOpts, port_path_string};

#[derive(Debug, Parser)]
struct Opts {
    #[command(flatten)]
    init: InitializeTest,

    /// Console/USB timeout.
    #[arg(long, value_parser = humantime::parse_duration, default_value = "60s")]
    timeout: Duration,

    /// USB options.
    #[command(flatten)]
    usb: UsbOpts,

    /// Do not wait for USB device to appear before continuing with the test.
    #[arg(long, default_value_t = false)]
    no_wait_for_usb_device: bool,

    /// Executable to run after USB device connection.
    /// This harness will spawn a process to execute and continue monitoring the UART
    /// until the test passes (or fails). After that, the process will be killed.
    /// Unless `no_wait_for_usb_device` is set, the harness will pass two extra arguments
    /// to the executable to specify the bus and address of the USB device, as follows:
    /// `--devide <bus>:<addr>`.
    #[arg(long)]
    exec: Option<PathBuf>,

    /// Arguments to pass to the executable.
    #[arg(long)]
    exec_arg: Vec<std::ffi::OsString>,
}

fn wait_for_device(opts: &Opts) -> Result<UsbDeviceHandle> {
    log::info!("waiting for device...");
    let mut devices = opts.usb.wait_for_device(opts.timeout)?;
    if devices.is_empty() {
        bail!("no USB device found");
    }
    if devices.len() > 1 {
        log::error!("several USB devices found:");
        for dev in devices {
            log::error!(
                "- bus={} address={}",
                dev.device().bus_number(),
                dev.device().address()
            );
        }
        bail!("several USB devices found");
    }
    let device = devices.remove(0);
    log::info!(
        "device found at bus={}, address={}, path={}",
        device.device().bus_number(),
        device.device().address(),
        port_path_string(&device.device())?
    );
    Ok(device)
}

fn lfsr_advance(lfsr: u8) -> u8 {
    (lfsr << 1) ^ (((lfsr >> 1) ^ (lfsr >> 2) ^ (lfsr >> 3) ^ (lfsr >> 7)) & 1)
}

struct QemuStreamState {
    id: u8,
    ep_in: u8,
    ep_out: u8,
    is_iso: bool,
    sig_received: bool,
    tst_lfsr: u8,
    dpi_lfsr: u8,
    seq: u16,
    target_bytes: u32,
    transferred_bytes: u32,
}

fn run_qemu_usb_streams(
    device: &dyn opentitanlib::io::usb::UsbDevice,
    timeout: Duration,
) -> Result<()> {
    let config = device.active_configuration()?;
    let mut streams = Vec::new();
    for intf in config.interface_alt_settings() {
        let intf_desc = intf.descriptor()?;
        let s_id = intf_desc.intf_num;
        let mut ep_in = None;
        let mut ep_out = None;
        let mut is_iso = false;
        for ep in intf.endpoints() {
            let ep_desc = ep.descriptor()?;
            if (ep_desc.addr & 0x80) != 0 {
                ep_in = Some(ep_desc.addr);
                is_iso = ep_desc.transfer_type()
                    == opentitanlib::io::usb::desc::TransferType::Isochronous;
            } else {
                ep_out = Some(ep_desc.addr);
            }
        }
        if let (Some(ep_in), Some(ep_out)) = (ep_in, ep_out) {
            streams.push(QemuStreamState {
                id: s_id,
                ep_in,
                ep_out,
                is_iso,
                sig_received: false,
                tst_lfsr: 0x10u8.wrapping_add(s_id.wrapping_mul(7)),
                dpi_lfsr: 0x9bu8.wrapping_sub(s_id.wrapping_mul(7)),
                seq: 0,
                target_bytes: u32::MAX,
                transferred_bytes: 0,
            });
        }
    }

    ensure!(
        !streams.is_empty(),
        "No USB stream interfaces found on QEMU device"
    );
    log::info!(
        "Running QEMU USB stream loop for {} stream(s)",
        streams.len()
    );

    let deadline = std::time::Instant::now() + timeout;
    loop {
        let all_done = streams
            .iter()
            .all(|s| s.sig_received && s.transferred_bytes >= s.target_bytes);
        if all_done {
            break;
        }
        ensure!(
            std::time::Instant::now() < deadline,
            "Timed out running QEMU USB streams"
        );

        for s in streams.iter_mut() {
            if s.sig_received && s.transferred_bytes >= s.target_bytes {
                continue;
            }
            let mut pkt = [0u8; 64];
            let n = match device.read_bulk_timeout(s.ep_in, &mut pkt, Duration::from_micros(200)) {
                Ok(n) => n,
                Err(_) => continue,
            };
            if n == 0 {
                continue;
            }

            if s.is_iso {
                ensure!(n >= 16, "Isochronous packet too short for signature: {n}");
                let head_sig = u32::from_le_bytes(pkt[0..4].try_into().unwrap());
                let init_lfsr = pkt[4];
                let seq = u16::from_le_bytes(pkt[6..8].try_into().unwrap());
                let num_bytes = u32::from_le_bytes(pkt[8..12].try_into().unwrap());
                let tail_sig = u32::from_le_bytes(pkt[12..16].try_into().unwrap());
                ensure!(
                    head_sig == 0x579EA01A && tail_sig == 0x160AE975,
                    "Invalid stream signature on iso stream {}: head={:#x}, tail={:#x}",
                    s.id,
                    head_sig,
                    tail_sig
                );
                if !s.sig_received {
                    s.sig_received = true;
                    s.target_bytes = num_bytes;
                    s.tst_lfsr = init_lfsr;
                    s.seq = seq;
                } else if seq > s.seq {
                    s.tst_lfsr = init_lfsr;
                    s.seq = seq;
                }
                s.seq = s.seq.wrapping_add(1);

                pkt[4] = s.dpi_lfsr;
                for b in &mut pkt[16..n] {
                    ensure!(
                        *b == s.tst_lfsr,
                        "Iso stream {} data mismatch: got {:#x}, expected {:#x}",
                        s.id,
                        *b,
                        s.tst_lfsr
                    );
                    *b ^= s.dpi_lfsr;
                    s.tst_lfsr = lfsr_advance(s.tst_lfsr);
                    s.dpi_lfsr = lfsr_advance(s.dpi_lfsr);
                }
                device.write_bulk_timeout(s.ep_out, &pkt[0..n], timeout)?;
                s.transferred_bytes += (n - 16) as u32;
            } else {
                let payload = if !s.sig_received {
                    ensure!(n >= 16, "First stream packet too short for signature: {n}");
                    let head_sig = u32::from_le_bytes(pkt[0..4].try_into().unwrap());
                    let init_lfsr = pkt[4];
                    let num_bytes = u32::from_le_bytes(pkt[8..12].try_into().unwrap());
                    let tail_sig = u32::from_le_bytes(pkt[12..16].try_into().unwrap());
                    ensure!(
                        head_sig == 0x579EA01A && tail_sig == 0x160AE975,
                        "Invalid stream signature on stream {}: head={:#x}, tail={:#x}",
                        s.id,
                        head_sig,
                        tail_sig
                    );
                    s.sig_received = true;
                    s.target_bytes = num_bytes;
                    s.tst_lfsr = init_lfsr;
                    &mut pkt[16..n]
                } else {
                    &mut pkt[0..n]
                };

                for b in payload.iter_mut() {
                    ensure!(
                        *b == s.tst_lfsr,
                        "Stream {} data mismatch: got {:#x}, expected {:#x}",
                        s.id,
                        *b,
                        s.tst_lfsr
                    );
                    *b ^= s.dpi_lfsr;
                    s.tst_lfsr = lfsr_advance(s.tst_lfsr);
                    s.dpi_lfsr = lfsr_advance(s.dpi_lfsr);
                }
                if !payload.is_empty() {
                    device.write_bulk_timeout(s.ep_out, payload, timeout)?;
                    s.transferred_bytes += payload.len() as u32;
                }
            }
        }
    }

    log::info!("Completed QEMU USB stream transfers");
    Ok(())
}

fn main() -> Result<()> {
    let opts = Opts::parse();
    opts.init.init_logging();
    let transport = opts.init.init_target()?;

    transport
        .capabilities()?
        .request(Capability::USB)
        .ok()
        .context("This transport does not support USB")?;

    // Certain backends such as QEMU will not enumerate USB device until
    // we request the USB context.
    let _usb_context = transport.usb().context("Cannot get USB context")?;

    // Wait until test is running.
    let uart = transport.uart("console")?;
    UartConsole::wait_for(&*uart, r"Running [^\r\n]*", opts.timeout)?;

    opts.usb.apply_strappings(&transport, true)?;
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
        let qemu_dev = if opts.no_wait_for_usb_device {
            None
        } else {
            Some(_usb_context.device_by_id_with_timeout(
                opts.usb.vid,
                opts.usb.pid,
                None,
                opts.timeout,
            )?)
        };

        if opts.exec.is_some()
            && let Some(ref dev) = qemu_dev
        {
            run_qemu_usb_streams(&**dev, opts.timeout)?;
        } else if let Some(ref dev) = qemu_dev {
            let mut in_only_eps = Vec::new();
            if let Ok(config) = dev.active_configuration() {
                for intf in config.interface_alt_settings() {
                    let mut ep_in = None;
                    let mut ep_out = None;
                    for ep in intf.endpoints() {
                        if let Ok(ep_desc) = ep.descriptor() {
                            if (ep_desc.addr & 0x80) != 0 {
                                ep_in = Some(ep_desc.addr);
                            } else {
                                ep_out = Some(ep_desc.addr);
                            }
                        }
                    }
                    if let (Some(addr), None) = (ep_in, ep_out) {
                        in_only_eps.push(addr);
                    }
                }
            }
            if !in_only_eps.is_empty() {
                log::info!(
                    "Draining {} QEMU USB IN-only logging stream(s) while waiting for pass...",
                    in_only_eps.len()
                );
                let deadline = std::time::Instant::now() + opts.timeout;
                let mut buf = [0u8; 512];
                let mut uart_buf = [0u8; 256];
                let mut uart_acc = String::new();
                let mut total_usb_bytes = 0usize;
                loop {
                    ensure!(
                        std::time::Instant::now() < deadline,
                        "Timed out draining QEMU USB IN-only logging streams (read {total_usb_bytes} USB bytes, uart={uart_acc:?})"
                    );
                    for &ep in &in_only_eps {
                        while let Ok(n) =
                            dev.read_bulk_timeout(ep, &mut buf, Duration::from_micros(200))
                        {
                            if n == 0 {
                                break;
                            }
                            total_usb_bytes += n;
                        }
                    }
                    while let Ok(n) = uart.read_timeout(&mut uart_buf, Duration::ZERO) {
                        if n == 0 {
                            break;
                        }
                        let s = String::from_utf8_lossy(&uart_buf[..n]);
                        print!("{s}");
                        uart_acc.push_str(&s);
                        if uart_acc.contains("PASS!") {
                            log::info!(
                                "Detected PASS! on UART after draining {total_usb_bytes} USB bytes"
                            );
                            return Ok(());
                        }
                        if uart_acc.contains("FAIL!") || uart_acc.contains("FAULT") {
                            bail!("device code reported a failure: {uart_acc}");
                        }
                    }
                }
            }
        }

        log::info!("wait for pass...");
        let res = UartConsole::wait_for(&*uart, r"PASS|FAIL", opts.timeout)?;
        match res[0].as_str() {
            "PASS" => (),
            "FAIL" => bail!("device code reported a failure"),
            _ => (),
        };
        return Ok(());
    }

    // Wait for USB device to appear.
    let device = if opts.no_wait_for_usb_device {
        None
    } else {
        Some(wait_for_device(&opts)?)
    };

    // Run executable if requested.
    let child = match opts.exec {
        Some(exec) => {
            let mut cmd = Command::new(exec);
            if let Some(device) = device {
                cmd.arg("--device").arg(format!(
                    "{}:{}",
                    device.device().bus_number(),
                    device.device().address()
                ));
            }
            cmd.args(opts.exec_arg);
            log::info!(
                "calling {:?} on {:?}",
                cmd.get_program(),
                cmd.get_args().collect::<Vec<_>>()
            );
            Some(cmd.spawn().context("could not start executable")?)
        }
        None => None,
    };

    // Wait for test to pass.
    log::info!("wait for pass...");
    let res = UartConsole::wait_for(&*uart, r"PASS|FAIL", opts.timeout)?;
    match res[0].as_str() {
        "PASS" => (),
        "FAIL" => bail!("device code reported a failure"),
        _ => (),
    };

    // Kill executable (if running).
    if let Some(mut child) = child {
        match child.try_wait() {
            Ok(Some(status)) => log::info!("executable exited with: {status}"),
            Ok(None) => {
                log::info!("executable did not finish and will be killed");
                let _ = child.kill();
            }
            Err(e) => {
                println!("error attempting to get executable status: {e}");
                log::info!("killing executable");
                let _ = child.kill();
            }
        }
    }

    Ok(())
}
