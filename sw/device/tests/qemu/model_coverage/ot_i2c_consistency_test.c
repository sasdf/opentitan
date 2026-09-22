// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_pinmux.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "i2c_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kI2c0Base = TOP_EARLGREY_I2C0_BASE_ADDR,
  kPinmuxBase = TOP_EARLGREY_PINMUX_AON_BASE_ADDR,
};

bool test_main(void) {
  LOG_INFO("Starting ot_i2c FPGA/QEMU consistency test on I2C0");

  // Disable I2C0 controller/target and reset all 4 FIFOs.
  abs_mmio_write32(kI2c0Base + I2C_CTRL_REG_OFFSET, 0x0u);
  const uint32_t kResetAllFifos =
      (1u << I2C_FIFO_CTRL_RXRST_BIT) | (1u << I2C_FIFO_CTRL_FMTRST_BIT) |
      (1u << I2C_FIFO_CTRL_ACQRST_BIT) | (1u << I2C_FIFO_CTRL_TXRST_BIT);
  abs_mmio_write32(kI2c0Base + I2C_FIFO_CTRL_REG_OFFSET, kResetAllFifos);
  abs_mmio_write32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET, 0x7fffu);

  // 1. Write-only register readbacks (INTR_TEST, ALERT_TEST, FDATA, TXDATA)
  // must return 0 without raising a bus fault.
  CHECK(abs_mmio_read32(kI2c0Base + I2C_INTR_TEST_REG_OFFSET) == 0x0u);
  CHECK(abs_mmio_read32(kI2c0Base + I2C_ALERT_TEST_REG_OFFSET) == 0x0u);
  CHECK(abs_mmio_read32(kI2c0Base + I2C_FDATA_REG_OFFSET) == 0x0u);
  CHECK(abs_mmio_read32(kI2c0Base + I2C_TXDATA_REG_OFFSET) == 0x0u);

  // 2. 64-entry Target TX FIFO full (FifoDepth = 64 in i2c_reg_pkg.sv:
  // STATUS.TXFULL == 1, TARGET_FIFO_STATUS.TXLVL == 64), 65th byte overflow
  // drop, and FIFO_CTRL.TXRST clear.
  uint32_t status = abs_mmio_read32(kI2c0Base + I2C_STATUS_REG_OFFSET);
  CHECK((status & (1u << I2C_STATUS_TXEMPTY_BIT)) != 0u);
  CHECK((status & (1u << I2C_STATUS_TXFULL_BIT)) == 0u);
  CHECK(abs_mmio_read32(kI2c0Base + I2C_TARGET_FIFO_STATUS_REG_OFFSET) == 0x0u);

  for (uint32_t i = 0; i < 64; ++i) {
    abs_mmio_write32(kI2c0Base + I2C_TXDATA_REG_OFFSET, (i ^ 0x3cu) & 0xffu);
  }

  uint32_t tgt_fifo_status =
      abs_mmio_read32(kI2c0Base + I2C_TARGET_FIFO_STATUS_REG_OFFSET);
  uint32_t txlvl = (tgt_fifo_status >> I2C_TARGET_FIFO_STATUS_TXLVL_OFFSET) &
                   I2C_TARGET_FIFO_STATUS_TXLVL_MASK;
  CHECK(txlvl == 64u);
  status = abs_mmio_read32(kI2c0Base + I2C_STATUS_REG_OFFSET);
  CHECK((status & (1u << I2C_STATUS_TXFULL_BIT)) != 0u);
  CHECK((status & (1u << I2C_STATUS_TXEMPTY_BIT)) == 0u);

  // Write 65th byte while TXFULL == 1; must be dropped and leave TXLVL == 64.
  abs_mmio_write32(kI2c0Base + I2C_TXDATA_REG_OFFSET, 0xffu);
  tgt_fifo_status =
      abs_mmio_read32(kI2c0Base + I2C_TARGET_FIFO_STATUS_REG_OFFSET);
  txlvl = (tgt_fifo_status >> I2C_TARGET_FIFO_STATUS_TXLVL_OFFSET) &
          I2C_TARGET_FIFO_STATUS_TXLVL_MASK;
  CHECK(txlvl == 64u);
  status = abs_mmio_read32(kI2c0Base + I2C_STATUS_REG_OFFSET);
  CHECK((status & (1u << I2C_STATUS_TXFULL_BIT)) != 0u);

  // Reset Target TX FIFO via FIFO_CTRL.TXRST and verify TXLVL == 0,
  // TXEMPTY == 1, TXFULL == 0.
  abs_mmio_write32(kI2c0Base + I2C_FIFO_CTRL_REG_OFFSET,
                   1u << I2C_FIFO_CTRL_TXRST_BIT);
  tgt_fifo_status =
      abs_mmio_read32(kI2c0Base + I2C_TARGET_FIFO_STATUS_REG_OFFSET);
  txlvl = (tgt_fifo_status >> I2C_TARGET_FIFO_STATUS_TXLVL_OFFSET) &
          I2C_TARGET_FIFO_STATUS_TXLVL_MASK;
  CHECK(txlvl == 0u);
  status = abs_mmio_read32(kI2c0Base + I2C_STATUS_REG_OFFSET);
  CHECK((status & (1u << I2C_STATUS_TXFULL_BIT)) == 0u);
  CHECK((status & (1u << I2C_STATUS_TXEMPTY_BIT)) != 0u);

  // 3. Pre-queuing FDATA while CTRL.ENABLEHOST == 0 and draining when
  // CTRL.ENABLEHOST = 1 is written (in Fast-Mode Plus).
  // Route I2c0Sda -> Ior0 and I2c0Scl -> Ior1 which have Xilinx IOB
  // PULLTYPE PULLUP enabled in hw/top_earlgrey/data/pins_cw341.xdc:66-67.
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

  // Program timings with sufficient T_R (20 cycles) and THD_DAT (10 cycles,
  // satisfying thd_dat + 1 < thd_sta = 60 in i2c_bus_monitor.sv) so open-drain
  // rise/fall times on CW340 FPGA never trigger false sda_released_but_low
  // arbitration loss or false stop_det during HoldBit -> ClockLow SDA changes.
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

  // While CTRL.ENABLEHOST == 0, queue 1 byte in FDATA with START | STOP |
  // NAKOK.
  const uint32_t kFdataEntry =
      (0x7eu << I2C_FDATA_FBYTE_OFFSET) | (1u << I2C_FDATA_START_BIT) |
      (1u << I2C_FDATA_STOP_BIT) | (1u << I2C_FDATA_NAKOK_BIT);
  abs_mmio_write32(kI2c0Base + I2C_FDATA_REG_OFFSET, kFdataEntry);

  // Verify the FDATA entry remains queued in fmt_fifo while ENABLEHOST == 0.
  uint32_t host_fifo_status =
      abs_mmio_read32(kI2c0Base + I2C_HOST_FIFO_STATUS_REG_OFFSET);
  uint32_t fmtlvl = (host_fifo_status >> I2C_HOST_FIFO_STATUS_FMTLVL_OFFSET) &
                    I2C_HOST_FIFO_STATUS_FMTLVL_MASK;
  CHECK(fmtlvl == 1u);
  status = abs_mmio_read32(kI2c0Base + I2C_STATUS_REG_OFFSET);
  CHECK((status & (1u << I2C_STATUS_FMTEMPTY_BIT)) == 0u);
  CHECK((status & (1u << I2C_STATUS_HOSTIDLE_BIT)) != 0u);
  CHECK((abs_mmio_read32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET) &
         (1u << I2C_INTR_STATE_CMD_COMPLETE_BIT)) == 0u);

  // Enable host controller (CTRL.ENABLEHOST = 1) and wait for pre-queued FDATA
  // to drain and complete.
  abs_mmio_write32(kI2c0Base + I2C_CTRL_REG_OFFSET,
                   1u << I2C_CTRL_ENABLEHOST_BIT);

  const uint32_t kHostDoneMask =
      (1u << I2C_STATUS_HOSTIDLE_BIT) | (1u << I2C_STATUS_FMTEMPTY_BIT);
  const uint32_t kHostAndTargetDoneMask =
      kHostDoneMask | (1u << I2C_STATUS_TARGETIDLE_BIT);
  while (((abs_mmio_read32(kI2c0Base + I2C_STATUS_REG_OFFSET) &
           kHostDoneMask) != kHostDoneMask) ||
         ((abs_mmio_read32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET) &
           (1u << I2C_INTR_STATE_CMD_COMPLETE_BIT)) == 0u)) {
  }

  host_fifo_status =
      abs_mmio_read32(kI2c0Base + I2C_HOST_FIFO_STATUS_REG_OFFSET);
  fmtlvl = (host_fifo_status >> I2C_HOST_FIFO_STATUS_FMTLVL_OFFSET) &
           I2C_HOST_FIFO_STATUS_FMTLVL_MASK;
  CHECK(fmtlvl == 0u);
  status = abs_mmio_read32(kI2c0Base + I2C_STATUS_REG_OFFSET);
  CHECK((status & (1u << I2C_STATUS_FMTEMPTY_BIT)) != 0u);
  CHECK((status & (1u << I2C_STATUS_HOSTIDLE_BIT)) != 0u);
  CHECK((abs_mmio_read32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET) &
         (1u << I2C_INTR_STATE_CMD_COMPLETE_BIT)) != 0u);

  // 4A. Target disabled address match (Chunk 18: ot_i2c_target_event when
  // TARGET_ID matches 0x33 while CTRL.ENABLETARGET == 0).
  abs_mmio_write32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET, 0x7fffu);
  abs_mmio_write32(kI2c0Base + I2C_TARGET_ID_REG_OFFSET,
                   (0x33u << I2C_TARGET_ID_ADDRESS0_OFFSET) |
                       (0x7fu << I2C_TARGET_ID_MASK0_OFFSET));
  abs_mmio_write32(
      kI2c0Base + I2C_FDATA_REG_OFFSET,
      (0x66u << I2C_FDATA_FBYTE_OFFSET) | (1u << I2C_FDATA_START_BIT) |
          (1u << I2C_FDATA_STOP_BIT) | (1u << I2C_FDATA_NAKOK_BIT));
  while (((abs_mmio_read32(kI2c0Base + I2C_STATUS_REG_OFFSET) &
           kHostDoneMask) != kHostDoneMask) ||
         ((abs_mmio_read32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET) &
           (1u << I2C_INTR_STATE_CMD_COMPLETE_BIT)) == 0u)) {
  }
  CHECK((abs_mmio_read32(kI2c0Base + I2C_STATUS_REG_OFFSET) &
         (1u << I2C_STATUS_ACQEMPTY_BIT)) != 0u);
  abs_mmio_write32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET, 0x7fffu);

  // 4B. Broadcast / non-matching address check with ENABLETARGET = 1 (Chunk 19:
  // ot_i2c_target_event when I2C_BROADCAST 0x00 does not match TARGET_ID 0x33).
  abs_mmio_write32(
      kI2c0Base + I2C_CTRL_REG_OFFSET,
      (1u << I2C_CTRL_ENABLEHOST_BIT) | (1u << I2C_CTRL_ENABLETARGET_BIT));
  abs_mmio_write32(
      kI2c0Base + I2C_FDATA_REG_OFFSET,
      (0x00u << I2C_FDATA_FBYTE_OFFSET) | (1u << I2C_FDATA_START_BIT) |
          (1u << I2C_FDATA_STOP_BIT) | (1u << I2C_FDATA_NAKOK_BIT));
  while (((abs_mmio_read32(kI2c0Base + I2C_STATUS_REG_OFFSET) &
           kHostAndTargetDoneMask) != kHostAndTargetDoneMask) ||
         ((abs_mmio_read32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET) &
           (1u << I2C_INTR_STATE_CMD_COMPLETE_BIT)) == 0u)) {
  }
  CHECK((abs_mmio_read32(kI2c0Base + I2C_STATUS_REG_OFFSET) &
         (1u << I2C_STATUS_ACQEMPTY_BIT)) != 0u);
  abs_mmio_write32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET, 0x7fffu);

  // 4C. Cross-address Repeated START (Chunk 12: START to 0x33 without STOP,
  // followed by Repeated START to 0x34 with STOP | NAKOK).
  abs_mmio_write32(
      kI2c0Base + I2C_FDATA_REG_OFFSET,
      (0x66u << I2C_FDATA_FBYTE_OFFSET) | (1u << I2C_FDATA_START_BIT));
  abs_mmio_write32(
      kI2c0Base + I2C_FDATA_REG_OFFSET,
      (0x68u << I2C_FDATA_FBYTE_OFFSET) | (1u << I2C_FDATA_START_BIT) |
          (1u << I2C_FDATA_STOP_BIT) | (1u << I2C_FDATA_NAKOK_BIT));
  while (((abs_mmio_read32(kI2c0Base + I2C_STATUS_REG_OFFSET) &
           kHostAndTargetDoneMask) != kHostAndTargetDoneMask) ||
         ((abs_mmio_read32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET) &
           (1u << I2C_INTR_STATE_CMD_COMPLETE_BIT)) == 0u)) {
  }
  abs_mmio_write32(kI2c0Base + I2C_FIFO_CTRL_REG_OFFSET, kResetAllFifos);
  abs_mmio_write32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET, 0x7fffu);

  // 4D. Target Read with READB + NAKOK (Chunk 10) and READB + RCONT + STOP
  // (Chunk 11), asserting RDATA readback and INTR_STATE.UNEXP_STOP == 1.
  // Push a 3rd byte (0xff) so that after the host ACKs the 2nd byte (RCONT=1),
  // the target enters TransmitSetup with SDA=1, SCL=1 (instead of StretchTx
  // holding SCL=0), allowing the host to issue STOP and trigger UNEXP_STOP.
  abs_mmio_write32(kI2c0Base + I2C_TXDATA_REG_OFFSET, 0xa5u);
  abs_mmio_write32(kI2c0Base + I2C_TXDATA_REG_OFFSET, 0x5au);
  abs_mmio_write32(kI2c0Base + I2C_TXDATA_REG_OFFSET, 0xffu);
  abs_mmio_write32(
      kI2c0Base + I2C_FDATA_REG_OFFSET,
      (0x67u << I2C_FDATA_FBYTE_OFFSET) | (1u << I2C_FDATA_START_BIT));
  abs_mmio_write32(
      kI2c0Base + I2C_FDATA_REG_OFFSET,
      (1u << I2C_FDATA_FBYTE_OFFSET) | (1u << I2C_FDATA_READB_BIT) |
          (1u << I2C_FDATA_RCONT_BIT) | (1u << I2C_FDATA_NAKOK_BIT));
  abs_mmio_write32(
      kI2c0Base + I2C_FDATA_REG_OFFSET,
      (1u << I2C_FDATA_FBYTE_OFFSET) | (1u << I2C_FDATA_READB_BIT) |
          (1u << I2C_FDATA_RCONT_BIT) | (1u << I2C_FDATA_STOP_BIT));
  while (((abs_mmio_read32(kI2c0Base + I2C_STATUS_REG_OFFSET) &
           kHostAndTargetDoneMask) != kHostAndTargetDoneMask) ||
         ((abs_mmio_read32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET) &
           (1u << I2C_INTR_STATE_CMD_COMPLETE_BIT)) == 0u)) {
  }
  CHECK((abs_mmio_read32(kI2c0Base + I2C_RDATA_REG_OFFSET) & 0xffu) == 0xa5u);
  CHECK((abs_mmio_read32(kI2c0Base + I2C_RDATA_REG_OFFSET) & 0xffu) == 0x5au);
  CHECK((abs_mmio_read32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET) &
         (1u << I2C_INTR_STATE_UNEXP_STOP_BIT)) != 0u);
  abs_mmio_write32(kI2c0Base + I2C_FIFO_CTRL_REG_OFFSET, kResetAllFifos);
  abs_mmio_write32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET, 0x7fffu);

  // 4E. Target ACK_CTRL_EN stretching & TARGET_ACK_CTRL.NACK (Chunks 1 & 22):
  // Verify STATUS.ACK_CTRL_STRETCH, ACQ_FIFO_NEXT_DATA, TARGET_NACK_COUNT == 1,
  // CONTROLLER_EVENTS.NACK == 1, and INTR_STATE.CONTROLLER_HALT == 1.
  (void)abs_mmio_read32(kI2c0Base + I2C_TARGET_NACK_COUNT_REG_OFFSET);
  abs_mmio_write32(kI2c0Base + I2C_CONTROLLER_EVENTS_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kI2c0Base + I2C_CTRL_REG_OFFSET,
                   (1u << I2C_CTRL_ENABLEHOST_BIT) |
                       (1u << I2C_CTRL_ENABLETARGET_BIT) |
                       (1u << I2C_CTRL_ACK_CTRL_EN_BIT));
  abs_mmio_write32(
      kI2c0Base + I2C_FDATA_REG_OFFSET,
      (0x66u << I2C_FDATA_FBYTE_OFFSET) | (1u << I2C_FDATA_START_BIT));
  abs_mmio_write32(
      kI2c0Base + I2C_FDATA_REG_OFFSET,
      (0x55u << I2C_FDATA_FBYTE_OFFSET) | (1u << I2C_FDATA_STOP_BIT));
  while ((abs_mmio_read32(kI2c0Base + I2C_STATUS_REG_OFFSET) &
          (1u << I2C_STATUS_ACK_CTRL_STRETCH_BIT)) == 0u) {
  }
  CHECK((abs_mmio_read32(kI2c0Base + I2C_ACQ_FIFO_NEXT_DATA_REG_OFFSET) &
         0xffu) == 0x55u);
  abs_mmio_write32(kI2c0Base + I2C_TARGET_ACK_CTRL_REG_OFFSET,
                   1u << I2C_TARGET_ACK_CTRL_NACK_BIT);
  while ((abs_mmio_read32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET) &
          (1u << I2C_INTR_STATE_CONTROLLER_HALT_BIT)) == 0u) {
  }
  CHECK((abs_mmio_read32(kI2c0Base + I2C_CONTROLLER_EVENTS_REG_OFFSET) &
         (1u << I2C_CONTROLLER_EVENTS_NACK_BIT)) != 0u);
  CHECK(abs_mmio_read32(kI2c0Base + I2C_TARGET_NACK_COUNT_REG_OFFSET) == 1u);
  abs_mmio_write32(kI2c0Base + I2C_CONTROLLER_EVENTS_REG_OFFSET,
                   1u << I2C_CONTROLLER_EVENTS_NACK_BIT);
  while ((abs_mmio_read32(kI2c0Base + I2C_STATUS_REG_OFFSET) &
          kHostAndTargetDoneMask) != kHostAndTargetDoneMask) {
  }
  abs_mmio_write32(kI2c0Base + I2C_CTRL_REG_OFFSET, 0x0u);
  abs_mmio_write32(kI2c0Base + I2C_FIFO_CTRL_REG_OFFSET, kResetAllFifos);
  abs_mmio_write32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET, 0x7fffu);

  // 4F. Target ACK_CTRL_EN stretching & TARGET_ACK_CTRL.NBYTES = 1 (NACK = 0)
  // with pending STOP (Chunk 11: ot_i2c.c:L1487-L1491).
  LOG_INFO("Step 4F: TARGET_ACK_CTRL NBYTES=1 with pending STOP");
  abs_mmio_write32(kI2c0Base + I2C_CTRL_REG_OFFSET,
                   (1u << I2C_CTRL_ENABLEHOST_BIT) |
                       (1u << I2C_CTRL_ENABLETARGET_BIT) |
                       (1u << I2C_CTRL_ACK_CTRL_EN_BIT));
  abs_mmio_write32(
      kI2c0Base + I2C_FDATA_REG_OFFSET,
      (0x66u << I2C_FDATA_FBYTE_OFFSET) | (1u << I2C_FDATA_START_BIT));
  abs_mmio_write32(
      kI2c0Base + I2C_FDATA_REG_OFFSET,
      (0x77u << I2C_FDATA_FBYTE_OFFSET) | (1u << I2C_FDATA_STOP_BIT));
  while ((abs_mmio_read32(kI2c0Base + I2C_STATUS_REG_OFFSET) &
          (1u << I2C_STATUS_ACK_CTRL_STRETCH_BIT)) == 0u) {
  }
  CHECK((abs_mmio_read32(kI2c0Base + I2C_ACQ_FIFO_NEXT_DATA_REG_OFFSET) &
         0xffu) == 0x77u);
  abs_mmio_write32(kI2c0Base + I2C_TARGET_ACK_CTRL_REG_OFFSET,
                   1u << I2C_TARGET_ACK_CTRL_NBYTES_OFFSET);
  while (((abs_mmio_read32(kI2c0Base + I2C_STATUS_REG_OFFSET) &
           kHostAndTargetDoneMask) != kHostAndTargetDoneMask) ||
         ((abs_mmio_read32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET) &
           (1u << I2C_INTR_STATE_CMD_COMPLETE_BIT)) == 0u)) {
  }
  CHECK((abs_mmio_read32(kI2c0Base + I2C_ACQDATA_REG_OFFSET) & 0xffu) == 0x66u);
  CHECK((abs_mmio_read32(kI2c0Base + I2C_ACQDATA_REG_OFFSET) & 0xffu) == 0x77u);
  (void)abs_mmio_read32(kI2c0Base + I2C_ACQDATA_REG_OFFSET);
  CHECK((abs_mmio_read32(kI2c0Base + I2C_STATUS_REG_OFFSET) &
         (1u << I2C_STATUS_ACQEMPTY_BIT)) != 0u);
  abs_mmio_write32(kI2c0Base + I2C_CTRL_REG_OFFSET, 0x0u);
  abs_mmio_write32(kI2c0Base + I2C_FIFO_CTRL_REG_OFFSET, kResetAllFifos);
  abs_mmio_write32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET, 0x7fffu);

  // 4G. Fill ACQDATA FIFO to >= AcqFifoDepth - 2 (266..268 entries,
  // i2c_target_fsm.sv:270,1027 acq_fifo_full_o = !acq_fifo_plenty_space) and
  // verify STATUS.ACQFULL == 0 at 264 entries and STATUS.ACQFULL == 1 at 267
  // entries (Chunk 6: ot_i2c.c:L963-L964).
  // Send 8 30-byte write transactions (8 * 32 = 256 ACQ entries) + 1 6-byte
  // write transaction (8 ACQ entries) = 264 ACQ entries.
  LOG_INFO("Step 4G: STATUS.ACQFULL at 267 entries");
  abs_mmio_write32(
      kI2c0Base + I2C_CTRL_REG_OFFSET,
      (1u << I2C_CTRL_ENABLEHOST_BIT) | (1u << I2C_CTRL_ENABLETARGET_BIT));
  for (uint32_t t = 0; t < 9u; ++t) {
    uint32_t data_bytes = (t < 8u) ? 30u : 6u;
    abs_mmio_write32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET, 0x7fffu);
    abs_mmio_write32(
        kI2c0Base + I2C_FDATA_REG_OFFSET,
        (0x66u << I2C_FDATA_FBYTE_OFFSET) | (1u << I2C_FDATA_START_BIT));
    for (uint32_t b = 0; b + 1u < data_bytes; ++b) {
      abs_mmio_write32(kI2c0Base + I2C_FDATA_REG_OFFSET,
                       ((b + 1u) & 0xffu) << I2C_FDATA_FBYTE_OFFSET);
    }
    abs_mmio_write32(
        kI2c0Base + I2C_FDATA_REG_OFFSET,
        (0x55u << I2C_FDATA_FBYTE_OFFSET) | (1u << I2C_FDATA_STOP_BIT));
    while (((abs_mmio_read32(kI2c0Base + I2C_STATUS_REG_OFFSET) &
             kHostAndTargetDoneMask) != kHostAndTargetDoneMask) ||
           ((abs_mmio_read32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET) &
             (1u << I2C_INTR_STATE_CMD_COMPLETE_BIT)) == 0u)) {
    }
  }
  tgt_fifo_status =
      abs_mmio_read32(kI2c0Base + I2C_TARGET_FIFO_STATUS_REG_OFFSET);
  uint32_t acqlvl = (tgt_fifo_status >> I2C_TARGET_FIFO_STATUS_ACQLVL_OFFSET) &
                    I2C_TARGET_FIFO_STATUS_ACQLVL_MASK;
  CHECK(acqlvl == 264u);
  CHECK((abs_mmio_read32(kI2c0Base + I2C_STATUS_REG_OFFSET) &
         (1u << I2C_STATUS_ACQFULL_BIT)) == 0u);

  // Send 1 more 1-byte write transaction (AcqStart + AcqData + AcqStop = 3
  // entries -> acqlvl = 267 >= 266), asserting STATUS.ACQFULL == 1.
  abs_mmio_write32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET, 0x7fffu);
  abs_mmio_write32(
      kI2c0Base + I2C_FDATA_REG_OFFSET,
      (0x66u << I2C_FDATA_FBYTE_OFFSET) | (1u << I2C_FDATA_START_BIT));
  abs_mmio_write32(
      kI2c0Base + I2C_FDATA_REG_OFFSET,
      (0x77u << I2C_FDATA_FBYTE_OFFSET) | (1u << I2C_FDATA_STOP_BIT));
  uint32_t spin9 = 0;
  while (((abs_mmio_read32(kI2c0Base + I2C_STATUS_REG_OFFSET) &
           kHostAndTargetDoneMask) != kHostAndTargetDoneMask) ||
         ((abs_mmio_read32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET) &
           (1u << I2C_INTR_STATE_CMD_COMPLETE_BIT)) == 0u)) {
    if (++spin9 > 50000u) {
      LOG_ERROR(
          "4G tx9 stall status=0x%08x intr=0x%08x cev=0x%08x tfifo=0x%08x",
          abs_mmio_read32(kI2c0Base + I2C_STATUS_REG_OFFSET),
          abs_mmio_read32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET),
          abs_mmio_read32(kI2c0Base + I2C_CONTROLLER_EVENTS_REG_OFFSET),
          abs_mmio_read32(kI2c0Base + I2C_TARGET_FIFO_STATUS_REG_OFFSET));
      CHECK(false);
    }
  }
  tgt_fifo_status =
      abs_mmio_read32(kI2c0Base + I2C_TARGET_FIFO_STATUS_REG_OFFSET);
  acqlvl = (tgt_fifo_status >> I2C_TARGET_FIFO_STATUS_ACQLVL_OFFSET) &
           I2C_TARGET_FIFO_STATUS_ACQLVL_MASK;
  CHECK(acqlvl == 267u);
  CHECK((abs_mmio_read32(kI2c0Base + I2C_STATUS_REG_OFFSET) &
         (1u << I2C_STATUS_ACQFULL_BIT)) != 0u);
  abs_mmio_write32(kI2c0Base + I2C_FIFO_CTRL_REG_OFFSET, kResetAllFifos);
  abs_mmio_write32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET, 0x7fffu);

  // 4H. Fast Mode Plus timing configuration check in ot_i2c_check_timings
  // (Chunks 1-5: ot_i2c.c:L817-L822, L836, L841, L846, L851).
  abs_mmio_write32(kI2c0Base + I2C_CTRL_REG_OFFSET, 0x0u);
  abs_mmio_write32(
      kI2c0Base + I2C_TIMING0_REG_OFFSET,
      (10u << I2C_TIMING0_THIGH_OFFSET) | (15u << I2C_TIMING0_TLOW_OFFSET));
  abs_mmio_write32(
      kI2c0Base + I2C_TIMING1_REG_OFFSET,
      (2u << I2C_TIMING1_T_R_OFFSET) | (2u << I2C_TIMING1_T_F_OFFSET));
  abs_mmio_write32(
      kI2c0Base + I2C_TIMING2_REG_OFFSET,
      (1u << I2C_TIMING2_TSU_STA_OFFSET) | (5u << I2C_TIMING2_THD_STA_OFFSET));
  abs_mmio_write32(
      kI2c0Base + I2C_TIMING3_REG_OFFSET,
      (1u << I2C_TIMING3_TSU_DAT_OFFSET) | (1u << I2C_TIMING3_THD_DAT_OFFSET));
  abs_mmio_write32(
      kI2c0Base + I2C_TIMING4_REG_OFFSET,
      (1u << I2C_TIMING4_TSU_STO_OFFSET) | (1u << I2C_TIMING4_T_BUF_OFFSET));
  abs_mmio_write32(kI2c0Base + I2C_CTRL_REG_OFFSET,
                   1u << I2C_CTRL_ENABLEHOST_BIT);
  CHECK(abs_mmio_read32(kI2c0Base + I2C_TIMING0_REG_OFFSET) ==
        ((10u << I2C_TIMING0_THIGH_OFFSET) | (15u << I2C_TIMING0_TLOW_OFFSET)));

  // Clean up I2C0 state.
  abs_mmio_write32(kI2c0Base + I2C_CTRL_REG_OFFSET, 0x0u);
  abs_mmio_write32(kI2c0Base + I2C_FIFO_CTRL_REG_OFFSET, kResetAllFifos);
  abs_mmio_write32(kI2c0Base + I2C_INTR_STATE_REG_OFFSET, 0x7fffu);

  LOG_INFO("ot_i2c FPGA/QEMU consistency test passed");
  return true;
}
