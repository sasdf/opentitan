#!/bin/bash
set -euo pipefail

COVERAGE_OUTPUT_DIR="/tmp/$USER/coverage/"

TARGETS=(
    //sw/device/tests:uart_smoketest_fpga_cw340_test_rom
    //sw/device/tests:uart_smoketest_fpga_cw340_instrumented_rom
    //sw/device/lib/crypto/drivers:aes_test_fpga_cw310_rom_with_fake_keys
    //sw/device/lib/base:crc32_unittest
    //sw/device/tests/crypto/cryptotest:hmac_sha256_kat_fpga_cw340_test_rom
    //sw/device/tests:uart_smoketest_fpga_cw340_rom_ext
    //sw/device/silicon_creator/rom_ext/e2e/dice_chain:no_refresh_dice_x509_test_fpga_cw340_rom_ext
    //sw/device/tests:rv_core_ibex_isa_test_prod_fpga_cw310_rom_with_fake_keys
)

BAZEL_ARGS=(
    --test_output=streamed
    --test_timeout=600
    --copt=-Wno-error
    --copt=-Wno-enum-constexpr-conversion
    --cache_test_results=no
    --config=ot_coverage
    --local_test_jobs=1
    --notest_runner_fail_fast
    --keep_going
    # --jobs=1
    # --subcommands
)

COVERAGE_DAT="bazel-out/_coverage/_coverage_report.dat"

rm -f "${COVERAGE_DAT}"

./bazelisk.sh coverage "${TARGETS[@]}" "${BAZEL_ARGS[@]}" "$@" || true

GENHTML_ARGS=(
    --prefix "${PWD}"
    --ignore-errors unsupported
    --ignore-errors inconsistent
    --ignore-errors category
    # --ignore-errors corrupt
    --html-epilog sw/device/coverage/report_epilog.html
    --output "${COVERAGE_OUTPUT_DIR}"
    "${COVERAGE_DAT}"
)

genhtml "${GENHTML_ARGS[@]}"
