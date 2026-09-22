// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_alert_handler.h"
#include "sw/device/lib/dif/dif_rv_plic.h"
#include "sw/device/lib/dif/dif_uart.h"
#include "sw/device/lib/runtime/irq.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "alert_handler_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "rv_plic_regs.h"

OTTF_DEFINE_TEST_CONFIG();

static volatile uint32_t fault_count = 0;
static volatile uint32_t last_mcause = 0;

void ottf_exception_handler(uint32_t *exc_info) {
  uint32_t mcause = 0;
  CSR_READ(CSR_REG_MCAUSE, &mcause);
  last_mcause = mcause;
  fault_count++;
}

static bool expect_load_fault_32(mmio_region_t base, uint32_t offset) {
  uint32_t before = fault_count;
  last_mcause = 0;
  (void)mmio_region_read32(base, (ptrdiff_t)offset);
  return (fault_count == before + 1u) && (last_mcause == 5u);
}

static bool expect_store_fault_32(mmio_region_t base, uint32_t offset,
                                  uint32_t val) {
  uint32_t before = fault_count;
  last_mcause = 0;
  mmio_region_write32(base, (ptrdiff_t)offset, val);
  return (fault_count == before + 1u) && (last_mcause == 7u);
}

static bool expect_store_fault_8(mmio_region_t base, uint32_t offset,
                                 uint8_t val) {
  uint32_t before = fault_count;
  last_mcause = 0;
  mmio_region_write8(base, (ptrdiff_t)offset, val);
  return (fault_count == before + 1u) && (last_mcause == 7u);
}

enum {
  kMipMsipBit = 3,
  kMipMeipBit = 11,
};

static bool get_mip_meip(void) {
  uint32_t mip = 0;
  CSR_READ(CSR_REG_MIP, &mip);
  return ((mip >> kMipMeipBit) & 1u) != 0;
}

static bool get_mip_msip(void) {
  uint32_t mip = 0;
  CSR_READ(CSR_REG_MIP, &mip);
  return ((mip >> kMipMsipBit) & 1u) != 0;
}

static void test_prio0_and_ie_last_word_masks(mmio_region_t plic_base) {
  // 1. In rv_plic_reg_top.sv (u_prio0), PRIO0 at offset 0x0 is a 2-bit
  // SwAccessRW register (mask 0x3), even though interrupt source 0 is tied
  // off to 0.
  mmio_region_write32(plic_base, RV_PLIC_PRIO0_REG_OFFSET, 0xffffffffu);
  uint32_t prio0 = mmio_region_read32(plic_base, RV_PLIC_PRIO0_REG_OFFSET);
  CHECK(prio0 == 0x3u, "Expected PRIO0=0x3 after writing 0xffffffff, got 0x%x",
        prio0);
  mmio_region_write32(plic_base, RV_PLIC_PRIO0_REG_OFFSET, 0u);
  prio0 = mmio_region_read32(plic_base, RV_PLIC_PRIO0_REG_OFFSET);
  CHECK(prio0 == 0x0u, "Expected PRIO0=0x0 after clearing, got 0x%x", prio0);

  // 2. In rv_plic_reg_top.sv (u_ie0_5), IE0_5 at offset 0x2014 only has 26
  // valid bits [25:0] (sources 160..185, mask 0x03ffffff).
  uint32_t orig_ie5 = mmio_region_read32(plic_base, RV_PLIC_IE0_5_REG_OFFSET);
  mmio_region_write32(plic_base, RV_PLIC_IE0_5_REG_OFFSET, 0xffffffffu);
  uint32_t ie5 = mmio_region_read32(plic_base, RV_PLIC_IE0_5_REG_OFFSET);
  CHECK(ie5 == 0x03ffffffu,
        "Expected IE0_5=0x03ffffff after writing 0xffffffff, got 0x%x", ie5);
  mmio_region_write32(plic_base, RV_PLIC_IE0_5_REG_OFFSET, orig_ie5);
}

