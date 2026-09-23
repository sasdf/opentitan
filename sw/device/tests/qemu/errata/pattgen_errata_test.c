// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file pattgen_errata_test.c
 * @brief CW340 FPGA & QEMU Hardware Errata Confirmation Test for `pattgen`
 * (`P12`).
 *
 * Empirically confirms the taped-out OpenTitan Earlgrey `pattgen` silicon
 * errata, specification errata, and security hardening behaviors on physical
 * CW340 FPGA silicon and QEMU (`sim_qemu`):
 *
 * - `[pattgen_chan.sv:49-67]` (`SPEC_DOC_ERRATA` — `LOW` / `HIGH (95%)`):
 *   1. `pattgen_chan.sv:55-57` freezes operational shadow flops
 *      `inactive_level_pcl_q` and `inactive_level_pda_q` while `ENABLE_CHx = 1`
 *      (even after pattern completion `complete_q = 1`), while MMIO CSR writes
 *      to `CTRL`, `DATA_CHx`, and `SIZE` in `pattgen_reg_top.sv` immediately
 *      update CSR readback.
 *   2. At pattern completion (`END`, `complete_q = 1`, `active = 0`), `pda_o`
 *      outputs `inactive_level_pda_q` (`1` when `INACTIVE_LEVEL_PDA_CHx = 1`),
 *      contradicting `theory_of_operation.md:52` ("the pda data lines reset to
 *      zero").
 *
 * - `[pattgen_chan.sv:145-150]` (`TRUE_SILICON_ERRATA` — `MEDIUM` / `HIGH
 * (90%)`): Disabling a channel mid-pattern (`CTRL.ENABLE_CHx = 0` while
 *   `complete_q == 0`) fails to clear `active_q` because
 *   `pattgen_chan.sv:145-150` defines
 *   `active_d = complete_q ? 1'b0 : enable ? 1'b1 : active_q`. This leaves
 *   `active == 1` while disabled so `pcl_o`/`pda_o` stay stuck driving
 *   `polarity_q` and `data_q[0]` instead of `INACTIVE_LEVEL_*` until a pattern
 *   is allowed to reach `complete_q == 1`.
 *
 * - `[pattgen_reg_pkg.sv:180-193]` (`INTENDED_SECURITY_HARDENING` — `INFO`):
 *   `PATTGEN_PERMIT[11] = 4'b1111` (`pattgen_reg_pkg.sv:180-193`,
 *   `pattgen_reg_top.sv:875-889`) rejects 16-bit half-word writes (`sh`) to
 *   the shared `SIZE` (`0x2c`) register (Channel 0 at `[15:0]`, Channel 1 at
 *   `[31:16]`) with a synchronous Store Access Fault (`mcause = 7`), whereas
 *   `CTRL` (`0x10`, `PERMIT = 4'b0001`) accepts byte writes (`sb`).
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
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

static volatile uint32_t g_store_fault_count = 0;
static volatile uint32_t g_last_mcause = 0;

void ottf_exception_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = ibex_mcause_read();
  g_last_mcause = mcause;
  if (mcause == kIbexExcStoreAccessFault) {
    g_store_fault_count++;
    return;
  }
  CHECK(false, "Unexpected exception mcause=0x%08x", mcause);
}

static uint32_t read_pda0_pcl0(void) {
  return abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) & 0x3u;
}

static void setup_pinmux_loopback(void) {
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
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OE_REG_OFFSET, 0x0u);
}

