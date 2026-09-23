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

#include "alert_handler_regs.h"
#include "csrng_regs.h"
#include "edn_regs.h"
#include "entropy_src_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "rv_core_ibex_regs.h"

OTTF_DEFINE_TEST_CONFIG(.ignore_alerts = true);

#define CHECK_EQ(a, b, ...) CHECK((a) == (b), __VA_ARGS__)

enum {
  kAlertHandlerBase = TOP_EARLGREY_ALERT_HANDLER_BASE_ADDR,
  kEntropySrcBase = TOP_EARLGREY_ENTROPY_SRC_BASE_ADDR,
  kCsrngBase = TOP_EARLGREY_CSRNG_BASE_ADDR,
  kEdn0Base = TOP_EARLGREY_EDN0_BASE_ADDR,
  kEdn1Base = TOP_EARLGREY_EDN1_BASE_ADDR,
  kIbexBase = TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR,
  kCsrngRecovAlertId = kTopEarlgreyAlertIdCsrngRecovAlert,
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
  abs_mmio_write32(kIbexBase + RV_CORE_IBEX_ERR_STATUS_REG_OFFSET, UINT32_MAX);
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

  // =========================================================================
  // 1. [csrng_core.sv:754-761] / [E3-01] (TRUE_SILICON_ERRATA) &
  //    [csrng_core.sv:783-846] / [E2-01a] (INTENDED_SECURITY_HARDENING):
  //    RECOV_ALERT_STS.FIPS_FORCE_ENABLE_FIELD_ALERT (bit 3) is omitted from
  //    recov_alert_event in csrng_core.sv:754-761 so recov_alert_o never fires
  //    to alert_handler when only CTRL.FIPS_FORCE_ENABLE is invalid (0x5999),
  //    whereas invalid CTRL.READ_INT_STATE (0x9599) immediately fires
  //    recov_alert_o. Also confirm continuous de=pfa prevents rw0c clearing
  //    while CTRL holds an invalid mubi4_t.
  // =========================================================================
  LOG_INFO(
      "Verifying [csrng_core.sv:754-761 / E3-01] (TRUE_SILICON_ERRATA) & "
      "[E2-01a] (INTENDED_SECURITY_HARDENING): FIPS_FORCE_ENABLE omitted "
      "from recov_alert_event & continuous de=pfa rw0c block...");
  uint32_t alert_en_addr = kAlertHandlerBase +
                           ALERT_HANDLER_ALERT_EN_SHADOWED_0_REG_OFFSET +
                           kCsrngRecovAlertId * 4u;
  uint32_t alert_cause_addr = kAlertHandlerBase +
                              ALERT_HANDLER_ALERT_CAUSE_0_REG_OFFSET +
                              kCsrngRecovAlertId * 4u;
  abs_mmio_write32_shadowed(alert_en_addr, 1u);
  abs_mmio_write32(alert_cause_addr, 1u);
  CHECK_EQ(abs_mmio_read32(alert_cause_addr), 0u,
           "Expected ALERT_CAUSE[51] cleared before test");

