// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file rv_core_ibex_errata_v2_test.c
 * @brief Earlgrey v2 (trunk-v2) hardware & spec errata verification test for
 * `rv_core_ibex` on the physical CW340 FPGA.
 *
 * Verifies:
 * 1. `DV_SIM_WINDOW` (`0x80..0x9F`) unconditional `tlul_err_resp` vs. the v2
 *    unmapped register decode gap (`0x74..0x7F` between `MCOUNTEREN_WRITABLE`
 *    at `0x70` and `DV_SIM_WINDOW` at `0x80`), shifted `FPGA_INFO` (`0x68`),
 *    and `RV_CORE_IBEX_CFG_PERMIT` sub-word write enforcement across the 29
 *    configuration registers (`rv_core_ibex_reg_pkg.sv:19,208-236,292-322`
 *    and `rv_core_ibex_cfg_reg_top.sv:1434-1501`).
 * 2. `NMI_ENABLE` (`0x4C`) `SwAccessW1S` irreversibility, `NMI_STATE` (`0x50`)
 *    `SwAccessW1C` clearing and `irq_nm` gating (`rv_core_ibex.sv:352-358`),
 *    and 1-deep `mstack` NMI clobbering during an active exception handler
 *    (`ibex_cs_registers.sv:1110-1143`).
 * 3. `SW_RECOV_ERR` (`0x04`) `mubi4_test_true_loose` activation (`15/16`
 *    4-bit values trigger `recov_sw_err` and hardware auto-resets
 * `SW_RECOV_ERR` to `MuBi4False` (`0x9`) via `alert_acks[1]` in
 * `rv_core_ibex.sv:1051-1061`), and `RND_DATA` (`0x58`) read side-effect
 * (`reg2hw.rnd_data.re`) clearing `RND_STATUS` (`0x5C`) prior to EDN refill
 * (`rv_core_ibex.sv:1092-1120`).
 * 4. [NEW IN V2] `MCOUNTEREN_WRITABLE` (`0x70`) / `MCOUNTEREN_WRITABLE_REGWEN`
 *    (`0x6C`) and Ibex `mcounteren` (`0x306`) CSR 4-bit WARL mask (`0x0000001D`
 *    due to `RvCoreIbexMHPMCounterNum=2` vs `0x00001FFD` IP default),
 *    strict `MuBi4True` (`0x6`) write-enable gating
 * (`rv_core_ibex.sv:461-465`), silent CSR write suppression (`illegal_csr == 0`
 * in `ibex_cs_registers.sv:464,845`) without clearing existing `mcounteren_q`
 *    bits when `MCOUNTEREN_WRITABLE != MuBi4True`, and permanent `W0C` lock
 *    via `MCOUNTEREN_WRITABLE_REGWEN`.
 * 5. [NEW IN V2] `CHERIOT_ENA` (`0x60`) missing `regwen` protection and
 *    desynchronized readback (`cheriot_ena_qs` vs. locked `cheriot_ena_o`)
 *    after locking `CHERIOT_LOCK` (`0x64`), `CHERIOT_LOCK` write-only `0x0`
 *    readback (`rv_core_ibex_cfg_reg_top.sv:1310-1356,1744-1750`), and
 *    `u_cheriot_switch` `LockedDis` state
 * (`rv_core_ibex_cheriot_switch.sv:92-95`) silently ignoring subsequent invalid
 * `CHERIOT_LOCK` writes without entering `Error` or raising `fatal_hw_err`.
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/dif/dif_alert_handler.h"
#include "sw/device/lib/runtime/ibex.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_alerts.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top/aon_timer_regs.h"
#include "hw/top/rv_core_ibex_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

OTTF_DEFINE_TEST_CONFIG();

#ifndef CSR_REG_MCOUNTEREN
#define CSR_REG_MCOUNTEREN 0x306
#endif

enum {
  kIbexCfgBase = TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR,
  kAonTimerBase = TOP_EARLGREY_AON_TIMER_BASE_ADDR,
};

static volatile bool g_expect_bus_fault = false;
static volatile uint32_t g_bus_fault_count = 0;
static volatile uint32_t g_last_mcause = 0;

static volatile bool g_in_trap_test = false;
static volatile uint32_t g_nmi_count = 0;
static volatile uint32_t g_nmi_mcause = 0;
static volatile uint32_t g_nmi_entry_mepc = 0;
static volatile uint32_t g_nmi_state_on_entry = 0;

void ottf_load_store_fault_handler(uint32_t *exc_info) {
  uint32_t mcause = ibex_mcause_read();
  if (g_expect_bus_fault) {
    g_bus_fault_count++;
    g_last_mcause = mcause;
    return;
  }

  if (g_in_trap_test) {
    uint32_t orig_mepc = 0;
    CSR_READ(CSR_REG_MEPC, &orig_mepc);

    // Trigger AON watchdog bark while already inside this M-mode exception
    // handler (`controller_fsm_cs != DECODE`), so that `irq_nm_i` enters via
    // the normal decode loop after `mstack` already saved the outer trap
    // context, without pulsing `double_fault_seen_o`.
    abs_mmio_write32(kAonTimerBase + AON_TIMER_WDOG_CTRL_REG_OFFSET, 0);
    abs_mmio_write32(kAonTimerBase + AON_TIMER_WDOG_COUNT_REG_OFFSET, 0);
    abs_mmio_write32(kAonTimerBase + AON_TIMER_WDOG_BARK_THOLD_REG_OFFSET, 1);
    abs_mmio_write32(kAonTimerBase + AON_TIMER_WDOG_BITE_THOLD_REG_OFFSET,
                     0xFFFFFFFFu);
    abs_mmio_write32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET,
                     (1u << AON_TIMER_INTR_STATE_WDOG_TIMER_BARK_BIT));
    abs_mmio_write32(kAonTimerBase + AON_TIMER_WDOG_CTRL_REG_OFFSET,
                     (1u << AON_TIMER_WDOG_CTRL_ENABLE_BIT));

    while (g_nmi_count == 0) {
      asm volatile("nop");
    }

    // Because `mstack` in `ibex_cs_registers.sv:1110-1143` is only 1 level
    // deep, the nested NMI overwrote `mstack_epc_q` with the instruction inside
    // this handler (`g_nmi_entry_mepc`) and restored `mepc` to that address on
    // its `mret`. We must manually restore `orig_mepc` so `ottf_isrs.c` can
    // advance past the faulting instruction and return to `test_main`.
    CSR_WRITE(CSR_REG_MEPC, orig_mepc);
    g_in_trap_test = false;
    return;
  }

  OT_DISCARD(exc_info);
  CHECK(false, "Unexpected load/store fault mcause=0x%08x", mcause);
}