static void test_inactive_level_freeze_and_end_pda(void) {
  LOG_INFO(
      "Verifying [pattgen_chan.sv:49-67] (SPEC_DOC_ERRATA): "
      "INACTIVE_LEVEL_* shadow freeze while ENABLE_CH0=1 & PDA at END...");

  const uint32_t kEn0 = 1u << PATTGEN_CTRL_ENABLE_CH0_BIT;
  const uint32_t kInactPcl0 = 1u << PATTGEN_CTRL_INACTIVE_LEVEL_PCL_CH0_BIT;
  const uint32_t kInactPda0 = 1u << PATTGEN_CTRL_INACTIVE_LEVEL_PDA_CH0_BIT;

  // Configure Channel 0 while disabled (ENABLE_CH0 = 0) with INACTIVE_LEVEL
  // PCL0 = 1, PDA0 = 1, and a fast 1-bit pattern (DATA_CH0_0 = 0).
  abs_mmio_write32(kPattgenBase + PATTGEN_CTRL_REG_OFFSET,
                   kInactPcl0 | kInactPda0);
  abs_mmio_write32(kPattgenBase + PATTGEN_INTR_STATE_REG_OFFSET, 0x3u);
  abs_mmio_write32(kPattgenBase + PATTGEN_PREDIV_CH0_REG_OFFSET, 0u);
  abs_mmio_write32(kPattgenBase + PATTGEN_SIZE_REG_OFFSET, 0u);
  abs_mmio_write32(kPattgenBase + PATTGEN_DATA_CH0_0_REG_OFFSET, 0u);
  abs_mmio_write32(kPattgenBase + PATTGEN_DATA_CH0_1_REG_OFFSET, 0u);
  CHECK(read_pda0_pcl0() == 0x3u);

  // Enable Channel 0 and wait for pattern completion (DONE_CH0, complete_q=1).
  abs_mmio_write32(kPattgenBase + PATTGEN_CTRL_REG_OFFSET,
                   kEn0 | kInactPcl0 | kInactPda0);
  while ((abs_mmio_read32(kPattgenBase + PATTGEN_INTR_STATE_REG_OFFSET) &
          0x1u) == 0u) {
  }

  // 1. Verify at END (complete_q == 1, active == 0), pda0 outputs
  // inactive_level_pda_q (1), NOT 0 (contradicting theory_of_operation.md:52).
  CHECK(read_pda0_pcl0() == 0x3u);

  // 2. While ENABLE_CH0 remains 1, write new values to CTRL (clearing
  // INACTIVE_LEVEL_PCL_CH0=0, INACTIVE_LEVEL_PDA_CH0=0), DATA_CH0_0, and SIZE.
  abs_mmio_write32(kPattgenBase + PATTGEN_CTRL_REG_OFFSET, kEn0);
  abs_mmio_write32(kPattgenBase + PATTGEN_DATA_CH0_0_REG_OFFSET, 0xa5a5a5a5u);
  abs_mmio_write32(kPattgenBase + PATTGEN_SIZE_REG_OFFSET, 0x00150005u);

  // Confirm two-tier behavior: MMIO CSR readbacks immediately reflect the new
  // values, while operational shadow flops inactive_level_*_q remain frozen at
  // 1 (read_pda0_pcl0() == 0x3u) until ENABLE_CH0 is cleared to 0!
  CHECK(abs_mmio_read32(kPattgenBase + PATTGEN_CTRL_REG_OFFSET) == kEn0);
  CHECK(abs_mmio_read32(kPattgenBase + PATTGEN_DATA_CH0_0_REG_OFFSET) ==
        0xa5a5a5a5u);
  CHECK(abs_mmio_read32(kPattgenBase + PATTGEN_SIZE_REG_OFFSET) == 0x00150005u);
  CHECK(read_pda0_pcl0() == 0x3u);

  // Clearing ENABLE_CH0 = 0 unfreezes inactive_level_*_q and drives 0 on both
  // PDA0 and PCL0.
  abs_mmio_write32(kPattgenBase + PATTGEN_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kPattgenBase + PATTGEN_INTR_STATE_REG_OFFSET, 0x3u);
  CHECK(read_pda0_pcl0() == 0x0u);
  LOG_INFO(
      "[pattgen_chan.sv:49-67] confirmed: INACTIVE_LEVEL_* frozen while "
      "ENABLE_CH0=1 and PDA outputs inactive_level_pda_q at END.");
}

