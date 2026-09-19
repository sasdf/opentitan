# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load(
    "//rules/opentitan:defs.bzl",
    "EARLGREY_TEST_ENVS",
    "opentitan_test",
    "qemu_params",
)

package(default_visibility = ["//visibility:public"])

opentitan_test(
    name = "aes_consistency_test",
    srcs = ["aes_consistency_test.c"],
    exec_env = EARLGREY_TEST_ENVS,
    qemu = qemu_params(
        # Marked broken until fix_rtl_discrepancy_aes_* tasks in ~/tasks/pending/ are resolved.
        tags = ["broken"],
    ),
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
