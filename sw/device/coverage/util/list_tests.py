import subprocess
import re
import argparse

from coverage_helper import (
  extract_tests,
  add_tests,
)

def query_tests(extended=False):
  skip_in_ci = 'skip_in_ci|' if not extended else ''
  proc = subprocess.run([
    './bazelisk.sh', 'query',
      f'tests(//sw/device/...) ' +
      f'except attr("tags", "{skip_in_ci}manual|broken|sim|silicon", //sw/device/...) ' +
      f'',
  ], stdout=subprocess.PIPE, check=True)
  return dict.fromkeys(proc.stdout.decode().splitlines(), True)

def fix_rom_e2e_env(tests):
  env = '_fpga_cw340_instrumented_rom'
  results = {}
  for key, value in tests.items():
    if '/rom/e2e' in key:
      assert '_fpga_' in key, key
      key = key.rsplit('_fpga_', 1)[0] + env
    elif env in key:
      continue
    results[key] = value
  return results

def filter_tests(tests, pattern):
  return {k: v for k, v in tests.items() if pattern not in k}


def print_tests(test_list):
  if len(test_list):
    for line in sorted(test_list):
      print(line)
  else:
    print('(No tests)')

def main():
  parser = argparse.ArgumentParser()
  parser.add_argument('--extended', action='store_true',
                      help='Include tests marked as skip_in_ci')
  args = parser.parse_args()

  listed = {}
  add_tests(listed, extract_tests('./run_all_coverage.sh'))
  add_tests(listed, extract_tests('./rom_targets.sh'))

  useful = {}
  add_tests(useful, extract_tests('./targets_useful_extra.sh'))

  if args.extended:
    add_tests(listed, extract_tests('./targets_skip_in_ci.sh'))

  minset = {}
  add_tests(minset, extract_tests('./run_min_set_coverage.sh'))
  add_tests(minset, extract_tests('./targets_min_set.sh'))

  available = query_tests(args.extended)

  test_groups = [available, listed, minset, useful]

  test_groups = [fix_rom_e2e_env(g) for g in test_groups]
  test_groups = [filter_tests(g, 'baseline_coverage') for g in test_groups]
  test_groups = [filter_tests(g, '/orchestrator/') for g in test_groups]
  test_groups_en = [{k for k, v in g.items() if v} for g in test_groups]
  test_groups = [set(g.keys()) for g in test_groups]

  available, listed, minset, useful = test_groups
  available_en, listed_en, minset_en, useful_en = test_groups_en

  print()
  print(f'Found {len(listed)} listed tests')
  print(f'Found {len(available)} available tests')
  print(f'Found {len(minset)} minset tests')

  print()
  print('Minset but not listed:')
  print_tests(minset_en - listed_en - useful_en)

  print()
  print('Listed but not available:')
  print_tests(listed - available)

  print()
  print('Available but not listed:')
  print_tests(available - listed)

if __name__ == '__main__':
  main()
