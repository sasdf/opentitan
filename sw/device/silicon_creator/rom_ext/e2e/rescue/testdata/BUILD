# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load("//rules/opentitan:cc.bzl", "exec_env_filegroup")

package(default_visibility = ["//visibility:public"])

genrule(
    name = "rom_ext_dice_x509_slot_virtual_qemu_bin",
    srcs = ["rom_ext_dice_x509_slot_virtual_fpga_cw310.signed.bin"],
    outs = ["rom_ext_dice_x509_slot_virtual_sim_qemu.signed.bin"],
    cmd = "cp $< $@",
)

# These ROM_EXTs were built at 6ae909eed1efd43e0abd36f580504b56aaf19850 to
# allow testing the the latest version of opentitantool against an older
# ROM_EXT implementing the (unversioned) version 0 rescue protocol.
exec_env_filegroup(
    name = "rom_ext_rescue_protocol_0",
    testonly = True,
    exec_env = {
        "//hw/top_earlgrey:fpga_hyper310_rom_with_fake_keys": "cw310",
        "//hw/top_earlgrey:fpga_cw340_rom_with_fake_keys": "cw340",
        "//hw/top_earlgrey:sim_qemu_base": "qemu",
    },
    files = {
        "rom_ext_dice_x509_slot_virtual_fpga_cw310.signed.bin": "cw310",
        "rom_ext_dice_x509_slot_virtual_fpga_cw340.signed.bin": "cw340",
        ":rom_ext_dice_x509_slot_virtual_qemu_bin": "qemu",
    },
)
