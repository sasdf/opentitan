// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/testing/entropy_testutils.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "rv_core_ibex_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kIbexBase = TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR,
};

bool test_main(void) {
  // 1. Stop the entropy complex (EDN0/EDN1/CSRNG/ENTROPY_SRC) so no new entropy
  // words are delivered to rv_core_ibex.
  CHECK_STATUS_OK(entropy_testutils_stop_all());

  // Drain any buffered 32-bit slices in prim_edn_req (u_edn_if) and rnd_data_q.
  for (int i = 0; i < 8; ++i) {
    (void)abs_mmio_read32(kIbexBase + RV_CORE_IBEX_RND_DATA_REG_OFFSET);
  }

  // Wait >= 50 us (> 500 ns) so QEMU's rnd_inval_until_ns expires and
  // ot_ibex_wrapper_read_rnd_data / ot_ibex_wrapper_read_rnd_status take the
  // RND_DATA_VALID == 0 path (hw/opentitan/ot_ibex_wrapper.c:1051-1074).
  busy_spin_micros(50);

  uint32_t rnd_status_disabled =
      abs_mmio_read32(kIbexBase + RV_CORE_IBEX_RND_STATUS_REG_OFFSET);
  CHECK(rnd_status_disabled == 0u);

  uint32_t rnd_data_disabled =
      abs_mmio_read32(kIbexBase + RV_CORE_IBEX_RND_DATA_REG_OFFSET);
  CHECK(rnd_data_disabled == 0u);

  // 2. Re-enable the entropy complex in auto mode and verify
  // RND_STATUS.RND_DATA_VALID becomes 1.
  CHECK_STATUS_OK(entropy_testutils_auto_mode_init());
  bool valid = false;
  for (int i = 0; i < 1000; ++i) {
    uint32_t status =
        abs_mmio_read32(kIbexBase + RV_CORE_IBEX_RND_STATUS_REG_OFFSET);
    if (status & (1u << RV_CORE_IBEX_RND_STATUS_RND_DATA_VALID_BIT)) {
      valid = true;
      break;
    }
    busy_spin_micros(10);
  }
  CHECK(valid);

  return true;
}
