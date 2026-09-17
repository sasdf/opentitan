# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load("//rules/opentitan:defs.bzl", "OPENTITAN_CPU", "opentitan_test", "qemu_params")
load("//rules/opentitan:qemu.bzl", "qemu_otp")
load(
    "//rules:otp.bzl",
    "STD_OTP_OVERLAYS_WITHOUT_SECRET_PARTITIONS",
    "otp_image",
    "otp_json",
    "otp_partition",
)


opentitan_test(
    name = "i2c_host_tx_rx_test",
    srcs = ["i2c_host_tx_rx_test.c"],
    exec_env = {
        "//hw/top_earlgrey:sim_dv": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_sival_rom_ext": None,
    },
    deps = [
        "//hw/ip/lc_ctrl/data:lc_ctrl_c_regs",
        "//hw/top_earlgrey/ip_autogen/clkmgr:clkmgr_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:math",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:i2c",
        "//sw/device/lib/dif:pinmux",
        "//sw/device/lib/dif:rv_plic",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:irq",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:i2c_testutils",
        "//sw/device/lib/testing:isr_testutils",
        "//sw/device/lib/testing:rand_testutils",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "i2c_device_tx_rx_test",
    srcs = ["i2c_device_tx_rx_test.c"],
    exec_env = {
        "//hw/top_earlgrey:sim_dv": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_sival_rom_ext": None,
    },
    deps = [
        "//hw/ip/lc_ctrl/data:lc_ctrl_c_regs",
        "//hw/top_earlgrey/ip_autogen/clkmgr:clkmgr_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:math",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:i2c",
        "//sw/device/lib/dif:pinmux",
        "//sw/device/lib/dif:rv_plic",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:irq",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:i2c_testutils",
        "//sw/device/lib/testing:isr_testutils",
        "//sw/device/lib/testing:rand_testutils",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "flash_ctrl_lc_rw_en_test",
    srcs = ["flash_ctrl_lc_rw_en_test.c"],
    exec_env = {"//hw/top_earlgrey:sim_dv": None},
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:macros",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:flash_ctrl",
        "//sw/device/lib/dif:keymgr",
        "//sw/device/lib/dif:kmac",
        "//sw/device/lib/dif:lc_ctrl",
        "//sw/device/lib/dif:otp_ctrl",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:flash_ctrl_testutils",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "flash_rma_unlocked_test",
    srcs = ["flash_rma_unlocked_test.c"],
    # This test doesn't use the OTTF.
    exec_env = {"//hw/top_earlgrey:sim_dv": None},
    # This test is designed to run and complete entirely in the ROM boot stage.
    # Setting the `test_in_rom` flag makes the `opentitan_test` rule aware
    # of this, and instructs it to load the test image into ROM.
    kind = "rom",
    linker_script = "//sw/device/lib/testing/test_rom:linker_script",
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/arch:device",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:flash_ctrl",
        "//sw/device/lib/dif:lc_ctrl",
        "//sw/device/lib/dif:otp_ctrl",
        "//sw/device/lib/dif:uart",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/runtime:print",
        "//sw/device/lib/testing:flash_ctrl_testutils",
        "//sw/device/lib/testing:pinmux_testutils",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:status",
        "//sw/device/lib/testing/test_rom:test_rom_lib",
    ],
)

opentitan_test(
    name = "flash_init_test",
    srcs = ["flash_init_test.c"],
    # This test doesn't use the OTTF.
    exec_env = {"//hw/top_earlgrey:sim_dv": None},
    # This test is designed to run and complete entirely in the ROM boot stage.
    # Setting the `test_in_rom` flag makes the `opentitan_test` rule aware
    # of this, and instructs it to load the test image into ROM (rather than
    # loading the default test ROM, or any other ROM that may be specified).
    kind = "rom",
    linker_script = "//sw/device/lib/testing/test_rom:linker_script",
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/arch:device",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:flash_ctrl",
        "//sw/device/lib/dif:uart",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/runtime:print",
        "//sw/device/lib/testing:flash_ctrl_testutils",
        "//sw/device/lib/testing:pinmux_testutils",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:status",
        "//sw/device/lib/testing/test_rom:test_rom_lib",
    ],
)

