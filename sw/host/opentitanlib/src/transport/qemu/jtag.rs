// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

use std::cell::RefCell;
use std::path::PathBuf;
use std::rc::Rc;

use crate::debug::openocd::{OpenOcd, OpenOcdJtagChain};
use crate::io::jtag::{Jtag, JtagChain, JtagParams, JtagTap};
use crate::transport::qemu::gpio::QemuGpio;

/// Proxy between the two available JTAG TAPs in QEMU.
///
/// The QEMU model of OpenTitan does not currently implement the single multiplexed
/// JTAG socket that hardware has, instead both sockets are always visible. We defer
/// connecting OpenOCD to the socket until we know which TAP we're connecting to.
pub struct QemuJtag {
    /// Params to be forwarded to [`OpenOcdJtagChain`]
    params: JtagParams,

    /// Remote bitbang socket for the debug module.
    rv_dm_sock: PathBuf,

    /// Remote bitbang socket for the lifecycle controller.
    lc_ctrl_sock: PathBuf,

    /// Optional reference to QEMU GPIO state to inspect TAP strap pins.
    gpio: Option<Rc<RefCell<QemuGpio>>>,
}

impl QemuJtag {
    pub fn new(
        mut params: JtagParams,
        rv_dm_sock: PathBuf,
        lc_ctrl_sock: PathBuf,
        gpio: Option<Rc<RefCell<QemuGpio>>>,
    ) -> QemuJtag {
        params.log_stdio = true;
        QemuJtag {
            params,
            rv_dm_sock,
            lc_ctrl_sock,
            gpio,
        }
    }
}

impl JtagChain for QemuJtag {
    fn connect(self: Box<Self>, tap: JtagTap) -> anyhow::Result<Box<dyn Jtag>> {
        let sock = match tap {
            JtagTap::RiscvTap => self.rv_dm_sock,
            JtagTap::LcTap => self.lc_ctrl_sock,
        };

        let adapter_cmd = format!(
            "adapter driver remote_bitbang; remote_bitbang port 0; remote_bitbang host {sock}",
            sock = sock.display(),
        );

        let mut last_err = None;
        for attempt in 0..3 {
            if attempt > 0 {
                std::thread::sleep(std::time::Duration::from_millis(50));
            }
            match OpenOcdJtagChain::new(&adapter_cmd, &self.params)
                .and_then(|openocd| Box::new(openocd).connect(tap))
            {
                Ok(jtag) => return Ok(jtag),
                Err(e) => last_err = Some(e),
            }
        }
        Err(last_err.unwrap())
    }

    fn into_raw(self: Box<Self>) -> anyhow::Result<OpenOcd> {
        let (strap0, strap1) = if let Some(ref gpio) = self.gpio {
            let gpio = gpio.borrow();
            let s0 =
                (gpio.host_output_enable & (1 << 30)) != 0 && (gpio.host_to_qemu & (1 << 30)) != 0;
            let s1 =
                (gpio.host_output_enable & (1 << 27)) != 0 && (gpio.host_to_qemu & (1 << 27)) != 0;
            (s0, s1)
        } else {
            (false, true)
        };

        let sock = match (strap0, strap1) {
            (false, true) => self.rv_dm_sock,
            (true, false) => self.lc_ctrl_sock,
            _ => {
                let disabled_sock = self.rv_dm_sock.with_file_name("qemu-jtag-disabled.sock");
                let _ = std::fs::remove_file(&disabled_sock);
                let listener = std::os::unix::net::UnixListener::bind(&disabled_sock)?;
                let sock_clone = disabled_sock.clone();
                std::thread::spawn(move || {
                    if let Ok((mut stream, _)) = listener.accept() {
                        let mut buf = [0u8; 256];
                        while let Ok(n) = std::io::Read::read(&mut stream, &mut buf) {
                            if n == 0 {
                                break;
                            }
                            let mut reply = Vec::new();
                            let mut quit = false;
                            for &b in &buf[..n] {
                                if b == b'R' {
                                    reply.push(b'1');
                                } else if b == b'Q' {
                                    quit = true;
                                }
                            }
                            if !reply.is_empty() {
                                let _ = std::io::Write::write_all(&mut stream, &reply);
                                let _ = std::io::Write::flush(&mut stream);
                            }
                            if quit {
                                break;
                            }
                        }
                    }
                    let _ = std::fs::remove_file(&sock_clone);
                });
                disabled_sock
            }
        };

        let openocd = OpenOcdJtagChain::new(
            &format!(
                "adapter driver remote_bitbang; remote_bitbang port 0; remote_bitbang host {sock}",
                sock = sock.display(),
            ),
            &self.params,
        )?;

        Box::new(openocd).into_raw()
    }
}
