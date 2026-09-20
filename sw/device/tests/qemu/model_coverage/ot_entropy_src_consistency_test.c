// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "csrng_regs.h"
#include "edn_regs.h"
#include "entropy_src_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/irq.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

OTTF_DEFINE_TEST_CONFIG(.ignore_alerts = true);

enum {
  kEntropySrcBase = TOP_EARLGREY_ENTROPY_SRC_BASE_ADDR,
  kCsrngBase = TOP_EARLGREY_CSRNG_BASE_ADDR,
  kEdn0Base = TOP_EARLGREY_EDN0_BASE_ADDR,
  kEdn1Base = TOP_EARLGREY_EDN1_BASE_ADDR,
};

static volatile uint32_t load_fault_count;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = ibex_mcause_read();
  if (mcause == kIbexExcLoadAccessFault) {
    load_fault_count++;
  }
}

static uint32_t make_conf(uint32_t fips_en, uint32_t fips_flag,
                          uint32_t rng_fips, uint32_t rng_bit_en,
                          uint32_t thresh_scope, uint32_t data_reg_en) {
  return (fips_en << ENTROPY_SRC_CONF_FIPS_ENABLE_OFFSET) |
         (fips_flag << ENTROPY_SRC_CONF_FIPS_FLAG_OFFSET) |
         (rng_fips << ENTROPY_SRC_CONF_RNG_FIPS_OFFSET) |
         (rng_bit_en << ENTROPY_SRC_CONF_RNG_BIT_ENABLE_OFFSET) |
         (thresh_scope << ENTROPY_SRC_CONF_THRESHOLD_SCOPE_OFFSET) |
         (data_reg_en << ENTROPY_SRC_CONF_ENTROPY_DATA_REG_ENABLE_OFFSET);
}

static void disable_entropy_complex(void) {
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9999u);
  abs_mmio_write32(kEdn1Base + EDN_CTRL_REG_OFFSET, 0x9999u);
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, 0x9999u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET, 0x9u);
  while (abs_mmio_read32(kEntropySrcBase + ENTROPY_SRC_REGWEN_REG_OFFSET) ==
         0u) {
  }
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_FW_OV_CONTROL_REG_OFFSET,
                   0x99u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   0x9u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_ENTROPY_CONTROL_REG_OFFSET,
                   0x99u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_CONF_REG_OFFSET,
                   make_conf(9u, 9u, 9u, 9u, 9u, 9u));
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_HEALTH_TEST_WINDOWS_REG_OFFSET,
                   0x00600200u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_REPCNTS_THRESHOLDS_REG_OFFSET,
                   0xffffffffu);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_ADAPTP_LO_THRESHOLDS_REG_OFFSET,
                   0x0u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_MARKOV_LO_THRESHOLDS_REG_OFFSET,
                   0x0u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_ALERT_THRESHOLD_REG_OFFSET,
                   0xfffd0002u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET,
                   0x0u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET, 0xfu);
}

