// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file uart_errata_test.c
 * @brief CW340 FPGA & QEMU Errata Confirmation Test for `uart` (`P10`).
 *
 * Empirically confirms on physical CW340 FPGA silicon and QEMU:
 * 1. [uart_core.sv:358-376] (TRUE_SILICON_ERRATA, MEDIUM):
 *    Setting `TIMEOUT_CTRL.EN = 1` with `TIMEOUT_CTRL.VAL = 0` (`0x80000000`)
 *    in `uart_core.sv:358-376` continuously asserts `INTR_STATE.rx_timeout`
 *    even when the RX FIFO is empty (`RXLVL == 0`) and `CTRL.RX == 0`, making
 *    `INTR_STATE.rx_timeout` unclearable via W1C until `EN` is cleared or
 *    `VAL >= 1` is programmed.
 * 2. [uart_core.sv:213-224] (SPEC_DOC_ERRATA, LOW):
 *    In `uart_core.sv:213-224, 260`, `OVRD.TXEN = 1, OVRD.TXVAL = 0` overrides
 *    the external pad flop `tx_out_q`, whereas `CTRL.SLPBK` (`sys_loopback`)
 *    taps `tx_out` upstream of `tx_out_q`, so `VAL` remains `0xffff`.
 * 3. [uart_core.sv:165-173] (BENIGN_RTL_IMPL_DETAIL, LOW):
 *    `VAL` (`rx_val_q`, `uart_core.sv:301-304`) resets to `0x0000` before
 *    `CTRL.TX` or `CTRL.RX` is enabled with `CTRL.NCO != 0`.
 * 4. [uart_reg_pkg.sv:397-411] (INTENDED_SECURITY_HARDENING, INFO — SEC_CM:
 * BUS.INTEGRITY): `UART_PERMIT` (`uart_reg_pkg.sv:397-411`) rejects 1-byte `sb`
 * writes to `INTR_ENABLE` (`4'b0011`, `mcause = 7`) while allowing 2-byte `sh`
 * writes at `+0`, and rejects both `sb` and `sh` writes to `CTRL` and
 *    `TIMEOUT_CTRL` (`4'b1111`, `mcause = 7`).
 */

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/irq.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "uart_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kUart1Base = TOP_EARLGREY_UART1_BASE_ADDR,
};

static volatile uint32_t g_fault_count = 0;
static volatile uint32_t g_last_mcause = 0;

void ottf_exception_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = 0;
  uint32_t mepc = 0;
  asm volatile("csrr %0, mcause" : "=r"(mcause));
  asm volatile("csrr %0, mepc" : "=r"(mepc));
  g_last_mcause = mcause;
  g_fault_count++;
  uint16_t insn16 = *(const uint16_t *)mepc;
  mepc += ((insn16 & 0x3u) != 0x3u) ? 2u : 4u;
  asm volatile("csrw mepc, %0" : : "r"(mepc));
}

