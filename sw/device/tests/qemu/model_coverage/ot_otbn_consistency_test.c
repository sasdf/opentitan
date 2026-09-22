// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * CW340 FPGA & QEMU Consistency Test for `ot_otbn` (`hw/opentitan/ot_otbn.c`).
 *
 * Strictly verifies identical hardware behavior between CW340 FPGA and QEMU
 * for:
 * 1. Write-only (`INTR_TEST`, `ALERT_TEST`, `CMD`) reads returning `0`, and
 *    read-only (`STATUS`, `FATAL_ALERT_CAUSE`) writes ignored (`ot_otbn.c`
 *    Chunks 15, 20).
 * 2. Invalid `CMD` opcodes (`0x12`) ignored while `IDLE`, keeping
 *    `STATUS == IDLE (0x0)` and `INTR_STATE.DONE == 0` (`ot_otbn.c` Chunk 10).
 * 3. Non-idle writes to `CMD`, `CTRL`, `ERR_BITS`, and `INSN_CNT` ignored while
 *    `STATUS == BUSY_EXECUTE (0x1)` (`ot_otbn.c` Chunks 8, 17, 18, 19).
 * 4. Illegal host bus write (`DMEM`/`IMEM`) during `BUSY_EXECUTE` locking OTBN
 *    (`STATUS == 0xFF`, `ERR_BITS.ILLEGAL_BUS_ACCESS`,
 *    `FATAL_ALERT_CAUSE.ILLEGAL_BUS_ACCESS`), ignored `DMEM`/`IMEM` writes
 * while locked, and writable `INTR_STATE`/`INSN_CNT`/`ERR_BITS` while `STATUS
 * == LOCKED`.
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/ip/otbn/data/otbn_regs.h"
#include "hw/ip/rv_core_ibex/data/rv_core_ibex_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG(.ignore_alerts = true);

enum {
  kOtbnBase = TOP_EARLGREY_OTBN_BASE_ADDR,
  kIbexBase = TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR,
  kCmdExecute = 0xd8u,
  kCmdSecWipeDmem = 0xc3u,
  kCmdSecWipeImem = 0x1eu,
  kStatusIdle = 0x00u,
  kStatusBusyExecute = 0x01u,
  kStatusLocked = 0xffu,
};

static volatile bool load_integrity_fault_seen = false;

void ottf_internal_isr(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t err_status =
      abs_mmio_read32(kIbexBase + RV_CORE_IBEX_ERR_STATUS_REG_OFFSET);
  if ((err_status & (1u << RV_CORE_IBEX_ERR_STATUS_FATAL_INTG_ERR_BIT)) != 0u) {
    load_integrity_fault_seen = true;
  }
  abs_mmio_write32(kIbexBase + RV_CORE_IBEX_ERR_STATUS_REG_OFFSET, UINT32_MAX);
}

#define OTBN_ECALL() (0x00000073u)
#define OTBN_NOP() (0x00000013u)
#define OTBN_LOOPI(iters, bodysize_minus_1)                    \
  ((((uint32_t)(bodysize_minus_1)&0xfffu) << 20) |             \
   ((((uint32_t)(iters) >> 5) & 0x1fu) << 15) | (0x1u << 12) | \
   (((uint32_t)(iters)&0x1fu) << 7) | 0x7bu)

static void otbn_wait_for_not_running(void) {
  for (uint32_t i = 0; i < 1000000; ++i) {
    uint32_t status = abs_mmio_read32(kOtbnBase + OTBN_STATUS_REG_OFFSET);
    if (status == kStatusIdle || status == kStatusLocked) {
      return;
    }
  }
  CHECK(false, "Timed out waiting for OTBN to finish");
}

