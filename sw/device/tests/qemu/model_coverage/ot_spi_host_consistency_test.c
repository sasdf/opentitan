// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/irq.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "spi_host_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kBase = TOP_EARLGREY_SPI_HOST0_BASE_ADDR,
  kCtrlSpien = (1u << SPI_HOST_CONTROL_SPIEN_BIT),
  kCtrlSwRst = (1u << SPI_HOST_CONTROL_SW_RST_BIT),
  kCtrlOutputEn = (1u << SPI_HOST_CONTROL_OUTPUT_EN_BIT),
  kCtrlDefaultWatermarks = 0x7fu,
  kCmdDirRxOnly = (1u << SPI_HOST_COMMAND_DIRECTION_OFFSET),
};

static volatile bool load_access_fault_seen = false;
static volatile bool store_access_fault_seen = false;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  uint32_t mcause = ibex_mcause_read();
  ibex_exc_t exc = (ibex_exc_t)(mcause & kIbexExcMax);
  if (exc == kIbexExcLoadAccessFault) {
    load_access_fault_seen = true;
  } else if (exc == kIbexExcStoreAccessFault) {
    store_access_fault_seen = true;
  } else {
    ottf_generic_fault_print(exc_info, "Unexpected Load/Store Fault", mcause);
    abort();
  }
}

static uint32_t get_rxqd(uint32_t status) {
  return (status >> SPI_HOST_STATUS_RXQD_OFFSET) & SPI_HOST_STATUS_RXQD_MASK;
}

static uint32_t get_cmdqd(uint32_t status) {
  return (status >> SPI_HOST_STATUS_CMDQD_OFFSET) & SPI_HOST_STATUS_CMDQD_MASK;
}

