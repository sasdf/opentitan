// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * Physical CW340 FPGA Errata Confirmation & Discovery Test for `padring` (v2).
 *
 * Verifies:
 * 1. [prim_pad_wrapper.sv:50,76-98] (`CONFIRMED_PRESENT_ON_V2`):
 *    - In `hw/ip/prim_xilinx_ultrascale/rtl/prim_pad_wrapper.sv` (lines 50,
 *      76-98), `out = out_i ^ attr_i.invert`, `ie = ie_i &
 * ~attr_i.input_disable`, `in_raw_o = ie ? in : 1'b0`, and `in_o =
 * attr_i.invert ^ in_raw_o`.
 *    - Across the bidirectional `IOBUF` loopback (`oe_i = 1, input_disable =
 * 0`), setting `invert = 1` (`0x01`) inverts `out_i` on the physical pad
 * (`out`) and inverts `in` a second time on the input path (`in_o = 1 ^ ~out_i
 * = out_i`), causing double-inversion cancellation so `GPIO.DATA_IN` reads
 * `out_i` unchanged.
 *    - Setting `input_disable = 1` (`0x80`) gates `in_raw_o` to `1'b0` BEFORE
 *      `in_o = attr_i.invert ^ in_raw_o`, so `MIO_PAD_ATTR = 0x80` forces
 *      `in_o = 0` (even when driving `out_i = 1`) and `MIO_PAD_ATTR = 0x81`
 *      (`input_disable = 1, invert = 1`) forces `in_o = 1` (even when driving
 *      `out_i = 0`).
 *    - `prim_pad_attr` (`hw/ip/prim_xilinx/rtl/prim_pad_attr.sv`) implements
 *      WARL mask `0x83` (`input_disable | virtual_od_en | invert`) for all 47
 *      `MIO_PAD_ATTR` (`BidirStd` and `BidirOd`) and 14 `BidirStd`/`BidirOd`
 *      `DIO_PAD_ATTR` registers, and `0x81` (`input_disable | invert`) for the
 *      2 `InputStd` `DIO_PAD_ATTR` registers (`DioSpiDeviceSck` = 12,
 *      `DioSpiDeviceCsb` = 13), while `PadType == BidirOd` drives push-pull
 *      when `virtual_od_en == 0`.
 *
 * 2. [chip_earlgrey_cw340.sv:647-692] (`NEW_IN_V2`):
 *    - In `hw/top_earlgrey/rtl/autogen/chip_earlgrey_cw340.sv` (lines 147-148,
 *      647-692), `DioUsbdevUsbDp` (`DIO[0]`) and `DioUsbdevUsbDn` (`DIO[1]`)
 *      are parameterized as `BidirStd` in `PinmuxTargetCfg`, so
 *      `PINMUX.DIO_PAD_ATTR[0..1]` accept and read back WARL mask `0x83`
 *      (`input_disable | virtual_od_en | invert`).
 *    - However, `chip_earlgrey_cw340.sv:690-691` routes
 *      `dio_attr[DioUsbdevUsbDp]` and `dio_attr[DioUsbdevUsbDn]` into
 *      `unused_usb_sigs` and hardcodes the physical USB pad attributes to `'0`.
 *    - Consequently, writing `0x80` (`input_disable=1, invert=0`) vs `0x81`
 *      (`input_disable=1, invert=1`) to `PINMUX.DIO_PAD_ATTR[0..1]` latches
 *      `0x80` / `0x81` in `DIO_PAD_ATTR[0..1]` but has zero effect on
 *      `dio_in[DioUsbdevUsbDp/Dn]` (`USBDEV.PHY_PINS_SENSE`
 * `rx_dp_s`/`rx_dn_s`).
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top/gpio_regs.h"
#include "hw/top/pinmux_regs.h"
#include "hw/top/usbdev_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG(.ignore_alerts = true);

enum {
  kPinmuxBase = TOP_EARLGREY_PINMUX_BASE_ADDR,
  kGpioBase = TOP_EARLGREY_GPIO_BASE_ADDR,
  kUsbdevBase = TOP_EARLGREY_USBDEV_BASE_ADDR,

