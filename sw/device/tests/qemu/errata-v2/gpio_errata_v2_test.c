// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// CW340 FPGA hardware behavior verification for General-Purpose I/O (gpio) on
// Earlgrey trunk-v2:
//
// 1. Bidirectional self-loopback double-XOR cancellation of MIO_PAD_ATTR.INVERT
//    (hw/top_earlgrey/ip_autogen/gpio/rtl/gpio.sv:115-120,
//    hw/ip/prim_xilinx_ultrascale/rtl/prim_pad_wrapper.sv:49-50, 76, 95, 98),
//    MIO_PERIPH_INSEL ConstantZero/ConstantOne bypass (pinmux.sv:470-475), and
//    MIO_PAD_ATTR.INPUT_DISABLE (0x80) pre-invert zero clamp.
// 2. GPIO_PERMIT[18]
// (hw/top_earlgrey/ip_autogen/gpio/rtl/gpio_reg_pkg.sv:283-302)
//    rejects 16-bit halfword writes (sh) to split MASKED_OUT_*/MASKED_OE_*
//    registers and sub-word writes (sb/sh) to read-only DATA_IN with
//    synchronous Store Access Fault (mcause = 7), silently ignores 32-bit sw to
//    DATA_IN, accepts sb at ALERT_TEST+0 (4'b0001) while faulting at +1, and
//    raises Load/Store Access Faults (mcause = 5 / 7) at unmapped offset >=
//    0x48.
// 3. Continuous level-interrupt (INTR_CTRL_EN_LVLLOW / LVLHIGH,
//    gpio.sv:187-194) immediate re-latch on INTR_STATE RW1C while the input
//    level remains active, and ALERT_TEST 1-cycle transient pulse
//    (gpio.sv:197-200).
// 4. Earlgrey v2 adds HW_STRAPS_DATA_IN_VALID (0x40, GPIO_PERMIT[16] = 4'b0001)
//    and HW_STRAPS_DATA_IN (0x44, GPIO_PERMIT[17] = 4'b1111) with
//    GpioAsHwStrapsEn defaulting to 1'b1 in gpio.hjson:72-80 and gpio.sv:16,
//    but top_earlgrey.sv:17, 347 overrides GpioGpioAsHwStrapsEn = 0, activating
//    gen_no_strap_sample (gpio.sv:100-112) so both CSRs permanently read 0 even
//    after cold boot; furthermore, sub-word writes (sb/sh at +0) to read-only
//    HW_STRAPS_DATA_IN_VALID (0x40) succeed without error (d_error = 0) whereas
//    sub-word writes (sb/sh at +0) to read-only HW_STRAPS_DATA_IN (0x44) raise
//    a synchronous Store Access Fault (mcause = 7).

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/memory.h"
#include "sw/device/lib/dif/dif_gpio.h"
#include "sw/device/lib/dif/dif_pinmux.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top/gpio_regs.h"
#include "hw/top/pinmux_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kGpioBase = TOP_EARLGREY_GPIO_BASE_ADDR,
  kPinmuxBase = TOP_EARLGREY_PINMUX_BASE_ADDR,
  kGpioUnmappedOffset = 0x48u,
};

static volatile bool g_expect_bus_fault = false;
static volatile bool g_bus_fault_seen = false;
static volatile uint32_t g_bus_fault_mcause = 0;

static void advance_mepc_over_faulting_insn(void) {
  uint32_t mepc = 0;
  CSR_READ(CSR_REG_MEPC, &mepc);
  uint16_t insn_half = *(const volatile uint16_t *)mepc;
  uint32_t step = ((insn_half & 0x3u) == 0x3u) ? 4u : 2u;
  CSR_WRITE(CSR_REG_MEPC, mepc + step);
}

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = ibex_mcause_read();
  CHECK(g_expect_bus_fault, "Unexpected load/store fault: mcause=0x%x", mcause);
  g_bus_fault_seen = true;
  g_bus_fault_mcause = mcause;
  advance_mepc_over_faulting_insn();
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

