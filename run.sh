#!/bin/bash

TMP="/tmp/${USER}/tests"
OUTPUT="/tmp/${USER}/logs"

mkdir -p "${TMP}"

QUERY='
  tests(//sw/...)
  except attr("tags",
    "broken|manual|sim|silicon|dv|verilator|qemu",
    tests(//sw/...)
  )
'

./bazelisk.sh query "$QUERY" > "${TMP}/all"

cat "${TMP}/all" | grep -P 'orchestrator/tests:e2e_' > "${TMP}/prov"
cat "${TMP}/all" | grep -v -P 'orchestrator/tests:e2e_' > "${TMP}/non-prov"
cat "${TMP}/non-prov" | grep -v -P '_fpga_' > "${TMP}/unittest"
cat "${TMP}/non-prov" | grep -P '_fpga_' > "${TMP}/fpga"

cat "${TMP}/fpga" | awk -F'_fpga_' '{print $NF}' | sort -r | uniq > "${TMP}/groups"

FLAGS=(
  --test_output=streamed
  --notest_runner_fail_fast
  --keep_going
)

for group in $(cat "${TMP}/groups"); do
  echo "Testing ${group} group"
  ./bazelisk.sh test ${FLAGS[@]} $(cat "${TMP}/fpga" | grep -P "${group}\$")
done

echo "Testing unittest group"
./bazelisk.sh test ${FLAGS[@]} $(cat "${TMP}/unittest")

echo "Testing provisioning group"
./bazelisk.sh test ${FLAGS[@]} $(cat "${TMP}/prov")

echo "Finding testlogs files"
./bazelisk.sh cquery "$QUERY" --output=starlark --starlark:expr='
[
  file.dirname
  for action in target.actions if action.mnemonic == "TestRunner"
  for file in action.outputs.to_list() if file.basename == "test.log"
][0]
' > "${TMP}/logs"

echo "Collecting testlogs to ${OUTPUT}"
for path in $(cat "${TMP}/logs"); do
  testdir="${path#bazel-out/*/testlogs/}"
  output="${OUTPUT}/${testdir}/test.xml"
  mkdir -p "$(dirname "${output}")"
  cp "${path}/test.xml" "${output}"
done

echo "Listing failed tests"
(
  cd "$OUTPUT"
  grep -Pr '(failures|errors)="[^0]"' -l | sort > failed.txt
  count="$(wc -l failed.txt)"
  echo "Found $count failed tests"
)
