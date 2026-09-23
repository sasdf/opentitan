# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load(
    "//rules/opentitan:defs.bzl",
    "fpga_params",
    "opentitan_test",
    "qemu_params",
)

package(default_visibility = ["//visibility:public"])

# OpenTitan Earlgrey v1.0.0 Hardware & Specification Errata Confirmation Suite
# Each module errata test is placed inside its dedicated anchor block below to prevent
# 3-way merge conflicts across parallel worker agents.

# ----------------------------------------------------------------------------
# Priority P01 Module: keymgr
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p01_keymgr ===
# === END ERRATA TEST: p01_keymgr ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P02 Module: hmac
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p02_hmac ===
# === END ERRATA TEST: p02_hmac ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P03 Module: otbn
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p03_otbn ===
# === END ERRATA TEST: p03_otbn ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P04 Module: pwm
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p04_pwm ===
# === END ERRATA TEST: p04_pwm ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P05 Module: adc_ctrl
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p05_adc_ctrl ===
# === END ERRATA TEST: p05_adc_ctrl ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P06 Module: kmac
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p06_kmac ===
# === END ERRATA TEST: p06_kmac ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P07 Module: spi_device
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p07_spi_device ===
# === END ERRATA TEST: p07_spi_device ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P08 Module: csrng
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p08_csrng ===
# === END ERRATA TEST: p08_csrng ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P09 Module: flash_ctrl
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p09_flash_ctrl ===
# === END ERRATA TEST: p09_flash_ctrl ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P10 Module: uart
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p10_uart ===
opentitan_test(
    name = "uart_errata_test",
    srcs = ["uart_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/ip/uart/data:uart_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:irq",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA TEST: p10_uart ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P11 Module: aon_timer
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p11_aon_timer ===
# === END ERRATA TEST: p11_aon_timer ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P12 Module: pattgen
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p12_pattgen ===
# === END ERRATA TEST: p12_pattgen ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P13 Module: edn
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p13_edn ===
# === END ERRATA TEST: p13_edn ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P14 Module: rv_plic
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p14_rv_plic ===
# === END ERRATA TEST: p14_rv_plic ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P15 Module: pinmux
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p15_pinmux ===
# === END ERRATA TEST: p15_pinmux ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P16 Module: rv_timer
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p16_rv_timer ===
# === END ERRATA TEST: p16_rv_timer ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P17 Module: usbdev
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p17_usbdev ===
# === END ERRATA TEST: p17_usbdev ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P18 Module: entropy_src
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p18_entropy_src ===
# === END ERRATA TEST: p18_entropy_src ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P19 Module: ast
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p19_ast ===
# === END ERRATA TEST: p19_ast ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P20 Module: aes
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p20_aes ===
# === END ERRATA TEST: p20_aes ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P21 Module: rv_dm
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p21_rv_dm ===
# === END ERRATA TEST: p21_rv_dm ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P22 Module: rstmgr
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p22_rstmgr ===
# === END ERRATA TEST: p22_rstmgr ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P23 Module: lc_ctrl
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p23_lc_ctrl ===
# === END ERRATA TEST: p23_lc_ctrl ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P24 Module: otp_ctrl
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p24_otp_ctrl ===
# === END ERRATA TEST: p24_otp_ctrl ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P25 Module: sram_ctrl
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p25_sram_ctrl ===
# === END ERRATA TEST: p25_sram_ctrl ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P26 Module: clkmgr
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p26_clkmgr ===
# === END ERRATA TEST: p26_clkmgr ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P27 Module: alert_handler
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p27_alert_handler ===
# === END ERRATA TEST: p27_alert_handler ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P28 Module: sensor_ctrl
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p28_sensor_ctrl ===
# === END ERRATA TEST: p28_sensor_ctrl ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P29 Module: pwrmgr
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p29_pwrmgr ===
# === END ERRATA TEST: p29_pwrmgr ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P30 Module: spi_host
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p30_spi_host ===
# === END ERRATA TEST: p30_spi_host ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P31 Module: i2c
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p31_i2c ===
# === END ERRATA TEST: p31_i2c ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P32 Module: sysrst_ctrl
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p32_sysrst_ctrl ===
# === END ERRATA TEST: p32_sysrst_ctrl ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P33 Module: rom_ctrl
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p33_rom_ctrl ===
# === END ERRATA TEST: p33_rom_ctrl ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P34 Module: rv_core_ibex
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p34_rv_core_ibex ===
# === END ERRATA TEST: p34_rv_core_ibex ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P35 Module: gpio
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p35_gpio ===
# === END ERRATA TEST: p35_gpio ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P36 Module: padring
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p36_padring ===
# === END ERRATA TEST: p36_padring ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P37 Module: top_earlgrey
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p37_top_earlgrey ===
# === END ERRATA TEST: p37_top_earlgrey ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P38 Module: xbar_main_and_xbar_peri
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p38_xbar_main_and_xbar_peri ===
# === END ERRATA TEST: p38_xbar_main_and_xbar_peri ===
#
#
#
