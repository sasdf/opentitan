# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load("//rules:linker.bzl", "ld_library")
load("@bazel_skylib//rules:common_settings.bzl", "bool_flag")
load("//rules/opentitan:defs.bzl", "OPENTITAN_CPU", "opentitan_binary")

package(default_visibility = ["//visibility:public"])

config_setting(
    name = "is_coverage_build",
    values = {
        "features": "coverage",
    },
)

cc_library(
    name = "runtime_api_asm",
    hdrs = ["runtime_asm.h"],
)

cc_library(
    name = "printer_api",
    hdrs = ["printer.h"],
    features = ["-coverage"],
)

cc_library(
    name = "asm_counters",
    srcs = [
        "asm_counters.c",
        "asm_runtime.c",
    ],
    deps = [
        "//sw/device/lib/base:macros",
    ],
    alwayslink = True,
)

cc_library(
    name = "common_printer",
    srcs = ["printer.c"],
    features = ["-coverage"],
    deps = [
        ":compress",
        ":printer_api",
        "//sw/device/lib/base:crc32",
    ],
    alwayslink = True,
)

cc_library(
    name = "compress",
    srcs = ["compress.c"],
    hdrs = ["compress.h"],
    features = ["-coverage"],
    deps = [
        ":printer_api",
    ],
)

cc_library(
    name = "llvm_printer",
    srcs = ["llvm_printer.c"],
    features = ["-coverage"],
    deps = [
        ":common_printer",
        ":compress",
        ":printer_api",
        "//sw/device/lib/base:macros",
        "//third_party/llvm_compiler_rt",
    ],
)

cc_library(
    name = "compressed_printer",
    srcs = ["compressed_printer.c"],
    features = ["-coverage"],
    deps = [
        ":common_printer",
        ":compress",
        ":printer_api",
        "//sw/device/silicon_creator/lib/drivers:uart",
    ],
)

alias(
    name = "printer",
    actual = "compressed_printer",
)

cc_library(
    name = "runtime_api",
    hdrs = ["runtime.h"],
    deps = [
        ":empty_runtime",
        ":asm_counters",
        "//sw/device/lib/base:macros",
    ],
)

cc_library(
    name = "empty_runtime",
    srcs = ["empty_runtime.c"],
    features = ["-coverage"],
    deps = [
        "//sw/device/lib/base:macros",
    ],
)

cc_library(
    name = "uart_runtime_real",
    srcs = ["uart_runtime.c"],
    target_compatible_with = [OPENTITAN_CPU],
    deps = [
        ":printer",
        "//sw/device/silicon_creator/lib/drivers:uart",
        "//sw/device/lib/base:macros",
    ],
    alwayslink = True,
)

alias(
    name = "uart_runtime",
    actual = select({
        "//sw/device:measure_coverage_on_target": ":uart_runtime_real",
        "//conditions:default": ":empty_runtime",
    }),
)

cc_library(
    name = "ottf_runtime_real",
    srcs = ["ottf_runtime.c"],
    target_compatible_with = [OPENTITAN_CPU],
    deps = [
        ":printer",
        "//sw/device/lib/base:memory",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/runtime:print",
        "//sw/device/lib/base:macros",
    ],
    alwayslink = True,
)

alias(
    name = "ottf_runtime",
    actual = select({
        "//sw/device:measure_coverage_on_target": ":ottf_runtime_real",
        "//conditions:default": ":empty_runtime",
    }),
)

ld_library(
    name = "llvm_prf_cnts",
    includes = [
        "llvm_prf_cnts.ld",
        "rodata.ld",
    ],
)

filegroup(
    name = "collect_cc_coverage",
    srcs = ["collect_cc_coverage.sh"],
)
