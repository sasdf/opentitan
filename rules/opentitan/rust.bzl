# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

"""Helper for creating bindings for on-device rust lib."""

load("@rules_rust//rust:defs.bzl", "rust_library")
load("@rules_rust//bindgen:defs.bzl", "rust_bindgen_library")
load(
    "//rules:cross_platform.bzl",
    "dual_cc_device_library_of",
    "dual_cc_host_library_of",
    "dual_cc_library",
    "dual_inputs",
    "merge_and_split_inputs",
)
load("@bazel_skylib//lib:dicts.bzl", "dicts")
load("@rules_cc//cc:defs.bzl", "CcInfo", "cc_library")
load(
    "@rules_rust//rust:rust_common.bzl",
    "BuildInfo",
    "ClippyInfo",
    "CrateInfo",
    "DepInfo",
    "DepVariantInfo",
    "TestCrateInfo",
)
load("@rules_rust//bindgen:defs.bzl", "rust_bindgen")
load("//rules:rv.bzl", "OPENTITAN_PLATFORM", "opentitan_transition", "rv_rule")

GLOBAL_DEPS = [
    "//sw/bindgen:global_headers",
]

GLOBAL_RUST_DEPS = [
    "//sw/bindgen:global_bindgen",
]

GLOBAL_PROC_MACRO_DEPS = [
    "//sw/bindgen:bindgen_proc_macro",
]

BINDGEN_RUSTC_FLAGS = [
    "--allow=unused-imports",
    "--allow=non_snake_case",
    "--allow=non_camel_case_types",
    "--allow=nonstandard_style",
    "--allow=non_upper_case_globals",
]

BINDGEN_FLAGS = [
    "--raw-line=#![no_std]",
    "--use-core",
    "--no-prepend-enum-name",
    "--no-layout-tests",
    "--no-doc-comments",
]

BINDGEN_CLANG_FLAGS = [
    "-Wno-c99-extensions",
]

BLOCKLISTED_HEADERS = {
    "sw/device/lib/ujson/ujson_rust.h": None,
}

GUARDED_HEADERS = {
    "sw/device/lib/base/internal/absl_status.h": "USING_ABSL_STATUS",
    "sw/device/lib/base/internal/status.h": "USING_INTERNAL_STATUS",
}

def add_defaults(arr, defaults):
    exists = {e: None for e in arr}
    defaults = [e for e in defaults if e not in exists]
    return defaults + arr

def add_suffix(name, suffix):
    if "~" not in name:
        return name + "~" + suffix
    return name + "_" + suffix

PROVIDERS = [
    CcInfo,
    CcToolchainConfigInfo,
    DebugPackageInfo,
    DefaultInfo,
    InstrumentedFilesInfo,
    OutputGroupInfo,

    # @rules_rust//rust:rust_common.bzl
    BuildInfo,
    ClippyInfo,
    CrateInfo,
    DepInfo,
    DepVariantInfo,
    TestCrateInfo,
]

def _switch_target_impl(ctx):
    inp = ctx.attr.inp[0]
    return [inp[p] for p in PROVIDERS if p in inp]

rv_target = rv_rule(
    doc = "Transition the target to riscv32 build",
    implementation = _switch_target_impl,
    fragments = ["cpp"],
    attrs = {
        "inp": attr.label(
            doc = "The target to switch",
            cfg = opentitan_transition,
            mandatory = True,
        ),
    },
)

def _copy_compilation_context_impl(ctx):
    rust_lib, cc_lib = ctx.attr.rust_lib, ctx.attr.cc_lib
    info = CcInfo(
        compilation_context = cc_lib[CcInfo].compilation_context,
        linking_context = rust_lib[CcInfo].linking_context,
    )
    return [info] + [rust_lib[p] for p in PROVIDERS if p != CcInfo and p in rust_lib]

_copy_compilation_context = rule(
    doc = "Overwrite CcInfo compilation contexts in rust_lib to the one in cc_lib",
    implementation = _copy_compilation_context_impl,
    fragments = ["cpp"],
    attrs = {
        "rust_lib": attr.label(
            doc = "The target to be overwritten",
            mandatory = True,
        ),
        "cc_lib": attr.label(
            doc = "The target to get the compilation_context",
            mandatory = True,
        ),
    },
)

