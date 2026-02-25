import argparse
import re
import subprocess
import sys

parser = argparse.ArgumentParser(description="Show git diff for files listed in a coverage view.")
parser.add_argument('--view', default='./bazel-out/_coverage/view/all_views.dat',
                    help='Path to the coverage view file (default: ./bazel-out/_coverage/view/all_views.dat)')
args = parser.parse_args()

base_commit = subprocess.check_output(
    ['git', 'merge-base', 'earlgrey_1.0.0', 'HEAD'],
    encoding='utf-8').strip()
print(f"Diff after commit {base_commit}", file=sys.stderr)

with open(args.view) as f:
  files = re.findall(r'SF:(.*)\n', f.read())

diff = subprocess.check_output(
  ['git', 'diff', base_commit, '--', *files],
  encoding='utf-8'
)
if diff:
  print(diff)
else:
  print("(No diff. All toe source code changes have been upstreamed)")
