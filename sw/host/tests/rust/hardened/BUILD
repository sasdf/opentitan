load("@rules_rust//rust:defs.bzl", "rust_binary")

rust_binary(
    name = "launder_test",
    srcs = ["launder_test.rs"],
    rustc_flags = ["-Copt-level=s"],
    deps = [
        "//sw/device/lib/base:hardened",
    ],
)
