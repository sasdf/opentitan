// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/memory.h"
#include "sw/device/lib/dif/dif_pinmux.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "gpio_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "pinmux_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kGpioBase = TOP_EARLGREY_GPIO_BASE_ADDR,
  kPinmuxBase = TOP_EARLGREY_PINMUX_AON_BASE_ADDR,
  kGpioUnmappedOffset = 0x40u,
};

static volatile bool g_expect_bus_fault = false;
static volatile bool g_bus_fault_seen = false;
static volatile uint32_t g_bus_fault_mcause = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = ibex_mcause_read();
  CHECK(g_expect_bus_fault, "Unexpected load/store fault: mcause=0x%x", mcause);
  g_bus_fault_seen = true;
  g_bus_fault_mcause = mcause;
}

static void expect_load_fault(uint32_t addr) {
  g_expect_bus_fault = true;
  g_bus_fault_seen = false;
  g_bus_fault_mcause = 0;
  asm volatile("" ::: "memory");
  (void)abs_mmio_read32(addr);
  asm volatile("" ::: "memory");
  g_expect_bus_fault = false;
  CHECK(g_bus_fault_seen, "Expected Load Access Fault (mcause=5) at 0x%08x",
        addr);
  CHECK(g_bus_fault_mcause == kIbexExcLoadAccessFault,
        "Expected mcause=5, got %u at 0x%08x", g_bus_fault_mcause, addr);
}

static void expect_store32_fault(uint32_t addr, uint32_t val) {
  g_expect_bus_fault = true;
  g_bus_fault_seen = false;
  g_bus_fault_mcause = 0;
  asm volatile("" ::: "memory");
  abs_mmio_write32(addr, val);
  asm volatile("" ::: "memory");
  g_expect_bus_fault = false;
  CHECK(g_bus_fault_seen, "Expected Store Access Fault (mcause=7) at 0x%08x",
        addr);
  CHECK(g_bus_fault_mcause == kIbexExcStoreAccessFault,
        "Expected mcause=7, got %u at 0x%08x", g_bus_fault_mcause, addr);
}

static void expect_store16_fault(uint32_t addr, uint16_t val) {
  g_expect_bus_fault = true;
  g_bus_fault_seen = false;
  g_bus_fault_mcause = 0;
  asm volatile("" ::: "memory");
  *(volatile uint16_t *)addr = val;
  asm volatile("" ::: "memory");
  g_expect_bus_fault = false;
  CHECK(g_bus_fault_seen,
        "Expected Store Access Fault (mcause=7) on 16-bit write at 0x%08x",
        addr);
  CHECK(g_bus_fault_mcause == kIbexExcStoreAccessFault,
        "Expected mcause=7, got %u at 0x%08x", g_bus_fault_mcause, addr);
}

static void expect_store8_fault(uint32_t addr, uint8_t val) {
  g_expect_bus_fault = true;
  g_bus_fault_seen = false;
  g_bus_fault_mcause = 0;
  asm volatile("" ::: "memory");
  abs_mmio_write8(addr, val);
  asm volatile("" ::: "memory");
  g_expect_bus_fault = false;
  CHECK(g_bus_fault_seen,
        "Expected Store Access Fault (mcause=7) on 8-bit write at 0x%08x",
        addr);
  CHECK(g_bus_fault_mcause == kIbexExcStoreAccessFault,
        "Expected mcause=7, got %u at 0x%08x", g_bus_fault_mcause, addr);
}

/**
 * [gpio.sv:62-63] (GPIO-E2-01 — BENIGN_RTL_IMPL_DETAIL):
 * Verify `GPIO.DATA_IN` bidirectional self-loopback double-XOR cancellation of
 * `MIO_PAD_ATTR.INVERT` (`prim_xilinx_ultrascale_pad_wrapper.sv:76, 97`),
 * `MIO_PERIPH_INSEL` `ConstantZero`/`ConstantOne` inversion bypass
 * (`pinmux.sv:470-475`), and `MIO_PAD_ATTR.INPUT_DISABLE` (`0x80`) pre-invert
 * clamp (`in_raw_o = 0` before `^ attr_i.invert`).
 */
