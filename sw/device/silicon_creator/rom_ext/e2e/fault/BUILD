# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load(
    "//rules/opentitan:defs.bzl",
    "fpga_params",
    "opentitan_binary",
    "opentitan_test",
)

opentitan_test(
    name = "rescue_unsigned_upgrade_test",
    data = ["unsigned_v5.bin"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_ext": None,
    },
    fpga = fpga_params(
        assemble = "{rom_ext}@{rom_ext_slot_a}",
        changes_otp = True,  # i.e. changes owner
        good_owner = "owner_page: 1-SEAL/1-SEAL\r\n",
        fault_owner = "owner_page: 5-SEAL/5-SEAL\r\n",
        test_cmd = """
            --exec="transport init"
            --exec="fpga load-bitstream {bitstream}"
            --exec="bootstrap --clear-uart=true {rom_ext}"
            # First make sure both owner page are version 1 test owner.
            --exec="console --non-interactive --exit-success='{good_owner}' --exit-failure='{exit_failure}'"
            # Upload the unsigned owner.
            --exec="rescue set-owner-config sw/device/silicon_creator/rom_ext/e2e/fault/unsigned_v5.bin"
            # Reboot and fault the ROM_EXT to accept unsigned owner.
            --exec="console --non-interactive --exit-success='{fault_owner}' --exit-failure='{exit_failure}'"
            no-op
        """,
    ),
)

opentitan_test(
    name = "rescue_self_signed_upgrade_test",
    data = ["self_signed.bin"],
exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_ext": None,
    },
    fpga = fpga_params(
        assemble = "{rom_ext}@{rom_ext_slot_a}",
        changes_otp = True,  # i.e. changes owner
        good_owner = "owner_page: 1-SEAL/1-SEAL\r\n",
        fault_owner = "owner_page: 2-SEAL/2-SEAL\r\n",
        test_cmd = """
            --exec="transport init"
            --exec="fpga load-bitstream {bitstream}"
            --exec="bootstrap --clear-uart=true {rom_ext}"
            # First make sure both owner page are version 1 test owner.
            --exec="console --non-interactive --exit-success='{good_owner}' --exit-failure='{exit_failure}'"
            # Upload the self-signed owner.
            --exec="rescue set-owner-config sw/device/silicon_creator/rom_ext/e2e/fault/self_signed.bin"
            # Reboot and fault the ROM_EXT to accept self-signed owner.
            --exec="console --non-interactive --exit-success='{fault_owner}' --exit-failure='{exit_failure}'"
            no-op
        """,
    ),
)

opentitan_test(
    name = "bl0_unsigned_upgrade_test",
    srcs = ["bl0_unsigned_upgrade_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_ext": None,
    },
    fpga = fpga_params(
        changes_otp = True,  # i.e. changes owner
    ),
    deps = [
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:status",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
        "//sw/device/silicon_creator/lib/drivers:flash_ctrl",
        "//sw/device/silicon_creator/lib/drivers:retention_sram",
        "//sw/device/silicon_creator/lib/drivers:rstmgr",
        "//sw/device/silicon_creator/lib/ownership:owner_block",
    ],
)

opentitan_test(
    name = "bl0_self_signed_upgrade_test",
    srcs = ["bl0_self_signed_upgrade_test.c"],
    exec_env = {
        "//hw/top_earlgrey:fpga_cw340_rom_ext": None,
    },
    fpga = fpga_params(
        changes_otp = True,  # i.e. changes owner
    ),
    deps = [
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:status",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
        "//sw/device/silicon_creator/lib/drivers:flash_ctrl",
        "//sw/device/silicon_creator/lib/drivers:retention_sram",
        "//sw/device/silicon_creator/lib/drivers:rstmgr",
        "//sw/device/silicon_creator/lib/ownership:owner_block",
        ":self_signed_bin_h",
    ],
)

cc_library(
    name = "self_signed_bin_h",
    hdrs = ["self_signed.bin.h"],
)