static void test_wo_ro_registers_and_invalid_cmd(void) {
  LOG_INFO(
      "Section 1 & 2a: W/O & R/O register accesses and invalid CMD opcode");

  otbn_wait_for_not_running();
  abs_mmio_write32(kOtbnBase + OTBN_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kOtbnBase + OTBN_INTR_STATE_REG_OFFSET, 1u);

  // 1a. Read write-only registers INTR_TEST, ALERT_TEST, and CMD -> must read
  // 0.
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_INTR_TEST_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_ALERT_TEST_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_CMD_REG_OFFSET) == 0u);

  // 1b. Write read-only registers STATUS and FATAL_ALERT_CAUSE -> must be
  // ignored.
  uint32_t status_before = abs_mmio_read32(kOtbnBase + OTBN_STATUS_REG_OFFSET);
  uint32_t fatal_before =
      abs_mmio_read32(kOtbnBase + OTBN_FATAL_ALERT_CAUSE_REG_OFFSET);
  CHECK(status_before == kStatusIdle);
  abs_mmio_write8(kOtbnBase + OTBN_STATUS_REG_OFFSET, 0xffu);
  abs_mmio_write8(kOtbnBase + OTBN_FATAL_ALERT_CAUSE_REG_OFFSET, 0xffu);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_STATUS_REG_OFFSET) == status_before);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_FATAL_ALERT_CAUSE_REG_OFFSET) ==
        fatal_before);

  // 2a. Write invalid CMD opcode (0x12) while IDLE -> must be ignored, keeping
  // STATUS == IDLE and INTR_STATE.DONE == 0.
  abs_mmio_write32(kOtbnBase + OTBN_CMD_REG_OFFSET, 0x12u);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_STATUS_REG_OFFSET) == kStatusIdle);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_INTR_STATE_REG_OFFSET) == 0u);
}

static void test_non_idle_register_writes(void) {
  LOG_INFO("Section 2b: Non-idle writes to CMD, CTRL, ERR_BITS, and INSN_CNT");

  const uint32_t prog_busy_loop[] = {
      OTBN_LOOPI(1000, 0),
      OTBN_NOP(),
      OTBN_ECALL(),
  };
  for (size_t i = 0; i < ARRAYSIZE(prog_busy_loop); ++i) {
    abs_mmio_write32(kOtbnBase + OTBN_IMEM_REG_OFFSET + i * sizeof(uint32_t),
                     prog_busy_loop[i]);
  }
  abs_mmio_write32(kOtbnBase + OTBN_CTRL_REG_OFFSET, 0u);
  abs_mmio_write32(kOtbnBase + OTBN_INSN_CNT_REG_OFFSET, 0u);
  abs_mmio_write32(kOtbnBase + OTBN_INTR_STATE_REG_OFFSET, 1u);

  // Start execution so OTBN enters BUSY_EXECUTE.
  abs_mmio_write32(kOtbnBase + OTBN_CMD_REG_OFFSET, kCmdExecute);

  // Wait until OTBN has started executing instructions (BUSY_EXECUTE).
  for (int i = 0;
       i < 100 && abs_mmio_read32(kOtbnBase + OTBN_INSN_CNT_REG_OFFSET) == 0u;
       ++i) {
  }
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_STATUS_REG_OFFSET) ==
        kStatusBusyExecute);

  // Issue writes to CMD, CTRL, ERR_BITS, and INSN_CNT while BUSY_EXECUTE:
  // all must be ignored.
  abs_mmio_write32(kOtbnBase + OTBN_CMD_REG_OFFSET, kCmdSecWipeDmem);
  abs_mmio_write32(kOtbnBase + OTBN_CTRL_REG_OFFSET, 1u);
  abs_mmio_write32(kOtbnBase + OTBN_ERR_BITS_REG_OFFSET, 0xffffffffu);
  abs_mmio_write32(kOtbnBase + OTBN_INSN_CNT_REG_OFFSET, 0u);

  otbn_wait_for_not_running();

  CHECK(abs_mmio_read32(kOtbnBase + OTBN_STATUS_REG_OFFSET) == kStatusIdle);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_INTR_STATE_REG_OFFSET) == 1u);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_CTRL_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_ERR_BITS_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_INSN_CNT_REG_OFFSET) == 1002u);
}

