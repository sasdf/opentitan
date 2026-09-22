// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_alert_handler.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "spi_device_regs.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kSpiDeviceBase = TOP_EARLGREY_SPI_DEVICE_BASE_ADDR,
  kAlertHandlerBase = TOP_EARLGREY_ALERT_HANDLER_BASE_ADDR,
};

static dif_alert_handler_t alert_handler;
static uint32_t g_failures = 0;
static volatile bool g_expect_access_fault = false;
static volatile uint32_t g_access_fault_count = 0;
static volatile uint32_t g_last_mcause = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  if (g_expect_access_fault) {
    g_last_mcause = ibex_mcause_read();
    ++g_access_fault_count;
    return;
  }
  ottf_generic_fault_print(exc_info, "Unhandled Load/Store Fault",
                           ibex_mcause_read());
  abort();
}

#define EXPECT_CHECK(cond, ...) \
  do {                          \
    if (!(cond)) {              \
      LOG_ERROR(__VA_ARGS__);   \
      ++g_failures;             \
    }                           \
  } while (0)

/**
 * Check 1: `SPI_DEVICE.ALERT_TEST` (`spi_device.sv:1877-1896` vs
 * `ot_spi_device.c:829-836, 2235-2239`) must pulse `alert_tx_o` (`1 -> 0`)
 * rather than latching `s->alerts[0]` high forever.
 */
static void test_alert_test_pulse(void) {
  const dif_alert_handler_alert_t kAlert =
      kTopEarlgreyAlertIdSpiDeviceFatalFault;

  CHECK_DIF_OK(dif_alert_handler_configure_alert(
      &alert_handler, kAlert, kDifAlertHandlerClassA,
      /*enabled=*/kDifToggleEnabled, /*locked=*/kDifToggleDisabled));

  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_ALERT_TEST_REG_OFFSET,
                   1u << SPI_DEVICE_ALERT_TEST_FATAL_FAULT_BIT);
  busy_spin_micros(10);

  bool is_cause = false;
  CHECK_DIF_OK(
      dif_alert_handler_alert_is_cause(&alert_handler, kAlert, &is_cause));
  EXPECT_CHECK(is_cause, "SPI_DEVICE ALERT_TEST should set ALERT_CAUSE");

  CHECK_DIF_OK(dif_alert_handler_alert_acknowledge(&alert_handler, kAlert));
  busy_spin_micros(10);

  is_cause = false;
  CHECK_DIF_OK(
      dif_alert_handler_alert_is_cause(&alert_handler, kAlert, &is_cause));
  EXPECT_CHECK(
      !is_cause,
      "SPI_DEVICE ALERT_CAUSE should remain 0 after clearing "
      "(ALERT_TEST is a single-cycle pulse in spi_device.sv:1877-1896)");
}

/**
 * Check 2: `INTR_TEST.TPM_HEADER_NOT_EMPTY` (`IntrT("Status")` in
 * `spi_device.sv:478-492` and `prim_intr_hw.sv:71-84`).
 * Writing 1 to `INTR_TEST.TPM_HEADER_NOT_EMPTY` (bit 5) latches `test_q = 1`
 * inside `prim_intr_hw` (`Status` mode), which drives
 * `hw2reg.intr_state.tpm_header_not_empty.d = event_intr_i | test_q = 1`,
 * so `INTR_STATE` must read `0x20`, and writing 0 to `INTR_TEST` clears
 * `test_q = 0` so `INTR_STATE` returns `0x0`.
 */
static void test_intr_test_tpm_header_not_empty_status_readback(void) {
  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_INTR_ENABLE_REG_OFFSET, 0u);
  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_INTR_TEST_REG_OFFSET, 0u);
  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_INTR_STATE_REG_OFFSET, 0xffu);

  const uint32_t kTpmHdrBit = 1u
                              << SPI_DEVICE_INTR_STATE_TPM_HEADER_NOT_EMPTY_BIT;
  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_INTR_TEST_REG_OFFSET,
                   kTpmHdrBit);
  uint32_t intr_state =
      abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_INTR_STATE_REG_OFFSET);
  EXPECT_CHECK((intr_state & kTpmHdrBit) != 0u,
               "Writing 1 to INTR_TEST.TPM_HEADER_NOT_EMPTY must set "
               "INTR_STATE.TPM_HEADER_NOT_EMPTY (prim_intr_hw Status test_q), "
               "got INTR_STATE=0x%02x",
               intr_state);

  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_INTR_TEST_REG_OFFSET, 0u);
  intr_state =
      abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_INTR_STATE_REG_OFFSET);
  EXPECT_CHECK((intr_state & kTpmHdrBit) == 0u,
               "Writing 0 to INTR_TEST.TPM_HEADER_NOT_EMPTY must clear "
               "INTR_STATE.TPM_HEADER_NOT_EMPTY, got INTR_STATE=0x%02x",
               intr_state);
}

