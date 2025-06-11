#!/usr/bin/env python3

import argparse
import itertools as it
import numpy as np
import sys
import re

from pathlib import Path

# pip install ortools
from ortools.linear_solver import pywraplp


from coverage_helper import (
  parse_lcov,
  filter_coverage,
  merge_inlined_copies,
  iter_lcov_files,
  collect_test_vectors,
)

view_path = './bazel-out/_coverage/baseline/all_baselines.dis.dat'
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

  test_costs = np.clip(np.round(test_durations / 1), 1, np.inf)
  print(test_costs, file=sys.stderr)

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

  # Minimize the execution time
  objective = 0
  for name, v, cost in zip(test_names, test_vars, test_costs):
    # Preference:
    #   unittest >>> test_rom tests > fake keys > others/rom_ext > instrumented rom
    name = str(name)
    if '_unittest' in name:
      objective += 0.000001 * v * cost
    elif '_test_rom' in name:
      objective += 0.997 * v * cost
    elif '_fake_keys' in name:
      objective += 0.998 * v * cost
    elif '_instrumented_rom' in name:
      objective += 1.001 * v * cost
    else:
      objective += 1.000 * v * cost
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
  estimated_time = 0
  print('Solution: ', file=sys.stderr)
  for v in test_vars:
    if v.solution_value():
      i = int(v.name())
      name = str(test_names[i])
      if '_unittest' in name:
        unit_tests += 1
      estimated_time += test_durations[i]
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
  print(f"  Estimated cost: {estimated_time/60:.2f} minutes", file=sys.stderr)

  filtered_values = test_values[pred].sum(0)
  assert (filtered_values > 0).all()

if __name__ == '__main__':
  main()