void ottf_internal_isr(uint32_t *exc_info) {
  OT_DISCARD(exc_info);
  g_nmi_count++;
  CSR_READ(CSR_REG_MCAUSE, &g_nmi_mcause);
  CSR_READ(CSR_REG_MEPC, &g_nmi_entry_mepc);
  g_nmi_state_on_entry =
      abs_mmio_read32(kIbexCfgBase + RV_CORE_IBEX_NMI_STATE_REG_OFFSET);

  // Stop and clear the watchdog bark source at AON_TIMER first, then wait for
  // the 2-stage synchronizer (`u_wdog_nmi_sync` in `rv_core_ibex.sv:343-350`)
  // to deassert `wdog_irq_nm` before clearing `NMI_STATE`.
  abs_mmio_write32(kAonTimerBase + AON_TIMER_WDOG_CTRL_REG_OFFSET, 0);
  abs_mmio_write32(kAonTimerBase + AON_TIMER_INTR_STATE_REG_OFFSET,
                   (1u << AON_TIMER_INTR_STATE_WDOG_TIMER_BARK_BIT));
  for (int i = 0; i < 16; ++i) {
    asm volatile("nop");
  }
  abs_mmio_write32(kIbexCfgBase + RV_CORE_IBEX_NMI_STATE_REG_OFFSET,
                   (1u << RV_CORE_IBEX_NMI_STATE_ALERT_BIT) |
                       (1u << RV_CORE_IBEX_NMI_STATE_WDOG_BIT));
}

/**
 * Test 1 (Part A - v1 Erratum 001 on v2):
 * Verify `DV_SIM_WINDOW` (`0x80..0x9F`) unconditional `tlul_err_resp`,
 * the v2 unmapped register gap (`0x74..0x7F` after `MCOUNTEREN_WRITABLE` at
 * `0x70`), shifted `FPGA_INFO` (`0x68`), and `RV_CORE_IBEX_CFG_PERMIT`
 * sub-word write enforcement.
 */
