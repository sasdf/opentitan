// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "rv_core_ibex_regs.h"
#include "sram_ctrl_regs.h"
#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/dif/dif_alert_handler.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"
#include "sw/device/silicon_creator/lib/drivers/retention_sram.h"

OTTF_DEFINE_TEST_CONFIG();

enum {
  kSramRetRegsBase = TOP_EARLGREY_SRAM_CTRL_RET_AON_REGS_BASE_ADDR,
  kSramRetRamBase = TOP_EARLGREY_SRAM_CTRL_RET_AON_RAM_BASE_ADDR,
  kIbexCfgBase = TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR,
};

static volatile uint32_t kFaultCount = 0;

void ottf_internal_isr(uint32_t *exc_info) {
  (void)exc_info;
  kFaultCount++;
  abs_mmio_write32(kIbexCfgBase + RV_CORE_IBEX_ERR_STATUS_REG_OFFSET,
                   0xffffffffu);
}

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  (void)exc_info;
  kFaultCount++;
  abs_mmio_write32(kIbexCfgBase + RV_CORE_IBEX_ERR_STATUS_REG_OFFSET,
                   0xffffffffu);
  uint32_t mepc = ibex_mepc_read();
  uint16_t inst = *(volatile uint16_t *)mepc;
  uint32_t step = ((inst & 0x3u) == 0x3u) ? 4u : 2u;
  CSR_WRITE(CSR_REG_MEPC, mepc + step);
}

