import argparse
from pathlib import Path

from coverage_helper import iter_lcov_paths, load_view, CoverageCollection

def main():
  parser = argparse.ArgumentParser(description='Collects lcov view files and saves coverage data to a gzipped JSON file.')
  parser.add_argument('--lcov_files', type=Path, default='./bazel-out/_coverage/lcov_files.tmp',
                      help='Path to the file containing a list of lcov files for views.')
  parser.add_argument('--output', type=Path, default='./bazel-out/_coverage/coverage_view.json.gz',
                      help='Path to the output gzipped JSON file for view coverage.')
  args = parser.parse_args()

  cov = CoverageCollection()
  for test_lcov in iter_lcov_paths(args.lcov_files):
    # Instead of loading coverage.dat, load the view zip from test.outputs/outputs.zip
    test_dir = test_lcov.path.parent / 'test.outputs'
    view_coverage = load_view(test_dir, use_disassembly=True)
    cov.add_test(test_lcov.name, view_coverage)

  print(f'Saving coverage view json to {args.output}')
  cov.save(args.output)

if __name__ == '__main__':
  main()


