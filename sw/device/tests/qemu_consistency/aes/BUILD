# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load(
    "//rules/opentitan:defs.bzl",
    "EARLGREY_TEST_ENVS",
    "opentitan_test",
)

package(default_visibility = ["//visibility:public"])

opentitan_test(
    name = "aes_consistency_test",
    srcs = ["aes_consistency_test.c"],
    exec_env = EARLGREY_TEST_ENVS | {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/ip/aes/data:aes_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_alerts",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
