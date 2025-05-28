set -euxo pipefail

BASELINE_TEST="rom_ext_dice_x509_slot_virtual_baseline_coverage"
LCOV_FILES="bazel-out/_coverage/lcov_files.tmp"
BASELINE_DAT="$(grep "${BASELINE_TEST}" "${LCOV_FILES}" | head -n 1 || true)"
COVERAGE_DAT="bazel-out/_coverage/_coverage_report.dat"
CACHED_DAT="bazel-out/_coverage/_coverage_filtered_baseline.dat"
FILTERED_DAT="bazel-out/_coverage/_coverage_filtered.dat"
COVERAGE_OUTPUT_DIR="/tmp/coverage/"

if [[ -f "${BASELINE_DAT}" ]]; then
  cp "${BASELINE_DAT}" "${CACHED_DAT}"
  echo "INFO: Baseline coverage cached successfully, please run the real test."
  exit 1
elif [[ -f "${CACHED_DAT}" ]]; then
  BASELINE_DAT="${CACHED_DAT}"
else
  echo "ERROR: Baseline coverage report not found!"
  exit 1
fi


mkdir -p "${COVERAGE_OUTPUT_DIR}"

echo "Filtering Coverage"

python3 sw/device/coverage/coverage_filter/coverage_filter.py \
  --baseline="${BASELINE_DAT}" \
  --coverage="${COVERAGE_DAT}" \
  --output="${FILTERED_DAT}" \

GENHTML_ARGS=(
    --prefix "${PWD}"
    --ignore-errors unsupported
    --ignore-errors inconsistent
    --ignore-errors category
    # --ignore-errors corrupt
    --output "${COVERAGE_OUTPUT_DIR}"
    "${FILTERED_DAT}"
)

genhtml "${GENHTML_ARGS[@]}"