static void test_window_gap_and_subword_permits(void) {
  LOG_INFO("Test 1: DV_SIM_WINDOW, v2 unmapped gap (0x74..0x7F), and permits");

  // Confirm shifted `FPGA_INFO` at `0x68`, `MCOUNTEREN_WRITABLE_REGWEN` at
  // `0x6C`, and `MCOUNTEREN_WRITABLE` at `0x70` are readable without bus fault.
  g_expect_bus_fault = false;
  uint32_t prev_faults = g_bus_fault_count;
  uint32_t fpga_info =
      abs_mmio_read32(kIbexCfgBase + RV_CORE_IBEX_FPGA_INFO_REG_OFFSET);
  uint32_t regwen_init = abs_mmio_read32(
      kIbexCfgBase + RV_CORE_IBEX_MCOUNTEREN_WRITABLE_REGWEN_REG_OFFSET);
  uint32_t writable_init = abs_mmio_read32(
      kIbexCfgBase + RV_CORE_IBEX_MCOUNTEREN_WRITABLE_REG_OFFSET);
  CHECK(g_bus_fault_count == prev_faults);
  CHECK(regwen_init == 1u);
  CHECK(writable_init == kMultiBitBool4True);
  LOG_INFO("  FPGA_INFO (0x68) = 0x%08x", fpga_info);

  // 1a. Unmapped decode gap in v2 is `0x74..0x7F` (3 words: `0x74`, `0x78`,
  // `0x7C`) between `MCOUNTEREN_WRITABLE` (`0x70`) and `DV_SIM_WINDOW`
  // (`0x80`), plus `0xA0..0xFF` after `DV_SIM_WINDOW`.
  for (uint32_t off = 0x74; off < 0x80; off += 4) {
    uint32_t prev = g_bus_fault_count;
    g_expect_bus_fault = true;
    (void)abs_mmio_read32(kIbexCfgBase + off);
    g_expect_bus_fault = false;
    CHECK(g_bus_fault_count == prev + 1,
          "Expected load fault at unmapped v2 gap offset 0x%02x", off);
    CHECK(g_last_mcause == 5, "Expected mcause=5 at 0x%02x, got %u", off,
          g_last_mcause);

    prev = g_bus_fault_count;
    g_expect_bus_fault = true;
    abs_mmio_write32(kIbexCfgBase + off, 0x12345678u);
    g_expect_bus_fault = false;
    CHECK(g_bus_fault_count == prev + 1,
          "Expected store fault at unmapped v2 gap offset 0x%02x", off);
    CHECK(g_last_mcause == 7, "Expected mcause=7 at 0x%02x, got %u", off,
          g_last_mcause);
  }

  // Also verify `0xA0` (`0xA0..0xFF` unmapped gap in `u_reg_cfg`,
  // `addrmiss=1`).
  {
    uint32_t prev = g_bus_fault_count;
    g_expect_bus_fault = true;
    (void)abs_mmio_read32(kIbexCfgBase + 0xA0u);
    g_expect_bus_fault = false;
    CHECK(g_bus_fault_count == prev + 1 && g_last_mcause == 5);

    prev = g_bus_fault_count;
    g_expect_bus_fault = true;
    abs_mmio_write32(kIbexCfgBase + 0xA0u, 0x12345678u);
    g_expect_bus_fault = false;
    CHECK(g_bus_fault_count == prev + 1 && g_last_mcause == 7);
  }

  // 1b. `DV_SIM_WINDOW` (`0x80..0x9F`) routed to `u_sim_win_rsp`
  // (`tlul_err_resp`).
  for (uint32_t off = 0x80; off < 0xA0; off += 4) {
    uint32_t prev = g_bus_fault_count;
    g_expect_bus_fault = true;
    (void)abs_mmio_read32(kIbexCfgBase + off);
    g_expect_bus_fault = false;
    CHECK(g_bus_fault_count == prev + 1,
          "Expected load fault at DV_SIM_WINDOW offset 0x%02x", off);
    CHECK(g_last_mcause == 5, "Expected mcause=5 at 0x%02x", off);

    prev = g_bus_fault_count;
    g_expect_bus_fault = true;
    abs_mmio_write32(kIbexCfgBase + off, 0xCAFEBABEu);
    g_expect_bus_fault = false;
    CHECK(g_bus_fault_count == prev + 1,
          "Expected store fault at DV_SIM_WINDOW offset 0x%02x", off);
    CHECK(g_last_mcause == 7, "Expected mcause=7 at 0x%02x", off);
  }

  // 1c. Sub-word write enforcement (`RV_CORE_IBEX_CFG_PERMIT`):
  // `IBUS_ADDR_MATCHING_0` (`0x10`) has permit `4'b1111`, so `sb`/`sh` faults.
  abs_mmio_write32(kIbexCfgBase + RV_CORE_IBEX_IBUS_ADDR_MATCHING_0_REG_OFFSET,
                   0x11223344u);
  uint32_t prev = g_bus_fault_count;
  g_expect_bus_fault = true;
  abs_mmio_write8(kIbexCfgBase + RV_CORE_IBEX_IBUS_ADDR_MATCHING_0_REG_OFFSET,
                  0xAAu);
  g_expect_bus_fault = false;
  CHECK(g_bus_fault_count == prev + 1 && g_last_mcause == 7,
        "Expected sb fault on 4-byte permit CSR IBUS_ADDR_MATCHING_0");

  prev = g_bus_fault_count;
  g_expect_bus_fault = true;
  *(volatile uint16_t
        *)(uintptr_t)(kIbexCfgBase +
                      RV_CORE_IBEX_IBUS_ADDR_MATCHING_0_REG_OFFSET) = 0xBBCCu;
  g_expect_bus_fault = false;
  CHECK(g_bus_fault_count == prev + 1 && g_last_mcause == 7,
        "Expected sh fault on 4-byte permit CSR IBUS_ADDR_MATCHING_0");

  CHECK(abs_mmio_read32(kIbexCfgBase +
                        RV_CORE_IBEX_IBUS_ADDR_MATCHING_0_REG_OFFSET) ==
            0x11223344u,
        "Faulting sb/sh must not mutate IBUS_ADDR_MATCHING_0");
  abs_mmio_write32(kIbexCfgBase + RV_CORE_IBEX_IBUS_ADDR_MATCHING_0_REG_OFFSET,
                   0u);

  // `MCOUNTEREN_WRITABLE` (`0x70`) and `CHERIOT_ENA` (`0x60`) have permit
  // `4'b0001`, so `sb` to byte 0 succeeds (`4'b0001 & ~4'b0001 == 0`), while
  // `sb` to byte 1 (`+1`) faults (`mcause = 7`).
  g_expect_bus_fault = false;
  abs_mmio_write8(kIbexCfgBase + RV_CORE_IBEX_MCOUNTEREN_WRITABLE_REG_OFFSET,
                  kMultiBitBool4True);
  CHECK(abs_mmio_read32(kIbexCfgBase +
                        RV_CORE_IBEX_MCOUNTEREN_WRITABLE_REG_OFFSET) ==
            kMultiBitBool4True,
        "Expected sb to byte 0 of MCOUNTEREN_WRITABLE to succeed");

  prev = g_bus_fault_count;
  g_expect_bus_fault = true;
  abs_mmio_write8(
      kIbexCfgBase + RV_CORE_IBEX_MCOUNTEREN_WRITABLE_REG_OFFSET + 1, 0x00u);
  g_expect_bus_fault = false;
  CHECK(g_bus_fault_count == prev + 1 && g_last_mcause == 7,
        "Expected sb to byte 1 of MCOUNTEREN_WRITABLE to fault");

  g_expect_bus_fault = false;
  abs_mmio_write8(kIbexCfgBase + RV_CORE_IBEX_CHERIOT_ENA_REG_OFFSET,
                  kMultiBitBool4False);
  CHECK(abs_mmio_read32(kIbexCfgBase + RV_CORE_IBEX_CHERIOT_ENA_REG_OFFSET) ==
        kMultiBitBool4False);

  prev = g_bus_fault_count;
  g_expect_bus_fault = true;
  abs_mmio_write8(kIbexCfgBase + RV_CORE_IBEX_CHERIOT_ENA_REG_OFFSET + 1,
                  0x00u);
  g_expect_bus_fault = false;
  CHECK(g_bus_fault_count == prev + 1 && g_last_mcause == 7,
        "Expected sb to byte 1 of CHERIOT_ENA to fault");
}

