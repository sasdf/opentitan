# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load("//rules:cross_platform.bzl", "dual_cc_device_library_of", "dual_cc_library", "dual_inputs")
load(
    "//rules/opentitan:defs.bzl",
    "EARLGREY_TEST_ENVS",
    "cw310_params",
    "fpga_params",
    "opentitan_test",
    "verilator_params",
)
load("@bazel_skylib//lib:dicts.bzl", "dicts")
load("//rules:host.bzl", "cc_library_host_only")
load("//rules/opentitan:rust.bzl", "cc_library_with_bindgen", "dual_cc_library_with_bindgen", "ot_rust_library")
load("@rules_rust//rust:defs.bzl", "rust_test")

package(default_visibility = ["//visibility:public"])

dual_cc_library_with_bindgen(
    name = "crc32",
    srcs = dual_inputs(
        device = ["crc32.c"],
        host = ["mock_crc32.cc"],
    ),
    hdrs = dual_inputs(
        host = ["mock_crc32.h"],
        shared = ["crc32.h"],
    ),
    # This library is a dependancy of the ujson library.
    visibility = ["//sw/device/lib/ujson:__pkg__"],
    deps = dual_inputs(
        host = [
            ":global_mock",
            "@googletest//:gtest",
        ],
        shared = [
            ":memory",
            ":macros",
        ],
    ),
)

cc_test(
    name = "crc32_unittest",
    srcs = ["crc32_unittest.cc"],
    deps = [
        dual_cc_device_library_of(":crc32"),
        "@googletest//:gtest_main",
    ],
)

