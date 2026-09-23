// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "gpio_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "pinmux_regs.h"
#include "pwm_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kPwmBase = TOP_EARLGREY_PWM_AON_BASE_ADDR,
  kPinmuxBase = TOP_EARLGREY_PINMUX_AON_BASE_ADDR,
  kGpioBase = TOP_EARLGREY_GPIO_BASE_ADDR,
};

static volatile uint32_t g_store_fault_count = 0;
static volatile uint32_t g_last_mcause = 0;

void ottf_exception_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = ibex_mcause_read();
  g_last_mcause = mcause;
  if (mcause == kIbexExcStoreAccessFault) {
    g_store_fault_count++;
    uint32_t mepc = ibex_mepc_read();
    uint16_t insn16 = *(const volatile uint16_t *)mepc;
    uint32_t step = ((insn16 & 0x3u) != 0x3u) ? 2u : 4u;
    CSR_WRITE(CSR_REG_MEPC, mepc + step);
    return;
  }
  ottf_generic_fault_print(exc_info, "Unhandled exception", mcause);
  abort();
}

static inline void mmio_write16(uint32_t addr, uint16_t val) {
  asm volatile("sh %1, 0(%0)" : : "r"(addr), "r"(val) : "memory");
}

static inline uint32_t sample_pwm0(void) {
  return abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) & 0x1u;
}

static void verify_pwm_invert_when_disabled(void) {
  LOG_INFO(
      "Verifying [pwm_chan.sv:182-186] (SPEC_DOC_ERRATA / "
      "BENIGN_RTL_IMPL_DETAIL): "
      "INVERT drives pwm_o high even when PWM_EN=0 and CFG.CNTR_EN=0");

  abs_mmio_write32(kPwmBase + PWM_PWM_EN_REG_OFFSET, 0x0u);
  abs_mmio_write32(kPwmBase + PWM_CFG_REG_OFFSET, 0x0u);
  abs_mmio_write32(kPwmBase + PWM_INVERT_REG_OFFSET, 0x0u);
  busy_spin_micros(30);
  CHECK(sample_pwm0() == 0u);

  // Enable INVERT[0] = 1 while PWM_EN = 0 and CFG.CNTR_EN = 0.
  // pwm_chan.sv:182-186 evaluates pwm_o = invert_i ? ~pwm_int : pwm_int
  // after !pwm_en_i clamps pwm_int = 0, immediately driving pwm_o[0] = 1.
  abs_mmio_write32(kPwmBase + PWM_INVERT_REG_OFFSET, 0x1u);
  busy_spin_micros(30);
  CHECK(sample_pwm0() == 1u);

  // Restore INVERT[0] = 0 and confirm pwm_o[0] returns to 0.
  abs_mmio_write32(kPwmBase + PWM_INVERT_REG_OFFSET, 0x0u);
  busy_spin_micros(30);
  CHECK(sample_pwm0() == 0u);
  LOG_INFO("[pwm_chan.sv:182-186] confirmed: pwm_o=%u when INVERT=1, PWM_EN=0",
           1u);
}

