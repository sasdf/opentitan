package(default_visibility = ["//visibility:public"])

load("//rules/opentitan:bindgen.bzl", "cc_library_with_bindgen", "ot_rust_library")

cc_library_with_bindgen(
    name = "ot",
    bindgen = {
        "bindgen_flags": [
            # "--help",
        ],
        "rustc_flags": [
        ],
    },
    hdrs = ["bindgen.h"],
    deps = [
        "//sw/device/silicon_creator/imm_rom_ext:imm_rom_ext_epmp",
        "//hw/top_earlgrey/ip_autogen/flash_ctrl:flash_ctrl_c_regs",
        "//sw/device/lib/arch:device",
        "//sw/device/lib/base:macros",
        "//sw/device/silicon_creator/lib:dbg_print",
        "//sw/device/silicon_creator/lib:epmp_state",
        "//sw/device/silicon_creator/lib:error",
        "//sw/device/silicon_creator/lib:manifest",
        "//sw/device/silicon_creator/lib:shutdown",
        "//sw/device/silicon_creator/lib/base:boot_measurements",
        "//sw/device/silicon_creator/lib/base:sec_mmio",
        "//sw/device/silicon_creator/lib/cert:dice_chain",
        "//sw/device/silicon_creator/lib/drivers:pinmux",
        "//sw/device/silicon_creator/lib/drivers:rnd",
        "//sw/device/silicon_creator/lib/drivers:uart",
        "//sw/device/silicon_creator/rom_ext:rom_ext_manifest",
    ],
)

ot_rust_library(
    name = "panic",
    srcs = ["panic.rs"],
    deps = [
        "//sw/device/silicon_creator/imm_rom_ext/rust/base:hardened",
    ],
)

ot_rust_library(
    name = "static_assert",
    srcs = ["static_assert.rs"],
)

ot_rust_library(
    name = "dbg_print",
    srcs = ["dbg_print.rs"],
    deps = [
        ":ot_bindgen",
    ],
)
