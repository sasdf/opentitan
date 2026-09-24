# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load("@rules_rust//rust:defs.bzl", "rust_doc", "rust_library", "rust_test")

package(default_visibility = ["//visibility:public"])

rust_library(
    name = "qemu",
    srcs = [
        "src/gpio.rs",
        "src/i2c.rs",
        "src/jtag.rs",
        "src/lib.rs",
        "src/monitor.rs",
        "src/reset.rs",
        "src/spi.rs",
        "src/uart.rs",
        "src/usbdev.rs",
    ],
    compile_data = glob(["config/*.json"]),
    crate_name = "ot_transport_qemu",
    deps = [
        "//sw/host/opentitanlib:opentitanlib_base",
        "@crate_index//:anyhow",
        "@crate_index//:clap",
        "@crate_index//:erased-serde",
        "@crate_index//:humantime",
        "@crate_index//:log",
        "@crate_index//:regex",
        "@crate_index//:rusb",
        "@crate_index//:serde",
        "@crate_index//:serde_json",
        "@crate_index//:serialport",
        "@crate_index//:thiserror",
        "@crate_index//:tokio",
        "@crate_index//:zerocopy",
    ],
)

rust_doc(
    name = "qemu_doc",
    crate = ":qemu",
)

rust_test(
    name = "qemu_test",
    crate = ":qemu",
)