static void verify_pwm_phase_ctr_clear_and_ungated_output(void) {
  LOG_INFO(
      "Verifying [pwm_core.sv:83-85] (TRUE_SILICON_ERRATA): "
      "phase_ctr_en = beat_end & (clr_phase_cntr | cntr_en) fails to reset "
      "phase_ctr_q when beat_ctr_q != clk_div and leaves pwm_o driven when "
      "CFG.CNTR_EN=0");

  // Cleanly reset beat_ctr_q and phase_ctr_q to 0 using CLK_DIV = 0, CNTR_EN =
  // 0 across two writes separated by >= 6 AON ticks.
  abs_mmio_write32(kPwmBase + PWM_PWM_EN_REG_OFFSET, 0x0u);
  abs_mmio_write32(kPwmBase + PWM_INVERT_REG_OFFSET, 0x0u);
  abs_mmio_write32(kPwmBase + PWM_CFG_REG_OFFSET, 0x0u);
  busy_spin_micros(30);
  abs_mmio_write32(kPwmBase + PWM_CFG_REG_OFFSET, 0x0u);
  busy_spin_micros(30);

  // Configure Channel 0: PHASE_DELAY = 0, BLINK_EN = 0, HTBT_EN = 0,
  // DUTY_CYCLE_0.A = 0x8000 (50% duty cycle: pwm_o = 1 when phase_ctr_q ==
  // 0x0000, pwm_o = 0 when phase_ctr_q == 0x8000 at DC_RESN = 0).
  abs_mmio_write32(kPwmBase + PWM_PWM_PARAM_0_REG_OFFSET, 0x0u);
  abs_mmio_write32(kPwmBase + PWM_DUTY_CYCLE_0_REG_OFFSET, 0x80008000u);
  busy_spin_micros(30);

  // Start counter with CLK_DIV = 200 (~1 ms per beat at 200 kHz AON clock),
  // DC_RESN = 0 (2 beats per cycle: beat 0 -> phase 0x0000, beat 1 -> phase
  // 0x8000), CNTR_EN = 1, and enable Channel 0 (PWM_EN = 1).
  const uint32_t kClkDiv = 200u;
  abs_mmio_write32(kPwmBase + PWM_CFG_REG_OFFSET, (1u << 31) | kClkDiv);
  abs_mmio_write32(kPwmBase + PWM_PWM_EN_REG_OFFSET, 0x1u);
  busy_spin_micros(30);

  // During Beat 0 (phase_ctr_q == 0x0000), sample_pwm0() is 1.
  CHECK(sample_pwm0() == 1u);

  // Poll until Beat 1 begins (phase_ctr_q advances to 0x8000 -> sample_pwm0()
  // == 0).
  bool saw_beat1_low = false;
  for (uint32_t i = 0; i < 600u; ++i) {
    if (sample_pwm0() == 0u) {
      saw_beat1_low = true;
      break;
    }
    busy_spin_micros(5);
  }
  CHECK(saw_beat1_low);

  // Immediately write CFG with CNTR_EN = 0, CLK_DIV = 200 while near the start
  // of Beat 1 (0 < beat_ctr_q << 200, so beat_end == 0).
  // Per pwm_core.sv:85, phase_ctr_en = beat_end & (clr_phase_cntr | cntr_en) ==
  // 0, so phase_ctr_q fails to reset to 0x0000 and remains frozen at 0x8000!
  abs_mmio_write32(kPwmBase + PWM_CFG_REG_OFFSET, kClkDiv);
  busy_spin_micros(50);

  // Because phase_ctr_q is still frozen at 0x8000 (not reset to 0x0000),
  // sample_pwm0() remains 0 even though DUTY_CYCLE_0.A = 0x8000 would drive 1
  // if phase_ctr_q had reset to 0x0000!
  uint32_t frozen_out = sample_pwm0();
  CHECK(frozen_out == 0u);

  // Now apply the [pwm_core.sv:83-85] workaround while CFG.CNTR_EN remains 0:
  // Because the previous CFG write reset beat_ctr_q to 0, writing CFG = 0
  // (CLK_DIV = 0, CNTR_EN = 0) makes beat_end = (beat_ctr_q == clk_div) = (0 ==
  // 0) = 1, asserting phase_ctr_en = 1 and resetting phase_ctr_q to 0x0000! And
  // because pwm_chan.sv:182-186 does NOT gate pwm_int by CFG.CNTR_EN, resetting
  // phase_ctr_q to 0x0000 immediately drives sample_pwm0() = 1 while CNTR_EN =
  // 0!
  abs_mmio_write32(kPwmBase + PWM_CFG_REG_OFFSET, 0x0u);
  busy_spin_micros(50);
  uint32_t reset_phase_out = sample_pwm0();
  CHECK(reset_phase_out == 1u);

  // Finally, clearing PWM_EN = 0 gates pwm_int = 0 and drops sample_pwm0() to
  // 0.
  abs_mmio_write32(kPwmBase + PWM_PWM_EN_REG_OFFSET, 0x0u);
  busy_spin_micros(30);
  CHECK(sample_pwm0() == 0u);

  LOG_INFO(
      "[pwm_core.sv:83-85] confirmed: CNTR_EN=0 with CLK_DIV=%u left "
      "phase_ctr_q "
      "frozen at 0x8000 (pwm_o=%u), and CLK_DIV=0 reset phase_ctr_q to 0x0000 "
      "driving ungated pwm_o=%u while CNTR_EN=0",
      kClkDiv, frozen_out, reset_phase_out);
}