charset = [
    "!",
    "%",
    "-",
    "@",
    "^",
    '"',
    "#",
    "$",
    "&",
    "'",
    "(",
    ")",
    "*",
    "-",
    "+",
    ",",
    ";",
    "<",
    "=",
    ">",
    "?",
    "[",
    "]",
    "{",
    "|",
    "}",
    "~",
    "/",
    ".",
]

def _compute_crate_name(name, crate_name = None, mod_name = None, **kwargs):
    if mod_name == None:
        mod_name = name
    if crate_name == None:
        crate_name = "ot_" + native.package_name() + "_" + mod_name
        for c in charset:
            crate_name = crate_name.replace(c, "_")
    return crate_name

def _rust_library(name, crate_name = None, mod_name = None, **kwargs):
    crate_name = _compute_crate_name(name, crate_name = crate_name, mod_name = mod_name)

    rust_library(
        name = name,
        crate_name = crate_name,
        **kwargs
    )

def _rust_bindgen(
        name,
        bindgen_flags = [],
        rustc_flags = [],
        clang_flags = [],
        testonly = False,
        **kwargs):
    rust_bindgen(
        name = name,
        bindgen_flags = add_defaults(bindgen_flags, BINDGEN_FLAGS),
        clang_flags = add_defaults(clang_flags, BINDGEN_CLANG_FLAGS),
        testonly = testonly,
        **kwargs
    )

CC_SUFFIX = [".c", ".cc", ".h", ".hpp", ".cpp", ".inc"]

def _is_c_source(name):
    return any([name.lower().endswith(suffix) for suffix in CC_SUFFIX])

def _split_source(srcs, cc_srcs = [], rs_srcs = []):
    cc_srcs = cc_srcs + [name for name in srcs if _is_c_source(name)]
    rs_srcs = rs_srcs + [name for name in srcs if not _is_c_source(name)]
    return cc_srcs, rs_srcs

def ot_rust_library(
        name,
        hdrs = [],
        srcs = [],
        deps = [],
        compile_data = [],
        rustc_env = {},
        proc_macro_deps = [],
        testonly = False,
        bindgen = {},
        global_deps = True,
        cc_srcs = [],
        rs_srcs = [],
        **kwargs):
    cc_srcs, rs_srcs = _split_source(srcs, cc_srcs, rs_srcs)

    if len(hdrs) == 0 and len(cc_srcs) == 0:
        _rust_library(
            name = name,
            srcs = rs_srcs,
            deps = deps,
            testonly = testonly,
            **kwargs
        )
        return

    if global_deps:
        deps = add_defaults(deps, GLOBAL_DEPS)
        deps = add_defaults(deps, GLOBAL_RUST_DEPS)

    cc_lib = add_suffix(name, "cc")
    rust_lib = add_suffix(name, "rust")
    bindgen_rs = add_suffix(cc_lib, "rs")
    mod_name = add_suffix(cc_lib, "do_not_use_directly")
    cc_library_with_bindgen(
        name = cc_lib,
        hdrs = hdrs,
        srcs = cc_srcs,
        deps = deps,
        bindgen = dicts.add(bindgen, {"mod_name": mod_name}),
        testonly = testonly,
    )
    deps = deps + [cc_lib]

    _rust_library(
        name = rust_lib,
        mod_name = name,
        srcs = rs_srcs,
        deps = deps,
        proc_macro_deps = add_defaults(proc_macro_deps, GLOBAL_PROC_MACRO_DEPS),
        compile_data = [bindgen_rs] + compile_data,
        rustc_env = dicts.add(rustc_env, {
            "OT_BINDGEN_RS_PATH": "$(location {})".format(bindgen_rs),
        }),
        testonly = testonly,
        **kwargs
    )

    _copy_compilation_context(
        name = name,
        rust_lib = rust_lib,
        cc_lib = cc_lib,
        testonly = testonly,
        tags = ["no-clippy"],
    )

def _get_crate_import_impl(ctx):
    imports = []

    exports = {}
    for dep in ctx.attr.exports:
        if CrateInfo not in dep:
            fail("No CrateInfo in exports target " + str(dep))
        if dep not in ctx.attr.deps:
            fail("Exports target " + str(dep) + " is not a dependency")
        imports.append("pub use " + dep[CrateInfo].name + "::*;\n")
        exports[dep[CrateInfo].name] = True

    for dep in ctx.attr.deps:
        if CrateInfo in dep:
            if dep[CrateInfo].name not in exports:
                imports.append("pub(crate) use " + dep[CrateInfo].name + "::*;\n")

    out = ctx.actions.declare_file(ctx.label.name + ".rs")

    ctx.actions.write(
        output = out,
        content = "".join(imports),
    )

    return [DefaultInfo(files = depset([out]))]

