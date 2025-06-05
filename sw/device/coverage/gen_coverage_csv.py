import re
import csv
from collections import namedtuple

path = './bazel-out/_coverage/baseline/all_baselines.dis.dat'

FileProfile = namedtuple('FileProfile', ['sf', 'fn', 'fnda', 'da'])

def parse_single_file(lines):
  path = lines.pop().strip()
  assert path.startswith('SF:'), path

  profile = FileProfile(
    sf=path,
    fn={},
    fnda={},
    da={},
  )

  while True:
    line = lines.pop().strip()
    tag, params, *_ = line.split(':', 1) + ['']
    if tag == 'FN':
      lineno, name = params.split(',')
      profile.fn[name] = int(lineno)
    elif tag == 'FNDA':
      count, name = params.split(',')
      profile.fnda[name] = int(count)
    elif tag == 'BRDA':
      pass
      # raise NotImplementedError('BRDA is not supported yet')
    elif tag == 'DA':
      lineno, count, *_ = params.split(',')
      profile.da[int(lineno)] = int(count)
    elif tag in {'LH', 'LF', 'BRH', 'BRF', 'FNH', 'FNF'}:
      # These are summary lines, we don't care.
      pass
    elif tag == 'end_of_record':
      break
    else:
      raise ValueError(f'Unexpected line: {line}')

  return profile

def parse_lcov(lines):
  lines = lines[::-1]
  files = {}
  while len(lines):
    profile = parse_single_file(lines)
    files[profile.sf] = profile
  return files

with open(path) as f:
  coverage = parse_lcov(f.read().splitlines())

for sf, cov in sorted(coverage.items()):
  fn_hit = sum(1 for count in cov.fnda.values() if count > 0)
  fn_rate = (fn_hit / len(cov.fnda)) if len(cov.fnda) else 1.0

  line_hit = sum(1 for count in cov.da.values() if count > 0)
  line_rate = (line_hit / len(cov.da)) if len(cov.da) else 1.0

  name = '//' + sf[3:] + '.gcov.html'
  print(f'{name},{line_rate*100:.2f},{fn_rate*100:.2f}')
