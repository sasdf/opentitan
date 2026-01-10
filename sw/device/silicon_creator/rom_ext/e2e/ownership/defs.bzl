# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load(
    "//rules/opentitan:defs.bzl",
    "fpga_params",
    "opentitan_test",
)

def ownership_transfer_test(
        name,
        srcs = ["//sw/device/silicon_creator/rom_ext/e2e/verified_boot:boot_test"],
        exec_env = {
            "//hw/top_earlgrey:fpga_hyper310_rom_ext": None,
        },
        ecdsa_key = {
            "//sw/device/silicon_creator/lib/ownership/keys/dummy:app_prod_ecdsa": "app_prod",
        },
        fpga = None,
        manifest = None,
        data = [
            "//sw/device/silicon_creator/lib/ownership/keys/dummy:activate_key",
            "//sw/device/silicon_creator/lib/ownership/keys/dummy:app_prod_ecdsa_pub",
            "//sw/device/silicon_creator/lib/ownership/keys/dummy:owner_key",
            "//sw/device/silicon_creator/lib/ownership/keys/dummy:owner_key_pub",
            "//sw/device/silicon_creator/lib/ownership/keys/dummy:unlock_key",
            "//sw/device/silicon_creator/lib/ownership/keys/fake:unlock_key",
            "//sw/device/silicon_creator/lib/ownership/keys/fake:activate_key",
            "//sw/device/silicon_creator/lib/ownership/keys/fake:owner_key",
            "//sw/device/silicon_creator/lib/ownership/keys/fake:owner_key_pub",
            "//sw/device/silicon_creator/lib/ownership/keys/fake:app_prod_ecdsa_pub",
        ],
        defines = ["WITH_OWNERSHIP_INFO=1"],
        deps = [
            "//sw/device/lib/base:status",
            "//sw/device/lib/testing/test_framework:ottf_main",
            "//sw/device/silicon_creator/lib:boot_log",
            "//sw/device/silicon_creator/lib/drivers:flash_ctrl",
            "//sw/device/silicon_creator/lib/drivers:retention_sram",
            "//sw/device/silicon_creator/lib/ownership:datatypes",
        ],
        **kwargs):
    if fpga == None:
      fpga = fpga_params()
    if not hasattr(fpga, "testopt_clear_after_test"):
      fpga.testopt_clear_after_test = "True",
    if not hasattr(fpga, "testopt_clear_before_test"):
      fpga.testopt_clear_before_test = "True",
    if not hasattr(fpga, "testopt_bootstrap"):
      fpga.testopt_bootstrap = "True",
    opentitan_test(
        name = name,
        srcs = srcs,
        exec_env = exec_env,
        ecdsa_key = ecdsa_key,
        fpga = fpga,
        manifest = manifest,
        data = data,
        defines = defines,
        deps = deps,
        **kwargs
    )
