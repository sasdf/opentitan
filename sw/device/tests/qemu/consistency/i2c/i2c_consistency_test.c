// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_alert_handler.h"
#include "sw/device/lib/runtime/irq.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "alert_handler_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "i2c_regs.h"

OTTF_DEFINE_TEST_CONFIG();

static void test_csr_masks_and_access(mmio_region_t i2c_base) {
  // 1. STATUS reset value: FMTEMPTY(bit 2) | HOSTIDLE(bit 3) | TARGETIDLE(bit
  // 4) | RXEMPTY(bit 5) | TXEMPTY(bit 8) | ACQEMPTY(bit 9) = 0x33c.
  uint32_t status = mmio_region_read32(i2c_base, I2C_STATUS_REG_OFFSET);
  CHECK(status == 0x33cu, "Expected initial I2C STATUS=0x33c, got 0x%x",
        status);

  // 2. TARGET_NACK_COUNT (offset 0x68) is SwAccessRC (8-bit Read-Clear,
  // read-only to software writes). Writing 0xffffffff must be ignored and read
  // back 0.
  mmio_region_write32(i2c_base, I2C_TARGET_NACK_COUNT_REG_OFFSET, 0xffffffffu);
  uint32_t nack_cnt =
      mmio_region_read32(i2c_base, I2C_TARGET_NACK_COUNT_REG_OFFSET);
  CHECK(nack_cnt == 0u,
        "Expected TARGET_NACK_COUNT=0 after SW write (SwAccessRC), got 0x%x",
        nack_cnt);

  // 3. HOST_TIMEOUT_CTRL (offset 0x60) is 20 bits wide [19:0] (mask
  // 0x000fffff).
  mmio_region_write32(i2c_base, I2C_HOST_TIMEOUT_CTRL_REG_OFFSET, 0xffffffffu);
  uint32_t host_to =
      mmio_region_read32(i2c_base, I2C_HOST_TIMEOUT_CTRL_REG_OFFSET);
  CHECK(host_to == 0x000fffffu,
        "Expected HOST_TIMEOUT_CTRL=0x000fffff after writing 0xffffffff, got "
        "0x%x",
        host_to);
  mmio_region_write32(i2c_base, I2C_HOST_TIMEOUT_CTRL_REG_OFFSET, 0u);

  // 4. TIMING0..4, TIMEOUT_CTRL, TARGET_ID, HOST_FIFO_CONFIG,
  // TARGET_FIFO_CONFIG field masks.
  mmio_region_write32(i2c_base, I2C_HOST_FIFO_CONFIG_REG_OFFSET, 0xffffffffu);
  uint32_t h_fifo_cfg =
      mmio_region_read32(i2c_base, I2C_HOST_FIFO_CONFIG_REG_OFFSET);
  CHECK(h_fifo_cfg == 0x0fff0fffu,
        "Expected HOST_FIFO_CONFIG=0x0fff0fff, got 0x%x", h_fifo_cfg);
  mmio_region_write32(i2c_base, I2C_HOST_FIFO_CONFIG_REG_OFFSET, 0u);

  mmio_region_write32(i2c_base, I2C_TARGET_FIFO_CONFIG_REG_OFFSET, 0xffffffffu);
  uint32_t t_fifo_cfg =
      mmio_region_read32(i2c_base, I2C_TARGET_FIFO_CONFIG_REG_OFFSET);
  CHECK(t_fifo_cfg == 0x0fff0fffu,
        "Expected TARGET_FIFO_CONFIG=0x0fff0fff, got 0x%x", t_fifo_cfg);
  mmio_region_write32(i2c_base, I2C_TARGET_FIFO_CONFIG_REG_OFFSET, 0u);
}

