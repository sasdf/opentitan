// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

use std::cell::RefCell;
use std::os::fd::BorrowedFd;
use std::rc::Rc;
use std::task::{Context, Poll};

use anyhow::Result;
use serialport::Parity;

use crate::monitor::Monitor;
use opentitanlib::io::console::ConsoleDevice;
use opentitanlib::io::uart::flow::SoftwareFlowControl;
use opentitanlib::io::uart::serial::SerialPortUart;
use opentitanlib::io::uart::{FlowControl, Uart};

pub struct QemuUart {
    /// Connection to the QEMU monitor which can control the emulator.
    pub monitor: Rc<RefCell<Monitor>>,

    // The UART CharDev's id
    id: String,

    // Underlying SerialPort used for the UART.
    serial_port: SoftwareFlowControl<SerialPortUart>,

    // Whether the UART is currently transmitting a break condition or not.
    in_break: RefCell<bool>,
}

impl QemuUart {
    pub fn new(monitor: Rc<RefCell<Monitor>>, id: &str, serial_port: SerialPortUart) -> Self {
        Self {
            monitor,
            id: id.into(),
            serial_port: SoftwareFlowControl::new(serial_port),
            in_break: RefCell::new(false),
        }
    }
}

impl ConsoleDevice for QemuUart {
    fn poll_read(&self, cx: &mut Context<'_>, buf: &mut [u8]) -> Poll<Result<usize>> {
        self.serial_port.poll_read(cx, buf)
    }

    fn write(&self, buf: &[u8]) -> Result<()> {
        self.serial_port.write(buf)
    }
}

impl Uart for QemuUart {
    fn get_baudrate(&self) -> Result<u32> {
        self.serial_port.get_baudrate()
    }

    fn set_baudrate(&self, baudrate: u32) -> Result<()> {
        self.serial_port.set_baudrate(baudrate)
    }

    fn get_flow_control(&self) -> Result<FlowControl> {
        self.serial_port.get_flow_control()
    }

    fn set_flow_control(&self, flow_control: bool) -> Result<()> {
        self.serial_port.set_flow_control(flow_control)
    }

    fn get_device_path(&self) -> Result<String> {
        self.serial_port.get_device_path()
    }

    fn clear_rx_buffer(&self) -> Result<()> {
        self.serial_port.clear_rx_buffer()
    }

    fn set_break(&self, enable: bool) -> Result<()> {
        // Assumes we have ot-uart.toggle-break=true, so always send break
        if *self.in_break.borrow() ^ enable {
            self.monitor.borrow_mut().send_chardev_break(&self.id)?;
            *self.in_break.borrow_mut() = enable;
        }
        Ok(())
    }

    fn set_parity(&self, parity: Parity) -> Result<()> {
        self.serial_port.set_parity(parity)
    }

    fn borrow_fd(&self) -> Result<BorrowedFd<'_>> {
        self.serial_port.borrow_fd()
    }
}

impl Drop for QemuUart {
    fn drop(&mut self) {
        // Stop transmitting break before deleting the UART
        if *self.in_break.borrow() {
            let _ = self.monitor.borrow_mut().send_chardev_break(&self.id);
            *self.in_break.borrow_mut() = false;
        }
    }
}
