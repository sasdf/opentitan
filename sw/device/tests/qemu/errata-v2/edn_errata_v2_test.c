// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_edn.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/irq.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top/csrng_regs.h"
#include "hw/top/edn_regs.h"
#include "hw/top/entropy_src_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kEntropySrcBase = TOP_EARLGREY_ENTROPY_SRC_BASE_ADDR,
  kCsrngBase = TOP_EARLGREY_CSRNG_BASE_ADDR,
  kEdn0Base = TOP_EARLGREY_EDN0_BASE_ADDR,
  kEdn1Base = TOP_EARLGREY_EDN1_BASE_ADDR,
};

static volatile uint32_t g_access_fault_count = 0;
static volatile uint32_t g_last_mcause = 0;

void ottf_exception_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = ibex_mcause_read();
  g_last_mcause = mcause;
  if (mcause == kIbexExcLoadAccessFault || mcause == kIbexExcStoreAccessFault) {
    g_access_fault_count++;
    uint32_t mepc = ibex_mepc_read();
    uint16_t insn16 = *(const volatile uint16_t *)mepc;
    uint32_t step = ((insn16 & 0x3u) != 0x3u) ? 2u : 4u;
    CSR_WRITE(CSR_REG_MEPC, mepc + step);
    return;
  }
  ottf_generic_fault_print(exc_info, "Unhandled exception", mcause);
  abort();
}

static inline void mmio_write16(uint32_t addr, uint16_t val) {
  asm volatile("sh %1, 0(%0)" : : "r"(addr), "r"(val) : "memory");
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

static void verify_edn_permit_and_addrmiss_faults(void) {
  LOG_INFO(
      "Verifying [edn_reg_pkg.sv:371] & [edn_reg_top.sv:1406-1428]: "
      "EDN_PERMIT sub-word write faults (mcause=7) and unmapped 0x48..0x7c "
      "addrmiss faults (mcause=5/7)");

  g_access_fault_count = 0;
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9999u);
  abs_mmio_write32(kEdn0Base + EDN_MAX_NUM_REQS_BETWEEN_RESEEDS_REG_OFFSET,
                   0x12345678u);
  CHECK(g_access_fault_count == 0u);

  // 1-byte write (sb) to CTRL (EDN_PERMIT = 4'b0011) must fault with mcause=7
  // and leave CTRL unchanged at 0x9999.
  abs_mmio_write8(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x66u);
  CHECK(g_access_fault_count == 1u);
  CHECK(g_last_mcause == (uint32_t)kIbexExcStoreAccessFault);
  CHECK(abs_mmio_read32(kEdn0Base + EDN_CTRL_REG_OFFSET) == 0x9999u);

  // 2-byte write (sh) to MAX_NUM_REQS_BETWEEN_RESEEDS (EDN_PERMIT = 4'b1111)
  // must fault with mcause=7 and leave the register unchanged.
  mmio_write16(kEdn0Base + EDN_MAX_NUM_REQS_BETWEEN_RESEEDS_REG_OFFSET,
               0xaaaau);
  CHECK(g_access_fault_count == 2u);
  CHECK(g_last_mcause == (uint32_t)kIbexExcStoreAccessFault);
  CHECK(abs_mmio_read32(kEdn0Base +
                        EDN_MAX_NUM_REQS_BETWEEN_RESEEDS_REG_OFFSET) ==
        0x12345678u);

  // Unmapped offsets 0x48 and 0x7c within the 0x80 aperture must fault.
  (void)abs_mmio_read32(kEdn0Base + 0x48u);
  CHECK(g_access_fault_count == 3u);
  CHECK(g_last_mcause == (uint32_t)kIbexExcLoadAccessFault);

  abs_mmio_write32(kEdn0Base + 0x7cu, 0x0u);
  CHECK(g_access_fault_count == 4u);
  CHECK(g_last_mcause == (uint32_t)kIbexExcStoreAccessFault);

  LOG_INFO(
      "[edn_reg_pkg.sv:371] & [edn_reg_top.sv:1406-1428] confirmed: faults=%u",
      g_access_fault_count);
}

