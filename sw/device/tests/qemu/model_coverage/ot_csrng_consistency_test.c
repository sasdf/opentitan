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

#define CHECK_EQ(a, b) CHECK((a) == (b))

enum {
  kEntropySrcBase = TOP_EARLGREY_ENTROPY_SRC_BASE_ADDR,
  kCsrngBase = TOP_EARLGREY_CSRNG_BASE_ADDR,
  kEdn0Base = TOP_EARLGREY_EDN0_BASE_ADDR,
  kEdn1Base = TOP_EARLGREY_EDN1_BASE_ADDR,
};

static volatile uint32_t load_fault_count;
static volatile uint32_t store_fault_count;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = ibex_mcause_read();
  if (mcause == kIbexExcLoadAccessFault) {
    load_fault_count++;
  } else if (mcause == kIbexExcStoreAccessFault) {
    store_fault_count++;
  }
}

static void disable_all(void) {
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9999u);
  abs_mmio_write32(kEdn1Base + EDN_CTRL_REG_OFFSET, 0x9999u);
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, 0x9999u);
  abs_mmio_write32(kEntropySrcBase + ENTROPY_SRC_MODULE_ENABLE_REG_OFFSET,
                   0x9u);
  abs_mmio_write32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET, 0u);
  abs_mmio_write32(kCsrngBase + CSRNG_HW_EXC_STS_REG_OFFSET, 0u);
  abs_mmio_write32(kCsrngBase + CSRNG_INTR_STATE_REG_OFFSET, 0xfu);
  abs_mmio_write32(kEdn0Base + EDN_RECOV_ALERT_STS_REG_OFFSET, 0u);
  abs_mmio_write32(kEdn0Base + EDN_INTR_STATE_REG_OFFSET, 0x3u);
}

static uint32_t csrng_send_sw_cmd(uint32_t header, const uint32_t *adata,
                                  uint32_t clen) {
  abs_mmio_write32(kCsrngBase + CSRNG_CMD_REQ_REG_OFFSET, header);
  for (uint32_t i = 0; i < clen; ++i) {
    abs_mmio_write32(kCsrngBase + CSRNG_CMD_REQ_REG_OFFSET, adata[i]);
  }
  IBEX_SPIN_FOR((abs_mmio_read32(kCsrngBase + CSRNG_SW_CMD_STS_REG_OFFSET) &
                 (1u << CSRNG_SW_CMD_STS_CMD_ACK_BIT)) != 0u,
                100000);
  return abs_mmio_read32(kCsrngBase + CSRNG_SW_CMD_STS_REG_OFFSET);
}

