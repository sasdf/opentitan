#!/usr/bin/env python3

import argparse
import sys

from coverage_helper import iter_lcov_files

lcov_files_path = './bazel-out/_coverage/lcov_files.tmp'

def main():
  parser = argparse.ArgumentParser(description='Find the tests that covers a given line.')
  parser.add_argument('line_spec', type=str, help='The //file:line to be search.')
  parser.add_argument('--lcov_files', type=str, default=lcov_files_path, help='Path to the coverage file list.')
  args = parser.parse_args()

  label, line = args.line_spec.rsplit(':', 1)
  line = int(line)
  sf = 'SF:' + label.rstrip('//')
  print(sf)

  tests = []
  for test in iter_lcov_files(args.lcov_files):
    coverage = test.coverage
    if sf in coverage:
      if coverage[sf].da.get(line, 0):
        tests.append(test)

  print('=' * 30)
  for test in tests:
    print(test.name)

if __name__ == '__main__':
  main()
