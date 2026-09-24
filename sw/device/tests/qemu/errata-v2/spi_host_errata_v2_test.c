// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// CW340 FPGA hardware behavior verification for SPI Host Controller (spi_host)
// on Earlgrey trunk-v2:
//
// 1. 73-word effective TXDATA queue depth (TxDepth + 1 = 73 via
//    spi_host_byte_select greedy pop, spi_host_data_fifos.sv:71-73, 122-125)
//    vs. documented TxDepth = 72 in spi_host.hjson:41-44, and combinational
//    STATUS.TXSTALL = 1 while STATUS.ACTIVE = 0 and STATUS.CMDQD = 1 in Idle
//    (spi_host_fsm.sv:96, 178-191).
// 2. Unconditional COMMAND queueing (CmdDepth = 4) and error detection
//    (CSIDINVAL / CMDINVAL / CMD_BUSY, where ~command_busy masks CMDINVAL on
//    the 5th command) while CONTROL.SPIEN == 0, paired with level-sensitive
//    CONTROL.SW_RST datapath clearing (spi_host.sv:212-286, 425-461).
// 3. SPI_HOST_PERMIT sub-word CSR write faults (spi_host_reg_pkg.sv:353-366)
//    and RXDATA (0x24) write / TXDATA (0x28) read window faults
//    (spi_host_window.sv:28-92) vs. legal sub-word sb/sh byte-enable writes to
//    TXDATA.
// 4. Newly introduced dif_spi_host_start_transaction() (sw/device/lib/dif/
//    dif_spi_host.c:340-413) issues write_command_reg() BEFORE writing TXDATA
//    and omits the length > 0 guard in issue_data_phase(), so a 0-length TX
//    segment underflows (length - 1 = 0xFFFF -> LEN = 511 = 512 bytes) and
//    writes a 512-byte WrOnly command with 0 TXDATA bytes, immediately
//    locking spi_host_fsm in STATUS.TXSTALL = 1 (CMDQD = 1) while returning
//    kDifOk; furthermore, when a 0-length Dummy segment is the final segment of
//    a multi-segment transaction, segment[0] is issued with CSAAT = 1 while
//    issue_dummy() skips segment[1] without clearing CSAAT.

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_spi_host.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top/spi_host_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kSpiHostBase = TOP_EARLGREY_SPI_HOST0_BASE_ADDR,
};

static volatile bool g_load_store_fault = false;
static volatile uint32_t g_last_mcause = 0;

static void advance_mepc_over_faulting_insn(void) {
  uint32_t mepc = 0;
  CSR_READ(CSR_REG_MEPC, &mepc);
  uint16_t insn_half = *(const volatile uint16_t *)mepc;
  uint32_t step = ((insn_half & 0x3u) == 0x3u) ? 4u : 2u;
  CSR_WRITE(CSR_REG_MEPC, mepc + step);
}

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = ibex_mcause_read();
  g_load_store_fault = true;
  g_last_mcause = mcause;
  advance_mepc_over_faulting_insn();
}

static void spi_host_sw_reset_and_clear(void) {
  abs_mmio_write32(kSpiHostBase + SPI_HOST_CONTROL_REG_OFFSET,
                   1u << SPI_HOST_CONTROL_SW_RST_BIT);
  abs_mmio_write32(kSpiHostBase + SPI_HOST_CONTROL_REG_OFFSET, 0u);
  abs_mmio_write32(kSpiHostBase + SPI_HOST_ERROR_STATUS_REG_OFFSET, 0x3fu);
  abs_mmio_write32(kSpiHostBase + SPI_HOST_INTR_STATE_REG_OFFSET, 0x3u);
}

