// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/silicon_creator/lib/drivers/ibex.h"

#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/base/csr.h"
#include "sw/device/lib/base/hardened.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/silicon_creator/lib/base/sec_mmio.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "rv_core_ibex_regs.h"
#include "rv_timer_regs.h"

enum {
  kBase = TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR,
};

uint32_t ibex_fpga_version(void) {
  return abs_mmio_read32(kBase + RV_CORE_IBEX_FPGA_INFO_REG_OFFSET);
}

void ibex_addr_remap_0_set(uint32_t matching_addr, uint32_t remap_addr,
                           size_t size) {
  // Work-around for opentitan#22884: Mask off bits below the alignment size
  // prior to programming the REMAP_ADDR register.
  size = size - 1;
  uint32_t match = (matching_addr & ~size) | size >> 1;
  remap_addr &= ~size;

  sec_mmio_write32(kBase + RV_CORE_IBEX_IBUS_ADDR_MATCHING_0_REG_OFFSET, match);
  sec_mmio_write32(kBase + RV_CORE_IBEX_DBUS_ADDR_MATCHING_0_REG_OFFSET, match);

  sec_mmio_write32(kBase + RV_CORE_IBEX_IBUS_REMAP_ADDR_0_REG_OFFSET,
                   remap_addr);
  sec_mmio_write32(kBase + RV_CORE_IBEX_DBUS_REMAP_ADDR_0_REG_OFFSET,
                   remap_addr);

  sec_mmio_write32(kBase + RV_CORE_IBEX_IBUS_ADDR_EN_0_REG_OFFSET, 1);
  sec_mmio_write32(kBase + RV_CORE_IBEX_DBUS_ADDR_EN_0_REG_OFFSET, 1);
  icache_invalidate();
}

