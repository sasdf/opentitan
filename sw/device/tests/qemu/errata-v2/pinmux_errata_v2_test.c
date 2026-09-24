// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file pinmux_errata_v2_test.c
 * @brief Earlgrey v2 (trunk-v2) hardware, spec, and DIF errata verification
 * test for `pinmux` on the physical CW340 FPGA.
 *
 * Verifies:
 * 1. `pinmux_wkup.sv:48-78` vs. `pinmux.hjson:1054-1067`: In `TimedHigh` (`3`)
 *    and `TimedLow` (`4`) wakeup modes, `aon_wkup_pulse_o` is assigned
 *    `cnt_eq_th` (`cnt_q >= wkup_cnt_th_i`) without gating by `cnt_en`
 *    (`filter_out_d` / `~filter_out_d`). Because `cnt_q` and
 *    `WKUP_DETECTOR_CNT_TH_0..7` reset to `0`, enabling `WKUP_DETECTOR_EN = 1`
 *    when `CNT_TH == 0` immediately latches `WKUP_CAUSE = 1` even when the
 *    selected pin is `ConstantZero` (`0`) in `TimedHigh` mode or
 *    `ConstantOne` (`1`) in `TimedLow` mode.
 * 2. `pinmux_wkup.sv:28-89`, `pinmux.sv:589-623`, and
 *    `prim_xilinx_pad_attr.sv:30-56`: Continuous 4-cycle AON filter
 *    (`u_prim_filter`) and `filter_out_q` edge-flop update while
 *    `WKUP_DETECTOR_EN == 0`, MIO (`+2` offset `{mio_wkup_no_scan, 1'b1,
 * 1'b0}`) vs. DIO (`0` offset) `PADSEL` indexing, and `MIO_PAD_ATTR` /
 *    `DIO_PAD_ATTR` WARL masks (`0x83` for BidirStd/BidirOd pads including
 *    DIO `10..11`, `0x81` for InputStd DIO `12..13`).
 * 3. `pinmux_reg_pkg.sv:2127-2696` and `pinmux_reg_top.sv:33108-33770`:
 *    `PINMUX_PERMIT[568]` enforces `4'b0111` (3 bytes required) on all 47
 *    `MIO_PAD_ATTR` and 16 `DIO_PAD_ATTR` CSRs despite the 1-byte (`0x83` /
 *    `0x81`) CW340 FPGA WARL mask, causing 8-bit (`sb`) and 16-bit (`sh`)
 *    stores to raise a synchronous TL-UL Store Access Fault (`d_error = 1`,
 *    `mcause = 7`).
 * 4. [NEW IN V2] `sw/device/lib/dif/dif_pinmux.c:41-76,500-536` and
 *    `pinmux_reg_pkg.sv:10` / `pinmux.sv:489-501`:
 *    a) `dif_pinmux_get_sleep_status_bit` computes
 *       `*reg_offset = (ptrdiff_t)index / 32 + reg_base` (missing
 *       `* sizeof(uint32_t)`). For MIO pads `32..46` (`index / 32 == 1`),
 *       `*reg_offset` evaluates to the unaligned byte offset `0x451` instead
 *       of `0x454` (`PINMUX_MIO_PAD_SLEEP_STATUS_1_REG_OFFSET`), which
 *       `mmio_region_read32`/`mmio_region_write32` integer division
 *       (`0x451 / 4 = 0x114`) silently truncates back to `0x450`
 *       (`PINMUX_MIO_PAD_SLEEP_STATUS_0_REG_OFFSET`), aliasing `MIO32..46`
 *       onto `MIO0..14` while leaving `MIO_PAD_SLEEP_STATUS_1` (`0x454`)
 *       unreachable via DIF (and faulting with `mcause = 7` on raw unaligned
 *       `0x451` stores).
 *    b) `NMioPeriphOut` decreased from `75` (in v1) to `64` (in v2
 *       `pinmux_reg_pkg.sv:10`), causing `dif_pinmux_output_select`
 *       (`dif_pinmux.c:186`) to reject `outsel >= 67` (`3 + 64`) with
 *       `kDifBadArg`, whereas `MIO_OUTSEL_0..46` (`0x1D0..0x28B`) in
 *       `pinmux_reg_top.sv` remains a 7-bit `prim_subreg` (`[6:0]`, `0..127`)
 *       with no WARL clamp at `66` that stores and reads back `67..127` while
 *       driving `mio_out = 0`, `mio_oe = 0` (High-Z) via the zero-extended
 *       upper entries `[127:67]` of `periph_data_mux` / `periph_oe_mux`.
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_pinmux.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top/gpio_regs.h"
#include "hw/top/pinmux_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kPinmuxBase = TOP_EARLGREY_PINMUX_BASE_ADDR,
  kGpioBase = TOP_EARLGREY_GPIO_BASE_ADDR,
  kTestDetectorIdx = 7,
  kTestMioPadIdx = 46,
  kTestDioInputStdIdx = 12,
};