opentitan_test(
    name = "gpio_test",
    srcs = ["gpio_test.c"],
    exec_env = {
        "//hw/top_earlgrey:sim_dv": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_sival_rom_ext": None,
    },
    qemu = qemu_params(
        test_harness = "//sw/host/tests/chip/gpio:gpio_sim_dv",
    ),
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/arch:device",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:gpio",
        "//sw/device/lib/dif:rv_plic",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:irq",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:pinmux_testutils",
        "//sw/device/lib/testing:rv_plic_testutils",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "sysrst_ctrl_ec_rst_l_test",
    srcs = ["sysrst_ctrl_ec_rst_l_test.c"],
    exec_env = {
        "//hw/top_earlgrey:sim_dv": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_sival_rom_ext": None,
    },
    qemu = qemu_params(
        test_cmd = "{firmware:elf}",
        test_harness = "//sw/host/tests/chip/sysrst_ctrl:sysrst_ctrl_ec_rst_l_sim_dv",
    ),
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:pinmux",
        "//sw/device/lib/dif:pwrmgr",
        "//sw/device/lib/dif:rstmgr",
        "//sw/device/lib/dif:sysrst_ctrl",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:flash_ctrl_testutils",
        "//sw/device/lib/testing:rstmgr_testutils",
        "//sw/device/lib/testing:sysrst_ctrl_testutils",
        "//sw/device/lib/testing/test_framework:ottf_main",
        "//sw/device/lib/testing/test_framework:ottf_utils",
    ],
)

