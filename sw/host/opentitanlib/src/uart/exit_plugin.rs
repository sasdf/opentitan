// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

use anyhow::Result;
use regex::{Captures, Regex};
use std::collections::VecDeque;

use crate::uart::console::ExitStatus;
use crate::uart::console_plugin::ConsolePlugin;

#[derive(Default)]
pub struct ExitPlugin {
    exit_success: Option<Regex>,
    exit_failure: Option<Regex>,
    buffer: VecDeque<u8>,
    exit_status: Option<ExitStatus>,
}

impl ExitPlugin {
    const BUFFER_LEN: usize = 32768;

    pub fn set_exit_success(mut self, exit_success: Option<Regex>) -> Self {
        self.exit_success = exit_success;
        self
    }

    pub fn set_exit_failure(mut self, exit_failure: Option<Regex>) -> Self {
        self.exit_failure = exit_failure;
        self
    }

    /// Returns a reference to the currently active buffer.
    fn buffer_str(&self) -> &str {
        std::str::from_utf8(self.buffer.as_slices().0).unwrap_or("")
    }

    fn process_exit_regex(&self) -> Option<ExitStatus> {
        let buffer_str = self.buffer_str();

        if self.exit_success.as_ref().map(|rx| rx.is_match(buffer_str)) == Some(true) {
            return Some(ExitStatus::ExitSuccess);
        }
        if self.exit_failure.as_ref().map(|rx| rx.is_match(buffer_str)) == Some(true) {
            return Some(ExitStatus::ExitFailure);
        }
        None
    }

    pub fn captures(&self, status: ExitStatus) -> Option<Captures> {
        let buffer_str = self.buffer_str();
        match status {
            ExitStatus::ExitSuccess => self
                .exit_success
                .as_ref()
                .and_then(|rx| rx.captures(buffer_str)),
            ExitStatus::ExitFailure => self
                .exit_failure
                .as_ref()
                .and_then(|rx| rx.captures(buffer_str)),
            _ => None,
        }
    }
}

impl ConsolePlugin for ExitPlugin {
    fn process_bytes(&mut self, bytes: Vec<u8>) -> Result<Vec<u8>> {
        for byte in &bytes {
            self.buffer.push_back(*byte);
            if self.buffer.len() > Self::BUFFER_LEN {
                self.buffer.pop_front();
            }

            self.exit_status = self.process_exit_regex();
            if self.exit_status.is_some() {
                break;
            }
        }
        Ok(bytes)
    }

    fn exit_status(&self) -> Option<ExitStatus> {
        self.exit_status
    }
}
