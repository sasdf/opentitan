// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file i2c_errata_v2_test.c
 * @brief CW340 FPGA Errata Verification & Discovery Test for Earlgrey v2 `i2c`
 * (`trunk-v2`).
 *
 * Empirically verifies on physical CW340 FPGA silicon (`trunk-v2`):
 *
 * 1. `[i2c.hjson:639-752, dif_i2c.c:212-290]` (`DIF_API_BUG` &
 *    `SPEC_DOC_ERRATA` — v2 Discovery):
 *    - In `trunk-v2`, `TIMING0..4` (`0x3c..0x4c`) were narrowed from 16-bit
 *      fields (`0xffff`) to asymmetric 9-bit, 10-bit, and 13-bit fields
 *      (`TIMING0` `0x1fff1fff`, `TIMING1` `0x01ff03ff`, `TIMING2` `0x1fff1fff`,
 *      `TIMING3` `0x1fff01ff`, `TIMING4` `0x1fff1fff`).
 *    - `dif_i2c_compute_timing()` (`dif_i2c.c:230-235`) still checks
 *      `lengthened_high_cycles > UINT16_MAX` (`0xffff`) instead of
 *      `I2C_TIMING0_THIGH_MASK` (`0x1fff`), returning `kDifOk` with
 *      `scl_time_high_cycles > 0x1fff` (`0x26d5`), and `dif_i2c_configure()`
 *      silently truncates out-of-range 16-bit timing fields to 9/10/13 bits.
 * 2. `[i2c_core.sv:215, 223-224, 330, 462-498, i2c.hjson:118-126, 603-605]`
 *    (`SPEC_DOC_ERRATA` — v2 Discovery):
 *    - `i2c.hjson:603-605` claims `OVRD` (`0x34`) triggers continuous
 *      `sda/scl_interference` interrupts during CSR writes (`CTRL == 0`), yet
 *      `controller_sda_interference = controller_transmitting &&
 *      sda_released_but_low` (`i2c_core.sv:495`) stays `0` whenever
 *      `CTRL.ENABLEHOST == 0`.
 *    - Conversely, because `OVRD` muxes `sda_o` downstream of `sda_fsm_q`
 *      without gating `u_i2c_controller_fsm`, driving `OVRD` (`TXOVRDEN = 1,
 *      SCLVAL = 1, SDAVAL = 0`) while `CTRL.ENABLEHOST == 1` and `FMT_FIFO` is
 *      active causes `sda_fsm_q (1) != sda_sync (0)` while `scl_sync == 1`,
 *      immediately firing `INTR_STATE.SDA_INTERFERENCE = 1`,
 *      `CONTROLLER_EVENTS.ARBITRATION_LOST = 1`, and
 *      `INTR_STATE.CONTROLLER_HALT = 1`.
 * 3. `[i2c_core.sv:337-342, 428, i2c_controller_fsm.sv:617-650]`
 *    (`SPEC_DOC_ERRATA`):
 *    - `FDATA` (`fmt_fifo`) and `TXDATA` (`tx_fifo`) accept preloaded entries
 *      while `CTRL.ENABLEHOST == 0` / `CTRL.ENABLETARGET == 0`,
 *      `STATUS.HOSTIDLE` stays `1` while `CTRL.ENABLEHOST == 0` even with
 *      `FMTLVL > 0`, and `VAL` (`0x38`) shifts in `0xffffffff` in 16 idle
 *      cycles and reflects `OVRD` (`0x34`).
 * 4. `[i2c_controller_fsm.sv:500-525, 843-848, i2c_target_fsm.sv:323-328]`
 *    (`SPEC_DOC_ERRATA`):
 *    - `FDATA` with `READB = 1`, `RCONT = 1`, and `STOP = 1` executes `ACK` +
 *      `STOP` and triggers `INTR_STATE.UNEXP_STOP = 1` on the Target FSM
 *      (contrary to `theory_of_operation.md:75`).
 *    - `TARGET_NACK_COUNT` (`0x68`, `SwAccessRC`) increments on the rising
 *      edge `!nack_transaction_q && nack_transaction_d` (`+1` per NACKed
 *      transaction even when `ACQDATA` records both `SIGNAL_NACK` and
 *      `SIGNAL_NACK_STOP`), clears to `0` on read, and ignores writes.
 * 5. `[i2c_reg_pkg.sv:548-581]` (`INTENDED_SECURITY_HARDENING`):
 *    - `I2C_PERMIT[32]` enforces 1/2/3/4-byte write masks (`4'b0001`,
 *      `4'b0011`, `4'b0111`, `4'b1111`), trapping narrower sub-word stores
 *      with `mcause = 7` while permitting sub-word reads (`lb`/`lh`).
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_base.h"
#include "sw/device/lib/dif/dif_i2c.h"
#include "sw/device/lib/dif/dif_pinmux.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top/i2c_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

#define CHECK_EQ(a, b, ...) CHECK((a) == (b), __VA_ARGS__)

