// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

use anyhow::Result;
use regex::{Captures, Regex};

// use crate::uart::console_plugin::ConsolePlugin;

/// ExitSuccess provides the exit status from a console.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ExitStatus {
    None,
    CtrlC,
    Timeout,
    ExitSuccess,
    ExitFailure,
}

/*
#[derive(Default)]
pub struct ExitStatusPlugin {
    pub exit_success: Option<Regex>,
    pub exit_failure: Option<Regex>,
    buffer: Vec<u8>,
    buffer_len: usize,
    exit_status: Option<ExitStatus>,
}

impl ExitStatusPlugin {
    pub fn new(exit_success: Option<Regex>, exit_failure: Option<Regex>, buffer_len: usize) -> Self {
        Self {
            exit_success,
            exit_failure,
            buffer_len,
            ..Default::default()
        }
    }

    /// Returns a reference to the currently active buffer (normal or coverage).
    fn get_active_buffer(&self) -> &str {
        std::str::from_utf8(&self.buffer).unwrap_or("")
    }

    fn process_exit_regex(&self) -> Option<ExitStatus> {
        let active_buffer = self.get_active_buffer();

        if self
            .exit_success
            .as_ref()
            .map(|rx| rx.is_match(active_buffer))
            == Some(true)
        {
            return Some(ExitStatus::ExitSuccess);
        }
        if self
            .exit_failure
            .as_ref()
            .map(|rx| rx.is_match(active_buffer))
            == Some(true)
        {
            return Some(ExitStatus::ExitFailure);
        }
        None
    }

    pub fn captures(&self, status: ExitStatus) -> Option<Captures> {
        let active_buffer = self.get_active_buffer();
        match status {
            ExitStatus::ExitSuccess => self
                .exit_success
                .as_ref()
                .and_then(|rx| rx.captures(active_buffer)),
            ExitStatus::ExitFailure => self
                .exit_failure
                .as_ref()
                .and_then(|rx| rx.captures(active_buffer)),
            _ => None,
        }
    }
}

impl ConsolePlugin for ExitStatusPlugin {
    fn process_bytes(&mut self, mut bytes: Vec<u8>) -> Result<Vec<u8>> {
        self.buffer.append(&mut bytes);
        while self.buffer.len() > self.buffer_len {
            self.buffer.remove(0);
        }
        self.exit_status = self.process_exit_regex();
        Ok(std::mem::take(&mut self.buffer))
    }

    fn get_exit_status(&self) -> Option<ExitStatus> {
        self.exit_status
    }
}
*/
