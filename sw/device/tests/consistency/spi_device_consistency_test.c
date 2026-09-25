// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// Earlgrey v2 (`trunk-v2`) hardware errata & DIF consistency verification test
// for `spi_device` (`TOP_EARLGREY_SPI_DEVICE_BASE_ADDR = 0x40050000`):
//
// 1. `FLASH_STATUS` (`0x28`) `sys_csb_deasserted_pulse_i` readback gate
//    (`hw/ip/spi_device/rtl/spid_status.sv:308-314` vs.
//    `hw/ip/spi_device/data/spi_device.hjson:715-761`):
//    `sys_status_o` (which feeds TL-UL software reads of `FLASH_STATUS`)
//    updates only on `sys_csb_deasserted_pulse_i` (after >= 8 `SCK` clocks plus
//    `CSb` deassertion), never while `CSb == 1` (`SCK` idle).
// 2. `ADDR_MODE` (`0x20`) unconditional `PENDING` (`bit 31`) assertion
//    (`hw/ip/spi_device/rtl/spid_addr_4b.sv:52-68, 92-117` vs.
//    `hw/ip/spi_device/data/spi_device.hjson:696-711`):
//    Writing `ADDR_MODE` asserts `PENDING` (`bit 31`) on every software write
//    (`reg2hw_addr_mode_addr_4b_en_qe_i`), even when writing the already-active
//    `addr_4b_en` value, and holds `PENDING == 1` while `SCK` is idle.
// 3. Unmapped SRAM window holes `[0x1d40..0x1dff]` (`192 B`) and
//    `[0x1fc0..0x1fff]` (`64 B`)
//    (`hw/ip/spi_device/rtl/spi_device_reg_top.sv:142-146` vs.
//    `hw/ip/spi_device/doc/programmers_guide.md:7-25`): Accesses to these
//    offsets inside `[0x1000..0x1fff]` steer to `u_reg_if`
//    (`addrmiss = 1 -> d_error = 1`) and raise synchronous Load/Store Access
//    Faults (`mcause = 5` / `mcause = 7`).
// 4. Directional (`ErrOnRead=1` / `ErrOnWrite=1`) and sub-word (`ByteAccess=0`)
//    restrictions on `egress_buffer` (`0x1000..0x1d3f`) and `ingress_buffer`
//    (`0x1e00..0x1fbf`) (`hw/ip/spi_device/rtl/spi_device.sv:1702-1776`).
// 5. Sub-word CSR write faults via `SPI_DEVICE_PERMIT` (`FLASH_STATUS` `0x28`
//    and `TPM_ACCESS_1` byte 1 `0x811`)
//    (`hw/ip/spi_device/rtl/spi_device_reg_pkg.sv`).
// 6. `dif_spi_device_get_flash_command_slot` copy-paste bug
//    (`sw/device/lib/dif/dif_spi_device.c:550-554`):
//    Reads `SPI_DEVICE_CMD_INFO_0_PAYLOAD_SWAP_EN_0_BIT` (`bit 21`) instead of
//    `SPI_DEVICE_CMD_INFO_0_ADDR_SWAP_EN_0_BIT` (`bit 10`) when populating
//    `command_info->passthrough_swap_address`.
// 7. `DefaultSramType = SramType1r1w`
// (`hw/ip/spi_device/rtl/spi_device_pkg.sv:411`,
//    `hw/ip/spi_device/rtl/spid_dpram.sv:79-128, 217-245`):
//    `u_spi2sys_mem` (`prim_ram_1r1w_async_adv` with `.EnableParity(1)` odd
//    parity) is only writable from `clk_spi_i` (`spi2sys_wr_req`). Reading any
//    unwritten word in `ingress_buffer` (`0x1e00..0x1fbf`) or reading
//    `UPLOAD_CMDFIFO` (`0x70`) / `UPLOAD_ADDRFIFO` (`0x74`) before an external
//    SPI upload asserts `rerror_o[1] = 1 -> d_error = 1` (`mcause = 5`).

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_spi_device.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top/spi_device_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kSpiDeviceBase = TOP_EARLGREY_SPI_DEVICE_BASE_ADDR,
  kEgressHoleStart = 0x1d40u,
  kEgressHoleEnd = 0x1dfcu,
  kIngressStart = SPI_DEVICE_INGRESS_BUFFER_REG_OFFSET,  // 0x1e00
  kIngressCmdFifoStart = 0x1f00u,
  kIngressAddrFifoStart = 0x1f40u,
  kIngressTpmWriteFifoStart = 0x1f80u,
  kIngressTailStart = 0x1fc0u,
  kIngressTailEnd = 0x1ffcu,
};

