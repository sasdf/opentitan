// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_alert_handler.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/irq.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "gpio_regs.h"
#include "pattgen_regs.h"
#include "pinmux_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kPattgenBase = TOP_EARLGREY_PATTGEN_BASE_ADDR,
  kPinmuxBase = TOP_EARLGREY_PINMUX_AON_BASE_ADDR,
  kGpioBase = TOP_EARLGREY_GPIO_BASE_ADDR,
};

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

bool test_main(void) {
  irq_global_ctrl(false);

  // Route PATTGEN CH0 outputs (`pda0=49, pcl0=50`) to MIO pads 2..3 (`IOA2..IOA3`)
  // and route MIO pads 2..3 (`insel = 4..5`) to GPIO inputs 2..3.
  abs_mmio_write32(kGpioBase + GPIO_DIRECT_OE_REG_OFFSET, 0u);
  abs_mmio_write32(kPinmuxBase + PINMUX_MIO_OUTSEL_0_REG_OFFSET + 2u * 4u, 49u);
  abs_mmio_write32(kPinmuxBase + PINMUX_MIO_OUTSEL_0_REG_OFFSET + 3u * 4u, 50u);
  abs_mmio_write32(kPinmuxBase + PINMUX_MIO_PERIPH_INSEL_0_REG_OFFSET + 2u * 4u,
                   4u);
  abs_mmio_write32(kPinmuxBase + PINMUX_MIO_PERIPH_INSEL_0_REG_OFFSET + 3u * 4u,
                   5u);

  // 1. Chunk 1 (`ot_pattgen_timer_cb:172-174`):
  //    Arm timer by enabling CH0 (`prediv=50, len=0, reps=0` -> `dur_ns = 4.25 us`),
  //    then immediately disable CH0 before the timer expires. Per `pattgen_chan.sv`
  //    lines 75-78 & 145-156, `active_q` stays `1` when `enable` is cleared before
  //    `complete_q`, so CH0 drives `pda_o = data_q[0] = 1` and `pcl_o = polarity_q = 0`
  //    while CH1 (`active_q == 0`) drives `inactive_level_pda = 0, inactive_level_pcl = 1`.
  abs_mmio_write32(kPattgenBase + PATTGEN_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kPattgenBase + PATTGEN_INTR_STATE_REG_OFFSET, 0x3u);
  abs_mmio_write32(kPattgenBase + PATTGEN_PREDIV_CH0_REG_OFFSET, 50u);
  abs_mmio_write32(kPattgenBase + PATTGEN_DATA_CH0_0_REG_OFFSET, 1u);
  abs_mmio_write32(kPattgenBase + PATTGEN_SIZE_REG_OFFSET, 0u);
  abs_mmio_write32(kPattgenBase + PATTGEN_CTRL_REG_OFFSET,
                   1u << PATTGEN_CTRL_ENABLE_CH0_BIT);
  abs_mmio_write32(kPattgenBase + PATTGEN_CTRL_REG_OFFSET,
                   1u << PATTGEN_CTRL_INACTIVE_LEVEL_PCL_CH1_BIT);
  busy_spin_micros(50);
  CHECK(abs_mmio_read32(kPattgenBase + PATTGEN_INTR_STATE_REG_OFFSET) == 0u,
        "Disabled channel must not assert DONE_CH0 when timer fires");
  CHECK(((abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) >> 2) & 0x3u) ==
            0x1u,
        "GPIO 2..3 must reflect CH0 active_q=1 levels (pda0=data_q[0]=1, pcl0=0)");

  // Now route PATTGEN CH1 outputs (`pda1=51, pcl1=52`) to MIO pads 2..3 (`IOA2..IOA3`)
  // so `ot_pattgen_get_pin_level(2..3)` are also exercised and verified.
  abs_mmio_write32(kPinmuxBase + PINMUX_MIO_OUTSEL_0_REG_OFFSET + 2u * 4u, 51u);
  abs_mmio_write32(kPinmuxBase + PINMUX_MIO_OUTSEL_0_REG_OFFSET + 3u * 4u, 52u);
  busy_spin_micros(10);
  CHECK(((abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) >> 2) & 0x3u) ==
            0x2u,
        "GPIO 2..3 must reflect CH1 inactive levels (pda1=0, pcl1=1)");

  // 2. Chunk 4 (`ot_pattgen_write_with_attrs:369-371`):
  //    Configure CH0 with minimum duration (`prediv=0, len=0, reps=0` ->
  //    `dur_ns = 333 < 1000ULL`, clamped to `1000ULL`), enable CH0 and CH1,
  //    and verify both `DONE_CH0` and `DONE_CH1` assert.
  abs_mmio_write32(kPattgenBase + PATTGEN_PREDIV_CH0_REG_OFFSET, 0u);
  abs_mmio_write32(kPattgenBase + PATTGEN_PREDIV_CH1_REG_OFFSET, 0u);
  abs_mmio_write32(kPattgenBase + PATTGEN_DATA_CH0_0_REG_OFFSET, 0x1u);
  abs_mmio_write32(kPattgenBase + PATTGEN_DATA_CH0_1_REG_OFFSET, 0x0u);
  abs_mmio_write32(kPattgenBase + PATTGEN_DATA_CH1_0_REG_OFFSET, 0x2u);
  abs_mmio_write32(kPattgenBase + PATTGEN_DATA_CH1_1_REG_OFFSET, 0x0u);
  abs_mmio_write32(kPattgenBase + PATTGEN_SIZE_REG_OFFSET, 0u);
  abs_mmio_write32(kPattgenBase + PATTGEN_INTR_ENABLE_REG_OFFSET, 0x3u);
  abs_mmio_write32(kPattgenBase + PATTGEN_CTRL_REG_OFFSET,
                   (1u << PATTGEN_CTRL_ENABLE_CH0_BIT) |
                       (1u << PATTGEN_CTRL_ENABLE_CH1_BIT) |
                       (1u << PATTGEN_CTRL_POLARITY_CH0_BIT) |
                       (1u << PATTGEN_CTRL_INACTIVE_LEVEL_PDA_CH0_BIT) |
                       (1u << PATTGEN_CTRL_INACTIVE_LEVEL_PCL_CH0_BIT));
  busy_spin_micros(50);
  CHECK(abs_mmio_read32(kPattgenBase + PATTGEN_INTR_STATE_REG_OFFSET) == 0x3u,
        "Minimum-duration CH0+CH1 pattern generation must set DONE_CH0|DONE_CH1");
  abs_mmio_write32(kPattgenBase + PATTGEN_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kPattgenBase + PATTGEN_INTR_STATE_REG_OFFSET, 0x3u);
  abs_mmio_write32(kPattgenBase + PATTGEN_INTR_ENABLE_REG_OFFSET, 0u);

  // 3. Chunk 2 (`ot_pattgen_read_with_attrs:278-281`) and
  //    Chunk 3 / Chunk 5 (`ot_pattgen_write_with_attrs:385-388`):
  //    Out-of-range offset (`0x30`) within the 0x40-byte PATTGEN window must
  //    raise Load Access Fault (`mcause == 5`) on read and Store Access Fault
  //    (`mcause == 7`) on write.
  g_fault_count = 0;
  (void)*(volatile uint32_t *)(kPattgenBase + 0x30u);
  CHECK(g_fault_count == 1u && g_last_mcause == 5u,
        "lw from PATTGEN+0x30 (addrmiss) must raise Load Access Fault (mcause=5)");

  *(volatile uint32_t *)(kPattgenBase + 0x30u) = 0xdeadbeefu;
  CHECK(g_fault_count == 2u && g_last_mcause == 7u,
        "sw to PATTGEN+0x30 (addrmiss) must raise Store Access Fault (mcause=7)");

  // Also verify sub-word write fault on 4-byte register `PREDIV_CH0` (`0x14`)
  *(volatile uint8_t *)(kPattgenBase + PATTGEN_PREDIV_CH0_REG_OFFSET) = 0x11u;
  CHECK(g_fault_count == 3u && g_last_mcause == 7u,
        "sb to PATTGEN_PREDIV_CH0 must raise Store Access Fault (mcause=7)");

  // Verify W/O registers (`INTR_TEST`, `ALERT_TEST`) read as 0
  CHECK(abs_mmio_read32(kPattgenBase + PATTGEN_INTR_TEST_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kPattgenBase + PATTGEN_ALERT_TEST_REG_OFFSET) == 0u);

  return true;
}
