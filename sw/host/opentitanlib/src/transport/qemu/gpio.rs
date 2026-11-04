// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::io::{BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;
use std::path::Path;
use std::rc::Rc;
use std::time::{Duration, Instant};

use anyhow::{Context, bail};

use crate::io::gpio::{
    BitbangEntry, ClockNature, DacBangEntry, Edge, GpioBitbangOperation, GpioBitbanging,
    GpioDacBangOperation, GpioMonitoring, GpioPin, MonitoringEvent, MonitoringReadResponse,
    MonitoringStartResponse, PinMode, PullMode,
};

const QEMU_GPIO_CLEAR: char = 'C';
const QEMU_GPIO_DIRECTION: char = 'D';
const QEMU_GPIO_OUTPUT: char = 'O';
const QEMU_GPIO_PULL: char = 'P';
const QEMU_GPIO_FLOATING: char = 'Z';
const QEMU_GPIO_INPUT_FORWARD: char = 'Y';
const QEMU_GPIO_REPEAT: char = 'R';
const QEMU_GPIO_TIME: char = 'T';
const QEMU_GPIO_QUERY: char = 'Q';

const QEMU_GPIO_HOST_DIRECTION: char = 'd';
const QEMU_GPIO_HOST_OUTPUT: char = 'o';
const QEMU_GPIO_HOST_FLOATING: char = 'z';
const QEMU_GPIO_HOST_PULL: char = 'p';

const QEMU_GPIO_LEGACY_MASK: char = 'M';
const QEMU_GPIO_LEGACY_INPUT: char = 'I';

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct GpioSideState {
    /// Mask of which pins have output driver enabled ('D' / 'd', or !'M')
    pub output_enable: u32,
    /// Output data driven on pins ('O' / 'o', or 'I')
    pub output_data: u32,
    /// Mask of which pins are floating / Hi-Z ('Z' / 'z')
    /// 1 indicates floating (no pull resistor), 0 indicates pull is active.
    pub floating: u32,
    /// Pull direction on pins ('P' / 'p')
    /// 1 indicates pull-up, 0 indicates pull-down.
    pub pull: u32,
}

impl GpioSideState {
    #[inline]
    pub fn is_driving(&self, pin: u8) -> bool {
        pin_bit(self.output_enable, pin)
    }

    #[inline]
    pub fn output_level(&self, pin: u8) -> bool {
        pin_bit(self.output_data, pin)
    }

    #[inline]
    pub fn is_pulled(&self, pin: u8) -> bool {
        !pin_bit(self.floating, pin)
    }

    #[inline]
    pub fn pull_level(&self, pin: u8) -> bool {
        pin_bit(self.pull, pin)
    }

    pub fn pull_mode(&self, pin: u8) -> PullMode {
        if self.is_pulled(pin) {
            if self.pull_level(pin) {
                PullMode::PullUp
            } else {
                PullMode::PullDown
            }
        } else {
            PullMode::None
        }
    }
}

#[inline]
fn pin_bit(val: u32, idx: u8) -> bool {
    (val >> idx) & 1 == 1
}

#[inline]
fn update_bit(val: u32, idx: u8, set: bool) -> u32 {
    let mask = 1 << idx;
    if set {
        val | mask
    } else {
        val & !mask
    }
}

fn get_pin_index(pin: &dyn GpioPin) -> anyhow::Result<u8> {
    let name = pin
        .get_internal_pin_name()
        .context("QEMU GPIO pin missing internal name")?;
    name.parse()
        .with_context(|| format!("invalid pin name: {name}"))
}

fn get_pin_indices(pins: &[&dyn GpioPin]) -> anyhow::Result<Vec<u8>> {
    pins.iter().map(|&pin| get_pin_index(pin)).collect()
}

pub struct QemuGpio {
    stream: BufReader<UnixStream>,
    pub pins: HashMap<u8, Rc<dyn GpioPin>>,

    /// Device (QEMU) side state.
    pub device: GpioSideState,

