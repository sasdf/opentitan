// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::io::{BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;
use std::path::Path;
use std::rc::Rc;
use std::time::Duration;

use anyhow::{Context, bail};

use crate::io::gpio::{
    BitbangEntry, DacBangEntry, GpioBitbangOperation, GpioBitbanging, GpioDacBangOperation,
    GpioPin, PinMode, PullMode,
};

const QEMU_GPIO_CLEAR: char = 'C';
const QEMU_GPIO_DIRECTION: char = 'D';
const QEMU_GPIO_OUTPUT: char = 'O';
const QEMU_GPIO_PULL: char = 'P';
const QEMU_GPIO_QUERY: char = 'Q';
const QEMU_GPIO_FLOATING: char = 'Z';
const QEMU_GPIO_INPUT: char = 'I';
const QEMU_GPIO_MASK: char = 'M';
const QEMU_GPIO_INPUT_FORWARD: char = 'Y';

pub struct QemuGpio {
    stream: BufReader<UnixStream>,
    pub pins: HashMap<u8, Rc<dyn GpioPin>>,

    /// Current outputs being driven from host to QEMU when pins are in an
    /// outputting mode (ignored for inputs).
    pub host_to_qemu: u32,

    /// Current mask of which host pins are connected to QEMU inputs.
    ///
    /// - 0 indicates QEMU should ignore this pin as a host -> device input.
    /// - 1 indicates QEMU should treat this pin as a host -> device input.
    pub host_output_enable: u32,

    /// Current outputs being driven from QEMU to host (if pin in output mode).
    pub qemu_to_host: u32,

    /// Last reported input/output directions from QEMU:
    ///
    /// - 0 indicates input into QEMU.
    /// - 1 indicates output from QEMU.
    pub qemu_outputting: u32,

    /// Last reported pull up/down from QEMU:
    ///
    /// - 0 indicates QEMU is pulling down.
    /// - 1 indicates QEMU is pulling up.
    pub qemu_pull: u32,

    /// Last reported high impedance mode from QEMU:
    ///
    /// - 0 indicates QEMU is pulling up/down (see `qemu_pull`).
    /// - 1 indicates the pins are floating (high impedance / HiZ).
    pub qemu_floating: u32,

    /// QEMU also forwards to us the last input values that it read.
    /// Last reported input from QEMU.
    ///
    /// - 0 indicates QEMU last read a 0.
    /// - 1 indicates QEMU last read a 1.
    pub qemu_input_fwd: u32,
}

impl QemuGpio {
    pub fn new<P: AsRef<Path>>(gpio_socket: P) -> anyhow::Result<Self> {
        let stream =
            UnixStream::connect(gpio_socket).context("failed to connect to QEMU GPIO Socket")?;
        // Set a minimal timeout to emulate non-blocking GPIO socket reads
        stream.set_read_timeout(Some(Duration::from_millis(1)))?;

        let stream = BufReader::new(stream);

        let qemu_gpio = QemuGpio {
            stream,
            pins: HashMap::default(),
            host_to_qemu: 0x0,
            host_output_enable: 0x0,
            qemu_to_host: 0x0,
            qemu_outputting: 0x0,
            qemu_pull: 0x0,
            qemu_floating: 0x0,
            qemu_input_fwd: 0x0,
        };

        Ok(qemu_gpio)
    }

