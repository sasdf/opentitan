# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load(
    "@lowrisc_opentitan//rules:rv.bzl",
    "rv_rule",
)
load(
    "//rules/opentitan:providers.bzl",
    "SiliconBinaryInfo",
    "get_one_binary_file",
)
load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cc_toolchain")

_TEST_SCRIPT = """\
env
elf="$(realpath "{elf_file}")"
ln -s "$elf" "${{TEST_UNDECLARED_OUTPUTS_DIR}}/test.elf"
ls -lah "$elf"
echo "Linking"
echo "  $elf"
echo "To"
echo "  ${{TEST_UNDECLARED_OUTPUTS_DIR}}/test.elf"

dis="$(realpath "{dis_file}")"
ln -s "$dis" "${{TEST_UNDECLARED_OUTPUTS_DIR}}/test.dis"
ls -lah "$dis"
echo "Linking"
echo "  $dis"
echo "To"
echo "  ${{TEST_UNDECLARED_OUTPUTS_DIR}}/test.dis"
"""

def _coverage_view_test(ctx):
    # Get the elf to be tested
    elf_label = ctx.attr.elf
    elf = get_one_binary_file(elf_label, field = "elf", providers = [SiliconBinaryInfo])
    dis = get_one_binary_file(elf_label, field = "disassembly", providers = [SiliconBinaryInfo])

    # Nop test
    script = ctx.actions.declare_file(ctx.attr.name + ".bash")
    ctx.actions.write(
        script,
        _TEST_SCRIPT.format(
            elf_file = elf.short_path,
            dis_file = dis.short_path,
        ),
        is_executable = True,
    )

    # Propagate all runfiles from elf attr
    runfiles = ctx.runfiles(files = ctx.files.elf + [elf, dis])
    runfiles = runfiles.merge(ctx.attr.elf[DefaultInfo].default_runfiles)

    # FIXME: workaround due to missing tools_path in rules_cc toolchain.
    # See also https://github.com/bazelbuild/rules_cc/issues/351
    cc_toolchain = find_cc_toolchain(ctx)
    toolchain_runfiles = ctx.runfiles(files = cc_toolchain.all_files.to_list())
    runfiles = runfiles.merge(toolchain_runfiles)

    return DefaultInfo(
        executable = script,
        runfiles = runfiles,
    )

coverage_view_test = rv_rule(
    implementation = _coverage_view_test,
    attrs = {
        "elf": attr.label(
            allow_files = True,
            doc = "ELF file to extract coverage view",
        ),
        "_cc_toolchain": attr.label(default = Label("@bazel_tools//tools/cpp:current_cc_toolchain")),
        "_lcov_merger": attr.label(
            default = configuration_field(fragment = "coverage", name = "output_generator"),
            executable = True,
            cfg = "exec",
        ),
        "_collect_cc_coverage": attr.label(
            default = "//sw/device/coverage/collect_cc_coverage:generate_coverage_view",
            executable = True,
            cfg = "exec",
        ),
    },
    fragments = ["cpp"],
    toolchains = ["@rules_cc//cc:toolchain_type"],
    test = True,
)
