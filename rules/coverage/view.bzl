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

    if ctx.var.get("ot_coverage_enabled", "false") == "true":
        coverage_runfiles = ctx.attr._collect_cc_coverage[DefaultInfo].default_runfiles
    else:
        coverage_runfiles = ctx.runfiles()
    runfiles = runfiles.merge(coverage_runfiles)

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
        "_lcov_merger": attr.label(
            default = configuration_field(fragment = "coverage", name = "output_generator"),
            executable = True,
            cfg = "exec",
        ),
        "_collect_cc_coverage": attr.label(
            default = "//util/coverage_new/collect_cc_coverage:generate_coverage_view",
            executable = True,
            cfg = "exec",
        ),
    },
    fragments = ["cpp"],
    test = True,
)