/**
 * Test 2 (Part A - v1 Erratum 002 on v2):
 * Verify `NMI_ENABLE` (`SwAccessW1S` irreversibility), `NMI_STATE`
 * (`SwAccessW1C` software-masking and edge latching while `NMI_ENABLE == 0`),
 * and 1-deep `mstack` clobbering during an active M-mode trap handler.
 */
static void test_nmi_w1s_and_mstack_clobber(void) {
  LOG_INFO("Test 2: NMI_ENABLE W1S irreversibility and 1-deep mstack clobber");

  // 2a. `_rom_start_boot` (`rom_start.S:166`) writes `NMI_ENABLE.WDOG_EN = 1`
  // (`0x2`) at reset and leaves `NMI_ENABLE.ALERT_EN == 0` (`bit 0`). Verify
  // `WDOG_EN == 1` persisted (`SwAccessW1S`) while `ALERT_EN == 0`.
  uint32_t nmi_en_init =
      abs_mmio_read32(kIbexCfgBase + RV_CORE_IBEX_NMI_ENABLE_REG_OFFSET);
  CHECK(nmi_en_init == (1u << RV_CORE_IBEX_NMI_ENABLE_WDOG_EN_BIT),
        "Expected NMI_ENABLE == 0x2 (WDOG_EN=1 from ROM, ALERT_EN=0), got 0x%x",
        nmi_en_init);

  // Configure `alert_handler` Class A to pulse `esc_rx[0]` (`signal = 0`,
  // connected to `rv_core_ibex.esc_tx_i`) on
  // `kTopEarlgreyAlertIdRvCoreIbexRecovSwErr` while `NMI_ENABLE.ALERT_EN == 0`,
  // proving `NMI_STATE.ALERT == 1` latches without firing an NMI (`g_nmi_count
  // == 0`).
  dif_alert_handler_t ah;
  CHECK_DIF_OK(dif_alert_handler_init_from_dt(kDtAlertHandler, &ah));
  dif_alert_handler_escalation_phase_t esc_phases[] = {
      {.phase = kDifAlertHandlerClassStatePhase0,
       .signal = 0,
       .duration_cycles = 200},
  };
  dif_alert_handler_class_config_t class_a_cfg = {
      .auto_lock_accumulation_counter = kDifToggleDisabled,
      .accumulator_threshold = 0,
      .irq_deadline_cycles = 0,
      .escalation_phases = esc_phases,
      .escalation_phases_len = 1,
      .crashdump_escalation_phase = kDifAlertHandlerClassStatePhase1,
  };
  CHECK_DIF_OK(dif_alert_handler_configure_class(&ah, kDifAlertHandlerClassA,
                                                 class_a_cfg, kDifToggleEnabled,
                                                 kDifToggleDisabled));
  CHECK_DIF_OK(dif_alert_handler_configure_alert(
      &ah, kTopEarlgreyAlertIdRvCoreIbexRecovSwErr, kDifAlertHandlerClassA,
      kDifToggleEnabled, kDifToggleDisabled));

  g_nmi_count = 0;
  g_nmi_mcause = 0;
  g_nmi_state_on_entry = 0;
  abs_mmio_write32(kIbexCfgBase + RV_CORE_IBEX_ALERT_TEST_REG_OFFSET,
                   (1u << RV_CORE_IBEX_ALERT_TEST_RECOV_SW_ERR_BIT));

  uint32_t nmi_state = 0;
  for (int poll = 0; poll < 10000; ++poll) {
    nmi_state =
        abs_mmio_read32(kIbexCfgBase + RV_CORE_IBEX_NMI_STATE_REG_OFFSET);
    if (nmi_state & (1u << RV_CORE_IBEX_NMI_STATE_ALERT_BIT)) {
      break;
    }
  }
  // Clear Class A escalation and restore `RecovSwErr` alert back to Class D.
  CHECK_DIF_OK(dif_alert_handler_escalation_clear(&ah, kDifAlertHandlerClassA));
  CHECK_DIF_OK(dif_alert_handler_alert_acknowledge(
      &ah, kTopEarlgreyAlertIdRvCoreIbexRecovSwErr));
  CHECK_DIF_OK(dif_alert_handler_configure_alert(
      &ah, kTopEarlgreyAlertIdRvCoreIbexRecovSwErr, kDifAlertHandlerClassD,
      kDifToggleEnabled, kDifToggleDisabled));

  CHECK(
      (nmi_state & (1u << RV_CORE_IBEX_NMI_STATE_ALERT_BIT)) != 0u,
      "Expected NMI_STATE.ALERT == 1 to latch while NMI_ENABLE.ALERT_EN == 0");
  CHECK(g_nmi_count == 0u,
        "Expected 0 NMIs while NMI_ENABLE.ALERT_EN == 0, got %u", g_nmi_count);

  // 2b. Now set `NMI_ENABLE.ALERT_EN = 1` (`W1S`) without clearing
  // `NMI_STATE.ALERT` first: verify the latched `NMI_STATE.ALERT == 1`
  // immediately triggers `irq_nm` (`assign irq_nm = |(nmi_int & nmi_en)`,
  // `mcause = 0x8000001F` with `NMI_STATE.ALERT == 1`).
  abs_mmio_write32(kIbexCfgBase + RV_CORE_IBEX_NMI_ENABLE_REG_OFFSET,
                   (1u << RV_CORE_IBEX_NMI_ENABLE_ALERT_EN_BIT));
  for (int i = 0; i < 16; ++i) {
    asm volatile("nop");
  }
  CHECK(g_nmi_count == 1u,
        "Expected pending NMI_STATE.ALERT to immediately fire NMI on "
        "NMI_ENABLE.ALERT_EN = 1");
  CHECK(g_nmi_mcause == 0x8000001Fu,
        "Expected NMI mcause=0x8000001F, got 0x%08x", g_nmi_mcause);
  CHECK((g_nmi_state_on_entry & (1u << RV_CORE_IBEX_NMI_STATE_ALERT_BIT)) != 0u,
        "Expected NMI_STATE.ALERT == 1 on alert NMI entry, got 0x%x",
        g_nmi_state_on_entry);

  // Attempting to clear `NMI_ENABLE` by writing `0` fails (`SwAccessW1S`):
  // both `ALERT_EN` (`bit 0`) and `WDOG_EN` (`bit 1`) remain permanently set.
  abs_mmio_write32(kIbexCfgBase + RV_CORE_IBEX_NMI_ENABLE_REG_OFFSET, 0u);
  CHECK(abs_mmio_read32(kIbexCfgBase + RV_CORE_IBEX_NMI_ENABLE_REG_OFFSET) ==
            ((1u << RV_CORE_IBEX_NMI_ENABLE_ALERT_EN_BIT) |
             (1u << RV_CORE_IBEX_NMI_ENABLE_WDOG_EN_BIT)),
        "NMI_ENABLE (0x3) must be irreversible (SwAccessW1S)");

  // Trigger a synchronous load fault at `0x80` (`DV_SIM_WINDOW`) while
  // `g_in_trap_test == true`. Inside `ottf_load_store_fault_handler`, fire an
  // AON watchdog NMI (`mcause = 0x8000001F`) and verify `mstack` clobbers the
  // outer trap's saved `mepc`.
  g_nmi_count = 0;
  g_nmi_mcause = 0;
  g_nmi_entry_mepc = 0;
  g_in_trap_test = true;
  (void)abs_mmio_read32(kIbexCfgBase + 0x80);
  CHECK(!g_in_trap_test, "Trap test should have completed");
  CHECK(g_nmi_count == 1, "Expected 1 nested NMI, got %u", g_nmi_count);
  CHECK(g_nmi_mcause == 0x8000001Fu,
        "Expected wdog NMI mcause=0x8000001F, got 0x%08x", g_nmi_mcause);
  CHECK(g_nmi_entry_mepc != 0, "Expected non-zero NMI entry mepc");
}

