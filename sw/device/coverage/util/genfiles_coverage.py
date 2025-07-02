#!/usr/bin/env python3

import argparse
import sys

from coverage_helper import (
  iter_raw_lcov_files,
  add_coverage,
  generate_lcov,
  parse_lcov,
)

bazel_coverage_path = './bazel-out/_coverage/_coverage_report.dat'
lcov_files_path = './bazel-out/_coverage/lcov_files.tmp'

def main():
  parser = argparse.ArgumentParser(description='Find the tests that covers a given line.')
  parser.add_argument('--coverage', type=str, default=bazel_coverage_path, help='Path to the bazel coverage file.')
  parser.add_argument('--lcov_files', type=str, default=lcov_files_path, help='Path to the coverage file list.')
  parser.add_argument('--output', type=str, required=True, help='Path to the output file.')
  args = parser.parse_args()

  with open(args.coverage, 'r') as f:
    coverage = parse_lcov(f.readlines())

  # Add coverage for generated files
  for test in iter_raw_lcov_files(args.lcov_files):
    genfiles = {k: v for k, v in test.coverage.items() if k.startswith('SF:bazel-out/')}
    coverage = add_coverage(coverage, genfiles)

  # Write the merged coverage to the output file
  coverage = generate_lcov(coverage)
  with open(args.output, 'w') as f:
    f.writelines(coverage)

if __name__ == '__main__':
  main()