static void verify_pwm_htbt_reload_requires_htbt_en_zero(void) {
  LOG_INFO(
      "Verifying [pwm_chan.sv:144] (TRUE_SILICON_ERRATA): "
      "dc_htbt_q only reloads DUTY_CYCLE.A when HTBT_EN=0 (ignoring "
      "BLINK_EN=0)");

  // Ensure phase_ctr_q == 0x0000, CFG.CNTR_EN == 0, DC_RESN == 0, INVERT == 0.
  abs_mmio_write32(kPwmBase + PWM_PWM_EN_REG_OFFSET, 0x0u);
  abs_mmio_write32(kPwmBase + PWM_CFG_REG_OFFSET, 0x0u);
  busy_spin_micros(30);
  abs_mmio_write32(kPwmBase + PWM_CFG_REG_OFFSET, 0x0u);
  busy_spin_micros(30);

  // Step 1: With HTBT_EN = 0 and BLINK_EN = 0, write DUTY_CYCLE_0 = 0x00000000
  // (A = 0x0000, B = 0x0000) so dc_htbt_q loads 0x0000 via !htbt_en_i.
  abs_mmio_write32(kPwmBase + PWM_PWM_PARAM_0_REG_OFFSET, 0x0u);
  abs_mmio_write32(kPwmBase + PWM_DUTY_CYCLE_0_REG_OFFSET, 0x00000000u);
  abs_mmio_write32(kPwmBase + PWM_PWM_EN_REG_OFFSET, 0x1u);
  busy_spin_micros(30);
  CHECK(sample_pwm0() == 0u);

  // Step 2: Set HTBT_EN = 1 while keeping BLINK_EN = 0 (as instructed by
  // theory_of_operation.md:157-160 & programmers_guide.md:30-32), and update
  // DUTY_CYCLE_0 to A = 0xffff (100% high), B = 0x8000.
  abs_mmio_write32(kPwmBase + PWM_PWM_PARAM_0_REG_OFFSET, 1u << 30);
  busy_spin_micros(30);
  abs_mmio_write32(kPwmBase + PWM_DUTY_CYCLE_0_REG_OFFSET, 0x8000ffffu);
  busy_spin_micros(30);

  // While BLINK_EN = 0, duty_cycle_actual selects duty_cycle_a_i (0xffff),
  // so sample_pwm0() is 1.
  CHECK(sample_pwm0() == 1u);

  // Step 3: Assert BLINK_EN = 1 (with HTBT_EN = 1 and CFG.CNTR_EN = 0).
  // Because pwm_chan.sv:144 only reloads dc_htbt_q when !htbt_en_i, dc_htbt_q
  // retained the stale 0x0000 value instead of reloading DUTY_CYCLE_0.A
  // (0xffff)! Switching duty_cycle_actual to duty_cycle_htbt (dc_htbt_q ==
  // 0x0000) immediately drops sample_pwm0() from 1 to 0!
  abs_mmio_write32(kPwmBase + PWM_PWM_PARAM_0_REG_OFFSET,
                   (1u << 31) | (1u << 30));
  busy_spin_micros(30);
  uint32_t stale_htbt_out = sample_pwm0();
  CHECK(stale_htbt_out == 0u);

  // Step 4 (Workaround): Clear HTBT_EN = 0 (alongside BLINK_EN = 0) for >= 6
  // AON ticks so !htbt_en_i reloads dc_htbt_q <= 0xffff, then re-assert
  // BLINK_EN = 1, HTBT_EN = 1. Now dc_htbt_q == 0xffff and sample_pwm0() is 1!
  abs_mmio_write32(kPwmBase + PWM_PWM_PARAM_0_REG_OFFSET, 0x0u);
  busy_spin_micros(30);
  abs_mmio_write32(kPwmBase + PWM_PWM_PARAM_0_REG_OFFSET,
                   (1u << 31) | (1u << 30));
  busy_spin_micros(30);
  uint32_t reloaded_htbt_out = sample_pwm0();
  CHECK(reloaded_htbt_out == 1u);

  abs_mmio_write32(kPwmBase + PWM_PWM_EN_REG_OFFSET, 0x0u);
  abs_mmio_write32(kPwmBase + PWM_PWM_PARAM_0_REG_OFFSET, 0x0u);
  busy_spin_micros(30);

  LOG_INFO(
      "[pwm_chan.sv:144] confirmed: stale dc_htbt_q drove pwm_o=%u when "
      "BLINK_EN=0->1 with HTBT_EN=1; clearing HTBT_EN=0 reloaded dc_htbt_q "
      "driving pwm_o=%u",
      stale_htbt_out, reloaded_htbt_out);
}

