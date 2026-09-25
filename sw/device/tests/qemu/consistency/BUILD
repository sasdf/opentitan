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
# Priority P01 Module: keymgr_dpe
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p01_keymgr_dpe ===
opentitan_test(
    name = "keymgr_dpe_consistency_test",
    srcs = ["keymgr_dpe_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
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
        "//sw/device/lib/testing/test_framework:ottf_alerts",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END CONSISTENCY TEST: p01_keymgr_dpe ===

# ----------------------------------------------------------------------------
# Priority P02 Module: rram_ctrl
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p02_rram_ctrl ===
opentitan_test(
    name = "rram_ctrl_consistency_test",
    srcs = ["rram_ctrl_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top:rram_ctrl_c_regs",
        "//hw/top:rv_core_ibex_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
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
# === END CONSISTENCY TEST: p02_rram_ctrl ===

# ----------------------------------------------------------------------------
# Priority P03 Module: otp_ctrl
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p03_otp_ctrl ===
opentitan_test(
    name = "otp_ctrl_consistency_test",
    srcs = ["otp_ctrl_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
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
# === END CONSISTENCY TEST: p03_otp_ctrl ===

# ----------------------------------------------------------------------------
# Priority P04 Module: lc_ctrl
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p04_lc_ctrl ===
opentitan_test(
    name = "lc_ctrl_consistency_test",
    srcs = ["lc_ctrl_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
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
# === END CONSISTENCY TEST: p04_lc_ctrl ===

# ----------------------------------------------------------------------------
# Priority P05 Module: otbn
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p05_otbn ===
opentitan_test(
    name = "otbn_consistency_test",
    srcs = ["otbn_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
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
# === END CONSISTENCY TEST: p05_otbn ===

# ----------------------------------------------------------------------------
# Priority P06 Module: kmac
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p06_kmac ===
opentitan_test(
    name = "kmac_consistency_test",
    srcs = ["kmac_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
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
# === END CONSISTENCY TEST: p06_kmac ===

# ----------------------------------------------------------------------------
# Priority P07 Module: csrng
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p07_csrng ===
opentitan_test(
    name = "csrng_consistency_test",
    srcs = ["csrng_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
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
# === END CONSISTENCY TEST: p07_csrng ===

# ----------------------------------------------------------------------------
# Priority P08 Module: edn
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p08_edn ===
opentitan_test(
    name = "edn_consistency_test",
    srcs = ["edn_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
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
# === END CONSISTENCY TEST: p08_edn ===

# ----------------------------------------------------------------------------
# Priority P09 Module: usbdev
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p09_usbdev ===
opentitan_test(
    name = "usbdev_consistency_test",
    srcs = ["usbdev_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top:usbdev_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:pinmux",
        "//sw/device/lib/dif:usbdev",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_alerts",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END CONSISTENCY TEST: p09_usbdev ===

# ----------------------------------------------------------------------------
# Priority P10 Module: uart
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p10_uart ===
opentitan_test(
    name = "uart_consistency_test",
    srcs = ["uart_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    fpga = fpga_params(
        timeout = "short",
    ),
    deps = [
        "//hw/top:uart_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:irq",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END CONSISTENCY TEST: p10_uart ===

# ----------------------------------------------------------------------------
# Priority P11 Module: i2c
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p11_i2c ===
opentitan_test(
    name = "i2c_consistency_test",
    srcs = ["i2c_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top:i2c_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:base",
        "//sw/device/lib/dif:i2c",
        "//sw/device/lib/dif:pinmux",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END CONSISTENCY TEST: p11_i2c ===

# ----------------------------------------------------------------------------
# Priority P12 Module: spi_device
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p12_spi_device ===
opentitan_test(
    name = "spi_device_consistency_test",
    srcs = ["spi_device_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top:spi_device_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:spi_device",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_alerts",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END CONSISTENCY TEST: p12_spi_device ===

# ----------------------------------------------------------------------------
# Priority P13 Module: spi_host
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p13_spi_host ===
opentitan_test(
    name = "spi_host_consistency_test",
    srcs = ["spi_host_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top:spi_host_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:spi_host",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END CONSISTENCY TEST: p13_spi_host ===

# ----------------------------------------------------------------------------
# Priority P14 Module: rv_plic
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p14_rv_plic ===
opentitan_test(
    name = "rv_plic_consistency_test",
    srcs = ["rv_plic_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
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
# === END CONSISTENCY TEST: p14_rv_plic ===

# ----------------------------------------------------------------------------
# Priority P15 Module: alert_handler
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p15_alert_handler ===
opentitan_test(
    name = "alert_handler_consistency_test",
    srcs = ["alert_handler_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
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
# === END CONSISTENCY TEST: p15_alert_handler ===

# ----------------------------------------------------------------------------
# Priority P16 Module: rv_core_ibex
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p16_rv_core_ibex ===
opentitan_test(
    name = "rv_core_ibex_consistency_test",
    srcs = ["rv_core_ibex_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
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
# === END CONSISTENCY TEST: p16_rv_core_ibex ===

# ----------------------------------------------------------------------------
# Priority P17 Module: rv_dm
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p17_rv_dm ===
opentitan_test(
    name = "rv_dm_consistency_test",
    srcs = ["rv_dm_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    fpga = fpga_params(
        timeout = "short",
    ),
    deps = [
        "//hw/top:lc_ctrl_c_regs",
        "//hw/top:otp_ctrl_c_regs",
        "//hw/top:rv_dm_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:multibits",
        "//sw/device/lib/dif:rv_dm",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END CONSISTENCY TEST: p17_rv_dm ===

# ----------------------------------------------------------------------------
# Priority P18 Module: entropy_src
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p18_entropy_src ===
opentitan_test(
    name = "entropy_src_consistency_test",
    srcs = ["entropy_src_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
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
# === END CONSISTENCY TEST: p18_entropy_src ===

# ----------------------------------------------------------------------------
# Priority P19 Module: aes
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p19_aes ===
opentitan_test(
    name = "aes_consistency_test",
    srcs = ["aes_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
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
# === END CONSISTENCY TEST: p19_aes ===

# ----------------------------------------------------------------------------
# Priority P20 Module: hmac
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p20_hmac ===
opentitan_test(
    name = "hmac_consistency_test",
    srcs = ["hmac_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top:hmac_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:hmac",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:irq",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END CONSISTENCY TEST: p20_hmac ===

# ----------------------------------------------------------------------------
# Priority P21 Module: rom_ctrl
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p21_rom_ctrl ===
opentitan_test(
    name = "rom_ctrl_consistency_test",
    srcs = ["rom_ctrl_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
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
# === END CONSISTENCY TEST: p21_rom_ctrl ===

# ----------------------------------------------------------------------------
# Priority P22 Module: sram_ctrl
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p22_sram_ctrl ===
opentitan_test(
    name = "sram_ctrl_consistency_test",
    srcs = ["sram_ctrl_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
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
# === END CONSISTENCY TEST: p22_sram_ctrl ===

# ----------------------------------------------------------------------------
# Priority P23 Module: rv_timer
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p23_rv_timer ===
opentitan_test(
    name = "rv_timer_consistency_test",
    srcs = ["rv_timer_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
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
# === END CONSISTENCY TEST: p23_rv_timer ===

# ----------------------------------------------------------------------------
# Priority P24 Module: aon_timer
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p24_aon_timer ===
opentitan_test(
    name = "aon_timer_consistency_test",
    srcs = ["aon_timer_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top:aon_timer_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:aon_timer",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END CONSISTENCY TEST: p24_aon_timer ===

# ----------------------------------------------------------------------------
# Priority P25 Module: pwrmgr
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p25_pwrmgr ===
opentitan_test(
    name = "pwrmgr_consistency_test",
    srcs = ["pwrmgr_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
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
# === END CONSISTENCY TEST: p25_pwrmgr ===

# ----------------------------------------------------------------------------
# Priority P26 Module: clkmgr
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p26_clkmgr ===
opentitan_test(
    name = "clkmgr_consistency_test",
    srcs = ["clkmgr_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    fpga = fpga_params(
        timeout = "short",
    ),
    deps = [
        "//hw/top:clkmgr_c_regs",
        "//hw/top/dt",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/base:multibits",
        "//sw/device/lib/dif:aon_timer",
        "//sw/device/lib/dif:clkmgr",
        "//sw/device/lib/dif:pwrmgr",
        "//sw/device/lib/dif:rstmgr",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:aon_timer_testutils",
        "//sw/device/lib/testing:pwrmgr_testutils",
        "//sw/device/lib/testing:rstmgr_testutils",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END CONSISTENCY TEST: p26_clkmgr ===

# ----------------------------------------------------------------------------
# Priority P27 Module: rstmgr
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p27_rstmgr ===
opentitan_test(
    name = "rstmgr_consistency_test",
    srcs = ["rstmgr_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
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
# === END CONSISTENCY TEST: p27_rstmgr ===

# ----------------------------------------------------------------------------
# Priority P28 Module: pinmux
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p28_pinmux ===
opentitan_test(
    name = "pinmux_consistency_test",
    srcs = ["pinmux_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    fpga = fpga_params(
        timeout = "short",
    ),
    deps = [
        "//hw/top:pinmux_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:pinmux",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END CONSISTENCY TEST: p28_pinmux ===

# ----------------------------------------------------------------------------
# Priority P29 Module: gpio
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p29_gpio ===
opentitan_test(
    name = "gpio_consistency_test",
    srcs = ["gpio_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top:gpio_c_regs",
        "//hw/top:pinmux_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:memory",
        "//sw/device/lib/dif:gpio",
        "//sw/device/lib/dif:pinmux",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_alerts",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END CONSISTENCY TEST: p29_gpio ===

# ----------------------------------------------------------------------------
# Priority P30 Module: cheriot
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p30_cheriot ===
opentitan_test(
    name = "cheriot_consistency_test",
    srcs = ["cheriot_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top:alert_handler_c_regs",
        "//hw/top:cheriot_c_regs",
        "//hw/top:rv_core_ibex_c_regs",
        "//hw/top:sram_ctrl_c_regs",
        "//hw/top/dt:cheriot",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/base:multibits",
        "//sw/device/lib/dif:alert_handler",
        "//sw/device/lib/dif/autogen:cheriot",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_alerts",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END CONSISTENCY TEST: p30_cheriot ===

# ----------------------------------------------------------------------------
# Priority P31 Module: pattgen
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p31_pattgen ===
opentitan_test(
    name = "pattgen_consistency_test",
    srcs = ["pattgen_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END CONSISTENCY TEST: p31_pattgen ===

# ----------------------------------------------------------------------------
# Priority P32 Module: sysrst_ctrl
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p32_sysrst_ctrl ===
opentitan_test(
    name = "sysrst_ctrl_consistency_test",
    srcs = ["sysrst_ctrl_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top:rv_core_ibex_c_regs",
        "//hw/top:sysrst_ctrl_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:base",
        "//sw/device/lib/dif:sysrst_ctrl",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END CONSISTENCY TEST: p32_sysrst_ctrl ===

# ----------------------------------------------------------------------------
# Priority P33 Module: adc_ctrl
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p33_adc_ctrl ===
opentitan_test(
    name = "adc_ctrl_consistency_test",
    srcs = ["adc_ctrl_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top:adc_ctrl_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:adc_ctrl",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END CONSISTENCY TEST: p33_adc_ctrl ===

# ----------------------------------------------------------------------------
# Priority P34 Module: sensor_ctrl
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p34_sensor_ctrl ===
opentitan_test(
    name = "sensor_ctrl_consistency_test",
    srcs = ["sensor_ctrl_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top:sensor_ctrl_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/base:multibits",
        "//sw/device/lib/dif:sensor_ctrl",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END CONSISTENCY TEST: p34_sensor_ctrl ===

# ----------------------------------------------------------------------------
# Priority P35 Module: ast
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p35_ast ===
opentitan_test(
    name = "ast_consistency_test",
    srcs = ["ast_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top:ast_c_regs",
        "//hw/top:sensor_ctrl_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END CONSISTENCY TEST: p35_ast ===

# ----------------------------------------------------------------------------
# Priority P36 Module: xbar_main_and_xbar_peri
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p36_xbar_main_and_xbar_peri ===
# === BEGIN CONSISTENCY TEST: p35_tlul ===
opentitan_test(
    name = "tlul_and_prim_consistency_test",
    srcs = ["tlul_and_prim_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top:rram_ctrl_c_regs",
        "//hw/top:rv_core_ibex_c_regs",
        "//hw/top:sram_ctrl_c_regs",
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

# === END CONSISTENCY TEST: p35_tlul ===
# === BEGIN CONSISTENCY TEST: p36_prim ===
opentitan_test(
    name = "prim_consistency_test",
    srcs = ["prim_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top:clkmgr_c_regs",
        "//hw/top:csrng_c_regs",
        "//hw/top:sram_ctrl_c_regs",
        "//hw/top:uart_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:multibits",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_alerts",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END CONSISTENCY TEST: p36_prim ===
# === END CONSISTENCY TEST: p36_xbar_main_and_xbar_peri ===

# ----------------------------------------------------------------------------
# Priority P37 Module: top_earlgrey
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p37_top_earlgrey ===
opentitan_test(
    name = "top_earlgrey_consistency_test",
    srcs = ["top_earlgrey_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top:lc_ctrl_c_regs",
        "//hw/top:rstmgr_c_regs",
        "//hw/top:rv_core_ibex_c_regs",
        "//hw/top:sram_ctrl_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:multibits",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
        "//sw/device/silicon_creator/lib/drivers:retention_sram",
    ],
)
# === END CONSISTENCY TEST: p37_top_earlgrey ===

# ----------------------------------------------------------------------------
# Priority P38 Module: padring
# ----------------------------------------------------------------------------
# === BEGIN CONSISTENCY TEST: p38_padring ===
opentitan_test(
    name = "padring_consistency_test",
    srcs = ["padring_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top:gpio_c_regs",
        "//hw/top:pinmux_c_regs",
        "//hw/top:usbdev_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END CONSISTENCY TEST: p38_padring ===
