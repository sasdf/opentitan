# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load("@rules_rust//rust:defs.bzl", "rust_library")

package(default_visibility = ["//visibility:public"])

rust_library(
    name = "perso_tlv_lib",
    srcs = ["src/lib.rs"],
    deps = [
        "//sw/device/silicon_creator/manuf/base:perso_tlv_data",
        "@crate_index//:anyhow",
    ],
)
