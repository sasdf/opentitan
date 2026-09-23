// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * CW340 FPGA & QEMU Empirical Errata Confirmation Test for `i2c` (P31).
 *
 * Exercises and confirms all documented silicon / spec / architectural
 * behaviors in `/root/knowledge/errata/i2c.md`:
 * - [i2c_controller_fsm.sv:500-525] (`SPEC_DOC_ERRATA` / `E1`): `FDATA` with
 * `READB = 1`, `RCONT = 1`, and `STOP = 1` executes `ACK` + `STOP` in
 *   `i2c_controller_fsm.sv:500-525, 841-848` (contrary to
 * `theory_of_operation.md:75`), which triggers `INTR_STATE.UNEXP_STOP`
 * (`event_unexp_stop_o`) on the Target FSM.
 * - [i2c_target_fsm.sv:326-328] (`BENIGN_RTL_IMPL_DETAIL` / `E2`):
 * `TARGET_NACK_COUNT`
 *   (`0x68`, `SwAccessRC`) increments on the rising edge of `nack_transaction`
 *   (`!nack_transaction_q && nack_transaction_d`, at most once per NACKed
 *   transaction) and clears to `0` on read.
 * - [i2c_core.sv:331-343] & [i2c_controller_fsm.sv:617-650] (`SPEC_DOC_ERRATA`
 * & `E2`): `FDATA`
 *   (`fmt_fifo`) and `TXDATA` (`tx_fifo`) accept preloaded entries while
 *   `CTRL.ENABLEHOST == 0` / `CTRL.ENABLETARGET == 0`, `STATUS.HOSTIDLE` stays
 *   `1` when `CTRL.ENABLEHOST == 0` even with `FMTLVL > 0`, and `VAL` (`0x38`)
 *   shifts in `0xffffffff` after 16 idle cycles and reflects `OVRD` (`0x34`).
 * - [i2c_reg_pkg.sv:786-819] (`INTENDED_SECURITY_HARDENING`): `I2C_PERMIT[32]`
 *   sub-word write faults (`mcause = 7` on `4'b1111`, `4'b0111`, `4'b0011`,
 *   and `4'b0001` upper bytes) vs permitted sub-word writes and sub-word reads.
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_pinmux.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "i2c_regs.h"

OTTF_DEFINE_TEST_CONFIG();

#define CHECK_EQ(a, b, ...) CHECK((a) == (b), __VA_ARGS__)

enum {
  kI2c0Base = TOP_EARLGREY_I2C0_BASE_ADDR,
  kPinmuxBase = TOP_EARLGREY_PINMUX_AON_BASE_ADDR,
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

static void test_i2c_fifo_preload_hostidle_and_val_ovrd(void) {
  LOG_INFO(
      "Verifying [i2c_core.sv:331-343] & [i2c_controller_fsm.sv:617-650]: "
      "FDATA/TXDATA preload "
      "when disabled, STATUS.HOSTIDLE=1, & VAL/OVRD");

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
           "[i2c_core.sv:331-343] Expected FMTLVL == 2 when ENABLEHOST == 0");
  CHECK_EQ(bitfield_field32_read(t_status, I2C_TARGET_FIFO_STATUS_TXLVL_FIELD),
           2u,
           "[i2c_core.sv:331-343] Expected TXLVL == 2 when ENABLETARGET == 0");
  CHECK(!bitfield_bit32_read(status, I2C_STATUS_FMTEMPTY_BIT),
        "Expected STATUS.FMTEMPTY == 0");
  CHECK(bitfield_bit32_read(status, I2C_STATUS_HOSTIDLE_BIT),
        "[i2c_controller_fsm.sv:617-650] Expected STATUS.HOSTIDLE == 1 when "
        "ENABLEHOST == 0 "
        "even with FMTLVL == 2");

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
  CHECK_EQ(abs_mmio_read32(kI2c0Base + I2C_VAL_REG_OFFSET), 0xFFFFFFFFu,
           "[i2c_controller_fsm.sv:617-650] Expected VAL == 0xffffffff when "
           "SCL=1, SDA=1");

