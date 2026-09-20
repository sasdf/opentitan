# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load("@rules_rust//rust:defs.bzl", "rust_binary")
load("//rules:lc.bzl", "lc_raw_unlock_token")

package(default_visibility = ["//visibility:public"])

lc_raw_unlock_token(
    name = "lc_raw_unlock_token",
)

rust_binary(
    name = "test_status",
    srcs = [
        "src/test_status.rs",
    ],
    edition = "2024",
    deps = [
        "//sw/host/opentitanlib",
        "@crate_index//:anyhow",
        "@crate_index//:clap",
    ],
)

rust_binary(
    name = "i2c_target",
    srcs = [
        "src/i2c_target.rs",
    ],
    edition = "2024",
    deps = [
        "//sw/host/opentitanlib",
        "@crate_index//:anyhow",
        "@crate_index//:clap",
        "@crate_index//:humantime",
    ],
)

rust_binary(
    name = "rom_ctrl_integrity_check",
    srcs = [
        "src/rom_ctrl_integrity_check.rs",
    ],
    edition = "2024",
    deps = [
        "//sw/host/opentitanlib",
        "@crate_index//:anyhow",
        "@crate_index//:clap",
        "@crate_index//:humantime",
        "@crate_index//:log",
    ],
)

rust_binary(
    name = "lc_ctrl_transition",
    srcs = [
        "src/lc_ctrl_transition.rs",
    ],
    edition = "2024",
    deps = [
        "//sw/host/opentitanlib",
        "@crate_index//:anyhow",
        "@crate_index//:clap",
        "@crate_index//:humantime",
        "@crate_index//:log",
    ],
)

rust_binary(
    name = "lc_walkthrough",
    srcs = [
        "src/lc_walkthrough.rs",
        ":lc_raw_unlock_token",
    ],
    edition = "2024",
    deps = [
        "//sw/host/opentitanlib",
        "@crate_index//:anyhow",
        "@crate_index//:clap",
        "@crate_index//:humantime",
        "@crate_index//:log",
    ],
)