static volatile bool g_expect_bus_fault = false;
static volatile uint32_t g_bus_fault_count = 0;
static volatile uint32_t g_last_mcause = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  OT_DISCARD(exc_info);
  uint32_t mcause = ibex_mcause_read();
  if (g_expect_bus_fault) {
    g_bus_fault_count++;
    g_last_mcause = mcause;
    return;
  }
  CHECK(false, "Unexpected load/store fault mcause=0x%08x", mcause);
}

/**
 * Test 1 (Part A - v1 Erratum 003 on v2):
 * Verify `TimedHigh` (`3`) and `TimedLow` (`4`) wakeup modes output ungated
 * `cnt_eq_th` (`cnt_q >= wkup_cnt_th_i`), immediately latching `WKUP_CAUSE = 1`
 * regardless of pin polarity when `WKUP_DETECTOR_CNT_TH == 0`
 * (`pinmux_wkup.sv:48-78`).
 */
static void test_wkup_timed_cnt_th_zero_hazard(void) {
  LOG_INFO("Test 1: TimedHigh/TimedLow ungated cnt_eq_th when CNT_TH == 0");

  const uint32_t en_off =
      PINMUX_WKUP_DETECTOR_EN_0_REG_OFFSET + 4u * kTestDetectorIdx;
  const uint32_t cfg_off =
      PINMUX_WKUP_DETECTOR_0_REG_OFFSET + 4u * kTestDetectorIdx;
  const uint32_t th_off =
      PINMUX_WKUP_DETECTOR_CNT_TH_0_REG_OFFSET + 4u * kTestDetectorIdx;
  const uint32_t padsel_off =
      PINMUX_WKUP_DETECTOR_PADSEL_0_REG_OFFSET + 4u * kTestDetectorIdx;
  const uint32_t cause_off = PINMUX_WKUP_CAUSE_REG_OFFSET;
  const uint32_t cause_bit = (1u << kTestDetectorIdx);

  // Disable detector 7, select ConstantZero (PADSEL = 0, MIODIO = 0), set
  // MODE = TimedHigh (3) and CNT_TH = 0.
  abs_mmio_write32(kPinmuxBase + en_off, 0u);
  abs_mmio_write32(kPinmuxBase + padsel_off, 0u);
  abs_mmio_write32(kPinmuxBase + th_off, 0u);
  abs_mmio_write32(kPinmuxBase + cfg_off,
                   PINMUX_WKUP_DETECTOR_0_MODE_0_VALUE_TIMEDHIGH);
  busy_spin_micros(30);
  abs_mmio_write32(kPinmuxBase + cause_off, 0u);
  CHECK((abs_mmio_read32(kPinmuxBase + cause_off) & cause_bit) == 0u,
        "Expected WKUP_CAUSE cleared while detector disabled");

  // Enable detector 7 while pin is ConstantZero (0) and MODE is TimedHigh (3)
  // with CNT_TH == 0. Because `cnt_eq_th = (0 >= 0) == 1` is not gated by
  // `cnt_en` (`filter_out_d == 0`), `WKUP_CAUSE` immediately latches 1!
  abs_mmio_write32(kPinmuxBase + en_off, 1u);
  busy_spin_micros(30);
  CHECK(
      (abs_mmio_read32(kPinmuxBase + cause_off) & cause_bit) != 0u,
      "Expected TimedHigh with CNT_TH=0 on ConstantZero to latch WKUP_CAUSE=1");

  // Conversely, when CNT_TH = 2 (>= 1) on ConstantZero in TimedHigh mode,
  // `cnt_q` stays 0 (`0 >= 2 == 0`), so no false wakeup fires.
  abs_mmio_write32(kPinmuxBase + en_off, 0u);
  abs_mmio_write32(kPinmuxBase + th_off, 2u);
  busy_spin_micros(30);
  abs_mmio_write32(kPinmuxBase + cause_off, 0u);
  abs_mmio_write32(kPinmuxBase + en_off, 1u);
  busy_spin_micros(30);
  CHECK((abs_mmio_read32(kPinmuxBase + cause_off) & cause_bit) == 0u,
        "Expected TimedHigh with CNT_TH=2 on ConstantZero NOT to fire");

  // Repeat for MODE = TimedLow (4) with PADSEL = 1 (ConstantOne):
  // When CNT_TH = 0, `cnt_eq_th = (0 >= 0) == 1` immediately latches
  // `WKUP_CAUSE = 1` even though the pin is ConstantOne (1); when CNT_TH = 2,
  // `cnt_q` stays 0 (`0 >= 2 == 0`), so `WKUP_CAUSE` stays 0.
  abs_mmio_write32(kPinmuxBase + en_off, 0u);
  abs_mmio_write32(kPinmuxBase + padsel_off, 1u);
  abs_mmio_write32(kPinmuxBase + th_off, 0u);
  abs_mmio_write32(kPinmuxBase + cfg_off,
                   PINMUX_WKUP_DETECTOR_0_MODE_0_VALUE_TIMEDLOW);
  busy_spin_micros(30);
  abs_mmio_write32(kPinmuxBase + cause_off, 0u);
  abs_mmio_write32(kPinmuxBase + en_off, 1u);
  busy_spin_micros(30);
  CHECK((abs_mmio_read32(kPinmuxBase + cause_off) & cause_bit) != 0u,
        "Expected TimedLow with CNT_TH=0 on ConstantOne to latch WKUP_CAUSE=1");

  abs_mmio_write32(kPinmuxBase + en_off, 0u);
  abs_mmio_write32(kPinmuxBase + th_off, 2u);
  busy_spin_micros(30);
  abs_mmio_write32(kPinmuxBase + cause_off, 0u);
  abs_mmio_write32(kPinmuxBase + en_off, 1u);
  busy_spin_micros(30);
  CHECK((abs_mmio_read32(kPinmuxBase + cause_off) & cause_bit) == 0u,
        "Expected TimedLow with CNT_TH=2 on ConstantOne NOT to fire");

  abs_mmio_write32(kPinmuxBase + en_off, 0u);
  abs_mmio_write32(kPinmuxBase + cause_off, 0u);
}

