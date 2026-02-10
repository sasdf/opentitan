# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load(
    "//rules/opentitan:providers.bzl",
    "SiliconBinaryInfo",
    "get_binary_files",
)

_TEST_SCRIPT = """\
{generate_coverage_view} \
  --elf="{elf_file}" \
  --kind="{kind}" \
  --output-dir="$TEST_UNDECLARED_OUTPUTS_DIR" \
"""

def _coverage_view_test(ctx):
    elf_label = ctx.attr.elf
    groups = elf_label.output_groups

    if "silicon_creator_elf" in groups:
        elf_list = groups["silicon_creator_elf"].to_list()
    elif "elf" in ctx.attr.elf.output_groups:
        elf_list = groups["elf"].to_list()
    else:
        elf_list = get_binary_files(elf_label, field = "elf", providers = [SiliconBinaryInfo])

    if len(elf_list) != 1:
        fail("The target must have exactly one elf file.")
    elf = elf_list[0]

    script = ctx.actions.declare_file(ctx.label.name + ".sh")
    ctx.actions.write(
        output = script,
        content = _TEST_SCRIPT.format(
            generate_coverage_view = ctx.executable._generate_coverage_view.short_path,
            elf_file = elf.short_path,
            kind = ctx.attr.kind,
        ),
        is_executable = True,
    )

    runfiles = ctx.runfiles(files = [elf])
    runfiles = runfiles.merge(ctx.attr._generate_coverage_view[DefaultInfo].default_runfiles)

    return DefaultInfo(
        executable = script,
        runfiles = runfiles,
    )

coverage_view_test = rule(
    implementation = _coverage_view_test,
    attrs = {
        "elf": attr.label(
            allow_files = True,
            doc = "ELF file to extract coverage view",
        ),
        "kind": attr.string(
            doc = "Kind of given elf file",
            default = "ibex",
            values = ["ibex", "otbn"],
        ),
        "_generate_coverage_view": attr.label(
            default = "//util/coverage/collect_cc_coverage:generate_coverage_view",
            executable = True,
            cfg = "exec",
        ),
    },
    fragments = ["cpp"],
    test = True,
)