static volatile bool g_expect_fault = false;
static volatile uint32_t g_fault_count = 0;
static volatile uint32_t g_last_mcause = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t mcause = 0;
  CSR_READ(CSR_REG_MCAUSE, &mcause);
  if (g_expect_fault) {
    g_fault_count++;
    g_last_mcause = mcause;
    return;
  }
  CHECK(false, "Unexpected load/store access fault (mcause=0x%x)", mcause);
}

static void expect_read32_fault(mmio_region_t base, uint32_t offset) {
  uint32_t prev = g_fault_count;
  g_expect_fault = true;
  (void)mmio_region_read32(base, (ptrdiff_t)offset);
  g_expect_fault = false;
  CHECK(g_fault_count == prev + 1,
        "Expected Load Access Fault at offset 0x%x (fault_count=%u)", offset,
        g_fault_count);
  CHECK(g_last_mcause == kIbexExcLoadAccessFault,
        "Expected mcause=5 at offset 0x%x, got 0x%x", offset, g_last_mcause);
}

static void expect_write32_fault(mmio_region_t base, uint32_t offset,
                                 uint32_t val) {
  uint32_t prev = g_fault_count;
  g_expect_fault = true;
  mmio_region_write32(base, (ptrdiff_t)offset, val);
  g_expect_fault = false;
  CHECK(g_fault_count == prev + 1,
        "Expected Store Access Fault at offset 0x%x (fault_count=%u)", offset,
        g_fault_count);
  CHECK(g_last_mcause == kIbexExcStoreAccessFault,
        "Expected mcause=7 at offset 0x%x, got 0x%x", offset, g_last_mcause);
}

static void expect_write8_fault(mmio_region_t base, uint32_t offset,
                                uint8_t val) {
  uint32_t prev = g_fault_count;
  g_expect_fault = true;
  mmio_region_write8(base, (ptrdiff_t)offset, val);
  g_expect_fault = false;
  CHECK(g_fault_count == prev + 1,
        "Expected Store Access Fault on 8-bit write at offset 0x%x", offset);
  CHECK(g_last_mcause == kIbexExcStoreAccessFault,
        "Expected mcause=7 on 8-bit write at offset 0x%x, got 0x%x", offset,
        g_last_mcause);
}

static void expect_write16_fault(mmio_region_t base, uint32_t offset,
                                 uint16_t val) {
  (void)base;
  uint32_t prev = g_fault_count;
  g_expect_fault = true;
  *(volatile uint16_t *)(kSpiDeviceBase + offset) = val;
  g_expect_fault = false;
  CHECK(g_fault_count == prev + 1,
        "Expected Store Access Fault on 16-bit write at offset 0x%x", offset);
  CHECK(g_last_mcause == kIbexExcStoreAccessFault,
        "Expected mcause=7 on 16-bit write at offset 0x%x, got 0x%x", offset,
        g_last_mcause);
}

static void test_flash_status_csb_deassert_gate(mmio_region_t base) {
  LOG_INFO("Test 1: FLASH_STATUS sys_csb_deasserted_pulse_i readback gate");
  uint32_t init_status =
      mmio_region_read32(base, SPI_DEVICE_FLASH_STATUS_REG_OFFSET);
  CHECK(init_status == 0u, "Expected initial FLASH_STATUS == 0, got 0x%08x",
        init_status);

  // Write non-zero WEL/BUSY/STATUS bits while CSb is high (SCK idle).
  // Per spid_status.sv:308-314, sys_status_o only updates on
  // sys_csb_deasserted_pulse_i (after >= 8 SCK clocks + CSb deassertion).
  mmio_region_write32(base, SPI_DEVICE_FLASH_STATUS_REG_OFFSET, 0x00abcdefu);
  uint32_t readback =
      mmio_region_read32(base, SPI_DEVICE_FLASH_STATUS_REG_OFFSET);
  CHECK(readback == 0u,
        "FLASH_STATUS readback while CSb==1 must remain 0x00000000, got 0x%08x",
        readback);

  // Restore staged sys_status_we value to 0.
  mmio_region_write32(base, SPI_DEVICE_FLASH_STATUS_REG_OFFSET, 0u);
}