static void test_fifo_preload_when_disabled(mmio_region_t i2c_base) {
  // Ensure CTRL has ENABLEHOST=0 and ENABLETARGET=0.
  mmio_region_write32(i2c_base, I2C_CTRL_REG_OFFSET, 0u);

  // Configure FMT_THRESH=2 and TX_THRESH=2.
  uint32_t h_cfg =
      bitfield_field32_write(0, I2C_HOST_FIFO_CONFIG_FMT_THRESH_FIELD, 2u);
  mmio_region_write32(i2c_base, I2C_HOST_FIFO_CONFIG_REG_OFFSET, h_cfg);

  uint32_t t_cfg =
      bitfield_field32_write(0, I2C_TARGET_FIFO_CONFIG_TX_THRESH_FIELD, 2u);
  mmio_region_write32(i2c_base, I2C_TARGET_FIFO_CONFIG_REG_OFFSET, t_cfg);

  // At depth 0 (< 2), both FMT_THRESHOLD and TX_THRESHOLD status interrupts
  // must be asserted in INTR_STATE.
  uint32_t intr_state = mmio_region_read32(i2c_base, I2C_INTR_STATE_REG_OFFSET);
  CHECK(bitfield_bit32_read(intr_state, I2C_INTR_STATE_FMT_THRESHOLD_BIT),
        "Expected FMT_THRESHOLD=1 when FMTLVL(0) < FMT_THRESH(2)");
  CHECK(bitfield_bit32_read(intr_state, I2C_INTR_STATE_TX_THRESHOLD_BIT),
        "Expected TX_THRESHOLD=1 when TXLVL(0) < TX_THRESH(2)");

  // In i2c_core.sv, FDATA writes push into fmt_fifo even when ENABLEHOST=0
  // (and controller FSM does not pop fmt_fifo while ENABLEHOST=0).
  mmio_region_write32(i2c_base, I2C_FDATA_REG_OFFSET, 0x11u);
  mmio_region_write32(i2c_base, I2C_FDATA_REG_OFFSET, 0x22u);

  uint32_t h_status =
      mmio_region_read32(i2c_base, I2C_HOST_FIFO_STATUS_REG_OFFSET);
  uint32_t fmtlvl =
      bitfield_field32_read(h_status, I2C_HOST_FIFO_STATUS_FMTLVL_FIELD);
  CHECK(fmtlvl == 2u,
        "Expected FMTLVL=2 after 2 FDATA writes with ENABLEHOST=0, got %u",
        fmtlvl);

  uint32_t status = mmio_region_read32(i2c_base, I2C_STATUS_REG_OFFSET);
  CHECK(!bitfield_bit32_read(status, I2C_STATUS_FMTEMPTY_BIT),
        "Expected STATUS.FMTEMPTY=0 when FMTLVL=2");
  CHECK(bitfield_bit32_read(status, I2C_STATUS_HOSTIDLE_BIT),
        "Expected STATUS.HOSTIDLE=1 when ENABLEHOST=0 even with FMTLVL=2");

  intr_state = mmio_region_read32(i2c_base, I2C_INTR_STATE_REG_OFFSET);
  CHECK(!bitfield_bit32_read(intr_state, I2C_INTR_STATE_FMT_THRESHOLD_BIT),
        "Expected FMT_THRESHOLD=0 when FMTLVL(2) >= FMT_THRESH(2)");

  // Similarly, TXDATA writes push into tx_fifo even when ENABLETARGET=0.
  mmio_region_write32(i2c_base, I2C_TXDATA_REG_OFFSET, 0xaau);
  mmio_region_write32(i2c_base, I2C_TXDATA_REG_OFFSET, 0xbbu);

  uint32_t t_status =
      mmio_region_read32(i2c_base, I2C_TARGET_FIFO_STATUS_REG_OFFSET);
  uint32_t txlvl =
      bitfield_field32_read(t_status, I2C_TARGET_FIFO_STATUS_TXLVL_FIELD);
  CHECK(txlvl == 2u,
        "Expected TXLVL=2 after 2 TXDATA writes with ENABLETARGET=0, got %u",
        txlvl);

  status = mmio_region_read32(i2c_base, I2C_STATUS_REG_OFFSET);
  CHECK(!bitfield_bit32_read(status, I2C_STATUS_TXEMPTY_BIT),
        "Expected STATUS.TXEMPTY=0 when TXLVL=2");

  intr_state = mmio_region_read32(i2c_base, I2C_INTR_STATE_REG_OFFSET);
  CHECK(!bitfield_bit32_read(intr_state, I2C_INTR_STATE_TX_THRESHOLD_BIT),
        "Expected TX_THRESHOLD=0 when TXLVL(2) >= TX_THRESH(2)");

  // Reset both FIFOs via FIFO_CTRL (FMTRST | TXRST).
  uint32_t fifo_ctrl = bitfield_bit32_write(0, I2C_FIFO_CTRL_FMTRST_BIT, true);
  fifo_ctrl = bitfield_bit32_write(fifo_ctrl, I2C_FIFO_CTRL_TXRST_BIT, true);
  mmio_region_write32(i2c_base, I2C_FIFO_CTRL_REG_OFFSET, fifo_ctrl);

  h_status = mmio_region_read32(i2c_base, I2C_HOST_FIFO_STATUS_REG_OFFSET);
  t_status = mmio_region_read32(i2c_base, I2C_TARGET_FIFO_STATUS_REG_OFFSET);
  CHECK(
      bitfield_field32_read(h_status, I2C_HOST_FIFO_STATUS_FMTLVL_FIELD) == 0u,
      "Expected FMTLVL=0 after FMTRST");
  CHECK(
      bitfield_field32_read(t_status, I2C_TARGET_FIFO_STATUS_TXLVL_FIELD) == 0u,
      "Expected TXLVL=0 after TXRST");

  mmio_region_write32(i2c_base, I2C_HOST_FIFO_CONFIG_REG_OFFSET, 0u);
  mmio_region_write32(i2c_base, I2C_TARGET_FIFO_CONFIG_REG_OFFSET, 0u);
}