bool test_main(void) {
  irq_external_ctrl(false);
  irq_global_ctrl(false);

  disable_all();

  // 1. Unmapped offset 0x7c access faults, W/O register reads, R/O register
  // writes, and empty reads of GENBITS and INT_STATE_VAL.
  load_fault_count = 0;
  (void)abs_mmio_read32(kCsrngBase + 0x7cu);
  CHECK_EQ(load_fault_count, 1u);

  store_fault_count = 0;
  abs_mmio_write32(kCsrngBase + 0x7cu, 0x12345678u);
  CHECK_EQ(store_fault_count, 1u);

  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_INTR_TEST_REG_OFFSET), 0u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_ALERT_TEST_REG_OFFSET), 0u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_CMD_REQ_REG_OFFSET), 0u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_REG_OFFSET), 0u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_INT_STATE_VAL_REG_OFFSET), 0u);

  // R/O writes should be ignored without fault.
  abs_mmio_write32(kCsrngBase + CSRNG_SW_CMD_STS_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kCsrngBase + CSRNG_GENBITS_VLD_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kCsrngBase + CSRNG_GENBITS_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kCsrngBase + CSRNG_INT_STATE_VAL_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kCsrngBase + CSRNG_ERR_CODE_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kCsrngBase + CSRNG_MAIN_SM_STATE_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kCsrngBase + CSRNG_RESEED_COUNTER_0_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kCsrngBase + CSRNG_RESEED_COUNTER_1_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kCsrngBase + CSRNG_RESEED_COUNTER_2_REG_OFFSET, 0xffffffffu);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_ERR_CODE_REG_OFFSET), 0u);

  // 2. Continuous MuBi4 field alerts in RECOV_ALERT_STS.
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, 0x5556u);
  uint32_t expected_ctrl_alerts =
      (1u << CSRNG_RECOV_ALERT_STS_SW_APP_ENABLE_FIELD_ALERT_BIT) |
      (1u << CSRNG_RECOV_ALERT_STS_READ_INT_STATE_FIELD_ALERT_BIT) |
      (1u << CSRNG_RECOV_ALERT_STS_FIPS_FORCE_ENABLE_FIELD_ALERT_BIT);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET) &
               expected_ctrl_alerts,
           expected_ctrl_alerts);
  // W0C write while invalid MuBi4 remains in CTRL must NOT clear the bits.
  abs_mmio_write32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET, 0u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET) &
               expected_ctrl_alerts,
           expected_ctrl_alerts);

  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, 0x9995u);
  CHECK((abs_mmio_read32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET) &
         (1u << CSRNG_RECOV_ALERT_STS_ENABLE_FIELD_ALERT_BIT)) != 0u);

  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, 0x9666u);
  abs_mmio_write32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET, 0u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET), 0u);

  // 3. SW Instance (CMD_REQ) Command Stage Errors, UPDATE with CLEN > 0, and
  // FIPS_FORCE.
  // 3a. Invalid ACMD (0x6) -> CMD_STS = 1 (INVALID_ACMD), RECOV_ALERT_STS bit
  // 13
  uint32_t sts = csrng_send_sw_cmd(0x00000006u, NULL, 0);
  CHECK_EQ(
      (sts >> CSRNG_SW_CMD_STS_CMD_STS_OFFSET) & CSRNG_SW_CMD_STS_CMD_STS_MASK,
      1u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET),
           1u << CSRNG_RECOV_ALERT_STS_CMD_STAGE_INVALID_ACMD_ALERT_BIT);
  abs_mmio_write32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET, 0u);

  // 3b. GENERATE before INSTANTIATE -> CMD_STS = 3 (INVALID_CMD_SEQ), bit 14
  sts = csrng_send_sw_cmd(0x00001003u, NULL, 0);
  CHECK_EQ(
      (sts >> CSRNG_SW_CMD_STS_CMD_STS_OFFSET) & CSRNG_SW_CMD_STS_CMD_STS_MASK,
      3u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET),
           1u << CSRNG_RECOV_ALERT_STS_CMD_STAGE_INVALID_CMD_SEQ_ALERT_BIT);
  abs_mmio_write32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET, 0u);

  // 3c. Enable FIPS_FORCE_ENABLE = True (CTRL = 0x6666) and FIPS_FORCE = 0x7,
  // instantiate with FLAG0 = True (0x00000601).
  abs_mmio_write32(kCsrngBase + CSRNG_FIPS_FORCE_REG_OFFSET, 0x7u);
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, 0x6666u);
  sts = csrng_send_sw_cmd(0x00000601u, NULL, 0);
  CHECK_EQ(
      (sts >> CSRNG_SW_CMD_STS_CMD_STS_OFFSET) & CSRNG_SW_CMD_STS_CMD_STS_MASK,
      0u);

  // 3d. Duplicate INSTANTIATE when already instantiated -> CMD_STS = 3
  sts = csrng_send_sw_cmd(0x00000601u, NULL, 0);
  CHECK_EQ(
      (sts >> CSRNG_SW_CMD_STS_CMD_STS_OFFSET) & CSRNG_SW_CMD_STS_CMD_STS_MASK,
      3u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET),
           1u << CSRNG_RECOV_ALERT_STS_CMD_STAGE_INVALID_CMD_SEQ_ALERT_BIT);
  abs_mmio_write32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET, 0u);

  // 3e. UPDATE with CLEN = 1 (0x00000014 + 0x12345678) -> CMD_STS = 0
  const uint32_t upd_adata[1] = {0x12345678u};
  sts = csrng_send_sw_cmd(0x00000014u, upd_adata, 1);
  CHECK_EQ(
      (sts >> CSRNG_SW_CMD_STS_CMD_STS_OFFSET) & CSRNG_SW_CMD_STS_CMD_STS_MASK,
      0u);

  // 3e2. GENERATE with GLEN = 0 (0x00000003) -> per csrng_cmd_stage.sv:378-431
  // and csrng_ctr_drbg_gen.sv:575-598, generates 1 128-bit block with glast=0
  // (no post-generate update, reseed_counter remains 0) and completes with
  // CMD_STS = 0 once GENBITS is drained.
  abs_mmio_write32(kCsrngBase + CSRNG_CMD_REQ_REG_OFFSET, 0x00000003u);
  IBEX_SPIN_FOR((abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_VLD_REG_OFFSET) &
                 (1u << CSRNG_GENBITS_VLD_GENBITS_VLD_BIT)) != 0u,
                100000);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_VLD_REG_OFFSET), 0x3u);
  for (int i = 0; i < 4; ++i) {
    (void)abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_REG_OFFSET);
  }
  IBEX_SPIN_FOR((abs_mmio_read32(kCsrngBase + CSRNG_SW_CMD_STS_REG_OFFSET) &
                 (1u << CSRNG_SW_CMD_STS_CMD_ACK_BIT)) != 0u,
                100000);
  sts = abs_mmio_read32(kCsrngBase + CSRNG_SW_CMD_STS_REG_OFFSET);
  CHECK_EQ(
      (sts >> CSRNG_SW_CMD_STS_CMD_STS_OFFSET) & CSRNG_SW_CMD_STS_CMD_STS_MASK,
      0u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_RESEED_COUNTER_2_REG_OFFSET), 0u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET), 0u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_ERR_CODE_REG_OFFSET), 0u);
  // Re-seed via RESEED (FLAG0=True) so state_db instance_status is 1 with
  // FIPS_FORCE
  sts = csrng_send_sw_cmd(0x00000602u, NULL, 0);
  CHECK_EQ(
      (sts >> CSRNG_SW_CMD_STS_CMD_STS_OFFSET) & CSRNG_SW_CMD_STS_CMD_STS_MASK,
      0u);

  // 3f. Set RESEED_INTERVAL = 1, GENERATE 1 packet -> GENBITS_VLD = 0x3
  // (GENBITS_VLD | GENBITS_FIPS due to FIPS_FORCE), then next GENERATE fails
  // with CMD_STS = 4 (RESEED_CNT_EXCEEDED).
  abs_mmio_write32(kCsrngBase + CSRNG_RESEED_INTERVAL_REG_OFFSET, 1u);
  abs_mmio_write32(kCsrngBase + CSRNG_CMD_REQ_REG_OFFSET, 0x00001003u);
  IBEX_SPIN_FOR((abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_VLD_REG_OFFSET) &
                 (1u << CSRNG_GENBITS_VLD_GENBITS_VLD_BIT)) != 0u,
                100000);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_VLD_REG_OFFSET), 0x3u);
  for (int i = 0; i < 4; ++i) {
    (void)abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_REG_OFFSET);
  }
  IBEX_SPIN_FOR((abs_mmio_read32(kCsrngBase + CSRNG_SW_CMD_STS_REG_OFFSET) &
                 (1u << CSRNG_SW_CMD_STS_CMD_ACK_BIT)) != 0u,
                100000);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_RESEED_COUNTER_2_REG_OFFSET), 1u);

  sts = csrng_send_sw_cmd(0x00001003u, NULL, 0);
  CHECK_EQ(
      (sts >> CSRNG_SW_CMD_STS_CMD_STS_OFFSET) & CSRNG_SW_CMD_STS_CMD_STS_MASK,
      4u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET),
           1u << CSRNG_RECOV_ALERT_STS_CMD_STAGE_RESEED_CNT_ALERT_BIT);
  abs_mmio_write32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET, 0u);
  abs_mmio_write32(kCsrngBase + CSRNG_RESEED_INTERVAL_REG_OFFSET, 0xffffffffu);

  // 4. HW App (EDN0) Command Stage Errors & Disable-While-Active.
  disable_all();
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, 0x9666u);
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9996u);

  // 4a. Invalid ACMD (0x6) via EDN0 SW_CMD_REQ -> EDN0 CMD_STS = 1, CSRNG
  // HW_EXC_STS = 0 (csrng_core.sv:1728-1729 continuous de=cs_enable_fo[50]),
  // INTR_STATE.CS_HW_INST_EXC = 1, RECOV_ALERT_STS bit 13.
  abs_mmio_write32(kEdn0Base + EDN_SW_CMD_REQ_REG_OFFSET, 0x00000006u);
  IBEX_SPIN_FOR((abs_mmio_read32(kEdn0Base + EDN_SW_CMD_STS_REG_OFFSET) &
                 (1u << EDN_SW_CMD_STS_CMD_ACK_BIT)) != 0u,
                100000);
  uint32_t edn_sts = abs_mmio_read32(kEdn0Base + EDN_SW_CMD_STS_REG_OFFSET);
  CHECK_EQ(
      (edn_sts >> EDN_SW_CMD_STS_CMD_STS_OFFSET) & EDN_SW_CMD_STS_CMD_STS_MASK,
      1u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_HW_EXC_STS_REG_OFFSET), 0u);
  CHECK((abs_mmio_read32(kCsrngBase + CSRNG_INTR_STATE_REG_OFFSET) &
         (1u << CSRNG_INTR_STATE_CS_HW_INST_EXC_BIT)) != 0u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET),
           1u << CSRNG_RECOV_ALERT_STS_CMD_STAGE_INVALID_ACMD_ALERT_BIT);

  // Clear HW_EXC_STS (RW0C) and reset EDN0.
  abs_mmio_write32(kCsrngBase + CSRNG_HW_EXC_STS_REG_OFFSET, 0u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_HW_EXC_STS_REG_OFFSET), 0u);
  abs_mmio_write32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET, 0u);
  abs_mmio_write32(kCsrngBase + CSRNG_INTR_STATE_REG_OFFSET, 0xfu);
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9999u);
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9996u);

  // 4b. RESEED_INTERVAL = 0 on EDN0 -> GENERATE triggers RESEED_CNT_EXCEEDED
  // (CMD_STS = 4) and INTR_STATE.CS_HW_INST_EXC = 1.
  abs_mmio_write32(kCsrngBase + CSRNG_RESEED_INTERVAL_REG_OFFSET, 0u);
  abs_mmio_write32(kEdn0Base + EDN_SW_CMD_REQ_REG_OFFSET, 0x00000601u);
  IBEX_SPIN_FOR((abs_mmio_read32(kEdn0Base + EDN_SW_CMD_STS_REG_OFFSET) &
                 (1u << EDN_SW_CMD_STS_CMD_ACK_BIT)) != 0u,
                100000);
  abs_mmio_write32(kEdn0Base + EDN_SW_CMD_REQ_REG_OFFSET, 0x00001003u);
  IBEX_SPIN_FOR((abs_mmio_read32(kEdn0Base + EDN_SW_CMD_STS_REG_OFFSET) &
                 (1u << EDN_SW_CMD_STS_CMD_ACK_BIT)) != 0u,
                100000);
  edn_sts = abs_mmio_read32(kEdn0Base + EDN_SW_CMD_STS_REG_OFFSET);
  CHECK_EQ(
      (edn_sts >> EDN_SW_CMD_STS_CMD_STS_OFFSET) & EDN_SW_CMD_STS_CMD_STS_MASK,
      4u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_HW_EXC_STS_REG_OFFSET), 0u);
  CHECK((abs_mmio_read32(kCsrngBase + CSRNG_INTR_STATE_REG_OFFSET) &
         (1u << CSRNG_INTR_STATE_CS_HW_INST_EXC_BIT)) != 0u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET),
           1u << CSRNG_RECOV_ALERT_STS_CMD_STAGE_RESEED_CNT_ALERT_BIT);
  abs_mmio_write32(kCsrngBase + CSRNG_RESEED_INTERVAL_REG_OFFSET, 0xffffffffu);

  // 4c. Queue INSTANTIATE (FLAG0 = False) via EDN0 while entropy_src is
  // disabled, then disable CSRNG while EDN0 is active.
  disable_all();
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, 0x9666u);
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9996u);
  abs_mmio_write32(kEdn0Base + EDN_SW_CMD_REQ_REG_OFFSET, 0x00000901u);
  // CSRNG is now waiting for entropy_src; disable CSRNG while EDN0 is active.
  // Per csrng_core.sv:930-946, disabling CSRNG clears CMD_RDY and CMD_ACK while
  // preserving the last SW CMD_STS (4 = RESEED_CNT_EXCEEDED).
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, 0x9999u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_SW_CMD_STS_REG_OFFSET),
           4u << CSRNG_SW_CMD_STS_CMD_STS_OFFSET);
  disable_all();

  // 4d. Queue SW INSTANTIATE (FLAG0 = False, 0x00000901) via CSRNG_CMD_REQ
  // while entropy_src is disabled, then push 2 additional words to fill
  // u_prim_fifo_cmd (AppCmdFifoDepth = 2 in csrng_core.sv:61) on CW340 FPGA and
  // exercise the queued cmd_req overflow guard in QEMU (ot_csrng.c:2071-2072),
  // asserting CMD_RDY == 0 on both targets before disabling CSRNG to exercise
  // the queued command discard loop in ot_csrng_handle_enable.
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, 0x9666u);
  CHECK((abs_mmio_read32(kCsrngBase + CSRNG_SW_CMD_STS_REG_OFFSET) &
         (1u << CSRNG_SW_CMD_STS_CMD_RDY_BIT)) != 0u);
  abs_mmio_write32(kCsrngBase + CSRNG_CMD_REQ_REG_OFFSET, 0x00000901u);
  abs_mmio_write32(kCsrngBase + CSRNG_CMD_REQ_REG_OFFSET, 0x00000000u);
  abs_mmio_write32(kCsrngBase + CSRNG_CMD_REQ_REG_OFFSET, 0x00000000u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_SW_CMD_STS_REG_OFFSET) &
               (1u << CSRNG_SW_CMD_STS_CMD_RDY_BIT),
           0u);
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, 0x9999u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_SW_CMD_STS_REG_OFFSET) &
               (1u << CSRNG_SW_CMD_STS_CMD_RDY_BIT),
           0u);
  disable_all();

  // 5. INT_STATE_READ_ENABLE_REGWEN, INT_STATE_NUM, ERR_CODE_TEST, and REGWEN.
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, 0x9666u);
  abs_mmio_write32(kCsrngBase + CSRNG_INT_STATE_READ_ENABLE_REG_OFFSET, 0x5u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_INT_STATE_READ_ENABLE_REG_OFFSET),
           0x5u);
  abs_mmio_write32(kCsrngBase + CSRNG_INT_STATE_READ_ENABLE_REGWEN_REG_OFFSET,
                   0u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase +
                           CSRNG_INT_STATE_READ_ENABLE_REGWEN_REG_OFFSET),
           0u);
  abs_mmio_write32(kCsrngBase + CSRNG_INT_STATE_READ_ENABLE_REG_OFFSET, 0x7u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_INT_STATE_READ_ENABLE_REG_OFFSET),
           0x5u);

  abs_mmio_write32(kCsrngBase + CSRNG_INT_STATE_NUM_REG_OFFSET, 3u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_INT_STATE_NUM_REG_OFFSET), 3u);

  // Trigger ERR_CODE_TEST = 0 (SFIFO_CMD_ERR) -> sets ERR_CODE bit 0 and
  // INTR_STATE.CS_FATAL_ERR without locking MAIN_SM_STATE.
  abs_mmio_write32(kCsrngBase + CSRNG_INTR_STATE_REG_OFFSET, 0xfu);
  abs_mmio_write32(kCsrngBase + CSRNG_ERR_CODE_TEST_REG_OFFSET, 0u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_ERR_CODE_REG_OFFSET), 1u);
  CHECK((abs_mmio_read32(kCsrngBase + CSRNG_INTR_STATE_REG_OFFSET) &
         (1u << CSRNG_INTR_STATE_CS_FATAL_ERR_BIT)) != 0u);

  // Trigger ERR_CODE_TEST = 20 (CMD_STAGE_SM_ERR) -> transitions MAIN_SM_STATE
  // to Error (0x78) and sets MAIN_SM_ERR (bit 21) + CMD_STAGE_SM_ERR (bit 20).
  abs_mmio_write32(kCsrngBase + CSRNG_INTR_STATE_REG_OFFSET, 0xfu);
  abs_mmio_write32(kCsrngBase + CSRNG_ERR_CODE_TEST_REG_OFFSET, 20u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_ERR_CODE_REG_OFFSET),
           1u | (1u << 20) | (1u << 21));
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_MAIN_SM_STATE_REG_OFFSET), 0x78u);
  CHECK((abs_mmio_read32(kCsrngBase + CSRNG_INTR_STATE_REG_OFFSET) &
         (1u << CSRNG_INTR_STATE_CS_FATAL_ERR_BIT)) != 0u);

  // Issue 2 SW command words (UNINSTANTIATE = 0x00000005) while MAIN_SM_STATE
  // is locked in Error (0x78): schedules cmd_scheduler BH while s->state !=
  // CSRNG_IDLE in QEMU (ot_csrng.c:1662) and fills the 2-entry sfifo_cmd in
  // RTL, clearing CMD_RDY = 0 on both targets, then disable/re-enable CTRL.
  abs_mmio_write32(kCsrngBase + CSRNG_CMD_REQ_REG_OFFSET, 0x00000005u);
  abs_mmio_write32(kCsrngBase + CSRNG_CMD_REQ_REG_OFFSET, 0x00000005u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_SW_CMD_STS_REG_OFFSET) &
               (1u << CSRNG_SW_CMD_STS_CMD_RDY_BIT),
           0u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_MAIN_SM_STATE_REG_OFFSET), 0x78u);
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, 0x9999u);
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, 0x9666u);
  CHECK((abs_mmio_read32(kCsrngBase + CSRNG_SW_CMD_STS_REG_OFFSET) &
         (1u << CSRNG_SW_CMD_STS_CMD_RDY_BIT)) != 0u);

  // Lock REGWEN = 0 and verify CTRL, FIPS_FORCE, and ERR_CODE_TEST are locked.
  abs_mmio_write32(kCsrngBase + CSRNG_REGWEN_REG_OFFSET, 0u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_REGWEN_REG_OFFSET), 0u);
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, 0x9999u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_CTRL_REG_OFFSET), 0x9666u);
  abs_mmio_write32(kCsrngBase + CSRNG_FIPS_FORCE_REG_OFFSET, 0u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_FIPS_FORCE_REG_OFFSET), 0x7u);
  abs_mmio_write32(kCsrngBase + CSRNG_ERR_CODE_TEST_REG_OFFSET, 2u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_ERR_CODE_TEST_REG_OFFSET), 20u);

  return true;
}
