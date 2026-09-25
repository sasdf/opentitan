# Copyright lowRISC contributors.
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load(
    "//rules/opentitan:defs.bzl",
    "opentitan_test",
)

opentitan_test(
    name = "ot_flash_op_invalid_test",
    srcs = ["ot_flash_op_invalid_test.c"],
    exec_env = {
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:flash_ctrl",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