/**
 * Test 2 (Part A - v1 Erratum 001 on v2):
 * Verify continuous `filter_out_q` tracking while `WKUP_DETECTOR_EN == 0`,
 * MIO `PADSEL` (`0 = ConstantZero`, `1 = ConstantOne`) edge generation, and
 * `MIO_PAD_ATTR` / `DIO_PAD_ATTR` WARL masks (`0x83` vs `0x81`).
 */
static void test_filter_tracking_and_pad_attr_warl(void) {
  LOG_INFO("Test 2: Continuous filter_out_q tracking and PAD_ATTR WARL masks");

  const uint32_t en_off =
      PINMUX_WKUP_DETECTOR_EN_0_REG_OFFSET + 4u * kTestDetectorIdx;
  const uint32_t cfg_off =
      PINMUX_WKUP_DETECTOR_0_REG_OFFSET + 4u * kTestDetectorIdx;
  const uint32_t padsel_off =
      PINMUX_WKUP_DETECTOR_PADSEL_0_REG_OFFSET + 4u * kTestDetectorIdx;
  const uint32_t cause_off = PINMUX_WKUP_CAUSE_REG_OFFSET;
  const uint32_t cause_bit = (1u << kTestDetectorIdx);

  // Set PADSEL = 1 (ConstantOne) while WKUP_DETECTOR_EN == 0 and wait 30 us
  // (~6 AON cycles) so `filter_out_q` settles to 1 while disabled.
  abs_mmio_write32(kPinmuxBase + en_off, 0u);
  abs_mmio_write32(kPinmuxBase + cfg_off,
                   PINMUX_WKUP_DETECTOR_0_MODE_0_VALUE_POSEDGE);
  abs_mmio_write32(kPinmuxBase + padsel_off, 1u);
  busy_spin_micros(30);
  abs_mmio_write32(kPinmuxBase + cause_off, 0u);

  // Enabling WKUP_DETECTOR_EN = 1 now must NOT fire Posedge because
  // `filter_out_q` already tracked `filter_out_d == 1` while disabled.
  abs_mmio_write32(kPinmuxBase + en_off, 1u);
  busy_spin_micros(30);
  CHECK((abs_mmio_read32(kPinmuxBase + cause_off) & cause_bit) == 0u,
        "Expected no Posedge wakeup when ConstantOne settled before EN=1");

  // Switching PADSEL 0 -> 1 while EN == 1 fires Posedge after 1 AON cycle.
  abs_mmio_write32(kPinmuxBase + padsel_off, 0u);
  busy_spin_micros(30);
  abs_mmio_write32(kPinmuxBase + padsel_off, 1u);
  busy_spin_micros(30);
  CHECK((abs_mmio_read32(kPinmuxBase + cause_off) & cause_bit) != 0u,
        "Expected Posedge wakeup on PADSEL 0 -> 1 transition");

  // Also verify the +2 MIO PADSEL indexing (`{mio_wkup_no_scan, 1'b1, 1'b0}`):
  // MIO46 is at PADSEL = 46 + 2 = 48. Drive MIO_OUTSEL_46 = 0 (ConstantZero)
  // -> 1 (ConstantOne) and wait 100 us (20 AON cycles for 4-cycle u_prim_filter
  // + u_wkup_cause CDC) to verify WKUP_CAUSE latches 1.
  const uint32_t outsel46_off =
      PINMUX_MIO_OUTSEL_0_REG_OFFSET + 4u * kTestMioPadIdx;
  uint32_t orig_outsel46 = abs_mmio_read32(kPinmuxBase + outsel46_off);
  abs_mmio_write32(kPinmuxBase + en_off, 0u);
  abs_mmio_write32(kPinmuxBase + outsel46_off,
                   kTopEarlgreyPinmuxOutselConstantZero);
  abs_mmio_write32(kPinmuxBase + padsel_off, kTestMioPadIdx + 2u);
  busy_spin_micros(100);
  abs_mmio_write32(kPinmuxBase + cause_off, 0u);
  abs_mmio_write32(kPinmuxBase + en_off, 1u);
  busy_spin_micros(100);
  CHECK((abs_mmio_read32(kPinmuxBase + cause_off) & cause_bit) == 0u,
        "Expected WKUP_CAUSE==0 while MIO46 (PADSEL=48) is ConstantZero");
  abs_mmio_write32(kPinmuxBase + outsel46_off,
                   kTopEarlgreyPinmuxOutselConstantOne);
  busy_spin_micros(100);
  CHECK((abs_mmio_read32(kPinmuxBase + cause_off) & cause_bit) != 0u,
        "Expected Posedge wakeup on MIO46 (PADSEL=48) 0 -> 1 transition");
  abs_mmio_write32(kPinmuxBase + outsel46_off, orig_outsel46);

  abs_mmio_write32(kPinmuxBase + en_off, 0u);
  abs_mmio_write32(kPinmuxBase + cause_off, 0u);

  // Verify WARL masks on CW340 FPGA (`prim_xilinx_pad_attr.sv:30-56`):
  // MIO46 (`BidirStd`) retains `0x83` (`input_disable[7]`, `virtual_od_en[1]`,
  // `invert[0]`), while DIO12 (`InputStd`) retains `0x81` (`input_disable[7]`,
  // `invert[0]`).
  const uint32_t mio_attr_off =
      PINMUX_MIO_PAD_ATTR_0_REG_OFFSET + 4u * kTestMioPadIdx;
  const uint32_t dio_attr_off =
      PINMUX_DIO_PAD_ATTR_0_REG_OFFSET + 4u * kTestDioInputStdIdx;
  uint32_t orig_mio = abs_mmio_read32(kPinmuxBase + mio_attr_off);
  uint32_t orig_dio = abs_mmio_read32(kPinmuxBase + dio_attr_off);

  abs_mmio_write32(kPinmuxBase + mio_attr_off, 0xFFFFFFFFu);
  CHECK(abs_mmio_read32(kPinmuxBase + mio_attr_off) == 0x00000083u,
        "Expected MIO_PAD_ATTR WARL mask 0x83");
  abs_mmio_write32(kPinmuxBase + mio_attr_off, orig_mio);

  abs_mmio_write32(kPinmuxBase + dio_attr_off, 0xFFFFFFFFu);
  CHECK(abs_mmio_read32(kPinmuxBase + dio_attr_off) == 0x00000081u,
        "Expected InputStd DIO_PAD_ATTR_12 WARL mask 0x81");
  abs_mmio_write32(kPinmuxBase + dio_attr_off, orig_dio);
}

