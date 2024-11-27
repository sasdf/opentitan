# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load("//rules/opentitan:bindgen.bzl", "cc_library_with_bindgen", "ot_rust_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "main_lib",
    deps = [
        ":rust_lib_cc",
    ],
)

ot_rust_library(
    name = "rust_lib",
    hdrs = ["imm_rom_ext.h"],
    srcs = ["imm_rom_ext.rs"],
    deps = [
        "//sw/device/silicon_creator/imm_rom_ext/rust:ot_bindgen",
        "//sw/device/silicon_creator/imm_rom_ext/rust:panic",
        "//sw/device/silicon_creator/imm_rom_ext/rust:dbg_print",
        "//sw/device/silicon_creator/imm_rom_ext/rust/base:hardened",
        "//sw/device/silicon_creator/imm_rom_ext/rust/base:error",
    ],
)

cc_library(
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