static void verify_edn_recov_alert_sts_rw0c_mubi_preserve(void) {
  LOG_INFO(
      "Verifying [edn_core.sv:421-485,691-695] & [edn_reg_top.sv:1174-1289]: "
      "RECOV_ALERT_STS *_FIELD_ALERT bits (0..3) ignore rw0c clears while "
      "CTRL holds invalid mubi4_t");

  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x0000u);
  uint32_t recov_before =
      abs_mmio_read32(kEdn0Base + EDN_RECOV_ALERT_STS_REG_OFFSET);
  CHECK((recov_before & 0xfu) == 0xfu);

  // Attempt rw0c clear while CTRL still contains 0x0000: continuous .de = 1'b1,
  // .d = 1'b1 in edn_core.sv:421-485,691-695 overrides software rw0c clear!
  abs_mmio_write32(kEdn0Base + EDN_RECOV_ALERT_STS_REG_OFFSET, 0x00000000u);
  uint32_t recov_during =
      abs_mmio_read32(kEdn0Base + EDN_RECOV_ALERT_STS_REG_OFFSET);
  CHECK((recov_during & 0xfu) == 0xfu);

  // Repair CTRL to valid MuBi4False (0x9999), then clear RECOV_ALERT_STS.
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9999u);
  abs_mmio_write32(kEdn0Base + EDN_RECOV_ALERT_STS_REG_OFFSET, 0x00000000u);
  uint32_t recov_after =
      abs_mmio_read32(kEdn0Base + EDN_RECOV_ALERT_STS_REG_OFFSET);
  CHECK(recov_after == 0u);

  LOG_INFO(
      "[edn_core.sv:421-485,691-695] confirmed: RECOV_ALERT_STS remained 0x%x "
      "during "
      "rw0c clear with invalid CTRL, cleared to 0x%x after CTRL repair",
      recov_during, recov_after);
}

static void verify_edn_cmd_fifo_rst_continuous_clear(void) {
  LOG_INFO(
      "Verifying [edn_core.sv:449-451,659,678]: CTRL.CMD_FIFO_RST == 0x6 "
      "continuously holds replay FIFOs cleared and drops writes");

  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x6996u);
  for (uint32_t i = 0; i < 20u; ++i) {
    abs_mmio_write32(kEdn0Base + EDN_RESEED_CMD_REG_OFFSET, 0x00000602u);
    abs_mmio_write32(kEdn0Base + EDN_GENERATE_CMD_REG_OFFSET, 0x00001903u);
  }
  uint32_t err_code = abs_mmio_read32(kEdn0Base + EDN_ERR_CODE_REG_OFFSET);
  CHECK(err_code == 0u);

  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9999u);
  LOG_INFO(
      "[edn_core.sv:449-451,659,678] confirmed: 20 writes while "
      "CMD_FIFO_RST=0x6 produced ERR_CODE=0x%x",
      err_code);
}