static void test_alert_test_pulse(mmio_region_t i2c_base,
                                  dif_alert_handler_t *alert_handler) {
  irq_global_ctrl(false);
  mmio_region_write32(i2c_base, I2C_ALERT_TEST_REG_OFFSET, 1u);

  bool is_cause = false;
  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
      alert_handler, kTopEarlgreyAlertIdI2c0FatalFault, &is_cause));
  CHECK(is_cause, "Expected I2C0 fatal_fault alert_cause to be set");

  CHECK_DIF_OK(dif_alert_handler_alert_acknowledge(
      alert_handler, kTopEarlgreyAlertIdI2c0FatalFault));
  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
      alert_handler, kTopEarlgreyAlertIdI2c0FatalFault, &is_cause));
  CHECK(!is_cause,
        "Expected I2C0 fatal_fault alert_cause to clear after W1C ack "
        "(ALERT_TEST must pulse, not latch high)");
}

#include "sw/device/lib/dif/dif_pinmux.h"
#include "sw/device/lib/runtime/hart.h"

static void test_intr_test_status_vs_event(mmio_region_t i2c_base) {
  // Ensure CTRL=0, thresholds=0 so all status interrupts are 0.
  mmio_region_write32(i2c_base, I2C_CTRL_REG_OFFSET, 0u);
  mmio_region_write32(i2c_base, I2C_HOST_FIFO_CONFIG_REG_OFFSET, 0u);
  mmio_region_write32(i2c_base, I2C_TARGET_FIFO_CONFIG_REG_OFFSET, 0u);
  mmio_region_write32(i2c_base, I2C_INTR_TEST_REG_OFFSET, 0u);
  mmio_region_write32(i2c_base, I2C_INTR_STATE_REG_OFFSET, 0xffffffffu);

  uint32_t intr_state = mmio_region_read32(i2c_base, I2C_INTR_STATE_REG_OFFSET);
  CHECK(intr_state == 0u, "Expected INTR_STATE=0 initially, got 0x%x",
        intr_state);

  // In prim_intr_hw.sv:
  // - IntrT("Status") stores intr_test_q in `test_q` (cleared only when SW
  //   writes 0 to INTR_TEST, NOT by RW1C to INTR_STATE).
  // - IntrT("Event") latches in intr_state_q and IS cleared by RW1C to
  //   INTR_STATE.
  uint32_t status_mask = (1u << I2C_INTR_COMMON_CONTROLLER_HALT_BIT) |
                         (1u << I2C_INTR_COMMON_RX_THRESHOLD_BIT) |
                         (1u << I2C_INTR_COMMON_ACQ_THRESHOLD_BIT);
  uint32_t event_mask = (1u << I2C_INTR_COMMON_CMD_COMPLETE_BIT);
  mmio_region_write32(i2c_base, I2C_INTR_TEST_REG_OFFSET,
                      status_mask | event_mask);

  intr_state = mmio_region_read32(i2c_base, I2C_INTR_STATE_REG_OFFSET);
  CHECK(intr_state == (status_mask | event_mask),
        "Expected INTR_STATE=0x%x after INTR_TEST write, got 0x%x",
        status_mask | event_mask, intr_state);

  // RW1C to INTR_STATE clears Event bit (CMD_COMPLETE), while Status bits stay
  // asserted via `test_q` until INTR_TEST is written with 0.
  mmio_region_write32(i2c_base, I2C_INTR_STATE_REG_OFFSET,
                      status_mask | event_mask);
  intr_state = mmio_region_read32(i2c_base, I2C_INTR_STATE_REG_OFFSET);
  CHECK(intr_state == status_mask,
        "Expected Status bits (0x%x) to remain set after INTR_STATE RW1C, got "
        "0x%x",
        status_mask, intr_state);

  // Writing 0 to INTR_TEST clears `test_q` for Status bits.
  mmio_region_write32(i2c_base, I2C_INTR_TEST_REG_OFFSET, 0u);
  intr_state = mmio_region_read32(i2c_base, I2C_INTR_STATE_REG_OFFSET);
  CHECK(intr_state == 0u,
        "Expected INTR_STATE=0 after writing 0 to INTR_TEST, got 0x%x",
        intr_state);
}

