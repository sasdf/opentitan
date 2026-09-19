# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load(
    "//rules/opentitan:defs.bzl",
    "opentitan_test",
)

package(default_visibility = ["//visibility:public"])

opentitan_test(
    name = "otp_ctrl_consistency_test",
    srcs = ["otp_ctrl_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:fpga_cw340_sival_rom_ext": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_sival_rom_ext": None,
        "//hw/top_earlgrey:sim_verilator": None,
    },
    deps = [
        "//hw/ip/lc_ctrl/data:lc_ctrl_c_regs",
        "//hw/ip/otp_ctrl/data:otp_ctrl_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