get_crate_import = rule(
    doc = "Generates a rust source file from a cc_library and a header.",
    implementation = _get_crate_import_impl,
    attrs = {
        "deps": attr.label_list(),
        "exports": attr.label_list(),
    },
    outputs = {"out": "%{name}.rs"},
)

def ot_bindgen(
        name,
        cc_lib,
        deps = [],
        rust_deps = [],
        testonly = False,
        blocklist = [],
        bindgen_flags = [],
        exports = [],
        transitive = False):
    import_target = add_suffix(name, "import")

    get_crate_import(
        name = import_target,
        deps = add_defaults(deps, rust_deps),
        exports = exports,
        testonly = testonly,
    )

    ot_bindgen_rs(
        name = name,
        cc_lib = cc_lib,
        crate_import = import_target,
        bindgen_flags = bindgen_flags,
        blocklist = blocklist,
        transitive = transitive,
        testonly = testonly,
    )

def ot_bindgen_library(
        name,
        cc_lib,
        deps = [],
        rust_deps = [],
        crate_name = None,
        mod_name = None,
        testonly = False,
        blocklist = [],
        bindgen_flags = [],
        exports = [],
        rustc_flags = [],
        transitive = False,
        bindgen_lib = None):
    rs_target = add_suffix(name, "rs")
    ot_bindgen(
        name = rs_target,
        cc_lib = bindgen_lib or cc_lib,
        deps = deps,
        rust_deps = rust_deps,
        testonly = testonly,
        blocklist = blocklist,
        bindgen_flags = bindgen_flags,
        exports = exports,
        transitive = transitive,
    )

    rust_lib = add_suffix(name, "rlib")
    _rust_library(
        name = rust_lib,
        crate_name = crate_name,
        mod_name = mod_name or name,
        srcs = [rs_target],
        rustc_flags = BINDGEN_RUSTC_FLAGS + rustc_flags,
        tags = ["no-clippy"],
        deps = [cc_lib] + add_defaults(deps, rust_deps),
        testonly = testonly,
    )

    _copy_compilation_context(
        name = name,
        rust_lib = rust_lib,
        cc_lib = cc_lib,
        testonly = testonly,
        tags = ["no-clippy"],
    )

def cc_library_with_bindgen(
        name,
        hdrs = [],
        deps = [],
        rust_deps = [],
        global_deps = True,
        bindgen = {},
        testonly = False,
        **kwargs):
    if global_deps:
        deps = add_defaults(deps, GLOBAL_DEPS)
        rust_deps = add_defaults(rust_deps, GLOBAL_RUST_DEPS)

    cc_lib = add_suffix(name, "cc")
    native.cc_library(name = cc_lib, hdrs = hdrs, deps = deps, testonly = testonly, **kwargs)

    ot_bindgen_library(
        name = name,
        cc_lib = cc_lib,
        deps = deps,
        rust_deps = rust_deps,
        testonly = testonly,
        **bindgen
    )

def dual_cc_library_with_bindgen(
        name,
        on_device_config_setting = "//rules:opentitan_platform",
        srcs = [],
        hdrs = [],
        copts = [],
        deps = [],
        visibility = ["//visibility:private"],
        target_compatible_with = [],
        rust_deps = [],
        bindgen = {},
        automock_name = None,
        global_deps = True,
        crate_name = None,
        testonly = False,
        **kwargs):
    deps_d, deps_h = merge_and_split_inputs(deps)
    tgts_d, tgts_h = merge_and_split_inputs(target_compatible_with)

    if global_deps:
        deps_d = add_defaults(deps_d, GLOBAL_DEPS)
        deps_h = add_defaults(deps_h, GLOBAL_DEPS)
        rust_deps = add_defaults(rust_deps, GLOBAL_RUST_DEPS)

    cc_lib = add_suffix(name, "cc")
    dual_cc_library(
        name = cc_lib,
        on_device_config_setting = on_device_config_setting,
        srcs = srcs,
        hdrs = hdrs,
        copts = copts,
        deps = deps,
        target_compatible_with = target_compatible_with,
        visibility = visibility,
        testonly = testonly,
        **kwargs
    )

    cc_lib_device = dual_cc_device_library_of(cc_lib)
    rust_lib_device = dual_cc_device_library_of(name)

    cc_lib_host = dual_cc_host_library_of(cc_lib)
    rust_lib_host = dual_cc_host_library_of(name)

    bindgen = dict(bindgen)
    mod_name = bindgen.pop("mod_name", name)

    ot_bindgen_library(
        name = rust_lib_device,
        mod_name = mod_name,
        bindgen_lib = cc_lib,
        cc_lib = cc_lib_device,
        deps = deps_d,
        rust_deps = rust_deps,
        testonly = testonly,
        **bindgen
    )

    ot_bindgen_library(
        name = rust_lib_host,
        mod_name = mod_name,
        bindgen_lib = cc_lib,
        cc_lib = cc_lib_host,
        deps = deps_h,
        rust_deps = rust_deps,
        testonly = testonly,
        **bindgen
    )

    native.alias(
        name = name,
        actual = select({
            on_device_config_setting: rust_lib_device,
            "//conditions:default": rust_lib_host,
        }),
        testonly = testonly,
    )

    # Generate automock if requested.
    automock_name = automock_name or name + "_automock"
    _rust_bindgen_automock(
        name = automock_name,
        rust_lib = rust_lib_host,
        deps = add_defaults(deps_h, rust_deps),
        testonly = testonly,
    )

