#!/usr/bin/env python3

import argparse
import zipfile

from coverage_helper import (
  load_view_zip,
  parse_lcov,
  or_coverage,
  filter_coverage,
  merge_inlined_copies,
  generate_lcov,
)


def main():
  parser = argparse.ArgumentParser(description='Filter related coverage based on a view.')
  parser.add_argument('--view', type=str, nargs='+', required=True, help='Path to the view coverage file.')
  parser.add_argument('--coverage', type=str, help='Path to the coverage file to filter.')
  parser.add_argument('--output', type=str, help='Path to the output file.')
  args = parser.parse_args()

  all_views = {}
  for view_dat in args.view:
    with open(view_dat, 'r') as f:
      view = parse_lcov(f.readlines())
    all_views = or_coverage(all_views, view)
  view = all_views

  # Read the coverage file to filter
  with open(args.coverage, 'r') as f:
    coverage = parse_lcov(f.readlines())
  original_coverage = coverage

  # Filter the coverage
  view = merge_inlined_copies(view)
  coverage = merge_inlined_copies(coverage)
  coverage = filter_coverage(coverage, view)

  # Keep otbn asm coverage unfiltered
  for key in original_coverage.keys():
    if 'sw/otbn/' in key and key.upper().endswith('.S'):
      coverage[key] = original_coverage[key]

  # Keep asm coverage unmodified
  for key in coverage.keys():
    if key.upper().endswith('.S'):
      coverage[key] = original_coverage[key]

  # Write the filtered coverage to the output file
  coverage = generate_lcov(coverage)
  with open(args.output, 'w') as f:
    f.writelines(coverage)


if __name__ == '__main__':
  main()