static void test_cc0_claim_threshold_and_zero_prio(mmio_region_t plic_base,
                                                   dif_rv_plic_t *plic,
                                                   dif_uart_t *uart) {
  const dif_rv_plic_irq_id_t kIrqId = kTopEarlgreyPlicIrqIdUart0RxParityErr;
  const dif_rv_plic_target_t kTarget = kTopEarlgreyPlicTargetIbex0;

  irq_global_ctrl(false);
  irq_external_ctrl(false);

  // Clean state for UART0 RxParityErr IRQ.
  CHECK_DIF_OK(dif_uart_irq_set_enabled(uart, kDifUartIrqRxParityErr,
                                        kDifToggleDisabled));
  CHECK_DIF_OK(dif_uart_irq_acknowledge(uart, kDifUartIrqRxParityErr));
  CHECK_DIF_OK(
      dif_rv_plic_irq_set_enabled(plic, kIrqId, kTarget, kDifToggleDisabled));

  // Case A: PRIO = 1, THRESHOLD0 = 2 (prio <= threshold).
  // In rv_plic_target.sv:
  //   irq_d    = (max_value > threshold_i) ? max_valid : 1'b0;  -> 0
  //   irq_id_d = (max_valid) ? max_idx : '0;                    -> kIrqId
  // So MIP.MEIP must stay 0, but reading CC0 must still return kIrqId and
  // claim the interrupt (clearing IP).
  CHECK_DIF_OK(dif_rv_plic_irq_set_priority(plic, kIrqId, 1u));
  CHECK_DIF_OK(dif_rv_plic_target_set_threshold(plic, kTarget, 2u));
  CHECK_DIF_OK(
      dif_rv_plic_irq_set_enabled(plic, kIrqId, kTarget, kDifToggleEnabled));

  CHECK_DIF_OK(dif_uart_irq_set_enabled(uart, kDifUartIrqRxParityErr,
                                        kDifToggleEnabled));
  CHECK_DIF_OK(dif_uart_irq_force(uart, kDifUartIrqRxParityErr, true));

  bool is_pending = false;
  CHECK_DIF_OK(dif_rv_plic_irq_is_pending(plic, kIrqId, &is_pending));
  CHECK(is_pending, "Expected IRQ %u to be pending in IP", kIrqId);
  CHECK(!get_mip_meip(), "Expected MIP.MEIP=0 when PRIO(1) <= THRESHOLD0(2)");

  dif_rv_plic_irq_id_t claimed_id = 0;
  CHECK_DIF_OK(dif_rv_plic_irq_claim(plic, kTarget, &claimed_id));
  CHECK(claimed_id == kIrqId,
        "Expected CC0 claim to return %u even when PRIO(1) <= THRESHOLD0(2), "
        "got %u",
        kIrqId, claimed_id);

  // Claiming CC0 must clear IP in rv_plic_gateway.
  CHECK_DIF_OK(dif_rv_plic_irq_is_pending(plic, kIrqId, &is_pending));
  CHECK(!is_pending, "Expected IP bit to clear upon CC0 claim");

  // Clear UART0 interrupt source and complete CC0.
  CHECK_DIF_OK(dif_uart_irq_acknowledge(uart, kDifUartIrqRxParityErr));
  CHECK_DIF_OK(dif_rv_plic_irq_complete(plic, kTarget, claimed_id));

  // Case B: PRIO = 0, THRESHOLD0 = 0 (prio == 0, enabled & pending).
  // prim_max_tree has valid_i = ip_i & ie_i, so max_valid = 1 and max_idx =
  // kIrqId. MIP.MEIP must be 0 (0 > 0 is false), and CC0 must return kIrqId.
  mmio_region_write32(plic_base, RV_PLIC_PRIO0_REG_OFFSET + (kIrqId * 4u), 0u);
  CHECK_DIF_OK(dif_rv_plic_target_set_threshold(plic, kTarget, 0u));
  CHECK_DIF_OK(dif_uart_irq_force(uart, kDifUartIrqRxParityErr, true));

  CHECK_DIF_OK(dif_rv_plic_irq_is_pending(plic, kIrqId, &is_pending));
  CHECK(is_pending, "Expected IRQ %u to be pending in IP", kIrqId);
  CHECK(!get_mip_meip(), "Expected MIP.MEIP=0 when PRIO(0) <= THRESHOLD0(0)");

  claimed_id = 0;
  CHECK_DIF_OK(dif_rv_plic_irq_claim(plic, kTarget, &claimed_id));
  CHECK(claimed_id == kIrqId,
        "Expected CC0 claim to return %u when PRIO=0 and IE=1, got %u", kIrqId,
        claimed_id);

  CHECK_DIF_OK(dif_uart_irq_acknowledge(uart, kDifUartIrqRxParityErr));
  CHECK_DIF_OK(dif_rv_plic_irq_complete(plic, kTarget, claimed_id));

  // Case C: PRIO = 2, THRESHOLD0 = 1 (prio > threshold).
  // MIP.MEIP must be 1 until CC0 is claimed.
  CHECK_DIF_OK(dif_rv_plic_irq_set_priority(plic, kIrqId, 2u));
  CHECK_DIF_OK(dif_rv_plic_target_set_threshold(plic, kTarget, 1u));
  CHECK_DIF_OK(dif_uart_irq_force(uart, kDifUartIrqRxParityErr, true));

  CHECK(get_mip_meip(), "Expected MIP.MEIP=1 when PRIO(2) > THRESHOLD0(1)");
  claimed_id = 0;
  CHECK_DIF_OK(dif_rv_plic_irq_claim(plic, kTarget, &claimed_id));
  CHECK(claimed_id == kIrqId, "Expected CC0 claim to return %u, got %u", kIrqId,
        claimed_id);
  CHECK(!get_mip_meip(), "Expected MIP.MEIP=0 after CC0 claim");

  CHECK_DIF_OK(dif_uart_irq_acknowledge(uart, kDifUartIrqRxParityErr));
  // Case D (Wave 2): CC0 completion ID in rv_plic_reg_top.sv (u_cc0) is 8 bits
  // wide (`assign cc0_wd = reg_wdata[7:0]`). Writing `0xdeadba00u | claimed_id`
  // to CC0 must still complete `claimed_id` in rv_plic_gateway so a subsequent
  // interrupt assertion latches into IP!
  mmio_region_write32(plic_base, RV_PLIC_CC0_REG_OFFSET,
                      0xdeadba00u | (uint32_t)claimed_id);

  CHECK_DIF_OK(dif_uart_irq_force(uart, kDifUartIrqRxParityErr, true));
  is_pending = false;
  CHECK_DIF_OK(dif_rv_plic_irq_is_pending(plic, kIrqId, &is_pending));
  CHECK(is_pending,
        "Expected CC0 write with non-zero upper bits [31:8] (0xdeadba00 | %u) "
        "to truncate to cc0_wd[7:0] and complete gateway, allowing re-pend",
        claimed_id);

  claimed_id = 0;
  CHECK_DIF_OK(dif_rv_plic_irq_claim(plic, kTarget, &claimed_id));
  CHECK(claimed_id == kIrqId, "Expected re-claimed CC0 to return %u, got %u",
        kIrqId, claimed_id);
  CHECK_DIF_OK(dif_uart_irq_acknowledge(uart, kDifUartIrqRxParityErr));
  CHECK_DIF_OK(dif_rv_plic_irq_complete(plic, kTarget, claimed_id));

  // Restore target threshold and disable test IRQ.
  CHECK_DIF_OK(
      dif_rv_plic_irq_set_enabled(plic, kIrqId, kTarget, kDifToggleDisabled));
  CHECK_DIF_OK(dif_rv_plic_target_set_threshold(plic, kTarget, 0u));
  CHECK_DIF_OK(dif_uart_irq_set_enabled(uart, kDifUartIrqRxParityErr,
                                        kDifToggleDisabled));
}