def _format_header_include(header):
    if header in BLOCKLISTED_HEADERS:
        return "// BLOCKLISTED: {}\n".format(header)

    guard = GUARDED_HEADERS.get(header, None)
    if guard != None:
        return (
            "#define {guard}\n" +
            '#include "{header}"\n' +
            "#undef {guard}\n"
        ).format(guard = guard, header = header)

    return '#include "{}"\n'.format(header)

def _all_cc_headers_impl(ctx):
    lib = ctx.attr.cc_lib[CcInfo].compilation_context

    deps = []
    for hdr in lib.headers.to_list():
        deps.append(hdr.path)
    deps = sorted(deps)

    hdrs = {}
    for hdr in lib.direct_headers:
        hdrs[hdr.path] = None

    deps = [h for h in deps if h not in hdrs]

    if not ctx.attr.exclude_direct:
        deps.extend(sorted(hdrs.keys()))

    content = "\n".join(
        ["#define " + e + "\n" for e in lib.defines.to_list()] +
        [_format_header_include(e) for e in deps],
    )

    out = ctx.actions.declare_file(ctx.label.name + ".h")
    ctx.actions.write(output = out, content = content)

    return [DefaultInfo(files = depset([out]))]

all_cc_headers = rule(
    doc = "Generates an all-in-one header file from cc dependencies.",
    implementation = _all_cc_headers_impl,
    attrs = {
        "cc_lib": attr.label(
            doc = "The cc_library that contains the `.h` file. This is used to find the transitive includes.",
            providers = [CcInfo],
            mandatory = True,
        ),
        "exclude_direct": attr.bool(
            doc = "Excludes direct public headers.",
        ),
    },
    outputs = {"out": "%{name}.h"},
)

def _rust_bindgen_all(name, cc_lib, exclude_direct, testonly = False, **kwargs):
    header_target = add_suffix(name, "hdr")
    lib_target = add_suffix(name, "lib")
    rv_cc_lib = add_suffix(name, "rv")
    bindgen_target = name

    rv_target(
        name = rv_cc_lib,
        inp = cc_lib,
        tags = ["no-clippy"],
        testonly = testonly,
    )

    all_cc_headers(
        name = header_target,
        cc_lib = rv_cc_lib,
        exclude_direct = exclude_direct,
        testonly = testonly,
    )
    cc_library(
        name = lib_target,
        hdrs = [header_target],
        deps = [rv_cc_lib],
        testonly = testonly,
    )
    _rust_bindgen(
        name = bindgen_target,
        cc_lib = lib_target,
        header = header_target,
        testonly = testonly,
        **kwargs
    )

def _rust_bindgen_diff(name, base, bindings, crate_import, testonly = False):
    native.genrule(
        name = name,
        testonly = testonly,
        srcs = [crate_import, base, bindings],
        outs = [name + ".rs"],
        cmd = """
          set -euo pipefail
          {rust_bindgen_diff} {crate_import} {base_deps} {bindings} \
          | {rustfmt} \
          > $@
      """.format(
            rust_bindgen_diff = "$(locations //sw/bindgen:rust_bindgen_diff) ",
            rustfmt = "$(locations @rules_rust//rust/toolchain:current_rustfmt_files) ",
            base_deps = "$(locations " + base + ") ",
            bindings = "$(locations " + bindings + ") ",
            crate_import = "$(locations " + crate_import + ") ",
        ),
        tools = ["//sw/bindgen:rust_bindgen_diff"],
        toolchains = [
            "@rules_rust//rust/toolchain:current_rustfmt_files",
        ],
    )