static void test_mid_pattern_disable_active_q_stuck(void) {
  LOG_INFO(
      "Verifying [pattgen_chan.sv:145-150] (TRUE_SILICON_ERRATA): clearing "
      "ENABLE_CH0 mid-pattern (complete_q==0) leaves active_q stuck at "
      "1...");

  const uint32_t kPol0 = 1u << PATTGEN_CTRL_POLARITY_CH0_BIT;
  const uint32_t kEn0 = 1u << PATTGEN_CTRL_ENABLE_CH0_BIT;

  abs_mmio_write32(kPattgenBase + PATTGEN_CTRL_REG_OFFSET, kPol0);
  abs_mmio_write32(kPattgenBase + PATTGEN_INTR_STATE_REG_OFFSET, 0x3u);
  abs_mmio_write32(kPattgenBase + PATTGEN_PREDIV_CH0_REG_OFFSET, 50u);
  abs_mmio_write32(kPattgenBase + PATTGEN_SIZE_REG_OFFSET,
                   (7u << PATTGEN_SIZE_LEN_CH0_OFFSET) |
                       (3u << PATTGEN_SIZE_REPS_CH0_OFFSET));
  abs_mmio_write32(kPattgenBase + PATTGEN_DATA_CH0_0_REG_OFFSET, 0x1u);
  abs_mmio_write32(kPattgenBase + PATTGEN_DATA_CH0_1_REG_OFFSET, 0x0u);
  CHECK(read_pda0_pcl0() == 0x0u);

  // Start Channel 0 and immediately disable it mid-pattern (complete_q == 0).
  abs_mmio_write32(kPattgenBase + PATTGEN_CTRL_REG_OFFSET, kPol0 | kEn0);
  abs_mmio_write32(kPattgenBase + PATTGEN_CTRL_REG_OFFSET, kPol0);

  // Because active_d = complete_q ? 0 : enable ? 1 : active_q, active_q remains
  // 1 while disabled, driving PDA0 = data_q[0] (1) and PCL0 = polarity_q (1)
  // instead of INACTIVE_LEVEL (0, 0)!
  CHECK(read_pda0_pcl0() == 0x3u);
  busy_spin_micros(2000);
  CHECK(abs_mmio_read32(kPattgenBase + PATTGEN_INTR_STATE_REG_OFFSET) == 0u);
  CHECK(read_pda0_pcl0() == 0x3u);

  // Verify workaround: configure minimal 1-bit pattern (PREDIV=0, SIZE=0) and
  // enable Channel 0 so complete_q reaches 1 and clears active_q -> 0.
  abs_mmio_write32(kPattgenBase + PATTGEN_PREDIV_CH0_REG_OFFSET, 0u);
  abs_mmio_write32(kPattgenBase + PATTGEN_SIZE_REG_OFFSET, 0u);
  abs_mmio_write32(kPattgenBase + PATTGEN_CTRL_REG_OFFSET, kPol0 | kEn0);
  while ((abs_mmio_read32(kPattgenBase + PATTGEN_INTR_STATE_REG_OFFSET) &
          0x1u) == 0u) {
  }
  CHECK(read_pda0_pcl0() == 0x0u);
  abs_mmio_write32(kPattgenBase + PATTGEN_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kPattgenBase + PATTGEN_INTR_STATE_REG_OFFSET, 0x3u);
  LOG_INFO(
      "[pattgen_chan.sv:145-150] confirmed: mid-pattern disable leaves "
      "active_q==1 (PDA0/PCL0=0x3); completion pulse clears active_q.");
}

static void test_size_subword_write_bus_fault(void) {
  LOG_INFO(
      "Verifying [pattgen_reg_pkg.sv:180-193] (INTENDED_SECURITY_HARDENING): "
      "16-bit half-word writes to shared SIZE (0x2c) fault with "
      "mcause=7...");

  g_store_fault_count = 0;
  g_last_mcause = 0;

  // 1-byte write to CTRL (0x10, PATTGEN_PERMIT[4] = 4'b0001) must succeed.
  abs_mmio_write8(kPattgenBase + PATTGEN_CTRL_REG_OFFSET, 0x0u);
  CHECK(g_store_fault_count == 0u);

  // Full 32-bit write to SIZE (0x2c) succeeds.
  const uint32_t kExpectedSize = 0x00040002u;
  abs_mmio_write32(kPattgenBase + PATTGEN_SIZE_REG_OFFSET, kExpectedSize);
  CHECK(abs_mmio_read32(kPattgenBase + PATTGEN_SIZE_REG_OFFSET) ==
        kExpectedSize);

  // 16-bit write to Channel 0 half-word (SIZE + 0x0) must fault (mcause = 7)
  // and preserve SIZE.
  *((volatile uint16_t *)(kPattgenBase + PATTGEN_SIZE_REG_OFFSET)) = 0x00ffu;
  CHECK(g_store_fault_count == 1u);
  CHECK(g_last_mcause == kIbexExcStoreAccessFault);
  CHECK(abs_mmio_read32(kPattgenBase + PATTGEN_SIZE_REG_OFFSET) ==
        kExpectedSize);

  // 16-bit write to Channel 1 half-word (SIZE + 0x2) must also fault (mcause=7)
  // and preserve SIZE.
  *((volatile uint16_t *)(kPattgenBase + PATTGEN_SIZE_REG_OFFSET + 2u)) =
      0x00ffu;
  CHECK(g_store_fault_count == 2u);
  CHECK(g_last_mcause == kIbexExcStoreAccessFault);
  CHECK(abs_mmio_read32(kPattgenBase + PATTGEN_SIZE_REG_OFFSET) ==
        kExpectedSize);

  LOG_INFO(
      "[pattgen_reg_pkg.sv:180-193] confirmed: 16-bit half-word writes to "
      "shared "
      "SIZE fault with mcause=7 (faults=%u).",
      g_store_fault_count);
}

bool test_main(void) {
  LOG_INFO("Starting pattgen hardware errata confirmation suite...");
  setup_pinmux_loopback();
  test_inactive_level_freeze_and_end_pda();
  test_mid_pattern_disable_active_q_stuck();
  test_size_subword_write_bus_fault();
  LOG_INFO("All pattgen errata checks confirmed on hardware!");
  return true;
}
