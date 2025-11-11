#!/bin/bash

TMP="/tmp/${USER}/tests"
OUTPUT="/tmp/${USER}/logs"

mkdir -p "${TMP}"

QUERY='
  tests(//sw/...)
  except attr("tags",
    "broken|skip_in_ci|manual|sim|silicon|dv|verilator|qemu",
    tests(//sw/...)
  )
'

./bazelisk.sh query "$QUERY" > "${TMP}/all"
# cat ./targets_passed.txt > "$TMP/all"

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

echo "# Targets" > targets_all.sh

for group in $(cat "${TMP}/groups"); do
  echo "Testing ${group} group"
  GROUP="$(echo "$group" | tr '[:lower:]' '[:upper:]')"
  echo "${GROUP}=(" >> targets_all.sh
  cat "${TMP}/fpga" | grep -P "${group}\$" >> targets_all.sh
  echo ")" >> targets_all.sh
done

echo "Testing unittest group"
echo "UNIT_TESTS=(" >> targets_all.sh
cat "${TMP}/unittest" >> targets_all.sh
echo ")" >> targets_all.sh

echo "Testing provisioning group"
echo "PROVISIONING_TESTS=(" >> targets_all.sh
cat "${TMP}/prov" >> targets_all.sh
echo ")" >> targets_all.sh

echo "TEST_GROUPS=(" >> targets_all.sh
cat "${TMP}/groups" | tr '[:lower:]' '[:upper:]' >> targets_all.sh
echo "UNIT_TESTS" >> targets_all.sh
echo "PROVISIONING_TESTS" >> targets_all.sh
echo ")" >> targets_all.sh