def _get_blocklist_impl(ctx):
    out = ctx.actions.declare_file(ctx.label.name + ".json")
    ctx.actions.write(
        output = out,
        content = json.encode({
            "regex": ctx.attr.regex,
            "literal": ctx.attr.literal,
        }),
    )
    return [DefaultInfo(files = depset([out]))]

get_blocklist = rule(
    doc = "Generates a rust source file from a cc_library and a header.",
    implementation = _get_blocklist_impl,
    attrs = {
        "regex": attr.string_list(),
        "literal": attr.string_list(),
    },
    outputs = {"out": "%{name}.json"},
)

def _rust_bindgen_filter(name, bindings, testonly = False, **kwargs):
    blocklist = name + "_blocklist"
    get_blocklist(name = blocklist, **kwargs)

    native.genrule(
        name = name,
        testonly = testonly,
        srcs = [bindings, blocklist],
        outs = [name + ".rs"],
        cmd = """
          set -euo pipefail
          {rust_bindgen_filter} {bindings} {blocklist} \
          | {rustfmt} \
          > $@
      """.format(
            rust_bindgen_filter = "$(locations //sw/bindgen:rust_bindgen_filter) ",
            rustfmt = "$(locations @rules_rust//rust/toolchain:current_rustfmt_files) ",
            bindings = "$(locations " + bindings + ") ",
            blocklist = "$(locations " + blocklist + ") ",
        ),
        tools = ["//sw/bindgen:rust_bindgen_filter"],
        toolchains = [
            "@rules_rust//rust/toolchain:current_rustfmt_files",
        ],
    )

def _rust_bindgen_automock(name, rust_lib, deps = [], testonly = False, **kwargs):
    input_target = add_suffix(rust_lib, "rs")
    import_target = add_suffix(name, "import")
    mock_rs = add_suffix(name, "rs")

    get_crate_import(
        name = import_target,
        deps = add_defaults(deps, [rust_lib]),
        testonly = testonly,
    )

    native.genrule(
        name = mock_rs,
        testonly = testonly,
        srcs = [input_target, import_target],
        outs = [name + ".rs"],
        cmd = """
          set -euo pipefail
          {rust_bindgen_automock} {import_rs} {input_rs} \
          | {rustfmt} \
          > $@
      """.format(
            rust_bindgen_automock = "$(locations //sw/bindgen:rust_bindgen_automock) ",
            rustfmt = "$(locations @rules_rust//rust/toolchain:current_rustfmt_files) ",
            import_rs = "$(locations " + import_target + ") ",
            input_rs = "$(locations " + input_target + ") ",
        ),
        tools = ["//sw/bindgen:rust_bindgen_automock"],
        toolchains = [
            "@rules_rust//rust/toolchain:current_rustfmt_files",
        ],
    )

    _rust_library(
        name = name,
        srcs = [mock_rs],
        deps = add_defaults(deps, [
            rust_lib,
            "@crate_index//:mockall",
        ]),
        testonly = testonly,
        **kwargs
    )

def ot_bindgen_rs(
        name,
        cc_lib,
        crate_import,
        blocklist = [],
        blocklist_regex = [],
        transitive = False,
        testonly = False,
        **kwargs):
    if transitive:
        trans_target = add_suffix(name, "trans")
        _rust_bindgen_all(name = trans_target, cc_lib = cc_lib, exclude_direct = False, testonly = testonly, **kwargs)
        _rust_bindgen_filter(name, trans_target, literal = blocklist, regex = blocklist_regex, testonly = testonly)
        return

    base_target = add_suffix(name, "base")
    bind_target = add_suffix(name, "bind")
    diff_target = add_suffix(name, "diff")

    _rust_bindgen_all(name = base_target, cc_lib = cc_lib, exclude_direct = True, testonly = testonly, **kwargs)
    _rust_bindgen_all(name = bind_target, cc_lib = cc_lib, exclude_direct = False, testonly = testonly, **kwargs)
    _rust_bindgen_diff(diff_target, base_target, bind_target, crate_import, testonly = testonly)
    _rust_bindgen_filter(name, diff_target, literal = blocklist, regex = blocklist_regex, testonly = testonly)
