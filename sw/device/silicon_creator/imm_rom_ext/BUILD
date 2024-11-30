# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load("//rules/opentitan:rust.bzl", "cc_library_with_bindgen", "ot_rust_library")

package(default_visibility = ["//visibility:public"])

ot_rust_library(
    name = "main_lib",
    srcs = ["imm_rom_ext.rs"],
    hdrs = ["imm_rom_ext.h"],
    deps = [
        "//sw/device/lib/arch:device",
        "//sw/device/lib/base:hardened",
        "//sw/device/silicon_creator/imm_rom_ext:imm_rom_ext_epmp",
        "//sw/device/silicon_creator/lib:dbg_print",
        "//sw/device/silicon_creator/lib:epmp_state",
        "//sw/device/silicon_creator/lib:error",
        "//sw/device/silicon_creator/lib:manifest",
        "//sw/device/silicon_creator/lib:shutdown",
        "//sw/device/silicon_creator/lib/base:boot_measurements",
        "//sw/device/silicon_creator/lib/base:sec_mmio",
        "//sw/device/silicon_creator/lib/cert:dice_chain",
        "//sw/device/silicon_creator/lib/drivers:pinmux",
        "//sw/device/silicon_creator/lib/drivers:rnd",
        "//sw/device/silicon_creator/lib/drivers:uart",
        "//sw/device/silicon_creator/rom_ext:rom_ext_manifest",
    ],
)

cc_library_with_bindgen(
    name = "imm_rom_ext_epmp",
    srcs = ["imm_rom_ext_epmp.c"],
    hdrs = ["imm_rom_ext_epmp.h"],
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:macros",
        "//sw/device/silicon_creator/lib:epmp_state",
        "//sw/device/silicon_creator/lib:error",
        "//sw/device/silicon_creator/lib:manifest",
        "//sw/device/silicon_creator/lib/drivers:epmp",
        "//sw/device/silicon_creator/lib/drivers:lifecycle",
    ],
)
