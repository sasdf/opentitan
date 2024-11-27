# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

load("//rules/opentitan:rust.bzl", "cc_library_with_bindgen", "dual_cc_library_with_bindgen")

package(default_visibility = ["//visibility:public"])

cc_library_with_bindgen(
    name = "address",
    srcs = ["address.c"],
    hdrs = ["address.h"],
    deps = [
        ":params",
        "//sw/device/lib/base:memory",
    ],
)

cc_test(
    name = "address_unittest",
    srcs = ["address_unittest.cc"],
    deps = [
        ":address",
        "//sw/device/silicon_creator/testing:rom_test",
        "@googletest//:gtest_main",
    ],
)

cc_library_with_bindgen(
    name = "context",
    hdrs = ["context.h"],
    deps = [
        ":params",
        "//sw/device/silicon_creator/lib/drivers:hmac",
    ],
)

cc_library_with_bindgen(
    name = "fors",
    srcs = ["fors.c"],
    hdrs = ["fors.h"],
    deps = [
        ":address",
        ":context",
        ":hash",
        ":thash",
        ":utils",
        "//sw/device/silicon_creator/lib:error",
    ],
)

cc_library_with_bindgen(
    name = "hash",
    srcs = ["hash_sha2.c"],
    hdrs = ["hash.h"],
    deps = [
        ":address",
        ":context",
        ":params",
        ":sha2",
        ":utils",
        "//sw/device/silicon_creator/lib:error",
        "//sw/device/silicon_creator/lib/drivers:hmac",
    ],
)

cc_library_with_bindgen(
    name = "params",
    hdrs = ["params.h"],
)

cc_library_with_bindgen(
    name = "sha2",
    srcs = ["sha2.c"],
    hdrs = ["sha2.h"],
    deps = [
        ":params",
        "//sw/device/silicon_creator/lib/drivers:hmac",
    ],
)

cc_library_with_bindgen(
    name = "thash",
    srcs = ["thash_sha2_simple.c"],
    hdrs = ["thash.h"],
    deps = [
        ":address",
        ":context",
        ":params",
        ":sha2",
        "//sw/device/lib/base:macros",
        "//sw/device/silicon_creator/lib/drivers:hmac",
    ],
)

cc_library_with_bindgen(
    name = "utils",
    srcs = ["utils.c"],
    hdrs = ["utils.h"],
    deps = [
        ":address",
        ":context",
        ":params",
        ":thash",
        "//sw/device/lib/base:memory",
        "//sw/device/silicon_creator/lib:error",
    ],
)

cc_library_with_bindgen(
    name = "verify",
    srcs = ["verify.c"],
    hdrs = ["verify.h"],
    deps = [
        ":address",
        ":context",
        ":fors",
        ":hash",
        ":params",
        ":thash",
        ":utils",
        ":wots",
        "//sw/device/lib/base:memory",
        "//sw/device/silicon_creator/lib:error",
    ],
)

cc_library_with_bindgen(
    name = "wots",
    srcs = ["wots.c"],
    hdrs = ["wots.h"],
    deps = [
        ":address",
        ":context",
        ":hash",
        ":params",
        ":sha2",
        ":thash",
        ":utils",
        "//sw/device/lib/base:memory",
        "//sw/device/silicon_creator/lib:error",
        "//sw/device/silicon_creator/lib/drivers:hmac",
    ],
)