  /* MIO pad 9 (`Iob0`, `BidirStd`) & MIO pad 6 (`Ioa6`, `BidirOd`) */
  kTestMioStdPad = kTopEarlgreyPinmuxMioOutIob0,
  kTestMioStdInsel = kTopEarlgreyPinmuxInselIob0,
  kTestMioOdPad = kTopEarlgreyPinmuxMioOutIoa6,
  kTestMioOdInsel = kTopEarlgreyPinmuxInselIoa6,

  /* GPIO channel 16 and 17 */
  kTestGpioStdBit = 16u,
  kTestGpioStdMask = (1u << kTestGpioStdBit),
  kTestGpioStdInselIdx = kTopEarlgreyPinmuxPeripheralInGpioGpio16,
  kTestGpioStdOutselVal = kTopEarlgreyPinmuxOutselGpioGpio16,

  kTestGpioOdBit = 17u,
  kTestGpioOdMask = (1u << kTestGpioOdBit),
  kTestGpioOdInselIdx = kTopEarlgreyPinmuxPeripheralInGpioGpio17,
  kTestGpioOdOutselVal = kTopEarlgreyPinmuxOutselGpioGpio17,

  /* Pad attribute bits: bit 0 = invert, bit 1 = virtual_od_en, bit 7 =
     input_disable */
  kPadAttrInvert = 0x01u,
  kPadAttrVirtOdEn = 0x02u,
  kPadAttrInputDisable = 0x80u,
  kPadAttrWarlBidir = kPadAttrInputDisable | kPadAttrVirtOdEn | kPadAttrInvert,
  kPadAttrWarlInputStd = kPadAttrInputDisable | kPadAttrInvert,
};

static inline uint32_t mio_insel_addr(uint32_t idx) {
  return kPinmuxBase + PINMUX_MIO_PERIPH_INSEL_0_REG_OFFSET + (idx * 4u);
}

static inline uint32_t mio_outsel_addr(uint32_t pad) {
  return kPinmuxBase + PINMUX_MIO_OUTSEL_0_REG_OFFSET + (pad * 4u);
}

static inline uint32_t mio_pad_attr_addr(uint32_t pad) {
  return kPinmuxBase + PINMUX_MIO_PAD_ATTR_0_REG_OFFSET + (pad * 4u);
}

static inline uint32_t dio_pad_attr_addr(uint32_t pad) {
  return kPinmuxBase + PINMUX_DIO_PAD_ATTR_0_REG_OFFSET + (pad * 4u);
}

static void gpio_drive_bit(uint32_t mask, bool high) {
  uint32_t shift = (mask >= 0x10000u) ? 16u : 0u;
  uint32_t reg_offset = (mask >= 0x10000u) ? GPIO_MASKED_OUT_UPPER_REG_OFFSET
                                           : GPIO_MASKED_OUT_LOWER_REG_OFFSET;
  uint16_t m16 = (uint16_t)(mask >> shift);
  uint16_t v16 = high ? m16 : 0u;
  abs_mmio_write32(kGpioBase + reg_offset, ((uint32_t)m16 << 16) | v16);
}

static void gpio_oe_bit(uint32_t mask, bool enable) {
  uint32_t shift = (mask >= 0x10000u) ? 16u : 0u;
  uint32_t reg_offset = (mask >= 0x10000u) ? GPIO_MASKED_OE_UPPER_REG_OFFSET
                                           : GPIO_MASKED_OE_LOWER_REG_OFFSET;
  uint16_t m16 = (uint16_t)(mask >> shift);
  uint16_t v16 = enable ? m16 : 0u;
  abs_mmio_write32(kGpioBase + reg_offset, ((uint32_t)m16 << 16) | v16);
}

static bool gpio_sample_bit(uint32_t bit) {
  busy_spin_micros(2);
  return ((abs_mmio_read32(kGpioBase + GPIO_DATA_IN_REG_OFFSET) >> bit) & 1u) !=
         0u;
}

