import subprocess
import re

from coverage_helper import load_view_zip


def parse_unified_diff(diff):
  parsed = {}
  file_headers = []
  current_file = []
  current_hunk = None
  lineno_a = None
  lineno_b = None
  for line in diff.splitlines():
    if line.startswith('--- a/'):
      file_headers.append(line)
    elif line.startswith('+++ b/'):
      file_headers.append(line)
      path = line[6:].strip()
      current_file = []
      parsed[path] = (file_headers, current_file)
      file_headers = []
    elif line.startswith('@@'):
      current_hunk = []
      current_file.append((line.strip(), current_hunk))
      lineno_a, lineno_b = re.match(r'@@ -(\d+)(,\d+)? \+(\d+)(,\d+)? @@', line).group(1, 3)
      lineno_a = int(lineno_a)
      lineno_b = int(lineno_b)
    elif line.startswith('+'):
      current_hunk.append((lineno_b, line))
      lineno_b += 1
    elif line.startswith('-'):
      current_hunk.append((lineno_a, line))
      lineno_a += 1
    elif line.startswith(' '):
      current_hunk.append((lineno_b, line))
      lineno_a += 1
      lineno_b += 1
    else:
      file_headers.append(line)
  return parsed


def filter_diff(diff, view):
  for sf, (headers, hunks) in diff.items():
    sf = f'SF:{sf}'
    if sf not in view:
      continue
    da = view[sf].da

    filtered_hunks = []
    for (hunk_header, lines) in hunks:
      keep_hunk = False
      # Keep the hunk if there's any line removed.
      keep_hunk |= any(line.startswith('-') for lineno, line in lines)
      # Keep the hunk if there's any extra line compiled-in.
      keep_hunk |= any(line.startswith('+') and da.get(lineno, 0) for lineno, line in lines)
      if keep_hunk:
        filtered_hunks.append((hunk_header, lines))

    if len(filtered_hunks):
      print('\n'.join(headers))
      for (hunk_header, lines) in filtered_hunks:
        print(hunk_header)
        print('\n'.join(line for _, line in lines))


def main():
  zip_path = './bazel-out/_coverage/view/instrumented_mask_rom_coverage_view.zip'
  view = load_view_zip(zip_path, use_disassembly=True)

  base_commit = 'earlgrey_1.0.0_A2_presilicon'

  files = [e.removeprefix('SF:') for e in view.keys()]

  if 'sw/device/coverage/asm_counters.c' in files:
    files.remove('sw/device/coverage/asm_counters.c')

  diff = subprocess.run([
    'git', 'diff', base_commit, '--', *files
  ], stdout=subprocess.PIPE).stdout.decode()


  diff = parse_unified_diff(diff)
  filter_diff(diff, view)


if __name__ == '__main__':
  main()