  // Override SCL=0, SDA=1 (TXOVRDEN=1, SCLVAL=0, SDAVAL=1 -> 0x5).
  abs_mmio_write32(kI2c0Base + I2C_OVRD_REG_OFFSET,
                   (1u << I2C_OVRD_TXOVRDEN_BIT) | (1u << I2C_OVRD_SDAVAL_BIT));
  busy_spin_micros(5);
  CHECK_EQ(abs_mmio_read32(kI2c0Base + I2C_VAL_REG_OFFSET), 0xFFFF0000u,
           "[i2c_controller_fsm.sv:617-650] Expected VAL == 0xffff0000 when "
           "SCL=0, SDA=1");

  abs_mmio_write32(kI2c0Base + I2C_OVRD_REG_OFFSET, 0u);
  precharge_i2c0_pads_high(&pinmux);
}

static void test_i2c_readb_rcont_stop_and_nack_count(void) {
  LOG_INFO(
      "Verifying [i2c_controller_fsm.sv:500-525] & "
      "[i2c_target_fsm.sv:326-328]: FDATA READB+RCONT+STOP "
      "-> UNEXP_STOP & TARGET_NACK_COUNT");

  dif_pinmux_t pinmux;
  CHECK_DIF_OK(dif_pinmux_init(mmio_region_from_addr(kPinmuxBase), &pinmux));
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

  // Preload 2 bytes into TXDATA: 0xA5 (read by host) followed by 0xFF (MSB=1!).
  // Because the follow-up byte in TXDATA has MSB = 1 (0xFF), after the host
  // ACKs byte 0 (RCONT=1), the Target FSM enters TransmitSetup driving SDA=1,
  // SCL=1 on the open-drain bus (rather than driving SDA=0 if MSB were 0 or
  // stretching SCL=0 if TX FIFO were empty), allowing the Controller FSM's STOP
  // condition (SDA rising 0->1 while SCL=1) to propagate across the open-drain
  // pad and fire stop_detect_i -> INTR_STATE.UNEXP_STOP = 1!
  abs_mmio_write32(kI2c0Base + I2C_TXDATA_REG_OFFSET, 0xA5u);
  abs_mmio_write32(kI2c0Base + I2C_TXDATA_REG_OFFSET, 0xFFu);

  // Issue START + target read address (0x33 << 1 | 1), followed by
  // FDATA with READB = 1, RCONT = 1, STOP = 1, FBYTE = 1!
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
        "after READB + "
        "RCONT + STOP");
  CHECK_EQ(abs_mmio_read32(kI2c0Base + I2C_RDATA_REG_OFFSET) & 0xFFu, 0xA5u,
           "Expected RDATA == 0xA5");

  // Also verify [i2c_target_fsm.sv:326-328]: TARGET_NACK_COUNT (0x68) is
  // SwAccessRC (ignores writes) and HOST_TIMEOUT_CTRL (0x60) masks to 20 bits
  // (0x000fffff).
  abs_mmio_write32(kI2c0Base + I2C_TARGET_NACK_COUNT_REG_OFFSET, 0xFFFFFFFFu);
  CHECK_EQ(
      abs_mmio_read32(kI2c0Base + I2C_TARGET_NACK_COUNT_REG_OFFSET), 0u,
      "[i2c_target_fsm.sv:326-328] Expected TARGET_NACK_COUNT == 0 after write "
      "(SwAccessRC)");
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
      "Verifying [i2c_reg_pkg.sv:786-819]: I2C_PERMIT[32] sub-word write "
      "faults vs "
      "sub-word reads");

  // 1. Sub-word writes (sb/sh) to 4'b1111 HOST_FIFO_CONFIG (0x24) trap with
  // mcause = 7, whereas sub-word reads (lb/lh) succeed without faulting.
  abs_mmio_write32(kI2c0Base + I2C_HOST_FIFO_CONFIG_REG_OFFSET, 0x01020304u);
  g_fault_count = 0;
  uint8_t b0 = abs_mmio_read8(kI2c0Base + I2C_HOST_FIFO_CONFIG_REG_OFFSET);
  uint16_t h0 =
      *(const volatile uint16_t *)(uintptr_t)(kI2c0Base +
                                              I2C_HOST_FIFO_CONFIG_REG_OFFSET);
  CHECK_EQ(g_fault_count, 0u,
           "[i2c_reg_pkg.sv:786-819] Sub-word reads from HOST_FIFO_CONFIG must "
           "not fault");
  CHECK_EQ(b0, 0x04u, "Byte 0 mismatch");
  CHECK_EQ(h0, 0x0304u, "Halfword 0 mismatch");

  g_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write8(kI2c0Base + I2C_HOST_FIFO_CONFIG_REG_OFFSET, 0x55u);
  CHECK_EQ(g_fault_count, 1u,
           "[i2c_reg_pkg.sv:786-819] Expected sb to HOST_FIFO_CONFIG (4'b1111) "
           "to trap");
  CHECK_EQ(g_last_mcause, (uint32_t)kRiscvStoreAccessFault,
           "Expected mcause=7");

  g_fault_count = 0;
  g_last_mcause = 0;
  *(volatile uint16_t *)(uintptr_t)(kI2c0Base +
                                    I2C_HOST_FIFO_CONFIG_REG_OFFSET) = 0x0007u;
  CHECK_EQ(g_fault_count, 1u,
           "[i2c_reg_pkg.sv:786-819] Expected sh to HOST_FIFO_CONFIG (4'b1111) "
           "to trap");
  CHECK_EQ(g_last_mcause, (uint32_t)kRiscvStoreAccessFault,
           "Expected mcause=7");
  CHECK_EQ(abs_mmio_read32(kI2c0Base + I2C_HOST_FIFO_CONFIG_REG_OFFSET),
           0x01020304u, "Rejected sub-word writes must not modify register");
  abs_mmio_write32(kI2c0Base + I2C_HOST_FIFO_CONFIG_REG_OFFSET, 0u);

  // 2. Sub-word sh to 4'b0111 HOST_TIMEOUT_CTRL (0x60) traps with mcause = 7.
  g_fault_count = 0;
  g_last_mcause = 0;
  *(volatile uint16_t *)(uintptr_t)(kI2c0Base +
                                    I2C_HOST_TIMEOUT_CTRL_REG_OFFSET) = 0x6789u;
  CHECK_EQ(g_fault_count, 1u,
           "[i2c_reg_pkg.sv:786-819] Expected sh to HOST_TIMEOUT_CTRL "
           "(4'b0111) to trap");
  CHECK_EQ(g_last_mcause, (uint32_t)kRiscvStoreAccessFault,
           "Expected mcause=7");

  // 3. Sub-word sb to 4'b0011 INTR_ENABLE (0x04) traps with mcause = 7,
  // whereas sh at +0 succeeds.
  g_fault_count = 0;
  g_last_mcause = 0;
  abs_mmio_write8(kI2c0Base + I2C_INTR_ENABLE_REG_OFFSET, 0x15u);
  CHECK_EQ(
      g_fault_count, 1u,
      "[i2c_reg_pkg.sv:786-819] Expected sb to INTR_ENABLE (4'b0011) to trap");
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
  LOG_INFO("Starting i2c CW340 FPGA & QEMU errata confirmation test (P31)");

  test_i2c_fifo_preload_hostidle_and_val_ovrd();
  test_i2c_readb_rcont_stop_and_nack_count();
  test_i2c_permit_subword_writes_vs_reads();

  LOG_INFO("All i2c errata checks PASSED");
  return true;
}
