load("@rules_fuzzing//fuzzing:cc_defs.bzl", "cc_fuzz_test")
load("//rules/opentitan:rust.bzl", "ot_rust_library")

ot_rust_library(
    name = "demo",
    srcs = ["demo_fuzz_test.rs"],
)

cc_library(
    name = "demo_cc",
    srcs = ["demo_fuzz_test.cc"],
)

# To test this target, you must specify `--config=asan-libfuzzer`.
cc_fuzz_test(
    name = "demo_fuzz_test",
    # srcs = ["demo_fuzz_test.cc"],
    tags = [
        "fuzzer",
        "manual",
    ],
    deps = [":demo"],
    # deps = [":demo_cc"],
)

sh_test(
    name = "demo_fuzz_test_cov",
    srcs = ["demo_fuzz_test.sh"],
    data = [":demo_fuzz_test_run"],
    tags = [
        "fuzzer",
        "manual",
    ],
)
