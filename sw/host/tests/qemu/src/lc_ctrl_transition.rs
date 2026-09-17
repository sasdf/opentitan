// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

//! Host test harness for `lc_ctrl_transition_test` and `clkmgr_external_clk_src_for_lc_test`.
//!
//! Matches the DV sequence (`chip_sw_lc_ctrl_transition_vseq.sv`):
//! 1. Device boots with OTP in `TEST_LOCKED1` (`lc_count = 8`). CPU execution is disabled.
//! 2. Host connects to `JtagTap::LcTap`, verifies `TEST_LOCKED1` and `count == 8`, and triggers
//!    JTAG transition to `TEST_UNLOCKED2` (`count = 9`) using `TEST_UNLOCK_TOKEN_PREIMAGE`.
//! 3. Upon reset into `TEST_UNLOCKED2`, Ibex CPU boots ROM and executes `lc_ctrl_transition_test.c`,
//!    which verifies `state == TEST_UNLOCKED2` and `count == 9`, configures transition to `DEV`
//!    using `kLcExitToken`, starts transition, releases mutex, and logs
//!    `"Waiting for LC transition done and reboot."`.
//! 4. Host connects to `JtagTap::LcTap`, claims transition interface, verifies
//!    `STATUS.TRANSITION_SUCCESSFUL`, checks `TRANSITION_CTRL.EXT_CLOCK_EN` matches expected
//!    `expect_ext_clk`, and resets the chip.
//! 5. Device resets into `DEV` (`count = 10`), device SW verifies `state == DEV` and `count == 10`,
//!    and reports `PASS!`.

use std::time::Duration;

use anyhow::{Context, Result, ensure};
use clap::Parser;
use opentitanlib::app::UartRx;
use opentitanlib::dif::lc_ctrl::{
    DifLcCtrlState, LcCtrlReg, LcCtrlStatus, LcCtrlTransitionCtrl,
};
use opentitanlib::io::jtag::JtagTap;
use opentitanlib::test_utils::init::InitializeTest;
use opentitanlib::test_utils::lc_transition;
use opentitanlib::uart::console::UartConsole;

const TEST_UNLOCK_TOKEN_PREIMAGE: [u32; 4] = [0x11111111, 0x11111111, 0x11111111, 0x11111111];

#[derive(Debug, Parser)]
struct Opts {
    #[command(flatten)]
    init: InitializeTest,

    /// Whether software is expected to enable external clock during transition to DEV.
    #[arg(long, default_value_t = false)]
    expect_ext_clk: bool,

    #[arg(long, value_parser = humantime::parse_duration, default_value = "60s")]
    pub timeout: Duration,
}

fn main() -> Result<()> {
    let opts = Opts::parse();
    opts.init.init_logging();

    let transport = opts.init.init_target()?;
    let uart = transport.uart("console")?;

    // Phase 1: Device starts in TEST_LOCKED1 (count = 8). CPU is disabled.
    log::info!("Phase 1: Connecting to LC TAP in TEST_LOCKED1 state...");
    transport.pin_strapping("PINMUX_TAP_LC")?.apply()?;
    transport.reset(UartRx::Clear)?;

    let mut jtag = opts
        .init
        .jtag_params
        .create(&transport)?
        .connect(JtagTap::LcTap)
        .context("Failed to connect to LC TAP in TEST_LOCKED1")?;

    let state = jtag.read_lc_ctrl_reg(&LcCtrlReg::LcState)?;
    ensure!(
        state == DifLcCtrlState::TestLocked1.redundant_encoding(),
        "Expected initial LC state TEST_LOCKED1, got 0x{:x}",
        state
    );
    let cnt = jtag.read_lc_ctrl_reg(&LcCtrlReg::LcTransitionCnt)?;
    ensure!(cnt == 8, "Expected initial LC transition count 8, got {}", cnt);

    log::info!("Triggering JTAG transition from TEST_LOCKED1 to TEST_UNLOCKED2...");
    uart.clear_rx_buffer()?;
    lc_transition::trigger_lc_transition(
        &transport,
        jtag,
        DifLcCtrlState::TestUnlocked2,
        Some(TEST_UNLOCK_TOKEN_PREIMAGE),
        /*use_external_clk=*/ true,
        /*reset_tap_straps=*/ Some(JtagTap::LcTap),
    )
    .context("Failed JTAG transition to TEST_UNLOCKED2")?;

    // Phase 2: Device resets into TEST_UNLOCKED2 (count = 9) with PINMUX_TAP_LC strapped.
    log::info!("Phase 2: Waiting for SW to initiate transition to DEV...");
    _ = UartConsole::wait_for(
        &*uart,
        r"Waiting for LC transition done and reboot\.",
        opts.timeout,
    )
    .context("Failed waiting for SW to initiate transition from TEST_UNLOCKED2 to DEV")?;

    // Connect to LC TAP over JTAG without resetting chip.
    let mut jtag = opts
        .init
        .jtag_params
        .create(&transport)?
        .connect(JtagTap::LcTap)
        .context("Failed to connect to LC TAP after SW transition request")?;

    lc_transition::claim_lc_mutex(&mut *jtag, true)
        .context("Failed to claim LC mutex from JTAG")?;

    let status_bits = jtag.read_lc_ctrl_reg(&LcCtrlReg::Status)?;
    let status = LcCtrlStatus::from_bits_truncate(status_bits);
    ensure!(
        status.contains(LcCtrlStatus::TRANSITION_SUCCESSFUL),
        "Expected TRANSITION_SUCCESSFUL in LC STATUS, got 0x{:x}",
        status_bits
    );

    let ctrl_bits = jtag.read_lc_ctrl_reg(&LcCtrlReg::TransitionCtrl)?;
    let ext_clk_en = (ctrl_bits & LcCtrlTransitionCtrl::EXT_CLOCK_EN.bits()) != 0;
    ensure!(
        ext_clk_en == opts.expect_ext_clk,
        "Expected TransitionCtrl.EXT_CLOCK_EN == {}, got {}",
        opts.expect_ext_clk,
        ext_clk_en
    );

    let post_state = jtag.read_lc_ctrl_reg(&LcCtrlReg::LcState)?;
    ensure!(
        post_state == DifLcCtrlState::PostTransition.redundant_encoding(),
        "Expected PostTransition state after SW transition, got 0x{:x}",
        post_state
    );

    jtag.disconnect()?;
    transport.pin_strapping("PINMUX_TAP_LC")?.remove()?;

    // Phase 3: Reset chip into DEV state (count = 10) and wait for SW test PASS.
    log::info!("Phase 3: Resetting chip into DEV state and waiting for PASS...");
    uart.clear_rx_buffer()?;
    transport.reset(UartRx::Clear)?;

    _ = UartConsole::wait_for(&*uart, r"PASS!.*", opts.timeout)
        .context("Failed waiting for test PASS in DEV state")?;

    log::info!("LC transition test completed successfully.");
    Ok(())
}