static void test_gpio_pad_invert_and_input_disable(void) {
  LOG_INFO(
      "Test 1: MIO_PAD_ATTR.INVERT double-XOR cancellation, Constant0/1 "
      "bypass, and INPUT_DISABLE pre-invert clamp");

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

  // 1. Bidirectional self-loopback with INVERT=1 (0x1):
  // out = d ^ 1 and in_o = 1 ^ out = d, so DATA_IN[2] equals DIRECT_OUT[2].
  abs_mmio_write32(kPinmuxBase + ioa2_attr_off,
                   1u << PINMUX_MIO_PAD_ATTR_0_INVERT_0_BIT);
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OE_REG_OFFSET, 1u << 2);

  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET, 0u);
  busy_spin_micros(10);
  CHECK(
      ((abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) >> 2) & 1u) == 0u,
      "Expected DATA_IN[2]==0 when DIRECT_OUT[2]==0 with INVERT=1");

  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET, 1u << 2);
  busy_spin_micros(10);
  CHECK(
      ((abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) >> 2) & 1u) == 1u,
      "Expected DATA_IN[2]==1 when DIRECT_OUT[2]==1 with INVERT=1");

  // 2. MIO_PERIPH_INSEL ConstantZero (0) and ConstantOne (1) bypass
  // MIO_PAD_ATTR[Ioa2].INVERT = 1.
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OE_REG_OFFSET, 0u);
  CHECK_DIF_OK(dif_pinmux_input_select(&pinmux,
                                       kTopEarlgreyPinmuxPeripheralInGpioGpio2,
                                       kTopEarlgreyPinmuxInselConstantZero));
  busy_spin_micros(10);
  CHECK(
      ((abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) >> 2) & 1u) == 0u,
      "ConstantZero must bypass MIO_PAD_ATTR.INVERT");

  CHECK_DIF_OK(dif_pinmux_input_select(&pinmux,
                                       kTopEarlgreyPinmuxPeripheralInGpioGpio2,
                                       kTopEarlgreyPinmuxInselConstantOne));
  busy_spin_micros(10);
  CHECK(
      ((abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) >> 2) & 1u) == 1u,
      "ConstantOne must bypass MIO_PAD_ATTR.INVERT");

  // 3. MIO_PAD_ATTR.INPUT_DISABLE (0x80) clamps in_raw_o to 0 BEFORE input XOR.
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
      "Expected DATA_IN[2]==0 when INPUT_DISABLE=1, INVERT=0");

  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET, 0u);
  abs_mmio_write32(kPinmuxBase + ioa2_attr_off,
                   (1u << PINMUX_MIO_PAD_ATTR_0_INPUT_DISABLE_0_BIT) |
                       (1u << PINMUX_MIO_PAD_ATTR_0_INVERT_0_BIT));
  busy_spin_micros(10);
  CHECK(
      ((abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) >> 2) & 1u) == 1u,
      "Expected DATA_IN[2]==1 when INPUT_DISABLE=1, INVERT=1");

  abs_mmio_write32(kPinmuxBase + ioa2_attr_off, 0u);
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OE_REG_OFFSET, 0u);
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET, 0u);
}

static void test_gpio_permit_subword_and_data_in_faults(void) {
  LOG_INFO(
      "Test 2: GPIO_PERMIT=4'b1111 rejecting 16-bit sh on "
      "MASKED_OUT_*/MASKED_OE_* and asymmetric DATA_IN write fault");

  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET, 0x12345678u);
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OE_REG_OFFSET, 0x0000a5a5u);

  expect_store16_fault(kGpioBase + GPIO_MASKED_OUT_LOWER_REG_OFFSET, 0xffffu);
  expect_store16_fault(kGpioBase + GPIO_MASKED_OUT_UPPER_REG_OFFSET, 0xffffu);
  expect_store16_fault(kGpioBase + GPIO_MASKED_OE_LOWER_REG_OFFSET, 0xffffu);
  expect_store16_fault(kGpioBase + GPIO_MASKED_OE_UPPER_REG_OFFSET, 0xffffu);
  expect_store8_fault(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET, 0xffu);

  CHECK(abs_mmio_read32(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET) == 0x12345678u,
        "DIRECT_OUT must remain unmodified after faulted sub-word writes");
  CHECK(abs_mmio_read32(kGpioBase + GPIO_DIRECT_OE_REG_OFFSET) == 0x0000a5a5u,
        "DIRECT_OE must remain unmodified after faulted sub-word writes");

  abs_mmio_write32(kGpioBase + GPIO_DATA_IN_REG_OFFSET, 0xffffffffu);
  expect_store16_fault(kGpioBase + GPIO_DATA_IN_REG_OFFSET, 0xffffu);
  expect_store8_fault(kGpioBase + GPIO_DATA_IN_REG_OFFSET, 0xffu);

  abs_mmio_write8(kGpioBase + GPIO_ALERT_TEST_REG_OFFSET, 0u);
  expect_store8_fault(kGpioBase + GPIO_ALERT_TEST_REG_OFFSET + 1u, 0u);

  expect_load_fault(kGpioBase + kGpioUnmappedOffset);
  expect_store32_fault(kGpioBase + kGpioUnmappedOffset, 0xdeadbeefu);

  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OUT_REG_OFFSET, 0u);
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OE_REG_OFFSET, 0u);
}

