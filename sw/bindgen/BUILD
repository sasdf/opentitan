package(default_visibility = ["//visibility:public"])

load("//rules/opentitan:rust.bzl", "cc_library_with_bindgen")
load("@rules_rust//rust:defs.bzl", "rust_proc_macro")

cc_library(
    name = "global_headers",
    hdrs = ["bindgen.h"],
)

cc_library_with_bindgen(
    name = "global_bindgen",
    bindgen = {
        "transitive": True,
        "blocklist": [
            "pub const kBootDataValidEntry",
        ],
    },
    global_deps = False,
    deps = [
        ":global_headers",
        "//sw/device/silicon_creator/lib:chip_info",
    ],
)

rust_proc_macro(
    name = "bindgen_proc_macro",
    srcs = ["bindgen_proc_macro.rs"],
    crate_name = "ot_bindgen",
    deps = [
        "@crate_index//:proc-macro2",
        "@crate_index//:quote",
        "@crate_index//:syn",
    ],
)
