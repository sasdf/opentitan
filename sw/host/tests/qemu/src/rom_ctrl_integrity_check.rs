// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

//! Test harness for `rom_ctrl_integrity_check_test`.
//!
//! 1. Phase 1 (Non-PROD LC state with corrupted ROM digest):
//!    - QEMU starts with `-global ot-rom_ctrl.corrupt-digest=true` and non-PROD OTP (`RMA`).
//!    - Because `lc_dft_en` and `lc_hw_debug_en` are both high in non-PROD (`TEST_UNLOCKED*` / `RMA`),
//!      `pwrmgr` bypasses the failed `rom_ctrl_good` check and releases CPU reset (`cpu_enable`).
//!    - Device software (`rom_ctrl_integrity_check_test.c`) verifies `lc_state != PROD` and
//!      `expected_digest != computed_digest`, logs `"Waiting for interrupt."`, and enters WFI.
//! 2. Phase 2 (Transition to PROD LC state and reset):
//!    - Overwrites `otp_img.mut.raw` in-place with `--prod-otp` (`img_prod`) and resets the target.
//!    - On reset, `otp_ctrl` reloads `img_prod`, `lc_ctrl` broadcasts `lc_dft_en = 0` and
//!      `lc_hw_debug_en = 0`, so `pwrmgr` enforces `rom_ctrl_good` (`false`) and holds CPU reset.
//!    - Verifies that no UART output is produced after reset in PROD state.

use std::io::Write;
use std::path::PathBuf;
use std::time::Duration;

use anyhow::{Context, Result, anyhow};
use clap::Parser;
use opentitanlib::test_utils::init::InitializeTest;
use opentitanlib::uart::console::UartConsole;

#[derive(Debug, Parser)]
struct Opts {
    #[command(flatten)]
    init: InitializeTest,

    /// Path to the PROD OTP raw image (`//hw/ip/otp_ctrl/data:img_prod`).
    #[arg(long)]
    prod_otp: PathBuf,

    #[arg(long, value_parser = humantime::parse_duration, default_value = "60s")]
    pub timeout: Duration,
}

fn main() -> Result<()> {
    let opts = Opts::parse();
    opts.init.init_logging();

    let transport = opts.init.init_target()?;
    let uart = transport.uart("console")?;

    log::info!("Phase 1: Waiting for device to verify corrupted digest in non-PROD state...");
    _ = UartConsole::wait_for(&*uart, r"Waiting for interrupt\.", opts.timeout)
        .context("Failed waiting for device to reach WFI in non-PROD LC state")?;

    log::info!("Phase 2: Updating OTP backend to PROD state and resetting target...");
    let prod_bytes = std::fs::read(&opts.prod_otp)
        .with_context(|| format!("Failed to read prod OTP image {:?}", opts.prod_otp))?;
    let mut otp_file = std::fs::OpenOptions::new()
        .write(true)
        .truncate(true)
        .open("otp_img.mut.raw")
        .context("Failed to open otp_img.mut.raw for in-place update")?;
    otp_file.write_all(&prod_bytes)?;
    otp_file.sync_all()?;
    drop(otp_file);

    // Clear any residual UART buffer before reset.
    uart.clear_rx_buffer()?;

    // Reset target so OTP reloads `img_prod` and `pwrmgr` re-evaluates `rom_ctrl_good`.
    transport.reset_target(Duration::from_millis(10), true)?;

    // In PROD state with a corrupted ROM digest, `pwrmgr` must hold CPU fetch disabled.
    log::info!("Verifying CPU does not boot in PROD state with corrupted ROM digest...");
    match UartConsole::wait_for(&*uart, r".+", Duration::from_millis(250)) {
        Ok(matched) => Err(anyhow!(
            "CPU unexpectedly booted and produced UART output in PROD state: {:?}",
            matched
        )),
        Err(_) => {
            log::info!("Verified: CPU held in reset by pwrmgr in PROD state with corrupted ROM digest.");
            Ok(())
        }
    }
}
