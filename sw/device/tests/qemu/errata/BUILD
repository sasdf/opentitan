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
opentitan_test(
    name = "keymgr_errata_test",
    srcs = ["keymgr_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    fpga = fpga_params(
        timeout = "short",
    ),
    qemu = qemu_params(
        timeout = "short",
    ),
    deps = [
        "//hw/ip/keymgr/data:keymgr_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/dif:flash_ctrl",
        "//sw/device/lib/dif:keymgr",
        "//sw/device/lib/dif:kmac",
        "//sw/device/lib/dif:otp_ctrl",
        "//sw/device/lib/dif:rstmgr",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:entropy_testutils",
        "//sw/device/lib/testing:keymgr_testutils",
        "//sw/device/lib/testing:kmac_testutils",
        "//sw/device/lib/testing:otp_ctrl_testutils",
        "//sw/device/lib/testing:rstmgr_testutils",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_alerts",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
        "//sw/device/silicon_creator/lib/drivers:retention_sram",
    ],
)
# === END ERRATA TEST: p01_keymgr ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P02 Module: hmac
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p02_hmac ===
opentitan_test(
    name = "hmac_errata_test",
    srcs = ["hmac_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/ip/hmac/data:hmac_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/arch:device",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA TEST: p02_hmac ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P03 Module: otbn
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p03_otbn ===
opentitan_test(
    name = "otbn_errata_test",
    srcs = ["otbn_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/ip/otbn/data:otbn_c_regs",
        "//hw/ip/rv_core_ibex/data:rv_core_ibex_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:entropy_testutils",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA TEST: p03_otbn ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P04 Module: pwm
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p04_pwm ===
opentitan_test(
    name = "pwm_errata_test",
    srcs = ["pwm_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/ip/gpio/data:gpio_c_regs",
        "//hw/ip/pwm/data:pwm_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/dif:pinmux",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA TEST: p04_pwm ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P05 Module: adc_ctrl
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p05_adc_ctrl ===
opentitan_test(
    name = "adc_ctrl_errata_test",
    srcs = ["adc_ctrl_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/ip/adc_ctrl/data:adc_ctrl_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/arch:device",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA TEST: p05_adc_ctrl ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P06 Module: kmac
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p06_kmac ===
opentitan_test(
    name = "kmac_errata_test",
    srcs = ["kmac_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    fpga = fpga_params(
        timeout = "short",
    ),
    qemu = qemu_params(
        timeout = "short",
    ),
    deps = [
        "//hw/ip/edn/data:edn_c_regs",
        "//hw/ip/kmac/data:kmac_c_regs",
        "//hw/ip/rv_core_ibex/data:rv_core_ibex_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA TEST: p06_kmac ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P07 Module: spi_device
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p07_spi_device ===
opentitan_test(
    name = "spi_device_errata_test",
    srcs = ["spi_device_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/ip/spi_device/data:spi_device_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA TEST: p07_spi_device ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P08 Module: csrng
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p08_csrng ===
opentitan_test(
    name = "csrng_errata_test",
    srcs = ["csrng_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    fpga = fpga_params(
        timeout = "short",
    ),
    qemu = qemu_params(
        timeout = "short",
    ),
    deps = [
        "//hw/ip/csrng/data:csrng_c_regs",
        "//hw/ip/edn/data:edn_c_regs",
        "//hw/ip/entropy_src/data:entropy_src_c_regs",
        "//hw/ip/rv_core_ibex/data:rv_core_ibex_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/dif:alert_handler",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:irq",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA TEST: p08_csrng ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P09 Module: flash_ctrl
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p09_flash_ctrl ===
opentitan_test(
    name = "flash_ctrl_errata_test",
    srcs = ["flash_ctrl_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    fpga = fpga_params(
        timeout = "short",
    ),
    qemu = qemu_params(
        timeout = "short",
    ),
    deps = [
        "//hw/ip/rv_core_ibex/data:rv_core_ibex_c_regs",
        "//hw/top_earlgrey/ip_autogen/flash_ctrl:flash_ctrl_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:multibits",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_alerts",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
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
opentitan_test(
    name = "aon_timer_errata_test",
    srcs = ["aon_timer_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/ip/aon_timer/data:aon_timer_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA TEST: p11_aon_timer ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P12 Module: pattgen
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p12_pattgen ===
opentitan_test(
    name = "pattgen_errata_test",
    srcs = ["pattgen_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/ip/gpio/data:gpio_c_regs",
        "//hw/ip/pattgen/data:pattgen_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
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
# === END ERRATA TEST: p12_pattgen ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P13 Module: edn
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p13_edn ===
opentitan_test(
    name = "edn_errata_test",
    srcs = ["edn_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/ip/csrng/data:csrng_c_regs",
        "//hw/ip/edn/data:edn_c_regs",
        "//hw/ip/entropy_src/data:entropy_src_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:irq",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA TEST: p13_edn ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P14 Module: rv_plic
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p14_rv_plic ===
opentitan_test(
    name = "rv_plic_errata_test",
    srcs = ["rv_plic_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:rv_plic",
        "//sw/device/lib/dif:uart",
        "//sw/device/lib/runtime:irq",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA TEST: p14_rv_plic ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P15 Module: pinmux
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p15_pinmux ===
opentitan_test(
    name = "pinmux_errata_test",
    srcs = ["pinmux_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top_earlgrey/ip/pinmux/data/autogen:pinmux_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/arch:device",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:alert_handler",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA TEST: p15_pinmux ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P16 Module: rv_timer
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p16_rv_timer ===
opentitan_test(
    name = "rv_timer_errata_test",
    srcs = ["rv_timer_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/ip/rv_timer/data:rv_timer_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA TEST: p16_rv_timer ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P17 Module: usbdev
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p17_usbdev ===
opentitan_test(
    name = "usbdev_errata_test",
    srcs = ["usbdev_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/ip/usbdev/data:usbdev_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:pinmux",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA TEST: p17_usbdev ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P18 Module: entropy_src
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p18_entropy_src ===
opentitan_test(
    name = "entropy_src_errata_test",
    srcs = ["entropy_src_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/ip/entropy_src/data:entropy_src_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:multibits",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:entropy_testutils",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_alerts",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA TEST: p18_entropy_src ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P19 Module: ast
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p19_ast ===
opentitan_test(
    name = "ast_errata_test",
    srcs = ["ast_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA TEST: p19_ast ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P20 Module: aes
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p20_aes ===
opentitan_test(
    name = "aes_errata_test",
    srcs = ["aes_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    fpga = fpga_params(
        timeout = "short",
    ),
    qemu = qemu_params(
        timeout = "short",
    ),
    deps = [
        "//hw/ip/aes/data:aes_c_regs",
        "//hw/ip/csrng/data:csrng_c_regs",
        "//hw/ip/edn/data:edn_c_regs",
        "//hw/ip/entropy_src/data:entropy_src_c_regs",
        "//hw/ip/rv_core_ibex/data:rv_core_ibex_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA TEST: p20_aes ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P21 Module: rv_dm
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p21_rv_dm ===
opentitan_test(
    name = "rv_dm_errata_test",
    srcs = ["rv_dm_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/base:multibits",
        "//sw/device/lib/dif:lc_ctrl",
        "//sw/device/lib/dif:rv_dm",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_alerts",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA TEST: p21_rv_dm ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P22 Module: rstmgr
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p22_rstmgr ===
opentitan_test(
    name = "rstmgr_errata_test",
    srcs = ["rstmgr_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/ip/usbdev/data:usbdev_c_regs",
        "//hw/top_earlgrey/ip_autogen/rstmgr:rstmgr_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/base:multibits",
        "//sw/device/lib/dif:pinmux",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:ottf_main",
        "//sw/device/silicon_creator/lib/drivers:retention_sram",
    ],
)
# === END ERRATA TEST: p22_rstmgr ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P23 Module: lc_ctrl
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p23_lc_ctrl ===
opentitan_test(
    name = "lc_ctrl_errata_test",
    srcs = ["lc_ctrl_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/ip/lc_ctrl/data:lc_ctrl_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:memory",
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
# === END ERRATA TEST: p23_lc_ctrl ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P24 Module: otp_ctrl
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p24_otp_ctrl ===
opentitan_test(
    name = "otp_ctrl_errata_test",
    srcs = ["otp_ctrl_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/dif:lc_ctrl",
        "//sw/device/lib/dif:otp_ctrl",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA TEST: p24_otp_ctrl ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P25 Module: sram_ctrl
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p25_sram_ctrl ===
opentitan_test(
    name = "sram_ctrl_errata_test",
    srcs = ["sram_ctrl_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/ip/sram_ctrl/data:sram_ctrl_c_regs",
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
        "//sw/device/silicon_creator/lib/drivers:retention_sram",
    ],
)
# === END ERRATA TEST: p25_sram_ctrl ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P26 Module: clkmgr
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p26_clkmgr ===
opentitan_test(
    name = "clkmgr_errata_test",
    srcs = ["clkmgr_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    fpga = fpga_params(
        timeout = "short",
    ),
    qemu = qemu_params(
        timeout = "short",
    ),
    deps = [
        "//hw/ip/aon_timer/data:aon_timer_c_regs",
        "//hw/ip/rv_core_ibex/data:rv_core_ibex_c_regs",
        "//hw/top_earlgrey/ip_autogen/clkmgr:clkmgr_c_regs",
        "//hw/top_earlgrey/ip_autogen/pwrmgr:pwrmgr_c_regs",
        "//hw/top_earlgrey/ip_autogen/rstmgr:rstmgr_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/arch:device",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:math",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/base:multibits",
        "//sw/device/lib/dif:aon_timer",
        "//sw/device/lib/dif:clkmgr",
        "//sw/device/lib/dif:pwrmgr",
        "//sw/device/lib/dif:rstmgr",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:aon_timer_testutils",
        "//sw/device/lib/testing:pwrmgr_testutils",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA TEST: p26_clkmgr ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P27 Module: alert_handler
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p27_alert_handler ===
opentitan_test(
    name = "alert_handler_errata_test",
    srcs = ["alert_handler_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/ip/uart/data:uart_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/dif:alert_handler",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA TEST: p27_alert_handler ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P28 Module: sensor_ctrl
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p28_sensor_ctrl ===
opentitan_test(
    name = "sensor_ctrl_errata_test",
    srcs = ["sensor_ctrl_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
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
# === END ERRATA TEST: p28_sensor_ctrl ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P29 Module: pwrmgr
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p29_pwrmgr ===
opentitan_test(
    name = "pwrmgr_errata_test",
    srcs = ["pwrmgr_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/ip/aon_timer/data:aon_timer_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:memory",
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
# === END ERRATA TEST: p29_pwrmgr ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P30 Module: spi_host
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p30_spi_host ===
opentitan_test(
    name = "spi_host_errata_test",
    srcs = ["spi_host_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/ip/spi_host/data:spi_host_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_alerts",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA TEST: p30_spi_host ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P31 Module: i2c
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p31_i2c ===
opentitan_test(
    name = "i2c_errata_test",
    srcs = ["i2c_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/ip/i2c/data:i2c_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:pinmux",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA TEST: p31_i2c ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P32 Module: sysrst_ctrl
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p32_sysrst_ctrl ===
opentitan_test(
    name = "sysrst_ctrl_errata_test",
    srcs = ["sysrst_ctrl_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    fpga = fpga_params(
        timeout = "short",
    ),
    qemu = qemu_params(
        timeout = "short",
    ),
    deps = [
        "//hw/ip/rv_core_ibex/data:rv_core_ibex_c_regs",
        "//hw/ip/sysrst_ctrl/data:sysrst_ctrl_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA TEST: p32_sysrst_ctrl ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P33 Module: rom_ctrl
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p33_rom_ctrl ===
opentitan_test(
    name = "rom_ctrl_errata_test",
    srcs = ["rom_ctrl_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:alert_handler",
        "//sw/device/lib/dif:rom_ctrl",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA TEST: p33_rom_ctrl ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P34 Module: rv_core_ibex
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p34_rv_core_ibex ===
opentitan_test(
    name = "rv_core_ibex_errata_test",
    srcs = ["rv_core_ibex_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/ip/edn/data:edn_c_regs",
        "//hw/ip/rv_core_ibex/data:rv_core_ibex_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:multibits",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:entropy_testutils",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_alerts",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
        "//sw/device/silicon_creator/lib/drivers:retention_sram",
    ],
)
# === END ERRATA TEST: p34_rv_core_ibex ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P35 Module: gpio
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p35_gpio ===
opentitan_test(
    name = "gpio_errata_test",
    srcs = ["gpio_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/ip/gpio/data:gpio_c_regs",
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
# === END ERRATA TEST: p35_gpio ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P36 Module: padring
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p36_padring ===
opentitan_test(
    name = "padring_errata_test",
    srcs = ["padring_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/ip/gpio/data:gpio_c_regs",
        "//hw/ip/pinmux/data:pinmux_c_regs",
        "//hw/ip/sysrst_ctrl/data:sysrst_ctrl_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA TEST: p36_padring ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P37 Module: top_earlgrey
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p37_top_earlgrey ===
opentitan_test(
    name = "top_earlgrey_errata_test",
    srcs = ["top_earlgrey_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/ip/lc_ctrl/data:lc_ctrl_c_regs",
        "//hw/ip/sram_ctrl/data:sram_ctrl_c_regs",
        "//hw/top_earlgrey/ip_autogen/rstmgr:rstmgr_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:multibits",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:ottf_main",
        "//sw/device/silicon_creator/lib/drivers:retention_sram",
    ],
)
# === END ERRATA TEST: p37_top_earlgrey ===
#
#
#

# ----------------------------------------------------------------------------
# Priority P38 Module: xbar_main_and_xbar_peri
# ----------------------------------------------------------------------------
# === BEGIN ERRATA TEST: p38_xbar_main_and_xbar_peri ===
opentitan_test(
    name = "xbar_main_and_xbar_peri_errata_test",
    srcs = ["xbar_main_and_xbar_peri_errata_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/ip/pattgen/data:pattgen_c_regs",
        "//hw/ip/spi_host/data:spi_host_c_regs",
        "//hw/ip/sysrst_ctrl/data:sysrst_ctrl_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
# === END ERRATA TEST: p38_xbar_main_and_xbar_peri ===
#
#
#
