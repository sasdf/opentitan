#!/bin/bash

OUTPUT="./targets_all.sh"

QUERY='
  tests(//sw/...)
  except attr("tags",
    "broken|skip_in_ci|manual|sim|silicon|dv|verilator|qemu",
    tests(//sw/...)
  )
'

./bazelisk.sh query "$QUERY" \
  | grep -v -P '/manuf/' \
  | grep -v -P '_coverage_view' \
  > "$OUTPUT"

cat ./targets_otbn.sh >> "$OUTPUT"
cat ./targets_useful_extra.sh >> "$OUTPUT"
cat ./targets_failed.txt >> "$OUTPUT"

echo 'y' | python3 util/coverage/group_targets.py "$OUTPUT"