static void test_addr_mode_unconditional_pending(mmio_region_t base) {
  LOG_INFO("Test 2: ADDR_MODE unconditional PENDING (bit 31) assertion");
  uint32_t init_mode =
      mmio_region_read32(base, SPI_DEVICE_ADDR_MODE_REG_OFFSET);
  CHECK(init_mode == 0u, "Expected initial ADDR_MODE == 0, got 0x%08x",
        init_mode);

  // Write 0x0 (same as current addr_4b_en == 0). Per spid_addr_4b.sv:52-68,
  // reg2hw_addr_mode_addr_4b_en_qe_i unconditionally sets
  // sys_fw_new_addr_mode_req <= 1'b1 without checking whether q != addr_4b_en.
  mmio_region_write32(base, SPI_DEVICE_ADDR_MODE_REG_OFFSET, 0u);
  uint32_t after_same_write =
      mmio_region_read32(base, SPI_DEVICE_ADDR_MODE_REG_OFFSET);
  CHECK(after_same_write == (1u << SPI_DEVICE_ADDR_MODE_PENDING_BIT),
        "Expected ADDR_MODE == 0x80000000 after redundant write, got 0x%08x",
        after_same_write);

  // Writing 1 while SCK is idle updates the clk_i subreg ADDR_4B_EN to 1
  // ("value to be sent") while holding PENDING == 1 (0x80000001) until the
  // 8th SCK rising edge (cmd_sync_pulse_i).
  mmio_region_write32(base, SPI_DEVICE_ADDR_MODE_REG_OFFSET, 1u);
  uint32_t after_toggle_write =
      mmio_region_read32(base, SPI_DEVICE_ADDR_MODE_REG_OFFSET);
  CHECK(after_toggle_write == ((1u << SPI_DEVICE_ADDR_MODE_PENDING_BIT) | 1u),
        "Expected ADDR_MODE == 0x80000001 while SCK idle, got 0x%08x",
        after_toggle_write);

  // Restore staged sys_fw_new_addr_mode to 0.
  mmio_region_write32(base, SPI_DEVICE_ADDR_MODE_REG_OFFSET, 0u);
  uint32_t after_restore_write =
      mmio_region_read32(base, SPI_DEVICE_ADDR_MODE_REG_OFFSET);
  CHECK(after_restore_write == (1u << SPI_DEVICE_ADDR_MODE_PENDING_BIT),
        "Expected ADDR_MODE == 0x80000000 after restoring 0, got 0x%08x",
        after_restore_write);
}

static void test_sram_window_holes_and_restrictions(mmio_region_t base) {
  LOG_INFO("Test 3 & 4: SRAM window holes and adapter access restrictions");

  // Last valid word of egress_buffer (0x1d3c) accepts 32-bit writes.
  uint32_t prev_faults = g_fault_count;
  mmio_region_write32(base, 0x1d3c, 0xa5a55a5au);
  mmio_region_write32(base, SPI_DEVICE_EGRESS_BUFFER_REG_OFFSET, 0x12345678u);
  CHECK(g_fault_count == prev_faults,
        "Valid 32-bit writes to egress_buffer (0x1000, 0x1d3c) must succeed");

  // Unmapped 192 B hole [0x1d40..0x1dff] routes to u_reg_if (addrmiss=1).
  expect_read32_fault(base, kEgressHoleStart);
  expect_write32_fault(base, kEgressHoleStart, 0u);
  expect_read32_fault(base, kEgressHoleEnd);
  expect_write32_fault(base, kEgressHoleEnd, 0u);

  // Unmapped 64 B tail [0x1fc0..0x1fff] routes to u_reg_if (addrmiss=1).
  expect_read32_fault(base, kIngressTailStart);
  expect_write32_fault(base, kIngressTailStart, 0u);
  expect_read32_fault(base, kIngressTailEnd);
  expect_write32_fault(base, kIngressTailEnd, 0u);

  // u_tlul2sram_egress has .ByteAccess(0) and .ErrOnRead(1).
  expect_write8_fault(base, SPI_DEVICE_EGRESS_BUFFER_REG_OFFSET, 0x11u);
  expect_write16_fault(base, SPI_DEVICE_EGRESS_BUFFER_REG_OFFSET, 0x2233u);
  expect_read32_fault(base, SPI_DEVICE_EGRESS_BUFFER_REG_OFFSET);
  expect_read32_fault(base, 0x1c00u);  // SFDP egress_buffer region

  // u_tlul2sram_ingress has .ErrOnWrite(1).
  expect_write32_fault(base, kIngressStart, 0xdeadbeefu);
  expect_write32_fault(base, kIngressCmdFifoStart, 0xdeadbeefu);
  expect_write32_fault(base, kIngressAddrFifoStart, 0xdeadbeefu);
  expect_write32_fault(base, kIngressTpmWriteFifoStart, 0xdeadbeefu);
}