static void verify_pwm_subword_write_permit_faults(void) {
  LOG_INFO(
      "Verifying [pwm_reg_pkg.sv:155-179] (INTENDED_SECURITY_HARDENING): "
      "PWM_PERMIT=4'b1111 rejects 16-bit/8-bit sub-word writes to DUTY_CYCLE, "
      "BLINK_PARAM, PWM_PARAM, and CFG with synchronous Store Access Fault "
      "(mcause=7)");

  g_store_fault_count = 0;
  g_last_mcause = 0;

  // 1. Full 32-bit word write (sw) to DUTY_CYCLE_0, BLINK_PARAM_0, PWM_PARAM_0
  // succeeds.
  abs_mmio_write32(kPwmBase + PWM_DUTY_CYCLE_0_REG_OFFSET, 0x12345678u);
  abs_mmio_write32(kPwmBase + PWM_BLINK_PARAM_0_REG_OFFSET, 0x9abcdef0u);
  abs_mmio_write32(kPwmBase + PWM_PWM_PARAM_0_REG_OFFSET, 0x40001357u);
  CHECK(g_store_fault_count == 0u);
  CHECK(abs_mmio_read32(kPwmBase + PWM_DUTY_CYCLE_0_REG_OFFSET) == 0x12345678u);
  CHECK(abs_mmio_read32(kPwmBase + PWM_BLINK_PARAM_0_REG_OFFSET) ==
        0x9abcdef0u);
  CHECK(abs_mmio_read32(kPwmBase + PWM_PWM_PARAM_0_REG_OFFSET) == 0x40001357u);

  // 2. 16-bit half-word writes (sh) to lower or upper half of DUTY_CYCLE_0,
  // BLINK_PARAM_0, PWM_PARAM_0, and CFG fault with mcause = 7 and do not modify
  // CSRs.
  mmio_write16(kPwmBase + PWM_DUTY_CYCLE_0_REG_OFFSET, 0xaaaau);
  CHECK(g_store_fault_count == 1u);
  CHECK(g_last_mcause == (uint32_t)kIbexExcStoreAccessFault);
  CHECK(abs_mmio_read32(kPwmBase + PWM_DUTY_CYCLE_0_REG_OFFSET) == 0x12345678u);

  mmio_write16(kPwmBase + PWM_DUTY_CYCLE_0_REG_OFFSET + 2u, 0xbbbbu);
  CHECK(g_store_fault_count == 2u);
  CHECK(abs_mmio_read32(kPwmBase + PWM_DUTY_CYCLE_0_REG_OFFSET) == 0x12345678u);

  mmio_write16(kPwmBase + PWM_BLINK_PARAM_0_REG_OFFSET, 0xccccu);
  CHECK(g_store_fault_count == 3u);
  CHECK(abs_mmio_read32(kPwmBase + PWM_BLINK_PARAM_0_REG_OFFSET) ==
        0x9abcdef0u);

  mmio_write16(kPwmBase + PWM_PWM_PARAM_0_REG_OFFSET, 0xddddu);
  CHECK(g_store_fault_count == 4u);
  CHECK(abs_mmio_read32(kPwmBase + PWM_PWM_PARAM_0_REG_OFFSET) == 0x40001357u);

  mmio_write16(kPwmBase + PWM_CFG_REG_OFFSET, 0x0010u);
  CHECK(g_store_fault_count == 5u);

  // 3. 1-byte write (sb) to byte 0 of INVERT (PWM_PERMIT = 4'b0001) succeeds,
  // whereas 1-byte write to byte 1 of INVERT faults with mcause = 7.
  abs_mmio_write8(kPwmBase + PWM_INVERT_REG_OFFSET, 0x15u);
  CHECK(g_store_fault_count == 5u);
  CHECK(abs_mmio_read32(kPwmBase + PWM_INVERT_REG_OFFSET) == 0x15u);

  abs_mmio_write8(kPwmBase + PWM_INVERT_REG_OFFSET + 1u, 0x01u);
  CHECK(g_store_fault_count == 6u);
  CHECK(abs_mmio_read32(kPwmBase + PWM_INVERT_REG_OFFSET) == 0x15u);

  abs_mmio_write8(kPwmBase + PWM_INVERT_REG_OFFSET, 0x00u);
  LOG_INFO(
      "[pwm_reg_pkg.sv:155-179] confirmed: all %u disallowed sub-word writes "
      "raised "
      "synchronous Store Access Fault (mcause=7) without mutating CSRs",
      g_store_fault_count);
}

bool test_main(void) {
  // Route PWM0 output to MIO pad IOA0 and IOA0 input to GPIO0.
  abs_mmio_write32(kPinmuxBase + PINMUX_MIO_OUTSEL_0_REG_OFFSET,
                   kTopEarlgreyPinmuxOutselPwmAonPwm0);
  abs_mmio_write32(kPinmuxBase + PINMUX_MIO_PERIPH_INSEL_0_REG_OFFSET,
                   kTopEarlgreyPinmuxInselIoa0);

  verify_pwm_invert_when_disabled();
  verify_pwm_phase_ctr_clear_and_ungated_output();
  verify_pwm_htbt_reload_requires_htbt_en_zero();
  verify_pwm_subword_write_permit_faults();

  LOG_INFO("All pwm errata checks ([pwm_core.sv:83-85..004]) confirmed!");
  return true;
}