enum {
  kI2c0Base = TOP_EARLGREY_I2C0_BASE_ADDR,
  kPinmuxBase = TOP_EARLGREY_PINMUX_BASE_ADDR,
  kRiscvLoadAccessFault = 5,
  kRiscvStoreAccessFault = 7,
};

static volatile uint32_t g_fault_count = 0;
static volatile uint32_t g_last_mcause = 0;

void ottf_exception_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = 0;
  uint32_t mepc = 0;
  CSR_READ(CSR_REG_MCAUSE, &mcause);
  CSR_READ(CSR_REG_MEPC, &mepc);
  if (mcause == kRiscvLoadAccessFault || mcause == kRiscvStoreAccessFault) {
    g_fault_count++;
    g_last_mcause = mcause;
    uint16_t insn16 = *(const volatile uint16_t *)mepc;
    uint32_t step = ((insn16 & 0x3u) != 0x3u) ? 2u : 4u;
    CSR_WRITE(CSR_REG_MEPC, mepc + step);
    return;
  }
  ottf_generic_fault_print(exc_info, "Unhandled exception", mcause);
  abort();
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

static void test_i2c_v2_timing_width_and_dif_truncation(void) {
  LOG_INFO(
      "Verifying [i2c.hjson:639-752, dif_i2c.c:212-290] (v2 Discovery): "
      "Narrowed 9/10/13-bit TIMING0..4 CSRs vs dif_i2c_compute_timing / "
      "dif_i2c_configure 16-bit truncation...");

  dif_i2c_t i2c;
  CHECK_DIF_OK(dif_i2c_init(mmio_region_from_addr(kI2c0Base), &i2c));

  // 1. Verify hardware writable masks of TIMING0..4 (`0x3c..0x4c`) on trunk-v2:
  abs_mmio_write32(kI2c0Base + I2C_TIMING0_REG_OFFSET, 0xFFFFFFFFu);
  abs_mmio_write32(kI2c0Base + I2C_TIMING1_REG_OFFSET, 0xFFFFFFFFu);
  abs_mmio_write32(kI2c0Base + I2C_TIMING2_REG_OFFSET, 0xFFFFFFFFu);
  abs_mmio_write32(kI2c0Base + I2C_TIMING3_REG_OFFSET, 0xFFFFFFFFu);
  abs_mmio_write32(kI2c0Base + I2C_TIMING4_REG_OFFSET, 0xFFFFFFFFu);

  CHECK_EQ(abs_mmio_read32(kI2c0Base + I2C_TIMING0_REG_OFFSET), 0x1FFF1FFFu,
           "[i2c.hjson:639-652] Expected TIMING0 13-bit mask 0x1fff1fff");
  CHECK_EQ(abs_mmio_read32(kI2c0Base + I2C_TIMING1_REG_OFFSET), 0x01FF03FFu,
           "[i2c.hjson:663-676] Expected TIMING1 9/10-bit mask 0x01ff03ff");
  CHECK_EQ(abs_mmio_read32(kI2c0Base + I2C_TIMING2_REG_OFFSET), 0x1FFF1FFFu,
           "[i2c.hjson:687-700] Expected TIMING2 13-bit mask 0x1fff1fff");
  CHECK_EQ(abs_mmio_read32(kI2c0Base + I2C_TIMING3_REG_OFFSET), 0x1FFF01FFu,
           "[i2c.hjson:711-727] Expected TIMING3 13/9-bit mask 0x1fff01ff");
  CHECK_EQ(abs_mmio_read32(kI2c0Base + I2C_TIMING4_REG_OFFSET), 0x1FFF1FFFu,
           "[i2c.hjson:738-751] Expected TIMING4 13-bit mask 0x1fff1fff");

  // 2. Verify `dif_i2c_compute_timing()` checks `UINT16_MAX` (`0xffff`) instead
  // of `I2C_TIMING0_THIGH_MASK` (`0x1fff`), returning `kDifOk` with
  // `scl_time_high_cycles > 0x1fff`, which `dif_i2c_configure()` silently
  // truncates to 13 bits (`& 0x1fff`):
  dif_i2c_timing_config_t timing_cfg = {
      .lowest_target_device_speed = kDifI2cSpeedStandard,
      .clock_period_nanos = 100,
      .sda_rise_nanos = 110000,     // 1100 cycles (> 10-bit T_R max 1023)
      .sda_fall_nanos = 60000,      // 600 cycles (> 9-bit T_F max 511)
      .scl_period_nanos = 1200000,  // 12000 cycles -> lengthened THIGH > 8191
  };
  dif_i2c_config_t computed_cfg;
  CHECK_DIF_OK(dif_i2c_compute_timing(timing_cfg, &computed_cfg));
  CHECK(computed_cfg.scl_time_high_cycles > I2C_TIMING0_THIGH_MASK,
        "[dif_i2c.c:230-235] Expected dif_i2c_compute_timing to produce "
        "scl_time_high_cycles (0x%x) > 0x1fff without returning kDifOutOfRange",
        computed_cfg.scl_time_high_cycles);
  CHECK(computed_cfg.rise_cycles > I2C_TIMING1_T_R_MASK &&
            computed_cfg.fall_cycles > I2C_TIMING1_T_F_MASK,
        "[dif_i2c.c:212-215] Expected rise_cycles (0x%x) > 0x3ff and "
        "fall_cycles (0x%x) > 0x1ff",
        computed_cfg.rise_cycles, computed_cfg.fall_cycles);

  CHECK_DIF_OK(dif_i2c_configure(&i2c, computed_cfg));
  uint32_t timing0_hw = abs_mmio_read32(kI2c0Base + I2C_TIMING0_REG_OFFSET);
  uint32_t timing1_hw = abs_mmio_read32(kI2c0Base + I2C_TIMING1_REG_OFFSET);
  CHECK_EQ(
      bitfield_field32_read(timing0_hw, I2C_TIMING0_THIGH_FIELD),
      (uint32_t)(computed_cfg.scl_time_high_cycles & I2C_TIMING0_THIGH_MASK),
      "[dif_i2c.c:255] Expected dif_i2c_configure to silently truncate "
      "16-bit scl_time_high_cycles to 13 bits");
  CHECK_EQ(bitfield_field32_read(timing1_hw, I2C_TIMING1_T_R_FIELD),
           (uint32_t)(computed_cfg.rise_cycles & I2C_TIMING1_T_R_MASK),
           "[dif_i2c.c:262] Expected dif_i2c_configure to silently truncate "
           "16-bit rise_cycles to 10 bits");
  CHECK_EQ(bitfield_field32_read(timing1_hw, I2C_TIMING1_T_F_FIELD),
           (uint32_t)(computed_cfg.fall_cycles & I2C_TIMING1_T_F_MASK),
           "[dif_i2c.c:264] Expected dif_i2c_configure to silently truncate "
           "16-bit fall_cycles to 9 bits");
}