static void test_msip0_and_alert_test(mmio_region_t plic_base,
                                      dif_alert_handler_t *alert_handler) {
  irq_software_ctrl(false);

  // MSIP0 reset value is 0.
  uint32_t msip0 = mmio_region_read32(plic_base, RV_PLIC_MSIP0_REG_OFFSET);
  CHECK(msip0 == 0u, "Expected MSIP0=0 at start, got 0x%x", msip0);
  CHECK(!get_mip_msip(), "Expected MIP.MSIP=0 when MSIP0=0");

  // Write 0xffffffff to MSIP0 -> 1-bit RW register (mask 0x1), asserts
  // MIP.MSIP.
  mmio_region_write32(plic_base, RV_PLIC_MSIP0_REG_OFFSET, 0xffffffffu);
  msip0 = mmio_region_read32(plic_base, RV_PLIC_MSIP0_REG_OFFSET);
  CHECK(msip0 == 1u, "Expected MSIP0=1 after writing 0xffffffff, got 0x%x",
        msip0);
  CHECK(get_mip_msip(), "Expected MIP.MSIP=1 when MSIP0=1");

  // Clear MSIP0 -> deasserts MIP.MSIP.
  mmio_region_write32(plic_base, RV_PLIC_MSIP0_REG_OFFSET, 0u);
  msip0 = mmio_region_read32(plic_base, RV_PLIC_MSIP0_REG_OFFSET);
  CHECK(msip0 == 0u, "Expected MSIP0=0 after writing 0, got 0x%x", msip0);
  CHECK(!get_mip_msip(), "Expected MIP.MSIP=0 after clearing MSIP0");

  // Write 1 to ALERT_TEST -> must pulse alert 41
  // (kTopEarlgreyAlertIdRvPlicFatalFault) and allow W1C acknowledgement in
  // alert_handler.
  mmio_region_write32(plic_base, RV_PLIC_ALERT_TEST_REG_OFFSET, 1u);

  bool is_cause = false;
  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
      alert_handler, kTopEarlgreyAlertIdRvPlicFatalFault, &is_cause));
  CHECK(is_cause, "Expected RV_PLIC fatal_fault alert_cause to be set");

  CHECK_DIF_OK(dif_alert_handler_alert_acknowledge(
      alert_handler, kTopEarlgreyAlertIdRvPlicFatalFault));
  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
      alert_handler, kTopEarlgreyAlertIdRvPlicFatalFault, &is_cause));
  CHECK(!is_cause,
        "Expected RV_PLIC fatal_fault alert_cause to clear after W1C ack "
        "(ALERT_TEST must pulse, not latch high)");
}

