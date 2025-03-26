# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load("//rules/opentitan:providers.bzl",
  "get_one_binary_file",
  "SiliconBinaryInfo",
)

load("@rules_pkg//pkg:tar.bzl", "pkg_tar")

load(
    "//rules:otp.bzl",
    "otp_hex",
    "otp_json_immutable_rom_ext",
    "otp_partition",
)

load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cc_toolchain")
load("@lowrisc_opentitan//rules:rv.bzl", "rv_rule")

def _bin_to_imm_section_object_impl(ctx):
    cc_toolchain = find_cc_toolchain(ctx)

    for src in ctx.files.src:
        if src.extension != "bin":
            continue
        object = ctx.actions.declare_file(
            "{}.{}".format(
                src.basename.replace("." + src.extension, ""),
                "o",
            ),
        )
        ctx.actions.run(
            outputs = [object],
            inputs = [src] + cc_toolchain.all_files.to_list(),
            arguments = [
                "-I",
                "binary",
                "-O",
                "elf32-littleriscv",
                "--rename-section",
                ".data=.rom_ext_immutable,alloc,load,readonly,data,contents",
                src.path,
                object.path,
            ],
            executable = cc_toolchain.objcopy_executable,
        )

        # e2e/exec_env tests ensure the immutable rom_ext is the same across all
        # exec env, so simply return the first one.
        outputs = [object]
        return [
            DefaultInfo(
                files = depset(outputs),
                runfiles = ctx.runfiles(files = outputs),
            ),
        ]

bin_to_imm_section_object = rv_rule(
    implementation = _bin_to_imm_section_object_impl,
    attrs = {
        "src": attr.label(allow_files = True),
        "_cc_toolchain": attr.label(default = Label("@bazel_tools//tools/cpp:current_cc_toolchain")),
    },
    toolchains = ["@rules_cc//cc:toolchain_type"],
)

def create_imm_section_targets(name, src):
    object_target_name = name + "_object"
    bin_to_imm_section_object(
        name = object_target_name,
        src = src,
    )
    native.cc_import(
        name = name,
        objects = [object_target_name],
        data = [object_target_name],
        alwayslink = 1,
    )

_RELEASE_BUILD_HEADER = """
# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load(
    "//sw/device/silicon_creator/rom_ext/imm_section:utils.bzl",
    "create_imm_section_targets",
)

package(default_visibility = ["//visibility:public"])
"""

_RELEASE_BUILD_TARGET = """
create_imm_section_targets(
    name = "{name}",
    src = "{filename}",
)
"""

def _filter_release_files(ctx):
    build_contents = [_RELEASE_BUILD_HEADER]
    build_file = ctx.actions.declare_file("BUILD")

    files = [build_file]
    for src in ctx.attr.srcs:
      bin = get_one_binary_file(src, field="binary", providers=[SiliconBinaryInfo])
      files.extend([
        bin,
        get_one_binary_file(src, field="elf", providers=[SiliconBinaryInfo]),
        get_one_binary_file(src, field="mapfile", providers=[SiliconBinaryInfo]),
      ])
      bin_path = bin.basename
      bin_name = bin_path.rsplit('.', 1)[0]
      build_contents.append(_RELEASE_BUILD_TARGET.format(name=bin_name, filename=bin.basename))

    ctx.actions.write(build_file, ''.join(build_contents).strip())

    return [
        DefaultInfo(files = depset(files)),
    ]

filter_release_files = rule(
    implementation = _filter_release_files,
    attrs = {
        "srcs": attr.label_list(doc = "Target of opentitan_binary to filter"),
    },
)

def imm_section_bundle(name, srcs, **kwargs):
    files = "{}_files".format(name)

    filter_release_files(
        name = files,
        srcs = srcs,
        visibility = ["//visibility:private"],
        testonly=True,
        **kwargs,
    )

    pkg_tar(
        name = name,
        srcs = [files],
        mode = "0644",
        testonly=True,
        **kwargs,
    )
