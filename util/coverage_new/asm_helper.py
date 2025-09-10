import re
import enum
import copy
from collections import namedtuple

ASM_FILES = [
  # Pre crt initialization assembly
  "sw/device/lib/crt/crt.S",
  "sw/device/silicon_creator/rom/rom_epmp_init.S",
  "sw/device/silicon_creator/rom/rom_start.S",
  "sw/device/silicon_creator/lib/irq_asm.S",

  # Post crt initialization assembly
  "sw/device/silicon_creator/rom_ext/imm_section/imm_section_start.S",
  "sw/device/silicon_creator/rom_ext/rom_ext_start.S",
  "sw/device/silicon_creator/lib/flash_exc_handler.S",
]

PRAGMA_REGEX = r"(// *PRAGMA_COVERAGE:.+\n?)"
PRAGMA_SECTION_REGEX = r"// *PRAGMA_COVERAGE: *section\((.+?)\)\n?"
PRAGMA_AUTOGEN_START = "// PRAGMA_COVERAGE: start autogen"
PRAGMA_AUTOGEN_STOP = "// PRAGMA_COVERAGE: stop autogen"
PRAGMA_SKIP_START = "// PRAGMA_COVERAGE: start block skip"
PRAGMA_SKIP_STOP = "// PRAGMA_COVERAGE: stop block skip"

ALL_PRAGMA = {
  PRAGMA_AUTOGEN_START,
  PRAGMA_AUTOGEN_STOP,
  PRAGMA_SKIP_START,
  PRAGMA_SKIP_STOP,
}

FUNCTYPE_REGEX = r"\.type +(\w+), *@function"
SIZE_REGEX = r"\.size +(\w+), *\. *- *(\w+)"
SECTION_REGEX = r"\.section +[\w\.]+(,.*)?"
COUNTER_REGEX = r"COVERAGE_ASM_(AUTOGEN|MANUAL)_MARK\((\w+),\s*(\d+)\)"

Line = namedtuple("Line", ["text", "lineno", "line_type", "continuation", "args"])

class LineType(enum.Enum):
  COMMENT = enum.auto()
  LABEL = enum.auto()
  BRANCH = enum.auto()
  TRAP = enum.auto()
  COUNTER = enum.auto()
  PRAGMA = enum.auto()
  FUNCTYPE = enum.auto()
  SIZE = enum.auto()
  OTHER = enum.auto()

COLOR_RED = "\033[31m"
COLOR_GREEN = "\033[32m"
COLOR_YELLOW = "\033[33m"
COLOR_BLUE = "\033[34m"
COLOR_MAGENTA = "\033[35m"
COLOR_CYAN = "\033[36m"
COLOR_GREY = "\033[90m"
COLOR_RESET = "\033[0m"


LINE_COLORS = {
  LineType.COMMENT: COLOR_MAGENTA,
  LineType.LABEL: COLOR_CYAN,
  LineType.BRANCH: COLOR_RED,
  LineType.TRAP: COLOR_RED,
  LineType.COUNTER: COLOR_GREEN,
  LineType.PRAGMA: COLOR_MAGENTA,
  LineType.FUNCTYPE: COLOR_BLUE,
  LineType.SIZE: COLOR_BLUE,
  LineType.OTHER: COLOR_RESET,
}

COMMENT_TYPES = {
  LineType.COMMENT,
  LineType.FUNCTYPE,
  LineType.SIZE,
  LineType.PRAGMA
}

def parse_counter_mark(line):
  m = re.search(COUNTER_REGEX, line.strip())
  manual, reg, off = m.groups()
  return int(off)

Block = namedtuple("Block", ["lines", "counter", "up", "down"])

