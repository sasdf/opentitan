// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"
#include "sw/device/silicon_creator/lib/base/sec_mmio.h"
#include "sw/device/silicon_creator/lib/drivers/keymgr.h"
#include "sw/device/silicon_creator/lib/drivers/kmac.h"
#include "sw/device/silicon_creator/lib/error.h"

#include "hw/ip/keymgr/data/keymgr_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

/**
 * Verify that when booted via ROM_EXT, keymgr has been advanced to the
 * OwnerKey state and software binding registers are unlocked.
 */
rom_error_t keymgr_owner_key_state_test(void) {
  RETURN_IF_ERROR(sc_keymgr_state_check(kScKeymgrStateOwnerKey));
  CHECK(abs_mmio_read32(TOP_EARLGREY_KEYMGR_BASE_ADDR +
                        KEYMGR_SW_BINDING_REGWEN_REG_OFFSET) == 1,
        "Keymgr SW binding should be unlocked in OwnerKey state.");
  return kErrorOk;
}

/**
 * Key manager disable test.
 */
rom_error_t keymgr_disable_test(void) {
  sc_keymgr_disable();
  RETURN_IF_ERROR(sc_keymgr_state_check(kScKeymgrStateDisabled));
  return kErrorOk;
}

bool test_main(void) {
  status_t result = OK_STATUS();

  sec_mmio_init();
  CHECK(kmac_keymgr_configure() == kErrorOk);

  EXECUTE_TEST(result, keymgr_owner_key_state_test);
  EXECUTE_TEST(result, keymgr_disable_test);
  return status_ok(result);
}
