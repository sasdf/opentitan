// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "rstmgr_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kRstmgrBase = TOP_EARLGREY_RSTMGR_AON_BASE_ADDR,
  kNumSwResets = RSTMGR_PARAM_NUM_SW_RESETS,
};

bool test_main(void) {
  // 1. W/O ALERT_TEST readback and R/O register write-ignore semantics
  // (ALERT_INFO_ATTR, ALERT_INFO, CPU_INFO_ATTR, CPU_INFO, ERR_CODE).
  CHECK(abs_mmio_read32(kRstmgrBase + RSTMGR_ALERT_TEST_REG_OFFSET) == 0u);

  uint32_t alert_info_attr_init =
      abs_mmio_read32(kRstmgrBase + RSTMGR_ALERT_INFO_ATTR_REG_OFFSET);
  uint32_t alert_info_init =
      abs_mmio_read32(kRstmgrBase + RSTMGR_ALERT_INFO_REG_OFFSET);
  uint32_t cpu_info_attr_init =
      abs_mmio_read32(kRstmgrBase + RSTMGR_CPU_INFO_ATTR_REG_OFFSET);
  uint32_t cpu_info_init =
      abs_mmio_read32(kRstmgrBase + RSTMGR_CPU_INFO_REG_OFFSET);
  uint32_t err_code_init =
      abs_mmio_read32(kRstmgrBase + RSTMGR_ERR_CODE_REG_OFFSET);

  CHECK(alert_info_attr_init == 9u);
  CHECK(cpu_info_attr_init == 8u);
  CHECK(err_code_init == 0u);

  abs_mmio_write32(kRstmgrBase + RSTMGR_ALERT_INFO_ATTR_REG_OFFSET,
                   0xffffffffu);
  abs_mmio_write32(kRstmgrBase + RSTMGR_ALERT_INFO_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kRstmgrBase + RSTMGR_CPU_INFO_ATTR_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kRstmgrBase + RSTMGR_CPU_INFO_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kRstmgrBase + RSTMGR_ERR_CODE_REG_OFFSET, 0xffffffffu);

  CHECK(abs_mmio_read32(kRstmgrBase + RSTMGR_ALERT_INFO_ATTR_REG_OFFSET) ==
        alert_info_attr_init);
  CHECK(abs_mmio_read32(kRstmgrBase + RSTMGR_ALERT_INFO_REG_OFFSET) ==
        alert_info_init);
  CHECK(abs_mmio_read32(kRstmgrBase + RSTMGR_CPU_INFO_ATTR_REG_OFFSET) ==
        cpu_info_attr_init);
  CHECK(abs_mmio_read32(kRstmgrBase + RSTMGR_CPU_INFO_REG_OFFSET) ==
        cpu_info_init);
  CHECK(abs_mmio_read32(kRstmgrBase + RSTMGR_ERR_CODE_REG_OFFSET) ==
        err_code_init);

  // 2. ALERT_REGWEN and CPU_REGWEN RW0C locking of ALERT_INFO_CTRL and
  // CPU_INFO_CTRL, plus out-of-bounds ALERT_INFO index (index >= 9 returns 0).
  CHECK(abs_mmio_read32(kRstmgrBase + RSTMGR_ALERT_REGWEN_REG_OFFSET) == 1u);
  CHECK(abs_mmio_read32(kRstmgrBase + RSTMGR_CPU_REGWEN_REG_OFFSET) == 1u);

  const uint32_t alert_ctrl_val =
      (9u << RSTMGR_ALERT_INFO_CTRL_INDEX_OFFSET);  // INDEX = 9, EN = 0
  const uint32_t cpu_ctrl_val =
      (3u << RSTMGR_CPU_INFO_CTRL_INDEX_OFFSET);  // INDEX = 3, EN = 0
  abs_mmio_write32(kRstmgrBase + RSTMGR_ALERT_INFO_CTRL_REG_OFFSET,
                   alert_ctrl_val);
  abs_mmio_write32(kRstmgrBase + RSTMGR_CPU_INFO_CTRL_REG_OFFSET, cpu_ctrl_val);

  CHECK(abs_mmio_read32(kRstmgrBase + RSTMGR_ALERT_INFO_CTRL_REG_OFFSET) ==
        alert_ctrl_val);
  CHECK(abs_mmio_read32(kRstmgrBase + RSTMGR_CPU_INFO_CTRL_REG_OFFSET) ==
        cpu_ctrl_val);
  CHECK(abs_mmio_read32(kRstmgrBase + RSTMGR_ALERT_INFO_REG_OFFSET) == 0u);

  // Lock ALERT_REGWEN and CPU_REGWEN (RW0C).
  abs_mmio_write32(kRstmgrBase + RSTMGR_ALERT_REGWEN_REG_OFFSET, 0u);
  abs_mmio_write32(kRstmgrBase + RSTMGR_CPU_REGWEN_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kRstmgrBase + RSTMGR_ALERT_REGWEN_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kRstmgrBase + RSTMGR_CPU_REGWEN_REG_OFFSET) == 0u);

  // Verify RW0C cannot be re-enabled by writing 1.
  abs_mmio_write32(kRstmgrBase + RSTMGR_ALERT_REGWEN_REG_OFFSET, 1u);
  abs_mmio_write32(kRstmgrBase + RSTMGR_CPU_REGWEN_REG_OFFSET, 1u);
  CHECK(abs_mmio_read32(kRstmgrBase + RSTMGR_ALERT_REGWEN_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kRstmgrBase + RSTMGR_CPU_REGWEN_REG_OFFSET) == 0u);

  // Verify writes to ALERT_INFO_CTRL and CPU_INFO_CTRL are ignored when locked.
  abs_mmio_write32(kRstmgrBase + RSTMGR_ALERT_INFO_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kRstmgrBase + RSTMGR_CPU_INFO_CTRL_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kRstmgrBase + RSTMGR_ALERT_INFO_CTRL_REG_OFFSET) ==
        alert_ctrl_val);
  CHECK(abs_mmio_read32(kRstmgrBase + RSTMGR_CPU_INFO_CTRL_REG_OFFSET) ==
        cpu_ctrl_val);

  // 3. SW_RST_REGWEN_0..7 RW0C locking of SW_RST_CTRL_N_0..7.
  for (uint32_t i = 0; i < kNumSwResets; ++i) {
    uint32_t regwen_addr = kRstmgrBase + RSTMGR_SW_RST_REGWEN_0_REG_OFFSET +
                           (i * sizeof(uint32_t));
    uint32_t ctrl_n_addr = kRstmgrBase + RSTMGR_SW_RST_CTRL_N_0_REG_OFFSET +
                           (i * sizeof(uint32_t));

    CHECK(abs_mmio_read32(regwen_addr) == 1u);
    CHECK(abs_mmio_read32(ctrl_n_addr) == 1u);

    // Clear SW_RST_REGWEN_i (RW0C) and verify writing 1 does not re-enable it.
    abs_mmio_write32(regwen_addr, 0u);
    CHECK(abs_mmio_read32(regwen_addr) == 0u);
    abs_mmio_write32(regwen_addr, 1u);
    CHECK(abs_mmio_read32(regwen_addr) == 0u);

    // Attempt to assert software reset (write 0 to SW_RST_CTRL_N_i) while
    // locked; verify write is ignored and SW_RST_CTRL_N_i remains 1.
    abs_mmio_write32(ctrl_n_addr, 0u);
    CHECK(abs_mmio_read32(ctrl_n_addr) == 1u);
  }

  return true;
}