static void precharge_i2c0_pads_high(const dif_pinmux_t *pinmux) {
  CHECK_DIF_OK(dif_pinmux_output_select(pinmux, kTopEarlgreyPinmuxMioOutIoa7,
                                        kTopEarlgreyPinmuxOutselConstantOne));
  CHECK_DIF_OK(dif_pinmux_output_select(pinmux, kTopEarlgreyPinmuxMioOutIoa8,
                                        kTopEarlgreyPinmuxOutselConstantOne));
  busy_spin_micros(2);
  CHECK_DIF_OK(dif_pinmux_output_select(pinmux, kTopEarlgreyPinmuxMioOutIoa7,
                                        kTopEarlgreyPinmuxOutselI2c0Sda));
  CHECK_DIF_OK(dif_pinmux_output_select(pinmux, kTopEarlgreyPinmuxMioOutIoa8,
                                        kTopEarlgreyPinmuxOutselI2c0Scl));
  busy_spin_micros(2);
}

static void test_val_and_ovrd(mmio_region_t i2c_base) {
  dif_pinmux_t pinmux;
  CHECK_DIF_OK(dif_pinmux_init(
      mmio_region_from_addr(TOP_EARLGREY_PINMUX_AON_BASE_ADDR), &pinmux));

  // Route I2C0 SDA to IOA7 and SCL to IOA8.
  CHECK_DIF_OK(dif_pinmux_input_select(&pinmux,
                                       kTopEarlgreyPinmuxPeripheralInI2c0Sda,
                                       kTopEarlgreyPinmuxInselIoa7));
  CHECK_DIF_OK(dif_pinmux_input_select(&pinmux,
                                       kTopEarlgreyPinmuxPeripheralInI2c0Scl,
                                       kTopEarlgreyPinmuxInselIoa8));

  mmio_region_write32(i2c_base, I2C_OVRD_REG_OFFSET, 0u);
  precharge_i2c0_pads_high(&pinmux);
  uint32_t val = mmio_region_read32(i2c_base, I2C_VAL_REG_OFFSET);
  CHECK(val == 0xffffffffu,
        "Expected VAL=0xffffffff when bus is idle (SCL=1, SDA=1), got 0x%x",
        val);

  // Override SCL=0, SDA=1 (TXOVRDEN=1, SCLVAL=0, SDAVAL=1 -> 0x5).
  uint32_t ovrd = bitfield_bit32_write(0, I2C_OVRD_TXOVRDEN_BIT, true);
  ovrd = bitfield_bit32_write(ovrd, I2C_OVRD_SCLVAL_BIT, false);
  ovrd = bitfield_bit32_write(ovrd, I2C_OVRD_SDAVAL_BIT, true);
  mmio_region_write32(i2c_base, I2C_OVRD_REG_OFFSET, ovrd);
  busy_spin_micros(5);
  val = mmio_region_read32(i2c_base, I2C_VAL_REG_OFFSET);
  CHECK(val == 0xffff0000u,
        "Expected VAL=0xffff0000 when OVRD drives SCL=0, SDA=1, got 0x%x", val);

  // Override SCL=1, SDA=0 (TXOVRDEN=1, SCLVAL=1, SDAVAL=0 -> 0x3).
  mmio_region_write32(i2c_base, I2C_OVRD_REG_OFFSET, 0u);
  precharge_i2c0_pads_high(&pinmux);
  ovrd = bitfield_bit32_write(0, I2C_OVRD_TXOVRDEN_BIT, true);
  ovrd = bitfield_bit32_write(ovrd, I2C_OVRD_SCLVAL_BIT, true);
  ovrd = bitfield_bit32_write(ovrd, I2C_OVRD_SDAVAL_BIT, false);
  mmio_region_write32(i2c_base, I2C_OVRD_REG_OFFSET, ovrd);
  busy_spin_micros(5);
  val = mmio_region_read32(i2c_base, I2C_VAL_REG_OFFSET);
  CHECK(val == 0x0000ffffu,
        "Expected VAL=0x0000ffff when OVRD drives SCL=1, SDA=0, got 0x%x", val);

  // Disable override (OVRD=0) -> bus returns to idle high (0xffffffff).
  mmio_region_write32(i2c_base, I2C_OVRD_REG_OFFSET, 0u);
  precharge_i2c0_pads_high(&pinmux);
  val = mmio_region_read32(i2c_base, I2C_VAL_REG_OFFSET);
  CHECK(val == 0xffffffffu,
        "Expected VAL=0xffffffff after clearing OVRD, got 0x%x", val);
}