static void test_i2c_fifo_preload_hostidle_val_and_ovrd_interference(void) {
  LOG_INFO(
      "Verifying [i2c_core.sv:337-342, 462-498] & [i2c.hjson:603-605]: "
      "FDATA/TXDATA preload when disabled, STATUS.HOSTIDLE=1, VAL/OVRD, & "
      "OVRD sda_interference / arbitration_lost when ENABLEHOST=1 vs 0...");

  abs_mmio_write32(kI2c0Base + I2C_CTRL_REG_OFFSET, 0u);
  uint32_t fifo_rst =
      (1u << I2C_FIFO_CTRL_RXRST_BIT) | (1u << I2C_FIFO_CTRL_FMTRST_BIT) |
      (1u << I2C_FIFO_CTRL_ACQRST_BIT) | (1u << I2C_FIFO_CTRL_TXRST_BIT);
  abs_mmio_write32(kI2c0Base + I2C_FIFO_CTRL_REG_OFFSET, fifo_rst);

  // 1. Pre-load 2 FDATA words and 2 TXDATA words while CTRL == 0.
  abs_mmio_write32(kI2c0Base + I2C_FDATA_REG_OFFSET, 0x11u);
  abs_mmio_write32(kI2c0Base + I2C_FDATA_REG_OFFSET, 0x22u);
  abs_mmio_write32(kI2c0Base + I2C_TXDATA_REG_OFFSET, 0xAAu);
  abs_mmio_write32(kI2c0Base + I2C_TXDATA_REG_OFFSET, 0xBBu);

  uint32_t h_status =
      abs_mmio_read32(kI2c0Base + I2C_HOST_FIFO_STATUS_REG_OFFSET);
  uint32_t t_status =
      abs_mmio_read32(kI2c0Base + I2C_TARGET_FIFO_STATUS_REG_OFFSET);
  uint32_t status = abs_mmio_read32(kI2c0Base + I2C_STATUS_REG_OFFSET);

  CHECK_EQ(bitfield_field32_read(h_status, I2C_HOST_FIFO_STATUS_FMTLVL_FIELD),
           2u,
           "[i2c_core.sv:337-342] Expected FMTLVL == 2 when ENABLEHOST == 0");
  CHECK_EQ(bitfield_field32_read(t_status, I2C_TARGET_FIFO_STATUS_TXLVL_FIELD),
           2u, "[i2c_core.sv:428] Expected TXLVL == 2 when ENABLETARGET == 0");
  CHECK(!bitfield_bit32_read(status, I2C_STATUS_FMTEMPTY_BIT),
        "Expected STATUS.FMTEMPTY == 0");
  CHECK(bitfield_bit32_read(status, I2C_STATUS_HOSTIDLE_BIT),
        "[i2c_controller_fsm.sv:617-650] Expected STATUS.HOSTIDLE == 1 when "
        "ENABLEHOST == 0 even with FMTLVL == 2");

  // Flush FIFOs.
  abs_mmio_write32(kI2c0Base + I2C_FIFO_CTRL_REG_OFFSET, fifo_rst);

  // 2. Verify VAL (0x38) shifts in 0xffffffff on idle bus and reflects OVRD
  // (0x34).
  dif_pinmux_t pinmux;
  CHECK_DIF_OK(dif_pinmux_init(mmio_region_from_addr(kPinmuxBase), &pinmux));
  CHECK_DIF_OK(dif_pinmux_input_select(&pinmux,
                                       kTopEarlgreyPinmuxPeripheralInI2c0Sda,
                                       kTopEarlgreyPinmuxInselIoa7));
  CHECK_DIF_OK(dif_pinmux_input_select(&pinmux,
                                       kTopEarlgreyPinmuxPeripheralInI2c0Scl,
                                       kTopEarlgreyPinmuxInselIoa8));

  abs_mmio_write32(kI2c0Base + I2C_OVRD_REG_OFFSET, 0u);
  precharge_i2c0_pads_high(&pinmux);
  CHECK_EQ(
      abs_mmio_read32(kI2c0Base + I2C_VAL_REG_OFFSET), 0xFFFFFFFFu,
      "[i2c_core.sv:283-284] Expected VAL == 0xffffffff when SCL=1, SDA=1");

  // Override SCL=0, SDA=1 (TXOVRDEN=1, SCLVAL=0, SDAVAL=1 -> 0x5) while
  // CTRL == 0:
  abs_mmio_write32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET, 0xFFFFFFFFu);
  abs_mmio_write32(kI2c0Base + I2C_OVRD_REG_OFFSET,
                   (1u << I2C_OVRD_TXOVRDEN_BIT) | (1u << I2C_OVRD_SDAVAL_BIT));
  busy_spin_micros(5);
  CHECK_EQ(
      abs_mmio_read32(kI2c0Base + I2C_VAL_REG_OFFSET), 0xFFFF0000u,
      "[i2c_core.sv:283-284] Expected VAL == 0xffff0000 when SCL=0, SDA=1");

  // Clean up OVRD and precharge pads back high before switching to Ior0/Ior1.
  abs_mmio_write32(kI2c0Base + I2C_OVRD_REG_OFFSET, 0u);
  abs_mmio_write32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET, 0xFFFFFFFFu);
  precharge_i2c0_pads_high(&pinmux);
}

