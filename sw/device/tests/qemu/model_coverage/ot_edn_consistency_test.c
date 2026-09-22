// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/irq.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/ip/csrng/data/csrng_regs.h"
#include "hw/ip/edn/data/edn_regs.h"
#include "hw/ip/entropy_src/data/entropy_src_regs.h"
#include "hw/ip/rv_core_ibex/data/rv_core_ibex_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kEntropySrcBase = TOP_EARLGREY_ENTROPY_SRC_BASE_ADDR,
  kCsrngBase = TOP_EARLGREY_CSRNG_BASE_ADDR,
  kEdn0Base = TOP_EARLGREY_EDN0_BASE_ADDR,
  kEdn1Base = TOP_EARLGREY_EDN1_BASE_ADDR,
  kIbexCfgBase = TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR,
};

static volatile uint32_t access_fault_count;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = ibex_mcause_read();
  if (mcause == kIbexExcLoadAccessFault || mcause == kIbexExcStoreAccessFault) {
    access_fault_count++;
  }
}

static void reset_entropy_complex_enable_csrng(void) {
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9999u);
  abs_mmio_write32(kEdn1Base + EDN_CTRL_REG_OFFSET, 0x9999u);
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, 0x9999u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   0x9u);
  while (abs_mmio_read32(kEntropySrcBase + ENTROPY_SRC_REGWEN_REG_OFFSET) ==
         0u) {
  }
  uint32_t conf = (6u << ENTROPY_SRC_CONF_FIPS_ENABLE_OFFSET) |
                  (6u << ENTROPY_SRC_CONF_FIPS_FLAG_OFFSET) |
                  (9u << ENTROPY_SRC_CONF_RNG_FIPS_OFFSET) |
                  (9u << ENTROPY_SRC_CONF_RNG_BIT_ENABLE_OFFSET) |
                  (9u << ENTROPY_SRC_CONF_THRESHOLD_SCOPE_OFFSET) |
                  (9u << ENTROPY_SRC_CONF_ENTROPY_DATA_REG_ENABLE_OFFSET);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_CONF_REG_OFFSET, conf);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_HEALTH_TEST_WINDOWS_REG_OFFSET,
                   0x00100020u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_ENTROPY_CONTROL_REG_OFFSET,
                   0x99u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   0x6u);
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, 0x9666u);
}

