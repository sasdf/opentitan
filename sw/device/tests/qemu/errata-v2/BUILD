# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load(
    "//rules/opentitan:defs.bzl",
    "fpga_params",
    "opentitan_test",
)

package(default_visibility = ["//visibility:public"])

# OpenTitan Earlgrey v2 (trunk-v2) Hardware & Specification Errata Confirmation Suite (CW340 FPGA)
# Each module errata-v2 test must be placed inside its dedicated anchor block below
# to prevent 3-way merge conflicts across concurrent worker agents.

# ----------------------------------------------------------------------------
# Priority P01 Module: keymgr
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p01_keymgr ===
opentitan_test(
    name = "keymgr_errata_v2_test",
    srcs = ["keymgr_errata_v2_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top:keymgr_dpe_c_regs",
        "//hw/top:kmac_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:keymgr_dpe",
        "//sw/device/lib/dif:kmac",
        "//sw/device/lib/dif:rstmgr",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:irq",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:entropy_testutils",
        "//sw/device/lib/testing:kmac_testutils",
        "//sw/device/lib/testing:rstmgr_testutils",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA-V2 TEST: p01_keymgr ===

# ----------------------------------------------------------------------------
# Priority P02 Module: flash_ctrl
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p02_flash_ctrl ===
# === END ERRATA-V2 TEST: p02_flash_ctrl ===

# ----------------------------------------------------------------------------
# Priority P03 Module: otp_ctrl
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p03_otp_ctrl ===
opentitan_test(
    name = "otp_ctrl_errata_v2_test",
    srcs = ["otp_ctrl_errata_v2_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top:otp_ctrl_c_regs",
        "//hw/top/dt",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/arch:device",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:alert_handler",
        "//sw/device/lib/dif:otp_ctrl",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA-V2 TEST: p03_otp_ctrl ===

# ----------------------------------------------------------------------------
# Priority P04 Module: lc_ctrl
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p04_lc_ctrl ===
opentitan_test(
    name = "lc_ctrl_errata_v2_test",
    srcs = ["lc_ctrl_errata_v2_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:fpga_cw340_test_rom": None,
    },
    fpga = fpga_params(
        timeout = "short",
    ),
    deps = [
        "//hw/top:lc_ctrl_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:memory",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/base:multibits",
        "//sw/device/lib/dif:lc_ctrl",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_alerts",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA-V2 TEST: p04_lc_ctrl ===

# ----------------------------------------------------------------------------
# Priority P05 Module: otbn
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p05_otbn ===
opentitan_test(
    name = "otbn_errata_v2_test",
    srcs = ["otbn_errata_v2_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:fpga_cw340_test_rom": None,
    },
    fpga = fpga_params(
        timeout = "short",
    ),
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:otbn",
        "//sw/device/lib/dif:rv_core_ibex",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:entropy_testutils",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA-V2 TEST: p05_otbn ===

# ----------------------------------------------------------------------------
# Priority P06 Module: kmac
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p06_kmac ===
opentitan_test(
    name = "kmac_errata_v2_test",
    srcs = ["kmac_errata_v2_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:fpga_cw340_test_rom": None,
    },
    fpga = fpga_params(
        timeout = "short",
    ),
    deps = [
        "//hw/top:kmac_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/dif:kmac",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA-V2 TEST: p06_kmac ===

# ----------------------------------------------------------------------------
# Priority P07 Module: csrng
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p07_csrng ===
opentitan_test(
    name = "csrng_errata_v2_test",
    srcs = ["csrng_errata_v2_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top:alert_handler_c_regs",
        "//hw/top:csrng_c_regs",
        "//hw/top:edn_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:multibits",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA-V2 TEST: p07_csrng ===

# ----------------------------------------------------------------------------
# Priority P08 Module: edn
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p08_edn ===
opentitan_test(
    name = "edn_errata_v2_test",
    srcs = ["edn_errata_v2_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top:csrng_c_regs",
        "//hw/top:edn_c_regs",
        "//hw/top:entropy_src_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:edn",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:irq",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA-V2 TEST: p08_edn ===

# ----------------------------------------------------------------------------
# Priority P09 Module: usbdev
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p09_usbdev ===
# === END ERRATA-V2 TEST: p09_usbdev ===

# ----------------------------------------------------------------------------
# Priority P10 Module: uart
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p10_uart ===
# === END ERRATA-V2 TEST: p10_uart ===

# ----------------------------------------------------------------------------
# Priority P11 Module: i2c
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p11_i2c ===
# === END ERRATA-V2 TEST: p11_i2c ===

# ----------------------------------------------------------------------------
# Priority P12 Module: spi_device
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p12_spi_device ===
# === END ERRATA-V2 TEST: p12_spi_device ===

# ----------------------------------------------------------------------------
# Priority P13 Module: spi_host
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p13_spi_host ===
# === END ERRATA-V2 TEST: p13_spi_host ===

# ----------------------------------------------------------------------------
# Priority P14 Module: rv_plic
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p14_rv_plic ===
opentitan_test(
    name = "rv_plic_errata_v2_test",
    srcs = ["rv_plic_errata_v2_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top:rv_plic_c_regs",
        "//hw/top:uart_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:rv_plic",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA-V2 TEST: p14_rv_plic ===

# ----------------------------------------------------------------------------
# Priority P15 Module: alert_handler
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p15_alert_handler ===
opentitan_test(
    name = "alert_handler_errata_v2_test",
    srcs = ["alert_handler_errata_v2_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top:alert_handler_c_regs",
        "//hw/top:uart_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA-V2 TEST: p15_alert_handler ===

# ----------------------------------------------------------------------------
# Priority P16 Module: rv_core_ibex
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p16_rv_core_ibex ===
opentitan_test(
    name = "rv_core_ibex_errata_v2_test",
    srcs = ["rv_core_ibex_errata_v2_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:fpga_cw340_test_rom": None,
    },
    fpga = fpga_params(
        timeout = "short",
    ),
    deps = [
        "//hw/top:aon_timer_c_regs",
        "//hw/top:rv_core_ibex_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:multibits",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_alerts",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA-V2 TEST: p16_rv_core_ibex ===

# ----------------------------------------------------------------------------
# Priority P17 Module: rv_dm
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p17_rv_dm ===
# === END ERRATA-V2 TEST: p17_rv_dm ===

# ----------------------------------------------------------------------------
# Priority P18 Module: entropy_src
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p18_entropy_src ===
opentitan_test(
    name = "entropy_src_errata_v2_test",
    srcs = ["entropy_src_errata_v2_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:fpga_cw340_test_rom": None,
    },
    fpga = fpga_params(
        timeout = "short",
    ),
    deps = [
        "//hw/top:entropy_src_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/base:multibits",
        "//sw/device/lib/dif:entropy_src",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:entropy_testutils",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_alerts",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA-V2 TEST: p18_entropy_src ===

# ----------------------------------------------------------------------------
# Priority P19 Module: aes
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p19_aes ===
opentitan_test(
    name = "aes_errata_v2_test",
    srcs = ["aes_errata_v2_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:fpga_cw340_test_rom": None,
    },
    fpga = fpga_params(
        timeout = "short",
    ),
    deps = [
        "//hw/top:aes_c_regs",
        "//hw/top:csrng_c_regs",
        "//hw/top:edn_c_regs",
        "//hw/top:entropy_src_c_regs",
        "//hw/top:rv_core_ibex_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:aes",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA-V2 TEST: p19_aes ===

# ----------------------------------------------------------------------------
# Priority P20 Module: hmac
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p20_hmac ===
# === END ERRATA-V2 TEST: p20_hmac ===

# ----------------------------------------------------------------------------
# Priority P21 Module: rom_ctrl
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p21_rom_ctrl ===
opentitan_test(
    name = "rom_ctrl_errata_v2_test",
    srcs = ["rom_ctrl_errata_v2_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top:alert_handler_c_regs",
        "//hw/top:rom_ctrl_c_regs",
        "//hw/top:rv_core_ibex_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:rom_ctrl",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA-V2 TEST: p21_rom_ctrl ===

# ----------------------------------------------------------------------------
# Priority P22 Module: sram_ctrl
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p22_sram_ctrl ===
opentitan_test(
    name = "sram_ctrl_errata_v2_test",
    srcs = ["sram_ctrl_errata_v2_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top:sram_ctrl_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:multibits",
        "//sw/device/lib/dif:sram_ctrl",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_alerts",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
        "//sw/device/silicon_creator/lib/drivers:retention_sram",
    ],
)
# === END ERRATA-V2 TEST: p22_sram_ctrl ===

# ----------------------------------------------------------------------------
# Priority P23 Module: rv_timer
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p23_rv_timer ===
opentitan_test(
    name = "rv_timer_errata_v2_test",
    srcs = ["rv_timer_errata_v2_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top:rv_timer_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:rv_timer",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA-V2 TEST: p23_rv_timer ===

# ----------------------------------------------------------------------------
# Priority P24 Module: aon_timer
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p24_aon_timer ===
# === END ERRATA-V2 TEST: p24_aon_timer ===

# ----------------------------------------------------------------------------
# Priority P25 Module: pwrmgr
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p25_pwrmgr ===
opentitan_test(
    name = "pwrmgr_errata_v2_test",
    srcs = ["pwrmgr_errata_v2_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top:aon_timer_c_regs",
        "//hw/top:otp_ctrl_c_regs",
        "//hw/top:pinmux_c_regs",
        "//hw/top:pwrmgr_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:memory",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:base",
        "//sw/device/lib/dif:pwrmgr",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:irq",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_alerts",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA-V2 TEST: p25_pwrmgr ===

# ----------------------------------------------------------------------------
# Priority P26 Module: clkmgr
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p26_clkmgr ===
# === END ERRATA-V2 TEST: p26_clkmgr ===

# ----------------------------------------------------------------------------
# Priority P27 Module: rstmgr
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p27_rstmgr ===
opentitan_test(
    name = "rstmgr_errata_v2_test",
    srcs = ["rstmgr_errata_v2_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top:alert_handler_c_regs",
        "//hw/top:rstmgr_c_regs",
        "//hw/top:rv_timer_c_regs",
        "//hw/top:spi_host_c_regs",
        "//hw/top:usbdev_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:multibits",
        "//sw/device/lib/dif:rstmgr",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA-V2 TEST: p27_rstmgr ===

# ----------------------------------------------------------------------------
# Priority P28 Module: pinmux
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p28_pinmux ===
# === END ERRATA-V2 TEST: p28_pinmux ===

# ----------------------------------------------------------------------------
# Priority P29 Module: gpio
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p29_gpio ===
# === END ERRATA-V2 TEST: p29_gpio ===

# ----------------------------------------------------------------------------
# Priority P30 Module: pwm
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p30_pwm ===
# === END ERRATA-V2 TEST: p30_pwm ===

# ----------------------------------------------------------------------------
# Priority P31 Module: pattgen
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p31_pattgen ===
# === END ERRATA-V2 TEST: p31_pattgen ===

# ----------------------------------------------------------------------------
# Priority P32 Module: sysrst_ctrl
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p32_sysrst_ctrl ===
# === END ERRATA-V2 TEST: p32_sysrst_ctrl ===

# ----------------------------------------------------------------------------
# Priority P33 Module: adc_ctrl
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p33_adc_ctrl ===
# === END ERRATA-V2 TEST: p33_adc_ctrl ===

# ----------------------------------------------------------------------------
# Priority P34 Module: sensor_ctrl
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p34_sensor_ctrl ===
# === END ERRATA-V2 TEST: p34_sensor_ctrl ===

# ----------------------------------------------------------------------------
# Priority P35 Module: ast
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p35_ast ===
# === END ERRATA-V2 TEST: p35_ast ===

# ----------------------------------------------------------------------------
# Priority P36 Module: xbar_main_and_xbar_peri
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p36_xbar_main_and_xbar_peri ===
# === END ERRATA-V2 TEST: p36_xbar_main_and_xbar_peri ===

# ----------------------------------------------------------------------------
# Priority P37 Module: top_earlgrey
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p37_top_earlgrey ===
# === END ERRATA-V2 TEST: p37_top_earlgrey ===

# ----------------------------------------------------------------------------
# Priority P38 Module: padring
# ----------------------------------------------------------------------------
# === BEGIN ERRATA-V2 TEST: p38_padring ===
# === END ERRATA-V2 TEST: p38_padring ===