bool test_main(void) {
  irq_external_ctrl(false);
  irq_global_ctrl(false);

  disable_entropy_complex();

  // 1. Test REV register (0x1c), W/O reads (ALERT_TEST, FW_OV_WR_DATA -> 0),
  // R/O writes (REV, REGWEN, MAIN_SM_STATE -> ignored), and out-of-bounds reads
  // (0xe8, 0xfc -> load access fault).
  CHECK(abs_mmio_read32(kEntropySrcBase + ENTROPY_SRC_REV_REG_OFFSET) ==
        0x00010303u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_REV_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kEntropySrcBase + ENTROPY_SRC_REV_REG_OFFSET) ==
        0x00010303u);
  CHECK(abs_mmio_read32(kEntropySrcBase + ENTROPY_SRC_ALERT_TEST_REG_OFFSET) ==
        0u);
  CHECK(abs_mmio_read32(kEntropySrcBase +
                        ENTROPY_SRC_FW_OV_WR_DATA_REG_OFFSET) == 0u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_REGWEN_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kEntropySrcBase + ENTROPY_SRC_REGWEN_REG_OFFSET) ==
        1u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_MAIN_SM_STATE_REG_OFFSET, 0u);

  load_fault_count = 0;
  (void)abs_mmio_read32(kEntropySrcBase + 0xe8u);
  CHECK(load_fault_count == 1u, "Expected load access fault at 0xe8");

  load_fault_count = 0;
  (void)abs_mmio_read32(kEntropySrcBase + 0xfcu);
  CHECK(load_fault_count == 1u, "Expected load access fault at 0xfc");

  // 2. Invalid MuBi4 values in CONF, ENTROPY_CONTROL, FW_OV_CONTROL,
  // FW_OV_SHA3_START, and MODULE_ENABLE -> continuous RECOV_ALERT_STS bits.
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_CONF_REG_OFFSET, 0x00000000u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_ENTROPY_CONTROL_REG_OFFSET,
                   0x00u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_FW_OV_CONTROL_REG_OFFSET,
                   0x00u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   0x0u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   0x0u);

  uint32_t recov =
      abs_mmio_read32(kEntropySrcBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET);
  uint32_t expected_mubi_alerts =
      (1u << ENTROPY_SRC_RECOV_ALERT_STS_MODULE_ENABLE_FIELD_ALERT_BIT) |
      (1u << ENTROPY_SRC_RECOV_ALERT_STS_FIPS_ENABLE_FIELD_ALERT_BIT) |
      (1u << ENTROPY_SRC_RECOV_ALERT_STS_FIPS_FLAG_FIELD_ALERT_BIT) |
      (1u << ENTROPY_SRC_RECOV_ALERT_STS_RNG_FIPS_FIELD_ALERT_BIT) |
      (1u << ENTROPY_SRC_RECOV_ALERT_STS_RNG_BIT_ENABLE_FIELD_ALERT_BIT) |
      (1u << ENTROPY_SRC_RECOV_ALERT_STS_ENTROPY_DATA_REG_EN_FIELD_ALERT_BIT) |
      (1u << ENTROPY_SRC_RECOV_ALERT_STS_THRESHOLD_SCOPE_FIELD_ALERT_BIT) |
      (1u << ENTROPY_SRC_RECOV_ALERT_STS_ES_ROUTE_FIELD_ALERT_BIT) |
      (1u << ENTROPY_SRC_RECOV_ALERT_STS_ES_TYPE_FIELD_ALERT_BIT) |
      (1u << ENTROPY_SRC_RECOV_ALERT_STS_FW_OV_MODE_FIELD_ALERT_BIT) |
      (1u << ENTROPY_SRC_RECOV_ALERT_STS_FW_OV_ENTROPY_INSERT_FIELD_ALERT_BIT) |
      (1u << ENTROPY_SRC_RECOV_ALERT_STS_FW_OV_SHA3_START_FIELD_ALERT_BIT);
  CHECK((recov & expected_mubi_alerts) == expected_mubi_alerts,
        "Expected all 12 MuBi field alerts set, got 0x%x (expected 0x%x)",
        recov, expected_mubi_alerts);

  disable_entropy_complex();
  CHECK(abs_mmio_read32(kEntropySrcBase +
                        ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET) == 0u);

  // 3. ENTROPY_DATA reads when disabled & when ENTROPY_DATA_REG_ENABLE = 9 -> 0.
  CHECK(abs_mmio_read32(kEntropySrcBase +
                        ENTROPY_SRC_ENTROPY_DATA_REG_OFFSET) == 0u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   0x6u);
  CHECK(abs_mmio_read32(kEntropySrcBase +
                        ENTROPY_SRC_ENTROPY_DATA_REG_OFFSET) == 0u);
  disable_entropy_complex();

  // Enable CONF with FIPS_ENABLE = 9, FIPS_FLAG = 6, RNG_FIPS = 9,
  // RNG_BIT_ENABLE = 6, THRESHOLD_SCOPE = 9, ENTROPY_DATA_REG_ENABLE = 6.
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_CONF_REG_OFFSET,
                   make_conf(9u, 6u, 9u, 6u, 9u, 6u));
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_ENTROPY_CONTROL_REG_OFFSET,
                   0x66u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   0x6u);
  IBEX_SPIN_FOR(
      (abs_mmio_read32(kEntropySrcBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET) &
       (1u << ENTROPY_SRC_INTR_STATE_ES_ENTROPY_VALID_BIT)) != 0u,
      100000);
  for (int i = 0; i < 12; ++i) {
    (void)abs_mmio_read32(kEntropySrcBase +
                          ENTROPY_SRC_ENTROPY_DATA_REG_OFFSET);
  }

  // Also test CSRNG instantiation when ENTROPY_SRC is in bypass mode (ES_TYPE=6, ES_ROUTE=9)
  // and when FIPS_ENABLE=9, FIPS_FLAG=6 to verify ot_entropy_src_get_entropy.
  disable_entropy_complex();
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_CONF_REG_OFFSET,
                   make_conf(9u, 6u, 9u, 9u, 9u, 9u));
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_ENTROPY_CONTROL_REG_OFFSET,
                   0x69u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   0x6u);
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, 0x9666u);
  abs_mmio_write32(kCsrngBase + CSRNG_CMD_REQ_REG_OFFSET, 0x00000901u);
  IBEX_SPIN_FOR((abs_mmio_read32(kCsrngBase + CSRNG_SW_CMD_STS_REG_OFFSET) &
                 (1u << CSRNG_SW_CMD_STS_CMD_ACK_BIT)) != 0u,
                100000);

  disable_entropy_complex();
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_CONF_REG_OFFSET,
                   make_conf(9u, 6u, 9u, 9u, 9u, 9u));
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_ENTROPY_CONTROL_REG_OFFSET,
                   0x99u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   0x6u);
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, 0x9666u);
  abs_mmio_write32(kCsrngBase + CSRNG_CMD_REQ_REG_OFFSET, 0x00000901u);
  IBEX_SPIN_FOR((abs_mmio_read32(kCsrngBase + CSRNG_SW_CMD_STS_REG_OFFSET) &
                 (1u << CSRNG_SW_CMD_STS_CMD_ACK_BIT)) != 0u,
                100000);

  // 4. FW override mode: FW_OV_RD_DATA when disabled & when empty (SFIFO_OBSERVE_ERR),
  // OBSERVE_FIFO_THRESH IRQ, FW_OV_WR_DATA bypass & SHA3 insertion, and ES_FW_OV_DISABLE_ALERT.
  disable_entropy_complex();
  CHECK(abs_mmio_read32(kEntropySrcBase +
                        ENTROPY_SRC_FW_OV_RD_DATA_REG_OFFSET) == 0u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_FW_OV_WR_DATA_REG_OFFSET,
                   0x12345678u);

  // Enable with FW_OV_MODE = 6, FW_OV_ENTROPY_INSERT = 9, OBSERVE_FIFO_THRESH = 1
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_OBSERVE_FIFO_THRESH_REG_OFFSET,
                   1u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_FW_OV_CONTROL_REG_OFFSET,
                   0x96u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   0x6u);
  IBEX_SPIN_FOR(
      abs_mmio_read32(kEntropySrcBase +
                      ENTROPY_SRC_FW_OV_RD_FIFO_OVERFLOW_REG_OFFSET) == 1u,
      100000);
  // Clear INTR_STATE while observe FIFO >= threshold so ES_OBSERVE_FIFO_READY re-asserts
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET, 0xfu);
  CHECK((abs_mmio_read32(kEntropySrcBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET) &
         (1u << ENTROPY_SRC_INTR_STATE_ES_OBSERVE_FIFO_READY_BIT)) != 0u);
  // Drain observe FIFO completely and read 1 extra word to trigger SFIFO_OBSERVE_ERR | FIFO_READ_ERR
  for (int i = 0; i < 65; ++i) {
    (void)abs_mmio_read32(kEntropySrcBase +
                          ENTROPY_SRC_FW_OV_RD_DATA_REG_OFFSET);
  }
  uint32_t err_obs =
      abs_mmio_read32(kEntropySrcBase + ENTROPY_SRC_ERR_CODE_REG_OFFSET);
  uint32_t exp_obs = (1u << ENTROPY_SRC_ERR_CODE_SFIFO_OBSERVE_ERR_BIT) |
                     (1u << ENTROPY_SRC_ERR_CODE_FIFO_READ_ERR_BIT);
  CHECK((err_obs & exp_obs) == exp_obs,
        "Expected SFIFO_OBSERVE_ERR | FIFO_READ_ERR, got 0x%x", err_obs);

  // FW_OV_ENTROPY_INSERT = 6 in bypass mode (ES_TYPE = 6, ES_ROUTE = 6, ENTROPY_DATA_REG_ENABLE = 6)
  disable_entropy_complex();
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_CONF_REG_OFFSET,
                   make_conf(9u, 9u, 9u, 9u, 9u, 6u));
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_ENTROPY_CONTROL_REG_OFFSET,
                   0x66u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_FW_OV_CONTROL_REG_OFFSET,
                   0x66u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   0x6u);
  // Push 12 words (1 packet) via FW_OV_WR_DATA in bypass mode and verify exact
  // readback from ENTROPY_DATA.
  for (uint32_t i = 0; i < 12; ++i) {
    abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_FW_OV_WR_DATA_REG_OFFSET,
                     0x1000u + i);
  }
  CHECK((abs_mmio_read32(kEntropySrcBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET) &
         (1u << ENTROPY_SRC_INTR_STATE_ES_ENTROPY_VALID_BIT)) != 0u);
  for (uint32_t i = 0; i < 12; ++i) {
    uint32_t w =
        abs_mmio_read32(kEntropySrcBase + ENTROPY_SRC_ENTROPY_DATA_REG_OFFSET);
    CHECK(w == 0x1000u + i, "Expected bypass word 0x%x, got 0x%x", 0x1000u + i,
          w);
  }
  // Read 12 more words from empty ENTROPY_DATA to trigger SFIFO_ESFINAL_ERR
  for (uint32_t i = 0; i < 12; ++i) {
    (void)abs_mmio_read32(kEntropySrcBase +
                          ENTROPY_SRC_ENTROPY_DATA_REG_OFFSET);
  }
  CHECK((abs_mmio_read32(kEntropySrcBase + ENTROPY_SRC_ERR_CODE_REG_OFFSET) &
         (1u << ENTROPY_SRC_ERR_CODE_SFIFO_ESFINAL_ERR_BIT)) != 0u,
        "Expected SFIFO_ESFINAL_ERR after 12 reads from empty ENTROPY_DATA");
  // Toggle FW_OV_SHA3_START in bypass mode (6 -> 9)
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   0x6u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   0x9u);

  // FW_OV_ENTROPY_INSERT = 6 in SHA3 mode (ES_TYPE = 9):
  // Test odd word (1 word) -> FW_OV_WR_FIFO_FULL == 0 -> disabling FW_OV_SHA3_START
  // does NOT assert ES_FW_OV_DISABLE_ALERT (verifies entropy_src_core.sv:2691!).
  disable_entropy_complex();
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_CONF_REG_OFFSET,
                   make_conf(6u, 6u, 9u, 9u, 9u, 6u));
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_ENTROPY_CONTROL_REG_OFFSET,
                   0x96u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_FW_OV_CONTROL_REG_OFFSET,
                   0x66u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   0x6u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   0x6u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_FW_OV_WR_DATA_REG_OFFSET,
                   0xdeadbeefu);
  CHECK(abs_mmio_read32(kEntropySrcBase +
                        ENTROPY_SRC_FW_OV_WR_FIFO_FULL_REG_OFFSET) == 0u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   0x9u);
  uint32_t sha_recov =
      abs_mmio_read32(kEntropySrcBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET);
  CHECK((sha_recov &
         (1u << ENTROPY_SRC_RECOV_ALERT_STS_ES_FW_OV_DISABLE_ALERT_BIT)) == 0u,
        "Expected ES_FW_OV_DISABLE_ALERT == 0 when FW_OV_WR_FIFO_FULL == 0, got 0x%x",
        sha_recov);

  // Also test FW_OV_SHA3_START transition when FW_OV_ENTROPY_INSERT = 9
  disable_entropy_complex();
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_FW_OV_CONTROL_REG_OFFSET,
                   0x96u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   0x6u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   0x6u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   0x9u);

  // 5. Startup health test failure (REPCNTS_TOTAL_FAILS, ADAPTP_LO_TOTAL_FAILS,
  // MARKOV_LO_TOTAL_FAILS, STARTUP_FAIL1 -> ALERT_HANG).
  disable_entropy_complex();
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_CONF_REG_OFFSET,
                   make_conf(6u, 9u, 9u, 6u, 6u, 9u));
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_HEALTH_TEST_WINDOWS_REG_OFFSET,
                   0x00100020u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_REPCNTS_THRESHOLDS_REG_OFFSET,
                   0x00010001u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_ADAPTP_LO_THRESHOLDS_REG_OFFSET,
                   0xffffffffu);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_MARKOV_LO_THRESHOLDS_REG_OFFSET,
                   0xffffffffu);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_ALERT_THRESHOLD_REG_OFFSET,
                   0xfffd0002u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   0x6u);
  IBEX_SPIN_FOR(
      (abs_mmio_read32(kEntropySrcBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET) &
       (1u << ENTROPY_SRC_INTR_STATE_ES_HEALTH_TEST_FAILED_BIT)) != 0u,
      100000);
  CHECK(abs_mmio_read32(kEntropySrcBase +
                        ENTROPY_SRC_REPCNTS_TOTAL_FAILS_REG_OFFSET) > 0u,
        "Expected REPCNTS_TOTAL_FAILS > 0");
  CHECK(abs_mmio_read32(kEntropySrcBase +
                        ENTROPY_SRC_ADAPTP_LO_TOTAL_FAILS_REG_OFFSET) > 0u,
        "Expected ADAPTP_LO_TOTAL_FAILS > 0");
  CHECK(abs_mmio_read32(kEntropySrcBase +
                        ENTROPY_SRC_MARKOV_LO_TOTAL_FAILS_REG_OFFSET) > 0u,
        "Expected MARKOV_LO_TOTAL_FAILS > 0");

  // 6. ERR_CODE_TEST (bit 20 ES_MAIN_SM_ERR -> ENTROPY_SRC_ERROR) & SW_REGUPD / ME_REGWEN.
  disable_entropy_complex();
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_ERR_CODE_TEST_REG_OFFSET,
                   ENTROPY_SRC_ERR_CODE_ES_MAIN_SM_ERR_BIT);
  CHECK((abs_mmio_read32(kEntropySrcBase + ENTROPY_SRC_ERR_CODE_REG_OFFSET) &
         (1u << ENTROPY_SRC_ERR_CODE_ES_MAIN_SM_ERR_BIT)) != 0u);
  CHECK(abs_mmio_read32(kEntropySrcBase +
                        ENTROPY_SRC_MAIN_SM_STATE_REG_OFFSET) == 0x73u);

  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_SW_REGUPD_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kEntropySrcBase + ENTROPY_SRC_SW_REGUPD_REG_OFFSET) ==
        0u);
  CHECK(abs_mmio_read32(kEntropySrcBase + ENTROPY_SRC_REGWEN_REG_OFFSET) == 0u);

  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_ME_REGWEN_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kEntropySrcBase + ENTROPY_SRC_ME_REGWEN_REG_OFFSET) ==
        0u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   0x6u);
  CHECK(abs_mmio_read32(kEntropySrcBase +
                        ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET) == 0x9u);

  return true;
}
