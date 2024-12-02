RUSTC_FLAGS_COMMON = [
]

# Flags from https://github.com/rust-fuzz/cargo-fuzz/blob/main/src/project.rs#L169
RUSTC_FLAGS_FUZZ = select({
    "@lowrisc_opentitan//rules/configs:fuzzing_build": [
        "-Zextra-const-ub-checks=yes",
        "-Cpasses=sancov-module",
        "-Cllvm-args=-sanitizer-coverage-level=4",
        "-Cllvm-args=-sanitizer-coverage-inline-8bit-counters",
        "-Cllvm-args=-sanitizer-coverage-pc-table",
        "-Cllvm-args=-sanitizer-coverage-trace-compares",
        "--cfg=fuzzing",
        "-Zstack-protector=all",
        "-Cinstrument-coverage=all",
    ],
    "//conditions:default": [],
})

RUSTC_FLAGS_COVERAGE = select({
    "@lowrisc_opentitan//rules/configs:collect_code_coverage": [
        "-Cinstrument-coverage=all",
    ],
    "//conditions:default": [],
})

RUSTC_FLAGS_SANITIZER = select({
    "@lowrisc_opentitan//rules/configs:fuzzing_sanitizer_asan": [
        "-Zsanitizer=address",
    ],
    "@lowrisc_opentitan//rules/configs:fuzzing_sanitizer_msan": [
        "-Zsanitizer=memory",
    ],
    "@lowrisc_opentitan//rules/configs:fuzzing_sanitizer_msan_origin": [
        "-Zsanitizer=memory",
        "-Zsanitizer-memory-track-origins",
    ],
    "//conditions:default": [],
})

RUSTC_FLAGS_PLATFORM = select({
    "@lowrisc_opentitan//rules:opentitan_platform": [
        "-Copt-level=s",
        "--cfg=ot_platform_rv32",
        '--cfg=feature="panic_immediate_abort"',
        "-Cpanic=abort",
    ],
    "//conditions:default": [
        "-Copt-level=2",
    ],
})

RUSTC_FLAGS_LTO = select({
    "@lowrisc_opentitan//rules/configs:lto_enabled": [
        "-Clinker-plugin-lto",
    ],
    "//conditions:default": [],
})

RUSTC_FLAGS = (
    RUSTC_FLAGS_COMMON +
    RUSTC_FLAGS_PLATFORM +
    RUSTC_FLAGS_LTO +
    RUSTC_FLAGS_COVERAGE +
    RUSTC_FLAGS_FUZZ +
    RUSTC_FLAGS_SANITIZER
)
