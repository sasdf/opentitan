// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_alert_handler.h"
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
  kAlertHandlerBase = TOP_EARLGREY_ALERT_HANDLER_BASE_ADDR,
};

static int g_failures = 0;

#define EXPECT_EQ(actual, expected, msg, ...)                         \
  do {                                                                \
    uint32_t _act = (uint32_t)(actual);                               \
    uint32_t _exp = (uint32_t)(expected);                             \
    if (_act != _exp) {                                               \
      LOG_ERROR("MISMATCH: " msg " (actual=0x%08x, expected=0x%08x)", \
                ##__VA_ARGS__, _act, _exp);                           \
      g_failures++;                                                   \
    } else {                                                          \
      LOG_INFO("OK: " msg " (0x%08x)", ##__VA_ARGS__, _act);          \
    }                                                                 \
  } while (0)

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

  dif_alert_handler_t alert_handler;
  CHECK_DIF_OK(dif_alert_handler_init(mmio_region_from_addr(kAlertHandlerBase),
                                      &alert_handler));

  // ---------------------------------------------------------------------------
  // 1. ALERT_TEST one-shot pulse behavior (UART1 = Alert 1)
  //    In uart.sv:78-81:
  //      assign alert_test = { reg2hw.alert_test.q & reg2hw.alert_test.qe };
  //    Writing 1 to ALERT_TEST pulses alert_test_i for 1 cycle.
  // ---------------------------------------------------------------------------
  CHECK_DIF_OK(dif_alert_handler_alert_acknowledge(
      &alert_handler, kTopEarlgreyAlertIdUart1FatalFault));
  abs_mmio_write32(kUart1Base + UART_ALERT_TEST_REG_OFFSET,
                   1u << UART_ALERT_TEST_FATAL_FAULT_BIT);
  busy_spin_micros(10);

  bool is_cause = false;
  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
      &alert_handler, kTopEarlgreyAlertIdUart1FatalFault, &is_cause));
  EXPECT_EQ(is_cause, 1u, "UART1 ALERT_TEST sets ALERT_CAUSE");

  CHECK_DIF_OK(dif_alert_handler_alert_acknowledge(
      &alert_handler, kTopEarlgreyAlertIdUart1FatalFault));
  busy_spin_micros(10);
  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
      &alert_handler, kTopEarlgreyAlertIdUart1FatalFault, &is_cause));
  EXPECT_EQ(is_cause, 0u,
            "UART1 ALERT_CAUSE stays 0 after ack (1-cycle ALERT_TEST pulse)");

  // ---------------------------------------------------------------------------
  // 2. OVRD register TXEN (bit 0) and TXVAL (bit 1) RW mask
  //    In uart.hjson:395-412:
  //      OVRD has fields TXEN (bit 0, rw) and TXVAL (bit 1, rw), mask 0x3.
  //    In QEMU ot_uart.c:1154, R_OVRD masks out TXEN (bit 0).
  // ---------------------------------------------------------------------------
  abs_mmio_write32(kUart1Base + UART_OVRD_REG_OFFSET, 0xffffffffu);
  EXPECT_EQ(abs_mmio_read32(kUart1Base + UART_OVRD_REG_OFFSET), 0x3u,
            "OVRD mask preserves both TXEN (bit 0) and TXVAL (bit 1)");

  abs_mmio_write32(kUart1Base + UART_OVRD_REG_OFFSET, 1u << UART_OVRD_TXEN_BIT);
  EXPECT_EQ(abs_mmio_read32(kUart1Base + UART_OVRD_REG_OFFSET), 0x1u,
            "OVRD preserves TXEN=1, TXVAL=0");
  abs_mmio_write32(kUart1Base + UART_OVRD_REG_OFFSET, 0u);

  // ---------------------------------------------------------------------------
  // 3. VAL register (rx_val_q) reset value and oversampled shift register
  //    In uart_core.sv:165-173, 301-304:
  //      rx_val_q resets to 16'h0000 and only shifts in rx_in on tick_baud_x16,
  //      which only runs when tx_enable || rx_enable.
  //      Once CTRL.RX=1, CTRL.LLPBK=1, CTRL.NCO=0xffff is enabled, rx_in=1 is
  //      shifted into rx_val_q on each tick_baud_x16 until VAL == 0xffff.
  // ---------------------------------------------------------------------------
  EXPECT_EQ(abs_mmio_read32(kUart1Base + UART_VAL_REG_OFFSET), 0x0000u,
            "VAL resets to 0x0000 before CTRL.TX/RX is enabled");

  uint32_t ctrl_llpbk = (0xffffu << UART_CTRL_NCO_OFFSET) |
                        (1u << UART_CTRL_LLPBK_BIT) | (1u << UART_CTRL_RX_BIT);
  abs_mmio_write32(kUart1Base + UART_CTRL_REG_OFFSET, ctrl_llpbk);
  busy_spin_micros(20);
  EXPECT_EQ(abs_mmio_read32(kUart1Base + UART_VAL_REG_OFFSET), 0xffffu,
            "VAL shifts in 16 high samples (0xffff) when RX+LLPBK enabled");
  abs_mmio_write32(kUart1Base + UART_CTRL_REG_OFFSET, 0u);

  // ---------------------------------------------------------------------------
  // 4. WDATA write while CTRL.TX == 0 clears BOTH STATUS.TXEMPTY and TXIDLE
  //    In uart_core.sv:146-147:
  //      assign hw2reg.status.txidle.d  = tx_uart_idle & ~tx_fifo_rvalid;
  //      assign hw2reg.status.txempty.d = ~tx_fifo_rvalid;
  //    When TX FIFO is non-empty (tx_fifo_rvalid == 1), BOTH TXEMPTY and TXIDLE
  //    must be 0, even when CTRL.TX == 0.
  // ---------------------------------------------------------------------------
  abs_mmio_write32(
      kUart1Base + UART_FIFO_CTRL_REG_OFFSET,
      (1u << UART_FIFO_CTRL_RXRST_BIT) | (1u << UART_FIFO_CTRL_TXRST_BIT));
  EXPECT_EQ(abs_mmio_read32(kUart1Base + UART_STATUS_REG_OFFSET), 0x3cu,
            "STATUS is 0x3c (RXEMPTY|RXIDLE|TXIDLE|TXEMPTY) when idle/empty");

  abs_mmio_write32(kUart1Base + UART_WDATA_REG_OFFSET, 0x5au);
  uint32_t fifo_status =
      abs_mmio_read32(kUart1Base + UART_FIFO_STATUS_REG_OFFSET);
  EXPECT_EQ(fifo_status & 0xffu, 1u,
            "WDATA write with CTRL.TX=0 pushes 1 byte to TX FIFO");

  uint32_t status = abs_mmio_read32(kUart1Base + UART_STATUS_REG_OFFSET);
  EXPECT_EQ((status >> UART_STATUS_TXEMPTY_BIT) & 1u, 0u,
            "STATUS.TXEMPTY is 0 when TX FIFO has 1 byte (CTRL.TX=0)");
  EXPECT_EQ((status >> UART_STATUS_TXIDLE_BIT) & 1u, 0u,
            "STATUS.TXIDLE is 0 when TX FIFO has 1 byte (CTRL.TX=0)");

  abs_mmio_write32(kUart1Base + UART_FIFO_CTRL_REG_OFFSET,
                   1u << UART_FIFO_CTRL_TXRST_BIT);
  EXPECT_EQ(abs_mmio_read32(kUart1Base + UART_STATUS_REG_OFFSET), 0x3cu,
            "STATUS returns to 0x3c after TXRST");

  // ---------------------------------------------------------------------------
  // 5. RDATA read when CTRL.RX == 0 still pops u_uart_rxfifo
  //    In uart_core.sv:282-299:
  //      u_uart_rxfifo rready_i is connected directly to reg2hw.rdata.re
  //      regardless of rx_enable (CTRL.RX).
  // ---------------------------------------------------------------------------
  uint32_t ctrl_slpbk = (0x8000u << UART_CTRL_NCO_OFFSET) |
                        (1u << UART_CTRL_SLPBK_BIT) | (1u << UART_CTRL_TX_BIT) |
                        (1u << UART_CTRL_RX_BIT);
  abs_mmio_write32(kUart1Base + UART_CTRL_REG_OFFSET, ctrl_slpbk);
  abs_mmio_write32(kUart1Base + UART_WDATA_REG_OFFSET, 0xa5u);

  for (int i = 0; i < 100; ++i) {
    uint32_t fs = abs_mmio_read32(kUart1Base + UART_FIFO_STATUS_REG_OFFSET);
    if (((fs >> UART_FIFO_STATUS_RXLVL_OFFSET) & 0xffu) >= 1u) {
      break;
    }
    busy_spin_micros(5);
  }

  // Disable CTRL.RX (and CTRL.TX) before reading RDATA.
  abs_mmio_write32(kUart1Base + UART_CTRL_REG_OFFSET, 0u);
  fifo_status = abs_mmio_read32(kUart1Base + UART_FIFO_STATUS_REG_OFFSET);
  EXPECT_EQ((fifo_status >> UART_FIFO_STATUS_RXLVL_OFFSET) & 0xffu, 1u,
            "RXLVL is 1 after SLPBK byte before RDATA read");

  uint32_t rdata = abs_mmio_read32(kUart1Base + UART_RDATA_REG_OFFSET);
  EXPECT_EQ(rdata, 0xa5u,
            "RDATA read with CTRL.RX=0 pops and returns queued byte 0xa5");

  fifo_status = abs_mmio_read32(kUart1Base + UART_FIFO_STATUS_REG_OFFSET);
  EXPECT_EQ((fifo_status >> UART_FIFO_STATUS_RXLVL_OFFSET) & 0xffu, 0u,
            "RXLVL decrements to 0 after RDATA read with CTRL.RX=0");

  // ---------------------------------------------------------------------------
  // 6. TIMEOUT_CTRL triggers INTR_STATE.RX_TIMEOUT when RX FIFO is non-empty
  //    In uart_core.sv:352-386:
  //      When rx_fifo_depth > 0 and TIMEOUT_CTRL.EN == 1, rx_timeout_count_q
  //      counts rx_tick_baud ticks and raises event_rx_timeout when equal to
  //      TIMEOUT_CTRL.VAL.
  // ---------------------------------------------------------------------------
  abs_mmio_write32(
      kUart1Base + UART_FIFO_CTRL_REG_OFFSET,
      (1u << UART_FIFO_CTRL_RXRST_BIT) | (1u << UART_FIFO_CTRL_TXRST_BIT));
  abs_mmio_write32(kUart1Base + UART_INTR_STATE_REG_OFFSET, 0x1ffu);
  abs_mmio_write32(kUart1Base + UART_TIMEOUT_CTRL_REG_OFFSET,
                   (1u << UART_TIMEOUT_CTRL_EN_BIT) | 4u);
  abs_mmio_write32(kUart1Base + UART_CTRL_REG_OFFSET, ctrl_slpbk);
  abs_mmio_write32(kUart1Base + UART_WDATA_REG_OFFSET, 0x3cu);

  busy_spin_micros(150);

  uint32_t intr_state =
      abs_mmio_read32(kUart1Base + UART_INTR_STATE_REG_OFFSET);
  EXPECT_EQ((intr_state >> UART_INTR_COMMON_RX_TIMEOUT_BIT) & 1u, 1u,
            "INTR_STATE.RX_TIMEOUT asserts after un-popped RX byte timeout");

  // ---------------------------------------------------------------------------
  // 7. Wave 2: TIMEOUT_CTRL with EN=1, VAL=0 fires INTR_STATE.RX_TIMEOUT
  //    immediately even when RX FIFO is empty and CTRL.RX=0
  //    In uart_core.sv:376:
  //      assign event_rx_timeout = (rx_timeout_count_q == uart_rxto_val) &
  //      uart_rxto_en;
  //    Because rx_timeout_count_q == 0 when rx_fifo_depth == 0, setting
  //    TIMEOUT_CTRL = (1 << 31) | 0 immediately asserts event_rx_timeout.
  // ---------------------------------------------------------------------------
  abs_mmio_write32(kUart1Base + UART_TIMEOUT_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kUart1Base + UART_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(
      kUart1Base + UART_FIFO_CTRL_REG_OFFSET,
      (1u << UART_FIFO_CTRL_RXRST_BIT) | (1u << UART_FIFO_CTRL_TXRST_BIT));
  abs_mmio_write32(kUart1Base + UART_INTR_STATE_REG_OFFSET, 0x1ffu);

  abs_mmio_write32(kUart1Base + UART_TIMEOUT_CTRL_REG_OFFSET,
                   (1u << UART_TIMEOUT_CTRL_EN_BIT) | 0u);
  intr_state = abs_mmio_read32(kUart1Base + UART_INTR_STATE_REG_OFFSET);
  EXPECT_EQ((intr_state >> UART_INTR_COMMON_RX_TIMEOUT_BIT) & 1u, 1u,
            "TIMEOUT_CTRL EN=1, VAL=0 immediately sets INTR_STATE.RX_TIMEOUT "
            "even when RX FIFO is empty");
  abs_mmio_write32(kUart1Base + UART_TIMEOUT_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kUart1Base + UART_INTR_STATE_REG_OFFSET, 0x1ffu);

  // ---------------------------------------------------------------------------
  // 8. Wave 2: OVRD.TXEN=1, OVRD.TXVAL=0 does NOT override rx_in in SLPBK
  //    In uart_core.sv:213-224, 260:
  //      ovrd_tx_en overrides tx_out_q (external tx pin), whereas sys_loopback
  //      routes tx_out (from uart_tx, which is 1 when idle) to rx_in.
  //      Therefore VAL remains 0xffff in SLPBK when OVRD.TXEN=1, OVRD.TXVAL=0.
  // ---------------------------------------------------------------------------
  abs_mmio_write32(kUart1Base + UART_OVRD_REG_OFFSET,
                   (1u << UART_OVRD_TXEN_BIT) | (0u << UART_OVRD_TXVAL_BIT));
  abs_mmio_write32(kUart1Base + UART_CTRL_REG_OFFSET,
                   (0xffffu << UART_CTRL_NCO_OFFSET) |
                       (1u << UART_CTRL_SLPBK_BIT) | (1u << UART_CTRL_TX_BIT));
  busy_spin_micros(20);
  EXPECT_EQ(abs_mmio_read32(kUart1Base + UART_VAL_REG_OFFSET), 0xffffu,
            "VAL remains 0xffff in SLPBK when OVRD.TXEN=1, OVRD.TXVAL=0 "
            "(sys_loopback taps tx_out before ovrd mux)");
  abs_mmio_write32(kUart1Base + UART_OVRD_REG_OFFSET, 0u);
  abs_mmio_write32(kUart1Base + UART_CTRL_REG_OFFSET, 0u);

  // ---------------------------------------------------------------------------
  // 9. Wave 2: TX shift register & FIFO timing when INTR_ENABLE == 0
  //    In uart_core.sv:179, 200-211, 324 & uart_tx.sv:58-77:
  //      When INTR_ENABLE == 0 and CTRL.TX=1, CTRL.NCO=1 (slow baud rate),
  //      writing 2 bytes to WDATA pops byte 0 into uart_tx (setting
  //      STATUS.TXIDLE = 0) and keeps byte 1 in u_uart_txfifo (setting
  //      FIFO_STATUS.TXLVL = 1, STATUS.TXEMPTY = 0, INTR_STATE.TX_DONE = 0).
  // ---------------------------------------------------------------------------
  abs_mmio_write32(kUart1Base + UART_INTR_ENABLE_REG_OFFSET, 0u);
  abs_mmio_write32(
      kUart1Base + UART_FIFO_CTRL_REG_OFFSET,
      (1u << UART_FIFO_CTRL_RXRST_BIT) | (1u << UART_FIFO_CTRL_TXRST_BIT));
  abs_mmio_write32(kUart1Base + UART_INTR_STATE_REG_OFFSET, 0x1ffu);
  abs_mmio_write32(kUart1Base + UART_CTRL_REG_OFFSET,
                   (1u << UART_CTRL_NCO_OFFSET) | (1u << UART_CTRL_TX_BIT));
  abs_mmio_write32(kUart1Base + UART_WDATA_REG_OFFSET, 0x11u);
  abs_mmio_write32(kUart1Base + UART_WDATA_REG_OFFSET, 0x22u);

  status = abs_mmio_read32(kUart1Base + UART_STATUS_REG_OFFSET);
  fifo_status = abs_mmio_read32(kUart1Base + UART_FIFO_STATUS_REG_OFFSET);
  intr_state = abs_mmio_read32(kUart1Base + UART_INTR_STATE_REG_OFFSET);

  EXPECT_EQ(
      (status >> UART_STATUS_TXIDLE_BIT) & 1u, 0u,
      "STATUS.TXIDLE is 0 while transmitting with INTR_ENABLE=0 and NCO=1");
  EXPECT_EQ((fifo_status >> UART_FIFO_STATUS_TXLVL_OFFSET) & 0xffu, 1u,
            "FIFO_STATUS.TXLVL is 1 after writing 2 bytes with INTR_ENABLE=0 "
            "and NCO=1");
  EXPECT_EQ((intr_state >> UART_INTR_COMMON_TX_DONE_BIT) & 1u, 0u,
            "INTR_STATE.TX_DONE is 0 while transmitting with INTR_ENABLE=0 and "
            "NCO=1");

  // Clean up UART1 state.
  abs_mmio_write32(kUart1Base + UART_TIMEOUT_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kUart1Base + UART_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(
      kUart1Base + UART_FIFO_CTRL_REG_OFFSET,
      (1u << UART_FIFO_CTRL_RXRST_BIT) | (1u << UART_FIFO_CTRL_TXRST_BIT));
  abs_mmio_write32(kUart1Base + UART_INTR_STATE_REG_OFFSET, 0x1ffu);

  // ---------------------------------------------------------------------------
  // 10. Wave 5: UART_PERMIT sub-word write wr_err & addrmiss (0x34..0x3c)
  // ---------------------------------------------------------------------------
  // a) 4'b1111 registers (UART_CTRL at 0x10, UART_TIMEOUT_CTRL at 0x30):
  //    sb/sh must raise Store Access Fault (mcause = 7) and NOT mutate state.
  abs_mmio_write32(kUart1Base + UART_CTRL_REG_OFFSET, 0x12340000u);
  g_fault_count = 0;
  *(volatile uint8_t *)(kUart1Base + UART_CTRL_REG_OFFSET) = 0x03u;
  EXPECT_EQ(g_fault_count, 1u, "sb to UART_CTRL raises Store Access Fault");
  EXPECT_EQ(g_last_mcause, 7u, "sb to UART_CTRL mcause == 7");
  *(volatile uint16_t *)(kUart1Base + UART_CTRL_REG_OFFSET) = 0x0033u;
  EXPECT_EQ(g_fault_count, 2u, "sh to UART_CTRL raises Store Access Fault");
  EXPECT_EQ(abs_mmio_read32(kUart1Base + UART_CTRL_REG_OFFSET), 0x12340000u,
            "UART_CTRL unchanged after rejected sub-word writes");
  abs_mmio_write32(kUart1Base + UART_CTRL_REG_OFFSET, 0u);

  // b) 4'b0011 register (UART_INTR_ENABLE at 0x04):
  //    sb at +0 must fail (mcause = 7), sh at +2 must fail (mcause = 7),
  //    sh at +0 must succeed!
  abs_mmio_write32(kUart1Base + UART_INTR_ENABLE_REG_OFFSET, 0u);
  g_fault_count = 0;
  *(volatile uint8_t *)(kUart1Base + UART_INTR_ENABLE_REG_OFFSET) = 0x05u;
  EXPECT_EQ(g_fault_count, 1u,
            "sb to UART_INTR_ENABLE raises Store Access Fault");
  *(volatile uint16_t *)(kUart1Base + UART_INTR_ENABLE_REG_OFFSET + 2u) = 0x05u;
  EXPECT_EQ(g_fault_count, 2u,
            "sh to UART_INTR_ENABLE+2 raises Store Access Fault");
  EXPECT_EQ(abs_mmio_read32(kUart1Base + UART_INTR_ENABLE_REG_OFFSET), 0u,
            "UART_INTR_ENABLE unchanged after rejected sub-word writes");

  g_fault_count = 0;
  *(volatile uint16_t *)(kUart1Base + UART_INTR_ENABLE_REG_OFFSET) = 0x0105u;
  EXPECT_EQ(g_fault_count, 0u,
            "sh to UART_INTR_ENABLE+0 (UART_PERMIT=4'b0011) succeeds");
  EXPECT_EQ(abs_mmio_read32(kUart1Base + UART_INTR_ENABLE_REG_OFFSET), 0x0105u,
            "UART_INTR_ENABLE updated to 0x0105 after sh to +0");
  abs_mmio_write32(kUart1Base + UART_INTR_ENABLE_REG_OFFSET, 0u);

  // c) 4'b0001 register (UART_OVRD at 0x28):
  //    sb at +1 must fail (mcause = 7), sb at +0 must succeed!
  abs_mmio_write32(kUart1Base + UART_OVRD_REG_OFFSET, 0u);
  g_fault_count = 0;
  *(volatile uint8_t *)(kUart1Base + UART_OVRD_REG_OFFSET + 1u) = 0x03u;
  EXPECT_EQ(g_fault_count, 1u, "sb to UART_OVRD+1 raises Store Access Fault");
  EXPECT_EQ(abs_mmio_read32(kUart1Base + UART_OVRD_REG_OFFSET), 0u,
            "UART_OVRD unchanged after rejected sb to +1");
  g_fault_count = 0;
  *(volatile uint8_t *)(kUart1Base + UART_OVRD_REG_OFFSET) = 0x03u;
  EXPECT_EQ(g_fault_count, 0u,
            "sb to UART_OVRD+0 (UART_PERMIT=4'b0001) succeeds");
  EXPECT_EQ(abs_mmio_read32(kUart1Base + UART_OVRD_REG_OFFSET), 0x03u,
            "UART_OVRD updated to 0x03 after sb to +0");
  abs_mmio_write32(kUart1Base + UART_OVRD_REG_OFFSET, 0u);

  // d) addrmiss on unmapped offset 0x34 within the 0x40-byte UART1 window:
  g_fault_count = 0;
  (void)*(volatile uint32_t *)(kUart1Base + 0x34u);
  EXPECT_EQ(g_fault_count, 1u, "lw from UART1+0x34 (addrmiss) faults");
  EXPECT_EQ(g_last_mcause, 5u, "lw from UART1+0x34 mcause == 5");
  *(volatile uint32_t *)(kUart1Base + 0x34u) = 0u;
  EXPECT_EQ(g_fault_count, 2u, "sw to UART1+0x34 (addrmiss) faults");
  EXPECT_EQ(g_last_mcause, 7u, "sw to UART1+0x34 mcause == 7");

  // ---------------------------------------------------------------------------
  // 11. Status-type INTR_TEST test_q latch behavior (prim_intr_hw IntrT=Status)
  //     In prim_intr_hw.sv:71-84, writing INTR_TEST latches reg2hw_intr_test_q
  //     into test_q for status-type interrupts (RX_WATERMARK, TX_WATERMARK,
  //     TX_EMPTY), asserting INTR_STATE even when event_intr_i is 0, ignoring
  //     RW1C writes to INTR_STATE, and clearing only when INTR_TEST is written
  //     with 0.
  // ---------------------------------------------------------------------------
  abs_mmio_write32(
      kUart1Base + UART_FIFO_CTRL_REG_OFFSET,
      (1u << UART_FIFO_CTRL_RXRST_BIT) | (1u << UART_FIFO_CTRL_TXRST_BIT));
  abs_mmio_write32(kUart1Base + UART_INTR_TEST_REG_OFFSET,
                   1u << UART_INTR_COMMON_RX_WATERMARK_BIT);
  intr_state = abs_mmio_read32(kUart1Base + UART_INTR_STATE_REG_OFFSET);
  EXPECT_EQ((intr_state >> UART_INTR_COMMON_RX_WATERMARK_BIT) & 1u, 1u,
            "INTR_TEST.RX_WATERMARK=1 asserts INTR_STATE.RX_WATERMARK when RX "
            "FIFO is empty");
  abs_mmio_write32(kUart1Base + UART_INTR_STATE_REG_OFFSET,
                   1u << UART_INTR_COMMON_RX_WATERMARK_BIT);
  intr_state = abs_mmio_read32(kUart1Base + UART_INTR_STATE_REG_OFFSET);
  EXPECT_EQ((intr_state >> UART_INTR_COMMON_RX_WATERMARK_BIT) & 1u, 1u,
            "RW1C write to INTR_STATE.RX_WATERMARK does not clear status-type "
            "test_q");
  abs_mmio_write32(kUart1Base + UART_INTR_TEST_REG_OFFSET, 0u);
  intr_state = abs_mmio_read32(kUart1Base + UART_INTR_STATE_REG_OFFSET);
  EXPECT_EQ((intr_state >> UART_INTR_COMMON_RX_WATERMARK_BIT) & 1u, 0u,
            "INTR_TEST=0 clears status-type test_q and deasserts "
            "INTR_STATE.RX_WATERMARK");

  return g_failures == 0;
}
