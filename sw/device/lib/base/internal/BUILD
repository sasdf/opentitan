# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load("//rules/opentitan:rust.bzl", "cc_library_with_bindgen")

package(default_visibility = ["//visibility:public"])

cc_library_with_bindgen(
    name = "absl_status",
    hdrs = ["absl_status.h"],
)

cc_library_with_bindgen(
    name = "status",
    hdrs = ["status.h"],
    bindgen = {
        "exports": [
            ":absl_status",
        ],
    },
    deps = [
        ":absl_status",
        "//sw/device/lib/base:bitfield",
    ],
)
