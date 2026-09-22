// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/arch/device.h"
#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_alert_handler.h"
#include "sw/device/lib/dif/dif_pinmux.h"
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
};

static volatile uint32_t load_fault_count = 0;
static volatile uint32_t store_fault_count = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = ibex_mcause_read();
  if (mcause == kIbexExcLoadAccessFault) {
    load_fault_count++;
  } else if (mcause == kIbexExcStoreAccessFault) {
    store_fault_count++;
  }
}

#define EXPECT_CHECK(cond, fmt, ...)              \
  do {                                            \
    if (!(cond)) {                                \
      LOG_ERROR("MISMATCH: " fmt, ##__VA_ARGS__); \
      failures++;                                 \
    }                                             \
  } while (0)

bool test_main(void) {
  uint32_t failures = 0;

  dif_alert_handler_t alert_handler;
  CHECK_DIF_OK(dif_alert_handler_init(mmio_region_from_addr(kAlertHandlerBase),
                                      &alert_handler));
  CHECK_DIF_OK(dif_alert_handler_configure_alert(
      &alert_handler, kTopEarlgreyAlertIdPinmuxAonFatalFault,
      kDifAlertHandlerClassA, /*enabled=*/kDifToggleEnabled,
      /*locked=*/kDifToggleEnabled));

  // ---------------------------------------------------------------------------
  // 1. ALERT_TEST single-cycle pulse vs persistent level
  // ---------------------------------------------------------------------------
  abs_mmio_write32(kPinmuxBase + PINMUX_ALERT_TEST_REG_OFFSET,
                   1u << PINMUX_ALERT_TEST_FATAL_FAULT_BIT);
  busy_spin_micros(10);

  bool is_cause = false;
  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
      &alert_handler, kTopEarlgreyAlertIdPinmuxAonFatalFault, &is_cause));
  EXPECT_CHECK(
      is_cause,
      "ALERT_CAUSE[PinmuxAonFatalFault] did not latch after ALERT_TEST");

  CHECK_DIF_OK(dif_alert_handler_alert_acknowledge(
      &alert_handler, kTopEarlgreyAlertIdPinmuxAonFatalFault));
  busy_spin_micros(20);
  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
      &alert_handler, kTopEarlgreyAlertIdPinmuxAonFatalFault, &is_cause));
  EXPECT_CHECK(!is_cause,
               "ALERT_CAUSE[PinmuxAonFatalFault] re-latched after ACK "
               "(pinmux ALERT_TEST stuck high)");

  // ---------------------------------------------------------------------------
  // 2. MIO_PAD_ATTR / DIO_PAD_ATTR WARL mask readback
  //    On Earlgrey RTL / CW340 (prim_xilinx_pad_attr.sv), BidirStd pads have
  //    warl_mask = 0x00000083 (invert, virt_od_en, input_disable).
  // ---------------------------------------------------------------------------
  const uint32_t kMioPadIoa2 = kTopEarlgreyPinmuxMioOutIoa2;
  const uint32_t kMioAttrOff =
      PINMUX_MIO_PAD_ATTR_0_REG_OFFSET + kMioPadIoa2 * sizeof(uint32_t);
  uint32_t orig_mio_attr = abs_mmio_read32(kPinmuxBase + kMioAttrOff);
  abs_mmio_write32(kPinmuxBase + kMioAttrOff, 0xffffffffu);
  uint32_t mio_attr_readback = abs_mmio_read32(kPinmuxBase + kMioAttrOff);
  abs_mmio_write32(kPinmuxBase + kMioAttrOff, orig_mio_attr);
  EXPECT_CHECK((mio_attr_readback & 0x00000083u) == 0x00000083u &&
                   (mio_attr_readback & ~0x0010008fu) == 0u,
               "MIO_PAD_ATTR WARL readback mismatch: got 0x%08x",
               mio_attr_readback);

  const uint32_t kDioPadSpiDevSd2 = kTopEarlgreyDirectPadsSpiDeviceSd2;
  const uint32_t kDioAttrOff =
      PINMUX_DIO_PAD_ATTR_0_REG_OFFSET + kDioPadSpiDevSd2 * sizeof(uint32_t);
  uint32_t orig_dio_attr = abs_mmio_read32(kPinmuxBase + kDioAttrOff);
  abs_mmio_write32(kPinmuxBase + kDioAttrOff, 0xffffffffu);
  uint32_t dio_attr_readback = abs_mmio_read32(kPinmuxBase + kDioAttrOff);
  abs_mmio_write32(kPinmuxBase + kDioAttrOff, orig_dio_attr);
  EXPECT_CHECK((dio_attr_readback & 0x00000083u) == 0x00000083u &&
                   (dio_attr_readback & ~0x0010008fu) == 0u,
               "DIO_PAD_ATTR[SpiDeviceSd2] WARL readback mismatch: got 0x%08x",
               dio_attr_readback);

  const uint32_t kDioPadSpiDevSck = kTopEarlgreyDirectPadsSpiDeviceSck;
  const uint32_t kDioSckAttrOff =
      PINMUX_DIO_PAD_ATTR_0_REG_OFFSET + kDioPadSpiDevSck * sizeof(uint32_t);
  uint32_t orig_sck_attr = abs_mmio_read32(kPinmuxBase + kDioSckAttrOff);
  abs_mmio_write32(kPinmuxBase + kDioSckAttrOff, 0xffffffffu);
  uint32_t dio_sck_readback = abs_mmio_read32(kPinmuxBase + kDioSckAttrOff);
  abs_mmio_write32(kPinmuxBase + kDioSckAttrOff, orig_sck_attr);
  EXPECT_CHECK((dio_sck_readback & 0x00000081u) == 0x00000081u &&
                   (dio_sck_readback & ~0x0000008du) == 0u,
               "DIO_PAD_ATTR[SpiDeviceSck] (InputStd) WARL readback mismatch: "
               "got 0x%08x",
               dio_sck_readback);

  // ---------------------------------------------------------------------------
  // 3. *_REGWEN rw0c semantics (writing 1 after clearing to 0 must not re-set)
  // ---------------------------------------------------------------------------
  const uint32_t kTestIdx = 6u;
  const uint32_t kDioTestIdx = 4u;
  const uint32_t kWkupRegwenOff =
      PINMUX_WKUP_DETECTOR_REGWEN_0_REG_OFFSET + kTestIdx * sizeof(uint32_t);
  const uint32_t kMioSleepRegwenOff =
      PINMUX_MIO_PAD_SLEEP_REGWEN_0_REG_OFFSET + kTestIdx * sizeof(uint32_t);
  const uint32_t kDioSleepRegwenOff =
      PINMUX_DIO_PAD_SLEEP_REGWEN_0_REG_OFFSET + kDioTestIdx * sizeof(uint32_t);

  abs_mmio_write32(kPinmuxBase + kWkupRegwenOff, 0u);
  abs_mmio_write32(kPinmuxBase + kWkupRegwenOff, 1u);
  uint32_t wkup_regwen_val = abs_mmio_read32(kPinmuxBase + kWkupRegwenOff);
  EXPECT_CHECK(wkup_regwen_val == 0u,
               "WKUP_DETECTOR_REGWEN[6] rw0c failed: writing 1 re-set it to %u",
               wkup_regwen_val);
  abs_mmio_write32(kPinmuxBase + kWkupRegwenOff, 0u);

  abs_mmio_write32(kPinmuxBase + kMioSleepRegwenOff, 0u);
  abs_mmio_write32(kPinmuxBase + kMioSleepRegwenOff, 1u);
  uint32_t mio_sleep_regwen_val =
      abs_mmio_read32(kPinmuxBase + kMioSleepRegwenOff);
  EXPECT_CHECK(mio_sleep_regwen_val == 0u,
               "MIO_PAD_SLEEP_REGWEN[6] rw0c failed: writing 1 re-set it to %u",
               mio_sleep_regwen_val);
  abs_mmio_write32(kPinmuxBase + kMioSleepRegwenOff, 0u);

  abs_mmio_write32(kPinmuxBase + kDioSleepRegwenOff, 0u);
  abs_mmio_write32(kPinmuxBase + kDioSleepRegwenOff, 1u);
  uint32_t dio_sleep_regwen_val =
      abs_mmio_read32(kPinmuxBase + kDioSleepRegwenOff);
  EXPECT_CHECK(dio_sleep_regwen_val == 0u,
               "DIO_PAD_SLEEP_REGWEN[4] rw0c failed: writing 1 re-set it to %u",
               dio_sleep_regwen_val);
  abs_mmio_write32(kPinmuxBase + kDioSleepRegwenOff, 0u);

  // ---------------------------------------------------------------------------
  // 4. REGWEN gating on MIO_PAD_SLEEP_MODE, DIO_PAD_SLEEP_MODE,
  //    WKUP_DETECTOR (CFG), WKUP_DETECTOR_CNT_TH, and WKUP_DETECTOR_PADSEL
  // ---------------------------------------------------------------------------
  const uint32_t kMioSleepModeOff =
      PINMUX_MIO_PAD_SLEEP_MODE_0_REG_OFFSET + kTestIdx * sizeof(uint32_t);
  uint32_t mio_mode_before = abs_mmio_read32(kPinmuxBase + kMioSleepModeOff);
  abs_mmio_write32(kPinmuxBase + kMioSleepModeOff, 0u);
  uint32_t mio_mode_after = abs_mmio_read32(kPinmuxBase + kMioSleepModeOff);
  EXPECT_CHECK(mio_mode_after == mio_mode_before,
               "MIO_PAD_SLEEP_MODE[6] modified while REGWEN==0: got %u, "
               "expected %u",
               mio_mode_after, mio_mode_before);

  const uint32_t kDioSleepModeOff =
      PINMUX_DIO_PAD_SLEEP_MODE_0_REG_OFFSET + kDioTestIdx * sizeof(uint32_t);
  uint32_t dio_mode_before = abs_mmio_read32(kPinmuxBase + kDioSleepModeOff);
  abs_mmio_write32(kPinmuxBase + kDioSleepModeOff, 1u);
  uint32_t dio_mode_after = abs_mmio_read32(kPinmuxBase + kDioSleepModeOff);
  EXPECT_CHECK(dio_mode_after == dio_mode_before,
               "DIO_PAD_SLEEP_MODE[4] modified while REGWEN==0: got %u, "
               "expected %u",
               dio_mode_after, dio_mode_before);

  const uint32_t kWkupCfgOff =
      PINMUX_WKUP_DETECTOR_0_REG_OFFSET + kTestIdx * sizeof(uint32_t);
  const uint32_t kWkupThOff =
      PINMUX_WKUP_DETECTOR_CNT_TH_0_REG_OFFSET + kTestIdx * sizeof(uint32_t);
  const uint32_t kWkupPadselOff =
      PINMUX_WKUP_DETECTOR_PADSEL_0_REG_OFFSET + kTestIdx * sizeof(uint32_t);

  abs_mmio_write32(kPinmuxBase + kWkupCfgOff, 0x1bu);
  abs_mmio_write32(kPinmuxBase + kWkupThOff, 0x42u);
  abs_mmio_write32(kPinmuxBase + kWkupPadselOff, 0x05u);
  busy_spin_micros(20);

  uint32_t wkup_cfg_val = abs_mmio_read32(kPinmuxBase + kWkupCfgOff);
  uint32_t wkup_th_val = abs_mmio_read32(kPinmuxBase + kWkupThOff);
  uint32_t wkup_padsel_val = abs_mmio_read32(kPinmuxBase + kWkupPadselOff);
  EXPECT_CHECK(wkup_cfg_val == 0u && wkup_th_val == 0u && wkup_padsel_val == 0u,
               "WKUP_DETECTOR[6] registers modified while REGWEN==0: "
               "cfg=0x%x th=0x%x padsel=0x%x (expected 0, 0, 0)",
               wkup_cfg_val, wkup_th_val, wkup_padsel_val);

  // ---------------------------------------------------------------------------
  // 5. Active-mode wakeup detector evaluation on mio_wkup_mux constant 1
  //    (PADSEL = 1, MODE = TimedHigh, CNT_TH = 1) -> sets WKUP_CAUSE[0] = 1
  // ---------------------------------------------------------------------------
  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_DETECTOR_EN_0_REG_OFFSET, 0u);
  busy_spin_micros(20);
  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_CAUSE_REG_OFFSET, 0u);
  busy_spin_micros(20);

  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_DETECTOR_0_REG_OFFSET,
                   PINMUX_WKUP_DETECTOR_0_MODE_0_VALUE_TIMEDHIGH);
  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_DETECTOR_CNT_TH_0_REG_OFFSET, 1u);
  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_DETECTOR_PADSEL_0_REG_OFFSET, 1u);
  busy_spin_micros(20);

  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_DETECTOR_EN_0_REG_OFFSET, 1u);
  busy_spin_micros(50);

  uint32_t wkup_cause =
      abs_mmio_read32(kPinmuxBase + PINMUX_WKUP_CAUSE_REG_OFFSET);
  EXPECT_CHECK((wkup_cause & 0x1u) == 0x1u,
               "WKUP_CAUSE[0] did not latch to 1 with TimedHigh on PADSEL=1 "
               "(ConstantOne): got 0x%02x",
               wkup_cause);

  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_DETECTOR_EN_0_REG_OFFSET, 0u);
  busy_spin_micros(20);
  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_CAUSE_REG_OFFSET, 0xfeu);
  busy_spin_micros(20);
  wkup_cause = abs_mmio_read32(kPinmuxBase + PINMUX_WKUP_CAUSE_REG_OFFSET);
  EXPECT_CHECK((wkup_cause & 0x1u) == 0x0u,
               "WKUP_CAUSE[0] did not clear on rw0c write of 0xfe: got 0x%02x",
               wkup_cause);

  // ---------------------------------------------------------------------------
  // 6. Enabling a Posedge wakeup detector while the selected input (PADSEL=1,
  //    ConstantOne) is already steady high must NOT trigger a spurious wakeup
  //    (pinmux_wkup.sv: filter_out_q tracks filter_out_d even while
  //    wkup_en_i=0)
  // ---------------------------------------------------------------------------
  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_DETECTOR_0_REG_OFFSET,
                   PINMUX_WKUP_DETECTOR_0_MODE_0_VALUE_POSEDGE);
  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_DETECTOR_PADSEL_0_REG_OFFSET, 1u);
  busy_spin_micros(50);

  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_DETECTOR_EN_0_REG_OFFSET, 1u);
  busy_spin_micros(50);

  wkup_cause = abs_mmio_read32(kPinmuxBase + PINMUX_WKUP_CAUSE_REG_OFFSET);
  EXPECT_CHECK((wkup_cause & 0x1u) == 0x0u,
               "WKUP_CAUSE[0] falsely triggered on Posedge enable while "
               "PADSEL=1 was already steady high: got 0x%02x",
               wkup_cause);

  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_DETECTOR_PADSEL_0_REG_OFFSET, 0u);
  busy_spin_micros(50);
  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_DETECTOR_PADSEL_0_REG_OFFSET, 1u);
  busy_spin_micros(50);

  wkup_cause = abs_mmio_read32(kPinmuxBase + PINMUX_WKUP_CAUSE_REG_OFFSET);
  EXPECT_CHECK((wkup_cause & 0x1u) == 0x1u,
               "WKUP_CAUSE[0] did not trigger on Posedge transition PADSEL "
               "0 -> 1 while enabled: got 0x%02x",
               wkup_cause);

  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_DETECTOR_EN_0_REG_OFFSET, 0u);
  busy_spin_micros(20);
  abs_mmio_write32(kPinmuxBase + PINMUX_WKUP_CAUSE_REG_OFFSET, 0xfeu);
  busy_spin_micros(20);

  // ---------------------------------------------------------------------------
  // 7. PINMUX_PERMIT sub-word write error (`wr_err`), valid sub-word
  // read/write,
  //    and unmapped `addrmiss` (`0x8e0`)
  // ---------------------------------------------------------------------------
  load_fault_count = 0;
  store_fault_count = 0;

  // Sub-word read from MIO_PAD_ATTR (PINMUX_PERMIT = 0x7) must succeed
  abs_mmio_write32(kPinmuxBase + kMioAttrOff, 0x00000083u);
  uint8_t mio_attr_b0 = abs_mmio_read8(kPinmuxBase + kMioAttrOff);
  EXPECT_CHECK(load_fault_count == 0u && mio_attr_b0 == 0x83u,
               "sub-word read8 from MIO_PAD_ATTR faulted (%u) or returned "
               "0x%02x (expected 0x83)",
               load_fault_count, mio_attr_b0);

  // Sub-word write16 to MIO_PAD_ATTR (PINMUX_PERMIT = 0x7, reg_be = 0x3) ->
  // wr_err
  store_fault_count = 0;
  *(volatile uint16_t *)(kPinmuxBase + kMioAttrOff) = 0x0000u;
  EXPECT_CHECK(store_fault_count == 1u,
               "sub-word write16 to MIO_PAD_ATTR (PERMIT=0x7) did not fault: "
               "store_fault_count=%u",
               store_fault_count);
  EXPECT_CHECK(abs_mmio_read32(kPinmuxBase + kMioAttrOff) == 0x00000083u,
               "MIO_PAD_ATTR modified by rejected sub-word write16");
  abs_mmio_write32(kPinmuxBase + kMioAttrOff, orig_mio_attr);

  // Sub-word write8 to DIO_PAD_ATTR (PINMUX_PERMIT = 0x7, reg_be = 0x1) ->
  // wr_err
  store_fault_count = 0;
  abs_mmio_write8(kPinmuxBase + kDioAttrOff, 0x83u);
  EXPECT_CHECK(store_fault_count == 1u,
               "sub-word write8 to DIO_PAD_ATTR (PERMIT=0x7) did not fault: "
               "store_fault_count=%u",
               store_fault_count);

  // Sub-word write16 to MIO_PAD_SLEEP_STATUS_0 (PINMUX_PERMIT = 0xf) -> wr_err
  store_fault_count = 0;
  *(volatile uint16_t *)(kPinmuxBase +
                         PINMUX_MIO_PAD_SLEEP_STATUS_0_REG_OFFSET) = 0xffffu;
  EXPECT_CHECK(store_fault_count == 1u,
               "sub-word write16 to MIO_PAD_SLEEP_STATUS_0 (PERMIT=0xf) did "
               "not fault: store_fault_count=%u",
               store_fault_count);

  // Sub-word write8 to DIO_PAD_SLEEP_STATUS (PINMUX_PERMIT = 0x3) -> wr_err,
  // whereas write16 (reg_be = 0x3) must succeed
  store_fault_count = 0;
  abs_mmio_write8(kPinmuxBase + PINMUX_DIO_PAD_SLEEP_STATUS_REG_OFFSET, 0xffu);
  EXPECT_CHECK(store_fault_count == 1u,
               "sub-word write8 to DIO_PAD_SLEEP_STATUS (PERMIT=0x3) did not "
               "fault: store_fault_count=%u",
               store_fault_count);
  store_fault_count = 0;
  *(volatile uint16_t *)(kPinmuxBase + PINMUX_DIO_PAD_SLEEP_STATUS_REG_OFFSET) =
      0xffffu;
  EXPECT_CHECK(store_fault_count == 0u,
               "sub-word write16 to DIO_PAD_SLEEP_STATUS (PERMIT=0x3, "
               "reg_be=0x3) faulted: store_fault_count=%u",
               store_fault_count);

  // Sub-word write8 to WKUP_DETECTOR_CNT_TH_0 (PINMUX_PERMIT = 0x1):
  // byte 0 (reg_be = 0x1) must succeed; byte 1 (reg_be = 0x2) must fault
  store_fault_count = 0;
  abs_mmio_write8(kPinmuxBase + PINMUX_WKUP_DETECTOR_CNT_TH_0_REG_OFFSET,
                  0x5au);
  EXPECT_CHECK(
      store_fault_count == 0u &&
          abs_mmio_read32(kPinmuxBase +
                          PINMUX_WKUP_DETECTOR_CNT_TH_0_REG_OFFSET) == 0x5au,
      "sub-word write8 to WKUP_DETECTOR_CNT_TH_0 byte 0 failed: faults=%u "
      "val=0x%x",
      store_fault_count,
      abs_mmio_read32(kPinmuxBase + PINMUX_WKUP_DETECTOR_CNT_TH_0_REG_OFFSET));
  store_fault_count = 0;
  abs_mmio_write8(kPinmuxBase + PINMUX_WKUP_DETECTOR_CNT_TH_0_REG_OFFSET + 1u,
                  0x11u);
  EXPECT_CHECK(store_fault_count == 1u,
               "sub-word write8 to WKUP_DETECTOR_CNT_TH_0 byte 1 "
               "(PERMIT=0x1, reg_be=0x2) did not fault: store_fault_count=%u",
               store_fault_count);

  // Unmapped offset 0x8e0 (addrmiss inside 0x1000 window)
  load_fault_count = 0;
  store_fault_count = 0;
  (void)abs_mmio_read32(kPinmuxBase + 0x8e0u);
  abs_mmio_write32(kPinmuxBase + 0x8e0u, 0u);
  EXPECT_CHECK(load_fault_count == 1u && store_fault_count == 1u,
               "unmapped offset 0x8e0 did not fault: load=%u store=%u",
               load_fault_count, store_fault_count);

  LOG_INFO("pinmux_rtl_consistency_test completed with %u failures", failures);
  return failures == 0;
}