static void test_i2c_ovrd_interference_when_enabled(void) {
  LOG_INFO(
      "Verifying [i2c_core.sv:495] & [i2c_controller_fsm.sv:278]: "
      "OVRD (TXOVRDEN=1, SCLVAL=1, SDAVAL=0) when CTRL=0 vs CTRL.ENABLEHOST=1 "
      "(SDA_INTERFERENCE, ARBITRATION_LOST, and CONTROLLER_HALT)...");

  dif_pinmux_t pinmux;
  CHECK_DIF_OK(dif_pinmux_init(mmio_region_from_addr(kPinmuxBase), &pinmux));
  CHECK_DIF_OK(dif_pinmux_input_select(&pinmux,
                                       kTopEarlgreyPinmuxPeripheralInI2c0Sda,
                                       kTopEarlgreyPinmuxInselIoa7));
  CHECK_DIF_OK(dif_pinmux_input_select(&pinmux,
                                       kTopEarlgreyPinmuxPeripheralInI2c0Scl,
                                       kTopEarlgreyPinmuxInselIoa8));
  precharge_i2c0_pads_high(&pinmux);

  uint32_t fifo_rst =
      (1u << I2C_FIFO_CTRL_RXRST_BIT) | (1u << I2C_FIFO_CTRL_FMTRST_BIT) |
      (1u << I2C_FIFO_CTRL_ACQRST_BIT) | (1u << I2C_FIFO_CTRL_TXRST_BIT);
  abs_mmio_write32(kI2c0Base + I2C_FIFO_CTRL_REG_OFFSET, fifo_rst);

  // 1. First override SCL=1, SDA=0 (TXOVRDEN=1, SCLVAL=1, SDAVAL=0 -> 0x3)
  // while CTRL == 0: verify that contrary to i2c.hjson:603-605, neither
  // SDA_INTERFERENCE nor SCL_INTERFERENCE fires when CTRL.ENABLEHOST == 0!
  abs_mmio_write32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET, 0xFFFFFFFFu);
  abs_mmio_write32(kI2c0Base + I2C_OVRD_REG_OFFSET,
                   (1u << I2C_OVRD_TXOVRDEN_BIT) | (1u << I2C_OVRD_SCLVAL_BIT));
  busy_spin_micros(5);
  uint32_t intr_when_disabled =
      abs_mmio_read32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET);
  CHECK(!bitfield_bit32_read(intr_when_disabled,
                             I2C_INTR_STATE_SDA_INTERFERENCE_BIT) &&
            !bitfield_bit32_read(intr_when_disabled,
                                 I2C_INTR_STATE_SCL_INTERFERENCE_BIT),
        "[i2c_core.sv:495] Expected zero sda/scl_interference when "
        "CTRL.ENABLEHOST == 0");

  // 2. Generate a STOP (SDA 0 -> 1 while SCL=1) so i2c_bus_monitor sees
  // bus_free = 1 before we enable CTRL.ENABLEHOST = 1!
  abs_mmio_write32(kI2c0Base + I2C_OVRD_REG_OFFSET, 0u);
  precharge_i2c0_pads_high(&pinmux);
  abs_mmio_write32(
      kI2c0Base + I2C_TIMING4_REG_OFFSET,
      (4u << I2C_TIMING4_TSU_STO_OFFSET) | (4u << I2C_TIMING4_T_BUF_OFFSET));
  busy_spin_micros(5);

  // 3. Now enable CTRL.ENABLEHOST = 1 and hold OVRD with TXOVRDEN = 1,
  // SCLVAL = 1, SDAVAL = 0 (`sda_o = 0` while `sda_fsm_q = 1` in SetupStart)
  // with a START + address byte in FDATA:
  abs_mmio_write32(kI2c0Base + I2C_OVRD_REG_OFFSET,
                   (1u << I2C_OVRD_TXOVRDEN_BIT) | (1u << I2C_OVRD_SCLVAL_BIT));
  abs_mmio_write32(
      kI2c0Base + I2C_TIMING0_REG_OFFSET,
      (10u << I2C_TIMING0_THIGH_OFFSET) | (10u << I2C_TIMING0_TLOW_OFFSET));
  abs_mmio_write32(
      kI2c0Base + I2C_TIMING1_REG_OFFSET,
      (4u << I2C_TIMING1_T_R_OFFSET) | (4u << I2C_TIMING1_T_F_OFFSET));
  abs_mmio_write32(kI2c0Base + I2C_TIMING2_REG_OFFSET,
                   (10u << I2C_TIMING2_TSU_STA_OFFSET) |
                       (10u << I2C_TIMING2_THD_STA_OFFSET));
  abs_mmio_write32(
      kI2c0Base + I2C_TIMING3_REG_OFFSET,
      (4u << I2C_TIMING3_TSU_DAT_OFFSET) | (4u << I2C_TIMING3_THD_DAT_OFFSET));
  abs_mmio_write32(
      kI2c0Base + I2C_TIMING4_REG_OFFSET,
      (10u << I2C_TIMING4_TSU_STO_OFFSET) | (10u << I2C_TIMING4_T_BUF_OFFSET));
  abs_mmio_write32(kI2c0Base + I2C_CONTROLLER_EVENTS_REG_OFFSET, 0xFFFFFFFFu);
  abs_mmio_write32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET, 0xFFFFFFFFu);
  abs_mmio_write32(kI2c0Base + I2C_CTRL_REG_OFFSET,
                   1u << I2C_CTRL_ENABLEHOST_BIT);
  abs_mmio_write32(kI2c0Base + I2C_FDATA_REG_OFFSET,
                   (1u << I2C_FDATA_START_BIT) | 0xAAu);
  busy_spin_micros(20);

  uint32_t intr_ovrd_host =
      abs_mmio_read32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET);
  uint32_t ctrl_events =
      abs_mmio_read32(kI2c0Base + I2C_CONTROLLER_EVENTS_REG_OFFSET);
  CHECK(
      bitfield_bit32_read(intr_ovrd_host, I2C_INTR_STATE_SDA_INTERFERENCE_BIT),
      "[i2c_core.sv:495] Expected OVRD (SCLVAL=1, SDAVAL=0) during active "
      "ENABLEHOST=1 to fire INTR_STATE.SDA_INTERFERENCE");
  CHECK(
      bitfield_bit32_read(ctrl_events,
                          I2C_CONTROLLER_EVENTS_ARBITRATION_LOST_BIT),
      "[i2c_controller_fsm.sv:278] Expected CONTROLLER_EVENTS.ARBITRATION_LOST "
      "== 1 when OVRD pulls SDA low against sda_fsm_q=1");
  CHECK(bitfield_bit32_read(intr_ovrd_host, I2C_INTR_STATE_CONTROLLER_HALT_BIT),
        "Expected INTR_STATE.CONTROLLER_HALT == 1 on arbitration lost");

  // Clean up OVRD, CONTROLLER_EVENTS, CTRL, and FIFOs.
  abs_mmio_write32(kI2c0Base + I2C_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kI2c0Base + I2C_OVRD_REG_OFFSET, 0u);
  abs_mmio_write32(kI2c0Base + I2C_FIFO_CTRL_REG_OFFSET, fifo_rst);
  abs_mmio_write32(kI2c0Base + I2C_CONTROLLER_EVENTS_REG_OFFSET, 0xFFFFFFFFu);
  abs_mmio_write32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET, 0xFFFFFFFFu);
  precharge_i2c0_pads_high(&pinmux);
}

