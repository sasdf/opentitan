COVERAGE_DAT="bazel-out/_coverage/_coverage_report.dat"
LCOV_FILES="bazel-out/_coverage/lcov_files.tmp"
VIEW_CACHE_DIR="bazel-out/_coverage/view/"
TEST_LOGS_DIR="bazel-out/k8-fastbuild/testlogs/"
VIEWER_DIR="${COVERAGE_OUTPUT_DIR}/viewer"

# FPGA checks
if lsusb | grep ':c310' > /dev/null; then
  BAZEL_ARGS+=(
    --//rules:fpga=""
  )
fi

if ! lsusb | grep ':c340' > /dev/null; then
  echo "ERROR: No CW340 board is connected"
  exit 1
fi

mkdir -p "${VIEWER_DIR}"

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

python3 ./util/coverage/collect_view_json.py \
    --output="${VIEWER_DIR}/view.json.gz"

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
./bazelisk.sh coverage "${TARGETS[@]}" "${COVERAGE_VIEWS[@]}" "${BAZEL_ARGS[@]}" "$@" || true

python3 ./util/coverage/collect_coverage_json.py \
    --output="${VIEWER_DIR}/coverage.json.gz"

python3 ./util/coverage/viewer_bundler.py bundle \
    --coverage_json="${VIEWER_DIR}/coverage.json.gz" \
    --view_json="${VIEWER_DIR}/view.json.gz" \
    --output_html="${VIEWER_DIR}/index.html"

if [[ "${#COVERAGE_VIEWS[@]}" == "0" ]]; then
    bash ./run_genhtml.sh \
        "${COVERAGE_DAT}" \
        "${COVERAGE_OUTPUT_DIR}/no_view/"
else
    function generate_report() {
        view_name="$1"
        shift
        view_dats=("$@")

        output_dir="${COVERAGE_OUTPUT_DIR}/${view_name}"
        temp_dat="${output_dir}.dat"
        output_dat="${output_dir}/coverage.dat"
        echo "Filter with view '${view_name}'"
        echo mkdir -p "${output_dir}"
        mkdir -p "${output_dir}"

        python3 util/coverage/coverage_filter.py \
            --view "${view_dats[@]}" \
            --coverage="${COVERAGE_DAT}" \
            --output="${temp_dat}"

        bash ./run_genhtml.sh \
            "${temp_dat}" \
            "${output_dir}"

        python3 util/coverage/gen_coverage_csv.py \
          --path="${temp_dat}" \
          > "${output_dir}/coverage.csv"

        mv "${temp_dat}" "${output_dat}"
    }

    view_files="$(cat "${LCOV_FILES}" | grep "_coverage_view/coverage.dat$")"
    for view_dat in $view_files; do
        view_dir="${view_dat%/*}"
        view_name="${view_dir##*/}"
        generate_report "${view_name}" "${view_dat}"
    done

    for group_name in "${COVERAGE_VIEW_GROUPS[@]}"; do
        group_expr="${group_name}[@]"
        group=( "${!group_expr}" )
        group=( "${group[@]//:/\/}" )  # replace : with /
        group=( "${group[@]/#\/\//$TEST_LOGS_DIR}" )  # TEST_LOGS_DIR prefix
        group=( "${group[@]/%/\/coverage.dat}" )  # coverage.dat suffix
        group_name="${group_name,,}"

        generate_report "${group_name}" "${group[@]}"
    done

    generate_report "all_views" $view_files
fi

echo "Save test target list"
printf '%s\n' "${TARGETS[@]}" | sort | uniq > "${COVERAGE_OUTPUT_DIR}/test_targets.txt"

echo "Save testlogs"
python3 util/coverage/bundle_logs.py "${COVERAGE_OUTPUT_DIR}/testlogs"

echo "Save ToE source diff"
python3 util/coverage/show_diff.py > "${COVERAGE_OUTPUT_DIR}/toe_source.diff"

base_commit="$(git merge-base earlgrey_1.0.0 HEAD)"
git log -n 5 "${base_commit}" > "${COVERAGE_OUTPUT_DIR}/commit_base.log"
git log -n 5 > "${COVERAGE_OUTPUT_DIR}/commit_coverage.log"