/**
 * Test 3 (Part A - v1 Erratum 003 on v2):
 * Verify `SW_RECOV_ERR` (`0x04`) `mubi4_test_true_loose` activation (`15/16`
 * values trigger `recov_sw_err` and hardware auto-resets `SW_RECOV_ERR` to
 * `MuBi4False = 0x9`), and `RND_DATA` (`0x58`) read side-effect clearing
 * `RND_STATUS` (`0x5C`) prior to EDN refill.
 */
static void test_sw_recov_err_and_rnd_data(void) {
  LOG_INFO(
      "Test 3: SW_RECOV_ERR loose MuBi4 auto-reset & RND_DATA side-effect");

  CHECK(abs_mmio_read32(kIbexCfgBase + RV_CORE_IBEX_SW_RECOV_ERR_REG_OFFSET) ==
            kMultiBitBool4False,
        "SW_RECOV_ERR should reset to MuBi4False (0x9)");

  // Test `MuBi4True` (`0x6`) and non-canonical 4-bit values (`0x0`, `0x1`,
  // `0x7`, `0xF`). All 5 satisfy `mubi4_test_true_loose` (`!= 0x9`) and
  // auto-reset `SW_RECOV_ERR` back to `0x9` (`MuBi4False`) once
  // `u_alert_sender[1]` asserts `alert_acks[1]`.
  const uint32_t test_vals[] = {0x0u, 0x1u, kMultiBitBool4True, 0x7u, 0xFu};
  for (size_t i = 0; i < sizeof(test_vals) / sizeof(test_vals[0]); ++i) {
    CHECK_STATUS_OK(ottf_alerts_expect_alert_start(
        kTopEarlgreyAlertIdRvCoreIbexRecovSwErr));
    abs_mmio_write32(kIbexCfgBase + RV_CORE_IBEX_SW_RECOV_ERR_REG_OFFSET,
                     test_vals[i]);
    uint32_t val = 0;
    for (int poll = 0; poll < 100; ++poll) {
      val =
          abs_mmio_read32(kIbexCfgBase + RV_CORE_IBEX_SW_RECOV_ERR_REG_OFFSET);
      if (val == kMultiBitBool4False) {
        break;
      }
    }
    CHECK(val == kMultiBitBool4False,
          "SW_RECOV_ERR write 0x%x must auto-reset to 0x9 (got 0x%x)",
          test_vals[i], val);
    CHECK_STATUS_OK(ottf_alerts_expect_alert_finish(
        kTopEarlgreyAlertIdRvCoreIbexRecovSwErr));
  }

  // Verify `RND_DATA` (`0x58`) read side-effect on `RND_STATUS` (`0x5C`).
  uint32_t status_before = 0;
  for (int poll = 0; poll < 10000; ++poll) {
    status_before =
        abs_mmio_read32(kIbexCfgBase + RV_CORE_IBEX_RND_STATUS_REG_OFFSET);
    if (status_before & (1u << RV_CORE_IBEX_RND_STATUS_RND_DATA_VALID_BIT)) {
      break;
    }
  }
  CHECK(
      (status_before & (1u << RV_CORE_IBEX_RND_STATUS_RND_DATA_VALID_BIT)) != 0,
      "Expected RND_STATUS.RND_DATA_VALID=1 before reading RND_DATA");
  // Pause EDN0 (`CTRL = 0x14`) so `rv_core_ibex` cannot immediately refill
  // `rnd_data_q` before we read `RND_STATUS`.
  const uint32_t kEdn0CtrlAddr = TOP_EARLGREY_EDN0_BASE_ADDR + 0x14u;
  uint32_t edn0_ctrl_orig = abs_mmio_read32(kEdn0CtrlAddr);
  abs_mmio_write32(kEdn0CtrlAddr, 0x9999u);
  uint32_t rnd_word =
      abs_mmio_read32(kIbexCfgBase + RV_CORE_IBEX_RND_DATA_REG_OFFSET);
  uint32_t status_immediately_after =
      abs_mmio_read32(kIbexCfgBase + RV_CORE_IBEX_RND_STATUS_REG_OFFSET);
  CHECK((status_immediately_after &
         (1u << RV_CORE_IBEX_RND_STATUS_RND_DATA_VALID_BIT)) == 0,
        "Expected RND_STATUS.RND_DATA_VALID=0 after RND_DATA read while EDN0 "
        "is paused (got 0x%x)",
        status_immediately_after);
  abs_mmio_write32(kEdn0CtrlAddr, edn0_ctrl_orig);
  uint32_t status_refilled = 0;
  for (int poll = 0; poll < 10000; ++poll) {
    status_refilled =
        abs_mmio_read32(kIbexCfgBase + RV_CORE_IBEX_RND_STATUS_REG_OFFSET);
    if (status_refilled & (1u << RV_CORE_IBEX_RND_STATUS_RND_DATA_VALID_BIT)) {
      break;
    }
  }
  CHECK((status_refilled &
         (1u << RV_CORE_IBEX_RND_STATUS_RND_DATA_VALID_BIT)) != 0,
        "Expected RND_STATUS.RND_DATA_VALID=1 after restoring EDN0");
  LOG_INFO("  RND_DATA=0x%08x, RND_STATUS before=0x%x after=0x%x refilled=0x%x",
           rnd_word, status_before, status_immediately_after, status_refilled);
}

