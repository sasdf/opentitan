import itertools as it
import argparse

from coverage_helper import (
  extract_tests,
  gen_targets,
  save_with_diff,
)

def main():
  parser = argparse.ArgumentParser()
  parser.add_argument('input_file', help='Path to the input targets file')
  parser.add_argument('-o', '--output', help='Path to the output file')
  args = parser.parse_args()

  output_file = args.output if args.output else args.input_file
  tests = extract_tests(args.input_file)
  output = gen_targets(tests)
  save_with_diff(output_file, output)

if __name__ == "__main__":
    main()