/**
 * Test 3 (Part A - v1 Erratum 002 on v2):
 * Verify `PINMUX_PERMIT = 4'b0111` rejects 8-bit (`sb`) and 16-bit (`sh`)
 * stores to `MIO_PAD_ATTR` and `DIO_PAD_ATTR` with Store Access Fault
 * (`mcause = 7`), despite the 1-byte (`0x83` / `0x81`) CW340 FPGA WARL mask.
 */
static void test_pad_attr_subword_store_faults(void) {
  LOG_INFO("Test 3: PINMUX_PERMIT=4'b0111 sub-word store fault on PAD_ATTR");

  const uint32_t mio_attr_off =
      PINMUX_MIO_PAD_ATTR_0_REG_OFFSET + 4u * kTestMioPadIdx;
  const uint32_t dio_attr_off =
      PINMUX_DIO_PAD_ATTR_0_REG_OFFSET + 4u * kTestDioInputStdIdx;
  uint32_t orig_mio = abs_mmio_read32(kPinmuxBase + mio_attr_off);
  uint32_t orig_dio = abs_mmio_read32(kPinmuxBase + dio_attr_off);
  abs_mmio_write32(kPinmuxBase + mio_attr_off, 0u);
  abs_mmio_write32(kPinmuxBase + dio_attr_off, 0u);

  uint32_t prev = g_bus_fault_count;
  g_expect_bus_fault = true;
  abs_mmio_write8(kPinmuxBase + mio_attr_off, 0x01u);
  g_expect_bus_fault = false;
  CHECK(g_bus_fault_count == prev + 1 && g_last_mcause == 7,
        "Expected sb to MIO_PAD_ATTR to fault with mcause=7");
  CHECK(abs_mmio_read32(kPinmuxBase + mio_attr_off) == 0u,
        "Faulting sb must not modify MIO_PAD_ATTR");

  prev = g_bus_fault_count;
  g_expect_bus_fault = true;
  uintptr_t addr = kPinmuxBase + mio_attr_off;
  uint32_t half_val = 0x0001u;
  asm volatile("sh %0, 0(%1)" ::"r"(half_val), "r"(addr) : "memory");
  g_expect_bus_fault = false;
  CHECK(g_bus_fault_count == prev + 1 && g_last_mcause == 7,
        "Expected sh to MIO_PAD_ATTR to fault with mcause=7");
  CHECK(abs_mmio_read32(kPinmuxBase + mio_attr_off) == 0u,
        "Faulting sh must not modify MIO_PAD_ATTR");

  prev = g_bus_fault_count;
  g_expect_bus_fault = true;
  abs_mmio_write8(kPinmuxBase + dio_attr_off, 0x01u);
  g_expect_bus_fault = false;
  CHECK(g_bus_fault_count == prev + 1 && g_last_mcause == 7,
        "Expected sb to DIO_PAD_ATTR_12 to fault with mcause=7");
  CHECK(abs_mmio_read32(kPinmuxBase + dio_attr_off) == 0u,
        "Faulting sb must not modify DIO_PAD_ATTR_12");

  prev = g_bus_fault_count;
  g_expect_bus_fault = true;
  uintptr_t dio_addr = kPinmuxBase + dio_attr_off;
  asm volatile("sh %0, 0(%1)" ::"r"(half_val), "r"(dio_addr) : "memory");
  g_expect_bus_fault = false;
  CHECK(g_bus_fault_count == prev + 1 && g_last_mcause == 7,
        "Expected sh to DIO_PAD_ATTR_12 to fault with mcause=7");
  CHECK(abs_mmio_read32(kPinmuxBase + dio_attr_off) == 0u,
        "Faulting sh must not modify DIO_PAD_ATTR_12");

  abs_mmio_write32(kPinmuxBase + mio_attr_off, orig_mio);
  abs_mmio_write32(kPinmuxBase + dio_attr_off, orig_dio);
}

