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

ASM_COUNTER_SIZE = 96
g_available_counters = set(range(ASM_COUNTER_SIZE))

class InstType(enum.Enum):
  DISABLED = enum.auto()
  REGISTER_BITS = enum.auto()
  PRF_CNTS = enum.auto()

PRAGMA_REGEX = r"(// PRAGMA_COVERAGE:.+\n?)"
PRAGMA_REG_START = "// PRAGMA_COVERAGE: start autogen with register bits"
PRAGMA_REG_STOP = "// PRAGMA_COVERAGE: stop autogen with register bits"
PRAGMA_PRF_START = "// PRAGMA_COVERAGE: start autogen with prf counters"
PRAGMA_PRF_STOP = "// PRAGMA_COVERAGE: stop autogen with prf counters"
PRAGMA_SKIP_START = "// PRAGMA_COVERAGE: start block skip"
PRAGMA_SKIP_STOP = "// PRAGMA_COVERAGE: stop block skip"

ALL_PRAGMA = {
  PRAGMA_REG_START,
  PRAGMA_REG_STOP,
  PRAGMA_PRF_START,
  PRAGMA_PRF_STOP,
  PRAGMA_SKIP_START,
  PRAGMA_SKIP_STOP,
}

COUNTER_REGEX = r"COVERAGE_ASM_(AUTOGEN|MANUAL)_MARK_(REG|PRF)\((\w+),\s*(\d+)\)"

Line = namedtuple("Line", ["text", "line_type", "continuation", "counter"])

class LineType(enum.Enum):
  COMMENT = enum.auto()
  LABEL = enum.auto()
  BRANCH = enum.auto()
  TRAP = enum.auto()
  COUNTER = enum.auto()
  PRAGMA = enum.auto()
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
  LineType.OTHER: COLOR_RESET,
}

def parse_counter_mark(line):
  m = re.search(COUNTER_REGEX, line.strip())
  manual, inst_type, reg, off = m.groups()
  if inst_type == 'REG':
    return int(off) + (32 if reg == 's11' else 0)
  else:
    return int(off)

Block = namedtuple("Block", ["lines", "counter", "up", "down"])

def segment_basic_blocks(lines):
  """ locate start and end markers """
  blocks = [Block(lines=[], counter=None, up=False, down=False)]
  inside_comment_block = False
  inside_skip_block = False
  continuation_line_type = None
  for line in lines:
    if line.strip().startswith('/*'):
      inside_comment_block = True
    elif '*/' not in line:
      assert '/*' not in line, line

    line_type = None
    counter = None

    is_comment = line.strip().startswith('//')
    is_comment = is_comment or inside_comment_block
    is_comment = is_comment or inside_skip_block

    if continuation_line_type is not None:
      line_type = continuation_line_type
    elif is_comment:
      if line.strip().endswith('*/'):
        inside_comment_block = False
      else:
        assert '*/' not in line
      if line.strip() == PRAGMA_SKIP_START:
        inside_skip_block = True
      if line.strip() == PRAGMA_SKIP_STOP:
        inside_skip_block = False
      line_type = LineType.COMMENT
      if re.fullmatch(PRAGMA_REGEX, line.strip()):
        assert line.strip() in ALL_PRAGMA, f'Unknown coverage pragma: {line.strip()}'
        line_type = LineType.PRAGMA
    elif line.strip() == '':
      line_type = LineType.COMMENT
    elif line.strip().startswith('#'):
      line_type = LineType.COMMENT
    elif ':' in line:
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
    elif re.match(COUNTER_REGEX, line.strip()):
      line_type = LineType.COUNTER
      counter = parse_counter_mark(line.strip())
      blocks[-1] = blocks[-1]._replace(counter=counter)
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
    if line_type == LineType.PRAGMA:
      blocks.append(Block(lines=[], counter=None, up=True, down=True))

    blocks[-1].lines.append(Line(
      text=line,
      line_type=line_type,
      continuation=is_continuation,
      counter=counter,
    ))

    # create new basic block after branch / pragma
    if line_type in {LineType.BRANCH, LineType.TRAP}:
      blocks[-1] = blocks[-1]._replace(down=False)
      blocks.append(Block(lines=[], counter=None, up=False, down=True))
    if line_type == LineType.PRAGMA:
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


def get_next_counter(inst_type):
  assert g_available_counters, "Coverage counter exhausted"
  if inst_type == InstType.REGISTER_BITS:
    counter = min(g_available_counters)
    assert counter < 64, "Coverage counter exhausted"
  else:
    counter = max(g_available_counters)
  g_available_counters.remove(counter)
  return counter

def instrument_blocks(blocks):
  start_line = 1
  instrumented = []
  mode = InstType.DISABLED
  for block in blocks:
    block = copy.deepcopy(block)

    # handle pragma
    if block.lines[0].line_type == LineType.PRAGMA:
      pragma = block.lines[0].text.strip()
      if pragma == PRAGMA_REG_START:
        mode = InstType.REGISTER_BITS
      elif pragma == PRAGMA_PRF_START:
        mode = InstType.PRF_CNTS
      elif pragma == PRAGMA_REG_STOP:
        assert mode == InstType.REGISTER_BITS, mode
        mode = InstType.DISABLED
      elif pragma == PRAGMA_PRF_STOP:
        assert mode == InstType.PRF_CNTS, mode
        mode = InstType.DISABLED
      instrumented.append(block)
      start_line += len(block.lines)
      continue

    # skip the block if auto instrumentation is not enabled.
    if mode == InstType.DISABLED:
      instrumented.append(block)
      start_line += len(block.lines)
      continue

    # skip the block if it only contains comments.
    comment_types = {LineType.COMMENT, LineType.PRAGMA}
    if all(line.line_type in comment_types for line in block.lines):
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
      if line.line_type not in {LineType.COMMENT, LineType.LABEL}:
        break
      insert_idx += 1

    # insert instrumentation
    counter = get_next_counter(mode)
    block = block._replace(counter=counter)
    kind = 'REG' if mode == InstType.REGISTER_BITS else 'PRF'
    block.lines.insert(insert_idx, Line(
      f"  COVERAGE_ASM_AUTOGEN_MARK_{kind}(t6, {counter})",
      LineType.COUNTER,
      False,
      counter,
    ))

    instrumented.append(block)
    start_line += len(block.lines)
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

def reserve_manual_counters(path):
  with open(path, "r") as f:
    lines = f.read()

  for c in re.finditer(COUNTER_REGEX, lines):
    if c.group(1) != 'MANUAL':
      continue

    counter = parse_counter_mark(c.group(0))
    assert counter in g_available_counters, f"Counter reused in file {path}: {c.group(0)}"
    g_available_counters.remove(counter)


def autogen_counters(path):
  with open(path, "r") as f:
    lines = f.read().splitlines()
  lines = remove_autogen(lines)
  blocks = segment_basic_blocks(lines)
  blocks = instrument_blocks(blocks)
  blocks = propagate_counters(blocks)
  return blocks