static void test_wave2_unmapped_and_subword_access(mmio_region_t plic_base) {
  // 1. Unmapped offsets in rv_plic_reg_top.sv set `addrmiss = 1 -> reg_error =
  // 1`, returning TL-UL `d_error = 1` (Load Access Fault mcause=5 / Store
  // Access Fault mcause=7).
  const uint32_t kUnmappedPrio186 =
      RV_PLIC_PRIO185_REG_OFFSET + 4u;                             // 0x0002e8
  const uint32_t kUnmappedIp6 = RV_PLIC_IP_5_REG_OFFSET + 4u;      // 0x001018
  const uint32_t kUnmappedIe6 = RV_PLIC_IE0_5_REG_OFFSET + 4u;     // 0x002018
  const uint32_t kUnmappedAfterCc0 = RV_PLIC_CC0_REG_OFFSET + 4u;  // 0x200008
  const uint32_t kUnmappedAfterMsip0 =
      RV_PLIC_MSIP0_REG_OFFSET + 4u;  // 0x4000004

  CHECK(expect_load_fault_32(plic_base, kUnmappedPrio186),
        "Expected Load Access Fault (mcause=5) reading unmapped PRIO186 (0x%x)",
        kUnmappedPrio186);
  CHECK(
      expect_store_fault_32(plic_base, kUnmappedPrio186, 1u),
      "Expected Store Access Fault (mcause=7) writing unmapped PRIO186 (0x%x)",
      kUnmappedPrio186);
  CHECK(expect_load_fault_32(plic_base, kUnmappedIp6),
        "Expected Load Access Fault (mcause=5) reading unmapped IP_6 (0x%x)",
        kUnmappedIp6);
  CHECK(expect_load_fault_32(plic_base, kUnmappedIe6),
        "Expected Load Access Fault (mcause=5) reading unmapped IE0_6 (0x%x)",
        kUnmappedIe6);
  CHECK(expect_store_fault_32(plic_base, kUnmappedIe6, 1u),
        "Expected Store Access Fault (mcause=7) writing unmapped IE0_6 (0x%x)",
        kUnmappedIe6);
  CHECK(expect_load_fault_32(plic_base, kUnmappedAfterCc0),
        "Expected Load Access Fault (mcause=5) reading unmapped offset 0x%x",
        kUnmappedAfterCc0);
  CHECK(expect_load_fault_32(plic_base, kUnmappedAfterMsip0),
        "Expected Load Access Fault (mcause=5) reading unmapped offset 0x%x",
        kUnmappedAfterMsip0);
  CHECK(expect_store_fault_32(plic_base, kUnmappedAfterMsip0, 1u),
        "Expected Store Access Fault (mcause=7) writing unmapped offset 0x%x",
        kUnmappedAfterMsip0);

  // 2. Sub-word access permissions in rv_plic_reg_pkg.sv (`RV_PLIC_PERMIT`):
  // - Sub-word reads (`lbu`/`lhu`) to mapped registers always succeed (`wr_err
  // = 0`).
  // - `PRIO1`, `THRESHOLD0`, `CC0`, `MSIP0` have `RV_PLIC_PERMIT = 4'b0001`:
  //   an 8-bit store (`sb`) to byte 0 (`+0`) has `reg_be = 4'b0001` (`wr_err =
  //   0`) and succeeds, whereas an 8-bit store (`sb`) to byte 1 (`+1`) has
  //   `reg_be = 4'b0010` (`|(4'b0001 & ~4'b0010) == 1 -> wr_err = 1`) and
  //   raises a synchronous Store Access Fault (`mcause = 7`) without modifying
  //   the register!
  uint32_t before = fault_count;
  mmio_region_write8(plic_base, RV_PLIC_PRIO1_REG_OFFSET, 0x2u);
  uint8_t prio1_b0 = mmio_region_read8(plic_base, RV_PLIC_PRIO1_REG_OFFSET);
  CHECK(fault_count == before,
        "Expected byte write/read at PRIO1+0 (PERMIT=4'b0001) to succeed "
        "without fault");
  CHECK(prio1_b0 == 0x2u, "Expected PRIO1 byte 0 to read back 0x2, got 0x%x",
        prio1_b0);

  CHECK(expect_store_fault_8(plic_base, RV_PLIC_PRIO1_REG_OFFSET + 1u, 0x3u),
        "Expected Store Access Fault (mcause=7) on byte write to PRIO1+1");
  CHECK(mmio_region_read32(plic_base, RV_PLIC_PRIO1_REG_OFFSET) == 0x2u,
        "Expected PRIO1 to remain 0x2 after rejected byte write to PRIO1+1");
  mmio_region_write32(plic_base, RV_PLIC_PRIO1_REG_OFFSET, 0u);

  // Check MSIP0 byte 0 write (`+0` succeeds) vs byte 1 write (`+1` faults and
  // preserves MSIP0=1).
  before = fault_count;
  mmio_region_write8(plic_base, RV_PLIC_MSIP0_REG_OFFSET, 1u);
  CHECK(fault_count == before &&
            mmio_region_read8(plic_base, RV_PLIC_MSIP0_REG_OFFSET) == 1u,
        "Expected byte write/read at MSIP0+0 (PERMIT=4'b0001) to succeed");
  CHECK(expect_store_fault_8(plic_base, RV_PLIC_MSIP0_REG_OFFSET + 1u, 0u),
        "Expected Store Access Fault (mcause=7) on byte write to MSIP0+1");
  CHECK(mmio_region_read32(plic_base, RV_PLIC_MSIP0_REG_OFFSET) == 1u,
        "Expected MSIP0 to remain 1 after rejected byte write to MSIP0+1");
  mmio_region_write32(plic_base, RV_PLIC_MSIP0_REG_OFFSET, 0u);

  // - `IE0_0..IE0_5` and `IP_0..IP_5` have `RV_PLIC_PERMIT = 4'b1111`:
  //   any sub-word write (`sb` even at byte 0) sets `wr_err = 1` and raises
  //   Store Access Fault (`mcause = 7`) while preserving register state.
  CHECK(expect_store_fault_8(plic_base, RV_PLIC_IP_0_REG_OFFSET, 0u),
        "Expected Store Access Fault (mcause=7) on byte write to IP_0+0 "
        "(PERMIT=4'b1111)");
  mmio_region_write32(plic_base, RV_PLIC_IE0_0_REG_OFFSET, 0x12345678u);
  CHECK(expect_store_fault_8(plic_base, RV_PLIC_IE0_0_REG_OFFSET, 0xffu),
        "Expected Store Access Fault (mcause=7) on byte write to IE0_0+0 "
        "(PERMIT=4'b1111)");
  CHECK(mmio_region_read32(plic_base, RV_PLIC_IE0_0_REG_OFFSET) == 0x12345678u,
        "Expected IE0_0 to remain 0x12345678 after rejected byte write");
  mmio_region_write32(plic_base, RV_PLIC_IE0_0_REG_OFFSET, 0u);

  // - `THRESHOLD0` and `CC0` also have `RV_PLIC_PERMIT = 4'b0001`:
  //   byte writes to offset +1 must fault with Store Access Fault (`mcause =
  //   7`).
  CHECK(expect_store_fault_8(plic_base, RV_PLIC_THRESHOLD0_REG_OFFSET + 1u, 0u),
        "Expected Store Access Fault (mcause=7) on byte write to THRESHOLD0+1");
  CHECK(expect_store_fault_8(plic_base, RV_PLIC_CC0_REG_OFFSET + 1u, 0u),
        "Expected Store Access Fault (mcause=7) on byte write to CC0+1");
}

bool test_main(void) {
  mmio_region_t plic_base =
      mmio_region_from_addr(TOP_EARLGREY_RV_PLIC_BASE_ADDR);
  dif_rv_plic_t plic;
  CHECK_DIF_OK(dif_rv_plic_init(plic_base, &plic));

  dif_uart_t uart;
  CHECK_DIF_OK(dif_uart_init(
      mmio_region_from_addr(TOP_EARLGREY_UART0_BASE_ADDR), &uart));

  dif_alert_handler_t alert_handler;
  CHECK_DIF_OK(dif_alert_handler_init(
      mmio_region_from_addr(TOP_EARLGREY_ALERT_HANDLER_BASE_ADDR),
      &alert_handler));

  test_prio0_and_ie_last_word_masks(plic_base);
  test_cc0_claim_threshold_and_zero_prio(plic_base, &plic, &uart);
  test_msip0_and_alert_test(plic_base, &alert_handler);
  test_wave2_unmapped_and_subword_access(plic_base);

  LOG_INFO("rv_plic_consistency_test passed!");
  return true;
}