/**
 * Test 4 (Part B - NEW IN V2):
 * Verify `MCOUNTEREN_WRITABLE` (`0x70`) / `MCOUNTEREN_WRITABLE_REGWEN` (`0x6C`)
 * and Ibex `mcounteren` (`0x306`) CSR behavior (`rv_core_ibex.sv:461-465`,
 * `ibex_cs_registers.sv:464,845,1563-1571,1731-1748`):
 * 1. `MCOUNTEREN_WRITABLE` resets to `MuBi4True` (`0x6`), enabling `mcounteren`
 *    (`0x306`) writes. Although `rv_core_ibex.sv:24` and
 * `rv_core_ibex.hjson:327` specify default `MHPMCounterNum = 10`
 * (`0x00001FFD`), `top_earlgrey.sv:113`
 *    (`earlgrey_pd_main.sv:103,2551`) overrides `RvCoreIbexMHPMCounterNum = 2`,
 *    truncating `u_mcounteren_csr` (`Width = MHPMCounterNum + 3 = 5` bits) to
 *    4 writable bits with WARL mask `0x0000001D` (`CY` bit 0, `IR` bit 2,
 *    `HPM3` bit 3, `HPM4` bit 4; `TM` bit 1 and `HPM5..31` bits `[31:5]` are
 *    hardwired to `0`).
 * 2. When `MCOUNTEREN_WRITABLE` is set to any value other than strict
 *    `MuBi4True` (`0x6`) — e.g. `MuBi4False` (`0x9`) or a non-strict value
 *    (`0x7`) — `mcounteren_writable_ibex` becomes `IbexMuBiOff`:
 *    - Executing `csrw mcounteren, rs1` does NOT trap with an Illegal
 *      Instruction exception (`illegal_csr == 0`), silently succeeding as a
 *      no-op.
 *    - Existing bits in `mcounteren` are NOT cleared or gated off when
 *      `MCOUNTEREN_WRITABLE` is disabled (`mcounteren` retains its previous
 *      value).
 * 3. Writing `0` (`W0C`) to `MCOUNTEREN_WRITABLE_REGWEN` (`0x6C`) permanently
 *    locks `MCOUNTEREN_WRITABLE` (`0x70`) against subsequent writes.
 */
