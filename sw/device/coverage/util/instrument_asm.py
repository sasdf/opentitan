import argparse
import re
import enum
import copy
from collections import namedtuple

from asm_helper import (
  ASM_FILES,
  g_available_counters,
  reserve_manual_counters,
  autogen_counters,
  remove_autogen,
  LINE_COLORS,
  COLOR_RESET,
)


if __name__ == '__main__':
  parser = argparse.ArgumentParser(description='Instrument asm files annotated with coverage pragma.')
  parser.add_argument('--files', type=str, nargs='+', default=ASM_FILES, help='Files to instrument.')
  parser.add_argument('--dryrun', action='store_true', help='Do not save instrumented code.')
  parser.add_argument('--clear', action='store_true', help='Remove all autogen instrumentations.')
  args = parser.parse_args()

  if args.clear:
    for path in args.files:
      with open(path, "r") as f:
        lines = f.read().splitlines()
      lines = remove_autogen(lines)
      lines = '\n'.join(lines) + '\n'
      print(lines)

      if not args.dryrun:
        print(f'File {path} instrumented')
        with open(path, 'w') as f:
          f.write(lines)
    exit(0)

  for path in args.files:
    reserve_manual_counters(path)

  all_blocks = []
  for path in args.files:
    all_blocks.append(autogen_counters(path))

  for path, blocks in zip(args.files, all_blocks):
    print()
    print('@' * (len(path) + 42))
    print('@' * 20, path, '@' * 20)
    print('@' * (len(path) + 42))
    print()

    lines = []
    for block in blocks:
      print('=' * 20, block._replace(lines=[]), '=' * 20)
      for line in block.lines:
        print(LINE_COLORS[line.line_type] + line.text + COLOR_RESET)
        lines.append(line.text)
    lines = '\n'.join(lines) + '\n'

    print('=' * 80)

    if not args.dryrun:
      print(f'File {path} instrumented')
      with open(path, 'w') as f:
        f.write(lines)
    else:
      print(f'Skip saving {path}')

  print('=' * 80)
  print(f'Remaining {len(g_available_counters)} counters')
  print(g_available_counters)