static void test_txdepth73_and_idle_txstall(void) {
  LOG_INFO("Test 1: 73-word effective TXDATA depth and Idle TXSTALL");
  spi_host_sw_reset_and_clear();

  for (uint32_t i = 0; i < 72u; ++i) {
    abs_mmio_write32(kSpiHostBase + SPI_HOST_TXDATA_REG_OFFSET, 0x1000u + i);
  }
  uint32_t status = abs_mmio_read32(kSpiHostBase + SPI_HOST_STATUS_REG_OFFSET);
  uint32_t err_sts =
      abs_mmio_read32(kSpiHostBase + SPI_HOST_ERROR_STATUS_REG_OFFSET);
  uint32_t txqd = bitfield_field32_read(status, SPI_HOST_STATUS_TXQD_FIELD);
  bool txfull = bitfield_bit32_read(status, SPI_HOST_STATUS_TXFULL_BIT);
  CHECK(txqd == 72u && !txfull && err_sts == 0u,
        "After 72 words expected TXQD=72, TXFULL=0, err=0 (got TXQD=%u, "
        "TXFULL=%d, err=0x%x)",
        txqd, txfull, err_sts);

  // 73rd word succeeds (1 in u_select + 72 in u_tx_fifo):
  abs_mmio_write32(kSpiHostBase + SPI_HOST_TXDATA_REG_OFFSET, 0x1048u);
  status = abs_mmio_read32(kSpiHostBase + SPI_HOST_STATUS_REG_OFFSET);
  err_sts = abs_mmio_read32(kSpiHostBase + SPI_HOST_ERROR_STATUS_REG_OFFSET);
  txqd = bitfield_field32_read(status, SPI_HOST_STATUS_TXQD_FIELD);
  txfull = bitfield_bit32_read(status, SPI_HOST_STATUS_TXFULL_BIT);
  CHECK(txqd == 73u && txfull && err_sts == 0u,
        "After 73rd word expected TXQD=73, TXFULL=1, err=0 (got TXQD=%u, "
        "TXFULL=%d, err=0x%x)",
        txqd, txfull, err_sts);

  // 74th word triggers ERROR_STATUS.OVERFLOW:
  abs_mmio_write32(kSpiHostBase + SPI_HOST_TXDATA_REG_OFFSET, 0x1049u);
  err_sts = abs_mmio_read32(kSpiHostBase + SPI_HOST_ERROR_STATUS_REG_OFFSET);
  CHECK(bitfield_bit32_read(err_sts, SPI_HOST_ERROR_STATUS_OVERFLOW_BIT),
        "Expected 74th TXDATA word to set ERROR_STATUS.OVERFLOW=1, got 0x%x",
        err_sts);

  // Combinational TXSTALL=1 with ACTIVE=0 and CMDQD=1 in Idle:
  spi_host_sw_reset_and_clear();
  abs_mmio_write32(kSpiHostBase + SPI_HOST_CONFIGOPTS_REG_OFFSET, 0u);
  abs_mmio_write32(kSpiHostBase + SPI_HOST_CSID_REG_OFFSET, 0u);
  abs_mmio_write32(kSpiHostBase + SPI_HOST_CONTROL_REG_OFFSET,
                   1u << SPI_HOST_CONTROL_SPIEN_BIT);
  uint32_t cmd_wronly = (2u << SPI_HOST_COMMAND_DIRECTION_OFFSET) |
                        (0u << SPI_HOST_COMMAND_SPEED_OFFSET) |
                        (0u << SPI_HOST_COMMAND_LEN_OFFSET);
  abs_mmio_write32(kSpiHostBase + SPI_HOST_COMMAND_REG_OFFSET, cmd_wronly);
  status = abs_mmio_read32(kSpiHostBase + SPI_HOST_STATUS_REG_OFFSET);
  bool txstall = bitfield_bit32_read(status, SPI_HOST_STATUS_TXSTALL_BIT);
  bool active = bitfield_bit32_read(status, SPI_HOST_STATUS_ACTIVE_BIT);
  uint32_t cmdqd = bitfield_field32_read(status, SPI_HOST_STATUS_CMDQD_FIELD);
  CHECK(txstall && !active && cmdqd == 1u,
        "Expected combinational TXSTALL=1, ACTIVE=0, CMDQD=1 in Idle "
        "(status=0x%08x)",
        status);
}