/**
 * Part 1: Verify [prim_pad_wrapper.sv:50,76-98] (`CONFIRMED_PRESENT_ON_V2`) on
 * physical CW340 FPGA:
 * - Double-inversion cancellation across `IOBUF` loopback when `invert = 1`
 *   and `input_disable = 0`.
 * - Pre-inversion clamping `in_raw_o = 0` when `input_disable = 1`, causing
 *   `in_o == attr_i.invert` (`0` for `0x80`, `1` for `0x81`).
 * - `PadType == BidirOd` (`Ioa6`) actively drives high (`DATA_IN == 1`) when
 *   `virtual_od_en == 0` because `od_en` is unimplemented (`WARL = 0x83`).
 * - `MIO_PAD_ATTR` and `DIO_PAD_ATTR` WARL masks across all 47 MIO and 16 DIO
 *   pads (`0x83` for `BidirStd`/`BidirOd`, `0x81` for `InputStd`).
 */
static void test_errata_padring_001_confirmed_on_v2(void) {
  LOG_INFO("Testing [prim_pad_wrapper.sv:50,76-98] on v2 CW340 FPGA...");

  uint32_t orig_std_insel =
      abs_mmio_read32(mio_insel_addr(kTestGpioStdInselIdx));
  uint32_t orig_std_outsel = abs_mmio_read32(mio_outsel_addr(kTestMioStdPad));
  uint32_t orig_std_attr = abs_mmio_read32(mio_pad_attr_addr(kTestMioStdPad));

  uint32_t orig_od_insel = abs_mmio_read32(mio_insel_addr(kTestGpioOdInselIdx));
  uint32_t orig_od_outsel = abs_mmio_read32(mio_outsel_addr(kTestMioOdPad));
  uint32_t orig_od_attr = abs_mmio_read32(mio_pad_attr_addr(kTestMioOdPad));

  /* Route GPIO[16] <-> MIO[9] (`Iob0`, `BidirStd`) and GPIO[17] <-> MIO[6]
   * (`Ioa6`, `BidirOd`). */
  abs_mmio_write32(mio_insel_addr(kTestGpioStdInselIdx), kTestMioStdInsel);
  abs_mmio_write32(mio_outsel_addr(kTestMioStdPad), kTestGpioStdOutselVal);
  abs_mmio_write32(mio_pad_attr_addr(kTestMioStdPad), 0u);

  abs_mmio_write32(mio_insel_addr(kTestGpioOdInselIdx), kTestMioOdInsel);
  abs_mmio_write32(mio_outsel_addr(kTestMioOdPad), kTestGpioOdOutselVal);
  abs_mmio_write32(mio_pad_attr_addr(kTestMioOdPad), 0u);

  gpio_oe_bit(kTestGpioStdMask, true);
  gpio_oe_bit(kTestGpioOdMask, true);

  /* 1A: Normal loopback (`attr = 0x00`). */
  gpio_drive_bit(kTestGpioStdMask, true);
  CHECK(gpio_sample_bit(kTestGpioStdBit) == true,
        "Expected GPIO[16] loopback=1 when out_i=1, attr=0x00");
  gpio_drive_bit(kTestGpioStdMask, false);
  CHECK(gpio_sample_bit(kTestGpioStdBit) == false,
        "Expected GPIO[16] loopback=0 when out_i=0, attr=0x00");

  /* 1B: `invert = 1` (`attr = 0x01`) double-inversion cancellation across
   * IOBUF: `out = out_i ^ 1` -> `in = out` -> `in_o = 1 ^ in = out_i`! */
  abs_mmio_write32(mio_pad_attr_addr(kTestMioStdPad), kPadAttrInvert);
  CHECK(abs_mmio_read32(mio_pad_attr_addr(kTestMioStdPad)) == kPadAttrInvert);

  gpio_drive_bit(kTestGpioStdMask, true);
  bool inv_loop_high = gpio_sample_bit(kTestGpioStdBit);
  gpio_drive_bit(kTestGpioStdMask, false);
  bool inv_loop_low = gpio_sample_bit(kTestGpioStdBit);
  LOG_INFO(
      "[prim_pad_wrapper.sv:50,98] (invert=1 loopback): out_i=1 -> in_o=%d, "
      "out_i=0 -> in_o=%d",
      inv_loop_high, inv_loop_low);
  CHECK(inv_loop_high == true && inv_loop_low == false,
        "Expected double-inversion cancellation (in_o == out_i) across IOBUF");

  /* 1C: `input_disable = 1` (`0x80` vs `0x81`) clamps `in_raw_o = 0` BEFORE
   * `in_o = attr_i.invert ^ in_raw_o`, so `in_o == attr_i.invert` regardless of
   * `out_i`. */
  abs_mmio_write32(mio_pad_attr_addr(kTestMioStdPad), kPadAttrInputDisable);
  CHECK(abs_mmio_read32(mio_pad_attr_addr(kTestMioStdPad)) ==
        kPadAttrInputDisable);
  gpio_drive_bit(kTestGpioStdMask, true);
  bool dis_noinv_drive1 = gpio_sample_bit(kTestGpioStdBit);
  gpio_drive_bit(kTestGpioStdMask, false);
  bool dis_noinv_drive0 = gpio_sample_bit(kTestGpioStdBit);
  CHECK(dis_noinv_drive1 == false && dis_noinv_drive0 == false,
        "Expected input_disable=1, invert=0 (0x80) to clamp in_o=0");

  abs_mmio_write32(mio_pad_attr_addr(kTestMioStdPad),
                   kPadAttrInputDisable | kPadAttrInvert);
  CHECK(abs_mmio_read32(mio_pad_attr_addr(kTestMioStdPad)) ==
        (kPadAttrInputDisable | kPadAttrInvert));
  gpio_drive_bit(kTestGpioStdMask, false);
  bool dis_inv_drive0 = gpio_sample_bit(kTestGpioStdBit);
  gpio_drive_bit(kTestGpioStdMask, true);
  bool dis_inv_drive1 = gpio_sample_bit(kTestGpioStdBit);
  LOG_INFO(
      "[prim_pad_wrapper.sv:76,97-98] (input_disable=1): attr=0x80 -> (%d,%d), "
      "attr=0x81 -> (%d,%d)",
      dis_noinv_drive0, dis_noinv_drive1, dis_inv_drive0, dis_inv_drive1);
  CHECK(dis_inv_drive0 == true && dis_inv_drive1 == true,
        "Expected input_disable=1, invert=1 (0x81) to force in_o=1");

  /* 1D: `PadType == BidirOd` (`Ioa6`, MIO[6]) drives push-pull when
   * `virtual_od_en == 0` and ignores `od_en` (`0x40`). */
  abs_mmio_write32(mio_pad_attr_addr(kTestMioOdPad), 0xFFFFFFFFu);
  uint32_t ioa6_warl = abs_mmio_read32(mio_pad_attr_addr(kTestMioOdPad));
  CHECK(ioa6_warl == kPadAttrWarlBidir,
        "Expected BidirOd pad Ioa6 WARL mask 0x83, got 0x%x", ioa6_warl);
  abs_mmio_write32(mio_pad_attr_addr(kTestMioOdPad), 0u);
  gpio_drive_bit(kTestGpioOdMask, false);
  CHECK(gpio_sample_bit(kTestGpioOdBit) == false);
  gpio_drive_bit(kTestGpioOdMask, true);
  CHECK(
      gpio_sample_bit(kTestGpioOdBit) == true,
      "Expected BidirOd pad Ioa6 to drive push-pull high when virtual_od_en=0");

  /* Restore GPIO/MIO state. */
  gpio_oe_bit(kTestGpioStdMask, false);
  gpio_oe_bit(kTestGpioOdMask, false);
  abs_mmio_write32(mio_pad_attr_addr(kTestMioStdPad), orig_std_attr);
  abs_mmio_write32(mio_outsel_addr(kTestMioStdPad), orig_std_outsel);
  abs_mmio_write32(mio_insel_addr(kTestGpioStdInselIdx), orig_std_insel);
  abs_mmio_write32(mio_pad_attr_addr(kTestMioOdPad), orig_od_attr);
  abs_mmio_write32(mio_outsel_addr(kTestMioOdPad), orig_od_outsel);
  abs_mmio_write32(mio_insel_addr(kTestGpioOdInselIdx), orig_od_insel);

  /* 1E: Verify WARL mask across all 47 MIO pads (`0x83`) and 16 DIO pads
   * (`0x81` for DIO[12..13] `InputStd`, `0x83` for all other 14
   * `BidirStd`/`BidirOd` DIOs). */
  uint32_t mio_bidir_count = 0;
  for (uint32_t k = 0; k < PINMUX_PARAM_N_MIO_PADS; ++k) {
    uint32_t saved = abs_mmio_read32(mio_pad_attr_addr(k));
    abs_mmio_write32(mio_pad_attr_addr(k), 0xFFFFFFFFu);
    uint32_t warl = abs_mmio_read32(mio_pad_attr_addr(k));
    abs_mmio_write32(mio_pad_attr_addr(k), saved);
    CHECK(warl == kPadAttrWarlBidir,
          "MIO_PAD_ATTR[%u] expected WARL 0x83, got 0x%x", k, warl);
    mio_bidir_count++;
  }
  CHECK(mio_bidir_count == 47u);

  uint32_t dio_bidir_count = 0;
  uint32_t dio_input_std_count = 0;
  for (uint32_t k = 0; k < PINMUX_PARAM_N_DIO_PADS; ++k) {
    uint32_t saved = abs_mmio_read32(dio_pad_attr_addr(k));
    abs_mmio_write32(dio_pad_attr_addr(k), 0xFFFFFFFFu);
    uint32_t warl = abs_mmio_read32(dio_pad_attr_addr(k));
    abs_mmio_write32(dio_pad_attr_addr(k), saved);
    if (k == kTopEarlgreyDirectPadsSpiDeviceSck ||
        k == kTopEarlgreyDirectPadsSpiDeviceCsb) {
      CHECK(warl == kPadAttrWarlInputStd,
            "DIO_PAD_ATTR[%u] (InputStd) expected WARL 0x81, got 0x%x", k,
            warl);
      dio_input_std_count++;
    } else {
      CHECK(warl == kPadAttrWarlBidir,
            "DIO_PAD_ATTR[%u] (Bidir) expected WARL 0x83, got 0x%x", k, warl);
      dio_bidir_count++;
    }
  }
  CHECK(dio_input_std_count == 2u && dio_bidir_count == 14u);
  LOG_INFO(
      "[prim_pad_attr.sv:30-45] confirmed: 47 MIO (0x83), 14 Bidir DIO (0x83), "
      "2 InputStd DIO (0x81).");
}