uint32_t ibex_addr_remap_1_set(uint32_t matching_addr, uint32_t remap_addr,
                               size_t size) {
  // Work-around for opentitan#22884: Mask off bits below the alignment size
  // prior to programming the REMAP_ADDR register.
  size = size - 1;
  uint32_t match = (matching_addr & ~size) | size >> 1;
  remap_addr &= ~size;

  sec_mmio_write32(kBase + RV_CORE_IBEX_IBUS_ADDR_MATCHING_1_REG_OFFSET, match);
  sec_mmio_write32(kBase + RV_CORE_IBEX_DBUS_ADDR_MATCHING_1_REG_OFFSET, match);

  sec_mmio_write32(kBase + RV_CORE_IBEX_IBUS_REMAP_ADDR_1_REG_OFFSET,
                   remap_addr);
  sec_mmio_write32(kBase + RV_CORE_IBEX_DBUS_REMAP_ADDR_1_REG_OFFSET,
                   remap_addr);

  sec_mmio_write32(kBase + RV_CORE_IBEX_IBUS_ADDR_EN_1_REG_OFFSET, 1);
  sec_mmio_write32(kBase + RV_CORE_IBEX_DBUS_ADDR_EN_1_REG_OFFSET, 1);

  uint32_t wfi_iters = 0;
#ifdef OT_PLATFORM_RV32
  uint32_t timer_base = TOP_EARLGREY_RV_TIMER_BASE_ADDR;
  // Ensure timer is stopped.
  abs_mmio_write32(timer_base + RV_TIMER_CTRL_REG_OFFSET, 0);

  // Enable timer interrupt in MIE CSR (bit 7 is MTIE).
  uint32_t old_mie;
  CSR_READ(CSR_REG_MIE, &old_mie);
  CSR_WRITE(CSR_REG_MIE, old_mie | (1u << 7));

  // Initialize timer for 1:1 clock ratio (prescale=0, step=1).
  abs_mmio_write32(timer_base + RV_TIMER_TIMER_V_LOWER0_REG_OFFSET, 0);
  abs_mmio_write32(timer_base + RV_TIMER_TIMER_V_UPPER0_REG_OFFSET, 0);
  abs_mmio_write32(timer_base + RV_TIMER_COMPARE_UPPER0_0_REG_OFFSET, 0);
  abs_mmio_write32(timer_base + RV_TIMER_COMPARE_LOWER0_0_REG_OFFSET, 1024);
  abs_mmio_write32(timer_base + RV_TIMER_INTR_STATE0_REG_OFFSET, 1);
  abs_mmio_write32(timer_base + RV_TIMER_INTR_ENABLE0_REG_OFFSET, 1);
  uint32_t cfg = 0;
  cfg = bitfield_field32_write(cfg, RV_TIMER_CFG0_PRESCALE_FIELD, 0);
  cfg = bitfield_field32_write(cfg, RV_TIMER_CFG0_STEP_FIELD, 1);
  abs_mmio_write32(timer_base + RV_TIMER_CFG0_REG_OFFSET, cfg);

  abs_mmio_write32(timer_base + RV_TIMER_CTRL_REG_OFFSET, 1);

  // Trigger flush.
  icache_invalidate();

  while (true) {
    wait_for_interrupt();
    wfi_iters++;

    uint32_t cpuctrl;
    CSR_READ(CSR_REG_CPUCTRL, &cpuctrl);
    if ((cpuctrl & (1u << 8)) != 0) {
      break;
    }

    // Key not received yet. Clear timer interrupt and set for another 1024
    // cycles.
    abs_mmio_write32(timer_base + RV_TIMER_INTR_STATE0_REG_OFFSET, 1);
    uint32_t now =
        abs_mmio_read32(timer_base + RV_TIMER_TIMER_V_LOWER0_REG_OFFSET);
    abs_mmio_write32(timer_base + RV_TIMER_COMPARE_LOWER0_0_REG_OFFSET,
                     now + 1024);
  }

  // Disable timer and restore MIE CSR.
  abs_mmio_write32(timer_base + RV_TIMER_CTRL_REG_OFFSET, 0);
  CSR_WRITE(CSR_REG_MIE, old_mie);
#endif
  return (wfi_iters << 16) +
         abs_mmio_read32(timer_base + RV_TIMER_TIMER_V_LOWER0_REG_OFFSET);
}

uint32_t ibex_addr_remap_get(uint32_t index) {
  HARDENED_CHECK_LT(index, 2);
  index *= sizeof(uint32_t);
  if (abs_mmio_read32(kBase + RV_CORE_IBEX_IBUS_ADDR_EN_0_REG_OFFSET + index)) {
    return abs_mmio_read32(kBase + RV_CORE_IBEX_IBUS_REMAP_ADDR_0_REG_OFFSET +
                           index);
  } else {
    return 0;
  }
}

void ibex_addr_remap_lockdown(uint32_t index) {
  HARDENED_CHECK_LT(index, 2);
  index *= sizeof(uint32_t);
  sec_mmio_write32(kBase + RV_CORE_IBEX_IBUS_REGWEN_0_REG_OFFSET + index, 0);
  sec_mmio_write32(kBase + RV_CORE_IBEX_DBUS_REGWEN_0_REG_OFFSET + index, 0);
}

void ibex_enable_nmi(ibex_nmi_source_t nmi_src) {
  abs_mmio_write32(kBase + RV_CORE_IBEX_NMI_ENABLE_REG_OFFSET, nmi_src);
}

void ibex_clear_nmi(ibex_nmi_source_t nmi_src) {
  abs_mmio_write32(kBase + RV_CORE_IBEX_NMI_STATE_REG_OFFSET, nmi_src);
}

// `extern` declarations to give the inline functions in the corresponding
// header a link location.
extern void ibex_mcycle_zero(void);
extern uint32_t ibex_mcycle32(void);
extern uint64_t ibex_mcycle(void);
extern uint64_t ibex_time_to_cycles(uint64_t time_us);