static void test_gpio_level_intr_relatch_and_alert_pulse(void) {
  LOG_INFO(
      "Test 3: Continuous level-interrupt INTR_STATE RW1C re-latch and "
      "ALERT_TEST pulse");

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

  abs_mmio_write32(kGpioBase + GPIO_INTR_STATE_REG_OFFSET, 1u);
  busy_spin_micros(5);
  CHECK((abs_mmio_read32(kGpioBase + GPIO_INTR_STATE_REG_OFFSET) & 1u) == 1u,
        "Expected INTR_STATE[0] to remain 1 after RW1C while LVLLOW active");

  abs_mmio_write32(kGpioBase + GPIO_INTR_CTRL_EN_LVLLOW_REG_OFFSET, 0u);
  abs_mmio_write32(kGpioBase + GPIO_INTR_STATE_REG_OFFSET, 0xffffffffu);
  CHECK(abs_mmio_read32(kGpioBase + GPIO_INTR_STATE_REG_OFFSET) == 0u);

  for (int i = 0; i < 2; ++i) {
    CHECK_STATUS_OK(
        ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdGpioFatalFault));
    abs_mmio_write32(kGpioBase + GPIO_ALERT_TEST_REG_OFFSET, 1u);
    CHECK_STATUS_OK(
        ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdGpioFatalFault));
  }
}

static void test_gpio_v2_hw_straps_disabled_and_permit_asymmetry(void) {
  LOG_INFO(
      "Test 4: Earlgrey v2 GpioAsHwStrapsEn=0 tie-off of "
      "HW_STRAPS_DATA_IN_VALID/HW_STRAPS_DATA_IN and permit mask asymmetry");

  // 1. Even though gpio.hjson:74 and gpio.sv:16 default GpioAsHwStrapsEn = 1,
  // top_earlgrey.sv:17 overrides GpioGpioAsHwStrapsEn = 0, activating
  // gen_no_strap_sample (gpio.sv:100-112) so both HW_STRAPS_DATA_IN_VALID
  // (0x40) and HW_STRAPS_DATA_IN (0x44) permanently read 0 after cold boot!
  uint32_t straps_valid =
      abs_mmio_read32(kGpioBase + GPIO_HW_STRAPS_DATA_IN_VALID_REG_OFFSET);
  uint32_t straps_data =
      abs_mmio_read32(kGpioBase + GPIO_HW_STRAPS_DATA_IN_REG_OFFSET);
  CHECK(straps_valid == 0u,
        "Expected HW_STRAPS_DATA_IN_VALID (0x40) == 0 when "
        "GpioGpioAsHwStrapsEn=0, got 0x%x",
        straps_valid);
  CHECK(straps_data == 0u,
        "Expected HW_STRAPS_DATA_IN (0x44) == 0 when GpioGpioAsHwStrapsEn=0, "
        "got 0x%x",
        straps_data);

  // 2. Both HW_STRAPS_DATA_IN_VALID (0x40) and HW_STRAPS_DATA_IN (0x44) are
  // swaccess: "ro", yet GPIO_PERMIT[16] = 4'b0001 while GPIO_PERMIT[17] =
  // 4'b1111 (gpio_reg_pkg.sv:300-301):
  // - 8-bit/16-bit/32-bit writes at offset +0 of HW_STRAPS_DATA_IN_VALID (0x40)
  //   succeed without fault (d_error = 0), while an 8-bit write at 0x41 faults
  //   (mcause = 7).
  abs_mmio_write8(kGpioBase + GPIO_HW_STRAPS_DATA_IN_VALID_REG_OFFSET, 1u);
  *(volatile uint16_t *)(kGpioBase + GPIO_HW_STRAPS_DATA_IN_VALID_REG_OFFSET) =
      1u;
  abs_mmio_write32(kGpioBase + GPIO_HW_STRAPS_DATA_IN_VALID_REG_OFFSET, 1u);
  expect_store8_fault(kGpioBase + GPIO_HW_STRAPS_DATA_IN_VALID_REG_OFFSET + 1u,
                      1u);

  // - Conversely, 8-bit and 16-bit writes at offset +0 of HW_STRAPS_DATA_IN
  //   (0x44) violate GPIO_PERMIT[17] = 4'b1111 and raise Store Access Fault
  //   (mcause = 7), while a 32-bit write is silently ignored!
  abs_mmio_write32(kGpioBase + GPIO_HW_STRAPS_DATA_IN_REG_OFFSET, 0xdeadbeefu);
  expect_store8_fault(kGpioBase + GPIO_HW_STRAPS_DATA_IN_REG_OFFSET, 0xffu);
  expect_store16_fault(kGpioBase + GPIO_HW_STRAPS_DATA_IN_REG_OFFSET, 0xffffu);
  CHECK(abs_mmio_read32(kGpioBase + GPIO_HW_STRAPS_DATA_IN_REG_OFFSET) == 0u,
        "HW_STRAPS_DATA_IN must remain 0");
}

bool test_main(void) {
  LOG_INFO("Starting gpio_errata_v2_test (P26) on CW340 FPGA...");
  test_gpio_pad_invert_and_input_disable();
  test_gpio_permit_subword_and_data_in_faults();
  test_gpio_level_intr_relatch_and_alert_pulse();
  test_gpio_v2_hw_straps_disabled_and_permit_asymmetry();
  LOG_INFO("All gpio_errata_v2_test checks PASSED!");
  return true;
}
