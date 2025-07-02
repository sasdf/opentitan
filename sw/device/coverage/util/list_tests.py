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
  args = parser.parse_args()

  listed = {}
  add_tests(listed, extract_tests('./run_all_coverage.sh'))
  add_tests(listed, extract_tests('./targets_ci.sh'))
  add_tests(listed, extract_tests('./targets_rom.sh'))

  useful = extract_tests('./targets_useful_extra.sh')

  extended = extract_tests('./targets_skip_in_ci.sh')

  minset = {}
  add_tests(minset, extract_tests('./run_min_set_coverage.sh'))
  add_tests(minset, extract_tests('./targets_min_set.sh'))

  quick_1m = extract_tests('./targets_quick_1m.sh')
  quick_5m = extract_tests('./targets_quick_5m.sh')
  quick_10m = extract_tests('./targets_quick_10m.sh')

  available_ci = query_tests(False)
  available = query_tests(True)

  test_groups_names = [
    'available', 'available_ci',
    'listed', 'extended', 'minset', 'useful',
    'quick_1m', 'quick_5m', 'quick_10m',
  ]
  test_groups = [eval(name) for name in test_groups_names]

  test_groups = [fix_rom_e2e_env(g) for g in test_groups]
  test_groups = [filter_tests(g, 'coverage_view') for g in test_groups]
  test_groups = [filter_tests(g, '/orchestrator/') for g in test_groups]
  test_groups_en = [{k for k, v in g.items() if v} for g in test_groups]
  test_groups = [set(g.keys()) for g in test_groups]

  E = dict(zip(test_groups_names, test_groups_en))
  G = dict(zip(test_groups_names, test_groups))

  print()
  for name, group in G.items():
    print(f'Found {len(group)} tests in {name} set')

  print()
  print('Minset but not listed:')
  print_tests(E['minset'] - E['listed'] - E['extended'] - E['useful'])

  print()
  print('Available but not listed:')
  print_tests(G['available_ci'] - G['listed'])

  print()
  print('Available but not listed (extended):')
  print_tests(G['available'] - G['available_ci'] - G['extended'])

  print()
  for name, group in G.items():
    diff = group - G['available']
    if len(diff):
      print(f'Listed in {name} but not available:')
      print_tests(diff)

if __name__ == '__main__':
  main()