opentitan_test(
    name = "crc32_functest",
    srcs = ["crc32_functest.c"],
    exec_env = EARLGREY_TEST_ENVS,
    verilator = verilator_params(
        timeout = "long",
    ),
    deps = [
        ":crc32",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

opentitan_test(
    name = "crc32_perftest",
    srcs = ["crc32_perftest.c"],
    exec_env = dicts.add(
        EARLGREY_TEST_ENVS,
        {
            "//hw/top_earlgrey:fpga_cw310_test_rom": None,
        },
    ),
    fpga = fpga_params(
        tags = [
            "manual",
        ],
    ),
    deps = [
        ":crc32",
        ":macros",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
        "//sw/device/lib/testing/test_framework:ottf_test_config",
    ],
)

cc_library_host_only(
    name = "global_mock",
    hdrs = ["global_mock.h"],
)

cc_test(
    name = "global_mock_unittest",
    srcs = ["global_mock_unittest.cc"],
    # Enable UBSan by setting copts and linkopts. This is roughly what
    # `--config=ubsan` does, as defined in `.bazelrc`.
    copts = [
        "-fsanitize=undefined",
        "-g",
        "-fno-omit-frame-pointer",
        # Make detected UB fatal. Without this flag, the test would always pass.
        "-fno-sanitize-recover",
    ],
    linkopts = [
        "-fsanitize=undefined",
    ],
    deps = [
        ":global_mock",
        "@googletest//:gtest_main",
    ],
)

cc_library_with_bindgen(
    name = "stdasm",
    hdrs = ["stdasm.h"],
)

# Included in rust global deps due to circular deps
# Imported by //sw/device/silicon_creator/lib:chip_info
cc_library(
    name = "macros",
    hdrs = [
        "adv_macros.h",
        "macros.h",
    ],
)

cc_library_with_bindgen(
    name = "math_polyfills",
    srcs = ["math_builtins.c"],

    # This library defines polyfills, so we need to ensure LLVM doesn't make
    # the polyfills call themselves.
    copts = ["-fno-builtin"],
    visibility = ["//visibility:private"],
    deps = [":macros"],
)

alias(
    name = "math_builtins",
    actual = select({
        "//sw/device:measure_coverage_on_target": "//sw/device:nothing",
        "//conditions:default": ":math_polyfills",
    }),
    visibility = ["//visibility:private"],
)

cc_library_with_bindgen(
    name = "math",
    srcs = ["math.c"],
    hdrs = ["math.h"],
    deps = [
        ":macros",
        ":math_builtins",
    ],
)

cc_test(
    name = "math_unittest",
    srcs = ["math_unittest.cc"],
    deps = [
        ":math",
        "@googletest//:gtest_main",
    ],
)

cc_library_with_bindgen(
    name = "bitfield",
    srcs = ["bitfield.c"],
    hdrs = ["bitfield.h"],
    deps = [
        ":macros",
        ":math",
    ],
)

cc_library_with_bindgen(
    name = "memory",
    srcs = ["memory.c"],
    hdrs = ["memory.h"],

    # This library defines memcpy(), so we can't have LLVM rewriting memcpy
    # into a call to itself.
    copts = ["-fno-builtin"],
    deps = [
        ":macros",
    ],
)

opentitan_test(
    name = "memory_perftest",
    srcs = ["memory_perftest.c"],
    exec_env = EARLGREY_TEST_ENVS,
    deps = [
        ":macros",
        ":memory",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/runtime:log",
        "//sw/device/lib/testing/test_framework:check",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

cc_test(
    name = "memory_unittest",
    srcs = ["memory_unittest.cc"],
    deps = [
        ":memory",
        "@googletest//:gtest_main",
    ],
)

ot_rust_library(
    name = "hardened",
    srcs = [
        "hardened.c",
        "hardened.rs",
    ],
    hdrs = [
        "hardened.h",
        "hardened_asm.h",
    ],
    deps = [
        ":macros",
        ":memory",
        ":stdasm",
    ],
)

cc_test(
    name = "hardened_unittest",
    srcs = ["hardened_unittest.cc"],
    deps = [
        ":hardened",
        "@googletest//:gtest_main",
    ],
)

rust_test(
    name = "hardened_unittest_rust",
    srcs = ["hardened_unittest.rs"],
    deps = [":hardened"],
)

opentitan_test(
    name = "hardened_functest",
    srcs = ["hardened_functest.c"],
    exec_env = EARLGREY_TEST_ENVS,
    verilator = verilator_params(
        timeout = "long",
    ),
    deps = [
        ":hardened",
        "//sw/device/lib/runtime:ibex",
        "//sw/device/lib/testing/test_framework:ottf_main",
    ],
)

cc_library_with_bindgen(
    name = "random_order",
    srcs = ["random_order.c"],
    hdrs = ["random_order.h"],
    deps = [":bitfield"],
)

cc_library_with_bindgen(
    name = "hardened_memory",
    srcs = ["hardened_memory.c"],
    hdrs = ["hardened_memory.h"],
    deps = [
        ":hardened",
        ":macros",
        ":memory",
        ":random_order",
    ],
)

cc_test(
    name = "hardened_memory_unittest",
    srcs = ["hardened_memory_unittest.cc"],
    deps = [
        ":hardened_memory",
        ":random_order",
        "@googletest//:gtest_main",
    ],
)

dual_cc_library_with_bindgen(
    name = "csr",
    srcs = dual_inputs(
        host = ["//sw/device/silicon_creator/lib/base:mock_csr.cc"],
    ),
    hdrs = dual_inputs(
        host = [
            "//sw/device/silicon_creator/lib/base:mock_csr.h",
        ],
        shared = [
            "csr.h",
            "csr_registers.h",
        ],
    ),
    deps = dual_inputs(
        host = [
            "global_mock",
            "@googletest//:gtest",
        ],
        shared = [
            ":macros",
            ":stdasm",
        ],
    ),
)

cc_test(
    name = "mock_csr_unittest",
    srcs = ["//sw/device/silicon_creator/lib/base:mock_csr_test.cc"],
    deps = [
        ":csr",
        "@googletest//:gtest_main",
    ],
)

cc_library_with_bindgen(
    name = "multibits",
    hdrs = [
        "multibits.h",
        "multibits_asm.h",
    ],
)

dual_cc_library_with_bindgen(
    name = "mmio",
    srcs = dual_inputs(
        host = ["mock_mmio.cc"],
        # NOTE: mmio.c is shared because it provides mmio_memcpy and friends.
        shared = ["mmio.c"],
    ),
    hdrs = dual_inputs(
        host = [
            "mock_mmio.h",
            "mock_mmio_test_utils.h",
        ],
        shared = ["mmio.h"],
    ),
    deps = dual_inputs(
        host = [
            "@googletest//:gtest",
        ],
        shared = [
            ":bitfield",
            ":macros",
            ":memory",
        ],
    ),
)

cc_test(
    name = "mmio_unittest",
    srcs = ["mock_mmio_test.cc"],
    deps = [
        ":mmio",
        "@googletest//:gtest_main",
    ],
)

dual_cc_library_with_bindgen(
    name = "abs_mmio",
    srcs = dual_inputs(
        device = ["abs_mmio.c"],
        host = ["mock_abs_mmio.cc"],
    ),
    hdrs = dual_inputs(
        host = [
            "mock_abs_mmio.h",
        ],
        shared = ["abs_mmio.h"],
    ),
    deps = dual_inputs(
        host = [
            "global_mock",
            "@googletest//:gtest",
        ],
        shared = [
            ":mmio",
            ":macros",
        ],
    ),
)

cc_library_with_bindgen(
    name = "status",
    srcs = ["status.c"],
    hdrs = ["status.h"],
    deps = [
        ":bitfield",
        ":macros",
        "//sw/device/lib/base/internal:status",
        "//sw/device/lib/dif:base",
        "//sw/device/silicon_creator/lib:error",
    ],
)

cc_test(
    name = "status_unittest",
    srcs = ["status_unittest.cc"],
    deps = [
        ":status",
        "@googletest//:gtest_main",
    ],
)

cc_library_with_bindgen(
    name = "status_report_unittest_c",
    srcs = ["status_report_unittest_c.c"],
    hdrs = ["status_report_unittest_c.h"],
    deps = [
        ":status",
    ],
)

cc_test(
    name = "status_report_unittest",
    srcs = [
        "status_report_unittest.cc",
    ],
    deps = [
        ":status",
        ":status_report_unittest_c",
        "@googletest//:gtest_main",
    ],
)

ot_rust_library(
    name = "static_assert_rust",
    srcs = ["static_assert.rs"],
)

ot_rust_library(
    name = "panic",
    srcs = ["panic.rs"],
)