static void test_csr_subword_permit_masks(mmio_region_t base) {
  LOG_INFO("Test 5: Sub-word CSR write faults via SPI_DEVICE_PERMIT");
  uint32_t prev_faults = g_fault_count;

  // Per spi_device_reg_top.sv:19606-19679 and spi_device_reg_pkg.sv:845-919,
  // wr_err = |(SPI_DEVICE_PERMIT[i] & ~reg_be). FLASH_STATUS (0x28, index 10)
  // has SPI_DEVICE_PERMIT[10] = 4'b0111: any 8-bit (reg_be=4'b0001) or 16-bit
  // (reg_be=4'b0011) write leaves byte 2 uncovered and raises a synchronous
  // Store Access Fault (mcause=7), whereas a 32-bit write (reg_be=4'b1111)
  // succeeds.
  expect_write8_fault(base, SPI_DEVICE_FLASH_STATUS_REG_OFFSET, 0x00u);
  expect_write16_fault(base, SPI_DEVICE_FLASH_STATUS_REG_OFFSET, 0x0000u);

  // TPM_ACCESS_1 (0x810, index 63) has SPI_DEVICE_PERMIT[63] = 4'b0001 (1 byte
  // for locality 4), so an 8-bit write to byte 0 (0x810, reg_be=4'b0001)
  // succeeds, whereas an 8-bit write to byte 1 (0x811, reg_be=4'b0010) or an
  // 8-bit / 16-bit write to byte 0 of TPM_ACCESS_0 (0x80c, index 62,
  // SPI_DEVICE_PERMIT[62] = 4'b1111) faults with mcause=7.
  prev_faults = g_fault_count;
  mmio_region_write8(base, SPI_DEVICE_TPM_ACCESS_1_REG_OFFSET, 0x00u);
  CHECK(g_fault_count == prev_faults,
        "8-bit write to TPM_ACCESS_1 byte 0 (PERMIT=4'b0001) must succeed");
  expect_write8_fault(base, SPI_DEVICE_TPM_ACCESS_1_REG_OFFSET + 1u, 0x00u);
  expect_write8_fault(base, SPI_DEVICE_TPM_ACCESS_0_REG_OFFSET, 0x00u);
  expect_write16_fault(base, SPI_DEVICE_TPM_ACCESS_0_REG_OFFSET, 0x0000u);
  expect_write8_fault(base, SPI_DEVICE_CMD_INFO_WRDI_REG_OFFSET, 0x00u);
  expect_write16_fault(base, SPI_DEVICE_CMD_INFO_WRDI_REG_OFFSET, 0x0000u);
  expect_write8_fault(base, SPI_DEVICE_TPM_CAP_REG_OFFSET, 0x00u);
  expect_write16_fault(base, SPI_DEVICE_TPM_CAP_REG_OFFSET, 0x0000u);
}

static void test_dif_get_flash_command_slot_swap_bug(mmio_region_t base) {
  LOG_INFO(
      "Test 6: dif_spi_device_get_flash_command_slot passthrough_swap_address "
      "copy-paste bug (dif_spi_device.c:550-554)");

  dif_spi_device_handle_t spi;
  CHECK_DIF_OK(dif_spi_device_init_handle(base, &spi));

  // Case A: passthrough_swap_address = true, payload_swap_enable = false.
  // dif_spi_device_set_flash_command_slot writes bit 10 (ADDR_SWAP_EN_0 = 1)
  // and bit 21 (PAYLOAD_SWAP_EN_0 = 0).
  dif_spi_device_flash_command_t cmd_a = {
      .opcode = 0x13u,
      .address_type = kDifSpiDeviceFlashAddr3Byte,
      .dummy_cycles = 0u,
      .payload_io_type = kDifSpiDevicePayloadIoSingle,
      .passthrough_swap_address = true,
      .payload_dir_to_host = false,
      .payload_swap_enable = false,
      .read_pipeline_mode = kDifSpiDeviceReadPipelineModeZeroStages,
      .upload = false,
      .set_busy_status = false,
  };
  CHECK_DIF_OK(dif_spi_device_set_flash_command_slot(&spi, /*slot=*/0,
                                                     kDifToggleEnabled, cmd_a));

  uint32_t raw_a = mmio_region_read32(base, SPI_DEVICE_CMD_INFO_0_REG_OFFSET);
  CHECK(((raw_a >> SPI_DEVICE_CMD_INFO_0_ADDR_SWAP_EN_0_BIT) & 1u) == 1u,
        "Expected hardware ADDR_SWAP_EN_0 (bit 10) == 1, raw=0x%08x", raw_a);
  CHECK(((raw_a >> SPI_DEVICE_CMD_INFO_0_PAYLOAD_SWAP_EN_0_BIT) & 1u) == 0u,
        "Expected hardware PAYLOAD_SWAP_EN_0 (bit 21) == 0, raw=0x%08x", raw_a);

  dif_toggle_t enabled = kDifToggleDisabled;
  dif_spi_device_flash_command_t readback_a = {0};
  CHECK_DIF_OK(dif_spi_device_get_flash_command_slot(&spi, /*slot=*/0, &enabled,
                                                     &readback_a));
  CHECK(enabled == kDifToggleEnabled, "Expected slot 0 enabled");
  // Because dif_spi_device.c:550-551 reads PAYLOAD_SWAP_EN_0_BIT (bit 21)
  // instead of ADDR_SWAP_EN_0_BIT (bit 10), passthrough_swap_address reads back
  // as false even though bit 10 is 1 in hardware!
  CHECK(!readback_a.passthrough_swap_address,
        "Expected dif_spi_device_get_flash_command_slot to return false for "
        "passthrough_swap_address when PAYLOAD_SWAP_EN_0 == 0");
  CHECK(!readback_a.payload_swap_enable,
        "Expected payload_swap_enable == false");

  // Case B: passthrough_swap_address = false, payload_swap_enable = true.
  dif_spi_device_flash_command_t cmd_b = cmd_a;
  cmd_b.passthrough_swap_address = false;
  cmd_b.payload_swap_enable = true;
  CHECK_DIF_OK(dif_spi_device_set_flash_command_slot(&spi, /*slot=*/0,
                                                     kDifToggleEnabled, cmd_b));

  uint32_t raw_b = mmio_region_read32(base, SPI_DEVICE_CMD_INFO_0_REG_OFFSET);
  CHECK(((raw_b >> SPI_DEVICE_CMD_INFO_0_ADDR_SWAP_EN_0_BIT) & 1u) == 0u,
        "Expected hardware ADDR_SWAP_EN_0 (bit 10) == 0, raw=0x%08x", raw_b);
  CHECK(((raw_b >> SPI_DEVICE_CMD_INFO_0_PAYLOAD_SWAP_EN_0_BIT) & 1u) == 1u,
        "Expected hardware PAYLOAD_SWAP_EN_0 (bit 21) == 1, raw=0x%08x", raw_b);

  dif_spi_device_flash_command_t readback_b = {0};
  CHECK_DIF_OK(dif_spi_device_get_flash_command_slot(&spi, /*slot=*/0, &enabled,
                                                     &readback_b));
  CHECK(readback_b.passthrough_swap_address,
        "Expected dif_spi_device_get_flash_command_slot to return true for "
        "passthrough_swap_address when PAYLOAD_SWAP_EN_0 == 1");
  CHECK(readback_b.payload_swap_enable, "Expected payload_swap_enable == true");

  // Disable slot 0 to restore clean state.
  CHECK_DIF_OK(dif_spi_device_set_flash_command_slot(
      &spi, /*slot=*/0, kDifToggleDisabled, cmd_a));
}

