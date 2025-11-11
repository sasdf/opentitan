import sys
import shutil
from pathlib import Path


all_dirs = {}
output = Path(sys.argv[1])

with open('./bazel-out/_coverage/lcov_files.tmp') as f:
  for line in f:
    line = Path(line.strip())
    if line.name != 'coverage.dat':
      continue
    line = line.parent / 'test.xml'
    assert line.exists(), line
    idx = line.parts.index('testlogs') + 1
    dst = Path(*line.parts[idx:])
    assert dst not in all_dirs, dst
    all_dirs[dst] = line

if output.exists():
  shutil.rmtree(output)

for key, value in all_dirs.items():
  key = output / key
  key.parent.mkdir(parents=True, exist_ok=True)
  shutil.copyfile(value, key)
  # print(key, '->', value)

print(f'[+] Collected {len(all_dirs)} testlogs.')