/**
 * Check 3: `CMD_INFO_0..23` include `READ_PIPELINE_MODE` at bits [23:22]
 * (`spi_device.hjson:1111-1152`, `spi_device.sv:595`), so the RW mask is
 * `0x83ffffff` (whereas `ot_spi_device.c:633-639` omits
 * `CMD_INFO_READ_PIPELINE_MODE_MASK` and masks with `0x833fffff`).
 */
static void test_cmd_info_read_pipeline_mode_mask(void) {
  uint32_t orig =
      abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_CMD_INFO_0_REG_OFFSET);
  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_CMD_INFO_0_REG_OFFSET,
                   0xffffffffu);
  uint32_t readback =
      abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_CMD_INFO_0_REG_OFFSET);
  EXPECT_CHECK(readback == 0x83ffffffu,
               "CMD_INFO_0 RW mask must include READ_PIPELINE_MODE [23:22] "
               "(expected 0x83ffffff, got 0x%08x)",
               readback);
  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_CMD_INFO_0_REG_OFFSET, orig);
}

/**
 * Check 4: `ADDR_MODE` (`spid_addr_4b.sv:52-94` vs
 * `ot_spi_device.c:2301-2304`). Writing `ADDR_MODE` from software sets
 * `sys_fw_new_addr_mode_req <= 1'b1`, asserting bit 31 (`PENDING = 1`) until
 * the 8th SPI clock edge of the next SPI command.
 */
static void test_addr_mode_pending_on_sw_write(void) {
  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_ADDR_MODE_REG_OFFSET, 1u);
  uint32_t addr_mode =
      abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_ADDR_MODE_REG_OFFSET);
  EXPECT_CHECK(
      addr_mode == 0x80000001u,
      "Writing 1 to ADDR_MODE while SPI is idle must set PENDING (bit 31) "
      "and ADDR_4B_EN (bit 0) -> 0x80000001, got 0x%08x",
      addr_mode);

  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_ADDR_MODE_REG_OFFSET, 0u);
  addr_mode = abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_ADDR_MODE_REG_OFFSET);
  EXPECT_CHECK(
      addr_mode == 0x80000000u,
      "Writing 0 to ADDR_MODE while SPI is idle must set PENDING (bit 31) "
      "with ADDR_4B_EN=0 -> 0x80000000, got 0x%08x",
      addr_mode);
}

/**
 * Check 5: `FLASH_STATUS` (`spid_status.sv:280-314` vs
 * `ot_spi_device.c:2305-2317`).
 * Software writes to `FLASH_STATUS` are queued into the async FIFO
 * `u_sw_status_update_sync` and only update `sys_status_o` (`FLASH_STATUS`
 * readback) when `sys_csb_deasserted_pulse_i` fires at the end of a SPI
 * transaction. While SPI is idle, `FLASH_STATUS` readback remains 0.
 */
static void test_flash_status_committed_readback_while_idle(void) {
  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_FLASH_STATUS_REG_OFFSET,
                   0x00a5a504u);
  uint32_t flash_status =
      abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_FLASH_STATUS_REG_OFFSET);
  EXPECT_CHECK(
      flash_status == 0u,
      "FLASH_STATUS readback reflects sys_status_o (committed at CSb "
      "de-assertion in spid_status.sv:308-314) and must remain 0 while SPI is "
      "idle, got 0x%08x",
      flash_status);
  // Clear the status update FIFO via CONTROL.FLASH_STATUS_FIFO_CLR (bit 0).
  uint32_t ctrl =
      abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_CONTROL_REG_OFFSET);
  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_CONTROL_REG_OFFSET,
                   ctrl | (1u << SPI_DEVICE_CONTROL_FLASH_STATUS_FIFO_CLR_BIT));
}