static void test_sram_1r1w_ingress_uninitialized_parity_faults(
    mmio_region_t base) {
  LOG_INFO(
      "Test 7: DefaultSramType=SramType1r1w u_spi2sys_mem uninitialized "
      "odd-parity synchronous Load Access Faults (mcause=5)");

  // In trunk-v2 (spi_device_pkg.sv:411, spid_dpram.sv:217-245), u_spi2sys_mem
  // is a dedicated 128x36-bit prim_ram_1r1w_async_adv with .EnableParity(1)
  // (odd parity via prim_secded_inv_hamming_39_32_dec) whose write port is
  // driven exclusively by clk_spi_i (spi2sys_wr_req).
  //
  // During ROM SPI flash bootstrap, the external SPI host uploads >80
  // PAGE_PROGRAM (256 B) commands, which populates all 64 words of the Payload
  // Buffer (0x1e00..0x1eff), all 16 words of CmdFIFO (0x1f00..0x1f3f), and all
  // 16 words of AddrFIFO (0x1f40..0x1f7f) with valid odd-parity bits via
  // clk_spi_i. Consequently, reading 0x1e00..0x1f7f succeeds without fault:
  uint32_t prev_faults = g_fault_count;
  (void)mmio_region_read32(base, kIngressStart);          // 0x1e00 Payload FIFO
  (void)mmio_region_read32(base, kIngressCmdFifoStart);   // 0x1f00 Command FIFO
  (void)mmio_region_read32(base, kIngressAddrFifoStart);  // 0x1f40 Address FIFO
  CHECK(g_fault_count == prev_faults,
        "Reads from bootstrap-populated ingress_buffer (0x1e00..0x1f7f) must "
        "succeed with valid odd parity");

  // By contrast, the 16-word TPM Write FIFO region (0x1f80..0x1fbf, words
  // 96..111 of u_spi2sys_mem) is never written by ROM SPI flash bootstrap and
  // cannot be initialized by software (u_tlul2sram_ingress has .ErrOnWrite(1)).
  // Every word in 0x1f80..0x1fbf therefore retains uninitialized all-zero BRAM
  // bits (36'h0 -> syndrome 7'h78 != 0 -> rerror_o[1]=1 -> d_error=1) and
  // raises a synchronous Load Access Fault (mcause=5) on CPU read!
  for (uint32_t i = 0; i < 16u; ++i) {
    expect_read32_fault(base, kIngressTpmWriteFifoStart + i * sizeof(uint32_t));
  }
}