/**
 * Test 4 (Part B - NEW IN V2):
 * Verify:
 * 1. `dif_pinmux_get_sleep_status_bit` (`sw/device/lib/dif/dif_pinmux.c:73`)
 *    computes `*reg_offset = (ptrdiff_t)index / 32 + reg_base = 0x450 + 1 =
 *    0x451` for `pad = 32..46` (`MIO32..MIO46`, missing `* sizeof(uint32_t)`).
 *    Because `mmio_region_read32` and `mmio_region_write32`
 *    (`sw/device/lib/base/mmio.h:97,149`) index
 *    `((volatile uint32_t *)base.base)[OT_UNSIGNED(offset) /
 * sizeof(uint32_t)]`, integer division `0x451 / 4 = 0x114` silently truncates
 * byte offset `0x451` back down to `0x450`
 * (`PINMUX_MIO_PAD_SLEEP_STATUS_0_REG_OFFSET`)! Consequently, calling
 * `dif_pinmux_pad_sleep_get_state` or `dif_pinmux_pad_sleep_clear_state` on
 * `pad = 32..46` (`MIO32..MIO46`) never accesses `MIO_PAD_SLEEP_STATUS_1`
 * (`0x454`) at all, and instead silently aliases onto bits `[14:0]`
 * (`MIO0..MIO14`) of `MIO_PAD_SLEEP_STATUS_0` (`0x450`) without raising a bus
 * fault, whereas a direct 32-bit `sw` to the unaligned byte offset `0x451`
 * splits across `0x450` (`be=4'b1110`, `PERMIT=4'b1111`) and `0x454`
 * (`be=4'b0001`, `PERMIT=4'b0011`) and faults with `mcause = 7` (Store Access
 * Fault).
 * 2. In `trunk-v2`, `PINMUX_PARAM_N_MIO_PERIPH_OUT` (`NMioPeriphOut` in
 *    `pinmux_reg_pkg.sv:10`) was reduced from `75` to `64`.
 *    `dif_pinmux_output_select` (`dif_pinmux.c:186`) rejects `outsel = 67`
 *    (`3 + 64`) with `kDifBadArg`, whereas `MIO_OUTSEL_46` (`0x288`) in
 *    `pinmux_reg_top.sv` still has a 7-bit `[6:0]` field (`0..127`) with no
 *    WARL clamp that stores and reads back `67..127` (`0x43..0x7F`).
 */
