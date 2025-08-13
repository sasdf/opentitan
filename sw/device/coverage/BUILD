# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load("//rules:linker.bzl", "ld_library")
load("@bazel_skylib//rules:common_settings.bzl", "bool_flag")
load("//rules/opentitan:defs.bzl", "OPENTITAN_CPU", "opentitan_binary")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "asm_runtime",
    srcs = ["asm_runtime.c"],
    deps = [
        "//sw/device/lib/base:macros",
    ],
)

cc_library(
    name = "printer",
    hdrs = ["printer.h"],
    srcs = ["printer.c"],
    deps = [
        "//sw/device/lib/base:crc32",
        "//sw/device/lib/base:macros",
    ],
    features = ["-coverage"],
    alwayslink = True,  # replacing weak symbol
)

cc_library(
    name = "runtime_api",
    hdrs = ["runtime.h"],
    deps = [
        ":empty_runtime",
        "//sw/device/lib/base:macros",
    ],
)

cc_library(
    name = "runtime_api_asm",
    hdrs = ["runtime_asm.h"],
    deps = [
        ":asm_runtime",
        ":runtime_api",
    ],
)

cc_library(
    name = "empty_runtime_real",
    srcs = ["empty_runtime.c"],
    deps = [
        "//sw/device/lib/base:macros",
    ],
    features = ["-coverage"],
)

alias(
    name = "empty_runtime",
    actual = select({
        "//rules/coverage:enabled": ":empty_runtime_real",
        "//conditions:default": "//sw/device:nothing",
    }),
)

cc_library(
    name = "uart_runtime_real",
    srcs = ["uart_runtime.c"],
    target_compatible_with = [OPENTITAN_CPU],
    deps = [
        ":printer",
        "//sw/device/lib/arch:device",
        "//sw/device/silicon_creator/lib/drivers:pinmux",
        "//sw/device/silicon_creator/lib/drivers:uart",
    ],
    features = ["-coverage"],
    alwayslink = True,  # replacing weak symbol
)

cc_library(
    name = "uart_runtime_skip",
    srcs = ["uart_runtime_skip.c"],
    deps = [
        "//sw/device/lib/arch:device",
        "//sw/device/silicon_creator/lib/drivers:pinmux",
        "//sw/device/silicon_creator/lib/drivers:uart",
    ],
    features = ["-coverage"],
    alwayslink = True,  # replacing weak symbol
)

alias(
    name = "uart_runtime",
    actual = select({
        "//rules/coverage:instrumented": ":uart_runtime_real",
        "//rules/coverage:enabled": ":uart_runtime_skip",
        "//conditions:default": "//sw/device:nothing",
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
    ],
    features = ["-coverage"],
    alwayslink = True,  # replacing weak symbol
)

cc_library(
    name = "ottf_runtime_skip",
    srcs = ["ottf_runtime_skip.c"],
    deps = [
        "//sw/device/lib/runtime:print",
    ],
    features = ["-coverage"],
    alwayslink = True,  # replacing weak symbol
)

alias(
    name = "ottf_runtime",
    actual = select({
        "//rules/coverage:instrumented": ":ottf_runtime_real",
        "//rules/coverage:enabled": ":ottf_runtime_skip",
        "//conditions:default": "//sw/device:nothing",
    }),
)

ld_library(
    name = "llvm_prf_cnts",
    includes = [
        "llvm_prf_cnts.ld",
        "rodata.ld",
    ],
)
