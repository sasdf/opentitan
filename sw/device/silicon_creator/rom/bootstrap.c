// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/silicon_creator/lib/bootstrap.h"

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/hardened.h"
#include "sw/device/silicon_creator/lib/base/chip.h"
#include "sw/device/silicon_creator/lib/drivers/flash_ctrl.h"
#include "sw/device/silicon_creator/lib/drivers/otp.h"
#include "sw/device/silicon_creator/lib/drivers/uart.h"
#include "sw/device/silicon_creator/lib/error.h"

#include "flash_ctrl_regs.h"
#include "gpio_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "otp_ctrl_regs.h"

rom_error_t bootstrap_chip_erase(void) {
#ifdef OT_COVERAGE_INSTRUMENTED
  flash_ctrl_data_default_perms_set((flash_ctrl_perms_t) {
    .read = kMultiBitBool4False,
    .write = kMultiBitBool4False,
    .erase = kMultiBitBool4True,
  });
  for (uint32_t addr = 0; addr < TOP_EARLGREY_EFLASH_SIZE_BYTES - 0x20000; addr += FLASH_CTRL_PARAM_BYTES_PER_PAGE) {
    HARDENED_RETURN_IF_ERROR(flash_ctrl_data_erase(addr, kFlashCtrlEraseTypePage));
  }
  flash_ctrl_data_default_perms_set((flash_ctrl_perms_t) {
    .read = kMultiBitBool4False,
    .write = kMultiBitBool4False,
    .erase = kMultiBitBool4False,
  });
  return kErrorOk;

#else
  flash_ctrl_bank_erase_perms_set(kHardenedBoolTrue);
  rom_error_t err_0 = flash_ctrl_data_erase(0, kFlashCtrlEraseTypeBank);
  rom_error_t err_1 = flash_ctrl_data_erase(FLASH_CTRL_PARAM_BYTES_PER_BANK,
                                            kFlashCtrlEraseTypeBank);
  flash_ctrl_bank_erase_perms_set(kHardenedBoolFalse);

  HARDENED_RETURN_IF_ERROR(err_0);
  return err_1;
#endif
}

rom_error_t bootstrap_erase_verify(void) {
#ifdef OT_COVERAGE_INSTRUMENTED
  for (uint32_t addr = 0; addr < TOP_EARLGREY_EFLASH_SIZE_BYTES - 0x20000; addr += FLASH_CTRL_PARAM_BYTES_PER_PAGE) {
    HARDENED_RETURN_IF_ERROR(flash_ctrl_data_erase_verify(addr, kFlashCtrlEraseTypePage));
  }
  return kErrorOk;
#else
  rom_error_t err_0 = flash_ctrl_data_erase_verify(0, kFlashCtrlEraseTypeBank);
  rom_error_t err_1 = flash_ctrl_data_erase_verify(
      FLASH_CTRL_PARAM_BYTES_PER_BANK, kFlashCtrlEraseTypeBank);
  HARDENED_RETURN_IF_ERROR(err_0);
  return err_1;
#endif
}

hardened_bool_t bootstrap_requested(void) {
  uint32_t bootstrap_dis =
      otp_read32(OTP_CTRL_PARAM_OWNER_SW_CFG_ROM_BOOTSTRAP_DIS_OFFSET);
  if (launder32(bootstrap_dis) == kHardenedBoolTrue) {
    return kHardenedBoolFalse;
  }
  HARDENED_CHECK_NE(bootstrap_dis, kHardenedBoolTrue);

  // A single read is sufficient since we expect strong pull-ups on the strap
  // pins.  We assume pinmux has already been configured (by pinmux_init) to
  // enable the internal pull-downs on the strap pins.  As such, an external
  // weak pull-up will not be detected as a logic-high.
  uint32_t res = launder32(kHardenedBoolTrue) ^ SW_STRAP_BOOTSTRAP;
  res ^=
      abs_mmio_read32(TOP_EARLGREY_GPIO_BASE_ADDR + GPIO_DATA_IN_REG_OFFSET) &
      SW_STRAP_MASK;
  if (launder32(res) != kHardenedBoolTrue) {
    return kHardenedBoolFalse;
  }
  HARDENED_CHECK_EQ(res, kHardenedBoolTrue);
  return res;
}

rom_error_t bootstrap(void) {
  hardened_bool_t requested = bootstrap_requested();
  if (launder32(requested) != kHardenedBoolTrue) {
    return kErrorBootstrapNotRequested;
  }
  HARDENED_CHECK_EQ(requested, kHardenedBoolTrue);

  return enter_bootstrap();
}