static volatile uint32_t g_fault_count = 0;
static volatile uint32_t g_last_mcause = 0;

void ottf_exception_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = 0;
  uint32_t mepc = 0;
  asm volatile("csrr %0, mcause" : "=r"(mcause));
  asm volatile("csrr %0, mepc" : "=r"(mepc));
  g_last_mcause = mcause;
  g_fault_count++;
  uint16_t insn16 = *(const uint16_t *)mepc;
  mepc += ((insn16 & 0x3u) != 0x3u) ? 2u : 4u;
  asm volatile("csrw mepc, %0" : : "r"(mepc));
}

static void test_subword_permit_and_unmapped_access(mmio_region_t i2c_base) {
  uintptr_t base = (uintptr_t)i2c_base.base;

  // 1. Sub-word write to 4'b1111 register (HOST_FIFO_CONFIG at 0x24) via sb/sh
  // must raise Store Access Fault (mcause = 7) and NOT modify the register.
  mmio_region_write32(i2c_base, I2C_HOST_FIFO_CONFIG_REG_OFFSET, 0x00020003u);
  g_fault_count = 0;
  *(volatile uint8_t *)(base + I2C_HOST_FIFO_CONFIG_REG_OFFSET) = 0x55u;
  CHECK(g_fault_count == 1u && g_last_mcause == 7u,
        "Expected Store Access Fault (mcause=7) on sb to HOST_FIFO_CONFIG");
  *(volatile uint16_t *)(base + I2C_HOST_FIFO_CONFIG_REG_OFFSET) = 0x0007u;
  CHECK(g_fault_count == 2u && g_last_mcause == 7u,
        "Expected Store Access Fault (mcause=7) on sh to HOST_FIFO_CONFIG");
  CHECK(mmio_region_read32(i2c_base, I2C_HOST_FIFO_CONFIG_REG_OFFSET) ==
            0x00020003u,
        "HOST_FIFO_CONFIG must not be mutated by rejected sub-word writes");

  // 2. Sub-word write to 4'b0111 register (HOST_TIMEOUT_CTRL at 0x60) via sh
  // must raise Store Access Fault (mcause = 7) and NOT modify the register.
  mmio_region_write32(i2c_base, I2C_HOST_TIMEOUT_CTRL_REG_OFFSET, 0x12345u);
  g_fault_count = 0;
  *(volatile uint16_t *)(base + I2C_HOST_TIMEOUT_CTRL_REG_OFFSET) = 0x6789u;
  CHECK(g_fault_count == 1u && g_last_mcause == 7u,
        "Expected Store Access Fault (mcause=7) on sh to HOST_TIMEOUT_CTRL");
  CHECK(mmio_region_read32(i2c_base, I2C_HOST_TIMEOUT_CTRL_REG_OFFSET) ==
            0x12345u,
        "HOST_TIMEOUT_CTRL must remain 0x12345 after rejected sh");
  mmio_region_write32(i2c_base, I2C_HOST_TIMEOUT_CTRL_REG_OFFSET, 0u);

  // 3. Sub-word write to 4'b0011 register (INTR_ENABLE at 0x04):
  // - sb at +0 must fail (mcause = 7), sh at +2 must fail (mcause = 7)
  // - sh at +0 must succeed!
  mmio_region_write32(i2c_base, I2C_INTR_ENABLE_REG_OFFSET, 0u);
  g_fault_count = 0;
  *(volatile uint8_t *)(base + I2C_INTR_ENABLE_REG_OFFSET) = 0x15u;
  CHECK(g_fault_count == 1u && g_last_mcause == 7u,
        "Expected Store Access Fault on sb to INTR_ENABLE");
  *(volatile uint16_t *)(base + I2C_INTR_ENABLE_REG_OFFSET + 2u) = 0x15u;
  CHECK(g_fault_count == 2u && g_last_mcause == 7u,
        "Expected Store Access Fault on sh to INTR_ENABLE+2");
  CHECK(mmio_region_read32(i2c_base, I2C_INTR_ENABLE_REG_OFFSET) == 0u,
        "INTR_ENABLE must remain 0 after rejected sub-word writes");

  g_fault_count = 0;
  *(volatile uint16_t *)(base + I2C_INTR_ENABLE_REG_OFFSET) = 0x0015u;
  CHECK(g_fault_count == 0u,
        "Expected sh to INTR_ENABLE+0 (I2C_PERMIT=4'b0011) to succeed");
  CHECK(mmio_region_read32(i2c_base, I2C_INTR_ENABLE_REG_OFFSET) == 0x0015u,
        "Expected INTR_ENABLE=0x15 after sh to +0");
  mmio_region_write32(i2c_base, I2C_INTR_ENABLE_REG_OFFSET, 0u);

  // 4. Sub-word write to 4'b0001 register (CTRL at 0x10):
  // - sb at +1 must fail (mcause = 7)
  // - sb at +0 must succeed!
  mmio_region_write32(i2c_base, I2C_CTRL_REG_OFFSET, 0u);
  g_fault_count = 0;
  *(volatile uint8_t *)(base + I2C_CTRL_REG_OFFSET + 1u) = 0x04u;
  CHECK(g_fault_count == 1u && g_last_mcause == 7u,
        "Expected Store Access Fault on sb to CTRL+1");
  CHECK(mmio_region_read32(i2c_base, I2C_CTRL_REG_OFFSET) == 0u,
        "CTRL must remain 0 after rejected sb to CTRL+1");
  g_fault_count = 0;
  *(volatile uint8_t *)(base + I2C_CTRL_REG_OFFSET) =
      (1u << I2C_CTRL_LLPBK_BIT);
  CHECK(g_fault_count == 0u,
        "Expected sb to CTRL+0 (I2C_PERMIT=4'b0001) to succeed");
  CHECK(mmio_region_read32(i2c_base, I2C_CTRL_REG_OFFSET) ==
            (1u << I2C_CTRL_LLPBK_BIT),
        "Expected CTRL.LLPBK=1 after sb to CTRL+0");
  mmio_region_write32(i2c_base, I2C_CTRL_REG_OFFSET, 0u);
  mmio_region_write32(i2c_base, I2C_HOST_FIFO_CONFIG_REG_OFFSET, 0u);
}

