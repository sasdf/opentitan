package(default_visibility = ["//visibility:public"])

load("//rules/opentitan:bindgen.bzl", "cc_library_with_bindgen", "ot_rust_library")

ot_rust_library(
    name = "hardened",
    srcs = ["hardened.rs"],
    deps = [
        "//sw/device/silicon_creator/imm_rom_ext/rust:ot_bindgen",
    ],
)

ot_rust_library(
    name = "error",
    srcs = ["error.rs"],
    deps = [
        ":hardened",
        "//sw/device/silicon_creator/imm_rom_ext/rust:ot_bindgen",
    ],
)
