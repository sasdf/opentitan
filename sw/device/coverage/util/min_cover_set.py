#!/usr/bin/env python3

import argparse
import itertools as it
import numpy as np
import sys

from pathlib import Path

# pip install ortools
from ortools.linear_solver import pywraplp


from coverage_helper import (
  parse_lcov,
  filter_coverage,
  merge_inlined_copies,
  iter_lcov_files,
)

view_path = './bazel-out/_coverage/baseline/all_baselines.dis.dat'
lcov_files_path = './bazel-out/_coverage/lcov_files.tmp'

def collect_vector(coverage, sf_keys):
  keys, values = [], []
  for sf in sf_keys:
    cov = coverage[sf]
    for name, count in sorted(cov.fnda.items()):
      keys.append(f'{sf}:fn:{name}')
      values.append(1 if count > 0 else 0)
    for lineno, count in sorted(cov.da.items()):
      keys.append(f'{sf}:line:{lineno}')
      values.append(1 if count > 0 else 0)
  return keys, np.array(values, dtype=int)


def main():
  parser = argparse.ArgumentParser(description='Find the minimum set of tests that produce full coverage.')
  parser.add_argument('--view', type=str, default=view_path,
      help='Path to the filtered view coverage file created by coverage_filter.py.')
  parser.add_argument('--lcov_files', type=str, default=lcov_files_path, help='Path to the coverage file list.')
  args = parser.parse_args()

  print(f'Loading {args.view}', file=sys.stderr)
  with open(args.view) as f:
    view = parse_lcov(f.readlines())
    view = merge_inlined_copies(view)

  sf_keys = list(view.keys())
  view_keys, view_values = collect_vector(view, sf_keys)

  tests = {}
  for test in iter_lcov_files(args.lcov_files):
    coverage = test.coverage
    coverage = filter_coverage(coverage, view)
    coverage_keys, coverage_values = collect_vector(coverage, sf_keys)
    assert coverage_keys == view_keys
    tests[test.name] = coverage_values

  test_names, test_values = zip(*tests.items())
  test_values = np.stack(test_values)
  expected_view = (test_values.sum(0) > 0).astype(int)
  assert (expected_view == view_values).all(), (expected_view != view_values).sum()

  test_values = test_values[:, view_values > 0]
  view_values = view_values[view_values > 0]
  assert (view_values == 1).all()
  assert (test_values.sum(0) > 0).all()

  assert len(test_names) == len(test_values)

  print(f'Collected {test_values.shape[1]} constraints from {len(test_names)} tests', file=sys.stderr)

  solver = pywraplp.Solver.CreateSolver("CP-SAT")

  # Variable of whether the test is included.
  test_vars = []
  for i, _ in enumerate(test_names):
    test_vars.append(solver.IntVar(0, 1, str(i)))

  # Set constraint that the coverage should be the same.
  print('Constructing ILP', file=sys.stderr)
  for row in test_values.T:
    assert len(row) == len(test_vars)
    expr = sum((v for v, c in zip(test_vars, row) if c > 0), 0)
    solver.Add(expr >= 1)

  # Minimize the number of tests
  objective = 0
  for name, v in zip(test_names, test_vars):
    # Preference:
    #   unittest >>> test_rom tests > fake keys > others/rom_ext > instrumented rom
    name = str(name)
    if '_unittest' in name:
      objective += 0.000001 * v
    elif '_test_rom' in name:
      objective += 0.997 * v
    elif '_fake_keys' in name:
      objective += 0.998 * v
    elif '_instrumented_rom' in name:
      objective += 1.001 * v
    else:
      objective += 1.000 * v
  solver.Minimize(objective)

  print(f"Solving with {solver.SolverVersion()}", file=sys.stderr)
  status = solver.Solve()
  print('Result:', status, file=sys.stderr)
  if status not in {pywraplp.Solver.OPTIMAL, pywraplp.Solver.FEASIBLE}:
    print('Failed to solve minimum cover set', file=sys.stderr)
    exit(-1)

  pred = []
  pred_names = []
  unit_tests = 0
  print('Solution: ', file=sys.stderr)
  for v in test_vars:
    if v.solution_value():
      i = int(v.name())
      name = str(test_names[i])
      if '_unittest' in name:
        unit_tests += 1
      pred.append(i)
      pred_names.append(name)

  def label_group(name):
    if '_fpga_' in name:
      return name.rsplit('_fpga_')[-1].upper() + '_TESTS', name
    assert '_unittest' in name
    return 'UNIT_TESTS', name

  test_groups = ['EXTRA_TESTS']
  group_with_names = sorted(map(label_group, pred_names))
  group_with_names = it.groupby(group_with_names, key=lambda x: x[0])
  print(f'EXTRA_TESTS=(\n)\n')
  for group, names in group_with_names:
    print(f'{group}=(')
    for _, name in names:
      print(f'  {repr(name)}')
    print(')')
    print()
    test_groups.append(group)
  print()
  print(f'TEST_GROUPS=(')
  for group in test_groups:
    print(f'  {repr(group)}')
  print(')')

  print('================', file=sys.stderr)
  print(f"Problem solved in {solver.wall_time():d} milliseconds", file=sys.stderr)
  print(f"Problem solved in {solver.iterations():d} iterations", file=sys.stderr)
  print(f"Problem solved in {solver.nodes():d} branch-and-bound nodes", file=sys.stderr)
  print(f"Minimum set containing {len(pred):d} tests", file=sys.stderr)
  print(f"  With {unit_tests} unit tests and {len(pred)-unit_tests} FPGA tests", file=sys.stderr)

  filtered_values = test_values[pred].sum(0)
  assert (filtered_values > 0).all()

if __name__ == '__main__':
  main()
