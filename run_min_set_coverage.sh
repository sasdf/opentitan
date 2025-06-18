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
//sw/device/silicon_creator/lib/drivers:flash_ctrl_unittest
//sw/device/lib/base:memory_unittest
//sw/device/silicon_creator/rom_ext/e2e/handoff:fault_return_boot_failed_fpga_hyper310_rom_ext
//sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_disability_spidfu_fpga_hyper310_rom_ext
//sw/device/silicon_creator/rom_ext/e2e/rescue:rescue_disability_xmodem_fpga_hyper310_rom_ext
//sw/device/silicon_creator/rom_ext/e2e/dice_chain:corrupted_digest_test_fpga_cw340_rom_ext
)

source ./targets_min_set.sh

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