  // Write invalid MuBi4 (0x5) ONLY to CTRL.FIPS_FORCE_ENABLE (bits 15:12).
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, 0x5999u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET),
           1u << CSRNG_RECOV_ALERT_STS_FIPS_FORCE_ENABLE_FIELD_ALERT_BIT,
           "[E3-01] Expected RECOV_ALERT_STS bit 3 set on CTRL=0x5999");

  // [E2-01a]: Verify rw0c write of 0 does NOT clear bit 3 while CTRL is
  // invalid.
  abs_mmio_write32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET, 0u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET),
           1u << CSRNG_RECOV_ALERT_STS_FIPS_FORCE_ENABLE_FIELD_ALERT_BIT,
           "[E2-01a] Expected rw0c clear blocked while CTRL holds invalid "
           "mubi4_t");

  // [E3-01]: Wait 20us across CDC and verify ALERT_CAUSE[51] remains 0!
  busy_spin_micros(20);
  CHECK_EQ(abs_mmio_read32(alert_cause_addr), 0u,
           "[E3-01] Expected recov_alert_o NOT to fire for "
           "FIPS_FORCE_ENABLE_FIELD_ALERT (bit 3)");

  // Contrast: write invalid MuBi4 (0x5) to CTRL.READ_INT_STATE (0x9599) ->
  // recov_alert_o fires immediately and ALERT_CAUSE[51] latches 1!
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, 0x9599u);
  busy_spin_micros(20);
  CHECK_EQ(abs_mmio_read32(alert_cause_addr), 1u,
           "[E3-01] Expected recov_alert_o to fire for "
           "READ_INT_STATE_FIELD_ALERT (bit 2)");

  // Restore valid CTRL (0x9999), clear RECOV_ALERT_STS (rw0c now succeeds),
  // and clean up alert_handler.
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, 0x9999u);
  abs_mmio_write32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET, 0u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_RECOV_ALERT_STS_REG_OFFSET), 0u,
           "[E2-01a] Expected RECOV_ALERT_STS rw0c clear to succeed once CTRL "
           "is valid");
  abs_mmio_write32_shadowed(alert_en_addr, 0u);
  abs_mmio_write32(alert_cause_addr, 1u);

  // =========================================================================
  // 2. [csrng_core.sv:1047-1054] / [E3-02] (TRUE_SILICON_ERRATA):
  //    HW_EXC_STS (rw0c) self-clears to 0x0000 after 1 clock cycle because
  //    hw2reg.hw_exc_sts.de = cs_enable_fo[50] (1'b1) instead of
  //    |hw_exception_sts, while INTR_STATE.CS_HW_INST_EXC latches 1.
  // =========================================================================
  LOG_INFO(
      "Verifying [csrng_core.sv:1047-1054 / E3-02] (TRUE_SILICON_ERRATA): "
      "HW_EXC_STS 1-cycle self-clear on HW app exception...");
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, 0x9666u);
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9996u);
  abs_mmio_write32(kEdn0Base + EDN_SW_CMD_REQ_REG_OFFSET, 0x00000006u);
  IBEX_SPIN_FOR((abs_mmio_read32(kEdn0Base + EDN_SW_CMD_STS_REG_OFFSET) &
                 (1u << EDN_SW_CMD_STS_CMD_ACK_BIT)) != 0u,
                100000);
  uint32_t edn_sts = abs_mmio_read32(kEdn0Base + EDN_SW_CMD_STS_REG_OFFSET);
  CHECK_EQ(
      (edn_sts >> EDN_SW_CMD_STS_CMD_STS_OFFSET) & EDN_SW_CMD_STS_CMD_STS_MASK,
      1u, "[E3-02] Expected EDN0 CMD_STS == 1 (INVALID_ACMD)");
  CHECK((abs_mmio_read32(kCsrngBase + CSRNG_INTR_STATE_REG_OFFSET) &
         (1u << CSRNG_INTR_STATE_CS_HW_INST_EXC_BIT)) != 0u,
        "[E3-02] Expected INTR_STATE.CS_HW_INST_EXC == 1");
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_HW_EXC_STS_REG_OFFSET), 0u,
           "[E3-02] Expected HW_EXC_STS == 0x0000 due to 1-cycle self-clear");
  disable_all();

  // =========================================================================
  // 3. [csrng_core.sv:948-952] / [E1-02] (SPEC_DOC_ERRATA):
  //    Reading GENBITS while CTRL.SW_APP_ENABLE is false (0x9) returns 0x0 on
  //    the bus yet destructively pops u_prim_packer_fifo_sw_genbits, and
  //    GENBITS_VLD remains ungated (reports 0x3).
  // =========================================================================
  LOG_INFO(
      "Verifying [csrng_core.sv:948-952 / E1-02] (SPEC_DOC_ERRATA): GENBITS "
      "read when !SW_APP_ENABLE returns 0 yet pops FIFO...");
  abs_mmio_write32(kCsrngBase + CSRNG_FIPS_FORCE_REG_OFFSET, 0x7u);
  // CTRL = 0x6696: FIPS_FORCE_ENABLE=0x6, READ_INT_STATE=0x6,
  // SW_APP_ENABLE=0x9 (False!), ENABLE=0x6.
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, 0x6696u);
  uint32_t sts = csrng_send_sw_cmd(0x00000601u, NULL, 0);
  CHECK_EQ(
      (sts >> CSRNG_SW_CMD_STS_CMD_STS_OFFSET) & CSRNG_SW_CMD_STS_CMD_STS_MASK,
      0u, "Expected INSTANTIATE success");

  // Issue GENERATE (GLEN = 1 -> 128 bits = 4 words).
  abs_mmio_write32(kCsrngBase + CSRNG_CMD_REQ_REG_OFFSET, 0x00001003u);
  IBEX_SPIN_FOR((abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_VLD_REG_OFFSET) &
                 (1u << CSRNG_GENBITS_VLD_GENBITS_VLD_BIT)) != 0u,
                100000);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_VLD_REG_OFFSET), 0x3u,
           "[E1-02] Expected GENBITS_VLD == 0x3 even when SW_APP_ENABLE=False");
  for (int i = 0; i < 4; ++i) {
    uint32_t gb = abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_REG_OFFSET);
    CHECK_EQ(gb, 0u,
             "[E1-02] Expected GENBITS read #%d to return 0 when "
             "SW_APP_ENABLE=False",
             i);
  }
  IBEX_SPIN_FOR((abs_mmio_read32(kCsrngBase + CSRNG_SW_CMD_STS_REG_OFFSET) &
                 (1u << CSRNG_SW_CMD_STS_CMD_ACK_BIT)) != 0u,
                100000);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_GENBITS_VLD_REG_OFFSET) & 1u, 0u,
           "[E1-02] Expected 4 reads to pop and drain GENBITS FIFO");
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_RESEED_COUNTER_2_REG_OFFSET), 1u,
           "[E1-02] Expected RESEED_COUNTER_2 == 1 after GENERATE completion");

  // =========================================================================
  // 4. [csrng_state_db.sv:142-175] / [E2-01b] (SPEC_DOC_ERRATA):
  //    Writing INT_STATE_NUM while CTRL.READ_INT_STATE is false (0x9) updates
  //    int_st_dump_id_q (to instance 2), but reg_rd_ptr_q remains forced to
  //    4'hf. Enabling CTRL.READ_INT_STATE afterwards without re-writing
  //    INT_STATE_NUM causes the 1st INT_STATE_VAL read to return 0x0 (ptr=4'hf)
  //    and the 2nd read to return word 0 (RESEED_COUNTER_2 == 1).
  // =========================================================================
  LOG_INFO(
      "Verifying [csrng_state_db.sv:142-175 / E2-01b] (SPEC_DOC_ERRATA): "
      "INT_STATE_NUM write when !READ_INT_STATE leaves reg_rd_ptr_q=4'hf...");
  abs_mmio_write32(kCsrngBase + CSRNG_INT_STATE_READ_ENABLE_REG_OFFSET, 0x7u);
  // Set READ_INT_STATE = MuBi4False (0x9): CTRL = 0x6966.
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, 0x6966u);
  abs_mmio_write32(kCsrngBase + CSRNG_INT_STATE_NUM_REG_OFFSET, 2u);
  // Now enable READ_INT_STATE = MuBi4True (0x6) without re-writing
  // INT_STATE_NUM.
  abs_mmio_write32(kCsrngBase + CSRNG_CTRL_REG_OFFSET, 0x6666u);
  uint32_t dump_w_ptr_f =
      abs_mmio_read32(kCsrngBase + CSRNG_INT_STATE_VAL_REG_OFFSET);
  uint32_t dump_w_ptr_0 =
      abs_mmio_read32(kCsrngBase + CSRNG_INT_STATE_VAL_REG_OFFSET);
  CHECK_EQ(dump_w_ptr_f, 0u,
           "[E2-01b] Expected 1st INT_STATE_VAL read (reg_rd_ptr_q=4'hf) to "
           "return 0");
  CHECK_EQ(dump_w_ptr_0, 1u,
           "[E2-01b] Expected 2nd INT_STATE_VAL read (reg_rd_ptr_q=4'h0, "
           "reseed_counter) to return 1");

  // =========================================================================
  // 5. [csrng_core.sv:1765-1771] / [E2-02] (INTENDED_SECURITY_HARDENING):
  //    UNINSTANTIATE (UNI) synchronously zeroizes all 418 bits of
  //    internal_states_q[id], including RESEED_COUNTER_2.
  // =========================================================================
  LOG_INFO(
      "Verifying [csrng_core.sv:1765-1771 / E2-02] "
      "(INTENDED_SECURITY_HARDENING): UNINSTANTIATE zeroizes "
      "RESEED_COUNTER_2...");
  sts = csrng_send_sw_cmd(0x00000005u, NULL, 0);
  CHECK_EQ(
      (sts >> CSRNG_SW_CMD_STS_CMD_STS_OFFSET) & CSRNG_SW_CMD_STS_CMD_STS_MASK,
      0u, "Expected UNINSTANTIATE success");
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_RESEED_COUNTER_2_REG_OFFSET), 0u,
           "[E2-02] Expected RESEED_COUNTER_2 == 0 after UNINSTANTIATE");

  // =========================================================================
  // 6. [csrng_reg_top.sv:2203-2232] / [E2-03] (INTENDED_SECURITY_HARDENING):
  //    Sub-word CSR writes (CSRNG_PERMIT) and unmapped offsets 0x60..0x7c
  //    (addrmiss) raise synchronous TileLink bus errors (mcause=7 / 5).
  // =========================================================================
  LOG_INFO(
      "Verifying [csrng_reg_top.sv:2203-2232 / E2-03] "
      "(INTENDED_SECURITY_HARDENING): CSRNG_PERMIT sub-word write fault & "
      "0x7c addrmiss fault...");
  abs_mmio_write32(kCsrngBase + CSRNG_RESEED_INTERVAL_REG_OFFSET, 0xa5a55a5au);
  store_fault_count = 0;
  abs_mmio_write8(kCsrngBase + CSRNG_RESEED_INTERVAL_REG_OFFSET, 0xffu);
  CHECK_EQ(store_fault_count, 1u,
           "[E2-03] Expected Store Access Fault on sub-word write to "
           "RESEED_INTERVAL");
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_RESEED_INTERVAL_REG_OFFSET),
           0xa5a55a5au,
           "[E2-03] Expected sub-word write to RESEED_INTERVAL to be dropped");

  load_fault_count = 0;
  (void)abs_mmio_read32(kCsrngBase + 0x7cu);
  CHECK_EQ(load_fault_count, 1u,
           "[E2-03] Expected Load Access Fault at unmapped offset 0x7c");

  // =========================================================================
  // 7. [csrng_core.sv:501-532] / [E1-01] (TRUE_SILICON_ERRATA):
  //    Asymmetric CTRL.ENABLE gating between ERR_CODE (bits 0..25, 28..30
  //    gated; bit 26 CMD_GEN_CNT_ERR ungated) and event_cs_fatal_err (bits
  //    20..26 ungated).
  //    When CTRL.ENABLE is false (0x9999):
  //    - ERR_CODE_TEST = 0 -> ignored by both ERR_CODE and INTR_STATE.
  //    - ERR_CODE_TEST = 20 -> fires INTR_STATE.CS_FATAL_ERR and locks
  //      MAIN_SM_STATE into Error (0x78) while ERR_CODE stays 0x00000000!
  //    - ERR_CODE_TEST = 26 -> latches bit 26 (CMD_GEN_CNT_ERR) in ERR_CODE
  //      even while CTRL.ENABLE is false!
  // =========================================================================
  LOG_INFO(
      "Verifying [csrng_core.sv:501-532 / E1-01] (TRUE_SILICON_ERRATA): "
      "Asymmetric CTRL.ENABLE gating on ERR_CODE vs event_cs_fatal_err...");
  disable_all();
  abs_mmio_write32(kCsrngBase + CSRNG_ERR_CODE_TEST_REG_OFFSET, 0u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_ERR_CODE_REG_OFFSET), 0u,
           "[E1-01] Expected ERR_CODE == 0 for ERR_CODE_TEST=0 when disabled");
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_INTR_STATE_REG_OFFSET) &
               (1u << CSRNG_INTR_STATE_CS_FATAL_ERR_BIT),
           0u,
           "[E1-01] Expected CS_FATAL_ERR == 0 for ERR_CODE_TEST=0 when "
           "disabled");

  abs_mmio_write32(kCsrngBase + CSRNG_ERR_CODE_TEST_REG_OFFSET, 20u);
  CHECK((abs_mmio_read32(kCsrngBase + CSRNG_INTR_STATE_REG_OFFSET) &
         (1u << CSRNG_INTR_STATE_CS_FATAL_ERR_BIT)) != 0u,
        "[E1-01] Expected CS_FATAL_ERR == 1 for ERR_CODE_TEST=20 when "
        "disabled");
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_ERR_CODE_REG_OFFSET), 0u,
           "[E1-01] Expected ERR_CODE == 0 for ERR_CODE_TEST=20 when disabled "
           "because bit 20 is gated by cs_enable_fo[18]");

  abs_mmio_write32(kCsrngBase + CSRNG_ERR_CODE_TEST_REG_OFFSET, 26u);
  CHECK_EQ(abs_mmio_read32(kCsrngBase + CSRNG_ERR_CODE_REG_OFFSET), 1u << 26,
           "[E1-01] Expected ERR_CODE == (1 << 26) for ERR_CODE_TEST=26 when "
           "disabled because bit 26 (cmd_gen_cnt_err) is ungated");

  LOG_INFO("All CSRNG errata & security hardening checks passed!");
  return true;
}
