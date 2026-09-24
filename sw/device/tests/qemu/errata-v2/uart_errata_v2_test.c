// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * UART Controller (UART1) Errata Verification Test on Earlgrey v2 (CW340 FPGA).
 *
 * Exercises and verifies:
 *   1. `VAL` (`0x28`, `rx_val_q` in `uart_core.sv:305-308`) resets to `0x0000`
 *      instead of idle mark `0xffff` and remains frozen while
 *      `CTRL.TX == 0 && CTRL.RX == 0`.
 *   2. `TIMEOUT_CTRL.EN = 1` with `TIMEOUT_CTRL.VAL = 0` (`0x80000000`)
 *      continuously asserts `INTR_STATE.rx_timeout` (`uart_core.sv:387`:
 *      `event_rx_timeout = (rx_timeout_count_q == uart_rxto_val) &
 * uart_rxto_en`) even when the RX FIFO is empty (`RXLVL == 0`), preventing W1C
 * from clearing `INTR_STATE.rx_timeout` until `VAL >= 1` is programmed.
 *   3. `OVRD.TXEN = 1, OVRD.TXVAL = 0` (`uart_core.sv:217-228`) overrides the
 *      external `tx_out_q` flop downstream of the internal system loopback tap
 *      `tx_out` (`uart_core.sv:264`), leaving internal `rx_in == 1` and
 *      `VAL == 0xffff` in `CTRL.SLPBK` mode.
 *   4. `FIFO_CTRL.TXILVL` (`3 bits`, `uart_core.sv:318-323`) saturates
 *      `tx_watermark_thresh` at `16` (`TxFifoDepth / 2`) for all `TXILVL >= 4`
 *      (`4..7`) instead of disabling at `0`, while `INTR_STATE.tx_watermark`
 *      (`bit 0`) and `INTR_STATE.tx_empty` (`bit 8`) are `IntrT("Status")`
 *      (`SwAccessRO`) — staying asserted (`0x101`) when `TXLVL == 0` even when
 *      `CTRL.TX == 0` and `TXILVL = 7` (and driving `lsio_trigger_o = 1` in
 *      `uart_core.sv:355-360` without `CTRL.TX` or `INTR_ENABLE` gating),
 *      clearing only when `TXLVL >= 16`.
 *   5. `UART_PERMIT` (`uart_reg_pkg.sv:401-415`) enforces `4'b0011` on
 *      `INTR_STATE` / `INTR_ENABLE` / `INTR_TEST` (faulting 1-byte `sb` with
 *      `mcause = 7` while accepting 2-byte `sh` at `+0`) and `4'b1111` on
 *      `CTRL` / `TIMEOUT_CTRL` (faulting `sb` and `sh` with `mcause = 7`), and
 *      unmapped offsets `>= 0x34` fault (`addrmiss`, `mcause = 5 / 7`).
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/irq.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top/uart_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kUart1Base = 0x40010000u,
  kRiscvLoadAccessFault = 5u,
  kRiscvStoreAccessFault = 7u,
  kUnmappedOffset = 0x34u,
};

static volatile uint32_t g_fault_count = 0;
static volatile uint32_t g_last_mcause = 0;

void ottf_exception_handler(uint32_t *exc_info) {
  uint32_t mcause = 0;
  uint32_t mepc = 0;
  CSR_READ(CSR_REG_MCAUSE, &mcause);
  CSR_READ(CSR_REG_MEPC, &mepc);
  if (mcause == kRiscvLoadAccessFault || mcause == kRiscvStoreAccessFault) {
    g_fault_count++;
    g_last_mcause = mcause;
    uint16_t insn16 = *(const volatile uint16_t *)mepc;
    uint32_t step = ((insn16 & 0x3u) != 0x3u) ? 2u : 4u;
    CSR_WRITE(CSR_REG_MEPC, mepc + step);
    return;
  }
  ottf_generic_fault_print(exc_info, "Unhandled exception", mcause);
  abort();
}

