load("@rules_rust//rust:defs.bzl", "rust_stdlib_filegroup")
load("@rules_rust//rust:toolchain.bzl", "rust_toolchain")
load("//third_party/rust/toolchain:variant.bzl", "with_rust_toolchain")
load("//third_party/rust/toolchain:sysroot.bzl", "toolchain_sysroot")
load("//third_party/rust:deps.bzl", "OPENTITAN_RUST_TOOLS", "OPENTITAN_TRIPLE")
load("//third_party/rust/toolchain:flags.bzl", "RUSTC_FLAGS")
load("//rules/opentitan:defs.bzl", "OPENTITAN_CPU")
load("@bazel_skylib//rules:common_settings.bzl", "string_flag")

string_flag(
    name = "variant",
    build_setting_default = "stock",
    values = [
        "stock",
        "opentitan",
    ],
    visibility = ["//visibility:public"],
)

config_setting(
    name = "stock",
    flag_values = {
        ":variant": "stock",
    },
)

config_setting(
    name = "opentitan",
    flag_values = {
        ":variant": "opentitan",
    },
)

# Bootstrap custom stdlib with stock toolchain
with_rust_toolchain(
    name = "stdlib_libraries",
    srcs = ["@ot_rust_stdlib//:stdlib_libraries"],
    channel = "nightly",
    variant = "stock",
)

toolchain_sysroot(
    name = "sysroot_with_stdlib",
    srcs = [":stdlib_libraries"],
    dirname = "sysroot_with_stdlib",
    target_triple = OPENTITAN_TRIPLE,
)

rust_stdlib_filegroup(
    name = "custom_stdlib_files",
    srcs = [":sysroot_with_stdlib"],
)

# Create new toolchain with bootstrapped custom stdlib.
rust_toolchain(
    name = "toolchain_" + OPENTITAN_TRIPLE + "_impl",
    binary_ext = "",
    default_edition = "2018",
    dylib_ext = ".so",
    exec_triple = "x86_64-unknown-linux-gnu",
    extra_rustc_flags = RUSTC_FLAGS,
    rust_doc = OPENTITAN_RUST_TOOLS + "//:rustdoc",
    rust_std = ":custom_stdlib_files",
    rustc = OPENTITAN_RUST_TOOLS + "//:rustc",
    staticlib_ext = ".a",
    stdlib_linkflags = [],
    target_triple = OPENTITAN_TRIPLE,
)

toolchain(
    name = "toolchain_" + OPENTITAN_TRIPLE,
    exec_compatible_with = [
        "@platforms//cpu:x86_64",
        "@platforms//os:linux",
    ],
    target_compatible_with = [OPENTITAN_CPU],
    target_settings = [":opentitan"],
    toolchain = ":toolchain_" + OPENTITAN_TRIPLE + "_impl",
    toolchain_type = "@rules_rust//rust:toolchain_type",
)

# Modified with extra rustc flags from
# @rust_linux_x86_64__x86_64-unknown-linux-gnu__nightly_tools//:rust_toolchain
rust_toolchain(
    name = "toolchain_x86_64-unknown-linux-gnu_impl",
    allocator_library = "@rules_rust//ffi/cc/allocator_library:allocator_library",
    binary_ext = "",
    cargo = "@rust_linux_x86_64__x86_64-unknown-linux-gnu__nightly_tools//:cargo",
    cargo_clippy = "@rust_linux_x86_64__x86_64-unknown-linux-gnu__nightly_tools//:cargo_clippy_bin",
    clippy_driver = "@rust_linux_x86_64__x86_64-unknown-linux-gnu__nightly_tools//:clippy_driver_bin",
    default_edition = "2021",
    dylib_ext = ".so",
    exec_triple = "x86_64-unknown-linux-gnu",
    extra_exec_rustc_flags = [],
    extra_rustc_flags = RUSTC_FLAGS,
    global_allocator_library = "@rules_rust//ffi/cc/global_allocator_library:global_allocator_library",
    llvm_cov = "@rust_linux_x86_64__x86_64-unknown-linux-gnu__nightly_tools//:llvm_cov_bin",
    llvm_profdata = "@rust_linux_x86_64__x86_64-unknown-linux-gnu__nightly_tools//:llvm_profdata_bin",
    rust_doc = "@rust_linux_x86_64__x86_64-unknown-linux-gnu__nightly_tools//:rustdoc",
    rust_std = "@rust_linux_x86_64__x86_64-unknown-linux-gnu__nightly_tools//:rust_std-x86_64-unknown-linux-gnu",
    rustc = "@rust_linux_x86_64__x86_64-unknown-linux-gnu__nightly_tools//:rustc",
    rustc_lib = "@rust_linux_x86_64__x86_64-unknown-linux-gnu__nightly_tools//:rustc_lib",
    rustfmt = "@rust_linux_x86_64__x86_64-unknown-linux-gnu__nightly_tools//:rustfmt_bin",
    staticlib_ext = ".a",
    stdlib_linkflags = [
        "-ldl",
        "-lpthread",
    ],
    target_triple = "x86_64-unknown-linux-gnu",
    visibility = ["//visibility:public"],
)

# Modified from
# @rust_toolchains//:rust_linux_x86_64__x86_64-unknown-linux-gnu__nightly
toolchain(
    name = "toolchain_x86_64-unknown-linux-gnu",
    exec_compatible_with = [
        "@platforms//cpu:x86_64",
        "@platforms//os:linux",
    ],
    target_compatible_with = [
        "@platforms//cpu:x86_64",
        "@platforms//os:linux",
    ],
    target_settings = [":opentitan"],
    toolchain = ":toolchain_x86_64-unknown-linux-gnu_impl",
    toolchain_type = "@rules_rust//rust:toolchain",
    visibility = ["//visibility:public"],
)