static void test_sw_rst_and_spien0_cmd_queue(void) {
  LOG_INFO("Test 2: Level-sensitive SW_RST and SPIEN=0 COMMAND queueing");
  abs_mmio_write32(kSpiHostBase + SPI_HOST_CONTROL_REG_OFFSET,
                   1u << SPI_HOST_CONTROL_SW_RST_BIT);
  abs_mmio_write32(kSpiHostBase + SPI_HOST_TXDATA_REG_OFFSET, 0xdeadbeefu);
  uint32_t cmd_rdonly = (1u << SPI_HOST_COMMAND_DIRECTION_OFFSET);
  abs_mmio_write32(kSpiHostBase + SPI_HOST_COMMAND_REG_OFFSET, cmd_rdonly);
  uint32_t status = abs_mmio_read32(kSpiHostBase + SPI_HOST_STATUS_REG_OFFSET);
  CHECK(bitfield_field32_read(status, SPI_HOST_STATUS_TXQD_FIELD) == 0u &&
            bitfield_field32_read(status, SPI_HOST_STATUS_CMDQD_FIELD) == 0u,
        "Expected TXDATA & COMMAND writes while SW_RST=1 to be discarded "
        "(status=0x%08x)",
        status);

  abs_mmio_write32(kSpiHostBase + SPI_HOST_CONTROL_REG_OFFSET, 0u);
  abs_mmio_write32(kSpiHostBase + SPI_HOST_ERROR_STATUS_REG_OFFSET, 0x3fu);

  for (int i = 0; i < 4; ++i) {
    abs_mmio_write32(kSpiHostBase + SPI_HOST_COMMAND_REG_OFFSET, cmd_rdonly);
  }
  status = abs_mmio_read32(kSpiHostBase + SPI_HOST_STATUS_REG_OFFSET);
  CHECK(bitfield_field32_read(status, SPI_HOST_STATUS_CMDQD_FIELD) == 4u &&
            !bitfield_bit32_read(status, SPI_HOST_STATUS_READY_BIT),
        "Expected CMDQD=4 and READY=0 when 4 commands are queued with SPIEN=0 "
        "(status=0x%08x)",
        status);

  uint32_t cmd_inval_dual_rdwr = (3u << SPI_HOST_COMMAND_DIRECTION_OFFSET) |
                                 (2u << SPI_HOST_COMMAND_SPEED_OFFSET);
  abs_mmio_write32(kSpiHostBase + SPI_HOST_COMMAND_REG_OFFSET,
                   cmd_inval_dual_rdwr);
  uint32_t err_sts =
      abs_mmio_read32(kSpiHostBase + SPI_HOST_ERROR_STATUS_REG_OFFSET);
  CHECK(bitfield_bit32_read(err_sts, SPI_HOST_ERROR_STATUS_CMDBUSY_BIT) &&
            !bitfield_bit32_read(err_sts, SPI_HOST_ERROR_STATUS_CMDINVAL_BIT),
        "Expected 5th invalid command to set CMD_BUSY=1 and mask CMDINVAL=0 "
        "(got ERROR_STATUS=0x%x)",
        err_sts);
}

static void test_permit_and_window_faults(void) {
  LOG_INFO("Test 3: SPI_HOST_PERMIT and RXDATA/TXDATA window access checks");
  spi_host_sw_reset_and_clear();

  g_load_store_fault = false;
  g_last_mcause = 0;
  abs_mmio_write8(kSpiHostBase + SPI_HOST_CONTROL_REG_OFFSET, 0u);
  CHECK(g_load_store_fault && g_last_mcause == 7u,
        "Expected sb to CONTROL (PERMIT=4'b1111) to fault with mcause=7");

  g_load_store_fault = false;
  g_last_mcause = 0;
  abs_mmio_write8(kSpiHostBase + SPI_HOST_COMMAND_REG_OFFSET, 0u);
  CHECK(g_load_store_fault && g_last_mcause == 7u,
        "Expected sb to COMMAND (PERMIT=4'b0011) to fault with mcause=7");

  g_load_store_fault = false;
  g_last_mcause = 0;
  abs_mmio_write32(kSpiHostBase + SPI_HOST_RXDATA_REG_OFFSET, 0u);
  CHECK(g_load_store_fault && g_last_mcause == 7u,
        "Expected write to RXDATA (0x24) to fault with mcause=7");

  g_load_store_fault = false;
  g_last_mcause = 0;
  (void)abs_mmio_read32(kSpiHostBase + SPI_HOST_TXDATA_REG_OFFSET);
  CHECK(g_load_store_fault && g_last_mcause == 5u,
        "Expected read from TXDATA (0x28) to fault with mcause=5");

  g_load_store_fault = false;
  CHECK(abs_mmio_read32(kSpiHostBase + SPI_HOST_COMMAND_REG_OFFSET) == 0u &&
            !g_load_store_fault,
        "Expected read from WO CSR COMMAND (0x20) to return 0 without fault");

  g_load_store_fault = false;
  abs_mmio_write8(kSpiHostBase + SPI_HOST_TXDATA_REG_OFFSET, 0xa5u);
  uint32_t status = abs_mmio_read32(kSpiHostBase + SPI_HOST_STATUS_REG_OFFSET);
  CHECK(!g_load_store_fault &&
            bitfield_field32_read(status, SPI_HOST_STATUS_TXQD_FIELD) == 1u,
        "Expected sb to TXDATA (0x28) to succeed and increment TXQD to 1");
}