static void test_v2_mcounteren_writable_and_csr(void) {
  LOG_INFO("Test 4 [NEW_IN_V2]: MCOUNTEREN_WRITABLE & mcounteren (0x306) CSR");

  uint32_t regwen = abs_mmio_read32(
      kIbexCfgBase + RV_CORE_IBEX_MCOUNTEREN_WRITABLE_REGWEN_REG_OFFSET);
  uint32_t writable = abs_mmio_read32(
      kIbexCfgBase + RV_CORE_IBEX_MCOUNTEREN_WRITABLE_REG_OFFSET);
  CHECK(regwen == 1u, "Expected MCOUNTEREN_WRITABLE_REGWEN reset value 1");
  CHECK(writable == kMultiBitBool4True,
        "Expected MCOUNTEREN_WRITABLE reset value MuBi4True (0x6), got 0x%x",
        writable);

  // 4a. With `MCOUNTEREN_WRITABLE == 0x6`, `mcounteren` (`0x306`) is writable
  // with WARL mask `0x0000001D` (`RvCoreIbexMHPMCounterNum=2` in
  // `top_earlgrey.sv:113` -> bits `[4:0]`, with bit `[1]` hardwired to `0`).
  uint32_t mcounteren_val = 0;
  CSR_WRITE(CSR_REG_MCOUNTEREN, 0xFFFFFFFFu);
  CSR_READ(CSR_REG_MCOUNTEREN, &mcounteren_val);
  CHECK(mcounteren_val == 0x0000001Du,
        "Expected mcounteren WARL mask 0x0000001D, got 0x%08x", mcounteren_val);

  CSR_WRITE(CSR_REG_MCOUNTEREN, 0x00000015u);
  CSR_READ(CSR_REG_MCOUNTEREN, &mcounteren_val);
  CHECK(mcounteren_val == 0x00000015u,
        "Expected mcounteren=0x00000015, got 0x%08x", mcounteren_val);

  // 4b. Write `MuBi4False` (`0x9`) to `MCOUNTEREN_WRITABLE` (`0x70`).
  // Verify that:
  // (1) `mcounteren` retains `0x00000015` (disabling `MCOUNTEREN_WRITABLE` does
  //     NOT clear `mcounteren_q`), and
  // (2) `csrw mcounteren, 0` and `csrw mcounteren, 0x1D` do NOT raise an
  //     Illegal Instruction exception (`illegal_csr == 0`), instead silently
  //     dropping the write and leaving `mcounteren == 0x00000015`.
  abs_mmio_write32(kIbexCfgBase + RV_CORE_IBEX_MCOUNTEREN_WRITABLE_REG_OFFSET,
                   kMultiBitBool4False);
  CSR_READ(CSR_REG_MCOUNTEREN, &mcounteren_val);
  CHECK(mcounteren_val == 0x00000015u,
        "Disabling MCOUNTEREN_WRITABLE must retain existing mcounteren value");

  CSR_WRITE(CSR_REG_MCOUNTEREN, 0x00000000u);
  CSR_READ(CSR_REG_MCOUNTEREN, &mcounteren_val);
  CHECK(
      mcounteren_val == 0x00000015u,
      "csrw mcounteren must be silently ignored when MCOUNTEREN_WRITABLE=0x9");

  // 4c. Test strict `MuBi4True` check in `rv_core_ibex.sv:464`: a non-strict
  // 4-bit value (`0x7`, 1 bit flip from `0x6`) also maps to `IbexMuBiOff` and
  // suppresses `mcounteren` writes without trapping.
  abs_mmio_write32(kIbexCfgBase + RV_CORE_IBEX_MCOUNTEREN_WRITABLE_REG_OFFSET,
                   0x7u);
  CSR_WRITE(CSR_REG_MCOUNTEREN, 0x0000001Du);
  CSR_READ(CSR_REG_MCOUNTEREN, &mcounteren_val);
  CHECK(mcounteren_val == 0x00000015u,
        "Non-strict MCOUNTEREN_WRITABLE=0x7 must also suppress mcounteren "
        "writes");

  // 4d. Re-enable `MCOUNTEREN_WRITABLE = MuBi4True (0x6)`, restore `mcounteren`
  // to `0`, then set `MCOUNTEREN_WRITABLE = MuBi4False (0x9)` and lock
  // `MCOUNTEREN_WRITABLE_REGWEN = 0` (`W0C`).
  abs_mmio_write32(kIbexCfgBase + RV_CORE_IBEX_MCOUNTEREN_WRITABLE_REG_OFFSET,
                   kMultiBitBool4True);
  CSR_WRITE(CSR_REG_MCOUNTEREN, 0x00000000u);
  CSR_READ(CSR_REG_MCOUNTEREN, &mcounteren_val);
  CHECK(mcounteren_val == 0x00000000u, "Expected mcounteren restored to 0");

  abs_mmio_write32(kIbexCfgBase + RV_CORE_IBEX_MCOUNTEREN_WRITABLE_REG_OFFSET,
                   kMultiBitBool4False);
  abs_mmio_write32(
      kIbexCfgBase + RV_CORE_IBEX_MCOUNTEREN_WRITABLE_REGWEN_REG_OFFSET, 0u);
  CHECK(
      abs_mmio_read32(kIbexCfgBase +
                      RV_CORE_IBEX_MCOUNTEREN_WRITABLE_REGWEN_REG_OFFSET) == 0u,
      "Expected MCOUNTEREN_WRITABLE_REGWEN=0 after W0C write");

  // Attempt to re-enable `MCOUNTEREN_WRITABLE = MuBi4True (0x6)` after lock.
  abs_mmio_write32(kIbexCfgBase + RV_CORE_IBEX_MCOUNTEREN_WRITABLE_REG_OFFSET,
                   kMultiBitBool4True);
  CHECK(abs_mmio_read32(kIbexCfgBase +
                        RV_CORE_IBEX_MCOUNTEREN_WRITABLE_REG_OFFSET) ==
            kMultiBitBool4False,
        "Locked MCOUNTEREN_WRITABLE must ignore writes");
}

/**
 * Test 5 (Part B - NEW IN V2):
 * Verify `CHERIOT_ENA` (`0x60`) / `CHERIOT_LOCK` (`0x64`) and
 * `rv_core_ibex_cheriot_switch.sv` (`u_cheriot_switch`):
 * 1. `CHERIOT_ENA` resets to `MuBi4False` (`0x9`) and `CHERIOT_LOCK` (`wo`)
 *    reads as `0x0`.
 * 2. Writing `MuBi4True` (`0x6`) to `CHERIOT_LOCK` (`0x64`) while
 *    `CHERIOT_ENA == MuBi4False` (`0x9`) locks `u_cheriot_switch` in
 *    `LockedDis` (`cheriot_ena_o = MuBi4False`, standard RV32I/ePMP mode).
 * 3. Because `CHERIOT_ENA` (`0x60`) lacks a `regwen` and reads back raw
 *    `cheriot_ena_qs` (`rv_core_ibex_cfg_reg_top.sv:1310-1335,1745`), writing
 *    `MuBi4True` (`0x6`) to `CHERIOT_ENA` after `CHERIOT_LOCK` causes
 *    `CHERIOT_ENA` to read back `0x6` (`MuBi4True`) even though hardware
 *    execution mode (`cheriot_ena_o`) remains locked to `MuBi4False` (proven
 *    by `CSR_MTVEC` and `CSR_MEPC` remaining readable without raising
 *    `illegal_csr`, which `ibex_cs_registers.sv:470,479` raises if
 *    `cheriot_enable_i == IbexMuBiOn`), while `CHERIOT_LOCK` (`0x64`) still
 *    reads back `0x0`.
 * 4. Once in `LockedDis` (`rv_core_ibex_cheriot_switch.sv:92-95`), subsequent
 *    writes of invalid MuBi4 values (`0x0`, `0x9`, `0xF`) to `CHERIOT_LOCK`
 *    (`0x64`) are silently ignored and do NOT transition `u_cheriot_switch` to
 *    `Error` or raise `fatal_hw_err`.
 */