bool test_main(void) {
  irq_external_ctrl(false);
  irq_global_ctrl(false);

  reset_entropy_complex_enable_csrng();

  // 1. Test W/O reads (ALERT_TEST, SW_CMD_REQ, RESEED_CMD, GENERATE_CMD -> 0),
  // R/O writes (REGWEN, SW_CMD_STS, HW_CMD_STS, ERR_CODE, MAIN_SM_STATE ->
  // ignored), sub-word write error (byte write to CTRL -> store access fault),
  // and out-of-bounds MMIO decode errors (0x48, 0x7c -> load/store access
  // fault).
  CHECK(abs_mmio_read32(kEdn0Base + EDN_ALERT_TEST_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_SW_CMD_REQ_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_RESEED_CMD_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_GENERATE_CMD_REG_OFFSET) == 0u);

  abs_mmio_write32(kEdn0Base + EDN_SW_CMD_STS_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kEdn0Base + EDN_HW_CMD_STS_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kEdn0Base + EDN_ERR_CODE_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kEdn0Base + EDN_MAIN_SM_STATE_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kEdn0Base + EDN_SW_CMD_REQ_REG_OFFSET, 0x00000001u);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_MAIN_SM_STATE_REG_OFFSET) == 0x0c1u);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_ERR_CODE_REG_OFFSET) == 0u);

  access_fault_count = 0;
  abs_mmio_write8(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x00u);
  CHECK(access_fault_count == 1u,
        "Expected store access fault on 1B write to CTRL");

  access_fault_count = 0;
  (void)abs_mmio_read32(kEdn0Base + 0x48u);
  CHECK(access_fault_count == 1u, "Expected load access fault at 0x48");

  access_fault_count = 0;
  abs_mmio_write32(kEdn0Base + 0x7cu, 0u);
  CHECK(access_fault_count == 1u, "Expected store access fault at 0x7c");

  // 2. Invalid MuBi4 values in CTRL -> RECOV_ALERT_STS field alert bits.
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x0000u);
  uint32_t recov = abs_mmio_read32(kEdn0Base + EDN_RECOV_ALERT_STS_REG_OFFSET);
  uint32_t exp_recov =
      (1u << EDN_RECOV_ALERT_STS_EDN_ENABLE_FIELD_ALERT_BIT) |
      (1u << EDN_RECOV_ALERT_STS_BOOT_REQ_MODE_FIELD_ALERT_BIT) |
      (1u << EDN_RECOV_ALERT_STS_AUTO_REQ_MODE_FIELD_ALERT_BIT) |
      (1u << EDN_RECOV_ALERT_STS_CMD_FIFO_RST_FIELD_ALERT_BIT);
  CHECK((recov & exp_recov) == exp_recov,
        "Expected all 4 MuBi field alerts set, got 0x%x", recov);
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9999u);
  abs_mmio_write32(kEdn0Base + EDN_RECOV_ALERT_STS_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_RECOV_ALERT_STS_REG_OFFSET) == 0u);

  // 3. Boot-request mode (BOOT_REQ_MODE = 6, AUTO_REQ_MODE = 9, EDN_ENABLE = 6
  // -> 0x9966) followed by endpoint read (RV_CORE_IBEX_RND_DATA) and boot
  // uninstantiate (CTRL = 0x9996).
  abs_mmio_write32(kEdn0Base + EDN_BOOT_INS_CMD_REG_OFFSET, 0x00000601u);
  abs_mmio_write32(kEdn0Base + EDN_BOOT_GEN_CMD_REG_OFFSET, 0x00001003u);
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9966u);
  IBEX_SPIN_FOR(
      abs_mmio_read32(kEdn0Base + EDN_MAIN_SM_STATE_REG_OFFSET) == 0x0f0u,
      100000);
  IBEX_SPIN_FOR(
      (abs_mmio_read32(kIbexCfgBase + RV_CORE_IBEX_RND_STATUS_REG_OFFSET) &
       1u) != 0u,
      100000);
  (void)abs_mmio_read32(kIbexCfgBase + RV_CORE_IBEX_RND_DATA_REG_OFFSET);
  // Deassert BOOT_REQ_MODE while keeping EDN_ENABLE = 6 (0x9996) to trigger
  // BOOT_LOAD_UNI
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9996u);
  IBEX_SPIN_FOR(((abs_mmio_read32(kEdn0Base + EDN_HW_CMD_STS_REG_OFFSET) >>
                  EDN_HW_CMD_STS_CMD_TYPE_OFFSET) &
                 EDN_HW_CMD_STS_CMD_TYPE_MASK) == 5u,
                100000);
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9999u);

  // 4. SW port mode (EDN_ENABLE = 6, BOOT_REQ_MODE = 9, AUTO_REQ_MODE = 9 ->
  // 0x9996): Test INSTANTIATE, GENERATE (GLEN=1), and UNINSTANTIATE via
  // SW_CMD_REQ.
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9996u);
  abs_mmio_write32(kEdn0Base + EDN_SW_CMD_REQ_REG_OFFSET, 0x00000601u);
  IBEX_SPIN_FOR((abs_mmio_read32(kEdn0Base + EDN_SW_CMD_STS_REG_OFFSET) &
                 (1u << EDN_SW_CMD_STS_CMD_ACK_BIT)) != 0u,
                100000);
  abs_mmio_write32(kEdn0Base + EDN_SW_CMD_REQ_REG_OFFSET, 0x00001903u);
  IBEX_SPIN_FOR((abs_mmio_read32(kEdn0Base + EDN_SW_CMD_STS_REG_OFFSET) &
                 (1u << EDN_SW_CMD_STS_CMD_ACK_BIT)) != 0u,
                100000);
  abs_mmio_write32(kEdn0Base + EDN_SW_CMD_REQ_REG_OFFSET, 0x00000005u);
  IBEX_SPIN_FOR((abs_mmio_read32(kEdn0Base + EDN_SW_CMD_STS_REG_OFFSET) &
                 (1u << EDN_SW_CMD_STS_CMD_ACK_BIT)) != 0u,
                100000);
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9999u);

  // 5a. Auto-request mode replay with 2 words in RESEED_CMD (CLEN=1, used=2)
  // and 1 word in GENERATE_CMD (GLEN=1), MAX_NUM_REQS_BETWEEN_RESEEDS = 1.
  abs_mmio_write32(kEdn0Base + EDN_MAX_NUM_REQS_BETWEEN_RESEEDS_REG_OFFSET, 1u);
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9696u);
  abs_mmio_write32(kEdn0Base + EDN_RESEED_CMD_REG_OFFSET, 0x00000612u);
  abs_mmio_write32(kEdn0Base + EDN_RESEED_CMD_REG_OFFSET, 0x11223344u);
  abs_mmio_write32(kEdn0Base + EDN_GENERATE_CMD_REG_OFFSET, 0x00001903u);
  abs_mmio_write32(kEdn0Base + EDN_SW_CMD_REQ_REG_OFFSET, 0x00000601u);
  IBEX_SPIN_FOR((abs_mmio_read32(kEdn0Base + EDN_SW_CMD_STS_REG_OFFSET) &
                 (1u << EDN_SW_CMD_STS_CMD_ACK_BIT)) != 0u,
                100000);
  IBEX_SPIN_FOR(
      (abs_mmio_read32(kIbexCfgBase + RV_CORE_IBEX_RND_STATUS_REG_OFFSET) &
       1u) != 0u,
      100000);
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9996u);
  (void)abs_mmio_read32(kIbexCfgBase + RV_CORE_IBEX_RND_DATA_REG_OFFSET);
  IBEX_SPIN_FOR((abs_mmio_read32(kEdn0Base + EDN_HW_CMD_STS_REG_OFFSET) &
                 (1u << EDN_HW_CMD_STS_CMD_ACK_BIT)) != 0u,
                100000);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_ERR_CODE_REG_OFFSET) == 0u);
  abs_mmio_write32(kEdn0Base + EDN_SW_CMD_REQ_REG_OFFSET, 0x00000005u);
  IBEX_SPIN_FOR((abs_mmio_read32(kEdn0Base + EDN_SW_CMD_STS_REG_OFFSET) &
                 (1u << EDN_SW_CMD_STS_CMD_ACK_BIT)) != 0u,
                100000);
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9999u);

  // 5b. Auto-request mode with 1 word in RESEED_CMD (CLEN=1, so length=2 >
  // used=1) and 1 word in GENERATE_CMD (GLEN=1), MAX_NUM_REQS_BETWEEN_RESEEDS =
  // 1: Verifies fix_rtl_discrepancy_w1_ot_edn_regs_size_and_replay_fifo.md! In
  // RTL (edn_core.sv:692-706, 814-825), sfifo_rescmd sends sfifo_rescmd_depth
  // (1) word and transitions to AutoAckWait (0x092u) with CMD_TYPE=2 (RESEED)
  // and ERR_CODE == 0 without raising SFIFO_RESCMD_ERR | FIFO_READ_ERR.
  abs_mmio_write32(kEdn0Base + EDN_MAX_NUM_REQS_BETWEEN_RESEEDS_REG_OFFSET, 1u);
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9696u);
  abs_mmio_write32(kEdn0Base + EDN_RESEED_CMD_REG_OFFSET, 0x00000612u);
  abs_mmio_write32(kEdn0Base + EDN_GENERATE_CMD_REG_OFFSET, 0x00001903u);
  abs_mmio_write32(kEdn0Base + EDN_SW_CMD_REQ_REG_OFFSET, 0x00000601u);
  IBEX_SPIN_FOR(((abs_mmio_read32(kEdn0Base + EDN_HW_CMD_STS_REG_OFFSET) >>
                  EDN_HW_CMD_STS_CMD_TYPE_OFFSET) &
                 EDN_HW_CMD_STS_CMD_TYPE_MASK) == 2u,
                100000);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_MAIN_SM_STATE_REG_OFFSET) == 0x092u,
        "Expected MAIN_SM_STATE == 0x092 (AutoAckWait), got 0x%x",
        abs_mmio_read32(kEdn0Base + EDN_MAIN_SM_STATE_REG_OFFSET));
  CHECK(abs_mmio_read32(kEdn0Base + EDN_ERR_CODE_REG_OFFSET) == 0u,
        "Expected ERR_CODE == 0 when RESEED_CMD has 1 word (CLEN=1), got 0x%x",
        abs_mmio_read32(kEdn0Base + EDN_ERR_CODE_REG_OFFSET));
  reset_entropy_complex_enable_csrng();

  // 6. Auto-request mode on EDN0 with EMPTY GENERATE_CMD FIFO ->
  // SFIFO_GENCMD_ERR | FIFO_READ_ERR and INTR_STATE.EDN_FATAL_ERR, plus
  // RESEED_CMD / GENERATE_CMD overflow (>13 words).
  uint32_t exp_gencmd_rd_err = (1u << EDN_ERR_CODE_SFIFO_GENCMD_ERR_BIT) |
                               (1u << EDN_ERR_CODE_FIFO_READ_ERR_BIT);
  abs_mmio_write32(kEdn0Base + EDN_MAX_NUM_REQS_BETWEEN_RESEEDS_REG_OFFSET, 1u);
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9696u);
  abs_mmio_write32(kEdn0Base + EDN_SW_CMD_REQ_REG_OFFSET, 0x00000601u);
  IBEX_SPIN_FOR((abs_mmio_read32(kEdn0Base + EDN_ERR_CODE_REG_OFFSET) &
                 exp_gencmd_rd_err) == exp_gencmd_rd_err,
                100000);
  CHECK((abs_mmio_read32(kEdn0Base + EDN_INTR_STATE_REG_OFFSET) &
         (1u << EDN_INTR_STATE_EDN_FATAL_ERR_BIT)) != 0u);
  for (int i = 0; i < 14; ++i) {
    abs_mmio_write32(kEdn0Base + EDN_RESEED_CMD_REG_OFFSET, 0x00000602u);
    abs_mmio_write32(kEdn0Base + EDN_GENERATE_CMD_REG_OFFSET, 0x00001903u);
  }
  uint32_t err0 = abs_mmio_read32(kEdn0Base + EDN_ERR_CODE_REG_OFFSET);
  CHECK((err0 & (1u << EDN_ERR_CODE_FIFO_WRITE_ERR_BIT)) != 0u,
        "Expected FIFO_WRITE_ERR after >13 writes to RESEED_CMD/GENERATE_CMD, "
        "got 0x%x",
        err0);

  // 7. Auto-request mode on EDN1 with MAX_NUM_REQS_BETWEEN_RESEEDS = 0 and
  // EMPTY RESEED_CMD FIFO
  // -> SFIFO_RESCMD_ERR | FIFO_READ_ERR, plus ERR_CODE_TEST =
  // EDN_MAIN_SM_ERR_BIT (21)
  // -> MAIN_SM_STATE == 0x17eu (EDN_ERROR), and REGWEN = 0 lock.
  uint32_t exp_rescmd_rd_err = (1u << EDN_ERR_CODE_SFIFO_RESCMD_ERR_BIT) |
                               (1u << EDN_ERR_CODE_FIFO_READ_ERR_BIT);
  abs_mmio_write32(kEdn1Base + EDN_MAX_NUM_REQS_BETWEEN_RESEEDS_REG_OFFSET, 0u);
  abs_mmio_write32(kEdn1Base + EDN_CTRL_REG_OFFSET, 0x9696u);
  abs_mmio_write32(kEdn1Base + EDN_SW_CMD_REQ_REG_OFFSET, 0x00000601u);
  IBEX_SPIN_FOR((abs_mmio_read32(kEdn1Base + EDN_ERR_CODE_REG_OFFSET) &
                 exp_rescmd_rd_err) == exp_rescmd_rd_err,
                100000);
  CHECK((abs_mmio_read32(kEdn1Base + EDN_INTR_STATE_REG_OFFSET) &
         (1u << EDN_INTR_STATE_EDN_FATAL_ERR_BIT)) != 0u);
  CHECK(abs_mmio_read32(kEdn1Base + EDN_MAIN_SM_STATE_REG_OFFSET) != 0x17eu,
        "Expected MAIN_SM_STATE != 0x17e (Error) on FIFO_READ_ERR alone, got "
        "0x%x",
        abs_mmio_read32(kEdn1Base + EDN_MAIN_SM_STATE_REG_OFFSET));

  abs_mmio_write32(kEdn1Base + EDN_ERR_CODE_TEST_REG_OFFSET,
                   EDN_ERR_CODE_EDN_MAIN_SM_ERR_BIT);
  uint32_t exp_sm_errs = (1u << EDN_ERR_CODE_EDN_MAIN_SM_ERR_BIT) |
                         (1u << EDN_ERR_CODE_EDN_ACK_SM_ERR_BIT);
  CHECK((abs_mmio_read32(kEdn1Base + EDN_ERR_CODE_REG_OFFSET) & exp_sm_errs) ==
            exp_sm_errs,
        "Expected both EDN_MAIN_SM_ERR and EDN_ACK_SM_ERR after ERR_CODE_TEST, "
        "got 0x%x",
        abs_mmio_read32(kEdn1Base + EDN_ERR_CODE_REG_OFFSET));
  CHECK(abs_mmio_read32(kEdn1Base + EDN_MAIN_SM_STATE_REG_OFFSET) == 0x17eu,
        "Expected MAIN_SM_STATE == 0x17e (Error) after ERR_CODE_TEST, got 0x%x",
        abs_mmio_read32(kEdn1Base + EDN_MAIN_SM_STATE_REG_OFFSET));

  abs_mmio_write32(kEdn1Base + EDN_REGWEN_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kEdn1Base + EDN_REGWEN_REG_OFFSET) == 0u);
  abs_mmio_write32(kEdn1Base + EDN_CTRL_REG_OFFSET, 0x9999u);
  CHECK(abs_mmio_read32(kEdn1Base + EDN_CTRL_REG_OFFSET) == 0x9696u);

  return true;
}
