COVERAGE_DAT="bazel-out/_coverage/_coverage_report.dat"
LCOV_FILES="bazel-out/_coverage/lcov_files.tmp"
BASELINE_CACHE_DIR="bazel-out/_coverage/baseline/"

rm -f "${COVERAGE_DAT}"

./bazelisk.sh coverage "${BASELINES[@]}" "${BAZEL_ARGS[@]}" "$@"
CACHED_BASELINES=()

rm -rf "${BASELINE_CACHE_DIR}"
mkdir -p "${BASELINE_CACHE_DIR}"

baseline_files="$(cat "${LCOV_FILES}" | grep "/coverage.dat$")"
for baseline_dat in $baseline_files; do
    baseline_dir="${baseline_dat%/*}"
    baseline_name="${baseline_dir##*/}"
    cached_zip="${BASELINE_CACHE_DIR}/${baseline_name}.zip"
    CACHED_BASELINES+=( "${cached_zip}" )
    cp "${baseline_dir}/test.outputs/outputs.zip" "${cached_zip}"
    echo "INFO: Baseline coverage cached to '${cached_zip}'."
done


if [[ "${#TARGETS[@]}" == "0" ]]; then
    for test_group_name in "${TEST_GROUPS[@]}"; do
        test_group_expr="${test_group_name}[@]"
        test_group=( "${!test_group_expr}" )
        TARGETS+=( "${test_group[@]}" )
        if [[ "${#test_group[@]}" != "0" ]]; then
            echo "Running test group ${test_group_name}"
            rm -f "${COVERAGE_DAT}"
            ./bazelisk.sh coverage "${test_group[@]}" "${BAZEL_ARGS[@]}" "$@" || true
        else
            echo "Skip empty test group ${test_group_name}"
        fi
    done
fi

echo "Collect overall coverage"
rm -f "${COVERAGE_DAT}"
./bazelisk.sh coverage "${TARGETS[@]}" "${BAZEL_ARGS[@]}" "$@" || true


GENHTML_ARGS=(
    --prefix "${PWD}"
    --ignore-errors unsupported
    --ignore-errors inconsistent
    --ignore-errors category
    # --ignore-errors corrupt
    --exclude sw/device/coverage/
    --ignore-errors unused
    --html-epilog sw/device/coverage/report_epilog.html
)

ASM_COVERAGE="${BASELINE_CACHE_DIR}/asm_coverage.dat"
python3 sw/device/coverage/util/gen_asm_coverage.py \
  --coverage="${COVERAGE_DAT}" \
  --output="${ASM_COVERAGE}"
genhtml "${GENHTML_ARGS[@]}" \
    --output "${COVERAGE_OUTPUT_DIR}/asm_coverage/" \
    "${ASM_COVERAGE}"

COVERAGE_DAT_WITH_ASM="${BASELINE_CACHE_DIR}/coverage_report_asm.dat"
python3 sw/device/coverage/util/gen_asm_coverage.py \
  --coverage="${COVERAGE_DAT}" \
  --append \
  --output="${COVERAGE_DAT_WITH_ASM}"

if [[ "${#CACHED_BASELINES[@]}" == "0" ]]; then
    genhtml "${GENHTML_ARGS[@]}" \
        --output "${COVERAGE_OUTPUT_DIR}/no_baseline/" \
        "${COVERAGE_DAT_WITH_ASM}"
else
    for cached_zip in "${CACHED_BASELINES[@]}"; do
        baseline_name="${cached_zip##*/}"
        baseline_name="${baseline_name%.zip}"
        filtered_dat="${BASELINE_CACHE_DIR}/${baseline_name}.dat"
        filtered_dis_dat="${BASELINE_CACHE_DIR}/${baseline_name}.dis.dat"
        echo "Filter with baseline '${baseline_name}'"

        python3 sw/device/coverage/util/coverage_filter.py \
          --baseline="${cached_zip}" \
          --coverage="${COVERAGE_DAT_WITH_ASM}" \
          --use_disassembly \
          --output="${filtered_dis_dat}"

        genhtml "${GENHTML_ARGS[@]}" \
            --output "${COVERAGE_OUTPUT_DIR}/${baseline_name}_dis" \
            "${filtered_dis_dat}"
    done

    filtered_dis_dat="${BASELINE_CACHE_DIR}/all_baselines.dis.dat"
    python3 sw/device/coverage/util/coverage_filter.py \
      --baseline "${CACHED_BASELINES[@]}" \
      --coverage="${COVERAGE_DAT_WITH_ASM}" \
      --use_disassembly \
      --output="${filtered_dis_dat}"

    genhtml "${GENHTML_ARGS[@]}" \
        --output "${COVERAGE_OUTPUT_DIR}/all_baselines_dis" \
        "${filtered_dis_dat}"
fi
