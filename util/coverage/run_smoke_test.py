#!/usr/bin/env python3
# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

import subprocess
import re
from pathlib import Path
from typing import Dict

# Bazel test targets to be executed for coverage.
TESTS = [
    "//sw/device/lib/coverage:smoke_unittest",
    "//sw/device/lib/coverage:smoke_functest_sim_qemu_sival_rom_ext",
    "//sw/device/lib/coverage:smoke_functest_sim_qemu_rom_with_fake_keys",
]

# Mapping of function names to their expected coverage status.
# True indicates the function must be covered (FNDA > 0).
# False indicates the function must be uncovered (FNDA == 0).
COMMON_EXPECTATIONS = {
    "covfunc_covered": True,
    "covfunc_nested_covered": True,
    "covfunc_nested_uncovered": False,
    "covfunc_uncovered": False,
    "smoke_target.c:inline_covfunc_uncovered": False,
    # This static inlined function is defined in the header but not called
    # from the smoke_target.c file.
    "smoke_target.c:inline_covfunc_covered": False,
}

# Expectation for '*smoke_unittest*' tests.
UNITTEST_EXPECTATIONS = {
    **COMMON_EXPECTATIONS,
    "smoke_unittest.cc:_ZL22inline_covfunc_coveredv": True,
    "smoke_unittest.cc:_ZL24inline_covfunc_uncoveredv": False,
}

# Expectation for '*smoke_functest*' tests.
FUNCTEST_EXPECTATIONS = {
    **COMMON_EXPECTATIONS,
    "smoke_functest.c:inline_covfunc_covered": True,
    "smoke_functest.c:inline_covfunc_uncovered": False,
}

# Expectation for bazel merged report.
MERGED_EXPECTATIONS = {
    **UNITTEST_EXPECTATIONS,
    **FUNCTEST_EXPECTATIONS,
}

MERGED_REPORT = "bazel-out/_coverage/_coverage_report.dat"
LCOV_LIST = "bazel-out/_coverage/lcov_files.tmp"


def check_file(path: str, expectations: Dict[str, bool]) -> bool:
    file_pass = True
    print(f"Checking coverage patterns in {path}")

    with open(path, 'r') as f:
        content = f.read()

    for pattern, should_be_covered in expectations.items():
        re_pattern = re.escape(pattern)
        is_covered = bool(re.search(f"FNDA:[1-9][0-9]*,{re_pattern}", content))
        is_uncovered = bool(re.search(f"FNDA:0,{re_pattern}", content))

        if should_be_covered and is_covered and not is_uncovered:
            print(f"PASS: Func '{pattern}' correctly marked as covered.")
        elif not should_be_covered and not is_covered and is_uncovered:
            print(f"PASS: Func '{pattern}' correctly marked as uncovered.")
        else:
            expectation_str = 'covered' if should_be_covered else 'uncovered'
            print(f"FAIL: Func '{pattern}' should be {expectation_str}, "
                  f"but is_covered={is_covered}, is_uncovered={is_uncovered}.")
            file_pass = False

    if not file_pass:
        print(f"FAIL: Coverage check for {path} failed.")
        print("Related FNDA entries:")
        for line in content.splitlines():
            if line.startswith("FNDA:") and "covfunc_" in line:
                print(' ' * 4 + line)
    print()

    return file_pass


def main() -> None:
    print("Running bazel coverage...")
    cmd = ["./bazelisk.sh", "coverage", "--config=ot_coverage"] + TESTS
    subprocess.run(cmd, check=True)

    overall_pass = True
    tested = 0

    with open(LCOV_LIST) as f:
        lcov_files = re.findall(r'.*/coverage.dat$', f.read(), re.M)
    lcov_files.append(MERGED_REPORT)

    for path in lcov_files:
        if not Path(path).is_file():
            print(f"FAIL: Coverage file {path} not found.")
            continue

        if "smoke_unittest" in path:
            expectation = UNITTEST_EXPECTATIONS
        elif "smoke_functest" in path:
            expectation = FUNCTEST_EXPECTATIONS
        elif MERGED_REPORT == path:
            expectation = MERGED_EXPECTATIONS
        else:
            raise ValueError(f"Unknown coverage file: {path}")

        overall_pass &= check_file(path, expectation)
        tested += 1

    expected_tested = len(TESTS) + 1
    if tested != expected_tested:
        print(f"FAIL: Only {tested}/{expected_tested} files were checked.")
        exit(2)

    if not overall_pass:
        print("FAIL: Some coverage smoke tests failed.")
        exit(1)

    print("PASS: All coverage smoke tests passed.")


if __name__ == "__main__":
    main()