static void verify_edn_err_code_test_and_auto_underflow(void) {
  LOG_INFO(
      "Verifying [edn_core.sv:307-318]: ERR_CODE_TEST bits 0,1,28..30 gated "
      "by EDN_ENABLE, and bits 28..30 omitted from event_edn_fatal_err");

  // 1. With EDN0 disabled (CTRL = 0x9999), writing 0, 1, 28, 29, 30 to
  // ERR_CODE_TEST is completely ignored (ERR_CODE stays 0).
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9999u);
  abs_mmio_write32(kEdn0Base + EDN_INTR_STATE_REG_OFFSET, 0xffffffffu);
  const uint32_t kFifoErrBits[] = {
      EDN_ERR_CODE_SFIFO_RESCMD_ERR_BIT, EDN_ERR_CODE_SFIFO_GENCMD_ERR_BIT,
      EDN_ERR_CODE_FIFO_WRITE_ERR_BIT,   EDN_ERR_CODE_FIFO_READ_ERR_BIT,
      EDN_ERR_CODE_FIFO_STATE_ERR_BIT,
  };
  for (uint32_t i = 0; i < 5u; ++i) {
    abs_mmio_write32(kEdn0Base + EDN_ERR_CODE_TEST_REG_OFFSET, kFifoErrBits[i]);
  }
  CHECK(abs_mmio_read32(kEdn0Base + EDN_ERR_CODE_REG_OFFSET) == 0u);
  CHECK((abs_mmio_read32(kEdn0Base + EDN_INTR_STATE_REG_OFFSET) &
         (1u << EDN_INTR_STATE_EDN_FATAL_ERR_BIT)) == 0u);

  // 2. With EDN0 enabled (CTRL = 0x9996), writing FIFO type bits 28, 29, 30 to
  // ERR_CODE_TEST latches ERR_CODE[30:28] = 0x70000000 WITHOUT asserting
  // INTR_STATE.EDN_FATAL_ERR (because edn_core.sv:314-317 omits
  // fifo_{write,read,state}_err_sum from event_edn_fatal_err)!
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9996u);
  abs_mmio_write32(kEdn0Base + EDN_INTR_STATE_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kEdn0Base + EDN_ERR_CODE_TEST_REG_OFFSET,
                   EDN_ERR_CODE_FIFO_WRITE_ERR_BIT);
  abs_mmio_write32(kEdn0Base + EDN_ERR_CODE_TEST_REG_OFFSET,
                   EDN_ERR_CODE_FIFO_READ_ERR_BIT);
  abs_mmio_write32(kEdn0Base + EDN_ERR_CODE_TEST_REG_OFFSET,
                   EDN_ERR_CODE_FIFO_STATE_ERR_BIT);

  uint32_t type_err_code = abs_mmio_read32(kEdn0Base + EDN_ERR_CODE_REG_OFFSET);
  uint32_t type_intr = abs_mmio_read32(kEdn0Base + EDN_INTR_STATE_REG_OFFSET);
  uint32_t exp_type_bits = (1u << EDN_ERR_CODE_FIFO_WRITE_ERR_BIT) |
                           (1u << EDN_ERR_CODE_FIFO_READ_ERR_BIT) |
                           (1u << EDN_ERR_CODE_FIFO_STATE_ERR_BIT);
  CHECK(type_err_code == exp_type_bits);
  CHECK((type_intr & (1u << EDN_INTR_STATE_EDN_FATAL_ERR_BIT)) == 0u);

  // By contrast, writing bit 0 (SFIFO_RESCMD_ERR) while enabled immediately
  // asserts INTR_STATE.EDN_FATAL_ERR!
  abs_mmio_write32(kEdn0Base + EDN_ERR_CODE_TEST_REG_OFFSET,
                   EDN_ERR_CODE_SFIFO_RESCMD_ERR_BIT);
  uint32_t src_intr = abs_mmio_read32(kEdn0Base + EDN_INTR_STATE_REG_OFFSET);
  CHECK((src_intr & (1u << EDN_INTR_STATE_EDN_FATAL_ERR_BIT)) != 0u);
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9999u);

  LOG_INFO(
      "[edn_core.sv:314-317] confirmed: ERR_CODE_TEST=28..30 latched "
      "ERR_CODE=0x%x with INTR_STATE=0x%x (no EDN_FATAL_ERR), whereas bit 0 "
      "set INTR_STATE=0x%x",
      type_err_code, type_intr, src_intr);

  // 3. Verify [edn_core.sv:620-632] on EDN0:
  // Queue a valid 1-word RESEED command (0x00000602) in RESEED_CMD with
  // MAX_NUM_REQS_BETWEEN_RESEEDS = 0, enable Auto Mode (0x9696), and send
  // UNINSTANTIATE (0x00000005) on SW_CMD_REQ so CSRNG stays uninstantiated.
  // When Auto Mode dispatches RESEED, cs_hw_cmd_handshake latches
  // HW_CMD_STS.AUTO_MODE = 1, and CSRNG rejects RESEED on the uninstantiated
  // instance (csrng_ack_err = 1), transitioning MAIN_SM_STATE to
  // RejectCsrngEntropy (0x018) while HW_CMD_STS.AUTO_MODE remains sticky at 1!
  LOG_INFO(
      "Verifying [edn_core.sv:620-632]: HW_CMD_STS.AUTO_MODE stays latched at "
      "1 in RejectCsrngEntropy (0x018)");
  reset_entropy_complex_enable_csrng();
  abs_mmio_write32(kEdn0Base + EDN_MAX_NUM_REQS_BETWEEN_RESEEDS_REG_OFFSET, 0u);
  abs_mmio_write32(kEdn0Base + EDN_RESEED_CMD_REG_OFFSET, 0x00000602u);
  abs_mmio_write32(kEdn0Base + EDN_CTRL_REG_OFFSET, 0x9696u);
  abs_mmio_write32(kEdn0Base + EDN_SW_CMD_REQ_REG_OFFSET, 0x00000005u);
  IBEX_SPIN_FOR(
      abs_mmio_read32(kEdn0Base + EDN_MAIN_SM_STATE_REG_OFFSET) == 0x018u,
      100000);
  uint32_t sm_reject =
      abs_mmio_read32(kEdn0Base + EDN_MAIN_SM_STATE_REG_OFFSET);
  uint32_t hw_sts_reject =
      abs_mmio_read32(kEdn0Base + EDN_HW_CMD_STS_REG_OFFSET);
  uint32_t recov_reject =
      abs_mmio_read32(kEdn0Base + EDN_RECOV_ALERT_STS_REG_OFFSET);
  CHECK(sm_reject == 0x018u);
  CHECK((hw_sts_reject & (1u << EDN_HW_CMD_STS_AUTO_MODE_BIT)) != 0u);
  CHECK((recov_reject & (1u << EDN_RECOV_ALERT_STS_CSRNG_ACK_ERR_BIT)) != 0u);

  // 4. Verify [edn_core.sv:307-318], [edn_core.sv:668-687], and
  // [dif_edn.c:154-181] on EDN1:
  // - Empty RESEED_CMD underflow in Auto Mode sets SFIFO_RESCMD_ERR |
  //   FIFO_READ_ERR and INTR_STATE.EDN_FATAL_ERR while entering
  //   AutoSendReseedCmd (0x06a), and transitions 0x06a -> RejectCsrngEntropy
  //   (0x018) instead of Error (0x17e) ([edn_core.sv:307-318]);
  // - Calling dif_edn_get_status(&edn1, kDifEdnStatusCsrngStatus, &csrng_err)
  //   returns csrng_err == false even though HW_CMD_STS.CMD_STS != 0 because
  //   dif_edn_get_status ([dif_edn.c:154-181]) only reads SW_CMD_STS (0x28);
  // - Writing ERR_CODE_TEST = 21 (EDN_MAIN_SM_ERR) simultaneously escalates
  //   both EDN_MAIN_SM_ERR (21) and EDN_ACK_SM_ERR (20), moves MAIN_SM_STATE
  //   to Error (0x17e), and keeps HW_CMD_STS.AUTO_MODE latched at 1!
  LOG_INFO(
      "Verifying [edn_core.sv:307-318] & [dif_edn.c:154-181]: Auto-mode "
      "empty-FIFO underflow does not escalate to Error (0x17e), and "
      "dif_edn_get_status ignores HW_CMD_STS");
  reset_entropy_complex_enable_csrng();
  abs_mmio_write32(kEdn1Base + EDN_RESEED_CMD_REG_OFFSET, 0x00000000u);
  abs_mmio_write32(kEdn1Base + EDN_CTRL_REG_OFFSET, 0x6999u);
  abs_mmio_write32(kEdn1Base + EDN_CTRL_REG_OFFSET, 0x9999u);
  uint32_t exp_rescmd_rd_err = (1u << EDN_ERR_CODE_SFIFO_RESCMD_ERR_BIT) |
                               (1u << EDN_ERR_CODE_FIFO_READ_ERR_BIT);
  abs_mmio_write32(kEdn1Base + EDN_MAX_NUM_REQS_BETWEEN_RESEEDS_REG_OFFSET, 0u);
  abs_mmio_write32(kEdn1Base + EDN_CTRL_REG_OFFSET, 0x9696u);
  abs_mmio_write32(kEdn1Base + EDN_SW_CMD_REQ_REG_OFFSET, 0x00000601u);
  IBEX_SPIN_FOR((abs_mmio_read32(kEdn1Base + EDN_ERR_CODE_REG_OFFSET) &
                 exp_rescmd_rd_err) == exp_rescmd_rd_err,
                100000);

  uint32_t sm_underflow =
      abs_mmio_read32(kEdn1Base + EDN_MAIN_SM_STATE_REG_OFFSET);
  uint32_t hw_sts_underflow =
      abs_mmio_read32(kEdn1Base + EDN_HW_CMD_STS_REG_OFFSET);
  CHECK((abs_mmio_read32(kEdn1Base + EDN_INTR_STATE_REG_OFFSET) &
         (1u << EDN_INTR_STATE_EDN_FATAL_ERR_BIT)) != 0u);
  CHECK(sm_underflow == 0x06au || sm_underflow == 0x018u);
  IBEX_SPIN_FOR(
      abs_mmio_read32(kEdn1Base + EDN_MAIN_SM_STATE_REG_OFFSET) == 0x018u,
      100000);
  sm_underflow = abs_mmio_read32(kEdn1Base + EDN_MAIN_SM_STATE_REG_OFFSET);
  hw_sts_underflow = abs_mmio_read32(kEdn1Base + EDN_HW_CMD_STS_REG_OFFSET);
  CHECK(sm_underflow == 0x018u);
  CHECK((hw_sts_underflow & (1u << EDN_HW_CMD_STS_AUTO_MODE_BIT)) != 0u);
  CHECK(((hw_sts_underflow >> EDN_HW_CMD_STS_CMD_STS_OFFSET) &
         EDN_HW_CMD_STS_CMD_STS_MASK) != 0u);

  dif_edn_t edn1;
  CHECK_DIF_OK(dif_edn_init(mmio_region_from_addr(kEdn1Base), &edn1));
  bool dif_csrng_err = true;
  CHECK_DIF_OK(
      dif_edn_get_status(&edn1, kDifEdnStatusCsrngStatus, &dif_csrng_err));
  CHECK(!dif_csrng_err);

  abs_mmio_write32(kEdn1Base + EDN_ERR_CODE_TEST_REG_OFFSET,
                   EDN_ERR_CODE_EDN_MAIN_SM_ERR_BIT);
  uint32_t err_escalated = abs_mmio_read32(kEdn1Base + EDN_ERR_CODE_REG_OFFSET);
  uint32_t sm_escalated =
      abs_mmio_read32(kEdn1Base + EDN_MAIN_SM_STATE_REG_OFFSET);
  uint32_t hw_sts_error =
      abs_mmio_read32(kEdn1Base + EDN_HW_CMD_STS_REG_OFFSET);
  uint32_t exp_both_sm_errs = (1u << EDN_ERR_CODE_EDN_MAIN_SM_ERR_BIT) |
                              (1u << EDN_ERR_CODE_EDN_ACK_SM_ERR_BIT);
  CHECK((err_escalated & exp_both_sm_errs) == exp_both_sm_errs);
  CHECK(sm_escalated == 0x17eu);
  CHECK((hw_sts_error & (1u << EDN_HW_CMD_STS_AUTO_MODE_BIT)) != 0u);

  LOG_INFO(
      "[edn_core.sv:307-318], [edn_core.sv:620-632], & [dif_edn.c:154-181] "
      "confirmed: reject state=0x%x (AUTO_MODE=%u), underflow state=0x%x "
      "(!=0x17e), HW_CMD_STS.CMD_STS=%u vs dif_edn_get_status=%u, escalated "
      "ERR_CODE=0x%x, state=0x%x",
      sm_reject, (hw_sts_reject >> EDN_HW_CMD_STS_AUTO_MODE_BIT) & 1u,
      sm_underflow,
      (hw_sts_underflow >> EDN_HW_CMD_STS_CMD_STS_OFFSET) &
          EDN_HW_CMD_STS_CMD_STS_MASK,
      dif_csrng_err ? 1u : 0u, err_escalated, sm_escalated);
}

bool test_main(void) {
  irq_external_ctrl(false);
  irq_global_ctrl(false);

  reset_entropy_complex_enable_csrng();

  verify_edn_permit_and_addrmiss_faults();
  verify_edn_recov_alert_sts_rw0c_mubi_preserve();
  verify_edn_cmd_fifo_rst_continuous_clear();
  verify_edn_err_code_test_and_auto_underflow();

  LOG_INFO("All edn v2 errata checks confirmed on CW340 FPGA!");
  return true;
}
