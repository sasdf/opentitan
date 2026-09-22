// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/irq.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "csrng_regs.h"
#include "edn_regs.h"
#include "entropy_src_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

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
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   0x9u);
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
  abs_mmio_write32(
      kEntropySrcBase + ENTROPY_SRC_ADAPTP_LO_THRESHOLDS_REG_OFFSET, 0x0u);
  abs_mmio_write32(
      kEntropySrcBase + ENTROPY_SRC_MARKOV_LO_THRESHOLDS_REG_OFFSET, 0x0u);
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
  CHECK(abs_mmio_read32(kEntropySrcBase + ENTROPY_SRC_REGWEN_REG_OFFSET) == 1u);
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

  // Write 0 (RW0C) to RECOV_ALERT_STS while invalid MuBi4 values are still in
  // CSRs to verify continuous field alerts remain asserted.
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET,
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
        "Expected all 12 MuBi field alerts set after RW0C clear, got 0x%x "
        "(expected 0x%x)",
        recov, expected_mubi_alerts);

  disable_entropy_complex();
  CHECK(abs_mmio_read32(kEntropySrcBase +
                        ENTROPY_SRC_RECOV_ALERT_STS_REG_OFFSET) == 0u);

  // 3. ENTROPY_DATA reads when disabled, when ENTROPY_DATA_REG_ENABLE = 9 -> 0,
  // and when ENTROPY_DATA_REG_ENABLE = 6 but ES_ROUTE = 9 -> 0.
  CHECK(abs_mmio_read32(kEntropySrcBase +
                        ENTROPY_SRC_ENTROPY_DATA_REG_OFFSET) == 0u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   0x6u);
  CHECK(abs_mmio_read32(kEntropySrcBase +
                        ENTROPY_SRC_ENTROPY_DATA_REG_OFFSET) == 0u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_FW_OV_WR_DATA_REG_OFFSET,
                   0x12345678u);
  disable_entropy_complex();
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_CONF_REG_OFFSET,
                   make_conf(9u, 9u, 9u, 9u, 9u, 6u));
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_ENTROPY_CONTROL_REG_OFFSET,
                   0x99u);
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

  // Also test CSRNG instantiation when ENTROPY_SRC is in bypass mode
  // (ES_TYPE=6, ES_ROUTE=9) and when FIPS_ENABLE=9, FIPS_FLAG=6 to verify
  // ot_entropy_src_get_entropy.
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

  // Also test CSRNG instantiation when ENTROPY_SRC is in FW_OV bypass mode
  // (FW_OV_MODE=6, FW_OV_ENTROPY_INSERT=6, ES_TYPE=6, ES_ROUTE=9,
  // FIPS_ENABLE=9, FIPS_FLAG=6).
  disable_entropy_complex();
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_CONF_REG_OFFSET,
                   make_conf(9u, 6u, 9u, 9u, 9u, 9u));
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_ENTROPY_CONTROL_REG_OFFSET,
                   0x69u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_FW_OV_CONTROL_REG_OFFSET,
                   0x66u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   0x6u);
  for (uint32_t i = 0; i < 12; ++i) {
    abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_FW_OV_WR_DATA_REG_OFFSET,
                     0x2000u + i);
  }
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, 0x9666u);
  abs_mmio_write32(kCsrngBase + CSRNG_CMD_REQ_REG_OFFSET, 0x00000901u);
  IBEX_SPIN_FOR((abs_mmio_read32(kCsrngBase + CSRNG_SW_CMD_STS_REG_OFFSET) &
                 (1u << CSRNG_SW_CMD_STS_CMD_ACK_BIT)) != 0u,
                100000);

  // 4. FW override mode: FW_OV_RD_DATA when disabled & when empty
  // (SFIFO_OBSERVE_ERR), OBSERVE_FIFO_THRESH IRQ, FW_OV_WR_DATA bypass & SHA3
  // insertion, and ES_FW_OV_DISABLE_ALERT.
  disable_entropy_complex();
  CHECK(abs_mmio_read32(kEntropySrcBase +
                        ENTROPY_SRC_FW_OV_RD_DATA_REG_OFFSET) == 0u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_FW_OV_WR_DATA_REG_OFFSET,
                   0x12345678u);

  // Enable with FW_OV_MODE = 6, FW_OV_ENTROPY_INSERT = 9, OBSERVE_FIFO_THRESH =
  // 1
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
  // Clear INTR_STATE while observe FIFO >= threshold so ES_OBSERVE_FIFO_READY
  // re-asserts
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET, 0xfu);
  CHECK((abs_mmio_read32(kEntropySrcBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET) &
         (1u << ENTROPY_SRC_INTR_STATE_ES_OBSERVE_FIFO_READY_BIT)) != 0u);
  // Drain observe FIFO completely and read 1 extra word to trigger
  // SFIFO_OBSERVE_ERR | FIFO_READ_ERR
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

  // FW_OV_ENTROPY_INSERT = 6 in bypass mode (ES_TYPE = 6, ES_ROUTE = 6,
  // ENTROPY_DATA_REG_ENABLE = 6)
  disable_entropy_complex();
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_CONF_REG_OFFSET,
                   make_conf(9u, 9u, 9u, 9u, 9u, 6u));
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_ENTROPY_CONTROL_REG_OFFSET,
                   0x66u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_FW_OV_CONTROL_REG_OFFSET,
                   0x66u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   0x6u);
  CHECK(abs_mmio_read32(kEntropySrcBase +
                        ENTROPY_SRC_FW_OV_WR_FIFO_FULL_REG_OFFSET) == 0u);
  IBEX_SPIN_FOR(abs_mmio_read32(kEntropySrcBase +
                                ENTROPY_SRC_OBSERVE_FIFO_DEPTH_REG_OFFSET) > 0u,
                100000);
  // Push 4 packets (48 words) via FW_OV_WR_DATA in bypass mode before reading
  // ENTROPY_DATA:
  // - Seed 1 (0x1000+i) moves into swread_fifo (es_rdata_capt staging).
  // - Seed 2 (0x2000+i) and Seed 3 (0x3000+i) fill esfinal_fifo (24 words).
  // - Seed 4 (0x4000+i) overflows esfinal_fifo and is dropped
  // (ot_fifo32_reset(&s->bypass_fifo)).
  for (uint32_t seed = 0; seed < 4; ++seed) {
    uint32_t base = (seed + 1u) * 0x1000u;
    for (uint32_t i = 0; i < 12; ++i) {
      abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_FW_OV_WR_DATA_REG_OFFSET,
                       base + i);
    }
  }
  CHECK((abs_mmio_read32(kEntropySrcBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET) &
         (1u << ENTROPY_SRC_INTR_STATE_ES_ENTROPY_VALID_BIT)) != 0u);
  for (uint32_t seed = 0; seed < 3; ++seed) {
    uint32_t base = (seed + 1u) * 0x1000u;
    for (uint32_t i = 0; i < 12; ++i) {
      uint32_t w = abs_mmio_read32(kEntropySrcBase +
                                   ENTROPY_SRC_ENTROPY_DATA_REG_OFFSET);
      CHECK(w == base + i, "Expected bypass word 0x%x, got 0x%x", base + i, w);
    }
  }
  // Read 12 more words from empty ENTROPY_DATA (Seed 4 was dropped) to trigger
  // SFIFO_ESFINAL_ERR
  for (uint32_t i = 0; i < 12; ++i) {
    (void)abs_mmio_read32(kEntropySrcBase +
                          ENTROPY_SRC_ENTROPY_DATA_REG_OFFSET);
  }
  CHECK((abs_mmio_read32(kEntropySrcBase + ENTROPY_SRC_ERR_CODE_REG_OFFSET) &
         (1u << ENTROPY_SRC_ERR_CODE_SFIFO_ESFINAL_ERR_BIT)) != 0u,
        "Expected SFIFO_ESFINAL_ERR after 12 reads from empty ENTROPY_DATA");
  // Toggle FW_OV_SHA3_START in bypass mode (6 -> 9) and verify MAIN_SM_STATE
  // stays Idle (0xf5)
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   0x6u);
  CHECK(abs_mmio_read32(kEntropySrcBase +
                        ENTROPY_SRC_MAIN_SM_STATE_REG_OFFSET) == 0xf5u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   0x9u);
  CHECK(abs_mmio_read32(kEntropySrcBase +
                        ENTROPY_SRC_MAIN_SM_STATE_REG_OFFSET) == 0xf5u);

  // Push 5 full 384-bit seeds (60 words) via FW_OV_WR_DATA in bypass mode
  // without reading ENTROPY_DATA so final_fifo fills completely and the 5th
  // seed exercises the final_fifo full / bypass_fifo reset path (line 1177),
  // while preserving Seed 1 in the output staging buffer.
  for (uint32_t i = 0; i < 60; ++i) {
    abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_FW_OV_WR_DATA_REG_OFFSET,
                     0x3000u + i);
  }
  for (uint32_t i = 0; i < 12; ++i) {
    uint32_t w =
        abs_mmio_read32(kEntropySrcBase + ENTROPY_SRC_ENTROPY_DATA_REG_OFFSET);
    CHECK(w == 0x3000u + i,
          "Expected preserved Seed 1 bypass word 0x%x after overflow, got 0x%x",
          0x3000u + i, w);
  }

  // FW_OV_ENTROPY_INSERT = 6 in SHA3 mode (ES_TYPE = 9):
  // 1. In FWInsertStart (FW_OV_SHA3_START == 9), sha3_start_o already pulsed in
  // Idle,
  //    so writing 2 words (0x11111111, 0x22222222) absorbs them into u_sha3 and
  //    leaves FW_OV_WR_FIFO_FULL == 0.
  // 2. Setting FW_OV_SHA3_START = 6 transitions FWInsertStart -> FWInsertMsg
  // without
  //    clearing u_sha3. Writing 1 odd word (0xdeadbeef) leaves 32 bits in
  //    precon_fifo (FW_OV_WR_FIFO_FULL == 0).
  // 3. Setting FW_OV_SHA3_START = 9 transitions FWInsertMsg -> Sha3Process ->
  // FWInsertStart,
  //    does NOT assert ES_FW_OV_DISABLE_ALERT, asserts ES_ENTROPY_VALID, and
  //    produces the exact SHA3-384 digest of (0x11111111, 0x22222222).
  disable_entropy_complex();
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_CONF_REG_OFFSET,
                   make_conf(6u, 6u, 9u, 9u, 9u, 6u));
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_ENTROPY_CONTROL_REG_OFFSET,
                   0x96u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_FW_OV_CONTROL_REG_OFFSET,
                   0x66u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   0x6u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_FW_OV_WR_DATA_REG_OFFSET,
                   0x11111111u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_FW_OV_WR_DATA_REG_OFFSET,
                   0x22222222u);
  CHECK(abs_mmio_read32(kEntropySrcBase +
                        ENTROPY_SRC_FW_OV_WR_FIFO_FULL_REG_OFFSET) == 0u);
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
        "Expected ES_FW_OV_DISABLE_ALERT == 0 when FW_OV_WR_FIFO_FULL == 0, "
        "got 0x%x",
        sha_recov);
  IBEX_SPIN_FOR(
      (abs_mmio_read32(kEntropySrcBase + ENTROPY_SRC_INTR_STATE_REG_OFFSET) &
       (1u << ENTROPY_SRC_INTR_STATE_ES_ENTROPY_VALID_BIT)) != 0u,
      100000);
  uint32_t sha_w0 =
      abs_mmio_read32(kEntropySrcBase + ENTROPY_SRC_ENTROPY_DATA_REG_OFFSET);
  CHECK(sha_w0 == 0x1d9845a5u,
        "Expected SHA3-384(0x11111111, 0x22222222) word 0 == 0x1d9845a5, got "
        "0x%x",
        sha_w0);
  for (int i = 1; i < 12; ++i) {
    (void)abs_mmio_read32(kEntropySrcBase +
                          ENTROPY_SRC_ENTROPY_DATA_REG_OFFSET);
  }

  // Also test FW_OV_SHA3_START transition when FW_OV_ENTROPY_INSERT = 9:
  // Verify MAIN_SM_STATE reaches BootPhaseDone (0x8e) and is NOT corrupted by
  // FW_OV_SHA3_START.
  disable_entropy_complex();
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_FW_OV_CONTROL_REG_OFFSET,
                   0x96u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   0x6u);
  IBEX_SPIN_FOR(abs_mmio_read32(kEntropySrcBase +
                                ENTROPY_SRC_MAIN_SM_STATE_REG_OFFSET) == 0x8eu,
                100000);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   0x6u);
  CHECK(abs_mmio_read32(kEntropySrcBase +
                        ENTROPY_SRC_MAIN_SM_STATE_REG_OFFSET) == 0x8eu,
        "Expected MAIN_SM_STATE == BootPhaseDone (0x8e) when "
        "FW_OV_ENTROPY_INSERT == 9");
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_FW_OV_SHA3_START_REG_OFFSET,
                   0x9u);
  CHECK(abs_mmio_read32(kEntropySrcBase +
                        ENTROPY_SRC_MAIN_SM_STATE_REG_OFFSET) == 0x8eu,
        "Expected MAIN_SM_STATE == BootPhaseDone (0x8e) when "
        "FW_OV_ENTROPY_INSERT == 9");

  // 5. Startup health test failure (REPCNTS_TOTAL_FAILS, ADAPTP_LO_TOTAL_FAILS,
  // MARKOV_LO_TOTAL_FAILS, STARTUP_FAIL1 -> ALERT_HANG).
  disable_entropy_complex();
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_CONF_REG_OFFSET,
                   make_conf(6u, 9u, 9u, 6u, 6u, 9u));
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_HEALTH_TEST_WINDOWS_REG_OFFSET,
                   0x00100020u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_REPCNTS_THRESHOLDS_REG_OFFSET,
                   0x00010001u);
  abs_mmio_write32(
      kEntropySrcBase + ENTROPY_SRC_ADAPTP_LO_THRESHOLDS_REG_OFFSET,
      0xffffffffu);
  abs_mmio_write32(
      kEntropySrcBase + ENTROPY_SRC_MARKOV_LO_THRESHOLDS_REG_OFFSET,
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

  // 6. ERR_CODE_TEST (bit 20 ES_MAIN_SM_ERR -> ENTROPY_SRC_ERROR) & SW_REGUPD /
  // ME_REGWEN.
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