bool test_main(void) {
  // 1. Enable kTopEarlgreyAlertIdSramCtrlRetAonFatalError in alert_handler and
  // exercise R_ALERT_TEST (ot_sram_ctrl_regs_write case R_ALERT_TEST),
  // verifying ALERT_CAUSE latches and clears on both QEMU and CW340 FPGA.
  dif_alert_handler_t alert_handler;
  CHECK_DIF_OK(dif_alert_handler_init(
      mmio_region_from_addr(TOP_EARLGREY_ALERT_HANDLER_BASE_ADDR),
      &alert_handler));
  CHECK_STATUS_OK(
      ottf_alerts_ignore_alert(kTopEarlgreyAlertIdRvCoreIbexFatalHwErr));
  CHECK_STATUS_OK(
      ottf_alerts_ignore_alert(kTopEarlgreyAlertIdSramCtrlRetAonFatalError));
  CHECK_DIF_OK(dif_alert_handler_configure_alert(
      &alert_handler, kTopEarlgreyAlertIdSramCtrlRetAonFatalError,
      kDifAlertHandlerClassA, /*enabled=*/kDifToggleEnabled,
      /*locked=*/kDifToggleDisabled));
  CHECK_DIF_OK(dif_alert_handler_alert_acknowledge(
      &alert_handler, kTopEarlgreyAlertIdSramCtrlRetAonFatalError));
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_ALERT_TEST_REG_OFFSET,
                   1u << SRAM_CTRL_ALERT_TEST_FATAL_ERROR_BIT);
  bool is_cause = false;
  CHECK_DIF_OK(dif_alert_handler_alert_is_cause(
      &alert_handler, kTopEarlgreyAlertIdSramCtrlRetAonFatalError, &is_cause));
  CHECK(is_cause,
        "Expected alert_handler ALERT_CAUSE to be set by SRAM_CTRL ALERT_TEST");
  CHECK_DIF_OK(dif_alert_handler_alert_acknowledge(
      &alert_handler, kTopEarlgreyAlertIdSramCtrlRetAonFatalError));

  // Preserve test_rom/rom creator words in retention SRAM.
  const uintptr_t reset_reasons_addr =
      kSramRetRamBase + offsetof(retention_sram_t, creator.reset_reasons);
  const uintptr_t creator_last_addr =
      kSramRetRamBase + offsetof(retention_sram_t, owner) - sizeof(uint32_t);
  uint32_t saved_reset_reasons = abs_mmio_read32(reset_reasons_addr);
  uint32_t saved_creator_last = abs_mmio_read32(creator_last_addr);

  const uintptr_t owner_base =
      kSramRetRamBase + offsetof(retention_sram_t, owner);
  for (size_t i = 0; i < 16; ++i) {
    abs_mmio_write32(owner_base + i * sizeof(uint32_t),
                     0xa5a50000u ^ (uint32_t)i);
  }

  // 1. Rotate scrambling key (CTRL.RENEW_SCR_KEY) without CTRL.INIT and read
  // previously written cells via ot_sram_ctrl_mem_init_read_with_attrs,
  // verifying load integrity faults occur on both QEMU and CW340 FPGA.
  kFaultCount = 0;
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_CTRL_REG_OFFSET,
                   1u << SRAM_CTRL_CTRL_RENEW_SCR_KEY_BIT);
  uint32_t status = 0;
  do {
    status = abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_STATUS_REG_OFFSET);
  } while (!bitfield_bit32_read(status, SRAM_CTRL_STATUS_SCR_KEY_VALID_BIT));
  CHECK(bitfield_bit32_read(status, SRAM_CTRL_STATUS_SCR_KEY_SEED_VALID_BIT));
  CHECK(abs_mmio_read32(kSramRetRegsBase +
                        SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET) ==
        kMultiBitBool4True);

  for (size_t i = 0; i < 16; ++i) {
    (void)abs_mmio_read32(owner_base + i * sizeof(uint32_t));
  }
  CHECK(kFaultCount > 0,
        "Expected load integrity fault reading SRAM after key rotation");

  // Overwrite the 16 words under the new key and verify clean reads.
  kFaultCount = 0;
  for (size_t i = 0; i < 16; ++i) {
    abs_mmio_write32(owner_base + i * sizeof(uint32_t),
                     0x5a5a0000u ^ (uint32_t)i);
  }
  for (size_t i = 0; i < 16; ++i) {
    uint32_t got = abs_mmio_read32(owner_base + i * sizeof(uint32_t));
    CHECK(got == (0x5a5a0000u ^ (uint32_t)i),
          "Retention SRAM mismatch at word %u: got 0x%08x", i, got);
  }
  CHECK(kFaultCount == 0, "Unexpected fault reading freshly written SRAM");

  // 2. Cover Chunk 11 (ot_sram_ctrl_mem_init_read_with_attrs lines 667-673):
  // Trigger CTRL.INIT and immediately read from Retention SRAM in the very next
  // instruction (before init_timer finishes). QEMU expedites the remaining
  // initialization chunks on read, and CW340 FPGA stalls the bus read until
  // hardware LFSR initialization completes.
  kFaultCount = 0;
  abs_mmio_write32(kSramRetRegsBase + SRAM_CTRL_CTRL_REG_OFFSET,
                   1u << SRAM_CTRL_CTRL_INIT_BIT);
  volatile uint32_t expedited_read = abs_mmio_read32(owner_base);
  (void)expedited_read;
  CHECK(kFaultCount == 0,
        "Immediate read during CTRL.INIT should stall/expedite without fault");

  status = abs_mmio_read32(kSramRetRegsBase + SRAM_CTRL_STATUS_REG_OFFSET);
  CHECK(bitfield_bit32_read(status, SRAM_CTRL_STATUS_INIT_DONE_BIT),
        "STATUS.INIT_DONE must be set after expedited CTRL.INIT read (0x%08x)",
        status);

  bool changed = false;
  for (size_t i = 0; i < 16; ++i) {
    if (abs_mmio_read32(owner_base + i * sizeof(uint32_t)) !=
        (0x5a5a0000u ^ (uint32_t)i)) {
      changed = true;
      break;
    }
  }
  CHECK(changed, "CTRL.INIT must overwrite previous retention SRAM contents");

  // Restore creator words.
  abs_mmio_write32(reset_reasons_addr, saved_reset_reasons);
  abs_mmio_write32(creator_last_addr, saved_creator_last);

  return true;
}
