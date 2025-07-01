#!/usr/bin/env python3

import argparse

from coverage_helper import parse_lcov

path = './bazel-out/_coverage/baseline/all_baselines.dis.dat'

def main():
  parser = argparse.ArgumentParser(description='List coverage ratio of each file in csv format.')
  parser.add_argument('--path', type=str, default=path, help='Path to the coverage file.')
  args = parser.parse_args()

  with open(args.path) as f:
    coverage = parse_lcov(f.read().splitlines())

  for sf, cov in sorted(coverage.items()):
    if sf.endswith('/asm_counters.c'):
      continue

    fn_hit = sum(1 for count in cov.fnda.values() if count > 0)
    fn_rate = (fn_hit / len(cov.fnda)) if len(cov.fnda) else 1.0

    line_hit = sum(1 for count in cov.da.values() if count > 0)
    line_miss = len(cov.da) - line_hit if len(cov.da) else 0
    line_rate = (line_hit / len(cov.da)) if len(cov.da) else 1.0

    name = '//' + sf[3:] + '.gcov.html'
    print(f'{name},{line_miss},{line_rate*100:.2f},{fn_rate*100:.2f}')

if __name__ == '__main__':
  main()