static void test_gpio_pad_invert_and_input_disable(void) {
  LOG_INFO(
      "Verifying [gpio.sv:62-63] (BENIGN_RTL_IMPL_DETAIL): "
      "MIO_PAD_ATTR.INVERT double-XOR cancellation, Constant0/1 bypass, and "
      "INPUT_DISABLE pre-invert clamp");

  dif_pinmux_t pinmux;
  CHECK_DIF_OK(dif_pinmux_init(mmio_region_from_addr(kPinmuxBase), &pinmux));
  CHECK_DIF_OK(dif_pinmux_output_select(&pinmux, kTopEarlgreyPinmuxMioOutIoa2,
                                        kTopEarlgreyPinmuxOutselGpioGpio2));
  CHECK_DIF_OK(dif_pinmux_input_select(&pinmux,
                                       kTopEarlgreyPinmuxPeripheralInGpioGpio2,
                                       kTopEarlgreyPinmuxInselIoa2));

  uint32_t ioa2_attr_off =
      PINMUX_MIO_PAD_ATTR_0_REG_OFFSET +
      (uint32_t)kTopEarlgreyPinmuxMioOutIoa2 * sizeof(uint32_t);

  /* 1. Bidirectional self-loopback with INVERT=1 (`0x1`):
   * `out = d ^ 1` and `in_o = 1 ^ out = d`, so DATA_IN[2] equals DIRECT_OUT[2].
   */
  abs_mmio_write32(kPinmuxBase + ioa2_attr_off,
                   1u << PINMUX_MIO_PAD_ATTR_0_INVERT_0_BIT);
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OE_REG_OFFSET, 1u << 2);

  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET, 0u);
  busy_spin_micros(10);
  CHECK(
      ((abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) >> 2) & 1u) == 0u,
      "[gpio.sv:62-63] Expected DATA_IN[2]==0 when DIRECT_OUT[2]==0 with "
      "INVERT=1 (double-XOR cancellation)");

  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET, 1u << 2);
  busy_spin_micros(10);
  CHECK(
      ((abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) >> 2) & 1u) == 1u,
      "[gpio.sv:62-63] Expected DATA_IN[2]==1 when DIRECT_OUT[2]==1 with "
      "INVERT=1 (double-XOR cancellation)");

  /* 2. MIO_PERIPH_INSEL ConstantZero (`0`) and ConstantOne (`1`) bypass
   * `MIO_PAD_ATTR[Ioa2].INVERT = 1`. */
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OE_REG_OFFSET, 0u);
  CHECK_DIF_OK(dif_pinmux_input_select(&pinmux,
                                       kTopEarlgreyPinmuxPeripheralInGpioGpio2,
                                       kTopEarlgreyPinmuxInselConstantZero));
  busy_spin_micros(10);
  CHECK(
      ((abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) >> 2) & 1u) == 0u,
      "[gpio.sv:62-63] ConstantZero must bypass MIO_PAD_ATTR.INVERT");

  CHECK_DIF_OK(dif_pinmux_input_select(&pinmux,
                                       kTopEarlgreyPinmuxPeripheralInGpioGpio2,
                                       kTopEarlgreyPinmuxInselConstantOne));
  busy_spin_micros(10);
  CHECK(
      ((abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) >> 2) & 1u) == 1u,
      "[gpio.sv:62-63] ConstantOne must bypass MIO_PAD_ATTR.INVERT");

  /* 3. MIO_PAD_ATTR.INPUT_DISABLE (`0x80`) clamps `in_raw_o` to `0` BEFORE
   * the input XOR (`in_o = attr_i.invert ^ 0`):
   * - With INPUT_DISABLE=1, INVERT=0 (`0x80`), driving DIRECT_OUT[2]=1 yields
   *   DATA_IN[2]==0.
   * - With INPUT_DISABLE=1, INVERT=1 (`0x81`), driving DIRECT_OUT[2]=0 yields
   *   DATA_IN[2]==1 (`1 ^ 0 == 1`)! */
  CHECK_DIF_OK(dif_pinmux_input_select(&pinmux,
                                       kTopEarlgreyPinmuxPeripheralInGpioGpio2,
                                       kTopEarlgreyPinmuxInselIoa2));
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OE_REG_OFFSET, 1u << 2);
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET, 1u << 2);
  abs_mmio_write32(kPinmuxBase + ioa2_attr_off,
                   1u << PINMUX_MIO_PAD_ATTR_0_INPUT_DISABLE_0_BIT);
  busy_spin_micros(10);
  CHECK(
      ((abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) >> 2) & 1u) == 0u,
      "[gpio.sv:62-63] Expected DATA_IN[2]==0 when INPUT_DISABLE=1, "
      "INVERT=0");

  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET, 0u);
  abs_mmio_write32(kPinmuxBase + ioa2_attr_off,
                   (1u << PINMUX_MIO_PAD_ATTR_0_INPUT_DISABLE_0_BIT) |
                       (1u << PINMUX_MIO_PAD_ATTR_0_INVERT_0_BIT));
  busy_spin_micros(10);
  CHECK(
      ((abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) >> 2) & 1u) == 1u,
      "[gpio.sv:62-63] Expected DATA_IN[2]==1 when INPUT_DISABLE=1, "
      "INVERT=1 (pre-invert clamp 0 ^ 1 == 1)");

  /* Restore pad attributes and GPIO OE/OUT. */
  abs_mmio_write32(kPinmuxBase + ioa2_attr_off, 0u);
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OE_REG_OFFSET, 0u);
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET, 0u);
}