static void test_i2c_readb_rcont_stop_and_nack_count(void) {
  LOG_INFO(
      "Verifying [i2c_controller_fsm.sv:500-525] & "
      "[i2c_target_fsm.sv:326-328]: FDATA READB+RCONT+STOP "
      "-> UNEXP_STOP & TARGET_NACK_COUNT single increment per transaction...");

  dif_pinmux_t pinmux;
  CHECK_DIF_OK(dif_pinmux_init(mmio_region_from_addr(kPinmuxBase), &pinmux));
  CHECK_DIF_OK(dif_pinmux_output_select(&pinmux, kTopEarlgreyPinmuxMioOutIoa7,
                                        kTopEarlgreyPinmuxOutselI2c0Sda));
  CHECK_DIF_OK(dif_pinmux_input_select(&pinmux,
                                       kTopEarlgreyPinmuxPeripheralInI2c0Sda,
                                       kTopEarlgreyPinmuxInselIoa7));
  CHECK_DIF_OK(dif_pinmux_output_select(&pinmux, kTopEarlgreyPinmuxMioOutIoa8,
                                        kTopEarlgreyPinmuxOutselI2c0Scl));
  CHECK_DIF_OK(dif_pinmux_input_select(&pinmux,
                                       kTopEarlgreyPinmuxPeripheralInI2c0Scl,
                                       kTopEarlgreyPinmuxInselIoa8));
  precharge_i2c0_pads_high(&pinmux);

  abs_mmio_write32(
      kI2c0Base + I2C_TIMING0_REG_OFFSET,
      (60u << I2C_TIMING0_THIGH_OFFSET) | (60u << I2C_TIMING0_TLOW_OFFSET));
  abs_mmio_write32(
      kI2c0Base + I2C_TIMING1_REG_OFFSET,
      (20u << I2C_TIMING1_T_R_OFFSET) | (4u << I2C_TIMING1_T_F_OFFSET));
  abs_mmio_write32(kI2c0Base + I2C_TIMING2_REG_OFFSET,
                   (60u << I2C_TIMING2_TSU_STA_OFFSET) |
                       (60u << I2C_TIMING2_THD_STA_OFFSET));
  abs_mmio_write32(kI2c0Base + I2C_TIMING3_REG_OFFSET,
                   (10u << I2C_TIMING3_TSU_DAT_OFFSET) |
                       (10u << I2C_TIMING3_THD_DAT_OFFSET));
  abs_mmio_write32(
      kI2c0Base + I2C_TIMING4_REG_OFFSET,
      (60u << I2C_TIMING4_TSU_STO_OFFSET) | (60u << I2C_TIMING4_T_BUF_OFFSET));
  abs_mmio_write32(kI2c0Base + I2C_TARGET_ID_REG_OFFSET,
                   (0x7fu << I2C_TARGET_ID_MASK0_OFFSET) |
                       (0x33u << I2C_TARGET_ID_ADDRESS0_OFFSET));

  uint32_t fifo_rst =
      (1u << I2C_FIFO_CTRL_RXRST_BIT) | (1u << I2C_FIFO_CTRL_FMTRST_BIT) |
      (1u << I2C_FIFO_CTRL_ACQRST_BIT) | (1u << I2C_FIFO_CTRL_TXRST_BIT);
  abs_mmio_write32(kI2c0Base + I2C_FIFO_CTRL_REG_OFFSET, fifo_rst);
  abs_mmio_write32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET, 0xFFFFFFFFu);

  abs_mmio_write32(
      kI2c0Base + I2C_CTRL_REG_OFFSET,
      (1u << I2C_CTRL_ENABLEHOST_BIT) | (1u << I2C_CTRL_ENABLETARGET_BIT));

  // Preload 2 bytes into TXDATA: 0xA5 (read by host) followed by 0xFF (MSB=1).
  abs_mmio_write32(kI2c0Base + I2C_TXDATA_REG_OFFSET, 0xA5u);
  abs_mmio_write32(kI2c0Base + I2C_TXDATA_REG_OFFSET, 0xFFu);

  // Issue START + target read address (0x33 << 1 | 1), followed by
  // FDATA with READB = 1, RCONT = 1, STOP = 1, FBYTE = 1:
  abs_mmio_write32(kI2c0Base + I2C_FDATA_REG_OFFSET,
                   (1u << I2C_FDATA_START_BIT) | (0x33u << 1) | 1u);
  abs_mmio_write32(kI2c0Base + I2C_FDATA_REG_OFFSET,
                   (1u << I2C_FDATA_READB_BIT) | (1u << I2C_FDATA_RCONT_BIT) |
                       (1u << I2C_FDATA_STOP_BIT) | 1u);

  const uint32_t kDoneMask = (1u << I2C_STATUS_HOSTIDLE_BIT) |
                             (1u << I2C_STATUS_FMTEMPTY_BIT) |
                             (1u << I2C_STATUS_TARGETIDLE_BIT);
  for (int i = 0; i < 5000; ++i) {
    uint32_t st = abs_mmio_read32(kI2c0Base + I2C_STATUS_REG_OFFSET);
    if ((st & kDoneMask) == kDoneMask) {
      break;
    }
    busy_spin_micros(2);
  }

  uint32_t intr = abs_mmio_read32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET);
  CHECK(bitfield_bit32_read(intr, I2C_INTR_STATE_CMD_COMPLETE_BIT),
        "[i2c_controller_fsm.sv:500-525] Expected CMD_COMPLETE after READB + "
        "RCONT + STOP");
  CHECK(bitfield_bit32_read(intr, I2C_INTR_STATE_UNEXP_STOP_BIT),
        "[i2c_controller_fsm.sv:500-525] Expected INTR_STATE.UNEXP_STOP == 1 "
        "after READB + RCONT + STOP");
  CHECK_EQ(abs_mmio_read32(kI2c0Base + I2C_RDATA_REG_OFFSET) & 0xFFu, 0xA5u,
           "Expected RDATA == 0xA5");

  // Verify [i2c_target_fsm.sv:326-328]: TARGET_NACK_COUNT (0x68) is
  // SwAccessRC (ignores writes), increments by +1 (not +2) when a NACKed
  // target transaction records both AcqNack and AcqNackStop in ACQDATA, and
  // clears to 0 on read.
  abs_mmio_write32(kI2c0Base + I2C_TARGET_NACK_COUNT_REG_OFFSET, 0xFFFFFFFFu);
  CHECK_EQ(
      abs_mmio_read32(kI2c0Base + I2C_TARGET_NACK_COUNT_REG_OFFSET), 0u,
      "[i2c_target_fsm.sv:326-328] Expected TARGET_NACK_COUNT == 0 after write "
      "(SwAccessRC)");

  // Reset FIFOs, enable ACK_CTRL_EN = 1 with NBYTES = 0 and TARGET_TIMEOUT_CTRL
  // (EN = 1, VAL = 20) so the target NACKs data byte 0x55 in StretchAcqFull,
  // and issue START + write address (0x33 << 1 | 0) followed by NAKOK | STOP |
  // 0x55.
  abs_mmio_write32(kI2c0Base + I2C_FIFO_CTRL_REG_OFFSET, fifo_rst);
  abs_mmio_write32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET, 0xFFFFFFFFu);
  abs_mmio_write32(kI2c0Base + I2C_TARGET_TIMEOUT_CTRL_REG_OFFSET,
                   (1u << I2C_TARGET_TIMEOUT_CTRL_EN_BIT) | 20u);
  abs_mmio_write32(kI2c0Base + I2C_CTRL_REG_OFFSET,
                   (1u << I2C_CTRL_ENABLEHOST_BIT) |
                       (1u << I2C_CTRL_ENABLETARGET_BIT) |
                       (1u << I2C_CTRL_ACK_CTRL_EN_BIT));
  abs_mmio_write32(kI2c0Base + I2C_FDATA_REG_OFFSET,
                   (1u << I2C_FDATA_START_BIT) | (0x33u << 1));
  abs_mmio_write32(
      kI2c0Base + I2C_FDATA_REG_OFFSET,
      (1u << I2C_FDATA_NAKOK_BIT) | (1u << I2C_FDATA_STOP_BIT) | 0x55u);
  for (int i = 0; i < 5000; ++i) {
    uint32_t st = abs_mmio_read32(kI2c0Base + I2C_STATUS_REG_OFFSET);
    if ((st & kDoneMask) == kDoneMask) {
      break;
    }
    busy_spin_micros(2);
  }
  uint32_t nack_cnt_first_read =
      abs_mmio_read32(kI2c0Base + I2C_TARGET_NACK_COUNT_REG_OFFSET);
  uint32_t nack_cnt_second_read =
      abs_mmio_read32(kI2c0Base + I2C_TARGET_NACK_COUNT_REG_OFFSET);
  CHECK_EQ(nack_cnt_first_read, 1u,
           "[i2c_target_fsm.sv:327] Expected TARGET_NACK_COUNT == 1 after "
           "single NACKed target transaction (AcqNack + AcqNackStop)");
  CHECK_EQ(nack_cnt_second_read, 0u,
           "[i2c_target_fsm.sv:327] Expected TARGET_NACK_COUNT to clear to 0 "
           "on read (SwAccessRC)");
  abs_mmio_write32(kI2c0Base + I2C_TARGET_TIMEOUT_CTRL_REG_OFFSET, 0u);

  abs_mmio_write32(kI2c0Base + I2C_HOST_TIMEOUT_CTRL_REG_OFFSET, 0xFFFFFFFFu);
  CHECK_EQ(abs_mmio_read32(kI2c0Base + I2C_HOST_TIMEOUT_CTRL_REG_OFFSET),
           0x000FFFFFu,
           "Expected HOST_TIMEOUT_CTRL == 0x000fffff (20-bit mask)");
  abs_mmio_write32(kI2c0Base + I2C_HOST_TIMEOUT_CTRL_REG_OFFSET, 0u);

  abs_mmio_write32(kI2c0Base + I2C_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kI2c0Base + I2C_FIFO_CTRL_REG_OFFSET, fifo_rst);
  abs_mmio_write32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET, 0xFFFFFFFFu);
}