    /// Process all GPIO command frames received from QEMU over the TTY.
    ///
    /// QEMU will send these when the state of the pins has changed in some way.
    /// Frames have the following format:
    ///
    /// ```text
    /// <command>:<value>\r\n
    /// ```
    ///
    /// where `<command>` is a single uppercase character and `<value>` is four
    /// uppercase hex characters forming a value. This is a custom protocol for
    /// OpenTitan's QEMU machine.
    pub fn process_frames(&mut self) -> anyhow::Result<()> {
        let mut line = String::new();
        while let Ok(bytes_read) = self.stream.read_line(&mut line) {
            if bytes_read == 0 {
                break; // EOF reached
            }

            let (cmd, value) = line
                .split_once(":")
                .context("bad QEMU GPIO frame: missing ':'")?;
            let (cmd, value) = (cmd.trim(), value.trim());

            let &[cmd] = cmd.as_bytes() else {
                bail!("bad QEMU GPIO frame: expected single ascii char command, got {cmd}");
            };

            let value = u32::from_str_radix(value, 16).with_context(|| {
                format!("bad QEMU GPIO frame: expected four-hex value, got {value}")
            })?;

            match cmd as char {
                // QEMU wants us to clear / reset what we know about its pins.
                QEMU_GPIO_CLEAR => {
                    self.qemu_to_host = 0;
                    self.qemu_outputting = 0;
                    self.qemu_pull = 0;
                    self.qemu_floating = 0;
                }
                // The direction of one or more pins has changed between input/output.
                QEMU_GPIO_DIRECTION => self.qemu_outputting = value,
                // The output of one or more pins has changed.
                QEMU_GPIO_OUTPUT => self.qemu_to_host = value,
                // The pull up/down of one or more pins has changed.
                QEMU_GPIO_PULL => self.qemu_pull = value,
                // QEMU is querying our inputs to its pins.
                QEMU_GPIO_QUERY => {
                    self.send_frame(QEMU_GPIO_MASK, !self.host_output_enable)?;
                    self.send_frame(QEMU_GPIO_INPUT, self.host_to_qemu)?;
                }
                // The hi-Z value of one or more pins has changed.
                QEMU_GPIO_FLOATING => self.qemu_floating = value,
                // QEMU is telling us what its last GPIO input values were
                QEMU_GPIO_INPUT_FORWARD => self.qemu_input_fwd = value,
                _ => bail!("unknown command from QEMU: {cmd}"),
            }

            line.clear();
        }

        Ok(())
    }

    /// Send a GPIO command frame to QEMU telling it about how we're driving the pins.
    ///
    /// See [`QemuGpio::process_frames`] for a description of the frame format.
    ///
    /// Frame commands currently supported by QEMU are:
    ///
    /// * `I:<value>`:  the inputs to the device's pads are now `<value>`, if driven.
    /// * `M:<value>`:  the inputs are masked with `<value>`, meaning driven/connected.
    /// * `R:00000000`: ask QEMU to repeat the last `D` and `O` frames (see [`QemuGpio::process_frames`]).
    pub fn send_frame(&mut self, cmd: char, value: u32) -> anyhow::Result<()> {
        writeln!(self.stream.get_mut(), "{cmd}:{value:08x}")
            .context("failed to send GPIO frame")?;

        Ok(())
    }

    pub fn run_bitbang_entry(
        &mut self,
        pins_str: &str,
        pin_indices: &[u8],
        period_ns: u64,
        wbuf: &[u8],
        rbuf: Option<&mut [u8]>,
    ) -> anyhow::Result<()> {
        use std::fmt::Write as _;
        let num_samples = wbuf.len();
        if num_samples == 0 {
            return Ok(());
        }

        let mut rle_wbuf = String::new();
        let mut cur_val = wbuf[0];
        let mut count = 1usize;
        for &b in &wbuf[1..] {
            if b == cur_val {
                count += 1;
            } else {
                if !rle_wbuf.is_empty() {
                    rle_wbuf.push(',');
                }
                write!(&mut rle_wbuf, "{}*{:02x}", count, cur_val).unwrap();
                cur_val = b;
                count = 1;
            }
        }
        if !rle_wbuf.is_empty() {
            rle_wbuf.push(',');
        }
        write!(&mut rle_wbuf, "{}*{:02x}", count, cur_val).unwrap();

        writeln!(
            self.stream.get_mut(),
            "B:{}:{}:{}:{}",
            period_ns,
            num_samples,
            pins_str,
            rle_wbuf
        )
        .context("failed to send bitbang frame to QEMU")?;
        self.stream.get_mut().flush()?;

        let last_wval = wbuf[num_samples - 1];
        for (bit_idx, &pad) in pin_indices.iter().enumerate() {
            let mask = 1u32 << pad;
            if (self.host_output_enable & mask) != 0 {
                if ((last_wval >> bit_idx) & 1) != 0 {
                    self.host_to_qemu |= mask;
                } else {
                    self.host_to_qemu &= !mask;
                }
            }
        }

        let mut line = String::new();
        loop {
            line.clear();
            let bytes_read = self
                .stream
                .read_line(&mut line)
                .context("failed waiting for QEMU bitbang response")?;
            if bytes_read == 0 {
                bail!("EOF while waiting for QEMU bitbang response");
            }
            let trimmed = line.trim();
            if let Some(rle_resp) = trimmed.strip_prefix("B:") {
                if let Some(out_slice) = rbuf {
                    let mut idx = 0usize;
                    for item in rle_resp.split(',') {
                        if item.is_empty() {
                            continue;
                        }
                        if let Some((cnt_str, val_str)) = item.split_once('*') {
                            let cnt: usize = cnt_str.parse()?;
                            let val: u8 = u8::from_str_radix(val_str, 16)?;
                            for _ in 0..cnt {
                                if idx < out_slice.len() {
                                    out_slice[idx] = val;
                                    idx += 1;
                                }
                            }
                        }
                    }
                }
                break;
            }
        }

        Ok(())
    }
}

