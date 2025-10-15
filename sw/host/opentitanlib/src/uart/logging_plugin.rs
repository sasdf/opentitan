// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

use anyhow::Result;
use std::fs::File;
use std::io::Write;
use std::time::SystemTime;

use crate::uart::console::ExitStatus;
use crate::uart::console_plugin::ConsolePlugin;

pub struct LoggingPlugin {
    logfile: Option<File>,
    timestamp: bool,
    newline: bool,
    carriage_return: bool,
    pub quiet: bool,
}

impl Default for LoggingPlugin {
    fn default() -> Self {
        Self {
            logfile: None,
            timestamp: false,
            newline: true, // Start with a newline for the first timestamp
            carriage_return: false,
            quiet: false,
        }
    }
}

impl LoggingPlugin {
    pub fn logfile(mut self, logfile: Option<File>) -> Self {
        self.logfile = logfile;
        self
    }

    pub fn timestamp(mut self, timestamp: bool) -> Self {
        self.timestamp = timestamp;
        self
    }

    pub fn quiet(mut self, quiet: bool) -> Self {
        self.quiet = quiet;
        self
    }
}

impl ConsolePlugin for LoggingPlugin {
    fn process_bytes(&mut self, bytes: Vec<u8>) -> Result<Vec<u8>> {
        if self.quiet {
            return Ok(bytes);
        }

        // If we're logging, write to the logfile and stdout.
        if self.logfile.is_some() || self.timestamp {
            let mut stdout = std::io::stdout();
            for byte in bytes.iter() {
                if self.timestamp && self.newline {
                    let t = humantime::format_rfc3339_millis(SystemTime::now());
                    stdout.write_fmt(format_args!("[{}  console]", t))?;
                    self.newline = false;
                }
                self.newline = *byte == b'\n';
                stdout.write_all(if self.newline && !self.carriage_return {
                    b"\r\n"
                } else {
                    std::slice::from_ref(byte)
                })?;
                self.carriage_return = *byte == b'\r';
            }
            stdout.flush()?;

            self.logfile
                .as_mut()
                .map_or(Ok(()), |f| f.write_all(&bytes))?;
        }
        Ok(bytes)
    }

    fn exit_status(&self) -> Option<ExitStatus> {
        None
    }
}
