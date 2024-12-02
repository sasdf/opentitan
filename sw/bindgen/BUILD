package(default_visibility = ["//visibility:public"])

load("//rules/opentitan:rust.bzl", "cc_library_with_bindgen")
load("@rules_rust//rust:defs.bzl", "rust_proc_macro")
load("@rules_rust//rust:defs.bzl", "rust_binary")
load("//rules/opentitan:rust.bzl", "ot_rust_library")

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

ot_rust_library(
    name = "rust_bindgen_util",
    srcs = ["rust_bindgen_util.rs"],
    rustc_flags = ["-Copt-level=3"],
    deps = [
        "@crate_index//:quote",
        "@crate_index//:syn",
    ],
)

rust_binary(
    name = "rust_bindgen_diff",
    srcs = ["rust_bindgen_diff.rs"],
    rustc_flags = ["-Copt-level=3"],
    deps = [
        ":rust_bindgen_util",
        "@crate_index//:clap",
        "@crate_index//:quote",
        "@crate_index//:syn",
    ],
)

rust_binary(
    name = "rust_bindgen_filter",
    srcs = ["rust_bindgen_filter.rs"],
    rustc_flags = ["-Copt-level=3"],
    deps = [
        ":rust_bindgen_util",
        "@crate_index//:clap",
        "@crate_index//:quote",
        "@crate_index//:regex",
        "@crate_index//:serde",
        "@crate_index//:serde_json",
        "@crate_index//:syn",
    ],
)

rust_binary(
    name = "rust_bindgen_automock",
    srcs = ["rust_bindgen_automock.rs"],
    rustc_flags = ["-Copt-level=3"],
    deps = [
        ":rust_bindgen_util",
        "@crate_index//:clap",
        "@crate_index//:proc-macro2",
        "@crate_index//:quote",
        "@crate_index//:syn",
    ],
)

rust_proc_macro(
    name = "bindgen_proc_macro",
    srcs = ["bindgen_proc_macro.rs"],
    crate_name = "ot_bindgen",
    deps = [
        ":rust_bindgen_util",
        "@crate_index//:proc-macro2",
        "@crate_index//:quote",
        "@crate_index//:syn",
    ],
)