/**
 * Part 2: Verify [chip_earlgrey_cw340.sv:647-692] (`NEW_IN_V2`) on physical
 * CW340 FPGA:
 * - `PINMUX.DIO_PAD_ATTR[0]` (`DioUsbdevUsbDp`) and `PINMUX.DIO_PAD_ATTR[1]`
 *   (`DioUsbdevUsbDn`) accept and read back `0x83` (`input_disable |
 *   virtual_od_en | invert`) because `TargetCfg.dio_pad_type[0..1]` is
 *   `BidirStd`.
 * - However, `chip_earlgrey_cw340.sv:690-691` ties `dio_attr[DioUsbdevUsbDp]`
 *   and `dio_attr[DioUsbdevUsbDn]` to `unused_usb_sigs` while leaving the
 *   physical USB pad attributes at `'0`.
 * - Consequently, writing `0x80` (`input_disable=1, invert=0`) vs `0x81`
 *   (`input_disable=1, invert=1`) to `DIO_PAD_ATTR[0..1]` fails to clamp or
 *   invert `dio_in[DioUsbdevUsbDp/Dn]` in `USBDEV.PHY_PINS_SENSE` (`rx_dp_s`
 *   and `rx_dn_s` remain identical under `0x00`, `0x80`, and `0x81`).
 */
