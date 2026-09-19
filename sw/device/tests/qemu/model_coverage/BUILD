# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load(
    "//rules/opentitan:defs.bzl",
    "EARLGREY_TEST_ENVS",
    "fpga_params",
    "opentitan_test",
    "qemu_params",
)

package(default_visibility = ["//visibility:public"])

opentitan_test(
    name = "ot_pwm_model_coverage_test",
    srcs = ["ot_pwm_model_coverage_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:fpga_cw340_sival_rom_ext": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_sival_rom_ext": None,
    },
    fpga = fpga_params(
        timeout = "short",
    ),
    qemu = qemu_params(
        timeout = "short",
    ),
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/dif:pinmux",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:ottf_main",
        "//sw/device/lib/testing/test_framework:check",
        "//hw/ip/gpio/data:gpio_c_regs",
        "//hw/ip/pwm/data:pwm_c_regs",
    ],
)
