# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load("@bazel_skylib//lib:dicts.bzl", "dicts")
load(
    "//rules/opentitan:defs.bzl",
    "EARLGREY_CW340_TEST_ENVS",
    "EARLGREY_SILICON_OWNER_ROM_EXT_ENVS",
    "EARLGREY_TEST_ENVS",
    "opentitan_test",
    "qemu_params",
)

package(default_visibility = ["//visibility:public"])

opentitan_test(
    name = "usbdev_rtl_consistency_test",
    srcs = ["usbdev_rtl_consistency_test.c"],
    exec_env = dicts.add(
        EARLGREY_TEST_ENVS,
        EARLGREY_CW340_TEST_ENVS,
        EARLGREY_SILICON_OWNER_ROM_EXT_ENVS,
    ),
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/arch:device",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/dif:rstmgr",
        "//sw/device/lib/dif:usbdev",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "sram_ctrl_consistency_test",
    srcs = ["sram_ctrl_consistency_test.c"],
    exec_env = dicts.add(
        EARLGREY_TEST_ENVS,
        EARLGREY_CW340_TEST_ENVS,
        EARLGREY_SILICON_OWNER_ROM_EXT_ENVS,
    ),
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/arch:device",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:multibits",
        "//sw/device/lib/dif:sram_ctrl",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
        "//sw/device/silicon_creator/lib/drivers:retention_sram",
    ],
)

