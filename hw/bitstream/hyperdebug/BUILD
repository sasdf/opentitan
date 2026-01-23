# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load("//rules:otp.bzl", "get_otp_images")
load("//rules:const.bzl", "KEY_AUTHENTICITY")

package(default_visibility = ["//visibility:public"])

filegroup(
    name = "_hyper310_bitstream",
    testonly = True,
    srcs = select({
        "//hw/bitstream:bitstream_skip": ["//hw/bitstream/universal:none"],
        "//hw/bitstream:bitstream_vivado": ["//hw/bitstream/vivado:fpga_cw310_test_rom_hyp"],
        "//hw/bitstream:bitstream_gcp_splice": ["@bitstreams//:chip_earlgrey_cw310_hyperdebug_bitstream"],
        "//conditions:default": ["@bitstreams//:chip_earlgrey_cw310_hyperdebug_bitstream"],
    }),
    tags = ["manual"],
)

alias(
    name = "bitstream",
    actual = select({
        "//rules:use_cw340": "//hw/bitstream/cw340:bitstream",
        "//conditions:default": ":_hyper310_bitstream",
    }),
)

filegroup(
    name = "_hyper310_mmi",
    testonly = True,
    srcs = select({
        "//hw/bitstream:bitstream_skip": ["//hw/bitstream/universal:none"],
        "//hw/bitstream:bitstream_vivado": ["//hw/bitstream/vivado:cw310_hyperdebug_mmi"],
        "//hw/bitstream:bitstream_gcp_splice": ["@bitstreams//:chip_earlgrey_cw310_hyperdebug_mmi"],
        "//conditions:default": ["@bitstreams//:chip_earlgrey_cw310_hyperdebug_mmi"],
    }),
    tags = ["manual"],
)

alias(
    name = "mmi",
    actual = select({
        "//rules:use_cw340": "//hw/bitstream/cw340:mmi",
        "//conditions:default": ":_hyper310_mmi",
    }),
)
