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
  parser = argparse.ArgumentParser(description='Filter related coverage based on a view.')
  parser.add_argument('--view', type=str, nargs='+', required=True, help='Path to the view coverage file.')
  parser.add_argument('--coverage', type=str, help='Path to the coverage file to filter.')
  parser.add_argument('--use_disassembly', action='store_true', help='Filter with disassembly.')
  parser.add_argument('--output', type=str, help='Path to the output file.')
  args = parser.parse_args()

  all_views = {}
  for zip_path in args.view:
    print(f'Loading {zip_path}')
    with zipfile.ZipFile(zip_path, 'r') as view_zip:
      with view_zip.open('coverage.dat', 'r') as f:
        view = parse_lcov(f.read().decode().splitlines())
      # Ignore objects that are discarded in the final firmware
      view = strip_discarded(view)

      if args.use_disassembly:
        with view_zip.open('test.dis', 'r') as f:
          compiled = parse_dis_file(f.read().decode())
        with view_zip.open('coverage.json', 'r') as f:
          segments = parse_llvm_json(f.read().decode())
        compiled = expand_dis_region(compiled, segments)

        # Use normal view coverage for these files.
        for sf in SKIP_DIS:
          compiled[sf] = view[sf]

        # Compiled functions lines includes comments,
        # apply a and filter here to remove them.
        view = and_coverage(compiled, view)
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
    if key.endswith('.S'):
      coverage[key] = original_coverage[key]

  # Write the filtered coverage to the output file
  coverage = generate_lcov(coverage)
  with open(args.output, 'w') as f:
    f.writelines(coverage)


if __name__ == '__main__':
  main()