    /// Host side state.
    pub host: GpioSideState,

    /// QEMU also forwards to us the last input values that it read.
    pub qemu_input_fwd: u32,

    pub epoch: Instant,
    pub monitored_pins: Vec<(u8, PullMode)>,
    pub monitored_levels: Vec<bool>,
    pub monitored_events: Vec<MonitoringEvent>,
    pub sync_seq: u32,
    pub last_qemu_us32: Option<u32>,
    pub qemu_time_us64: u64,
}

impl QemuGpio {
    pub fn new<P: AsRef<Path>>(gpio_socket: P) -> anyhow::Result<Self> {
        let stream =
            UnixStream::connect(gpio_socket).context("failed to connect to QEMU GPIO Socket")?;
        // Set a minimal timeout to emulate non-blocking GPIO socket reads
        stream.set_read_timeout(Some(Duration::from_millis(1)))?;

        let stream = BufReader::new(stream);

        let mut qemu_gpio = QemuGpio {
            stream,
            pins: HashMap::default(),
            device: GpioSideState::default(),
            host: GpioSideState::default(),
            qemu_input_fwd: 0x0,
            epoch: Instant::now(),
            monitored_pins: Vec::new(),
            monitored_levels: Vec::new(),
            monitored_events: Vec::new(),
            sync_seq: 0,
            last_qemu_us32: None,
            qemu_time_us64: 0,
        };

        // Query current host pin status from QEMU.
        qemu_gpio.query_initial_host_state()?;

        Ok(qemu_gpio)
    }

    fn query_initial_host_state(&mut self) -> anyhow::Result<()> {
        // Send query to QEMU requesting current host pin configuration and sync.
        self.send_frame_sync(QEMU_GPIO_QUERY, 0)
    }

    #[inline]
    pub fn is_host_pull_up(&self, idx: u8) -> bool {
        self.host.is_pulled(idx) && self.host.pull_level(idx)
    }

    #[inline]
    pub fn is_host_pull_down(&self, idx: u8) -> bool {
        self.host.is_pulled(idx) && !self.host.pull_level(idx)
    }

    #[inline]
    pub fn is_host_output_enabled(&self, idx: u8) -> bool {
        self.host.is_driving(idx)
    }

    fn process_single_frame(&mut self, line: &str) -> anyhow::Result<()> {
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

        let cmd_char = cmd as char;
        match cmd_char {
            // QEMU wants us to clear / reset what we know about its pins.
            QEMU_GPIO_CLEAR => {
                self.device = GpioSideState::default();
            }
            // Device (QEMU) side frames:
            QEMU_GPIO_DIRECTION => self.device.output_enable = value,
            QEMU_GPIO_OUTPUT => self.device.output_data = value,
            QEMU_GPIO_FLOATING => self.device.floating = value,
            QEMU_GPIO_PULL => self.device.pull = value,

            // Host side symmetric frames:
            QEMU_GPIO_HOST_DIRECTION => self.host.output_enable = value,
            QEMU_GPIO_HOST_OUTPUT => self.host.output_data = value,
            QEMU_GPIO_HOST_FLOATING => self.host.floating = value,
            QEMU_GPIO_HOST_PULL => self.host.pull = value,

            // Legacy host frame aliases from QEMU:
            QEMU_GPIO_LEGACY_MASK => self.host.output_enable = !value,
            QEMU_GPIO_LEGACY_INPUT => self.host.output_data = value,

            // QEMU is querying our inputs to its pins (kept for backward compatibility).
            QEMU_GPIO_QUERY => {
                self.send_frame(QEMU_GPIO_LEGACY_MASK, !self.host.output_enable)?;
                self.send_frame(QEMU_GPIO_LEGACY_INPUT, self.host.output_data)?;
                self.send_frame(QEMU_GPIO_HOST_FLOATING, self.host.floating)?;
                self.send_frame(QEMU_GPIO_HOST_PULL, self.host.pull)?;
            }

            // QEMU is telling us what its last GPIO input values were
            QEMU_GPIO_INPUT_FORWARD => self.qemu_input_fwd = value,
            QEMU_GPIO_TIME => self.update_qemu_time(value),
            QEMU_GPIO_REPEAT => {}
            _ => bail!("unknown command from QEMU: {cmd_char}"),
        }

        if matches!(cmd_char, QEMU_GPIO_OUTPUT | QEMU_GPIO_INPUT_FORWARD) {
            self.check_monitored_edges();
        }

        Ok(())
    }