static void test_i2c_permit_subword_writes_vs_reads(void) {
  LOG_INFO(
      "Verifying [i2c_reg_pkg.sv:548-581]: I2C_PERMIT[32] sub-word write "
      "faults vs sub-word reads...");

  abs_mmio_write32(kI2c0Base + I2C_HOST_FIFO_CONFIG_REG_OFFSET, 0x01020304u);
  g_fault_count = 0;
  uint8_t b0 = abs_mmio_read8(kI2c0Base + I2C_HOST_FIFO_CONFIG_REG_OFFSET);
  uint16_t h0 =
      *(const volatile uint16_t *)(uintptr_t)(kI2c0Base +
                                              I2C_HOST_FIFO_CONFIG_REG_OFFSET);
  CHECK_EQ(g_fault_count, 0u,
           "[i2c_reg_pkg.sv:548-581] Sub-word reads from HOST_FIFO_CONFIG must "
           "not fault");
  CHECK_EQ(b0, 0x04u, "Byte 0 mismatch");
  CHECK_EQ(h0, 0x0304u, "Halfword 0 mismatch");

  g_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write8(kI2c0Base + I2C_HOST_FIFO_CONFIG_REG_OFFSET, 0x55u);
  CHECK_EQ(g_fault_count, 1u,
           "[i2c_reg_pkg.sv:548-581] Expected sb to HOST_FIFO_CONFIG (4'b1111) "
           "to trap");
  CHECK_EQ(g_last_mcause, (uint32_t)kRiscvStoreAccessFault,
           "Expected mcause=7");

  g_fault_count = 0;
  g_last_mcause = 0;
  *(volatile uint16_t *)(uintptr_t)(kI2c0Base +
                                    I2C_HOST_FIFO_CONFIG_REG_OFFSET) = 0x0007u;
  CHECK_EQ(g_fault_count, 1u,
           "[i2c_reg_pkg.sv:548-581] Expected sh to HOST_FIFO_CONFIG (4'b1111) "
           "to trap");
  CHECK_EQ(g_last_mcause, (uint32_t)kRiscvStoreAccessFault,
           "Expected mcause=7");
  CHECK_EQ(abs_mmio_read32(kI2c0Base + I2C_HOST_FIFO_CONFIG_REG_OFFSET),
           0x01020304u, "Rejected sub-word writes must not modify register");
  abs_mmio_write32(kI2c0Base + I2C_HOST_FIFO_CONFIG_REG_OFFSET, 0u);

  g_fault_count = 0;
  g_last_mcause = 0;
  *(volatile uint16_t *)(uintptr_t)(kI2c0Base +
                                    I2C_HOST_TIMEOUT_CTRL_REG_OFFSET) = 0x6789u;
  CHECK_EQ(g_fault_count, 1u,
           "[i2c_reg_pkg.sv:548-581] Expected sh to HOST_TIMEOUT_CTRL "
           "(4'b0111) to trap");
  CHECK_EQ(g_last_mcause, (uint32_t)kRiscvStoreAccessFault,
           "Expected mcause=7");

  g_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write8(kI2c0Base + I2C_INTR_ENABLE_REG_OFFSET, 0x15u);
  CHECK_EQ(
      g_fault_count, 1u,
      "[i2c_reg_pkg.sv:548-581] Expected sb to INTR_ENABLE (4'b0011) to trap");
  CHECK_EQ(g_last_mcause, (uint32_t)kRiscvStoreAccessFault,
           "Expected mcause=7");

  g_fault_count = 0;
  *(volatile uint16_t *)(uintptr_t)(kI2c0Base + I2C_INTR_ENABLE_REG_OFFSET) =
      0x0015u;
  CHECK_EQ(g_fault_count, 0u,
           "Expected sh to INTR_ENABLE+0 (4'b0011) to succeed");
  CHECK_EQ(abs_mmio_read32(kI2c0Base + I2C_INTR_ENABLE_REG_OFFSET), 0x0015u,
           "Expected INTR_ENABLE == 0x15");
  abs_mmio_write32(kI2c0Base + I2C_INTR_ENABLE_REG_OFFSET, 0u);
}

bool test_main(void) {
  LOG_INFO("Starting I2C Earlgrey v2 CW340 FPGA Errata Test (trunk-v2)...");

  test_i2c_v2_timing_width_and_dif_truncation();
  test_i2c_fifo_preload_hostidle_val_and_ovrd_interference();
  test_i2c_readb_rcont_stop_and_nack_count();
  test_i2c_ovrd_interference_when_enabled();
  test_i2c_permit_subword_writes_vs_reads();

  LOG_INFO("All I2C Earlgrey v2 errata checks PASSED on CW340 FPGA!");
  return true;
}