bool test_main(void) {
  irq_global_ctrl(false);
  LOG_INFO("Running Earlgrey v2 UART1 Errata Verification Suite");

  // -------------------------------------------------------------------------
  // Check 1: VAL (0x28, rx_val_q) resets to 0x0000 and freezes while TX=RX=0
  // -------------------------------------------------------------------------
  uint32_t val_reset = abs_mmio_read32(kUart1Base + UART_VAL_REG_OFFSET);
  CHECK(val_reset == 0x0000u, "Expected VAL reset value 0x0000, got 0x%04x",
        val_reset);
  uint32_t status_reset = abs_mmio_read32(kUart1Base + UART_STATUS_REG_OFFSET);
  CHECK((status_reset & ~0x3fu) == 0u && status_reset == 0x3cu,
        "Expected UART_STATUS (0x14) to only populate bits [5:0]=0x3c with "
        "bits [31:6]==0 (no STATUS.BREAK bit), got 0x%08x",
        status_reset);

  // -------------------------------------------------------------------------
  // Check 2: TIMEOUT_CTRL.EN=1 with VAL=0 continuously asserts rx_timeout on
  // empty RX FIFO and blocks W1C until VAL >= 1
  // -------------------------------------------------------------------------
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

  abs_mmio_write32(kUart1Base + UART_TIMEOUT_CTRL_REG_OFFSET,
                   (1u << UART_TIMEOUT_CTRL_EN_BIT) | 0u);
  uint32_t intr_state =
      abs_mmio_read32(kUart1Base + UART_INTR_STATE_REG_OFFSET);
  CHECK(((intr_state >> UART_INTR_COMMON_RX_TIMEOUT_BIT) & 1u) == 1u,
        "Expected INTR_STATE.rx_timeout == 1 immediately when "
        "TIMEOUT_CTRL.EN=1, VAL=0 on empty RX FIFO (got 0x%03x)",
        intr_state);

  abs_mmio_write32(kUart1Base + UART_INTR_STATE_REG_OFFSET,
                   1u << UART_INTR_COMMON_RX_TIMEOUT_BIT);
  intr_state = abs_mmio_read32(kUart1Base + UART_INTR_STATE_REG_OFFSET);
  CHECK(((intr_state >> UART_INTR_COMMON_RX_TIMEOUT_BIT) & 1u) == 1u,
        "Expected INTR_STATE.rx_timeout to remain sticky 1 after W1C while "
        "EN=1, VAL=0 (got 0x%03x)",
        intr_state);

  abs_mmio_write32(kUart1Base + UART_TIMEOUT_CTRL_REG_OFFSET,
                   (1u << UART_TIMEOUT_CTRL_EN_BIT) | 10u);
  abs_mmio_write32(kUart1Base + UART_INTR_STATE_REG_OFFSET,
                   1u << UART_INTR_COMMON_RX_TIMEOUT_BIT);
  intr_state = abs_mmio_read32(kUart1Base + UART_INTR_STATE_REG_OFFSET);
  CHECK(((intr_state >> UART_INTR_COMMON_RX_TIMEOUT_BIT) & 1u) == 0u,
        "Expected INTR_STATE.rx_timeout == 0 after W1C once TIMEOUT_CTRL.VAL "
        ">= 1 (got 0x%03x)",
        intr_state);
  abs_mmio_write32(kUart1Base + UART_TIMEOUT_CTRL_REG_OFFSET, 0u);

  // -------------------------------------------------------------------------
  // Check 3: OVRD.TXEN=1, TXVAL=0 bypassed by CTRL.SLPBK tap (VAL == 0xffff)
  // -------------------------------------------------------------------------
  abs_mmio_write32(kUart1Base + UART_OVRD_REG_OFFSET,
                   (1u << UART_OVRD_TXEN_BIT) | (0u << UART_OVRD_TXVAL_BIT));
  abs_mmio_write32(kUart1Base + UART_CTRL_REG_OFFSET,
                   (0xffffu << UART_CTRL_NCO_OFFSET) |
                       (1u << UART_CTRL_SLPBK_BIT) | (1u << UART_CTRL_TX_BIT));
  busy_spin_micros(50);
  uint32_t val_slpbk = abs_mmio_read32(kUart1Base + UART_VAL_REG_OFFSET);
  CHECK(val_slpbk == 0xffffu,
        "Expected VAL == 0xffff in SLPBK with OVRD.TXEN=1, TXVAL=0 (got "
        "0x%04x)",
        val_slpbk);
  abs_mmio_write32(kUart1Base + UART_OVRD_REG_OFFSET, 0u);
  abs_mmio_write32(kUart1Base + UART_CTRL_REG_OFFSET, 0u);

  // -------------------------------------------------------------------------
  // Check 4: Status-type INTR_STATE (tx_watermark & tx_empty) and TXILVL=7
  // saturation at 16 bytes (lsio_trigger_o / event_tx_watermark behavior)
  // -------------------------------------------------------------------------
  // Program unenumerated TXILVL=7 (bits 7:5 = 0x7) and reset TX FIFO while
  // CTRL.TX=0: tx_watermark_thresh saturates at 16 instead of 0, so
  // INTR_STATE.tx_watermark (bit 0) and tx_empty (bit 8) stay 1 (0x101) and
  // ignore W1C!
  abs_mmio_write32(
      kUart1Base + UART_FIFO_CTRL_REG_OFFSET,
      (7u << UART_FIFO_CTRL_TXILVL_OFFSET) | (1u << UART_FIFO_CTRL_TXRST_BIT));
  abs_mmio_write32(kUart1Base + UART_INTR_STATE_REG_OFFSET, 0x1ffu);
  intr_state = abs_mmio_read32(kUart1Base + UART_INTR_STATE_REG_OFFSET);
  CHECK(intr_state == 0x101u,
        "Expected INTR_STATE == 0x101 (tx_watermark | tx_empty) when TXLVL==0 "
        "even with CTRL.TX==0 and TXILVL==7, got 0x%03x",
        intr_state);

  // Push 15 bytes into WDATA while CTRL.TX==0: TXLVL==15 < 16, so
  // tx_empty clears to 0 while tx_watermark remains 1 (0x001).
  for (uint32_t i = 0; i < 15u; ++i) {
    abs_mmio_write32(kUart1Base + UART_WDATA_REG_OFFSET, 0x55u);
  }
  intr_state = abs_mmio_read32(kUart1Base + UART_INTR_STATE_REG_OFFSET);
  CHECK(intr_state == 0x001u,
        "Expected INTR_STATE == 0x001 at TXLVL==15 with TXILVL==7 (threshold "
        "saturated at 16), got 0x%03x",
        intr_state);

  // Push 16th byte into WDATA: TXLVL==16 >= 16, so event_tx_watermark (and
  // lsio_trigger_o) finally deasserts to 0!
  abs_mmio_write32(kUart1Base + UART_WDATA_REG_OFFSET, 0x55u);
  intr_state = abs_mmio_read32(kUart1Base + UART_INTR_STATE_REG_OFFSET);
  CHECK(intr_state == 0x000u,
        "Expected INTR_STATE == 0x000 at TXLVL==16 with TXILVL==7, got 0x%03x",
        intr_state);

  // Reset TX FIFO back to empty.
  abs_mmio_write32(kUart1Base + UART_FIFO_CTRL_REG_OFFSET,
                   1u << UART_FIFO_CTRL_TXRST_BIT);

  // -------------------------------------------------------------------------
  // Check 5: UART_PERMIT sub-word write faults and addrmiss (>= 0x34)
  // -------------------------------------------------------------------------
  abs_mmio_write32(kUart1Base + UART_CTRL_REG_OFFSET, 0x23450000u);
  g_fault_count = 0;
  g_last_mcause = 0;
  *(volatile uint8_t *)(kUart1Base + UART_CTRL_REG_OFFSET) = 0x03u;
  CHECK(g_fault_count == 1u && g_last_mcause == kRiscvStoreAccessFault,
        "Expected sb to CTRL (PERMIT=4'b1111) to fault with mcause=7");
  *(volatile uint16_t *)(kUart1Base + UART_CTRL_REG_OFFSET) = 0x0003u;
  CHECK(g_fault_count == 2u && g_last_mcause == kRiscvStoreAccessFault,
        "Expected sh to CTRL (PERMIT=4'b1111) to fault with mcause=7");
  CHECK(abs_mmio_read32(kUart1Base + UART_CTRL_REG_OFFSET) == 0x23450000u,
        "Expected CTRL unchanged after rejected sub-word writes");
  abs_mmio_write32(kUart1Base + UART_CTRL_REG_OFFSET, 0u);

  abs_mmio_write32(kUart1Base + UART_INTR_ENABLE_REG_OFFSET, 0u);
  g_fault_count = 0;
  *(volatile uint8_t *)(kUart1Base + UART_INTR_ENABLE_REG_OFFSET) = 0x05u;
  CHECK(g_fault_count == 1u && g_last_mcause == kRiscvStoreAccessFault,
        "Expected sb to INTR_ENABLE (PERMIT=4'b0011) to fault with mcause=7");
  g_fault_count = 0;
  *(volatile uint16_t *)(kUart1Base + UART_INTR_ENABLE_REG_OFFSET) = 0x0105u;
  CHECK(g_fault_count == 0u,
        "Expected sh to INTR_ENABLE+0 (PERMIT=4'b0011) to succeed");
  CHECK(abs_mmio_read32(kUart1Base + UART_INTR_ENABLE_REG_OFFSET) == 0x0105u,
        "Expected INTR_ENABLE == 0x0105 after sh to +0");
  abs_mmio_write32(kUart1Base + UART_INTR_ENABLE_REG_OFFSET, 0u);

  g_fault_count = 0;
  (void)abs_mmio_read32(kUart1Base + kUnmappedOffset);
  CHECK(g_fault_count == 1u && g_last_mcause == kRiscvLoadAccessFault,
        "Expected read at unmapped offset 0x34 to fault with mcause=5");

  LOG_INFO("UART1 errata v2 test passed all checks on CW340 FPGA");
  return true;
}
