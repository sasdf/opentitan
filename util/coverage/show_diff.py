import subprocess
import re

base_commit = '12a2262bfe5b2336e2c7e0eeae0aac7e608c29e4'

with open('./bazel-out/_coverage/view/all_views.dat') as f:
  files = re.findall(r'SF:(.*)\n', f.read())

subprocess.run([
  'git', 'diff', base_commit, '--', *files
])