    pub fn now_us(&self) -> u64 {
        self.epoch.elapsed().as_micros() as u64
    }

    pub fn update_qemu_time(&mut self, val: u32) {
        if let Some(prev) = self.last_qemu_us32 {
            let delta = val.wrapping_sub(prev) as u64;
            self.qemu_time_us64 = self.qemu_time_us64.wrapping_add(delta);
        } else {
            self.qemu_time_us64 = val as u64;
        }
        self.last_qemu_us32 = Some(val);
    }

    pub fn current_timestamp(&self) -> u64 {
        if self.last_qemu_us32.is_some() {
            self.qemu_time_us64
        } else {
            self.now_us()
        }
    }

    pub fn pin_level(&self, idx: u8, pull_override: PullMode) -> bool {
        let dev_driving = self.device.is_driving(idx);
        let host_driving = self.host.is_driving(idx);
        let dev_level = self.device.output_level(idx);
        let host_level = self.host.output_level(idx);

        match (dev_driving, host_driving) {
            (true, true) => {
                let host_pull = if pull_override != PullMode::None {
                    pull_override
                } else {
                    self.host.pull_mode(idx)
                };
                if host_pull == PullMode::PullUp {
                    dev_level && host_level
                } else {
                    dev_level
                }
            }
            (true, false) => dev_level,
            (false, true) => host_level,
            (false, false) => {
                let host_pull = if pull_override != PullMode::None {
                    pull_override
                } else {
                    self.host.pull_mode(idx)
                };

                let dev_pulled = self.device.is_pulled(idx);
                let host_is_pulled = host_pull != PullMode::None;

                if dev_pulled {
                    // Device internal pull takes priority over host weak pull
                    self.device.pull_level(idx)
                } else if host_is_pulled {
                    host_pull == PullMode::PullUp
                } else {
                    // Completely floating pin: return random bit
                    rand::random::<bool>()
                }
            }
        }
    }

    pub fn check_monitored_edges(&mut self) {
        if self.monitored_pins.is_empty() {
            return;
        }
        let now = self.current_timestamp();
        for (i, &(idx, pull_mode)) in self.monitored_pins.iter().enumerate() {
            let curr = self.pin_level(idx, pull_mode);
            if curr != self.monitored_levels[i] {
                self.monitored_levels[i] = curr;
                self.monitored_events.push(MonitoringEvent {
                    signal_index: i as u8,
                    edge: if curr { Edge::Rising } else { Edge::Falling },
                    timestamp: now,
                });
            }
        }
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
        self.stream.get_ref().set_nonblocking(true)?;
        let deadline = Instant::now() + Duration::from_millis(50);
        let mut line = String::new();
        while Instant::now() < deadline {
            match self.stream.read_line(&mut line) {
                Ok(0) | Err(_) => break,
                Ok(_) => {
                    self.process_single_frame(&line)?;
                    line.clear();
                }
            }
        }
        self.stream.get_ref().set_nonblocking(false)?;

        Ok(())
    }

