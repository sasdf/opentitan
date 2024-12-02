config_setting(
    name = "collect_code_coverage",
    values = {"collect_code_coverage": "true"},
)

config_setting(
    name = "fuzzing_build",
    flag_values = {"@rules_fuzzing//fuzzing:cc_engine_instrumentation": "libfuzzer"},
)

config_setting(
    name = "fuzzing_sanitizer_asan",
    flag_values = {"@rules_fuzzing//fuzzing:cc_engine_sanitizer": "asan"},
)

config_setting(
    name = "fuzzing_sanitizer_msan",
    flag_values = {"@rules_fuzzing//fuzzing:cc_engine_sanitizer": "msan"},
)

config_setting(
    name = "fuzzing_sanitizer_msan_origin",
    flag_values = {"@rules_fuzzing//fuzzing:cc_engine_sanitizer": "msan-origin-tracking"},
)

config_setting(
    name = "lto_enabled",
    values = {"features": "lto"},
)