/**
 * Check 6: TPM register bitmasks (`spi_device.hjson:1311-1514` vs
 * `ot_spi_device.c:2465-2476`):
 * - `TPM_CFG` (`0x804`): 5 bits `[4:0]` (`0x1f` mask)
 * - `TPM_ACCESS_1` (`0x810`): 8 bits `[7:0]` (`0xff` mask)
 * - `TPM_INT_VECTOR` (`0x820`): 8 bits `[7:0]` (`0xff` mask)
 * - `TPM_RID` (`0x82c`): 8 bits `[7:0]` (`0xff` mask)
 */
static void test_tpm_register_bitmasks(void) {
  // Keep EN (bit 0) = 0 while testing upper bits of TPM_CFG.
  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_TPM_CFG_REG_OFFSET, 0xfffffffeu);
  uint32_t tpm_cfg =
      abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_TPM_CFG_REG_OFFSET);
  EXPECT_CHECK(tpm_cfg == 0x0000001eu,
               "TPM_CFG bits [31:5] are reserved (expected 0x0000001e, "
               "got 0x%08x)",
               tpm_cfg);
  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_TPM_CFG_REG_OFFSET, 0u);

  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_TPM_ACCESS_1_REG_OFFSET,
                   0xffffffffu);
  uint32_t tpm_access_1 =
      abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_TPM_ACCESS_1_REG_OFFSET);
  EXPECT_CHECK(tpm_access_1 == 0x000000ffu,
               "TPM_ACCESS_1 bits [31:8] are reserved (expected 0x000000ff, "
               "got 0x%08x)",
               tpm_access_1);
  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_TPM_ACCESS_1_REG_OFFSET, 0u);

  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_TPM_INT_VECTOR_REG_OFFSET,
                   0xffffffffu);
  uint32_t tpm_int_vec =
      abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_TPM_INT_VECTOR_REG_OFFSET);
  EXPECT_CHECK(tpm_int_vec == 0x000000ffu,
               "TPM_INT_VECTOR bits [31:8] are reserved (expected 0x000000ff, "
               "got 0x%08x)",
               tpm_int_vec);
  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_TPM_INT_VECTOR_REG_OFFSET, 0u);

  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_TPM_RID_REG_OFFSET, 0xffffffffu);
  uint32_t tpm_rid =
      abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_TPM_RID_REG_OFFSET);
  EXPECT_CHECK(tpm_rid == 0x000000ffu,
               "TPM_RID bits [31:8] are reserved (expected 0x000000ff, "
               "got 0x%08x)",
               tpm_rid);
  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_TPM_RID_REG_OFFSET, 0u);
}

/**
 * Wave 2 Check 7 (W2-1): Egress SRAM `ByteAccess(0)` sub-word write fault
 * (`spi_device.sv:1660`, `tlul_adapter_sram.sv:144-147` vs
 * `ot_spi_device.c:2567-2601, 3122`).
 *
 * In RTL, `u_tlul2sram_egress` is instantiated with `.ByteAccess(0)`. Any
 * partial write (`a_mask != 4'b1111 || a_size != 2'h2`) asserts
 * `wr_attr_error = 1'b1` (`tl_o.d_error = 1'b1`), raising a synchronous Store
 * Access Fault (`mcause = 7`).
 */
static void test_w2_egress_sram_subword_write_fault(void) {
  g_access_fault_count = 0;
  g_last_mcause = 0;
  g_expect_access_fault = true;
  abs_mmio_write8(kSpiDeviceBase + SPI_DEVICE_EGRESS_BUFFER_REG_OFFSET, 0x5au);
  g_expect_access_fault = false;

  EXPECT_CHECK(g_access_fault_count == 1u && g_last_mcause == 7u,
               "W2-1: 8-bit write to SPI_DEVICE Egress SRAM (0x1000) must "
               "raise Store Access Fault (mcause=7) due to ByteAccess(0) in "
               "u_tlul2sram_egress, got count=%u mcause=%u",
               g_access_fault_count, g_last_mcause);
}