opentitan_test(
    name = "lc_ctrl_scrap_test",
    srcs = ["lc_ctrl_scrap_test.c"],
    exec_env = {
        "//hw/top_earlgrey:sim_dv": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    qemu = qemu_params(
        needs_jtag = True,
        test_cmd = "--bootstrap={firmware} --initial-lc-state=rma",
        test_harness = "//sw/host/tests/manuf/manuf_scrap",
    ),
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:memory",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:lc_ctrl",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:lc_ctrl_testutils",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "otp_ctrl_vendor_test_csr_access_test",
    srcs = ["otp_ctrl_vendor_test_csr_access_test.c"],
    exec_env = {
        "//hw/top_earlgrey:sim_dv": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_sival_rom_ext": None,
    },
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/arch:device",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:memory",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:lc_ctrl",
        "//sw/device/lib/dif:otp_ctrl",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "otp_ctrl_vendor_test_ecc_error_test",
    srcs = ["otp_ctrl_vendor_test_ecc_error_test.c"],
    exec_env = {
        "//hw/top_earlgrey:sim_dv": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    qemu = qemu_params(
        globals = {
            "ot-otp-eg.inject-vendor-test-ecc-error": "true",
        },
    ),
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:memory",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:otp_ctrl",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:otp_ctrl_testutils",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

cc_library(
    name = "lc_ctrl_transition_impl",
    srcs = ["lc_ctrl_transition_impl.c"],
    hdrs = ["lc_ctrl_transition_impl.h"],
    target_compatible_with = [OPENTITAN_CPU],
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:memory",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:lc_ctrl",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:lc_ctrl_testutils",
        "//sw/device/lib/testing/test_framework:check",
    ],
)

otp_json(
    name = "lc_ctrl_transition_test_otp_json",
    partitions = [
        otp_partition(
            name = "LIFE_CYCLE",
            count = 8,
            state = "TEST_LOCKED1",
        ),
        otp_partition(
            name = "SECRET0",
            items = {
                "TEST_UNLOCK_TOKEN": "0xde0a1f1e0d6a649fd35fadb75ec82674",
                "TEST_EXIT_TOKEN": "0x547070d7503264af5b9a971b894ef3be",
            },
            lock = True,
        ),
    ],
    seed = "52408960416235844780753299194502148156786072650816676092165912261205302331741",
)

otp_image(
    name = "lc_ctrl_transition_test_otp_img",
    src = ":lc_ctrl_transition_test_otp_json",
    overlays = STD_OTP_OVERLAYS_WITHOUT_SECRET_PARTITIONS + [
        "//hw/ip/otp_ctrl/data:otp_json_secret1",
        "//hw/ip/otp_ctrl/data:otp_json_secret2_unlocked",
    ],
)

opentitan_test(
    name = "lc_ctrl_transition_test",
    srcs = ["lc_ctrl_transition_test.c"],
    ecdsa_key = {"//sw/device/silicon_creator/rom/keys/fake/ecdsa:ecdsa_keyset": "prod_key_0"},
    exec_env = {
        "//hw/top_earlgrey:sim_dv": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    qemu = qemu_params(
        needs_jtag = True,
        otp = ":lc_ctrl_transition_test_otp_img",
        test_harness = "//sw/host/tests/qemu:lc_ctrl_transition",
    ),
    deps = [
        ":lc_ctrl_transition_impl",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "lc_ctrl_volatile_raw_unlock_test",
    srcs = ["lc_ctrl_volatile_raw_unlock_test.c"],
    exec_env = {
        "//hw/top_earlgrey:sim_dv": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    qemu = qemu_params(
        otp = "//hw/ip/otp_ctrl/data:img_test_unlocked0",
    ),
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:memory",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:lc_ctrl",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:lc_ctrl_testutils",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

otp_image(
    name = "lc_walkthrough_test_otp_img",
    src = "//hw/ip/otp_ctrl/data:otp_json_raw",
    overlays = STD_OTP_OVERLAYS_WITHOUT_SECRET_PARTITIONS,
)

opentitan_test(
    name = "lc_walkthrough_test",
    srcs = ["lc_walkthrough_test.c"],
    ecdsa_key = {"//sw/device/silicon_creator/rom/keys/fake/ecdsa:ecdsa_keyset": "prod_key_0"},
    exec_env = {
        "//hw/top_earlgrey:sim_dv": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    qemu = qemu_params(
        needs_jtag = True,
        otp = ":lc_walkthrough_test_otp_img",
        test_cmd = "--firmware={firmware}",
        test_harness = "//sw/host/tests/qemu:lc_walkthrough",
    ),
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:memory",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:lc_ctrl",
        "//sw/device/lib/dif:otp_ctrl",
        "//sw/device/lib/dif:rstmgr",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:lc_ctrl_testutils",
        "//sw/device/lib/testing:otp_ctrl_testutils",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
        "//sw/device/lib/testing/test_framework:ottf_test_config",
    ],
)

opentitan_test(
    name = "lc_walkthrough_testunlocks_test",
    srcs = ["lc_walkthrough_testunlocks_test.c"],
    exec_env = {
        "//hw/top_earlgrey:sim_dv": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    qemu = qemu_params(
        needs_jtag = True,
        otp = ":lc_walkthrough_test_otp_img",
        test_cmd = "--testunlocks",
        test_harness = "//sw/host/tests/qemu:lc_walkthrough",
    ),
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:memory",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:lc_ctrl",
        "//sw/device/lib/dif:otp_ctrl",
        "//sw/device/lib/dif:rstmgr",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:lc_ctrl_testutils",
        "//sw/device/lib/testing:otp_ctrl_testutils",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "all_escalation_resets_test",
    srcs = ["all_escalation_resets_test.c"],
    exec_env = {"//hw/top_earlgrey:sim_dv": None},
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:memory",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:aes",
        "//sw/device/lib/dif:alert_handler",
        "//sw/device/lib/dif:aon_timer",
        "//sw/device/lib/dif:clkmgr",
        "//sw/device/lib/dif:flash_ctrl",
        "//sw/device/lib/dif:kmac",
        "//sw/device/lib/dif:lc_ctrl",
        "//sw/device/lib/dif:otp_ctrl",
        "//sw/device/lib/dif:pwrmgr",
        "//sw/device/lib/dif:rom_ctrl",
        "//sw/device/lib/dif:rstmgr",
        "//sw/device/lib/dif:rv_core_ibex",
        "//sw/device/lib/dif:rv_plic",
        "//sw/device/lib/dif:sram_ctrl",
        "//sw/device/lib/runtime:irq",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:alert_handler_testutils",
        "//sw/device/lib/testing:aon_timer_testutils",
        "//sw/device/lib/testing:ret_sram_testutils",
        "//sw/device/lib/testing:rstmgr_testutils",
        "//sw/device/lib/testing:rv_plic_testutils",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "data_integrity_escalation_reset_test",
    srcs = ["data_integrity_escalation_reset_test.c"],
    exec_env = {"//hw/top_earlgrey:sim_dv": None},
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/dif:alert_handler",
        "//sw/device/lib/dif:aon_timer",
        "//sw/device/lib/dif:flash_ctrl",
        "//sw/device/lib/dif:rstmgr",
        "//sw/device/lib/dif:rv_core_ibex",
        "//sw/device/lib/dif:rv_plic",
        "//sw/device/lib/dif:sram_ctrl",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:alert_handler_testutils",
        "//sw/device/lib/testing:aon_timer_testutils",
        "//sw/device/lib/testing:flash_ctrl_testutils",
        "//sw/device/lib/testing:ret_sram_testutils",
        "//sw/device/lib/testing:rstmgr_testutils",
        "//sw/device/lib/testing:rv_plic_testutils",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "clkmgr_external_clk_src_for_lc_test",
    srcs = ["clkmgr_external_clk_src_for_lc_test.c"],
    ecdsa_key = {"//sw/device/silicon_creator/rom/keys/fake/ecdsa:ecdsa_keyset": "prod_key_0"},
    exec_env = {
        "//hw/top_earlgrey:sim_dv": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    qemu = qemu_params(
        needs_jtag = True,
        otp = ":lc_ctrl_transition_test_otp_img",
        test_cmd = "--expect-ext-clk",
        test_harness = "//sw/host/tests/qemu:lc_ctrl_transition",
    ),
    deps = [
        ":lc_ctrl_transition_impl",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "pwrmgr_main_power_glitch_test",
    srcs = ["pwrmgr_main_power_glitch_test.c"],
    exec_env = {
        "//hw/top_earlgrey:sim_dv": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_sival_rom_ext": None,
    },
    deps = [
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:rstmgr",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:rstmgr_testutils",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "pwrmgr_sleep_power_glitch_test",
    srcs = ["pwrmgr_sleep_power_glitch_test.c"],
    exec_env = {
        "//hw/top_earlgrey:sim_dv": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_sival_rom_ext": None,
    },
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:pwrmgr",
        "//sw/device/lib/dif:rstmgr",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:pwrmgr_testutils",
        "//sw/device/lib/testing:rstmgr_testutils",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "pwrmgr_deep_sleep_power_glitch_test",
    srcs = ["pwrmgr_deep_sleep_power_glitch_test.c"],
    exec_env = {
        "//hw/top_earlgrey:sim_dv": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_sival_rom_ext": None,
    },
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:pwrmgr",
        "//sw/device/lib/dif:rstmgr",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:pwrmgr_testutils",
        "//sw/device/lib/testing:rstmgr_testutils",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "pwrmgr_sysrst_ctrl_test",
    srcs = ["pwrmgr_sysrst_ctrl_test.c"],
    exec_env = {
        "//hw/top_earlgrey:sim_dv": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_sival_rom_ext": None,
    },
    qemu = qemu_params(
        test_harness = "//sw/host/tests/chip/sysrst_ctrl:pwrmgr_sysrst_ctrl_sim_dv",
    ),
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:aon_timer",
        "//sw/device/lib/dif:flash_ctrl",
        "//sw/device/lib/dif:pinmux",
        "//sw/device/lib/dif:pwrmgr",
        "//sw/device/lib/dif:rstmgr",
        "//sw/device/lib/dif:sysrst_ctrl",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:aon_timer_testutils",
        "//sw/device/lib/testing:flash_ctrl_testutils",
        "//sw/device/lib/testing:nv_counter_testutils",
        "//sw/device/lib/testing:pinmux_testutils",
        "//sw/device/lib/testing:pwrmgr_testutils",
        "//sw/device/lib/testing:rstmgr_testutils",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "pwrmgr_random_sleep_power_glitch_reset_test",
    srcs = ["pwrmgr_random_sleep_power_glitch_reset_test.c"],
    exec_env = {
        "//hw/top_earlgrey:sim_dv": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_sival_rom_ext": None,
    },
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:alert_handler",
        "//sw/device/lib/dif:aon_timer",
        "//sw/device/lib/dif:flash_ctrl",
        "//sw/device/lib/dif:pinmux",
        "//sw/device/lib/dif:pwrmgr",
        "//sw/device/lib/dif:rstmgr",
        "//sw/device/lib/dif:rv_plic",
        "//sw/device/lib/dif:rv_timer",
        "//sw/device/lib/dif:sysrst_ctrl",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:alert_handler_testutils",
        "//sw/device/lib/testing:aon_timer_testutils",
        "//sw/device/lib/testing:flash_ctrl_testutils",
        "//sw/device/lib/testing:nv_counter_testutils",
        "//sw/device/lib/testing:pinmux_testutils",
        "//sw/device/lib/testing:pwrmgr_testutils",
        "//sw/device/lib/testing:rstmgr_testutils",
        "//sw/device/lib/testing:rv_plic_testutils",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

qemu_otp(
    name = "rom_ctrl_integrity_check_otp_prod",
    vmem = "//hw/ip/otp_ctrl/data:img_prod",
)

opentitan_test(
    name = "rom_ctrl_integrity_check_test",
    srcs = ["rom_ctrl_integrity_check_test.c"],
    exec_env = {
        "//hw/top_earlgrey:sim_dv": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    qemu = qemu_params(
        data = [":rom_ctrl_integrity_check_otp_prod"],
        globals = {
            "ot-rom_ctrl.corrupt-digest": "true",
        },
        test_cmd = "--prod-otp=$(rootpath :rom_ctrl_integrity_check_otp_prod)",
        test_harness = "//sw/host/tests/qemu:rom_ctrl_integrity_check",
    ),
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/arch:device",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:lc_ctrl",
        "//sw/device/lib/dif:rom_ctrl",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:ottf_main",
        "//sw/device/lib/testing/test_framework:status",
    ],
)

opentitan_test(
    name = "spi_host_tx_rx_test",
    srcs = ["spi_host_tx_rx_test.c"],
    exec_env = {
        "//hw/top_earlgrey:sim_dv": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_sival_rom_ext": None,
    },
    qemu = qemu_params(
        globals = {
            "ot-spi_host.loopback": "true",
        },
    ),
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:pinmux",
        "//sw/device/lib/dif:spi_host",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:irq",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:pinmux_testutils",
        "//sw/device/lib/testing:rand_testutils",
        "//sw/device/lib/testing:spi_host_testutils",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "spi_passthrough_test",
    srcs = ["spi_passthrough_test.c"],
    exec_env = {
        "//hw/top_earlgrey:sim_dv": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_sival_rom_ext": None,
    },
    qemu = qemu_params(
        test_harness = "//sw/host/tests/chip/spi_device:spi_passthrough_sim_dv",
    ),
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/arch:device",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:pinmux",
        "//sw/device/lib/dif:rv_plic",
        "//sw/device/lib/dif:spi_device",
        "//sw/device/lib/dif:spi_host",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:irq",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:pinmux_testutils",
        "//sw/device/lib/testing:spi_device_testutils",
        "//sw/device/lib/testing:spi_flash_testutils",
        "//sw/device/lib/testing:spi_host_testutils",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "sram_ctrl_execution_main_test",
    srcs = ["sram_ctrl_execution_main_test.c"],
    exec_env = {
        "//hw/top_earlgrey:sim_dv": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_sival": None,
    },
    deps = [
        "//hw/ip/otp_ctrl/data:otp_ctrl_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/arch:device",
        "//sw/device/lib/base:bitfield",
        "//sw/device/lib/base:csr",
        "//sw/device/lib/base:macros",
        "//sw/device/lib/base:multibits",
        "//sw/device/lib/dif:sram_ctrl",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:lc_ctrl_testutils",
        "//sw/device/lib/testing:otp_ctrl_testutils",
        "//sw/device/lib/testing:sram_ctrl_testutils",
        "//sw/device/lib/testing/test_framework:ottf_main",
        "//sw/device/lib/testing/test_framework:status",
        "//sw/device/silicon_creator/lib:epmp_state",
    ],
)

cc_library(
    name = "pwrmgr_sleep_all_wake_ups_impl",
    srcs = ["pwrmgr_sleep_all_wake_ups_impl.c"],
    hdrs = ["pwrmgr_sleep_all_wake_ups_impl.h"],
    target_compatible_with = [OPENTITAN_CPU],
    visibility = ["//sw/device/tests:__pkg__"],
    deps = [
        "//hw/top_earlgrey/ip_autogen/pwrmgr:pwrmgr_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/arch:device",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:adc_ctrl",
        "//sw/device/lib/dif:pinmux",
        "//sw/device/lib/dif:pwrmgr",
        "//sw/device/lib/dif:rv_plic",
        "//sw/device/lib/dif:sensor_ctrl",
        "//sw/device/lib/dif:usbdev",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:irq",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:aon_timer_testutils",
        "//sw/device/lib/testing:isr_testutils",
        "//sw/device/lib/testing:pwrmgr_testutils",
        "//sw/device/lib/testing:rv_plic_testutils",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "adc_ctrl_sleep_debug_cable_wakeup_test",
    srcs = ["adc_ctrl_sleep_debug_cable_wakeup_test.c"],
    exec_env = {
        "//hw/top_earlgrey:sim_dv": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_sival_rom_ext": None,
    },
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:adc_ctrl",
        "//sw/device/lib/dif:pwrmgr",
        "//sw/device/lib/dif:rstmgr",
        "//sw/device/lib/dif:rv_plic",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:irq",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:isr_testutils",
        "//sw/device/lib/testing:pwrmgr_testutils",
        "//sw/device/lib/testing:rstmgr_testutils",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "pwrmgr_b2b_sleep_reset_test",
    srcs = ["pwrmgr_b2b_sleep_reset_test.c"],
    exec_env = {
        "//hw/top_earlgrey:sim_dv": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_sival_rom_ext": None,
    },
    qemu = qemu_params(
        test_harness = "//sw/host/tests/chip/sysrst_ctrl:pwrmgr_sysrst_ctrl_sim_dv",
    ),
    deps = [
        "//hw/ip/aon_timer/data:aon_timer_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:flash_ctrl",
        "//sw/device/lib/dif:pinmux",
        "//sw/device/lib/dif:pwrmgr",
        "//sw/device/lib/dif:rstmgr",
        "//sw/device/lib/dif:sysrst_ctrl",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:aon_timer_testutils",
        "//sw/device/lib/testing:flash_ctrl_testutils",
        "//sw/device/lib/testing:nv_counter_testutils",
        "//sw/device/lib/testing:pwrmgr_testutils",
        "//sw/device/lib/testing:rstmgr_testutils",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "sensor_ctrl_status_test",
    srcs = ["sensor_ctrl_status.c"],
    exec_env = {
        "//hw/top_earlgrey:sim_dv": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_sival_rom_ext": None,
    },
    deps = [
        "//hw/top_earlgrey/ip/sensor_ctrl/data:sensor_ctrl_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:memory",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:rv_plic",
        "//sw/device/lib/dif:sensor_ctrl",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:irq",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/runtime:print",
        "//sw/device/lib/testing:isr_testutils",
        "//sw/device/lib/testing:rv_plic_testutils",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "alert_handler_entropy_test",
    srcs = ["alert_handler_entropy_test.c"],
    exec_env = {
        "//hw/top_earlgrey:sim_dv": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_sival_rom_ext": None,
    },
    deps = [
        "//hw/top_earlgrey:alert_handler_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/arch:device",
        "//sw/device/lib/base:memory",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:alert_handler",
        "//sw/device/lib/runtime:irq",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:alert_handler_testutils",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "alert_handler_lpg_sleep_mode_alerts_test",
    srcs = ["alert_handler_lpg_sleep_mode_alerts.c"],
    # The test requires to drive fatal alerts which is possible only in DV sim.
    exec_env = {
        "//hw/top_earlgrey:sim_dv": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
        "//hw/top_earlgrey:sim_qemu_sival_rom_ext": None,
    },
    deps = [
        "//hw/top_earlgrey:alert_handler_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:math",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:alert_handler",
        "//sw/device/lib/dif:rstmgr",
        "//sw/device/lib/dif:rv_plic",
        "//sw/device/lib/dif:sensor_ctrl",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:irq",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:alert_handler_testutils",
        "//sw/device/lib/testing:aon_timer_testutils",
        "//sw/device/lib/testing:isr_testutils",
        "//sw/device/lib/testing:pwrmgr_testutils",
        "//sw/device/lib/testing:rand_testutils",
        "//sw/device/lib/testing:ret_sram_testutils",
        "//sw/device/lib/testing:rstmgr_testutils",
        "//sw/device/lib/testing:rv_plic_testutils",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "lc_ctrl_program_error",
    srcs = ["lc_ctrl_program_error.c"],
    exec_env = {
        "//hw/top_earlgrey:sim_dv": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    qemu = qemu_params(
        globals = {
            "ot-lc_ctrl.inject-otp-prog-error": "true",
        },
    ),
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/arch:device",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:memory",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:alert_handler",
        "//sw/device/lib/dif:lc_ctrl",
        "//sw/device/lib/dif:rstmgr",
        "//sw/device/lib/dif:rv_core_ibex",
        "//sw/device/lib/dif:rv_plic",
        "//sw/device/lib/dif:sram_ctrl",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:irq",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:alert_handler_testutils",
        "//sw/device/lib/testing:lc_ctrl_testutils",
        "//sw/device/lib/testing:ret_sram_testutils",
        "//sw/device/lib/testing:rstmgr_testutils",
        "//sw/device/lib/testing:rv_plic_testutils",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "ast_usb_clk_calib",
    srcs = ["ast_usb_clk_calib.c"],
    exec_env = {"//hw/top_earlgrey:sim_dv": None},
    deps = [
        "//hw/top_earlgrey/ip/ast/data:ast_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:memory",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:base",
        "//sw/device/lib/dif:clkmgr",
        "//sw/device/lib/dif:pinmux",
        "//sw/device/lib/dif:usbdev",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:clkmgr_testutils",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "csrng_fuse_en_sw_app_read",
    srcs = ["csrng_fuse_en_sw_app_read.c"],
    exec_env = {"//hw/top_earlgrey:sim_dv": None},
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:memory",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:base",
        "//sw/device/lib/dif:csrng",
        "//sw/device/lib/dif:rstmgr",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:csrng_testutils",
        "//sw/device/lib/testing:otp_ctrl_testutils",
        "//sw/device/lib/testing:rstmgr_testutils",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "csrng_lc_hw_debug_en_test",
    srcs = ["csrng_lc_hw_debug_en_test.c"],
    exec_env = {"//hw/top_earlgrey:sim_dv": None},
    deps = [
        "//sw/device/lib/base:macros",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:csrng",
        "//sw/device/lib/dif:entropy_src",
        "//sw/device/lib/dif:flash_ctrl",
        "//sw/device/lib/dif:kmac",
        "//sw/device/lib/dif:lc_ctrl",
        "//sw/device/lib/dif:otp_ctrl",
        "//sw/device/lib/dif:rstmgr",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:csrng_testutils",
        "//sw/device/lib/testing:entropy_testutils",
        "//sw/device/lib/testing:flash_ctrl_testutils",
        "//sw/device/lib/testing:otp_ctrl_testutils",
        "//sw/device/lib/testing:rand_testutils",
        "//sw/device/lib/testing:rstmgr_testutils",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

otp_image(
    name = "inject_scramble_seed_otp_img",
    src = "//hw/ip/otp_ctrl/data:otp_json_prod",
    overlays = STD_OTP_OVERLAYS_WITHOUT_SECRET_PARTITIONS + [
        "//hw/ip/otp_ctrl/data:otp_json_secret0",
        "//hw/ip/otp_ctrl/data:otp_json_secret2_unlocked",
    ],
)

opentitan_test(
    name = "inject_scramble_seed",
    srcs = ["inject_scramble_seed.c"],
    ecdsa_key = {"//sw/device/silicon_creator/rom/keys/fake/ecdsa:ecdsa_keyset": "prod_key_0"},
    exec_env = {
        "//hw/top_earlgrey:sim_dv": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    qemu = qemu_params(
        otp = ":inject_scramble_seed_otp_img",
    ),
    deps = [
        "//hw/ip/otp_ctrl/data:otp_ctrl_c_regs",
        "//hw/top_earlgrey/ip_autogen/flash_ctrl:flash_ctrl_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:flash_ctrl",
        "//sw/device/lib/dif:otp_ctrl",
        "//sw/device/lib/dif:rstmgr",
        "//sw/device/lib/runtime:hart",
        "//sw/device/lib/runtime:irq",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:flash_ctrl_testutils",
        "//sw/device/lib/testing:otp_ctrl_testutils",
        "//sw/device/lib/testing:rand_testutils",
        "//sw/device/lib/testing:rstmgr_testutils",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "exit_test_unlocked_bootstrap",
    srcs = ["exit_test_unlocked_bootstrap.c"],
    exec_env = {"//hw/top_earlgrey:sim_dv": None},
    deps = [
        "//hw/ip/otp_ctrl/data:otp_ctrl_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:abs_mmio",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:lc_ctrl",
        "//sw/device/lib/dif:rstmgr",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

otp_image(
    name = "otp_ctrl_lc_signals_test_otp_img",
    src = "//hw/ip/otp_ctrl/data:otp_json_dev",
    overlays = STD_OTP_OVERLAYS_WITHOUT_SECRET_PARTITIONS + [
        "//hw/ip/otp_ctrl/data:otp_json_secret0",
        "//hw/ip/otp_ctrl/data:otp_json_secret1",
    ],
)

opentitan_test(
    name = "otp_ctrl_lc_signals_test",
    srcs = ["otp_ctrl_lc_signals_test.c"],
    ecdsa_key = {"//sw/device/silicon_creator/rom/keys/fake/ecdsa:ecdsa_keyset": "prod_key_0"},
    exec_env = {
        "//hw/top_earlgrey:sim_dv": None,
        "//hw/top_earlgrey:sim_qemu_rom_with_fake_keys": None,
    },
    qemu = qemu_params(
        otp = ":otp_ctrl_lc_signals_test_otp_img",
    ),
    deps = [
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:memory",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:flash_ctrl",
        "//sw/device/lib/dif:keymgr",
        "//sw/device/lib/dif:kmac",
        "//sw/device/lib/dif:lc_ctrl",
        "//sw/device/lib/dif:otp_ctrl",
        "//sw/device/lib/dif:rstmgr",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:keymgr_testutils",
        "//sw/device/lib/testing:kmac_testutils",
        "//sw/device/lib/testing:lc_ctrl_testutils",
        "//sw/device/lib/testing:otp_ctrl_testutils",
        "//sw/device/lib/testing:rstmgr_testutils",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
        "//sw/device/lib/testing/test_framework:ottf_test_config",
        "//sw/device/silicon_creator/lib/base:chip",
        "//sw/device/silicon_creator/lib/drivers:retention_sram",
    ],
)

opentitan_test(
    name = "ast_clk_rst_inputs",
    srcs = ["ast_clk_rst_inputs.c"],
    exec_env = {"//hw/top_earlgrey:sim_dv": None},
    deps = [
        "//hw/top_earlgrey/ip/sensor_ctrl/data:sensor_ctrl_c_regs",
        "//hw/top_earlgrey/sw/autogen:top_earlgrey",
        "//sw/device/lib/base:memory",
        "//sw/device/lib/base:mmio",
        "//sw/device/lib/dif:alert_handler",
        "//sw/device/lib/dif:aon_timer",
        "//sw/device/lib/dif:clkmgr",
        "//sw/device/lib/dif:entropy_src",
        "//sw/device/lib/dif:pwrmgr",
        "//sw/device/lib/dif:rstmgr",
        "//sw/device/lib/dif:rv_plic",
        "//sw/device/lib/dif:sensor_ctrl",
        "//sw/device/lib/runtime:irq",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing:alert_handler_testutils",
        "//sw/device/lib/testing:aon_timer_testutils",
        "//sw/device/lib/testing:clkmgr_testutils",
        "//sw/device/lib/testing:entropy_testutils",
        "//sw/device/lib/testing:isr_testutils",
        "//sw/device/lib/testing:pwrmgr_testutils",
        "//sw/device/lib/testing:rstmgr_testutils",
        "//sw/device/lib/testing:rv_plic_testutils",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)
