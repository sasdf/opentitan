import subprocess
import re

base_commit = 'c8ba1a'

with open('./bazel-out/_coverage/baseline/all_baselines.dis.dat') as f:
  files = re.findall(r'SF:(.*)\n', f.read())

subprocess.run([
  'git', 'diff', base_commit, '--', *files
])