/**
 * Wave 2 Check 8 (W2-2): Unmapped SRAM hole `[0x1d40..0x1dff]` write fault
 * (`spi_device_reg_top.sv:131-137, 171` vs `ot_spi_device.c:2580-2586`).
 *
 * In RTL, `u_tlul2sram_egress` maps `[4096:7487]` (`0x1000..0x1d3f`, 848
 * words). Offsets `0x1d40..0x1dff` (`7488..7679`) steer to `reg_steer = 2'd2`
 * (`u_reg_if`), where `addrmiss = 1 -> reg_error = 1` (`d_error = 1`),
 * raising a Store Access Fault (`mcause = 7`).
 */
static void test_w2_egress_unmapped_hole_write_fault(void) {
  g_access_fault_count = 0;
  g_last_mcause = 0;
  g_expect_access_fault = true;
  abs_mmio_write32(kSpiDeviceBase + 0x1d40u, 0xdeadbeefu);
  g_expect_access_fault = false;

  EXPECT_CHECK(
      g_access_fault_count == 1u && g_last_mcause == 7u,
      "W2-2: 32-bit write to unmapped SPI_DEVICE offset 0x1d40 "
      "(between Egress [0x1000..0x1d3f] and Ingress [0x1e00..0x1fbf]) "
      "must raise Store Access Fault (mcause=7), got count=%u mcause=%u",
      g_access_fault_count, g_last_mcause);
}

/**
 * Wave 2 Check 9 (W2-3): Unmapped SRAM hole `[0x1fc0..0x1fff]` read fault
 * (`spi_device_reg_top.sv:131-137, 171` vs `ot_spi_device.c:2544-2564`).
 *
 * In RTL, `u_tlul2sram_ingress` maps `[7680:8127]` (`0x1e00..0x1fbf`, 112
 * words). Offsets `0x1fc0..0x1fff` (`8128..8191`) steer to `reg_steer = 2'd2`
 * (`u_reg_if`), where `addrmiss = 1 -> reg_error = 1` (`d_error = 1`),
 * raising a Load Access Fault (`mcause = 5`).
 */
static void test_w2_ingress_unmapped_hole_read_fault(void) {
  g_access_fault_count = 0;
  (void)abs_mmio_read32(kSpiDeviceBase + 0x1f00u);  // SPI_SRAM_CMD_OFFSET
  (void)abs_mmio_read32(kSpiDeviceBase + 0x1f40u);  // SPI_SRAM_ADDR_OFFSET
  EXPECT_CHECK(g_access_fault_count == 0u,
               "W2-3a: 32-bit reads from valid Ingress CMD (0x1f00) and ADDR "
               "(0x1f40) buffer ranges must succeed without access fault, got "
               "count=%u",
               g_access_fault_count);

  g_access_fault_count = 0;
  g_last_mcause = 0;
  g_expect_access_fault = true;
  (void)abs_mmio_read32(kSpiDeviceBase + 0x1fc0u);
  g_expect_access_fault = false;

  EXPECT_CHECK(g_access_fault_count == 1u && g_last_mcause == 5u,
               "W2-3: 32-bit read from unmapped SPI_DEVICE offset 0x1fc0 "
               "(past Ingress [0x1e00..0x1fbf]) must raise Load Access Fault "
               "(mcause=5), got count=%u mcause=%u",
               g_access_fault_count, g_last_mcause);
}

/**
 * Wave 5 Check 10 (W5-1): `SPI_DEVICE_PERMIT` sub-word write error (`wr_err`)
 * (`spi_device_reg_pkg.sv:837-911`, `spi_device_reg_top.sv` vs
 * `ot_spi_device.c:3110-3124`).
 *
 * - `CFG` (`0x14`, `PERMIT = 4'b1111`) and `TPM_STS` (`0x814`, `PERMIT =
 * 4'b1111`) reject sub-word writes (`sb`/`sh`) with a Store Access Fault
 * (`mcause = 7`) and leave register values unchanged.
 * - `TPM_RID` (`0x82c`, `PERMIT = 4'b0001`) accepts a byte write at offset +0
 *   (`reg_be = 4'b0001`) and rejects a byte write at offset +1 (`reg_be =
 * 4'b0010`) with `mcause = 7`.
 */