/**
 * [gpio_reg_pkg.sv:247-264] (GPIO-E2-02 — INTENDED_SECURITY_HARDENING):
 * Verify `GPIO_PERMIT[16]` (`gpio_reg_pkg.sv:247-264`) rejects 16-bit halfword
 * (`sh`) and 8-bit (`sb`) writes to `MASKED_OUT_*` / `MASKED_OE_*` /
 * `DIRECT_OUT` (`GPIO_PERMIT = 4'b1111`) with synchronous Store Access Fault
 * (`mcause = 7`), silently ignores 32-bit `sw` to read-only `DATA_IN` (`0x10`)
 * while faulting (`mcause = 7`) on sub-word writes to `DATA_IN`, allows `sb`
 * to `ALERT_TEST+0` (`GPIO_PERMIT = 4'b0001`) while faulting at `ALERT_TEST+1`,
 * and raises Load/Store Access Faults (`mcause = 5 / 7`) at offset `>= 0x40`.
 */
static void test_gpio_permit_subword_and_data_in_faults(void) {
  LOG_INFO(
      "Verifying [gpio_reg_pkg.sv:247-264] (INTENDED_SECURITY_HARDENING): "
      "GPIO_PERMIT=4'b1111 rejecting 16-bit sh on MASKED_OUT_*/MASKED_OE_* and "
      "asymmetric DATA_IN write fault");

  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET, 0x12345678u);
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OE_REG_OFFSET, 0x0000a5a5u);

  /* 1. 16-bit halfword write (`sh`) to `MASKED_OUT_LOWER` (`0x18`),
   * `MASKED_OUT_UPPER` (`0x1c`), `MASKED_OE_LOWER` (`0x24`), and
   * `MASKED_OE_UPPER` (`0x28`) must fault (`mcause = 7`) without modifying
   * `DIRECT_OUT` / `DIRECT_OE`! */
  expect_store16_fault(kGpioBase + GPIO_MASKED_OUT_LOWER_REG_OFFSET, 0xffffu);
  expect_store16_fault(kGpioBase + GPIO_MASKED_OUT_UPPER_REG_OFFSET, 0xffffu);
  expect_store16_fault(kGpioBase + GPIO_MASKED_OE_LOWER_REG_OFFSET, 0xffffu);
  expect_store16_fault(kGpioBase + GPIO_MASKED_OE_UPPER_REG_OFFSET, 0xffffu);
  expect_store8_fault(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET, 0xffu);

  CHECK(abs_mmio_read32(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET) == 0x12345678u,
        "DIRECT_OUT must remain unmodified after faulted sub-word writes");
  CHECK(abs_mmio_read32(kGpioBase + GPIO_DIRECT_OE_REG_OFFSET) == 0x0000a5a5u,
        "DIRECT_OE must remain unmodified after faulted sub-word writes");

  /* 2. Asymmetric write response on read-only `DATA_IN` (`0x10`,
   * `GPIO_PERMIT[4] = 4'b1111`):
   * - 32-bit `sw` (`reg_be = 4'b1111`) is silently ignored (`d_error = 0`)
   * - 16-bit `sh` or 8-bit `sb` violates `GPIO_PERMIT[4]` and raises Store
   *   Access Fault (`mcause = 7`)! */
  abs_mmio_write32(kGpioBase + GPIO_DATA_IN_REG_OFFSET, 0xffffffffu);
  expect_store16_fault(kGpioBase + GPIO_DATA_IN_REG_OFFSET, 0xffffu);
  expect_store8_fault(kGpioBase + GPIO_DATA_IN_REG_OFFSET, 0xffu);

  /* 3. `ALERT_TEST` (`0x0c`, `GPIO_PERMIT[3] = 4'b0001`) accepts 8-bit write at
   * `+0` (`reg_be = 4'b0001`) and faults at `+1` (`reg_be = 4'b0010`). */
  abs_mmio_write8(kGpioBase + GPIO_ALERT_TEST_REG_OFFSET, 0u);
  expect_store8_fault(kGpioBase + GPIO_ALERT_TEST_REG_OFFSET + 1u, 0u);

  /* 4. Unmapped offset `0x40` (`addrmiss`) -> Load/Store Access Fault! */
  expect_load_fault(kGpioBase + kGpioUnmappedOffset);
  expect_store32_fault(kGpioBase + kGpioUnmappedOffset, 0xdeadbeefu);

  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET, 0u);
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OE_REG_OFFSET, 0u);
}

