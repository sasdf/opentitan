load("@rules_rust//rust:defs.bzl", "rust_library")
load("@lowrisc_opentitan//third_party/rust/toolchain:flags.bzl", "RUSTC_FLAGS")

package(default_visibility = ["//visibility:public"])

exports_files(["bin/*"])

filegroup(
    name = "binaries",
    srcs = glob([
        "bin/*",
        "lib/*",
        "lib64/*",
    ]),
)

filegroup(
    name = "stdlib_libraries",
    srcs = [
        ":compiler_builtins",
        ":core",
    ],
)

rust_library(
    name = "compiler_builtins",
    srcs = glob([
        "vendor/compiler_builtins/src/**/*.rs",
        "vendor/compiler_builtins/libm/**/*.rs",
    ]),
    compile_data = glob(["vendor/compiler_builtins/src/**/*.md"]),
    crate_features = [
        "mem-unaligned",
        "compiler-builtins",
        "core",
        # "default",
        "unstable",
        "no_std",
        "no-asm",
    ],
    crate_name = "compiler_builtins",
    edition = "2015",
    rustc_env = {
        "RUSTC_BOOTSTRAP": "1",
    },
    rustc_flags = RUSTC_FLAGS,
    deps = [":core"],
)

rust_library(
    name = "core",
    srcs = glob([
        "library/core/src/**/*.rs",
        "library/stdarch/crates/core_arch/src/**/*.rs",
        "library/portable-simd/crates/core_simd/src/**/*.rs",
    ]),
    compile_data = glob([
        "library/core/src/**/*.md",
        "library/core/primitive_docs/*.md",
        "library/stdarch/crates/core_arch/src/**/*.md",
        "library/portable-simd/crates/core_simd/src/**/*.md",
    ]),
    crate_features = [
        "no_std",
    ],
    crate_name = "core",
    edition = "2021",
    rustc_env = {
        "RUSTC_BOOTSTRAP": "1",
    },
    rustc_flags = RUSTC_FLAGS + [
        "--cfg=bootstrap",
        "--cap-lints=allow",
        "-Zmacro-backtrace",
    ],
)