static void test_errata_padring_v2_001_unconnected_usb_dio_attr(void) {
  LOG_INFO("Testing [chip_earlgrey_cw340.sv:647-692] on v2 CW340 FPGA...");

  uint32_t dp_idx = kTopEarlgreyDirectPadsUsbdevUsbDp;
  uint32_t dn_idx = kTopEarlgreyDirectPadsUsbdevUsbDn;
  uint32_t orig_dp_attr = abs_mmio_read32(dio_pad_attr_addr(dp_idx));
  uint32_t orig_dn_attr = abs_mmio_read32(dio_pad_attr_addr(dn_idx));

  /* Set both DP and DN DIO_PAD_ATTR to 0x00 (`input_disable=0, invert=0`). */
  abs_mmio_write32(dio_pad_attr_addr(dp_idx), 0x00u);
  abs_mmio_write32(dio_pad_attr_addr(dn_idx), 0x00u);
  busy_spin_micros(2);
  uint32_t sense_attr_00 =
      abs_mmio_read32(kUsbdevBase + USBDEV_PHY_PINS_SENSE_REG_OFFSET) & 0x3u;

  /* Set both DP and DN DIO_PAD_ATTR to 0x80 (`input_disable=1, invert=0`).
   * If `dio_attr` were connected to `u_padring`, `in_raw_o` would be clamped
   * to 0 and `in_o` would be forced to 0 (`sense == 0x0`). */
  abs_mmio_write32(dio_pad_attr_addr(dp_idx), kPadAttrInputDisable);
  abs_mmio_write32(dio_pad_attr_addr(dn_idx), kPadAttrInputDisable);
  CHECK(abs_mmio_read32(dio_pad_attr_addr(dp_idx)) == kPadAttrInputDisable);
  CHECK(abs_mmio_read32(dio_pad_attr_addr(dn_idx)) == kPadAttrInputDisable);
  busy_spin_micros(2);
  uint32_t sense_attr_80 =
      abs_mmio_read32(kUsbdevBase + USBDEV_PHY_PINS_SENSE_REG_OFFSET) & 0x3u;

  /* Set both DP and DN DIO_PAD_ATTR to 0x81 (`input_disable=1, invert=1`).
   * If `dio_attr` were connected to `u_padring`, `in_raw_o` would be clamped
   * to 0 and `in_o = invert ^ 0` would be forced to 1 (`sense == 0x3`). */
  abs_mmio_write32(dio_pad_attr_addr(dp_idx),
                   kPadAttrInputDisable | kPadAttrInvert);
  abs_mmio_write32(dio_pad_attr_addr(dn_idx),
                   kPadAttrInputDisable | kPadAttrInvert);
  CHECK(abs_mmio_read32(dio_pad_attr_addr(dp_idx)) ==
        (kPadAttrInputDisable | kPadAttrInvert));
  CHECK(abs_mmio_read32(dio_pad_attr_addr(dn_idx)) ==
        (kPadAttrInputDisable | kPadAttrInvert));
  busy_spin_micros(2);
  uint32_t sense_attr_81 =
      abs_mmio_read32(kUsbdevBase + USBDEV_PHY_PINS_SENSE_REG_OFFSET) & 0x3u;

  /* Restore DP/DN DIO_PAD_ATTR. */
  abs_mmio_write32(dio_pad_attr_addr(dp_idx), orig_dp_attr);
  abs_mmio_write32(dio_pad_attr_addr(dn_idx), orig_dn_attr);

  LOG_INFO(
      "[chip_earlgrey_cw340.sv:647-692] DIO_PAD_ATTR[0..1] readback=0x80/0x81, "
      "USBDEV.PHY_PINS_SENSE[1:0]: attr_00=0x%x, attr_80=0x%x, attr_81=0x%x",
      sense_attr_00, sense_attr_80, sense_attr_81);

  /* Because `dio_attr[DioUsbdevUsbDp/Dn]` are discarded into `unused_usb_sigs`,
   * `sense_attr_80` and `sense_attr_81` do NOT toggle between 0x0 and 0x3;
   * instead `sense_attr_00 == sense_attr_80 == sense_attr_81`. */
  CHECK(sense_attr_80 == sense_attr_00 && sense_attr_81 == sense_attr_00,
        "Expected USBDEV.PHY_PINS_SENSE[1:0] to remain unchanged (0x%x) when "
        "DIO_PAD_ATTR[0..1] = 0x80 (0x%x) or 0x81 (0x%x)",
        sense_attr_00, sense_attr_80, sense_attr_81);
}

bool test_main(void) {
  test_errata_padring_001_confirmed_on_v2();
  test_errata_padring_v2_001_unconnected_usb_dio_attr();
  LOG_INFO("All padring v2 errata checks passed on physical CW340 FPGA.");
  return true;
}