static const uint8_t kSpiDevicePermit[73] = {
    // Main CSR window (0x000..0x0ec, indices 0..58):
    0x1u,  // [ 0] 0x000 INTR_STATE
    0x1u,  // [ 1] 0x004 INTR_ENABLE
    0x1u,  // [ 2] 0x008 INTR_TEST
    0x1u,  // [ 3] 0x00c ALERT_TEST
    0x1u,  // [ 4] 0x010 CONTROL
    0xfu,  // [ 5] 0x014 CFG
    0x1u,  // [ 6] 0x018 STATUS
    0x1u,  // [ 7] 0x01c INTERCEPT_EN
    0xfu,  // [ 8] 0x020 ADDR_MODE
    0xfu,  // [ 9] 0x024 LAST_READ_ADDR
    0x7u,  // [10] 0x028 FLASH_STATUS
    0x3u,  // [11] 0x02c JEDEC_CC
    0x7u,  // [12] 0x030 JEDEC_ID
    0x3u,  // [13] 0x034 READ_THRESHOLD
    0xfu,  // [14] 0x038 MAILBOX_ADDR
    0x3u,  // [15] 0x03c UPLOAD_STATUS
    0x7u,  // [16] 0x040 UPLOAD_STATUS2
    0x3u,  // [17] 0x044 UPLOAD_CMDFIFO
    0xfu,  // [18] 0x048 UPLOAD_ADDRFIFO
    0xfu,
    0xfu,
    0xfu,
    0xfu,
    0xfu,
    0xfu,
    0xfu,
    0xfu,  // [19..26] 0x04c..0x068 CMD_FILTER_0..7
    0xfu,  // [27] 0x06c ADDR_SWAP_MASK
    0xfu,  // [28] 0x070 ADDR_SWAP_DATA
    0xfu,  // [29] 0x074 PAYLOAD_SWAP_MASK
    0xfu,  // [30] 0x078 PAYLOAD_SWAP_DATA
    0xfu,
    0xfu,
    0xfu,
    0xfu,
    0xfu,
    0xfu,
    0xfu,
    0xfu,  // [31..38] 0x07c..0x098 CMD_INFO_0..7
    0xfu,
    0xfu,
    0xfu,
    0xfu,
    0xfu,
    0xfu,
    0xfu,
    0xfu,  // [39..46] 0x09c..0x0b8 CMD_INFO_8..15
    0xfu,
    0xfu,
    0xfu,
    0xfu,
    0xfu,
    0xfu,
    0xfu,
    0xfu,  // [47..54] 0x0bc..0x0d8 CMD_INFO_16..23
    0xfu,  // [55] 0x0dc CMD_INFO_EN4B
    0xfu,  // [56] 0x0e0 CMD_INFO_EX4B
    0xfu,  // [57] 0x0e4 CMD_INFO_WREN
    0xfu,  // [58] 0x0e8 CMD_INFO_WRDI
    // TPM CSR window (0x800..0x834, indices 59..72):
    0x7u,  // [59] 0x800 TPM_CAP
    0x1u,  // [60] 0x804 TPM_CFG
    0x1u,  // [61] 0x808 TPM_STATUS
    0xfu,  // [62] 0x80c TPM_ACCESS_0
    0x1u,  // [63] 0x810 TPM_ACCESS_1
    0xfu,  // [64] 0x814 TPM_STS
    0xfu,  // [65] 0x818 TPM_INTF_CAPABILITY
    0xfu,  // [66] 0x81c TPM_INT_ENABLE
    0x1u,  // [67] 0x820 TPM_INT_VECTOR
    0xfu,  // [68] 0x824 TPM_INT_STATUS
    0xfu,  // [69] 0x828 TPM_DID_VID
    0x1u,  // [70] 0x82c TPM_RID
    0xfu,  // [71] 0x830 TPM_CMD_ADDR
    0xfu,  // [72] 0x834 TPM_READ_FIFO
};