def segment_basic_blocks(lines):
  """ locate start and end markers """
  blocks = [Block(lines=[], counter=None, up=False, down=False)]
  inside_comment_block = False
  inside_skip_block = False
  inside_code_section = False
  continuation_line_type = None
  for lineno, line in enumerate(lines):
    if line.strip().startswith('/*'):
      inside_comment_block = True
    elif '*/' not in line:
      assert '/*' not in line, line

    if line.strip() == PRAGMA_SKIP_START:
      inside_skip_block = True
    if line.strip() == PRAGMA_SKIP_STOP:
      inside_skip_block = False

    if (m := re.match(SECTION_REGEX, line.strip())):
      inside_code_section = 'ax' in (m.group(1) or '')

    line_type = None
    args = None

    is_comment = line.strip().startswith('//')
    is_comment = is_comment or inside_comment_block
    is_comment = is_comment or inside_skip_block
    is_comment = is_comment or not inside_code_section

    if continuation_line_type is not None:
      line_type = continuation_line_type
    elif re.fullmatch(PRAGMA_SECTION_REGEX, line.strip()):
      line_type = LineType.PRAGMA
    elif re.search(PRAGMA_REGEX, line.strip()):
      assert line.strip() in ALL_PRAGMA, f'Unknown coverage pragma: {line.strip()}'
      line_type = LineType.PRAGMA
    elif is_comment:
      if line.strip().endswith('*/'):
        inside_comment_block = False
      else:
        assert '*/' not in line
      line_type = LineType.COMMENT
    elif line.strip() == '':
      line_type = LineType.COMMENT
    elif line.strip().startswith('#'):
      line_type = LineType.COMMENT
    elif ':' in line:
      assert line.strip().endswith(':'), "Got extra contents after label"
      args = line.strip().removesuffix(':')
      line_type = LineType.LABEL
    elif line.strip().startswith('LABEL_FOR_TEST'):
      # Treat LABEL_FOR_TEST as a comment. These are special markers for
      # testing with OpenOCD and do not impact the ROM's execution flow.
      line_type = LineType.COMMENT
    elif line.strip().startswith('b'):
      line_type = LineType.BRANCH
    elif line.strip().startswith('j'):
      line_type = LineType.BRANCH
    elif line.strip().startswith('tail'):
      line_type = LineType.BRANCH
    elif line.strip().startswith('ret'):
      line_type = LineType.BRANCH
    elif line.strip().startswith('mret'):
      line_type = LineType.BRANCH
    elif line.strip().startswith('unimp'):
      line_type = LineType.TRAP
    elif (m := re.match(COUNTER_REGEX, line.strip())):
      line_type = LineType.COUNTER
      counter = parse_counter_mark(line.strip())
      blocks[-1] = blocks[-1]._replace(counter=counter)
      args = counter
    elif (m := re.match(FUNCTYPE_REGEX, line.strip())):
      line_type = LineType.FUNCTYPE
      args = m.group(1).strip()
    elif (m := re.match(SIZE_REGEX, line.strip())):
      assert m.group(1) == m.group(2), f"Got different label for size statement {m.groups()}"
      line_type = LineType.SIZE
      args = m.group(1).strip()
    elif line.strip().startswith('.'):
      line_type = LineType.COMMENT
    else:
      line_type = LineType.OTHER

    is_continuation = continuation_line_type is not None

    if line.strip().endswith('\\'):
      continuation_line_type = line_type
    else:
      continuation_line_type = None

    # create new basic block for new label / pragma
    if line_type == LineType.LABEL:
      blocks.append(Block(lines=[], counter=None, up=False, down=True))
    if line_type in {LineType.PRAGMA, LineType.SIZE}:
      blocks.append(Block(lines=[], counter=None, up=True, down=True))

    blocks[-1].lines.append(Line(
      text=line,
      lineno=lineno,
      line_type=line_type,
      continuation=is_continuation,
      args=args,
    ))

    # create new basic block after branch / pragma
    if line_type in {LineType.BRANCH, LineType.TRAP}:
      blocks[-1] = blocks[-1]._replace(down=False)
      blocks.append(Block(lines=[], counter=None, up=False, down=True))
    if line_type in {LineType.PRAGMA, LineType.SIZE}:
      blocks.append(Block(lines=[], counter=None, up=True, down=True))

  # merge trap chain
  blocks = [b for b in blocks if len(b.lines)]
  last_trap = None
  for block in blocks:
    line = block.lines[-1]
    is_trap = line.line_type == LineType.TRAP
    code_lines = sum(l.line_type != LineType.COMMENT for l in block.lines)
    if last_trap is not None and code_lines == 1 and is_trap:
      last_trap.lines.extend(block.lines)
      block.lines.clear()
      continue
    last_trap = block if is_trap else None

  blocks = [b for b in blocks if len(b.lines)]
  return blocks

def is_autogen(line):
  if line.strip().startswith('COVERAGE_ASM_AUTOGEN_'):
    return True
  return False

def remove_autogen(lines):
  return [l for l in lines if not is_autogen(l)]


def get_max_counter(blocks):
  counters = []
  for block in blocks:
    for line in block.lines:
      if line.line_type == LineType.COUNTER:
        counters.append(line.args)
  return max(counters, default=-1)

def instrument_blocks(blocks):
  counter = get_max_counter(blocks) + 1

  start_line = 1
  instrumented = []
  autogen_enabled = False
  for block in blocks:
    block = copy.deepcopy(block)

    # handle pragma
    if block.lines[0].line_type == LineType.PRAGMA:
      pragma = block.lines[0].text.strip()
      if pragma == PRAGMA_AUTOGEN_START:
        autogen_enabled = True
      elif pragma == PRAGMA_AUTOGEN_STOP:
        autogen_enabled = False
      instrumented.append(block)
      start_line += len(block.lines)
      continue

    # skip the block if auto instrumentation is not enabled.
    if not autogen_enabled:
      instrumented.append(block)
      start_line += len(block.lines)
      continue

    # skip the block if it only contains comments.
    if all(line.line_type in COMMENT_TYPES for line in block.lines):
      instrumented.append(block)
      start_line += len(block.lines)
      continue

    # skip the block if instrumented.
    if block.counter is not None:
      instrumented.append(block)
      start_line += len(block.lines)
      continue

    # go over comments and labels
    insert_idx = 0
    for line in block.lines:
      if line.line_type not in COMMENT_TYPES | {LineType.LABEL}:
        break
      insert_idx += 1

    # insert instrumentation
    block = block._replace(counter=counter)
    block.lines.insert(insert_idx, Line(
      text=f"  COVERAGE_ASM_AUTOGEN_MARK(t6, {counter})",
      lineno=None,
      line_type=LineType.COUNTER,
      continuation=False,
      args=counter,
    ))

    instrumented.append(block)
    start_line += len(block.lines)
    counter += 1
  return instrumented

def propagate_counters(blocks):
  blocks = copy.deepcopy(blocks)

  last_counter = None
  for i in range(len(blocks)):
    if blocks[i].counter is None and last_counter is not None:
      blocks[i] = blocks[i]._replace(counter = last_counter)
    last_counter = blocks[i].counter if blocks[i].down else None

  last_counter = None
  for i in range(len(blocks)-1, -1, -1):
    if blocks[i].counter is None and last_counter is not None:
      blocks[i] = blocks[i]._replace(counter = last_counter)
    last_counter = blocks[i].counter if blocks[i].up else None

  return blocks

def autogen_counters(path):
  with open(path, "r") as f:
    lines = f.read().splitlines()
  lines = remove_autogen(lines)
  blocks = segment_basic_blocks(lines)
  blocks = instrument_blocks(blocks)
  blocks = propagate_counters(blocks)
  return blocks
