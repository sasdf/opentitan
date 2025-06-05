#!/usr/bin/env python3

import argparse
import zipfile

from coverage_helper import (
  parse_dis_file,
  expand_dis_region,
  parse_lcov,
  strip_discarded,
  parse_llvm_json,
  and_coverage,
  or_coverage,
  filter_coverage,
  merge_inlined_copies,
  generate_lcov,
  SKIP_DIS,
)


def main():
  parser = argparse.ArgumentParser(description='Filter related coverage based on a baseline.')
  parser.add_argument('--baseline', type=str, nargs='+', required=True, help='Path to the baseline coverage file.')
  parser.add_argument('--coverage', type=str, help='Path to the coverage file to filter.')
  parser.add_argument('--use_disassembly', action='store_true', help='Filter with disassembly.')
  parser.add_argument('--output', type=str, help='Path to the output file.')
  args = parser.parse_args()

  all_baselines = {}
  for zip_path in args.baseline:
    print(f'Loading {zip_path}')
    with zipfile.ZipFile(zip_path, 'r') as baseline_zip:
      with baseline_zip.open('coverage.dat', 'r') as f:
        baseline = parse_lcov(f.read().decode().splitlines())
      # Ignore objects that are discarded in the final firmware
      baseline = strip_discarded(baseline)

      if args.use_disassembly:
        with baseline_zip.open('test.dis', 'r') as f:
          compiled = parse_dis_file(f.read().decode())
        with baseline_zip.open('coverage.json', 'r') as f:
          segments = parse_llvm_json(f.read().decode())
        compiled = expand_dis_region(compiled, segments)

        # Use normal baseline coverage for these files.
        for sf in SKIP_DIS:
          compiled[sf] = baseline[sf]

        # Compiled functions lines includes comments,
        # apply a and filter here to remove them.
        baseline = and_coverage(compiled, baseline)
    all_baselines = or_coverage(all_baselines, baseline)
  baseline = all_baselines

  # Read the coverage file to filter
  with open(args.coverage, 'r') as f:
    coverage = parse_lcov(f.readlines())

  # Filter the coverage
  baseline = merge_inlined_copies(baseline)
  coverage = merge_inlined_copies(coverage)
  coverage = filter_coverage(coverage, baseline)
  coverage = generate_lcov(coverage)

  # # Write the filtered coverage to the output file
  with open(args.output, 'w') as f:
    f.writelines(coverage)


if __name__ == '__main__':
  main()
