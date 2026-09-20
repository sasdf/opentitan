# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load(
    "//rules/opentitan:defs.bzl",
    "opentitan_test",
)

package(default_visibility = ["//visibility:public"])

opentitan_test(
    name = "rv_plic_consistency_test",
    srcs = ["rv_plic_consistency_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    deps = [
        "//hw/top_earlgrey:alert_handler_c_regs",
        "//hw/top_earlgrey:rv_plic_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:alert_handler",
        "//sw/device/lib/dif:rv_plic",
        "//sw/device/lib/dif:uart",
        "//sw/device/lib/runtime:irq",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