/**
 * [gpio.sv:123-152] (GPIO-E4-01 — BENIGN_RTL_IMPL_DETAIL):
 * Verify continuous level-interrupt (`INTR_CTRL_EN_LVLLOW` / `LVLHIGH`)
 * immediate re-latch on `INTR_STATE` `RW1C` while input level remains active
 * (`gpio.sv:139-145`) and `ALERT_TEST` (`0x0c`) 1-cycle transient pulse
 * (`gpio.sv:149-152`).
 */
static void test_gpio_level_intr_relatch_and_alert_pulse(void) {
  LOG_INFO(
      "Verifying [gpio.sv:123-152] (BENIGN_RTL_IMPL_DETAIL): "
      "continuous level-interrupt INTR_STATE RW1C re-latch and ALERT_TEST "
      "pulse");

  dif_pinmux_t pinmux;
  CHECK_DIF_OK(dif_pinmux_init(mmio_region_from_addr(kPinmuxBase), &pinmux));
  CHECK_DIF_OK(dif_pinmux_input_select(&pinmux,
                                       kTopEarlgreyPinmuxPeripheralInGpioGpio0,
                                       kTopEarlgreyPinmuxInselConstantZero));
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OE_REG_OFFSET, 0u);
  abs_mmio_write32(kGpioBase + GPIO_INTR_CTRL_EN_LVLLOW_REG_OFFSET, 1u);
  busy_spin_micros(10);

  CHECK((abs_mmio_read32(kGpioBase + GPIO_INTR_STATE_REG_OFFSET) & 1u) == 1u,
        "Expected INTR_STATE[0]==1 when LVLLOW=1 and input=0");

  /* RW1C clear of INTR_STATE[0] must immediately re-latch to 1 while LVLLOW=1
   * and input=0! */
  abs_mmio_write32(kGpioBase + GPIO_INTR_STATE_REG_OFFSET, 1u);
  busy_spin_micros(5);
  CHECK((abs_mmio_read32(kGpioBase + GPIO_INTR_STATE_REG_OFFSET) & 1u) == 1u,
        "[gpio.sv:123-152] Expected INTR_STATE[0] to remain 1 after RW1C while "
        "LVLLOW condition is active");

  /* Once LVLLOW is disabled, RW1C clears INTR_STATE[0] to 0. */
  abs_mmio_write32(kGpioBase + GPIO_INTR_CTRL_EN_LVLLOW_REG_OFFSET, 0u);
  abs_mmio_write32(kGpioBase + GPIO_INTR_STATE_REG_OFFSET, 0xffffffffu);
  CHECK(abs_mmio_read32(kGpioBase + GPIO_INTR_STATE_REG_OFFSET) == 0u);

  /* Verify `ALERT_TEST` 1-cycle transient pulse on repeated writes. */
  for (int i = 0; i < 2; ++i) {
    CHECK_STATUS_OK(
        ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdGpioFatalFault));
    abs_mmio_write32(kGpioBase + GPIO_ALERT_TEST_REG_OFFSET, 1u);
    CHECK_STATUS_OK(
        ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdGpioFatalFault));
  }
}

bool test_main(void) {
  LOG_INFO("Starting gpio_errata_test (P35) on CW340 FPGA / QEMU...");
  test_gpio_pad_invert_and_input_disable();
  test_gpio_permit_subword_and_data_in_faults();
  test_gpio_level_intr_relatch_and_alert_pulse();
  LOG_INFO("All gpio_errata_test checks PASSED!");
  return true;
}
