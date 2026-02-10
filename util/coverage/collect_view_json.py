import argparse
from pathlib import Path

from coverage_helper import iter_lcov_files, CoverageCollection

def main():
  parser = argparse.ArgumentParser(description='Collects lcov view files and saves coverage data to a gzipped JSON file.')
  parser.add_argument('--lcov_files', type=Path, default='./bazel-out/_coverage/lcov_files.tmp',
                      help='Path to the file containing a list of lcov files for views.')
  parser.add_argument('--output', type=Path, default='./bazel-out/_coverage/coverage_view.json.gz',
                      help='Path to the output gzipped JSON file for view coverage.')
  args = parser.parse_args()

  cov = CoverageCollection()
  for test_lcov in iter_lcov_files(args.lcov_files):
    cov.add_test(test_lcov.name, test_lcov.coverage, add_uncovered=True)

  print(f'Saving coverage view json to {args.output}')
  cov.save(args.output)

if __name__ == '__main__':
  main()


