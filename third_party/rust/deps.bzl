# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load("@rules_rust//bindgen:repositories.bzl", "rust_bindgen_dependencies")
load("@rules_rust//rust:repositories.bzl", "rules_rust_dependencies", "rust_register_toolchains")
load("@rules_rust//tools/rust_analyzer:deps.bzl", "rust_analyzer_dependencies")
load("//third_party/rust/toolchain:stdlib.bzl", "rust_stdlib_repo")
load("//third_party/rust/toolchain:flags.bzl", "RUSTC_FLAGS")

# The dates need to be sequential so that the stdlibs are built
# using the previous beta rustc
OPENTITAN_TRIPLE = "riscv32imc-unknown-none-elf"
OPENTITAN_RUST_TOOLS = "@rust_linux_x86_64__" + OPENTITAN_TRIPLE + "__nightly_tools"

def rust_deps():
    rules_rust_dependencies()

    # Register the opentitan toolchain variant.
    # This variant is aware of bazel enviroments, corrsponding rustc flags
    # will be populated automatically.
    # These targets need to be registered before rust_register_toolchains to
    # have higher priority when the variant constraint is satisfied.
    for env in [OPENTITAN_TRIPLE, "x86_64-unknown-linux-gnu"]:
        native.register_toolchains(
            "//third_party/rust/toolchain:toolchain_" + env,
        )

    # This rule creates @ot_rust_stdlib repo with rust sources.
    rust_stdlib_repo(
        name = "ot_rust_stdlib",
        channel = "nightly",
        date = "2023-07-30",
        build_template = "//third_party/rust/toolchain:stdlib.BUILD",
    )

    # Register default stock toolchains.
    rust_register_toolchains(
        # TODO(#15300): set this to `True` to support rust-analyzer, after fixing
        # upstream `rules_rust`.
        #include_rustc_srcs = False,
        edition = "2021",
        versions = ["1.71.1", "nightly/2023-07-30"],
        rustfmt_version = "nightly/2024-07-25",
        extra_target_triples = [OPENTITAN_TRIPLE],
        sha256s = {
            "2023-07-30/rustc-nightly-x86_64-unknown-linux-gnu.tar.xz": "ecdee8821a57efbb699b7e3aa4cbfbd60b7970bce89a8cfb9bc7d65b9058ee42",
            "2023-07-30/clippy-nightly-x86_64-unknown-linux-gnu.tar.xz": "76ee5aac81d1348bfebd3d94d5fb65c3f4ea0cf5fc2de834926f93772547380c",
            "2023-07-30/cargo-nightly-x86_64-unknown-linux-gnu.tar.xz": "4ddb3ed2dd2acedf9097f4a1fe17b8cd571fdd7c9a49b1e31c228a284ec95049",
            "2023-07-30/llvm-tools-nightly-x86_64-unknown-linux-gnu.tar.xz": "dc71b9ae6a4a4b9fa259724b29f4ad19467197ced89a8aad675f5af112c4fb77",
            "2023-07-30/rust-std-nightly-riscv32imc-unknown-none-elf.tar.xz": "9790d50d4510443bbf4c13b68227a273345d28b84d29372bc5f5ea2d14d05f2d",
            "2023-07-30/rust-std-nightly-x86_64-unknown-linux-gnu.tar.xz": "b5a589a243923c5fa2a1f08e7b902bb0a64ae08010067b9074501a6e1fb8b042",
        },
    )

    rust_bindgen_dependencies()
    native.register_toolchains(
        "@lowrisc_opentitan//third_party/rust:bindgen_toolchain",
    )

    rust_analyzer_dependencies()