    /// Synchronize host GPIO state with QEMU using a synchronous barrier request (`R:<seq>`).
    pub fn sync_with_qemu(&mut self) -> anyhow::Result<()> {
        self.sync_seq = self.sync_seq.wrapping_add(1);
        let target_seq = self.sync_seq;
        self.send_frame(QEMU_GPIO_REPEAT, target_seq)?;

        self.stream
            .get_ref()
            .set_read_timeout(Some(Duration::from_millis(50)))?;

        let deadline = Instant::now() + Duration::from_millis(50);
        let mut line = String::new();
        while Instant::now() < deadline {
            let Ok(bytes_read) = self.stream.read_line(&mut line) else {
                break;
            };
            if bytes_read == 0 {
                break;
            };
            if let Some((cmd, value)) = line.split_once(":") {
                let (cmd, value) = (cmd.trim(), value.trim());
                if let (Some(&cmd_byte), Ok(val)) =
                    (cmd.as_bytes().first(), u32::from_str_radix(value, 16))
                {
                    if cmd_byte as char == QEMU_GPIO_REPEAT && val == target_seq {
                        break;
                    }
                    let _ = self.process_single_frame(&line);
                }
            }
            line.clear();
        }

        self.stream
            .get_ref()
            .set_read_timeout(Some(Duration::from_millis(1)))?;

        self.check_monitored_edges();
        Ok(())
    }

    /// Send a GPIO command frame to QEMU telling it about how we're driving the pins.
    pub fn send_frame(&mut self, cmd: char, value: u32) -> anyhow::Result<()> {
        writeln!(self.stream.get_mut(), "{cmd}:{value:08x}")
            .context("failed to send GPIO frame")?;
        self.stream.get_mut().flush()?;

        Ok(())
    }

