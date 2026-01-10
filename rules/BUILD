# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load("//rules:opentitan.bzl", "OPENTITAN_PLATFORM")
load("//rules:stamp.bzl", "stamp_flag")
load("@bazel_skylib//rules:common_settings.bzl", "string_flag")

package(default_visibility = ["//visibility:public"])

config_setting(
    name = "opentitan_platform",
    values = {"platforms": OPENTITAN_PLATFORM},
)

# See stamp.bzl for explanation.
stamp_flag(name = "stamp_flag")

string_flag(
    name = "fpga",
    build_setting_default = "hyper340",
    values = [
        "",
        "hyper340",
    ],
)

config_setting(
    name = "use_cw340",
    flag_values = {":fpga": "hyper340"},
)
