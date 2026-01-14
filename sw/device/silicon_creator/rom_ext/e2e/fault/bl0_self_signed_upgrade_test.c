// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/status.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"
#include "sw/device/silicon_creator/lib/drivers/flash_ctrl.h"
#include "sw/device/silicon_creator/lib/drivers/retention_sram.h"
#include "sw/device/silicon_creator/lib/drivers/rstmgr.h"
#include "sw/device/silicon_creator/lib/ownership/owner_block.h"
#include "sw/device/silicon_creator/rom_ext/e2e/fault/self_signed.bin.h"

OTTF_DEFINE_TEST_CONFIG();

bool test_main(void) {
  retention_sram_t *retram = retention_sram_get();

  // read the active owner block page 0 from flash
  owner_block_t block;
  CHECK(flash_ctrl_info_read(&kFlashCtrlInfoPageOwnerSlot0, 0,
                             sizeof(block) / sizeof(uint32_t),
                             &block) == kErrorOk);
  LOG_INFO("owner_page_0: %d", block.config_version);
  // print owner_key
  LOG_INFO("owner_key: %08x", block.owner_key.ecdsa.x[0]);

  if (bitfield_bit32_read(retram->creator.reset_reasons,
                          kRstmgrReasonPowerOn)) {
    // It should be the default test owner.
    CHECK(block.config_version == 1);
    CHECK(block.owner_key.ecdsa.x[0] == 0x8e3dcb50);

    // write the self-signed upgrade request to page 1
    CHECK(flash_ctrl_info_erase(&kFlashCtrlInfoPageOwnerSlot1,
                                kFlashCtrlEraseTypePage) == kErrorOk);
    CHECK(flash_ctrl_info_write(&kFlashCtrlInfoPageOwnerSlot1, 0,
                                self_signed_bin_len / sizeof(uint32_t),
                                self_signed_bin) == kErrorOk);
    rstmgr_reset();
    return false;
  } else {
    // check if the self-signed owner is accepted and owner is changed.
    CHECK(block.config_version == 2);
    CHECK(block.owner_key.ecdsa.x[0] == 0x665ff5e3);
    return true;
  }
}