pub struct QemuGpioPin {
    qemu_gpio: Rc<RefCell<QemuGpio>>,
    idx: u8,
    name: String,

    mode: Cell<PinMode>,

    /// Pull mode of the current pin.
    ///
    /// NOTE: QEMU does not currently support pulling the pin from the host
    /// side. We could attempt to emulate pulling by driving the output from
    /// our side, but we cannot model our pull being weaker than QEMU's driven
    /// output. For now we do not model pulling.
    pull_mode: Cell<PullMode>,
}

impl QemuGpioPin {
    pub(crate) fn new(qemu_gpio: Rc<RefCell<QemuGpio>>, idx: u8) -> Rc<Self> {
        let pin = QemuGpioPin {
            qemu_gpio,
            idx,
            name: format!("{}", idx),
            mode: Cell::new(PinMode::Input),
            pull_mode: Cell::new(PullMode::None),
        };
        Rc::new(pin)
    }
}

impl GpioPin for QemuGpioPin {
    fn get_internal_pin_name(&self) -> Option<&str> {
        Some(&self.name)
    }

    fn read(&self) -> anyhow::Result<bool> {
        if [PinMode::PushPull, PinMode::AnalogOutput].contains(&self.mode.get()) {
            log::warn!("attempting to read from output pin {}", self.idx);
        }

        let mut gpio = self.qemu_gpio.borrow_mut();

        gpio.process_frames()?;

        let qemu_outputting = (gpio.qemu_outputting >> self.idx & 1) == 1;

        let value = match qemu_outputting {
            true => gpio.qemu_to_host >> self.idx & 1,
            false => {
                let qemu_floating = (gpio.qemu_floating >> self.idx & 1) == 1;
                if qemu_floating {
                    match self.pull_mode.get() {
                        PullMode::PullUp => 1,
                        PullMode::PullDown => 0,
                        PullMode::None => gpio.qemu_pull >> self.idx & 1,
                    }
                } else {
                    gpio.qemu_pull >> self.idx & 1
                }
            }
        };

        Ok(value == 1)
    }

    fn write(&self, value: bool) -> anyhow::Result<()> {
        let mut gpio = self.qemu_gpio.borrow_mut();
        let mut new_value = gpio.host_to_qemu;

        let mask = 1 << self.idx;
        if value {
            new_value |= mask;
        } else {
            new_value &= !mask;
        }

        if new_value != gpio.host_to_qemu {
            gpio.send_frame(QEMU_GPIO_INPUT, new_value)?;
            gpio.host_to_qemu = new_value;
        }

        Ok(())
    }

    fn set_mode(&self, mode: PinMode) -> anyhow::Result<()> {
        let mut gpio = self.qemu_gpio.borrow_mut();
        let mut new_value = gpio.host_output_enable;

        let mask = 1 << self.idx;
        if mode == PinMode::OpenDrain && (gpio.host_output_enable & mask) == 0 {
            if (gpio.host_to_qemu & mask) == 0 {
                let val = gpio.host_to_qemu | mask;
                gpio.send_frame(QEMU_GPIO_INPUT, val)?;
                gpio.host_to_qemu = val;
            }
        }

        if [PinMode::PushPull, PinMode::OpenDrain, PinMode::AnalogOutput].contains(&mode) {
            new_value |= mask;
        } else {
            new_value &= !mask;
        }

        if new_value != gpio.host_output_enable {
            // Note: the protocol inverts this mask so that `1` means ignored
            // and `0` means connected.
            gpio.send_frame(QEMU_GPIO_MASK, !new_value)?;
            gpio.host_output_enable = new_value;
        }

        self.mode.set(mode);

        Ok(())
    }

