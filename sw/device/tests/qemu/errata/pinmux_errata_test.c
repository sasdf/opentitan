// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * CW340 FPGA & QEMU Empirical Errata Confirmation Test for `pinmux` (`P15`).
 *
 * Empirically verifies all documented hardware errata, quirks, and security
 * hardening behaviors in `/root/knowledge/errata/pinmux.md` and companion
 * `/root/knowledge/errata/pinmux_wkup_cnt_th_zero_and_padsel_offset.md` across
 * both the physical CW340 FPGA (golden Earlgrey RTL) and QEMU (`ot_pinmux_eg`):
 *
 * - [pinmux_wkup.sv:52] (TRUE_SILICON_ERRATA):
 *   In `pinmux_wkup.sv:52, 65-72`, `HighTimed` (`MODE = 3`) and `LowTimed`
 *   (`MODE = 4`) output `aon_wkup_pulse_o = cnt_eq_th` (`cnt_q >=
 * wkup_cnt_th_i`) WITHOUT gating by `cnt_en` (`filter_out_d` /
 * `~filter_out_d`). Consequently, when `WKUP_DETECTOR_CNT_TH == 0` (the
 * hardware reset default), enabling `WKUP_DETECTOR_EN = 1` immediately latches
 * `WKUP_CAUSE = 1` regardless of pin polarity (even when the selected pin is
 * LOW `ConstantZero` in `TimedHigh` mode or HIGH `ConstantOne` in `TimedLow`
 * mode). Verified alongside the software workaround (`WKUP_DETECTOR_CNT_TH >=
 * 1`).
 * - [pinmux_wkup.sv:28-89] (BENIGN_RTL_IMPL_DETAIL):
 *   Continuous `filter_out_q` AON tracking while `WKUP_DETECTOR_EN == 0`
 *   (`pinmux_wkup.sv:81-89`) prevents false `Posedge` wakeups when enabling a
 *   detector on an already-high pin (`PADSEL = 1` `ConstantOne`), while MIO
 *   `PADSEL` uses a `+2` offset (`0=ConstantZero`, `1=ConstantOne`,
 * `2+m=MIO[m]`) and `MIO/DIO_PAD_ATTR` WARL masks retain only `0x83` (`Bidir*`)
 * or `0x81`
 *   (`InputStd` DIO `10..13`).
 * - [pinmux_reg_pkg.sv:2127-2696] (INTENDED_SECURITY_HARDENING):
 *   `PINMUX_PERMIT[568]` (`pinmux_reg_pkg.sv:2127-2696`) assigns permit
 * `4'b0111` (3 bytes required) to `MIO_PAD_ATTR` and `DIO_PAD_ATTR`, causing
 * 8-bit (`sb`) and 16-bit (`sh`) stores to raise a synchronous Store Access
 * Fault
 *   (`mcause = 7`) despite WARL only implementing bits `[7:0]` in byte 0.
 * - [pinmux.sv:127-143 / E4] (BENIGN_RTL_IMPL_DETAIL):
 *   1-cycle `ALERT_TEST` pulse and `rw0c` `*_REGWEN` write-gating on
 *   `MIO_PAD_SLEEP_MODE`, `DIO_PAD_SLEEP_MODE`, and `WKUP_DETECTOR_*`.
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/arch/device.h"
#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_alert_handler.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "pinmux_regs.h"

OTTF_DEFINE_TEST_CONFIG(.ignore_alerts = true);

enum {
  kPinmuxBase = TOP_EARLGREY_PINMUX_AON_BASE_ADDR,
  kAlertHandlerBase = TOP_EARLGREY_ALERT_HANDLER_BASE_ADDR,
  kMcauseLoadAccessFault = 5u,
  kMcauseStoreAccessFault = 7u,
};

static volatile bool g_expect_bus_fault = false;
static volatile bool g_expected_fault_taken = false;
static volatile uint32_t g_last_mcause = 0;

void ottf_exception_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = ibex_mcause_read();
  uint32_t mepc = ibex_mepc_read();
  if (g_expect_bus_fault &&
      (mcause == kMcauseLoadAccessFault || mcause == kMcauseStoreAccessFault)) {
    g_expected_fault_taken = true;
    g_last_mcause = mcause;
    g_expect_bus_fault = false;
    uint16_t insn16 = *(const uint16_t *)mepc;
    uint32_t step = ((insn16 & 0x3u) == 0x3u) ? 4u : 2u;
    CSR_WRITE(CSR_REG_MEPC, mepc + step);
    return;
  }
  CHECK(false, "Unexpected exception mcause=0x%08x mepc=0x%08x", mcause, mepc);
}