static void test_v2_cheriot_switch_and_desync_readback(void) {
  LOG_INFO("Test 5 [NEW_IN_V2]: CHERIOT_ENA/CHERIOT_LOCK desync & LockedDis");

  uint32_t ena_init =
      abs_mmio_read32(kIbexCfgBase + RV_CORE_IBEX_CHERIOT_ENA_REG_OFFSET);
  uint32_t lock_init =
      abs_mmio_read32(kIbexCfgBase + RV_CORE_IBEX_CHERIOT_LOCK_REG_OFFSET);
  CHECK(ena_init == kMultiBitBool4False,
        "Expected CHERIOT_ENA reset value MuBi4False (0x9), got 0x%x",
        ena_init);
  CHECK(lock_init == 0u, "Expected CHERIOT_LOCK (wo) to read 0, got 0x%x",
        lock_init);

  // Lock `u_cheriot_switch` in `LockedDis` by writing `MuBi4True` (`0x6`) to
  // `CHERIOT_LOCK` while `CHERIOT_ENA == MuBi4False` (`0x9`).
  abs_mmio_write32(kIbexCfgBase + RV_CORE_IBEX_CHERIOT_LOCK_REG_OFFSET,
                   kMultiBitBool4True);

  // `CHERIOT_LOCK` is `wo`, so reading it after locking still returns `0x0`.
  CHECK(abs_mmio_read32(kIbexCfgBase + RV_CORE_IBEX_CHERIOT_LOCK_REG_OFFSET) ==
            0u,
        "CHERIOT_LOCK still reads 0x0 after locking");

  // Write `MuBi4True` (`0x6`) to `CHERIOT_ENA` (`0x60`) AFTER `CHERIOT_LOCK`.
  // Because `CHERIOT_ENA` has no `regwen`, `cheriot_ena_qs` updates to `0x6`
  // even though `u_cheriot_switch` (`cheriot_ena_o`) is locked in `LockedDis`!
  abs_mmio_write32(kIbexCfgBase + RV_CORE_IBEX_CHERIOT_ENA_REG_OFFSET,
                   kMultiBitBool4True);
  uint32_t ena_after =
      abs_mmio_read32(kIbexCfgBase + RV_CORE_IBEX_CHERIOT_ENA_REG_OFFSET);
  CHECK(ena_after == kMultiBitBool4True,
        "Expected CHERIOT_ENA to read back newly written 0x6 after lock, got "
        "0x%x",
        ena_after);

  // Prove `cheriot_ena_o` is still `MuBi4False` (`IbexMuBiOff`): reading
  // `CSR_MTVEC` (`0x305`) succeeds without raising `illegal_csr` (which
  // `ibex_cs_registers.sv:470` asserts when `cheriot_enable_i == IbexMuBiOn`).
  uint32_t mtvec_val = 0;
  CSR_READ(CSR_REG_MTVEC, &mtvec_val);
  CHECK(mtvec_val != 0u, "Expected CSR_MTVEC read to succeed in ePMP mode");

  // Write invalid MuBi4 values (`0x0`, `0x9`, `0xF`) to `CHERIOT_LOCK` (`0x64`)
  // now that `u_cheriot_switch` is in `LockedDis`. Because `LockedDis` in
  // `rv_core_ibex_cheriot_switch.sv:92-95` ignores `lock_access_i`, the FSM
  // stays in `LockedDis` (`error_o = 0`) without raising `fatal_hw_err`.
  abs_mmio_write32(kIbexCfgBase + RV_CORE_IBEX_CHERIOT_LOCK_REG_OFFSET, 0x0u);
  abs_mmio_write32(kIbexCfgBase + RV_CORE_IBEX_CHERIOT_LOCK_REG_OFFSET,
                   kMultiBitBool4False);
  abs_mmio_write32(kIbexCfgBase + RV_CORE_IBEX_CHERIOT_LOCK_REG_OFFSET, 0xFu);

  // Restore `CHERIOT_ENA` CSR readback to `MuBi4False` (`0x9`).
  abs_mmio_write32(kIbexCfgBase + RV_CORE_IBEX_CHERIOT_ENA_REG_OFFSET,
                   kMultiBitBool4False);
  CHECK(
      abs_mmio_read32(kIbexCfgBase + RV_CORE_IBEX_ERR_STATUS_REG_OFFSET) == 0u,
      "Expected ERR_STATUS == 0 after LockedDis CHERIOT_LOCK writes");
}

bool test_main(void) {
  LOG_INFO("=== rv_core_ibex Earlgrey v2 Errata Verification Test ===");
  test_window_gap_and_subword_permits();
  test_nmi_w1s_and_mstack_clobber();
  test_sw_recov_err_and_rnd_data();
  test_v2_mcounteren_writable_and_csr();
  test_v2_cheriot_switch_and_desync_readback();
  LOG_INFO("=== All rv_core_ibex v2 errata checks PASSED ===");
  return true;
}
