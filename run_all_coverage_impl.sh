COVERAGE_DAT="bazel-out/_coverage/_coverage_report.dat"
LCOV_FILES="bazel-out/_coverage/lcov_files.tmp"
VIEW_CACHE_DIR="bazel-out/_coverage/view/"

rm -f "${COVERAGE_DAT}"

if ! declare -p COVERAGE_VIEWS &>/dev/null; then
  COVERAGE_VIEWS=()
fi

if [[ "${#COVERAGE_VIEWS[@]}" == "0" ]]; then
    COVERAGE_VIEWS=()
    for group_name in "${COVERAGE_VIEW_GROUPS[@]}"; do
        group_expr="${group_name}[@]"
        COVERAGE_VIEWS+=( "${!group_expr}" )
    done
fi

./bazelisk.sh coverage "${COVERAGE_VIEWS[@]}" "${BAZEL_ARGS[@]}" "$@"
CACHED_VIEWS=()

rm -rf "${VIEW_CACHE_DIR}"
mkdir -p "${VIEW_CACHE_DIR}"

view_files="$(cat "${LCOV_FILES}" | grep "/coverage.dat$")"
for view_dat in $view_files; do
    view_dir="${view_dat%/*}"
    view_name="${view_dir##*/}"
    outputs_dir="${view_dir}/test.outputs"
    outputs_zip="${outputs_dir}/outputs.zip"
    cached_zip="${VIEW_CACHE_DIR}/${view_name}.zip"
    CACHED_VIEWS+=( "${cached_zip}" )
    if [[ -f "$outputs_zip" ]]; then
        cp "${outputs_zip}" "${cached_zip}"
    else
        zip -q -0 -j "${cached_zip}" -r "${outputs_dir}"
    fi
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

MERGED_DAT="${VIEW_CACHE_DIR}/merged_coverage.dat"
python3 sw/device/coverage/util/genfiles_coverage.py \
  --coverage="${COVERAGE_DAT}" \
  --lcov_files="${LCOV_FILES}" \
  --output="${MERGED_DAT}"

if [[ "${#CACHED_VIEWS[@]}" == "0" ]]; then
    bash ./run_genhtml.sh \
        "${MERGED_DAT}" \
        "${COVERAGE_OUTPUT_DIR}/no_view/"
else
    for cached_zip in "${CACHED_VIEWS[@]}"; do
        view_name="${cached_zip##*/}"
        view_name="${view_name%.zip}"
        filtered_dat="${VIEW_CACHE_DIR}/${view_name}.dat"
        output_dir="${COVERAGE_OUTPUT_DIR}/${view_name}"
        echo "Filter with view '${view_name}'"

        python3 sw/device/coverage/util/coverage_filter.py \
          --view="${cached_zip}" \
          --coverage="${MERGED_DAT}" \
          --use_disassembly \
          --output="${filtered_dat}"

        bash ./run_genhtml.sh \
            "${filtered_dat}" \
            "${output_dir}"

        python3 sw/device/coverage/util/gen_coverage_csv.py \
          --path="${filtered_dat}" \
          > "${output_dir}/coverage.csv"
    done

    for group_name in "${COVERAGE_VIEW_GROUPS[@]}"; do
        group_expr="${group_name}[@]"
        group=( "${!group_expr##*:}" )
        group_zip=( "${group[@]/#/${VIEW_CACHE_DIR}}" )
        group_zip=( "${group_zip[@]/%/.zip}" )
        filtered_dat="${VIEW_CACHE_DIR}/${group_name,,}.dat"
        output_dir="${COVERAGE_OUTPUT_DIR}/${group_name,,}"
        echo "Filter with view group '${group_name,,}'"

        python3 sw/device/coverage/util/coverage_filter.py \
          --view "${group_zip[@]}" \
          --coverage="${MERGED_DAT}" \
          --use_disassembly \
          --output="${filtered_dat}"

        bash ./run_genhtml.sh \
            "${filtered_dat}" \
            "${output_dir}"

        python3 sw/device/coverage/util/gen_coverage_csv.py \
          --path="${filtered_dat}" \
          > "${output_dir}/coverage.csv"
    done

    filtered_dat="${VIEW_CACHE_DIR}/all_views.dat"
    output_dir="${COVERAGE_OUTPUT_DIR}/all_views"
    python3 sw/device/coverage/util/coverage_filter.py \
      --view "${CACHED_VIEWS[@]}" \
      --coverage="${MERGED_DAT}" \
      --use_disassembly \
      --output="${filtered_dat}"

    bash ./run_genhtml.sh \
        "${filtered_dat}" \
        "${output_dir}"

    python3 sw/device/coverage/util/gen_coverage_csv.py \
      --path="${filtered_dat}" \
      > "${output_dir}/coverage.csv"
fi

echo "Save test target list"
printf '%s\n' "${TARGETS[@]}" | sort | uniq > "${COVERAGE_OUTPUT_DIR}/test_targets.txt"

echo "Save ToE source diff"
python3 sw/device/coverage/util/show_diff.py > "${COVERAGE_OUTPUT_DIR}/toe_source.diff"

echo "Save ROM source diff"
python3 sw/device/coverage/util/show_rom_diff.py > "${COVERAGE_OUTPUT_DIR}/taped_out_rom.diff"
