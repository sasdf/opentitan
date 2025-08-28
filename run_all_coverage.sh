#!/bin/bash
set -euo pipefail

COVERAGE_OUTPUT_DIR="/tmp/${USER}/all_coverage/"

source ./targets_coverage_views.sh

TARGETS=(
)

source ./targets_ci.sh
source ./targets_rom.sh
source ./targets_skip_in_ci.sh
source ./targets_useful_extra.sh

TEST_GROUPS=(
    "UNIT_TESTS"
    "OTBN_TESTS"
    "TEST_ROM_TESTS"
    "CW310_FAKE_KEYS_TESTS"
    "MANUF_TESTS"
    "CRYPTO_TESTS"
    "CW310_SIVAL_ROMEXT_TESTS"
    "CW310_SIVAL_TESTS"
    "IMM_TESTS"
    "HYPER310_ROMEXT_TESTS"
    "CW310_ROMEXT_TESTS"
    "CW340_SIVAL_TESTS"
    "CW340_FAKE_KEYS_TESTS"
    "CW340_ROM_EXT_TESTS"
    "HYPER310_FAKE_KEYS_TESTS"
    "INS_ROM_TESTS"
    "PROVISIONING_TESTS"
    "USEFUL_EXTRA_TESTS"
    # "${EX_TEST_GROUPS[@]}"
)

BAZEL_ARGS=(
    --test_output=streamed
    # --test_timeout=600
    --copt=-Wno-error
    --copt=-Wno-enum-constexpr-conversion
    # --cache_test_results=no
    --config=ot_coverage
    --local_test_jobs=1
    --notest_runner_fail_fast
    --keep_going
    --spawn_strategy=processwrapper-sandbox
    # --jobs=1
    # --subcommands
)

source ./run_all_coverage_impl.sh
