#!/usr/bin/env python3

import argparse
import itertools as it
import numpy as np
import sys
import re

from pathlib import Path


from coverage_helper import (
  parse_lcov,
  filter_coverage,
  merge_inlined_copies,
  iter_lcov_files,
  extract_tests,
  collect_test_vectors,
  add_tests,
)

view_path = './bazel-out/_coverage/view/all_views.dat'
lcov_files_path = './bazel-out/_coverage/lcov_files.tmp'

def main():
  parser = argparse.ArgumentParser(description='Find the minimum set of tests that produce full coverage.')
  parser.add_argument('--view', type=str, default=view_path,
      help='Path to the filtered view coverage file created by coverage_filter.py.')
  parser.add_argument('--lcov_files', type=str, default=lcov_files_path, help='Path to the coverage file list.')
  args = parser.parse_args()

  test_names, test_values, test_durations = collect_test_vectors(args.view, args.lcov_files)

  print(f'Collected {test_values.shape[1]} constraints from {len(test_names)} tests', file=sys.stderr)
  print(f'Total test duration {test_durations.sum()/60:.2f} minutes', file=sys.stderr)


  normal = {}
  add_tests(normal, extract_tests('./run_all_coverage.sh'))
  add_tests(normal, extract_tests('./targets_rom.sh'))

  extended = normal.copy()
  add_tests(extended, extract_tests('./run_min_set_coverage.sh'))
  add_tests(extended, extract_tests('./targets_min_set.sh'))

  normal_values = []
  for name, values in zip(test_names, test_values):
    if normal.get(name, False):
      normal_values.append(values)
  normal_uncovered = np.sum(normal_values, 0) == 0

  print('USEFUL_EXTRA_TESTS=(')
  for name, values in zip(test_names, test_values):
    if extended.get(name, False):
      diff = np.logical_and(normal_uncovered, values > 0)
      if diff.any():
        print(name)
  print(')')


if __name__ == '__main__':
  main()