static void test_all_73_csrs_subword_permit_and_readback(mmio_region_t base) {
  LOG_INFO("Test 8: All 73 CSRs SPI_DEVICE_PERMIT sub-word access & readbacks");

  // Verify hardware reset / post-bootstrap readbacks of key Main and TPM CSRs:
  uint32_t tpm_cap = mmio_region_read32(base, SPI_DEVICE_TPM_CAP_REG_OFFSET);
  CHECK(tpm_cap == 0x00660100u,
        "Expected TPM_CAP == 0x00660100 (max_rd=6, max_wr=6, loc=1, rev=0), "
        "got 0x%08x",
        tpm_cap);
  uint32_t jedec_cc = mmio_region_read32(base, SPI_DEVICE_JEDEC_CC_REG_OFFSET);
  CHECK(jedec_cc == 0x0000007fu,
        "Expected JEDEC_CC == 0x0000007f (cc=0x7f, num_cc=0), got 0x%08x",
        jedec_cc);
  uint32_t ctrl = mmio_region_read32(base, SPI_DEVICE_CONTROL_REG_OFFSET);
  CHECK(ctrl == 0x00000010u,
        "Expected CONTROL == 0x00000010 (MODE[5:4] == FlashMode=1), got 0x%08x",
        ctrl);

  // Verify out-of-bounds CSR offsets immediately after Main (0x0f0) and TPM
  // (0x838) windows fault with Load/Store Access Fault (mcause = 5 / 7):
  expect_read32_fault(base, 0x0f0u);
  expect_write32_fault(base, 0x0f0u, 0u);
  expect_read32_fault(base, 0x838u);
  expect_write32_fault(base, 0x838u, 0u);

  // Exhaustively verify all 73 CSRs (indices 0..58 at 0x000..0x0ec, and
  // indices 59..72 at 0x800..0x834) against SPI_DEVICE_PERMIT[73]:
  for (uint32_t i = 0; i < 73u; ++i) {
    uint32_t off = (i < 59u) ? (i * 4u) : (0x800u + (i - 59u) * 4u);
    uint8_t permit = kSpiDevicePermit[i];

    // Save current value for all non-FIFO readable CSRs (avoid popping
    // UPLOAD_CMDFIFO=0x44, UPLOAD_ADDRFIFO=0x48, TPM_CMD_ADDR=0x830).
    uint32_t orig = 0u;
    bool restore = (off != SPI_DEVICE_UPLOAD_CMDFIFO_REG_OFFSET &&
                    off != SPI_DEVICE_UPLOAD_ADDRFIFO_REG_OFFSET &&
                    off != SPI_DEVICE_TPM_CMD_ADDR_REG_OFFSET &&
                    off != SPI_DEVICE_TPM_READ_FIFO_REG_OFFSET &&
                    off != SPI_DEVICE_INTR_TEST_REG_OFFSET &&
                    off != SPI_DEVICE_ALERT_TEST_REG_OFFSET);
    if (restore) {
      orig = mmio_region_read32(base, (ptrdiff_t)off);
    }

    if (permit == 0x1u) {
      // 4'b0001: 8-bit write at byte +0 succeeds; 8-bit write at byte +1
      // faults.
      uint8_t safe_b0 =
          (off == SPI_DEVICE_CONTROL_REG_OFFSET) ? (uint8_t)orig : 0x00u;
      uint32_t prev = g_fault_count;
      mmio_region_write8(base, (ptrdiff_t)off, safe_b0);
      CHECK(g_fault_count == prev,
            "8-bit write at byte +0 of permit=0x1 CSR 0x%03x (idx %u) must "
            "succeed",
            off, i);
      expect_write8_fault(base, off + 1u, 0x00u);
    } else if (permit == 0x3u) {
      // 4'b0011: 8-bit write at byte +0 faults; 16-bit write at +0 succeeds.
      expect_write8_fault(base, off, 0x00u);
      uint32_t prev = g_fault_count;
      *(volatile uint16_t *)(kSpiDeviceBase + off) = (uint16_t)(orig & 0xffffu);
      CHECK(g_fault_count == prev,
            "16-bit write at permit=0x3 CSR 0x%03x (idx %u) must succeed", off,
            i);
    } else {
      // 4'b0111 (0x7) or 4'b1111 (0xf): both 8-bit and 16-bit writes at +0
      // fault!
      expect_write8_fault(base, off, 0x00u);
      expect_write16_fault(base, off, 0x0000u);
    }

    if (restore && off != SPI_DEVICE_ADDR_MODE_REG_OFFSET &&
        off != SPI_DEVICE_FLASH_STATUS_REG_OFFSET) {
      mmio_region_write32(base, (ptrdiff_t)off, orig);
    }
  }
}

