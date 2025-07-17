# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load("@bazel_skylib//rules:common_settings.bzl", "bool_flag")

package(default_visibility = ["//visibility:public"])

config_setting(
    name = "enabled",
    define_values = {
        "ot_coverage_enabled": "true",
    },
)

bool_flag(
    name = "enabled_flag",
    build_setting_default = False,
)
