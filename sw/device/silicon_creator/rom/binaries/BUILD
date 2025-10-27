# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load("//rules/opentitan:defs.bzl", "opentitan_binary")

package(default_visibility = ["//visibility:public"])

exports_files(glob(["*.elf"]))

opentitan_binary(
    name = "earlgrey_A2_rom",
    collect_code_coverage = 0,
    exec_env = [
        "//hw/top_earlgrey:fpga_cw310",
        "//hw/top_earlgrey:fpga_cw340",
        "//hw/top_earlgrey:sim_dv_base",
        "//hw/top_earlgrey:sim_verilator_base",
        "//hw/top_earlgrey:silicon_creator",
    ],
    kind = "rom",
    prebuilt_elf = glob(["earlgrey_A2_rom_*.elf"]),
)
