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
  // 1. STATUS reset value: FMTEMPTY(bit 2) | HOSTIDLE(bit 3) | TARGETIDLE(bit 4)
  // | RXEMPTY(bit 5) | TXEMPTY(bit 8) | ACQEMPTY(bit 9) = 0x33c.
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

  // 3. HOST_TIMEOUT_CTRL (offset 0x60) is 20 bits wide [19:0] (mask 0x000fffff).
  mmio_region_write32(i2c_base, I2C_HOST_TIMEOUT_CTRL_REG_OFFSET, 0xffffffffu);
  uint32_t host_to =
      mmio_region_read32(i2c_base, I2C_HOST_TIMEOUT_CTRL_REG_OFFSET);
  CHECK(host_to == 0x000fffffu,
        "Expected HOST_TIMEOUT_CTRL=0x000fffff after writing 0xffffffff, got "
        "0x%x",
        host_to);
  mmio_region_write32(i2c_base, I2C_HOST_TIMEOUT_CTRL_REG_OFFSET, 0u);

  // 4. TIMING0..4, TIMEOUT_CTRL, TARGET_ID, HOST_FIFO_CONFIG, TARGET_FIFO_CONFIG
  // field masks.
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
  CHECK(fmtlvl == 2u, "Expected FMTLVL=2 after 2 FDATA writes with ENABLEHOST=0, got %u",
        fmtlvl);

  uint32_t status = mmio_region_read32(i2c_base, I2C_STATUS_REG_OFFSET);
  CHECK(!bitfield_bit32_read(status, I2C_STATUS_FMTEMPTY_BIT),
        "Expected STATUS.FMTEMPTY=0 when FMTLVL=2");

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
  CHECK(txlvl == 2u, "Expected TXLVL=2 after 2 TXDATA writes with ENABLETARGET=0, got %u",
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
  CHECK(bitfield_field32_read(h_status, I2C_HOST_FIFO_STATUS_FMTLVL_FIELD) == 0u,
        "Expected FMTLVL=0 after FMTRST");
  CHECK(bitfield_field32_read(t_status, I2C_TARGET_FIFO_STATUS_TXLVL_FIELD) == 0u,
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

bool test_main(void) {
  mmio_region_t i2c0_base = mmio_region_from_addr(TOP_EARLGREY_I2C0_BASE_ADDR);

  dif_alert_handler_t alert_handler;
  CHECK_DIF_OK(dif_alert_handler_init(
      mmio_region_from_addr(TOP_EARLGREY_ALERT_HANDLER_BASE_ADDR),
      &alert_handler));

  test_csr_masks_and_access(i2c0_base);
  test_fifo_preload_when_disabled(i2c0_base);
  test_alert_test_pulse(i2c0_base, &alert_handler);

  LOG_INFO("i2c_consistency_test passed!");
  return true;
}