static void test_fifo_ctrl_and_loopback_restart(mmio_region_t i2c_base) {
  dif_pinmux_t pinmux;
  CHECK_DIF_OK(dif_pinmux_init(
      mmio_region_from_addr(TOP_EARLGREY_PINMUX_AON_BASE_ADDR), &pinmux));
  CHECK_DIF_OK(dif_pinmux_output_select(&pinmux, kTopEarlgreyPinmuxMioOutIor0,
                                        kTopEarlgreyPinmuxOutselI2c0Sda));
  CHECK_DIF_OK(dif_pinmux_input_select(&pinmux,
                                       kTopEarlgreyPinmuxPeripheralInI2c0Sda,
                                       kTopEarlgreyPinmuxInselIor0));
  CHECK_DIF_OK(dif_pinmux_output_select(&pinmux, kTopEarlgreyPinmuxMioOutIor1,
                                        kTopEarlgreyPinmuxOutselI2c0Scl));
  CHECK_DIF_OK(dif_pinmux_input_select(&pinmux,
                                       kTopEarlgreyPinmuxPeripheralInI2c0Scl,
                                       kTopEarlgreyPinmuxInselIor1));

  mmio_region_write32(
      i2c_base, I2C_TIMING0_REG_OFFSET,
      (60u << I2C_TIMING0_THIGH_OFFSET) | (60u << I2C_TIMING0_TLOW_OFFSET));
  mmio_region_write32(
      i2c_base, I2C_TIMING1_REG_OFFSET,
      (20u << I2C_TIMING1_T_R_OFFSET) | (4u << I2C_TIMING1_T_F_OFFSET));
  mmio_region_write32(i2c_base, I2C_TIMING2_REG_OFFSET,
                      (60u << I2C_TIMING2_TSU_STA_OFFSET) |
                          (60u << I2C_TIMING2_THD_STA_OFFSET));
  mmio_region_write32(i2c_base, I2C_TIMING3_REG_OFFSET,
                      (10u << I2C_TIMING3_TSU_DAT_OFFSET) |
                          (10u << I2C_TIMING3_THD_DAT_OFFSET));
  mmio_region_write32(
      i2c_base, I2C_TIMING4_REG_OFFSET,
      (60u << I2C_TIMING4_TSU_STO_OFFSET) | (60u << I2C_TIMING4_T_BUF_OFFSET));
  mmio_region_write32(i2c_base, I2C_TARGET_ID_REG_OFFSET,
                      (0x7fu << I2C_TARGET_ID_MASK0_OFFSET) |
                          (0x55u << I2C_TARGET_ID_ADDRESS0_OFFSET));

  uint32_t ctrl =
      (1u << I2C_CTRL_ENABLEHOST_BIT) | (1u << I2C_CTRL_ENABLETARGET_BIT);
  mmio_region_write32(i2c_base, I2C_CTRL_REG_OFFSET, ctrl);

  uint32_t fifo_rst =
      (1u << I2C_FIFO_CTRL_RXRST_BIT) | (1u << I2C_FIFO_CTRL_FMTRST_BIT) |
      (1u << I2C_FIFO_CTRL_ACQRST_BIT) | (1u << I2C_FIFO_CTRL_TXRST_BIT);
  mmio_region_write32(i2c_base, I2C_FIFO_CTRL_REG_OFFSET, fifo_rst);
  mmio_region_write32(i2c_base, I2C_INTR_STATE_REG_OFFSET, 0xffffffffu);

  const uint32_t kDoneMask =
      (1u << I2C_STATUS_HOSTIDLE_BIT) | (1u << I2C_STATUS_FMTEMPTY_BIT);

  // Step 1a: Repeated START to the same target address (0x55 -> 0x55) without
  // intervening STOP so s->in_target_transfer remains true across Repeated
  // START.
  mmio_region_write32(i2c_base, I2C_FDATA_REG_OFFSET,
                      (1u << I2C_FDATA_START_BIT) | (0x55u << 1));
  mmio_region_write32(
      i2c_base, I2C_FDATA_REG_OFFSET,
      (1u << I2C_FDATA_START_BIT) | (1u << I2C_FDATA_STOP_BIT) | (0x55u << 1));
  for (int i = 0; i < 5000; ++i) {
    uint32_t st = mmio_region_read32(i2c_base, I2C_STATUS_REG_OFFSET);
    uint32_t intr = mmio_region_read32(i2c_base, I2C_INTR_STATE_REG_OFFSET);
    if ((st & kDoneMask) == kDoneMask &&
        bitfield_bit32_read(intr, I2C_INTR_STATE_CMD_COMPLETE_BIT)) {
      break;
    }
    busy_spin_micros(2);
  }
  uint32_t intr = mmio_region_read32(i2c_base, I2C_INTR_STATE_REG_OFFSET);
  CHECK(bitfield_bit32_read(intr, I2C_INTR_STATE_CMD_COMPLETE_BIT),
        "Expected CMD_COMPLETE interrupt after same-address repeated-START");

  mmio_region_write32(i2c_base, I2C_FIFO_CTRL_REG_OFFSET, fifo_rst);
  mmio_region_write32(i2c_base, I2C_INTR_STATE_REG_OFFSET, 0xffffffffu);

  // Step 1b: Repeated START across two addresses (0x55 -> 0x56) without STOP.
  mmio_region_write32(i2c_base, I2C_FDATA_REG_OFFSET,
                      (1u << I2C_FDATA_START_BIT) | (0x55u << 1));
  mmio_region_write32(i2c_base, I2C_FDATA_REG_OFFSET,
                      (1u << I2C_FDATA_START_BIT) | (1u << I2C_FDATA_STOP_BIT) |
                          (1u << I2C_FDATA_NAKOK_BIT) | (0x56u << 1));
  for (int i = 0; i < 5000; ++i) {
    uint32_t st = mmio_region_read32(i2c_base, I2C_STATUS_REG_OFFSET);
    intr = mmio_region_read32(i2c_base, I2C_INTR_STATE_REG_OFFSET);
    if ((st & kDoneMask) == kDoneMask &&
        bitfield_bit32_read(intr, I2C_INTR_STATE_CMD_COMPLETE_BIT)) {
      break;
    }
    busy_spin_micros(2);
  }
  intr = mmio_region_read32(i2c_base, I2C_INTR_STATE_REG_OFFSET);
  CHECK(bitfield_bit32_read(intr, I2C_INTR_STATE_CMD_COMPLETE_BIT),
        "Expected CMD_COMPLETE interrupt after loopback repeated-START");

  // Step 2: Reset FIFOs so ACQ_FIFO is empty, then perform Target Read of 0xa5.
  mmio_region_write32(i2c_base, I2C_FIFO_CTRL_REG_OFFSET, fifo_rst);
  mmio_region_write32(i2c_base, I2C_INTR_STATE_REG_OFFSET, 0xffffffffu);
  mmio_region_write32(i2c_base, I2C_TXDATA_REG_OFFSET, 0xa5u);
  mmio_region_write32(i2c_base, I2C_FDATA_REG_OFFSET,
                      (1u << I2C_FDATA_START_BIT) | (0x55u << 1) | 1u);
  mmio_region_write32(
      i2c_base, I2C_FDATA_REG_OFFSET,
      (1u << I2C_FDATA_READB_BIT) | (1u << I2C_FDATA_STOP_BIT) | 1u);
  for (int i = 0; i < 5000; ++i) {
    uint32_t st = mmio_region_read32(i2c_base, I2C_STATUS_REG_OFFSET);
    intr = mmio_region_read32(i2c_base, I2C_INTR_STATE_REG_OFFSET);
    if ((st & kDoneMask) == kDoneMask &&
        bitfield_bit32_read(intr, I2C_INTR_STATE_CMD_COMPLETE_BIT)) {
      break;
    }
    busy_spin_micros(2);
  }
  CHECK((mmio_region_read32(i2c_base, I2C_RDATA_REG_OFFSET) & 0xffu) == 0xa5u,
        "Expected RDATA=0xa5 after target read");

  mmio_region_write32(i2c_base, I2C_CTRL_REG_OFFSET, 0u);
  mmio_region_write32(i2c_base, I2C_FIFO_CTRL_REG_OFFSET, fifo_rst);
  mmio_region_write32(i2c_base, I2C_INTR_STATE_REG_OFFSET, 0xffffffffu);
}

bool test_main(void) {
  mmio_region_t i2c0_base = mmio_region_from_addr(TOP_EARLGREY_I2C0_BASE_ADDR);

  dif_alert_handler_t alert_handler;
  CHECK_DIF_OK(dif_alert_handler_init(
      mmio_region_from_addr(TOP_EARLGREY_ALERT_HANDLER_BASE_ADDR),
      &alert_handler));

  test_csr_masks_and_access(i2c0_base);
  test_fifo_preload_when_disabled(i2c0_base);
  test_intr_test_status_vs_event(i2c0_base);
  test_val_and_ovrd(i2c0_base);
  test_alert_test_pulse(i2c0_base, &alert_handler);
  test_subword_permit_and_unmapped_access(i2c0_base);
  test_fifo_ctrl_and_loopback_restart(i2c0_base);

  LOG_INFO("i2c_consistency_test passed!");
  return true;
}