static void test_v2_dif_start_transaction_zero_len_underflow_and_csaat(void) {
  LOG_INFO(
      "Test 4: dif_spi_host_start_transaction() 0-length TX segment underflow "
      "(LEN=511 -> 512 bytes) and trailing 0-length Dummy CSAAT leak");
  spi_host_sw_reset_and_clear();

  dif_spi_host_t spi_host;
  CHECK_DIF_OK(
      dif_spi_host_init(mmio_region_from_addr(kSpiHostBase), &spi_host));
  abs_mmio_write32(kSpiHostBase + SPI_HOST_CONFIGOPTS_REG_OFFSET, 0u);
  abs_mmio_write32(kSpiHostBase + SPI_HOST_CSID_REG_OFFSET, 0u);
  abs_mmio_write32(kSpiHostBase + SPI_HOST_CONTROL_REG_OFFSET,
                   1u << SPI_HOST_CONTROL_SPIEN_BIT);

  // Part A: Calling dif_spi_host_start_transaction() with a 0-length TX segment
  // returns kDifOk, but issue_data_phase() calls write_command_reg(spi_host, 0)
  // (programming LEN = (0 - 1) & 0x1FF = 511 -> 512 bytes!) and writes 0 bytes
  // to TXDATA, immediately stalling the hardware in STATUS.TXSTALL = 1!
  uint8_t dummy_buf = 0x5au;
  dif_spi_host_segment_t zero_tx_seg = {
      .type = kDifSpiHostSegmentTypeTx,
      .tx =
          {
              .width = kDifSpiHostWidthStandard,
              .buf = &dummy_buf,
              .length = 0u,
          },
  };
  CHECK_DIF_OK(dif_spi_host_start_transaction(&spi_host, 0u, &zero_tx_seg, 1u));

  uint32_t status = abs_mmio_read32(kSpiHostBase + SPI_HOST_STATUS_REG_OFFSET);
  CHECK(bitfield_bit32_read(status, SPI_HOST_STATUS_TXSTALL_BIT) &&
            bitfield_field32_read(status, SPI_HOST_STATUS_CMDQD_FIELD) == 1u &&
            bitfield_field32_read(status, SPI_HOST_STATUS_TXQD_FIELD) == 0u,
        "Expected 0-length TX segment in dif_spi_host_start_transaction to "
        "underflow LEN=511 and stall in TXSTALL=1, CMDQD=1, TXQD=0 "
        "(status=0x%08x)",
        status);

  // Part B: Calling dif_spi_host_start_transaction() with a 2-segment array
  // {Opcode, Dummy(length=0)} while SPIEN=0 queues segments[0] (Opcode) with
  // CSAAT = 1 (because i=0 != length-1) and then skips segments[1] in
  // issue_dummy() without issuing any CSAAT=0 command, leaving only 1 command
  // in u_cmd_queue with CSAAT=1!
  spi_host_sw_reset_and_clear();
  dif_spi_host_segment_t two_segs[2] = {
      {
          .type = kDifSpiHostSegmentTypeOpcode,
          .opcode =
              {
                  .width = kDifSpiHostWidthStandard,
                  .opcode = 0x9fu,
              },
      },
      {
          .type = kDifSpiHostSegmentTypeDummy,
          .dummy =
              {
                  .width = kDifSpiHostWidthStandard,
                  .length = 0u,
              },
      },
  };
  CHECK_DIF_OK(dif_spi_host_start_transaction(&spi_host, 0u, two_segs, 2u));
  status = abs_mmio_read32(kSpiHostBase + SPI_HOST_STATUS_REG_OFFSET);
  CHECK(bitfield_field32_read(status, SPI_HOST_STATUS_CMDQD_FIELD) == 1u &&
            bitfield_field32_read(status, SPI_HOST_STATUS_TXQD_FIELD) == 1u,
        "Expected trailing 0-length Dummy segment to be skipped after setting "
        "CSAAT=1 on Opcode (CMDQD=%u, TXQD=%u)",
        bitfield_field32_read(status, SPI_HOST_STATUS_CMDQD_FIELD),
        bitfield_field32_read(status, SPI_HOST_STATUS_TXQD_FIELD));
}

bool test_main(void) {
  test_txdepth73_and_idle_txstall();
  test_sw_rst_and_spien0_cmd_queue();
  test_permit_and_window_faults();
  test_v2_dif_start_transaction_zero_len_underflow_and_csaat();
  spi_host_sw_reset_and_clear();
  LOG_INFO("All spi_host trunk-v2 errata tests passed on CW340 FPGA!");
  return true;
}
