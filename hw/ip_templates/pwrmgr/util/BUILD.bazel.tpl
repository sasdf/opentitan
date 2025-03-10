# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

package(default_visibility = ["//visibility:public"])

py_library(
    name = "ipconfig",
    srcs = ["ipconfig.py"],
)

py_library(
    name = "dt",
    srcs = ["dt.py"],
    deps = [
        ":ipconfig",
        "//util/dtgen:helper",
        "//util/topgen",
    ],
)
