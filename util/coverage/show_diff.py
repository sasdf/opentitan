import re
import subprocess
import sys

base_commit = subprocess.check_output(
    ['git', 'merge-base', 'earlgrey_1.0.0', 'HEAD'],
    encoding='utf-8').strip()
print(f"Diff after commit {base_commit}", file=sys.stderr)

with open('./bazel-out/_coverage/view/all_views.dat') as f:
  files = re.findall(r'SF:(.*)\n', f.read())

subprocess.run([
  'git', 'diff', base_commit, '--', *files
])
