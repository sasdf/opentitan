// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/status.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"
#include "sw/device/silicon_creator/lib/drivers/retention_sram.h"
#include "sw/device/silicon_creator/lib/drivers/rstmgr.h"
#include "sw/device/silicon_creator/lib/drivers/flash_ctrl.h"
#include "sw/device/silicon_creator/lib/ownership/owner_block.h"

OTTF_DEFINE_TEST_CONFIG();

bool test_main(void) {
  retention_sram_t *retram = retention_sram_get();

  // read the active owner block page 0 from flash
  owner_block_t block;
  CHECK(flash_ctrl_info_read(&kFlashCtrlInfoPageOwnerSlot0, 0,
                             sizeof(block) / sizeof(uint32_t),
                             &block) == kErrorOk);
  LOG_INFO("owner_page_0: %d", block.config_version);

  if (bitfield_bit32_read(retram->creator.reset_reasons,
                          kRstmgrReasonPowerOn)) {
    // It should be the default test owner.
    CHECK(block.config_version == 1);

    // modify it (unsigned)
    block.config_version = 5;

    // write the upgrade request to page 1
    CHECK(flash_ctrl_info_erase(&kFlashCtrlInfoPageOwnerSlot1,
                                kFlashCtrlEraseTypePage) == kErrorOk);
    CHECK(flash_ctrl_info_write(&kFlashCtrlInfoPageOwnerSlot1, 0,
                                sizeof(block) / sizeof(uint32_t),
                                &block) == kErrorOk);
    rstmgr_reset();
    return false;
  } else {
    // check if the unsigned owner is accepted.
    return block.config_version == 5;
  }
}