    fn set_pull_mode(&self, mode: PullMode) -> anyhow::Result<()> {
        // Currently ignored, see [`QemuGpioPin::pull_mode`].
        self.pull_mode.set(mode);

        Ok(())
    }

    fn set(
        &self,
        mode: Option<PinMode>,
        value: Option<bool>,
        pull: Option<PullMode>,
        analog_value: Option<f32>,
    ) -> anyhow::Result<()> {
        if let Some(mode) = mode {
            self.set_mode(mode)?;
        }

        if let Some(value) = value {
            self.write(value)?;
        }

        if let Some(pull) = pull {
            self.set_pull_mode(pull)?;
        }

        if let Some(_analog_value) = analog_value {
            todo!("QEMU transport does not yet support analogue GPIOs");
        }

        Ok(())
    }
}

pub struct QemuGpioBitbanging {
    qemu_gpio: Rc<RefCell<QemuGpio>>,
}

impl QemuGpioBitbanging {
    pub fn new(qemu_gpio: Rc<RefCell<QemuGpio>>) -> Self {
        Self { qemu_gpio }
    }
}

pub struct QemuGpioBitbangOperation<'rd, 'wr> {
    waveform: Box<[BitbangEntry<'rd, 'wr>]>,
}

impl<'rd, 'wr> GpioBitbangOperation<'rd, 'wr> for QemuGpioBitbangOperation<'rd, 'wr> {
    fn query(&mut self) -> anyhow::Result<bool> {
        Ok(true)
    }

    fn get_result(self: Box<Self>) -> anyhow::Result<Box<[BitbangEntry<'rd, 'wr>]>> {
        Ok(self.waveform)
    }
}

impl GpioBitbanging for QemuGpioBitbanging {
    fn start<'a>(
        &self,
        pins: &[&dyn GpioPin],
        clock_tick: Duration,
        mut waveform: Box<[BitbangEntry<'a, 'a>]>,
    ) -> anyhow::Result<Box<dyn GpioBitbangOperation<'a, 'a> + 'a>> {
        let mut pin_indices = Vec::with_capacity(pins.len());
        for pin in pins {
            let name = pin
                .get_internal_pin_name()
                .context("QEMU GPIO pin missing internal name")?;
            let idx: u8 = name.parse()?;
            pin_indices.push(idx);
        }
        let pins_str = pin_indices
            .iter()
            .map(|idx| idx.to_string())
            .collect::<Vec<_>>()
            .join(".");

        let period_ns = clock_tick.as_nanos() as u64;
        let mut gpio = self.qemu_gpio.borrow_mut();

        let _ = gpio.process_frames();

        gpio.stream
            .get_ref()
            .set_read_timeout(Some(Duration::from_secs(60)))?;

        for entry in waveform.iter_mut() {
            match entry {
                BitbangEntry::Write(wbuf) => {
                    gpio.run_bitbang_entry(&pins_str, &pin_indices, period_ns, wbuf, None)?;
                }
                BitbangEntry::WriteOwned(wbuf) => {
                    gpio.run_bitbang_entry(&pins_str, &pin_indices, period_ns, wbuf, None)?;
                }
                BitbangEntry::Both(wbuf, rbuf) => {
                    gpio.run_bitbang_entry(
                        &pins_str,
                        &pin_indices,
                        period_ns,
                        wbuf,
                        Some(*rbuf),
                    )?;
                }
                BitbangEntry::BothOwned(buf) => {
                    let wbuf = buf.clone();
                    gpio.run_bitbang_entry(
                        &pins_str,
                        &pin_indices,
                        period_ns,
                        &wbuf,
                        Some(&mut **buf),
                    )?;
                }
                _ => bail!("Unsupported BitbangEntry variant on QEMU"),
            }
        }

        gpio.stream
            .get_ref()
            .set_read_timeout(Some(Duration::from_millis(1)))?;

        Ok(Box::new(QemuGpioBitbangOperation { waveform }))
    }

    fn dac_start(
        &self,
        _pins: &[&dyn GpioPin],
        _clock_tick: Duration,
        _waveform: Box<[DacBangEntry]>,
    ) -> anyhow::Result<Box<dyn GpioDacBangOperation>> {
        bail!("DAC bitbanging not supported on QEMU")
    }
}