    /// Send a GPIO command frame to QEMU and synchronize host state with QEMU.
    pub fn send_frame_sync(&mut self, cmd: char, value: u32) -> anyhow::Result<()> {
        self.send_frame(cmd, value)?;
        self.sync_with_qemu()
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
            if (self.host.output_enable & mask) != 0 {
                if ((last_wval >> bit_idx) & 1) != 0 {
                    self.host.output_data |= mask;
                } else {
                    self.host.output_data &= !mask;
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
        let mut gpio = self.qemu_gpio.borrow_mut();
        gpio.sync_with_qemu()?;

        if gpio.is_host_output_enabled(self.idx)
            || [PinMode::PushPull, PinMode::AnalogOutput].contains(&self.mode.get())
        {
            log::warn!("attempting to read from output pin {}", self.idx);
        }

        let pull_mode = gpio.host.pull_mode(self.idx);

        Ok(gpio.pin_level(self.idx, pull_mode))
    }

    fn write(&self, value: bool) -> anyhow::Result<()> {
        let mut gpio = self.qemu_gpio.borrow_mut();
        let new_value = update_bit(gpio.host.output_data, self.idx, value);

        if new_value != gpio.host.output_data {
            gpio.host.output_data = new_value;
            gpio.send_frame_sync(QEMU_GPIO_HOST_OUTPUT, new_value)?;
        }

        Ok(())
    }

    fn set_mode(&self, mode: PinMode) -> anyhow::Result<()> {
        let mut gpio = self.qemu_gpio.borrow_mut();

        if mode == PinMode::OpenDrain
            && !gpio.is_host_output_enabled(self.idx)
            && !pin_bit(gpio.host.output_data, self.idx)
        {
            let val = update_bit(gpio.host.output_data, self.idx, true);
            gpio.host.output_data = val;
            gpio.send_frame(QEMU_GPIO_HOST_OUTPUT, val)?;
        }

        let is_output =
            [PinMode::PushPull, PinMode::OpenDrain, PinMode::AnalogOutput].contains(&mode);
        let new_value = update_bit(gpio.host.output_enable, self.idx, is_output);

        if new_value != gpio.host.output_enable {
            gpio.host.output_enable = new_value;
            gpio.send_frame_sync(QEMU_GPIO_HOST_DIRECTION, new_value)?;
        }

        self.mode.set(mode);

        Ok(())
    }

    fn set_pull_mode(&self, mode: PullMode) -> anyhow::Result<()> {
        let mut gpio = self.qemu_gpio.borrow_mut();
        let (is_pulled, pull_val) = match mode {
            PullMode::None => (false, false),
            PullMode::PullUp => (true, true),
            PullMode::PullDown => (true, false),
        };

        let new_pull = update_bit(gpio.host.pull, self.idx, pull_val);
        if new_pull != gpio.host.pull {
            gpio.host.pull = new_pull;
            gpio.send_frame_sync(QEMU_GPIO_HOST_PULL, new_pull)?;
        }

        let new_floating = update_bit(gpio.host.floating, self.idx, !is_pulled);
        if new_floating != gpio.host.floating {
            gpio.host.floating = new_floating;
            gpio.send_frame_sync(QEMU_GPIO_HOST_FLOATING, new_floating)?;
        }

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
        let pin_indices = get_pin_indices(pins)?;
        let pins_str = pin_indices
            .iter()
            .map(ToString::to_string)
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
                    gpio.run_bitbang_entry(&pins_str, &pin_indices, period_ns, wbuf, Some(*rbuf))?;
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

pub struct QemuGpioMonitoring {
    qemu_gpio: Rc<RefCell<QemuGpio>>,
}

impl QemuGpioMonitoring {
    pub fn new(qemu_gpio: Rc<RefCell<QemuGpio>>) -> Self {
        Self { qemu_gpio }
    }
}

impl GpioMonitoring for QemuGpioMonitoring {
    fn get_clock_nature(&self) -> anyhow::Result<ClockNature> {
        Ok(ClockNature::Wallclock {
            resolution: 1_000_000,
            offset: None,
        })
    }

    fn monitoring_start(&self, pins: &[&dyn GpioPin]) -> anyhow::Result<MonitoringStartResponse> {
        let pin_indices = get_pin_indices(pins)?;
        let mut gpio = self.qemu_gpio.borrow_mut();
        gpio.sync_with_qemu()?;
        gpio.monitored_pins.clear();
        gpio.monitored_levels.clear();
        gpio.monitored_events.clear();

        let mut initial_levels = Vec::with_capacity(pin_indices.len());
        for &idx in &pin_indices {
            let pull_mode = gpio.host.pull_mode(idx);
            let lvl = gpio.pin_level(idx, pull_mode);
            gpio.monitored_pins.push((idx, pull_mode));
            gpio.monitored_levels.push(lvl);
            initial_levels.push(lvl);
        }

        Ok(MonitoringStartResponse {
            timestamp: gpio.current_timestamp(),
            initial_levels,
        })
    }

    fn monitoring_read(
        &self,
        _pins: &[&dyn GpioPin],
        continue_monitoring: bool,
    ) -> anyhow::Result<MonitoringReadResponse> {
        let mut gpio = self.qemu_gpio.borrow_mut();
        gpio.sync_with_qemu()?;
        let events = std::mem::take(&mut gpio.monitored_events);
        let timestamp = gpio.current_timestamp();
        if !continue_monitoring {
            gpio.monitored_pins.clear();
            gpio.monitored_levels.clear();
        }
        Ok(MonitoringReadResponse { events, timestamp })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_side_state_helpers() {
        let state = GpioSideState {
            output_enable: 0b0001,
            output_data: 0b0001,
            floating: 0b0010, // pin 1 is floating (not pulled), pins 0, 2, 3 are pulled
            pull: 0b0100,     // pin 2 is pull-up, pin 0, 1, 3 are pull-down
        };

        // Pin 0: driving 1, pulled down
        assert!(state.is_driving(0));
        assert!(state.output_level(0));
        assert!(state.is_pulled(0));
        assert!(!state.pull_level(0));
        assert_eq!(state.pull_mode(0), PullMode::PullDown);

        // Pin 1: not driving, floating
        assert!(!state.is_driving(1));
        assert!(!state.output_level(1));
        assert!(!state.is_pulled(1));
        assert_eq!(state.pull_mode(1), PullMode::None);

        // Pin 2: not driving, pulled up
        assert!(!state.is_driving(2));
        assert!(state.is_pulled(2));
        assert!(state.pull_level(2));
        assert_eq!(state.pull_mode(2), PullMode::PullUp);

        // Pin 3: not driving, pulled down
        assert!(!state.is_driving(3));
        assert!(state.is_pulled(3));
        assert!(!state.pull_level(3));
        assert_eq!(state.pull_mode(3), PullMode::PullDown);
    }

    fn make_test_gpio() -> QemuGpio {
        let (s1, _s2) = UnixStream::pair().unwrap();
        QemuGpio {
            stream: BufReader::new(s1),
            pins: HashMap::default(),
            device: GpioSideState {
                output_enable: 0,
                output_data: 0,
                floating: u32::MAX,
                pull: 0,
            },
            host: GpioSideState {
                output_enable: 0,
                output_data: 0,
                floating: u32::MAX,
                pull: 0,
            },
            qemu_input_fwd: 0,
            epoch: Instant::now(),
            monitored_pins: Vec::new(),
            monitored_levels: Vec::new(),
            monitored_events: Vec::new(),
            sync_seq: 0,
            last_qemu_us32: None,
            qemu_time_us64: 0,
        }
    }

    #[test]
    fn test_process_frames_symmetric_and_legacy() {
        let mut gpio = make_test_gpio();

        // Host frames: d, o, z, p
        gpio.process_single_frame("d:00000003").unwrap();
        assert_eq!(gpio.host.output_enable, 3);

        gpio.process_single_frame("o:00000001").unwrap();
        assert_eq!(gpio.host.output_data, 1);

        gpio.process_single_frame("z:00000004").unwrap();
        assert_eq!(gpio.host.floating, 4);

        gpio.process_single_frame("p:00000002").unwrap();
        assert_eq!(gpio.host.pull, 2);

        // Device frames: D, O, Z, P
        gpio.process_single_frame("D:00000005").unwrap();
        assert_eq!(gpio.device.output_enable, 5);

        gpio.process_single_frame("O:00000004").unwrap();
        assert_eq!(gpio.device.output_data, 4);

        gpio.process_single_frame("Z:00000001").unwrap();
        assert_eq!(gpio.device.floating, 1);

        gpio.process_single_frame("P:00000002").unwrap();
        assert_eq!(gpio.device.pull, 2);

        // Legacy host aliases: M, I
        gpio.process_single_frame("M:fffffff0").unwrap();
        assert_eq!(gpio.host.output_enable, 0x0f);

        gpio.process_single_frame("I:00000007").unwrap();
        assert_eq!(gpio.host.output_data, 7);
    }

    #[test]
    fn test_pin_level_pull_and_drive() {
        let mut gpio = make_test_gpio();

        // Pin 0: Host driving high, device floating
        gpio.host.output_enable = 1 << 0;
        gpio.host.output_data = 1 << 0;
        assert!(gpio.pin_level(0, PullMode::None));

        // Pin 1: Device driving low, host driving high (open drain pull-up case)
        gpio.device.output_enable = 1 << 1;
        gpio.device.output_data = 0;
        gpio.host.output_enable = 1 << 1;
        gpio.host.output_data = 1 << 1;
        assert!(!gpio.pin_level(1, PullMode::PullUp));

        // Pin 2: Host weak pull-up (device floating)
        gpio.host.floating &= !(1 << 2);
        gpio.host.pull |= 1 << 2;
        assert!(gpio.pin_level(2, PullMode::None));

        // Pin 3: Host weak pull-down (device floating)
        gpio.host.floating &= !(1 << 3);
        gpio.host.pull &= !(1 << 3);
        assert!(!gpio.pin_level(3, PullMode::None));

        // Pin 4: Device pull overrides host pull
        // Host has pull-up
        gpio.host.floating &= !(1 << 4);
        gpio.host.pull |= 1 << 4;
        // Device has pull-down
        gpio.device.floating &= !(1 << 4);
        gpio.device.pull &= !(1 << 4);
        // Result must be false (device pull-down wins)
        assert!(!gpio.pin_level(4, PullMode::None));

        // Pin 5: Floating pin produces a bool without panic
        let _ = gpio.pin_level(5, PullMode::None);
    }
}
