#!/bin/bash
set -euo pipefail

COVERAGE_OUTPUT_DIR="/tmp/${USER}/all_coverage/"

BASELINES=(
    "//sw/device/silicon_creator/rom_ext:rom_ext_prod_dice_cwt_spidfu_baseline_coverage"
    "//sw/device/silicon_creator/rom_ext:rom_ext_dice_x509_slot_virtual_baseline_coverage"
    "//sw/device/silicon_creator/rom_ext/imm_section:main_binaries_dice_cwt_slot_virtual_baseline_coverage"
    "//sw/device/silicon_creator/rom_ext/imm_section:main_binaries_dice_x509_slot_virtual_baseline_coverage"
    "//sw/device/silicon_creator/rom:instrumented_mask_rom_baseline_coverage"
)

TARGETS=()

EXTRA_TESTS=(
)

source "$1"
shift

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
    # --jobs=1
    # --subcommands
)

source ./run_all_coverage_impl.sh