static void test_spi_device_intr_and_alert_test(mmio_region_t base) {
  LOG_INFO(
      "Test 9: INTR_STATE/INTR_ENABLE/INTR_TEST (7 Event + 1 Status) & "
      "ALERT_TEST");

  // 1. Verify INTR_ENABLE (0x04) 8-bit writable mask (0xff) while keeping
  //    PLIC interrupts masked before testing INTR_TEST.
  mmio_region_write32(base, SPI_DEVICE_INTR_ENABLE_REG_OFFSET, 0u);
  CHECK(mmio_region_read32(base, SPI_DEVICE_INTR_ENABLE_REG_OFFSET) == 0u,
        "Expected INTR_ENABLE == 0");
  mmio_region_write32(base, SPI_DEVICE_INTR_ENABLE_REG_OFFSET, 0xffffffffu);
  CHECK(mmio_region_read32(base, SPI_DEVICE_INTR_ENABLE_REG_OFFSET) == 0xffu,
        "Expected INTR_ENABLE 8-bit mask 0xff");
  mmio_region_write32(base, SPI_DEVICE_INTR_ENABLE_REG_OFFSET, 0u);

  // Clear any prior pending bits in INTR_TEST (0x08) and INTR_STATE (0x00).
  mmio_region_write32(base, SPI_DEVICE_INTR_TEST_REG_OFFSET, 0u);
  mmio_region_write32(base, SPI_DEVICE_INTR_STATE_REG_OFFSET, 0xffu);
  CHECK(mmio_region_read32(base, SPI_DEVICE_INTR_STATE_REG_OFFSET) == 0u,
        "Expected INTR_STATE == 0 after clearing");

  // 2. Verify all 7 Event-type interrupts (0xdf: bits 0,1,2,3,4,6,7 in
  //    spi_device.sv:465-571) latch via INTR_TEST (WO, readback 0) and clear
  //    via RW1C in INTR_STATE:
  mmio_region_write32(base, SPI_DEVICE_INTR_TEST_REG_OFFSET, 0xdfu);
  CHECK(mmio_region_read32(base, SPI_DEVICE_INTR_TEST_REG_OFFSET) == 0u,
        "Expected INTR_TEST (WO) to read back 0");
  CHECK(mmio_region_read32(base, SPI_DEVICE_INTR_STATE_REG_OFFSET) == 0xdfu,
        "Expected INTR_TEST=0xdf to set all 7 Event bits in INTR_STATE");
  mmio_region_write32(base, SPI_DEVICE_INTR_STATE_REG_OFFSET, 0xdfu);
  CHECK(mmio_region_read32(base, SPI_DEVICE_INTR_STATE_REG_OFFSET) == 0u,
        "Expected RW1C write 0xdf to clear all 7 Event bits in INTR_STATE");

  // 3. Verify bit 5 (`tpm_header_not_empty`, 0x20), which is instantiated with
  //    `prim_intr_hw #(.IntrT("Status"))` (`spi_device.sv:525-539`):
  //    - Writing 0x20 to INTR_TEST latches test_q[5] = 1 -> INTR_STATE = 0x20.
  //    - Writing 0x20 to INTR_STATE does NOT clear INTR_STATE while test_q[5]
  //    == 1.
  //    - Writing 0x00 to INTR_TEST clears test_q[5] = 0 -> INTR_STATE = 0x00.
  mmio_region_write32(base, SPI_DEVICE_INTR_TEST_REG_OFFSET, 0x20u);
  CHECK(mmio_region_read32(base, SPI_DEVICE_INTR_STATE_REG_OFFSET) == 0x20u,
        "Expected INTR_TEST=0x20 to set Status-type tpm_header_not_empty (bit "
        "5)");
  mmio_region_write32(base, SPI_DEVICE_INTR_STATE_REG_OFFSET, 0x20u);
  CHECK(mmio_region_read32(base, SPI_DEVICE_INTR_STATE_REG_OFFSET) == 0x20u,
        "Expected Status-type bit 5 in INTR_STATE to stay 1 while test_q == 1");
  mmio_region_write32(base, SPI_DEVICE_INTR_TEST_REG_OFFSET, 0x00u);
  CHECK(
      mmio_region_read32(base, SPI_DEVICE_INTR_STATE_REG_OFFSET) == 0u,
      "Expected writing INTR_TEST=0 to clear Status-type bit 5 in INTR_STATE");

  // 4. Verify ALERT_TEST (0x0c, WO, readback 0) transient pulse on
  //    kTopEarlgreyAlertIdSpiDeviceFatalFault:
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_start(kTopEarlgreyAlertIdSpiDeviceFatalFault));
  mmio_region_write32(base, SPI_DEVICE_ALERT_TEST_REG_OFFSET, 1u);
  CHECK(mmio_region_read32(base, SPI_DEVICE_ALERT_TEST_REG_OFFSET) == 0u,
        "Expected ALERT_TEST (WO) to read back 0");
  CHECK_STATUS_OK(
      ottf_alerts_expect_alert_finish(kTopEarlgreyAlertIdSpiDeviceFatalFault));
}

bool test_main(void) {
  mmio_region_t base = mmio_region_from_addr(kSpiDeviceBase);

  test_flash_status_csb_deassert_gate(base);
  test_addr_mode_unconditional_pending(base);
  test_sram_window_holes_and_restrictions(base);
  test_csr_subword_permit_masks(base);
  test_dif_get_flash_command_slot_swap_bug(base);
  test_sram_1r1w_ingress_uninitialized_parity_faults(base);
  test_all_73_csrs_subword_permit_and_readback(base);
  test_spi_device_intr_and_alert_test(base);

  LOG_INFO("All spi_device Earlgrey v2 errata checks passed (faults=%u)",
           g_fault_count);
  return true;
}
