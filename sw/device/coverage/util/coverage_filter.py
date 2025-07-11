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
  parser.add_argument('--use_disassembly', action='store_true', help='Filter with disassembly.')
  parser.add_argument('--output', type=str, help='Path to the output file.')
  args = parser.parse_args()

  all_views = {}
  for zip_path in args.view:
    view = load_view_zip(zip_path, args.use_disassembly)
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

  # Keep asm coverage unmodified
  for key in original_coverage.keys():
    if key.upper().endswith('.S'):
      coverage[key] = original_coverage[key]

  # Write the filtered coverage to the output file
  coverage = generate_lcov(coverage)
  with open(args.output, 'w') as f:
    f.writelines(coverage)


if __name__ == '__main__':
  main()