static void test_w5_spi_device_permit_subword_wr_err(void) {
  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_CFG_REG_OFFSET, 0x00000004u);
  g_access_fault_count = 0;
  g_last_mcause = 0;
  g_expect_access_fault = true;
  abs_mmio_write8(kSpiDeviceBase + SPI_DEVICE_CFG_REG_OFFSET, 0x03u);
  g_expect_access_fault = false;
  uint32_t cfg_val =
      abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_CFG_REG_OFFSET);
  EXPECT_CHECK(g_access_fault_count == 1u && g_last_mcause == 7u &&
                   cfg_val == 0x00000004u,
               "W5-1a: 8-bit write to CFG (PERMIT=4'b1111) must raise Store "
               "Access Fault (mcause=7) and preserve CFG=0x04, got count=%u "
               "mcause=%u cfg=0x%08x",
               g_access_fault_count, g_last_mcause, cfg_val);
  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_CFG_REG_OFFSET, 0u);

  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_TPM_STS_REG_OFFSET, 0x12345678u);
  g_access_fault_count = 0;
  g_last_mcause = 0;
  g_expect_access_fault = true;
  abs_mmio_write8(kSpiDeviceBase + SPI_DEVICE_TPM_STS_REG_OFFSET, 0xaau);
  g_expect_access_fault = false;
  uint32_t tpm_sts =
      abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_TPM_STS_REG_OFFSET);
  EXPECT_CHECK(g_access_fault_count == 1u && g_last_mcause == 7u &&
                   tpm_sts == 0x12345678u,
               "W5-1b: 8-bit write to TPM_STS (PERMIT=4'b1111) must raise "
               "Store Access Fault (mcause=7) and preserve TPM_STS=0x12345678, "
               "got count=%u mcause=%u tpm_sts=0x%08x",
               g_access_fault_count, g_last_mcause, tpm_sts);
  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_TPM_STS_REG_OFFSET, 0u);

  g_access_fault_count = 0;
  abs_mmio_write8(kSpiDeviceBase + SPI_DEVICE_TPM_RID_REG_OFFSET, 0x5au);
  uint32_t tpm_rid =
      abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_TPM_RID_REG_OFFSET);
  EXPECT_CHECK(g_access_fault_count == 0u && tpm_rid == 0x5au,
               "W5-1c: 8-bit write to TPM_RID+0 (PERMIT=4'b0001) must succeed "
               "(got count=%u tpm_rid=0x%08x)",
               g_access_fault_count, tpm_rid);

  g_access_fault_count = 0;
  g_last_mcause = 0;
  g_expect_access_fault = true;
  abs_mmio_write8(kSpiDeviceBase + SPI_DEVICE_TPM_RID_REG_OFFSET + 1u, 0xa5u);
  g_expect_access_fault = false;
  tpm_rid = abs_mmio_read32(kSpiDeviceBase + SPI_DEVICE_TPM_RID_REG_OFFSET);
  EXPECT_CHECK(
      g_access_fault_count == 1u && g_last_mcause == 7u && tpm_rid == 0x5au,
      "W5-1d: 8-bit write to TPM_RID+1 (PERMIT=4'b0001, reg_be=4'b0010) "
      "must raise Store Access Fault (mcause=7) and preserve 0x5a, "
      "got count=%u mcause=%u tpm_rid=0x%08x",
      g_access_fault_count, g_last_mcause, tpm_rid);
  abs_mmio_write32(kSpiDeviceBase + SPI_DEVICE_TPM_RID_REG_OFFSET, 0u);
}

bool test_main(void) {
  CHECK_DIF_OK(dif_alert_handler_init(mmio_region_from_addr(kAlertHandlerBase),
                                      &alert_handler));

  LOG_INFO("Running spi_device_rtl_consistency_test...");
  test_alert_test_pulse();
  test_intr_test_tpm_header_not_empty_status_readback();
  test_cmd_info_read_pipeline_mode_mask();
  test_addr_mode_pending_on_sw_write();
  test_flash_status_committed_readback_while_idle();
  test_tpm_register_bitmasks();
  test_w2_egress_sram_subword_write_fault();
  test_w2_egress_unmapped_hole_write_fault();
  test_w2_ingress_unmapped_hole_read_fault();
  test_w5_spi_device_permit_subword_wr_err();
  LOG_INFO("spi_device_rtl_consistency_test finished with %u failure(s).",
           g_failures);
  return g_failures == 0;
}
