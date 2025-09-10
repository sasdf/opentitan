# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load("@rules_python//python:defs.bzl", "py_binary", "py_library")

package(default_visibility = ["//visibility:public"])

py_library(
    name = "asm_helper",
    srcs = ["asm_helper.py"],
)

py_binary(
    name = "add_asm_prf_data",
    srcs = ["add_asm_prf_data.py"],
    deps = [
        ":asm_helper",
    ],
)