opentitan_test(
    name = "flash_ctrl_consistency_test",
    srcs = ["flash_ctrl_consistency_test.c"],
    exec_env = dicts.add(
        EARLGREY_TEST_ENVS,
        EARLGREY_CW340_TEST_ENVS,
        EARLGREY_SILICON_OWNER_ROM_EXT_ENVS,
    ),
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/arch:device",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:multibits",
        "//sw/device/lib/dif:flash_ctrl",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_alerts",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)


opentitan_test(
    name = "csrng_consistency_test",
    srcs = ["csrng_consistency_test.c"],
    exec_env = dicts.add(
        EARLGREY_TEST_ENVS,
        EARLGREY_CW340_TEST_ENVS,
        EARLGREY_SILICON_OWNER_ROM_EXT_ENVS,
    ),
    deps = [
        "//hw/ip/csrng/data:csrng_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:multibits",
        "//sw/device/lib/dif:csrng",
        "//sw/device/lib/dif:edn",
        "//sw/device/lib/dif:entropy_src",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:csrng_testutils",
        "//sw/device/lib/testing:entropy_testutils",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_alerts",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "edn_consistency_test",
    srcs = ["edn_consistency_test.c"],
    exec_env = dicts.add(
        EARLGREY_TEST_ENVS,
        EARLGREY_CW340_TEST_ENVS,
        EARLGREY_SILICON_OWNER_ROM_EXT_ENVS,
    ),
    deps = [
        "//hw/ip/csrng/data:csrng_c_regs",
        "//hw/ip/edn/data:edn_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:multibits",
        "//sw/device/lib/dif:alert_handler",
        "//sw/device/lib/dif:csrng",
        "//sw/device/lib/dif:edn",
        "//sw/device/lib/dif:entropy_src",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:irq",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:entropy_testutils",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_alerts",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "hmac_rtl_consistency_test",
    srcs = ["hmac_rtl_consistency_test.c"],
    exec_env = dicts.add(
        EARLGREY_TEST_ENVS,
        EARLGREY_CW340_TEST_ENVS,
        EARLGREY_SILICON_OWNER_ROM_EXT_ENVS,
    ),
    deps = [
        "//hw/ip/hmac/data:hmac_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/arch:device",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/dif:hmac",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "kmac_rtl_consistency_test",
    srcs = ["kmac_rtl_consistency_test.c"],
    exec_env = dicts.add(
        EARLGREY_TEST_ENVS,
        EARLGREY_CW340_TEST_ENVS,
        EARLGREY_SILICON_OWNER_ROM_EXT_ENVS,
    ),
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/arch:device",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/dif:kmac",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "otbn_rtl_consistency_test",
    srcs = ["otbn_rtl_consistency_test.c"],
    exec_env = dicts.add(
        EARLGREY_TEST_ENVS,
        EARLGREY_CW340_TEST_ENVS,
        EARLGREY_SILICON_OWNER_ROM_EXT_ENVS,
    ),
    deps = [
        "//hw/ip/otbn/data:otbn_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/arch:device",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "aon_timer_rtl_consistency_test",
    srcs = ["aon_timer_rtl_consistency_test.c"],
    exec_env = dicts.add(
        EARLGREY_TEST_ENVS,
        EARLGREY_CW340_TEST_ENVS,
        EARLGREY_SILICON_OWNER_ROM_EXT_ENVS,
    ),
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/arch:device",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/dif:alert_handler",
        "//sw/device/lib/dif:aon_timer",
        "//sw/device/lib/dif:rstmgr",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
opentitan_test(
    name = "sysrst_ctrl_rtl_consistency_test",
    srcs = ["sysrst_ctrl_rtl_consistency_test.c"],
    exec_env = dicts.add(
        EARLGREY_TEST_ENVS,
        EARLGREY_CW340_TEST_ENVS,
        EARLGREY_SILICON_OWNER_ROM_EXT_ENVS,
    ),
    deps = [
        "//hw/ip/sysrst_ctrl/data:sysrst_ctrl_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/arch:device",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "pwm_rtl_consistency_test",
    srcs = ["pwm_rtl_consistency_test.c"],
    exec_env = dicts.add(
        EARLGREY_TEST_ENVS,
        EARLGREY_CW340_TEST_ENVS,
        EARLGREY_SILICON_OWNER_ROM_EXT_ENVS,
    ),
    deps = [
        "//hw/ip/gpio/data:gpio_c_regs",
        "//hw/ip/pwm/data:pwm_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/arch:device",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/dif:alert_handler",
        "//sw/device/lib/dif:pinmux",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
opentitan_test(
    name = "keymgr_rtl_consistency_test",
    srcs = ["keymgr_rtl_consistency_test.c"],
    exec_env = dicts.add(
        EARLGREY_TEST_ENVS,
        EARLGREY_CW340_TEST_ENVS,
        EARLGREY_SILICON_OWNER_ROM_EXT_ENVS,
    ),
    deps = [
        "//hw/ip/keymgr/data:keymgr_c_regs",
        "//hw/ip/kmac/data:kmac_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/arch:device",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/dif:flash_ctrl",
        "//sw/device/lib/dif:keymgr",
        "//sw/device/lib/dif:kmac",
        "//sw/device/lib/dif:otp_ctrl",
        "//sw/device/lib/dif:rstmgr",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:entropy_testutils",
        "//sw/device/lib/testing:flash_ctrl_testutils",
        "//sw/device/lib/testing:keymgr_testutils",
        "//sw/device/lib/testing:kmac_testutils",
        "//sw/device/lib/testing:otp_ctrl_testutils",
        "//sw/device/lib/testing:rstmgr_testutils",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_alerts",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "lc_ctrl_rtl_consistency_test",
    srcs = ["lc_ctrl_rtl_consistency_test.c"],
    exec_env = dicts.add(
        EARLGREY_TEST_ENVS,
        EARLGREY_CW340_TEST_ENVS,
        EARLGREY_SILICON_OWNER_ROM_EXT_ENVS,
    ),
    deps = [
        "//hw/ip/lc_ctrl/data:lc_ctrl_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/dif:alert_handler",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:multibits",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_alerts",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "alert_handler_consistency_test",
    srcs = ["alert_handler_consistency_test.c"],
    exec_env = dicts.add(
        EARLGREY_TEST_ENVS,
        EARLGREY_CW340_TEST_ENVS,
        EARLGREY_SILICON_OWNER_ROM_EXT_ENVS,
    ),
    deps = [
        "//hw/ip/uart/data:uart_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/dif:alert_handler",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "pattgen_rtl_consistency_test",
    srcs = ["pattgen_rtl_consistency_test.c"],
    exec_env = dicts.add(
        EARLGREY_TEST_ENVS,
        EARLGREY_CW340_TEST_ENVS,
        EARLGREY_SILICON_OWNER_ROM_EXT_ENVS,
    ),
    deps = [
        "//hw/ip/gpio/data:gpio_c_regs",
        "//hw/ip/pattgen/data:pattgen_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/arch:device",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/dif:alert_handler",
        "//sw/device/lib/dif:pinmux",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "adc_ctrl_rtl_consistency_test",
    srcs = ["adc_ctrl_rtl_consistency_test.c"],
    exec_env = dicts.add(
        EARLGREY_TEST_ENVS,
        EARLGREY_CW340_TEST_ENVS,
        EARLGREY_SILICON_OWNER_ROM_EXT_ENVS,
    ),
    deps = [
        "//hw/ip/adc_ctrl/data:adc_ctrl_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_alerts",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
opentitan_test(
    name = "gpio_rtl_consistency_test",
    srcs = ["gpio_rtl_consistency_test.c"],
    exec_env = dicts.add(
        EARLGREY_TEST_ENVS,
        EARLGREY_CW340_TEST_ENVS,
        EARLGREY_SILICON_OWNER_ROM_EXT_ENVS,
    ),
    deps = [
        "//hw/ip/gpio/data:gpio_c_regs",
        "//hw/ip/pinmux/data:pinmux_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/arch:device",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/dif:alert_handler",
        "//sw/device/lib/dif:pinmux",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "ast_consistency_test",
    srcs = ["ast_consistency_test.c"],
    exec_env = dicts.add(
        EARLGREY_TEST_ENVS,
        EARLGREY_CW340_TEST_ENVS,
        EARLGREY_SILICON_OWNER_ROM_EXT_ENVS,
    ),
    deps = [
        "//hw/top_earlgrey/ip/ast/data:ast_c_regs",
        "//hw/top_earlgrey/ip/sensor_ctrl/data:sensor_ctrl_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "clkmgr_rtl_consistency_test",
    srcs = ["clkmgr_rtl_consistency_test.c"],
    exec_env = dicts.add(
        EARLGREY_CW340_TEST_ENVS,
        EARLGREY_SILICON_OWNER_ROM_EXT_ENVS,
    ),
    deps = [
        "//hw/top_earlgrey/ip_autogen/clkmgr:clkmgr_c_regs",
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

opentitan_test(
    name = "rom_ctrl_consistency_test",
    srcs = ["rom_ctrl_consistency_test.c"],
    exec_env = dicts.add(
        EARLGREY_TEST_ENVS,
        EARLGREY_CW340_TEST_ENVS,
        EARLGREY_SILICON_OWNER_ROM_EXT_ENVS,
    ),
    deps = [
        "//hw/ip/rom_ctrl/data:rom_ctrl_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:alert_handler",
        "//sw/device/lib/dif:rom_ctrl",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_isrs",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

