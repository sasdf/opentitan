// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_alert_handler.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/irq.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "uart_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kUart1Base = TOP_EARLGREY_UART1_BASE_ADDR,
  kAlertHandlerBase = TOP_EARLGREY_ALERT_HANDLER_BASE_ADDR,
};

static int g_failures = 0;

#define EXPECT_EQ(actual, expected, msg, ...)                                  \
  do {                                                                         \
    uint32_t _act = (uint32_t)(actual);                                        \
    uint32_t _exp = (uint32_t)(expected);                                      \
    if (_act != _exp) {                                                        \
      LOG_ERROR("MISMATCH: " msg " (actual=0x%08x, expected=0x%08x)",          \
                ##__VA_ARGS__, _act, _exp);                                    \
      g_failures++;                                                            \
    } else {                                                                   \
      LOG_INFO("OK: " msg " (0x%08x)", ##__VA_ARGS__, _act);                   \
    }                                                                          \
  } while (0)

bool test_main(void) {
  irq_global_ctrl(false);

  dif_alert_handler_t alert_handler;
  CHECK_DIF_OK(dif_alert_handler_init(
      mmio_region_from_addr(kAlertHandlerBase), &alert_handler));

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

  abs_mmio_write32(kUart1Base + UART_OVRD_REG_OFFSET,
                   1u << UART_OVRD_TXEN_BIT);
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
                        (1u << UART_CTRL_LLPBK_BIT) |
                        (1u << UART_CTRL_RX_BIT);
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
  abs_mmio_write32(kUart1Base + UART_FIFO_CTRL_REG_OFFSET,
                   (1u << UART_FIFO_CTRL_RXRST_BIT) |
                       (1u << UART_FIFO_CTRL_TXRST_BIT));
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
                        (1u << UART_CTRL_SLPBK_BIT) |
                        (1u << UART_CTRL_TX_BIT) |
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
  abs_mmio_write32(kUart1Base + UART_FIFO_CTRL_REG_OFFSET,
                   (1u << UART_FIFO_CTRL_RXRST_BIT) |
                       (1u << UART_FIFO_CTRL_TXRST_BIT));
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

  // Clean up UART1 state.
  abs_mmio_write32(kUart1Base + UART_TIMEOUT_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kUart1Base + UART_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kUart1Base + UART_FIFO_CTRL_REG_OFFSET,
                   (1u << UART_FIFO_CTRL_RXRST_BIT) |
                       (1u << UART_FIFO_CTRL_TXRST_BIT));
  abs_mmio_write32(kUart1Base + UART_INTR_STATE_REG_OFFSET, 0x1ffu);

  return g_failures == 0;
}
