// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_pinmux.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "gpio_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "pattgen_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kPattgenBase = TOP_EARLGREY_PATTGEN_BASE_ADDR,
  kGpioBase = TOP_EARLGREY_GPIO_BASE_ADDR,
  kPinmuxBase = TOP_EARLGREY_PINMUX_AON_BASE_ADDR,
};

static volatile bool g_saw_store_fault = false;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = ibex_mcause_read();
  CHECK((mcause & kIbexExcMax) == kIbexExcStoreAccessFault);
  g_saw_store_fault = true;
}

static uint32_t read_pda0_pcl0(void) {
  return abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) & 0x3u;
}

bool test_main(void) {
  LOG_INFO("Starting ot_pattgen FPGA/QEMU consistency test");

  // Configure pinmux internal loopback:
  // PattgenPda0Tx -> MioOutIob0 -> InselIob0 -> GpioGpio0 (bit 0)
  // PattgenPcl0Tx -> MioOutIob1 -> InselIob1 -> GpioGpio1 (bit 1)
  dif_pinmux_t pinmux;
  CHECK_DIF_OK(dif_pinmux_init(mmio_region_from_addr(kPinmuxBase), &pinmux));
  CHECK_DIF_OK(dif_pinmux_output_select(&pinmux, kTopEarlgreyPinmuxMioOutIob0,
                                        kTopEarlgreyPinmuxOutselPattgenPda0Tx));
  CHECK_DIF_OK(dif_pinmux_input_select(&pinmux,
                                       kTopEarlgreyPinmuxPeripheralInGpioGpio0,
                                       kTopEarlgreyPinmuxInselIob0));
  CHECK_DIF_OK(dif_pinmux_output_select(&pinmux, kTopEarlgreyPinmuxMioOutIob1,
                                        kTopEarlgreyPinmuxOutselPattgenPcl0Tx));
  CHECK_DIF_OK(dif_pinmux_input_select(&pinmux,
                                       kTopEarlgreyPinmuxPeripheralInGpioGpio1,
                                       kTopEarlgreyPinmuxInselIob1));

  // Ensure GPIO0 and GPIO1 output enables are disabled so Pattgen drives the
  // pads cleanly.
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OE_REG_OFFSET, 0x0u);

  // 1. Aborting an active channel mid-pattern retains active_q == 1 outputs
  // and suppresses DONE interrupt (pattgen_chan.sv:59-80, 145-157).
  const uint32_t kCtrlPol0 = 1u << PATTGEN_CTRL_POLARITY_CH0_BIT;
  const uint32_t kCtrlEn0 = 1u << PATTGEN_CTRL_ENABLE_CH0_BIT;

  abs_mmio_write32(kPattgenBase + PATTGEN_CTRL_REG_OFFSET, kCtrlPol0);
  abs_mmio_write32(kPattgenBase + PATTGEN_INTR_STATE_REG_OFFSET, 0x3u);
  abs_mmio_write32(kPattgenBase + PATTGEN_PREDIV_CH0_REG_OFFSET, 50u);
  abs_mmio_write32(kPattgenBase + PATTGEN_SIZE_REG_OFFSET,
                   (7u << PATTGEN_SIZE_LEN_CH0_OFFSET) |
                       (3u << PATTGEN_SIZE_REPS_CH0_OFFSET));
  abs_mmio_write32(kPattgenBase + PATTGEN_DATA_CH0_0_REG_OFFSET, 0x1u);
  abs_mmio_write32(kPattgenBase + PATTGEN_DATA_CH0_1_REG_OFFSET, 0x0u);

  // Before first enable, active_q == 0 so PDA0 == 0 and PCL0 == 0.
  CHECK(read_pda0_pcl0() == 0x0u);

  // Enable Channel 0, then immediately disable Channel 0 before the slow
  // pattern finishes (complete_q == 0).
  abs_mmio_write32(kPattgenBase + PATTGEN_CTRL_REG_OFFSET,
                   kCtrlPol0 | kCtrlEn0);
  abs_mmio_write32(kPattgenBase + PATTGEN_CTRL_REG_OFFSET, kCtrlPol0);

  // Because complete_q == 0, active_q remains 1 in RTL (pattgen_chan.sv:145),
  // so PDA0 outputs data_q[0] == 1 (bit 0) and PCL0 outputs polarity_q == 1
  // (bit 1), even though ENABLE_CH0 == 0 and INACTIVE_LEVEL_* == 0.
  CHECK(read_pda0_pcl0() == 0x3u);

  // Wait past the pattern timer callback window and verify INTR_STATE remains 0
  // and PDA0/PCL0 remain 1.
  busy_spin_micros(2000);
  CHECK(abs_mmio_read32(kPattgenBase + PATTGEN_INTR_STATE_REG_OFFSET) == 0x0u);
  CHECK(read_pda0_pcl0() == 0x3u);

  // 2. Minimal-duration pattern completion (LEN=0, REPS=0, PREDIV=0) sets
  // complete_q == 1, clears active_q to 0, and restores INACTIVE_LEVEL (0, 0).
  abs_mmio_write32(kPattgenBase + PATTGEN_PREDIV_CH0_REG_OFFSET, 0u);
  abs_mmio_write32(kPattgenBase + PATTGEN_SIZE_REG_OFFSET, 0u);
  abs_mmio_write32(kPattgenBase + PATTGEN_DATA_CH0_0_REG_OFFSET, 0x1u);
  abs_mmio_write32(kPattgenBase + PATTGEN_CTRL_REG_OFFSET,
                   kCtrlPol0 | kCtrlEn0);

  // Poll until DONE_CH0 (bit 0) asserts in INTR_STATE.
  while ((abs_mmio_read32(kPattgenBase + PATTGEN_INTR_STATE_REG_OFFSET) &
          0x1u) == 0u) {
  }

  // Once complete_q == 1, active_q clears to 0 so PDA0 and PCL0 return to
  // INACTIVE_LEVEL (0, 0).
  CHECK(read_pda0_pcl0() == 0x0u);

  // Disable CTRL.ENABLE_CH0 and clear INTR_STATE via W1C.
  abs_mmio_write32(kPattgenBase + PATTGEN_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kPattgenBase + PATTGEN_INTR_STATE_REG_OFFSET, 0x1u);
  CHECK(abs_mmio_read32(kPattgenBase + PATTGEN_INTR_STATE_REG_OFFSET) == 0x0u);

  // 3. Unmapped offset write (0x30) inside pattgen aperture raises Store
  // Access Fault (addr_miss -> d_error).
  g_saw_store_fault = false;
  abs_mmio_write32(kPattgenBase + 0x30u, 0xdeadbeefu);
  CHECK(g_saw_store_fault);

  LOG_INFO("ot_pattgen FPGA/QEMU consistency test passed");
  return true;
}
