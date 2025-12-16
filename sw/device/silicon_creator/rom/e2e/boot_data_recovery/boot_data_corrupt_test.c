// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/status.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"
#include "sw/device/silicon_creator/lib/drivers/flash_ctrl.h"

OTTF_DEFINE_TEST_CONFIG();

const flash_ctrl_cfg_t without_ecc = {
  .he = kMultiBitBool4False,
  .scrambling = kMultiBitBool4False,
  .ecc = kMultiBitBool4False,
};

const flash_ctrl_cfg_t with_ecc = {
  .he = kMultiBitBool4False,
  .scrambling = kMultiBitBool4True,
  .ecc = kMultiBitBool4True,
};

#define SZ 512

uint32_t rbuf[SZ] = {0};
uint32_t wbuf[SZ] = {0};


bool test_main(void) {
  flash_ctrl_cert_info_page_creator_cfg(&kFlashCtrlInfoPageOwnerReserved0);
  flash_ctrl_cert_info_page_creator_cfg(&kFlashCtrlInfoPageOwnerReserved1);
  flash_ctrl_info_cfg_set(&kFlashCtrlInfoPageOwnerReserved0, with_ecc);
  flash_ctrl_info_cfg_set(&kFlashCtrlInfoPageOwnerReserved1, without_ecc);


  CHECK(flash_ctrl_info_read(&kFlashCtrlInfoPageOwnerReserved1, 0, 1, rbuf) == kErrorOk);
  if (rbuf[0] == 0) {
    LOG_INFO("Initialize test");
    CHECK(flash_ctrl_info_erase(&kFlashCtrlInfoPageOwnerReserved0, kFlashCtrlEraseTypePage) == kErrorOk);
    CHECK(flash_ctrl_info_erase(&kFlashCtrlInfoPageOwnerReserved1, kFlashCtrlEraseTypePage) == kErrorOk);
    uint32_t wbuf[1] = {0xdeadbeef};
    CHECK(flash_ctrl_info_write(&kFlashCtrlInfoPageOwnerReserved1, 0, 1, wbuf) == kErrorOk);
  }


  LOG_INFO("Checking");
  rom_error_t error = flash_ctrl_info_read(&kFlashCtrlInfoPageOwnerReserved0, 0, SZ, rbuf);
  if (error == kErrorFlashCtrlInfoRead) {
    return true;
  } else if (error != kErrorOk) {
    LOG_INFO("error: %x", error);
    return false;
  }

  LOG_INFO("Writing");
  while(1) {
    CHECK(flash_ctrl_info_erase(&kFlashCtrlInfoPageOwnerReserved0, kFlashCtrlEraseTypePage) == kErrorOk);
    CHECK(flash_ctrl_info_write(&kFlashCtrlInfoPageOwnerReserved0, 0, SZ, wbuf) == kErrorOk);
    for (uint32_t i=0; i<SZ; i++) {
      wbuf[i] += i + 1;
    }
  }
}
