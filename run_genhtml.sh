set -euo pipefail

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

# function to run genhtml
lcov_file="$1"
output_dir="$2"

if [[ -z "${output_dir}" ]]; then
  echo "ERROR: Output directory not specified."
  exit 1
fi

rm -rf "${output_dir}"

genhtml "${GENHTML_ARGS[@]}" \
  "${lcov_file}" \
  --output "${output_dir}" \

echo "Post-processing paths"

# Replace "${PWD}" with "//" recursivly in the output_dir
find "${output_dir}" -type f -exec sed -i "s|${PWD}/|//|g" {} \;

# Replace bazel-out generated folder for a stable path
if [[ -d "${output_dir}"/bazel-out ]]; then
  generated_dirs=( "${output_dir}"/bazel-out/* )
  if [[ "${#generated_dirs[@]}" != "1" ]]; then
    ls "${output_dir}/bazel-out"
    echo "ERROR: There are more than one bazel-out build directories in ${output_dir}/bazel-out."
    exit 1
  fi

  generated_dir="${generated_dirs[0]}"

  mv "${generated_dir}" "${output_dir}/bazel-out/build"

  generated_build="$(basename "${generated_dir}")"
  find "${output_dir}/bazel-out" -type f -exec sed -i "s|${generated_build}|build|g" {} \;
  find "${output_dir}" -maxdepth 1 -iname '*.html' -type f -exec sed -i "s|${generated_build}|build|g" {} \;

  # Replace "bazel-out/build/bin" with "generated:" recursivly in the output_dir
  find "${output_dir}/bazel-out" -type f -exec sed -i "s|>bazel-out/build/bin/|>generated:|g" {} \;
  find "${output_dir}" -maxdepth 1 -iname '*.html' -type f -exec sed -i "s|>bazel-out/build/bin/|>generated:|g" {} \;
fi