static void test_v2_dif_sleep_status_unaligned_fault_and_mio_outsel(void) {
  LOG_INFO(
      "Test 4 [NEW_IN_V2]: dif_pinmux_get_sleep_status_bit(MIO32..46) "
      "0x451/4->0x450 aliasing & NMioPeriphOut=64 vs 7-bit MIO_OUTSEL");

  dif_pinmux_t pinmux;
  CHECK_DIF_OK(dif_pinmux_init(mmio_region_from_addr(kPinmuxBase), &pinmux));

  // 4a. `dif_pinmux_pad_sleep_get_state` and `dif_pinmux_pad_sleep_clear_state`
  // on `pad = 32..46` compute `reg_offset = 0x451`, which `mmio_region_read32`
  // and `mmio_region_write32` truncate via `0x451 / 4 = 0x114` back to `0x450`
  // (`MIO_PAD_SLEEP_STATUS_0`), silently aliasing `MIO32..MIO46` onto
  // `MIO0..MIO14` while leaving `MIO_PAD_SLEEP_STATUS_1` (`0x454`) untouched.
  bool in_sleep = true;
  uint32_t prev = g_bus_fault_count;
  g_expect_bus_fault = false;
  CHECK_DIF_OK(dif_pinmux_pad_sleep_get_state(&pinmux, 31, kDifPinmuxPadKindMio,
                                              &in_sleep));
  CHECK(!in_sleep, "Expected MIO31 not in sleep mode");

  CHECK_DIF_OK(dif_pinmux_pad_sleep_get_state(&pinmux, 32, kDifPinmuxPadKindMio,
                                              &in_sleep));
  CHECK(!in_sleep, "Expected MIO32 get_state (aliased to MIO0 at 0x450) == 0");
  CHECK_DIF_OK(
      dif_pinmux_pad_sleep_clear_state(&pinmux, 32, kDifPinmuxPadKindMio));

  CHECK_DIF_OK(dif_pinmux_pad_sleep_get_state(&pinmux, kTestMioPadIdx,
                                              kDifPinmuxPadKindMio, &in_sleep));
  CHECK(!in_sleep, "Expected MIO46 get_state (aliased to MIO14 at 0x450) == 0");
  CHECK_DIF_OK(dif_pinmux_pad_sleep_clear_state(&pinmux, kTestMioPadIdx,
                                                kDifPinmuxPadKindMio));
  CHECK(g_bus_fault_count == prev,
        "mmio_region_read32/write32 truncates 0x451/4 to 0x114 (0x450) without "
        "bus fault");

  // Conversely, a direct 32-bit `sw` at the raw byte offset `0x451` computed by
  // `dif_pinmux_get_sleep_status_bit` splits into two `PutPartialData` writes
  // (`0x450` with `be=4'b1110` vs `PINMUX_PERMIT[335]=4'b1111`, and `0x454`
  // with `be=4'b0001` vs `PINMUX_PERMIT[336]=4'b0011`), raising Store Access
  // Fault (`mcause = 7`).
  prev = g_bus_fault_count;
  g_expect_bus_fault = true;
  volatile uint32_t *unaligned_sleep_status =
      (volatile uint32_t *)(kPinmuxBase +
                            PINMUX_MIO_PAD_SLEEP_STATUS_0_REG_OFFSET + 1u);
  *unaligned_sleep_status = 0u;
  g_expect_bus_fault = false;
  CHECK(g_bus_fault_count >= prev + 1,
        "Expected Store Access Fault on raw 32-bit store to offset 0x451");
  CHECK(g_last_mcause == 7, "Expected mcause=7 on unaligned 0x451 sw, got %u",
        g_last_mcause);

  // Direct aligned read of `PINMUX_MIO_PAD_SLEEP_STATUS_1_REG_OFFSET` (`0x454`)
  // succeeds without fault.
  CHECK(abs_mmio_read32(kPinmuxBase +
                        PINMUX_MIO_PAD_SLEEP_STATUS_1_REG_OFFSET) == 0u,
        "Expected aligned MIO_PAD_SLEEP_STATUS_1 (0x454) read to return 0");

  // 4b. Verify `NMioPeriphOut = 64` in v2: `dif_pinmux_output_select` accepts
  // `outsel = 66` (`3 + 64 - 1`) and rejects `outsel = 67` (`3 + 64`) with
  // `kDifBadArg`, while `MIO_OUTSEL_46` (`0x288`) accepts and reads back raw
  // 7-bit values `67` (`0x43`) and `127` (`0x7F`), and drives High-Z (`mio_out
  // = 0, mio_oe = 0`) for `67..127`.
  const uint32_t outsel46_off =
      PINMUX_MIO_OUTSEL_0_REG_OFFSET + 4u * kTestMioPadIdx;
  uint32_t orig_outsel46 = abs_mmio_read32(kPinmuxBase + outsel46_off);

  CHECK_DIF_OK(dif_pinmux_output_select(&pinmux, kTestMioPadIdx, 66u));
  CHECK(abs_mmio_read32(kPinmuxBase + outsel46_off) == 66u,
        "Expected MIO_OUTSEL_46 == 66");

  CHECK(dif_pinmux_output_select(&pinmux, kTestMioPadIdx, 67u) == kDifBadArg,
        "Expected dif_pinmux_output_select(outsel=67) to return kDifBadArg in "
        "v2");

  abs_mmio_write32(kPinmuxBase + outsel46_off, 67u);
  CHECK(abs_mmio_read32(kPinmuxBase + outsel46_off) == 0x43u,
        "Expected 7-bit MIO_OUTSEL_46 to retain 0x43 (67)");

  abs_mmio_write32(kPinmuxBase + outsel46_off, 0xFFu);
  CHECK(abs_mmio_read32(kPinmuxBase + outsel46_off) == 0x7Fu,
        "Expected 7-bit MIO_OUTSEL_46 to retain 0x7F (127) without WARL clamp "
        "at 66");

  // Route MIO46 (PADSEL = 46 + 2 = 48) into GPIO0 (`MIO_PERIPH_INSEL_0 = 48`):
  // Verify `outsel = 0` (`ConstantZero`, `mio_oe = 1, mio_out = 0`) drives 0,
  // `outsel = 1` (`ConstantOne`, `mio_oe = 1, mio_out = 1`) drives 1, and
  // out-of-range `outsel = 67` (`0x43`) and `127` (`0x7F`) drive High-Z
  // (`mio_oe = 0, mio_out = 0`), matching `outsel = 2` (`ConstantHighZ`).
  CHECK_DIF_OK(dif_pinmux_input_select(
      &pinmux, kTopEarlgreyPinmuxPeripheralInGpioGpio0, kTestMioPadIdx + 2u));
  abs_mmio_write32(kPinmuxBase + outsel46_off,
                   kTopEarlgreyPinmuxOutselConstantZero);
  busy_spin_micros(10);
  CHECK((abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) & 1u) == 0u);

  abs_mmio_write32(kPinmuxBase + outsel46_off,
                   kTopEarlgreyPinmuxOutselConstantHighZ);
  busy_spin_micros(10);
  uint32_t hz_after_0 =
      abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) & 1u;

  abs_mmio_write32(kPinmuxBase + outsel46_off,
                   kTopEarlgreyPinmuxOutselConstantZero);
  busy_spin_micros(10);
  abs_mmio_write32(kPinmuxBase + outsel46_off, 67u);  // 0x43 (out of range)
  busy_spin_micros(10);
  CHECK((abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) & 1u) ==
        hz_after_0);

  abs_mmio_write32(kPinmuxBase + outsel46_off,
                   kTopEarlgreyPinmuxOutselConstantZero);
  busy_spin_micros(10);
  abs_mmio_write32(kPinmuxBase + outsel46_off, 127u);  // 0x7F (out of range)
  busy_spin_micros(10);
  CHECK((abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) & 1u) ==
        hz_after_0);

  abs_mmio_write32(kPinmuxBase + outsel46_off,
                   kTopEarlgreyPinmuxOutselConstantOne);
  busy_spin_micros(10);
  CHECK((abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) & 1u) == 1u);

  abs_mmio_write32(kPinmuxBase + outsel46_off,
                   kTopEarlgreyPinmuxOutselConstantHighZ);
  busy_spin_micros(10);
  uint32_t hz_after_1 =
      abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) & 1u;

  abs_mmio_write32(kPinmuxBase + outsel46_off,
                   kTopEarlgreyPinmuxOutselConstantOne);
  busy_spin_micros(10);
  abs_mmio_write32(kPinmuxBase + outsel46_off, 67u);  // 0x43 (out of range)
  busy_spin_micros(10);
  CHECK((abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) & 1u) ==
        hz_after_1);

  abs_mmio_write32(kPinmuxBase + outsel46_off, orig_outsel46);
}

bool test_main(void) {
  LOG_INFO("=== pinmux Earlgrey v2 Errata Verification Test ===");
  test_wkup_timed_cnt_th_zero_hazard();
  test_filter_tracking_and_pad_attr_warl();
  test_pad_attr_subword_store_faults();
  test_v2_dif_sleep_status_unaligned_fault_and_mio_outsel();
  LOG_INFO("=== All pinmux v2 errata checks PASSED ===");
  return true;
}
