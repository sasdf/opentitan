// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

use anyhow::Result;
use clap::Args;
use std::any::Any;

use opentitanlib::app::command::CommandDispatch;
use opentitanlib::app::{StagedProgressBar, TransportWrapper};
use opentitanlib::bootstrap::{Bootstrap, BootstrapOptions};
use opentitanlib::transport::common::fpga;
use opentitanlib::util::parse_int::ParseInt;

/// Clear the instrumented ROM.
#[derive(Debug, Args)]
pub struct ClearInstrumentedRomCommand {
    #[command(flatten)]
    bootstrap_options: BootstrapOptions,
    /// The byte offset to the instrumented rom slot.
    #[arg(long, value_parser = usize::from_str)]
    slot: usize,
    /// The byte offset to the magic bytes in the instrumented rom.
    #[arg(long, value_parser = usize::from_str, default_value = "0x180")]
    magic_bytes_offset: usize,
}

impl ClearInstrumentedRomCommand {
    fn clear_by_bootstrap(&self, transport: &TransportWrapper) -> Result<()> {
        // Pad to magic bytes
        let mut payload = vec![0xff; self.slot + self.magic_bytes_offset];
        // Clear magic bytes with zeros
        payload.extend_from_slice(&[0x00; 4]);

        let progress = StagedProgressBar::new();
        Bootstrap::update_with_progress(transport, &self.bootstrap_options, &payload, &progress)
    }
}

impl CommandDispatch for ClearInstrumentedRomCommand {
    fn run(
        &self,
        _context: &dyn Any,
        transport: &TransportWrapper,
    ) -> Result<Option<Box<dyn erased_serde::Serialize>>> {
        if let Err(e) = self.clear_by_bootstrap(transport) {
            log::warn!(
                "Could not clear instrumented ROM via bootstrap, falling back to clear bitstream: {e}"
            );
            transport.dispatch(&fpga::ClearBitstream)?;
        }
        Ok(None)
    }
}