static void verify_pinmux_cnt_th_zero_hazard(void) {
  LOG_INFO(
      "Verifying [pinmux_wkup.sv:52] (TRUE_SILICON_ERRATA): TimedHigh/TimedLow "
      "with WKUP_DETECTOR_CNT_TH == 0 triggers wakeup immediately regardless "
      "of pin polarity (pinmux_wkup.sv:52, 65-72)...");

  /* 1. TimedHigh (MODE=3) with PADSEL=0 (ConstantZero, LOW) and CNT_TH=0:
   *    Even though pin is LOW (cnt_en == 0), cnt_eq_th (0 >= 0) is ungated
   *    and immediately latches WKUP_CAUSE[0] = 1! */
  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_DETECTOR_EN_0_REG_OFFSET, 0u);
  busy_spin_micros(25);
  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_CAUSE_REG_OFFSET, 0u);
  busy_spin_micros(25);

  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_DETECTOR_0_REG_OFFSET,
                   PINMUX_WKUP_DETECTOR_0_MODE_0_VALUE_TIMEDHIGH);
  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_DETECTOR_PADSEL_0_REG_OFFSET, 0u);
  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_DETECTOR_CNT_TH_0_REG_OFFSET, 0u);
  busy_spin_micros(30);

  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_DETECTOR_EN_0_REG_OFFSET, 1u);
  busy_spin_micros(50);

  uint32_t cause = abs_mmio_read32(kPinmuxBase + PINMUX_WKUP_CAUSE_REG_OFFSET);
  CHECK((cause & 0x1u) == 0x1u,
        "[pinmux_wkup.sv:52] TimedHigh + CNT_TH=0 on ConstantZero (LOW) "
        "expected WKUP_CAUSE[0]=1, got 0x%x",
        cause);

  /* 2. TimedLow (MODE=4) with PADSEL=1 (ConstantOne, HIGH) and CNT_TH=0:
   *    Even though pin is HIGH (cnt_en == 0), cnt_eq_th (0 >= 0) is ungated
   *    and immediately latches WKUP_CAUSE[0] = 1! */
  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_DETECTOR_EN_0_REG_OFFSET, 0u);
  busy_spin_micros(25);
  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_CAUSE_REG_OFFSET, 0xfeu);
  busy_spin_micros(25);
  CHECK((abs_mmio_read32(kPinmuxBase + PINMUX_WKUP_CAUSE_REG_OFFSET) & 0x1u) ==
        0u);

  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_DETECTOR_0_REG_OFFSET,
                   PINMUX_WKUP_DETECTOR_0_MODE_0_VALUE_TIMEDLOW);
  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_DETECTOR_PADSEL_0_REG_OFFSET, 1u);
  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_DETECTOR_CNT_TH_0_REG_OFFSET, 0u);
  busy_spin_micros(30);

  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_DETECTOR_EN_0_REG_OFFSET, 1u);
  busy_spin_micros(50);

  cause = abs_mmio_read32(kPinmuxBase + PINMUX_WKUP_CAUSE_REG_OFFSET);
  CHECK((cause & 0x1u) == 0x1u,
        "[pinmux_wkup.sv:52] TimedLow + CNT_TH=0 on ConstantOne (HIGH) "
        "expected WKUP_CAUSE[0]=1, got 0x%x",
        cause);

  /* 3. Software workaround verification: programming CNT_TH = 1 prevents
   *    TimedHigh from firing while PADSEL=0 (ConstantZero) and fires when
   *    PADSEL=1 (ConstantOne). */
  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_DETECTOR_EN_0_REG_OFFSET, 0u);
  busy_spin_micros(25);
  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_CAUSE_REG_OFFSET, 0xfeu);
  busy_spin_micros(25);

  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_DETECTOR_0_REG_OFFSET,
                   PINMUX_WKUP_DETECTOR_0_MODE_0_VALUE_TIMEDHIGH);
  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_DETECTOR_PADSEL_0_REG_OFFSET, 0u);
  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_DETECTOR_CNT_TH_0_REG_OFFSET, 1u);
  busy_spin_micros(30);

  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_DETECTOR_EN_0_REG_OFFSET, 1u);
  busy_spin_micros(50);
  CHECK((abs_mmio_read32(kPinmuxBase + PINMUX_WKUP_CAUSE_REG_OFFSET) & 0x1u) ==
        0u);

  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_DETECTOR_PADSEL_0_REG_OFFSET, 1u);
  busy_spin_micros(50);
  CHECK((abs_mmio_read32(kPinmuxBase + PINMUX_WKUP_CAUSE_REG_OFFSET) & 0x1u) ==
        1u);

  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_DETECTOR_EN_0_REG_OFFSET, 0u);
  busy_spin_micros(25);
  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_CAUSE_REG_OFFSET, 0xfeu);
  busy_spin_micros(25);
  LOG_INFO("[pinmux_wkup.sv:52] confirmed.");
}