bool test_main(void) {
  irq_global_ctrl(false);
  LOG_INFO("=== OpenTitan Earlgrey UART Errata Confirmation Suite (P10) ===");

  // ---------------------------------------------------------------------------
  // 1. [uart_core.sv:165-173] (BENIGN_RTL_IMPL_DETAIL):
  //    VAL (0x28, rx_val_q) resets to 0x0000 before CTRL.TX/RX is enabled.
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [uart_core.sv:165-173] (BENIGN_RTL_IMPL_DETAIL): VAL "
      "reset=0x0000");
  uint32_t val_reset = abs_mmio_read32(kUart1Base + UART_VAL_REG_OFFSET);
  CHECK(val_reset == 0x0000u,
        "[uart_core.sv:165-173] Expected VAL reset value 0x0000, got 0x%04x",
        val_reset);

  // ---------------------------------------------------------------------------
  // 2. [uart_core.sv:358-376] (TRUE_SILICON_ERRATA, MEDIUM):
  //    TIMEOUT_CTRL.EN=1 with VAL=0 (0x80000000) continuously asserts
  //    INTR_STATE.rx_timeout even when RX FIFO is empty (RXLVL == 0) and
  //    CTRL.RX == 0, and prevents W1C from clearing INTR_STATE.rx_timeout!
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [uart_core.sv:358-376] (TRUE_SILICON_ERRATA): TIMEOUT_CTRL "
      "EN=1, "
      "VAL=0 sticky rx_timeout on empty RX FIFO");
  abs_mmio_write32(kUart1Base + UART_TIMEOUT_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kUart1Base + UART_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(
      kUart1Base + UART_FIFO_CTRL_REG_OFFSET,
      (1u << UART_FIFO_CTRL_RXRST_BIT) | (1u << UART_FIFO_CTRL_TXRST_BIT));
  abs_mmio_write32(kUart1Base + UART_INTR_STATE_REG_OFFSET, 0x1ffu);

  uint32_t fifo_status =
      abs_mmio_read32(kUart1Base + UART_FIFO_STATUS_REG_OFFSET);
  CHECK(((fifo_status >> UART_FIFO_STATUS_RXLVL_OFFSET) & 0xffu) == 0u,
        "Expected empty RX FIFO (RXLVL == 0)");
  CHECK((abs_mmio_read32(kUart1Base + UART_INTR_STATE_REG_OFFSET) &
         (1u << UART_INTR_COMMON_RX_TIMEOUT_BIT)) == 0u,
        "Expected INTR_STATE.rx_timeout == 0 before enabling TIMEOUT_CTRL");

  // Enable TIMEOUT_CTRL with EN=1, VAL=0 while RX FIFO is empty.
  abs_mmio_write32(kUart1Base + UART_TIMEOUT_CTRL_REG_OFFSET,
                   (1u << UART_TIMEOUT_CTRL_EN_BIT) | 0u);
  uint32_t intr_state =
      abs_mmio_read32(kUart1Base + UART_INTR_STATE_REG_OFFSET);
  CHECK(
      ((intr_state >> UART_INTR_COMMON_RX_TIMEOUT_BIT) & 1u) == 1u,
      "[uart_core.sv:358-376] Expected INTR_STATE.rx_timeout == 1 immediately "
      "when TIMEOUT_CTRL.EN=1, VAL=0 on empty RX FIFO (got 0x%03x)",
      intr_state);

  // Attempt W1C clear of INTR_STATE.rx_timeout while EN=1, VAL=0 remains
  // active: Because event_rx_timeout is 1 on every cycle, INTR_STATE.rx_timeout
  // stays 1!
  abs_mmio_write32(kUart1Base + UART_INTR_STATE_REG_OFFSET,
                   1u << UART_INTR_COMMON_RX_TIMEOUT_BIT);
  intr_state = abs_mmio_read32(kUart1Base + UART_INTR_STATE_REG_OFFSET);
  CHECK(((intr_state >> UART_INTR_COMMON_RX_TIMEOUT_BIT) & 1u) == 1u,
        "[uart_core.sv:358-376] Expected INTR_STATE.rx_timeout to remain "
        "sticky 1 "
        "after W1C while EN=1, VAL=0 (got 0x%03x)",
        intr_state);

  // Programming VAL >= 1 (e.g., VAL=10) while RX FIFO is empty stops
  // event_rx_timeout from firing, allowing W1C to clear INTR_STATE.rx_timeout.
  abs_mmio_write32(kUart1Base + UART_TIMEOUT_CTRL_REG_OFFSET,
                   (1u << UART_TIMEOUT_CTRL_EN_BIT) | 10u);
  abs_mmio_write32(kUart1Base + UART_INTR_STATE_REG_OFFSET,
                   1u << UART_INTR_COMMON_RX_TIMEOUT_BIT);
  intr_state = abs_mmio_read32(kUart1Base + UART_INTR_STATE_REG_OFFSET);
  CHECK(((intr_state >> UART_INTR_COMMON_RX_TIMEOUT_BIT) & 1u) == 0u,
        "[uart_core.sv:358-376] Expected INTR_STATE.rx_timeout == 0 after W1C "
        "once "
        "TIMEOUT_CTRL.VAL >= 1 (got 0x%03x)",
        intr_state);
  abs_mmio_write32(kUart1Base + UART_TIMEOUT_CTRL_REG_OFFSET, 0u);
  LOG_INFO(
      "[uart_core.sv:358-376] CONFIRMED: EN=1, VAL=0 sticky rx_timeout=1, "
      "cleared=0 "
      "with VAL=10");

  // ---------------------------------------------------------------------------
  // 3. [uart_core.sv:213-224] (SPEC_DOC_ERRATA, LOW):
  //    OVRD.TXEN=1, OVRD.TXVAL=0 overrides external tx_out_q downstream of
  //    CTRL.SLPBK tap tx_out, leaving rx_in=1 and VAL=0xffff in SLPBK.
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [uart_core.sv:213-224] (SPEC_DOC_ERRATA): OVRD.TXEN=1, "
      "TXVAL=0 "
      "bypassed by CTRL.SLPBK");
  abs_mmio_write32(kUart1Base + UART_OVRD_REG_OFFSET,
                   (1u << UART_OVRD_TXEN_BIT) | (0u << UART_OVRD_TXVAL_BIT));
  abs_mmio_write32(kUart1Base + UART_CTRL_REG_OFFSET,
                   (0xffffu << UART_CTRL_NCO_OFFSET) |
                       (1u << UART_CTRL_SLPBK_BIT) | (1u << UART_CTRL_TX_BIT));
  busy_spin_micros(25);
  uint32_t val_slpbk = abs_mmio_read32(kUart1Base + UART_VAL_REG_OFFSET);
  CHECK(val_slpbk == 0xffffu,
        "[uart_core.sv:213-224] Expected VAL == 0xffff in SLPBK with "
        "OVRD.TXEN=1, "
        "TXVAL=0 (got 0x%04x)",
        val_slpbk);
  abs_mmio_write32(kUart1Base + UART_OVRD_REG_OFFSET, 0u);
  abs_mmio_write32(kUart1Base + UART_CTRL_REG_OFFSET, 0u);
  LOG_INFO(
      "[uart_core.sv:213-224] CONFIRMED: VAL=0x%04x in SLPBK despite "
      "OVRD.TXEN=1, "
      "TXVAL=0",
      val_slpbk);

  // ---------------------------------------------------------------------------
  // 4. [uart_reg_pkg.sv:397-411] (INTENDED_SECURITY_HARDENING, INFO — SEC_CM:
  // BUS.INTEGRITY):
  //    UART_PERMIT enforces 4'b0011 on INTR_ENABLE (rejecting sb with mcause=7
  //    while accepting sh at +0) and 4'b1111 on CTRL and TIMEOUT_CTRL
  //    (rejecting both sb and sh with mcause=7).
  // ---------------------------------------------------------------------------
  LOG_INFO(
      "Verifying [uart_reg_pkg.sv:397-411] (INTENDED_SECURITY_HARDENING): "
      "UART_PERMIT "
      "sub-word write protection");
  abs_mmio_write32(kUart1Base + UART_CTRL_REG_OFFSET, 0x23450000u);
  g_fault_count = 0;
  g_last_mcause = 0;
  *(volatile uint8_t *)(kUart1Base + UART_CTRL_REG_OFFSET) = 0x03u;
  CHECK(g_fault_count == 1u && g_last_mcause == 7u,
        "[uart_reg_pkg.sv:397-411] Expected sb to CTRL to fault with mcause=7");
  *(volatile uint16_t *)(kUart1Base + UART_CTRL_REG_OFFSET) = 0x0003u;
  CHECK(g_fault_count == 2u && g_last_mcause == 7u,
        "[uart_reg_pkg.sv:397-411] Expected sh to CTRL to fault with mcause=7");
  CHECK(abs_mmio_read32(kUart1Base + UART_CTRL_REG_OFFSET) == 0x23450000u,
        "[uart_reg_pkg.sv:397-411] Expected CTRL unchanged after rejected "
        "sub-word "
        "writes");
  abs_mmio_write32(kUart1Base + UART_CTRL_REG_OFFSET, 0u);

  abs_mmio_write32(kUart1Base + UART_INTR_ENABLE_REG_OFFSET, 0u);
  g_fault_count = 0;
  *(volatile uint8_t *)(kUart1Base + UART_INTR_ENABLE_REG_OFFSET) = 0x05u;
  CHECK(g_fault_count == 1u && g_last_mcause == 7u,
        "[uart_reg_pkg.sv:397-411] Expected sb to INTR_ENABLE to fault with "
        "mcause=7");
  g_fault_count = 0;
  *(volatile uint16_t *)(kUart1Base + UART_INTR_ENABLE_REG_OFFSET) = 0x0105u;
  CHECK(g_fault_count == 0u,
        "[uart_reg_pkg.sv:397-411] Expected sh to INTR_ENABLE+0 "
        "(PERMIT=4'b0011) to "
        "succeed");
  CHECK(abs_mmio_read32(kUart1Base + UART_INTR_ENABLE_REG_OFFSET) == 0x0105u,
        "[uart_reg_pkg.sv:397-411] Expected INTR_ENABLE == 0x0105 after sh to "
        "+0");
  abs_mmio_write32(kUart1Base + UART_INTR_ENABLE_REG_OFFSET, 0u);
  LOG_INFO(
      "[uart_reg_pkg.sv:397-411] CONFIRMED: UART_PERMIT 4'b1111 (CTRL) & "
      "4'b0011 "
      "(INTR_ENABLE) enforced");

  LOG_INFO("=== ALL UART ERRATA CHECKS PASSED ===");
  return true;
}