static void test_illegal_bus_access_lock_and_locked_writes(void) {
  LOG_INFO(
      "Section 3: Illegal host memory access lock & locked register/mem "
      "writes");

  // Seed non-zero value in DMEM[0] while IDLE and verify it reads back.
  abs_mmio_write32(kOtbnBase + OTBN_DMEM_REG_OFFSET, 0xdeadbeefu);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_DMEM_REG_OFFSET) == 0xdeadbeefu);

  // Record expected CRC32 for writing 0x12345678 to DMEM[0] and IMEM[0] while
  // IDLE.
  abs_mmio_write32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET, 0u);
  abs_mmio_write32(kOtbnBase + OTBN_DMEM_REG_OFFSET, 0x12345678u);
  abs_mmio_write32(kOtbnBase + OTBN_IMEM_REG_OFFSET, 0x12345678u);
  const uint32_t expected_crc =
      abs_mmio_read32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET);
  CHECK(expected_crc != 0u);

  const uint32_t prog_long_loop[] = {
      OTBN_LOOPI(1000, 0),
      OTBN_NOP(),
      OTBN_ECALL(),
  };
  for (size_t i = 0; i < ARRAYSIZE(prog_long_loop); ++i) {
    abs_mmio_write32(kOtbnBase + OTBN_IMEM_REG_OFFSET + i * sizeof(uint32_t),
                     prog_long_loop[i]);
  }
  abs_mmio_write32(kOtbnBase + OTBN_INTR_STATE_REG_OFFSET, 1u);
  abs_mmio_write32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET, 0u);
  abs_mmio_write32(kOtbnBase + OTBN_CMD_REG_OFFSET, kCmdExecute);

  for (int i = 0;
       i < 100 && abs_mmio_read32(kOtbnBase + OTBN_INSN_CNT_REG_OFFSET) == 0u;
       ++i) {
  }

  // Trigger ILLEGAL_BUS_ACCESS by reading DMEM while BUSY_EXECUTE.
  // Per otbn.sv:709-711,737-739:
  // 1) Because dmem_access_core == 1 while BUSY_EXECUTE, mem_crc_data_in_valid
  // == 0
  //    (LOAD_CHECKSUM does not advance).
  // 2) On the cycle dmem_dummy_response_q returns rvalid_bus, locking_q is 0
  // and
  //    u_dmem_rdata_bus_blanker outputs 39'h0 (7'h00 ECC instead of 7'h39),
  //    which raises RV_CORE_IBEX_ERR_STATUS.FATAL_INTG_ERR and an internal NMI.
  load_integrity_fault_seen = false;
  (void)abs_mmio_read32(kOtbnBase + OTBN_DMEM_REG_OFFSET);
  otbn_wait_for_not_running();
  for (volatile int i = 0; i < 500; ++i) {
  }

  CHECK(load_integrity_fault_seen);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_STATUS_REG_OFFSET) == kStatusLocked);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_ERR_BITS_REG_OFFSET) ==
        (1u << OTBN_ERR_BITS_ILLEGAL_BUS_ACCESS_BIT));
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_FATAL_ALERT_CAUSE_REG_OFFSET) ==
        (1u << OTBN_FATAL_ALERT_CAUSE_ILLEGAL_BUS_ACCESS_BIT));
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_INTR_STATE_REG_OFFSET) == 1u);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_INSN_CNT_REG_OFFSET) == 0u);

  // Once STATUS == LOCKED (busy_execute_q == 0), mem_crc_data_in_valid in
  // otbn.sv:737-739 (~(dmem_access_core | imem_access_core)) is 1, so 32-bit
  // bus writes to DMEM/IMEM advance LOAD_CHECKSUM to the exact same CRC32.
  abs_mmio_write32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET, 0u);
  abs_mmio_write32(kOtbnBase + OTBN_DMEM_REG_OFFSET, 0x12345678u);
  abs_mmio_write32(kOtbnBase + OTBN_IMEM_REG_OFFSET, 0x12345678u);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_LOAD_CHECKSUM_REG_OFFSET) ==
        expected_crc);

  abs_mmio_write32(kOtbnBase + OTBN_INTR_STATE_REG_OFFSET, 1u);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_INTR_STATE_REG_OFFSET) == 0u);

  abs_mmio_write32(kOtbnBase + OTBN_INSN_CNT_REG_OFFSET, 0xffffffffu);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_INSN_CNT_REG_OFFSET) == 0u);

  abs_mmio_write32(kOtbnBase + OTBN_ERR_BITS_REG_OFFSET, 0xffffffffu);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_ERR_BITS_REG_OFFSET) == 0u);
  CHECK(abs_mmio_read32(kOtbnBase + OTBN_FATAL_ALERT_CAUSE_REG_OFFSET) ==
        (1u << OTBN_FATAL_ALERT_CAUSE_ILLEGAL_BUS_ACCESS_BIT));
}

bool test_main(void) {
  test_wo_ro_registers_and_invalid_cmd();
  test_non_idle_register_writes();
  test_illegal_bus_access_lock_and_locked_writes();
  return true;
}