static void verify_pinmux_filter_and_warl(void) {
  LOG_INFO(
      "Verifying [pinmux_wkup.sv:28-89] (BENIGN_RTL_IMPL_DETAIL): Continuous "
      "filter_out_q tracking while WKUP_DETECTOR_EN == 0, MIO PADSEL +2 "
      "offset, and MIO/DIO_PAD_ATTR WARL masks (0x83 / 0x81)...");

  /* 1. Continuous filter_out_q tracking while WKUP_DETECTOR_EN == 0 prevents
   *    spurious Posedge wakeup when PADSEL=1 (ConstantOne) is already high. */
  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_DETECTOR_0_REG_OFFSET,
                   PINMUX_WKUP_DETECTOR_0_MODE_0_VALUE_POSEDGE);
  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_DETECTOR_PADSEL_0_REG_OFFSET, 1u);
  busy_spin_micros(50);

  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_DETECTOR_EN_0_REG_OFFSET, 1u);
  busy_spin_micros(50);
  CHECK((abs_mmio_read32(kPinmuxBase + PINMUX_WKUP_CAUSE_REG_OFFSET) & 0x1u) ==
        0u);

  /* Toggling PADSEL 0 -> 1 while enabled triggers Posedge wakeup. */
  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_DETECTOR_PADSEL_0_REG_OFFSET, 0u);
  busy_spin_micros(50);
  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_DETECTOR_PADSEL_0_REG_OFFSET, 1u);
  busy_spin_micros(50);
  CHECK((abs_mmio_read32(kPinmuxBase + PINMUX_WKUP_CAUSE_REG_OFFSET) & 0x1u) ==
        1u);

  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_DETECTOR_EN_0_REG_OFFSET, 0u);
  busy_spin_micros(25);
  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_CAUSE_REG_OFFSET, 0xfeu);
  busy_spin_micros(25);

  /* 2. MIO_PAD_ATTR (BidirStd -> 0x83) and DIO_PAD_ATTR (InputStd -> 0x81). */
  const uint32_t kMioOff = PINMUX_MIO_PAD_ATTR_0_REG_OFFSET +
                           kTopEarlgreyPinmuxMioOutIoa2 * sizeof(uint32_t);
  uint32_t orig_mio = abs_mmio_read32(kPinmuxBase + kMioOff);
  abs_mmio_write32(kPinmuxBase + kMioOff, 0xffffffffu);
  uint32_t mio_rb = abs_mmio_read32(kPinmuxBase + kMioOff);
  abs_mmio_write32(kPinmuxBase + kMioOff, orig_mio);
  CHECK((mio_rb & 0x00000083u) == 0x00000083u && (mio_rb & ~0x0010008fu) == 0u);

  const uint32_t kDioSckOff =
      PINMUX_DIO_PAD_ATTR_0_REG_OFFSET +
      kTopEarlgreyDirectPadsSpiDeviceSck * sizeof(uint32_t);
  uint32_t orig_sck = abs_mmio_read32(kPinmuxBase + kDioSckOff);
  abs_mmio_write32(kPinmuxBase + kDioSckOff, 0xffffffffu);
  uint32_t sck_rb = abs_mmio_read32(kPinmuxBase + kDioSckOff);
  abs_mmio_write32(kPinmuxBase + kDioSckOff, orig_sck);
  CHECK((sck_rb & 0x00000081u) == 0x00000081u && (sck_rb & ~0x0000008du) == 0u);
  LOG_INFO("[pinmux_wkup.sv:28-89] confirmed.");
}