bool test_main(void) {
  irq_global_ctrl(false);
  irq_external_ctrl(false);

  // Reset and initialize SPI_HOST0.
  abs_mmio_write32(kBase + SPI_HOST_CONTROL_REG_OFFSET,
                   kCtrlSwRst | kCtrlDefaultWatermarks);
  abs_mmio_write32(kBase + SPI_HOST_CONTROL_REG_OFFSET,
                   kCtrlSpien | kCtrlOutputEn | kCtrlDefaultWatermarks);
  abs_mmio_write32(kBase + SPI_HOST_CONFIGOPTS_REG_OFFSET, 0x0u);
  abs_mmio_write32(kBase + SPI_HOST_CSID_REG_OFFSET, 0x0u);
  abs_mmio_write32(kBase + SPI_HOST_ERROR_ENABLE_REG_OFFSET, 0x1fu);
  abs_mmio_write32(kBase + SPI_HOST_ERROR_STATUS_REG_OFFSET, 0x3fu);
  abs_mmio_write32(kBase + SPI_HOST_INTR_STATE_REG_OFFSET, 0x3u);

  // 1. RXDATA Underflow Error (ERROR_STATUS.UNDERFLOW & INTR_STATE.ERROR).
  uint32_t status = abs_mmio_read32(kBase + SPI_HOST_STATUS_REG_OFFSET);
  CHECK((status & (1u << SPI_HOST_STATUS_RXEMPTY_BIT)) != 0u);
  CHECK(get_rxqd(status) == 0u);

  (void)abs_mmio_read32(kBase + SPI_HOST_RXDATA_REG_OFFSET);
  uint32_t err_status =
      abs_mmio_read32(kBase + SPI_HOST_ERROR_STATUS_REG_OFFSET);
  uint32_t intr_state = abs_mmio_read32(kBase + SPI_HOST_INTR_STATE_REG_OFFSET);
  CHECK((err_status & (1u << SPI_HOST_ERROR_STATUS_UNDERFLOW_BIT)) != 0u);
  CHECK((intr_state & (1u << SPI_HOST_INTR_STATE_ERROR_BIT)) != 0u);

  // Clear ERROR_STATUS and INTR_STATE (RW1C).
  abs_mmio_write32(kBase + SPI_HOST_ERROR_STATUS_REG_OFFSET,
                   (1u << SPI_HOST_ERROR_STATUS_UNDERFLOW_BIT));
  abs_mmio_write32(kBase + SPI_HOST_INTR_STATE_REG_OFFSET,
                   (1u << SPI_HOST_INTR_STATE_ERROR_BIT));
  CHECK(abs_mmio_read32(kBase + SPI_HOST_ERROR_STATUS_REG_OFFSET) == 0x0u);
  CHECK((abs_mmio_read32(kBase + SPI_HOST_INTR_STATE_REG_OFFSET) &
         (1u << SPI_HOST_INTR_STATE_ERROR_BIT)) == 0x0u);

  // 2. RX FIFO Full Stall (STATUS.RXFULL & STATUS.RXSTALL) & Resume.
  // Issue a 268-byte (LEN = 267, 67 words, DIRECTION = 1 RX-only) command
  // exceeding rx_fifo (64 words = 256B) + byte_merge (4B) + shift_reg (1B)
  // without reading RXDATA so the FSM stalls with RXSTALL = 1.
  abs_mmio_write32(kBase + SPI_HOST_COMMAND_REG_OFFSET, kCmdDirRxOnly | 267u);

  for (uint32_t i = 0; i < 2000u; ++i) {
    status = abs_mmio_read32(kBase + SPI_HOST_STATUS_REG_OFFSET);
    if ((status & (1u << SPI_HOST_STATUS_RXSTALL_BIT)) != 0u) {
      break;
    }
    busy_spin_micros(5);
  }
  status = abs_mmio_read32(kBase + SPI_HOST_STATUS_REG_OFFSET);
  CHECK((status & (1u << SPI_HOST_STATUS_RXFULL_BIT)) != 0u);
  CHECK((status & (1u << SPI_HOST_STATUS_RXSTALL_BIT)) != 0u);
  CHECK(get_rxqd(status) == 64u);
  CHECK((status & (1u << SPI_HOST_STATUS_ACTIVE_BIT)) != 0u);

  // Pop 64 words from RXDATA; the FSM unstalls and receives the remaining 3
  // words (12 bytes).
  for (uint32_t i = 0; i < 64u; ++i) {
    (void)abs_mmio_read32(kBase + SPI_HOST_RXDATA_REG_OFFSET);
  }
  for (uint32_t i = 0; i < 1000u; ++i) {
    status = abs_mmio_read32(kBase + SPI_HOST_STATUS_REG_OFFSET);
    if ((status & (1u << SPI_HOST_STATUS_ACTIVE_BIT)) == 0u &&
        get_rxqd(status) == 3u) {
      break;
    }
    busy_spin_micros(5);
  }
  status = abs_mmio_read32(kBase + SPI_HOST_STATUS_REG_OFFSET);
  CHECK((status & (1u << SPI_HOST_STATUS_RXSTALL_BIT)) == 0u);
  CHECK(get_rxqd(status) == 3u);

  // Pop the final 3 words and assert RXEMPTY == 1 and ACTIVE == 0.
  for (uint32_t i = 0; i < 3u; ++i) {
    (void)abs_mmio_read32(kBase + SPI_HOST_RXDATA_REG_OFFSET);
  }
  status = abs_mmio_read32(kBase + SPI_HOST_STATUS_REG_OFFSET);
  CHECK((status & (1u << SPI_HOST_STATUS_RXEMPTY_BIT)) != 0u);
  CHECK(get_rxqd(status) == 0u);
  CHECK((status & (1u << SPI_HOST_STATUS_ACTIVE_BIT)) == 0u);

  // 3. COMMAND Write Under CONTROL.SW_RST & FSM Resume on ERROR_STATUS Clear.
  // Set CONTROL.SW_RST = 1 and CSID = 1 (invalid CSID since NumCS == 1), then
  // write COMMAND: SW_RST keeps FIFOs/FSM reset (CMDQD == 0) while latching
  // ERROR_STATUS.CSIDINVAL.
  abs_mmio_write32(kBase + SPI_HOST_CSID_REG_OFFSET, 1u);
  abs_mmio_write32(
      kBase + SPI_HOST_CONTROL_REG_OFFSET,
      kCtrlSpien | kCtrlSwRst | kCtrlOutputEn | kCtrlDefaultWatermarks);
  abs_mmio_write32(kBase + SPI_HOST_COMMAND_REG_OFFSET, kCmdDirRxOnly | 3u);

  status = abs_mmio_read32(kBase + SPI_HOST_STATUS_REG_OFFSET);
  CHECK(get_cmdqd(status) == 0u);
  CHECK(get_rxqd(status) == 0u);

  // Release SW_RST = 0 and restore CSID = 0 while ERROR_STATUS.CSIDINVAL is
  // still latched.
  abs_mmio_write32(kBase + SPI_HOST_CONTROL_REG_OFFSET,
                   kCtrlSpien | kCtrlOutputEn | kCtrlDefaultWatermarks);
  abs_mmio_write32(kBase + SPI_HOST_CSID_REG_OFFSET, 0u);
  err_status = abs_mmio_read32(kBase + SPI_HOST_ERROR_STATUS_REG_OFFSET);
  CHECK((err_status & (1u << SPI_HOST_ERROR_STATUS_CSIDINVAL_BIT)) != 0u);
  status = abs_mmio_read32(kBase + SPI_HOST_STATUS_REG_OFFSET);
  CHECK(get_cmdqd(status) == 0u);
  CHECK(get_rxqd(status) == 0u);

  // Queue a valid 4-byte RX COMMAND while ERROR_STATUS != 0: the command stays
  // queued in cmd_fifo (CMDQD == 1, RXQD == 0).
  abs_mmio_write32(kBase + SPI_HOST_COMMAND_REG_OFFSET, kCmdDirRxOnly | 3u);
  status = abs_mmio_read32(kBase + SPI_HOST_STATUS_REG_OFFSET);
  CHECK(get_cmdqd(status) == 1u);
  CHECK(get_rxqd(status) == 0u);

  // Clear ERROR_STATUS (RW1C): FSM automatically resumes, executes the queued
  // command, and deposits 1 word in RXDATA.
  abs_mmio_write32(kBase + SPI_HOST_ERROR_STATUS_REG_OFFSET, 0x3fu);
  abs_mmio_write32(kBase + SPI_HOST_INTR_STATE_REG_OFFSET, 0x3u);
  for (uint32_t i = 0; i < 1000u; ++i) {
    status = abs_mmio_read32(kBase + SPI_HOST_STATUS_REG_OFFSET);
    if ((status & (1u << SPI_HOST_STATUS_ACTIVE_BIT)) == 0u &&
        get_rxqd(status) == 1u) {
      break;
    }
    busy_spin_micros(5);
  }
  status = abs_mmio_read32(kBase + SPI_HOST_STATUS_REG_OFFSET);
  CHECK(get_cmdqd(status) == 0u);
  CHECK(get_rxqd(status) == 1u);
  (void)abs_mmio_read32(kBase + SPI_HOST_RXDATA_REG_OFFSET);
  status = abs_mmio_read32(kBase + SPI_HOST_STATUS_REG_OFFSET);
  CHECK((status & (1u << SPI_HOST_STATUS_RXEMPTY_BIT)) != 0u);

  // 4. TXDATA Read & RXDATA Write Bus Error (spi_host_window.sv).
  load_access_fault_seen = false;
  (void)abs_mmio_read32(kBase + SPI_HOST_TXDATA_REG_OFFSET);
  CHECK(load_access_fault_seen);

  store_access_fault_seen = false;
  abs_mmio_write32(kBase + SPI_HOST_RXDATA_REG_OFFSET, 0xdeadbeefu);
  CHECK(store_access_fault_seen);

  LOG_INFO("ot_spi_host_consistency_test passed");
  return true;
}
