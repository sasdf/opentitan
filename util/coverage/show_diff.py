import subprocess
import re

base_commit = 'Earlgrey-PROD-A2-M6-ROM-RC1'

proc = subprocess.run([
  'python3', 'util/coverage/list_sources.py',
    '--pattern=_silicon_creator.elf',
    '//sw/device/silicon_creator/rom:mask_rom',
], stdout=subprocess.PIPE, check=True)
files = proc.stdout.decode().splitlines()
files = [f for f in files if not f.startswith('bazel-out/')]
files = [f for f in files if not f.startswith('external/')]

subprocess.run([
  'git', 'diff', base_commit, '--', *files
])
