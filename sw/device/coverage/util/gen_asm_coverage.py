import re
import enum
import copy
import argparse
from collections import namedtuple

from coverage_helper import (
  parse_lcov,
  MISSING
)

from asm_helper import (
  LineType,
  ASM_FILES,
  ASM_COUNTER_SIZE,
  LINE_COLORS,
  COMMENT_TYPES,
  COLOR_RESET,
  segment_basic_blocks,
  propagate_counters
)

COUNTER_FILE = 'SF:sw/device/coverage/asm_counters.c'

if __name__ == '__main__':
  parser = argparse.ArgumentParser(description='Filter related coverage based on a baseline.')
  parser.add_argument('--coverage', type=str, help='Path to the coverage file to filter.')
  parser.add_argument('--print_blocks', action='store_true', help='Print basic block debug info.')
  parser.add_argument('--append', action='store_true', help='Append after the input coverage.')
  parser.add_argument('--output', type=str, help='Path to the output file.')
  args = parser.parse_args()

  with open(args.coverage, 'r') as f:
    coverage = parse_lcov(f.readlines())

  if COUNTER_FILE not in coverage:
    print('WARNING: The counter status `asm_counters.c` has no coverage')
    da = {i: 0 for i in range(ASM_COUNTER_SIZE)}
  else:
    da = dict(coverage[COUNTER_FILE].da)
    da = {i: da[i+1] for i in range(ASM_COUNTER_SIZE)}
  da[None] = 0

  all_blocks = []
  for path in ASM_FILES:
    with open(path, "r") as f:
      lines = f.read().splitlines()
    blocks = segment_basic_blocks(lines)
    blocks = propagate_counters(blocks)
    all_blocks.append(blocks)

  if args.append:
    with open(args.coverage, 'r') as f:
      input_coverage = f.read()

  with open(args.output, 'w') as dat:
    if args.append:
      dat.write(input_coverage)

    for path, blocks in zip(ASM_FILES, all_blocks):
      dat.write(f'SF:{path}\n')
      if args.print_blocks:
        print()
        print('@' * (len(path) + 42))
        print('@' * 20, path, '@' * 20)
        print('@' * (len(path) + 42))
        print()

      lines = []
      lineno = 1
      functions = {}
      for block in blocks:
        hit = da[block.counter]
        if args.print_blocks:
          print('=' * 20, block._replace(lines=hit), '=' * 20)
        for line in block.lines:
          if args.print_blocks:
            print(LINE_COLORS[line.line_type] + line.text + COLOR_RESET)
          if line.line_type not in COMMENT_TYPES:
            dat.write(f'DA:{lineno},{hit}\n')
          if line.line_type == LineType.FUNCTYPE:
            func = line.args
            if '_interrupt_vector' not in func:
              functions[func] = (None, None)
          if line.line_type == LineType.LABEL:
            func = line.text.strip().rstrip(':')
            if func in functions:
              functions[func] = (hit, lineno)
          lineno += 1

      if args.print_blocks:
          print('=' * 20, block._replace(lines=hit), '=' * 20)
          print('Function coverage')
          print(functions)
      for func, (hit, lineno) in functions.items():
        assert hit is not None, f"Function label {func} not found"
        dat.write(f'FN:{lineno},{func}\n')
        dat.write(f'FNDA:{hit},{func}\n')

      dat.write("end_of_record\n")