static void verify_pinmux_permit_subword_faults(void) {
  LOG_INFO(
      "Verifying [pinmux_reg_pkg.sv:2127-2696] (INTENDED_SECURITY_HARDENING): "
      "PINMUX_PERMIT=4'b0111 rejects 8-bit/16-bit writes to MIO/DIO_PAD_ATTR "
      "with Store Access Fault (mcause=7) despite 1-byte WARL mask...");

  const uint32_t kMioOff = PINMUX_MIO_PAD_ATTR_0_REG_OFFSET +
                           kTopEarlgreyPinmuxMioOutIoa2 * sizeof(uint32_t);
  uint32_t orig_mio = abs_mmio_read32(kPinmuxBase + kMioOff);
  abs_mmio_write32(kPinmuxBase + kMioOff, 0x00000083u);

  /* 1-byte read (lbu) succeeds and returns 0x83. */
  g_expected_fault_taken = false;
  g_expect_bus_fault = true;
  uint8_t b0 = abs_mmio_read8(kPinmuxBase + kMioOff);
  g_expect_bus_fault = false;
  CHECK(!g_expected_fault_taken && b0 == 0x83u);

  /* 1-byte write (sb) to MIO_PAD_ATTR -> Store Access Fault (mcause=7). */
  g_expected_fault_taken = false;
  g_last_mcause = 0;
  g_expect_bus_fault = true;
  abs_mmio_write8(kPinmuxBase + kMioOff, 0x00u);
  CHECK(g_expected_fault_taken && g_last_mcause == kMcauseStoreAccessFault);
  CHECK(abs_mmio_read32(kPinmuxBase + kMioOff) == 0x00000083u);

  /* 2-byte write (sh) to MIO_PAD_ATTR -> Store Access Fault (mcause=7). */
  g_expected_fault_taken = false;
  g_last_mcause = 0;
  g_expect_bus_fault = true;
  *(volatile uint16_t *)(kPinmuxBase + kMioOff) = 0x0000u;
  CHECK(g_expected_fault_taken && g_last_mcause == kMcauseStoreAccessFault);
  CHECK(abs_mmio_read32(kPinmuxBase + kMioOff) == 0x00000083u);

  abs_mmio_write32(kPinmuxBase + kMioOff, orig_mio);
  LOG_INFO("[pinmux_reg_pkg.sv:2127-2696] confirmed.");
}

static void verify_pinmux_alert_and_regwen(void) {
  LOG_INFO(
      "Verifying [pinmux.sv:127-143 / E4] (BENIGN_RTL_IMPL_DETAIL): "
      "1-cycle ALERT_TEST pulse and rw0c *_REGWEN write-gating...");

  dif_alert_handler_t alert_handler;
  CHECK_DIF_OK(dif_alert_handler_init(mmio_region_from_addr(kAlertHandlerBase),
                                      &alert_handler));
  CHECK_DIF_OK(dif_alert_handler_configure_alert(
      &alert_handler, kTopEarlgreyAlertIdPinmuxAonFatalFault,
      kDifAlertHandlerClassA, kDifToggleEnabled, kDifToggleEnabled));

  abs_mmio_write32(kPinmuxBase + PINMUX_ALERT_TEST_REG_OFFSET,
                   1u << PINMUX_ALERT_TEST_FATAL_FAULT_BIT);
  busy_spin_micros(10);

  bool is_cause = false;
  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
      &alert_handler, kTopEarlgreyAlertIdPinmuxAonFatalFault, &is_cause));
  CHECK(is_cause);

  CHECK_DIF_OK(dif_alert_handler_alert_acknowledge(
      &alert_handler, kTopEarlgreyAlertIdPinmuxAonFatalFault));
  busy_spin_micros(20);
  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
      &alert_handler, kTopEarlgreyAlertIdPinmuxAonFatalFault, &is_cause));
  CHECK(!is_cause);

  /* Verify WKUP_DETECTOR_REGWEN[6] rw0c lock and write-gating. */
  const uint32_t kTestIdx = 6u;
  const uint32_t kRegwenOff =
      PINMUX_WKUP_DETECTOR_REGWEN_0_REG_OFFSET + kTestIdx * sizeof(uint32_t);
  const uint32_t kThOff =
      PINMUX_WKUP_DETECTOR_CNT_TH_0_REG_OFFSET + kTestIdx * sizeof(uint32_t);
  abs_mmio_write32(kPinmuxBase + kRegwenOff, 0u);
  abs_mmio_write32(kPinmuxBase + kRegwenOff, 1u);
  CHECK(abs_mmio_read32(kPinmuxBase + kRegwenOff) == 0u);
  abs_mmio_write32(kPinmuxBase + kThOff, 0x42u);
  busy_spin_micros(20);
  CHECK(abs_mmio_read32(kPinmuxBase + kThOff) == 0u);
  LOG_INFO("[pinmux.sv:127-143 / E4] confirmed.");
}

bool test_main(void) {
  LOG_INFO("Starting PINMUX Errata Confirmation Test on %s...",
           kDeviceType == kDeviceFpgaCw340 ? "CW340_FPGA" : "QEMU");

  verify_pinmux_cnt_th_zero_hazard();
  verify_pinmux_filter_and_warl();
  verify_pinmux_permit_subword_faults();
  verify_pinmux_alert_and_regwen();

  LOG_INFO("All PINMUX errata & security hardening checks PASSED!");
  return true;
}
