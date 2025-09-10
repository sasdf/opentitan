import argparse
import enum
import hashlib
import re
import textwrap
import zlib

from collections import namedtuple
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple

try:
    import sw.device.coverage.util.asm_helper as asm
except ImportError:
    import asm_helper as asm


LineMapping = namedtuple('LineMapping', ['counter', 'lineno', 'cols'])


# https://llvm.org/docs/CoverageMappingFormat.html#id3
# Special counter expression for uncovered code.
COUNTER_UNCOVERED: int = 0 << 3


def collect_functions(blocks: List[asm.Block]) -> Tuple[Dict[str, List[asm.Block]], List[asm.Block]]:
    """Collects function names and groups basic blocks into functions.

    Basic blocks not assigned to any function are grouped into `global_code`.

    Args:
        blocks: A list of `Block` objects representing basic blocks.

    Returns:
        A tuple containing:
        - A dictionary where keys are function names (str) and values are lists
          of `Block` objects belonging to that function.
        - A list of `Block` objects that do not belong to any specific function.
    """
    # Collect function names
    func_names: Set[str] = set()
    for blockidx, block in enumerate(blocks):
        for line in block.lines:
            if line.line_type == asm.LineType.FUNCTYPE:
                func_names.add(line.args.strip())

    # Group basic blocks into functions.
    # Basic blocks not assigned to any function are grouped into global_code.
    global_code: List[asm.Block] = []
    current: Optional[str] = None
    functions: Dict[str, List[asm.Block]] = {}
    for blockidx, block in enumerate(blocks):
        if not len(block.lines):
            continue

        line: asm.Line = block.lines[0]
        if line.line_type == asm.LineType.LABEL and line.args in func_names:
            assert current is None, "Got overlapped functions"
            current = line.args.strip()
            functions[current] = []

        if current is not None:
            functions[current].append(block)
        else:
            global_code.append(block)

        if line.line_type == asm.LineType.SIZE and line.args == current:
            assert block.counter is None, "Size directive should not be reachable"
            current = None

    return functions, global_code


def collect_mappings(blocks: List[asm.Block]) -> List[LineMapping]:
    """Collects line mappings for coverage based on the provided basic blocks.

    Each mapping includes the counter, line number, and column width.

    Args:
        blocks: A list of `Block` objects.

    Returns:
        A list of `LineMapping` namedtuples.
    """
    mappings: List[LineMapping] = []
    for block in blocks:
        if not len(block.lines):
            continue

        if block.counter is None:
            counter: int = COUNTER_UNCOVERED
        else:
            # https://llvm.org/docs/CoverageMappingFormat.html#id3
            # Tag 1 for references to the profile instrumentation counter.
            counter = (block.counter << 2) | 0x01

        for line in block.lines:
            if line.line_type not in asm.COMMENT_TYPES:
                mappings.append(LineMapping(
                    counter=counter,
                    lineno=line.lineno,
                    cols=len(line.text) + 1,
                ))
    return mappings


def encode_leb128(value: int) -> bytes:
    """Encodes an integer into LEB128 format.

    Args:
        value: The integer to encode.

    Returns:
        The LEB128 encoded bytes.
    """
    result = bytearray()
    while True:
        byte: int = value & 0x7f
        value >>= 7
        if value:
            byte |= 0x80
        result.append(byte)
        if not value:
            break
    return bytes(result)


def encode_leb128_array(values: List[int]) -> bytes:
    """Encodes a list of integers into a sequence of LEB128 encoded bytes.

    Args:
        values: A list of integers to encode.

    Returns:
        The concatenated LEB128 encoded bytes for all values.
    """
    return b''.join(map(encode_leb128, values))


def encode_regions(mappings: List[LineMapping]) -> bytes:
    """Encodes a list of line mappings into LLVM coverage mapping regions format.

    Args:
        mappings: A list of `LineMapping` namedtuples.

    Returns:
        The encoded regions as bytes.
    """
    # https://llvm.org/docs/CoverageMappingFormat.html#encoding
    out: List[int] = [
        1, # num_file_id
        1, # file_id_0
        0, # num_counter_expr,
        len(mappings),
    ]
    last_line: int = -1
    for line in mappings:
        assert line.lineno >= last_line
        out.extend([
            line.counter,
            line.lineno - last_line,  # delta line start
            1,  # column start
            0,  # lines
            line.cols + 1,  # column end
        ])
        last_line = line.lineno

    return encode_leb128_array(out)


def encode_filepath(path: Path) -> bytes:
    """Encodes a file path into LLVM coverage mapping file path format.

    Args:
        path: The `Path` object representing the file.

    Returns:
        The encoded file path as bytes.
    """

    # The first element is the compilation directory, which Bazel sets to
    # `/proc/self/cwd`.
    paths = [b'/proc/self/cwd', str(path).encode()]
    encoded = b''.join(encode_leb128(len(s)) + s for s in paths)
    compressed = zlib.compress(encoded, level=9)
    headers = [2, len(encoded), len(compressed)]
    return encode_leb128_array(headers) + compressed


def encode_prf_names(names: List[str]) -> bytes:
    """Encodes a list of profile names into LLVM profile names format.

    Args:
        names: A list of strings representing profile names.

    Returns:
        The encoded profile names as bytes.
    """
    encoded = '\x01'.join(names).encode()
    compressed = zlib.compress(encoded, level=9)
    headers = [len(encoded), len(compressed)]
    return encode_leb128_array(headers) + compressed


def encode_oct(inp: bytes) -> str:
    """Encodes bytes into an octal string representation suitable for assembly.

    Args:
        inp: The input bytes.

    Returns:
        A string with octal escapes, enclosed in double quotes.
    """
    return '"' + ''.join(f'\\{b:03o}' for b in inp) + '"'


def get_hash(input_bytes: bytes) -> int:
    """Calculates the MD5 hash of input bytes and returns the first 8 bytes as an integer.

    Args:
        input_bytes: The bytes to hash.

    Returns:
        An integer representing the first 8 bytes of the MD5 hash (little-endian).
    """
    hash_digest = hashlib.md5(input_bytes).digest()
    return int.from_bytes(hash_digest[:8], 'little')


def main(args: argparse.Namespace) -> None:
    assert len(args.input) == len(args.output), "Number of inputs and outputs should be matched"

    for inp, out in zip(args.input, args.output):
        counter_sections = re.findall(asm.PRAGMA_SECTION_REGEX, inp.read_text())
        assert len(counter_sections) == 1, "Coverage counter section pragma should be specified once"
        counter_section = counter_sections[0].strip()
        assert counter_section, "Counter section should not be empty"

        with open(inp, "r") as f:
            lines = f.read().splitlines()
        blocks: List[asm.Block] = asm.segment_basic_blocks(lines)
        blocks = asm.propagate_counters(blocks)
        counter_size = asm.get_max_counter(blocks) + 1

        covmap = encode_filepath(inp)
        covmap_hash = get_hash(covmap)
        unique_name = f'{inp.stem}_{covmap_hash:016X}u'

        functions, global_code = collect_functions(blocks)

        all_mappings: Dict[str, List[LineMapping]] = {}
        for name, func_blocks in functions.items():
            mappings = collect_mappings(func_blocks)
            if mappings:
                all_mappings[name] = mappings
            else:
                print('WARN: Empty function ', name)

        global_mappings = collect_mappings(global_code)
        if global_mappings:
            print("WARN: Some instructions are not assigned to a function.")
            all_mappings[unique_name] = global_mappings

        prf_names: bytes = encode_prf_names(list(all_mappings.keys()))

        # The structures are related to the compiler version and may require
        # updates if the compiler changes. These templates are adapted from
        # the assembly output of `clang` when compiling a simple C file with
        # coverage instrumentation enabled.
        #
        # $ /path/to/lowrisc/toolchain/clang -S test.c \
        #     -fprofile-instr-generate -fcoverage-mapping \
        #     -mllvm --enable-single-byte-coverage \
        with open(out, 'w') as outfile:
            outfile.write(inp.read_text())

            # https://llvm.org/docs/InstrProfileFormat.html#names
            # Counters are shared between all functions.
            outfile.write(textwrap.dedent(f"""
                .type    .L__asm_profc,@object
                .section {counter_section},"aGw",@progbits,__asm_profc_{unique_name}
            .L__asm_profc:
                .zero    {counter_size}, 255
                .size    .L__asm_profc, {counter_size}
            """))

            # https://llvm.org/docs/InstrProfileFormat.html#names
            outfile.write(textwrap.dedent(f"""
                .type    .L__llvm_prf_nm,@object
                .section __llvm_prf_names,"aR",@progbits
            .L__llvm_prf_nm:
                .ascii   {encode_oct(prf_names)}
                .size    .L__llvm_prf_nm, {len(prf_names)}
            """))

            # https://llvm.org/docs/CoverageMappingFormat.html#coverage-mapping-header
            outfile.write(textwrap.dedent(f"""
                .type    .L__llvm_coverage_mapping,@object
                .section __llvm_covmap,"R",@progbits
                .p2align 3, 0x0
            .L__llvm_coverage_mapping:
                .word    0  /* Always zero */
                .word    {len(covmap)}
                .word    0  /* Always zero */
                .word    5  /* Coverage mapping format version */
                .ascii   {encode_oct(covmap)}
                .size    .L__llvm_coverage_mapping, {len(covmap) + 16}
            """))

            for name, mappings in all_mappings.items():
                name_hash = get_hash(name.encode())

                covrec = encode_regions(mappings)

                # https://llvm.org/docs/InstrProfileFormat.html#profile-metadata
                outfile.write(textwrap.dedent(f"""
                /* Function {name} */
                    .type    .L__asm_profd_{name},@object
                    .section __llvm_prf_data,"aGw",@progbits,__asm_profc_{unique_name}
                    .p2align 3
                .L__asm_profd_{name}:
                    .quad    0x{name_hash:016X}
                    .quad    31337  /* Unused structural hash */
                    .word    .L__asm_profc - .L__asm_profd_{name}
                    .word    0
                    .word    0
                    .word    {counter_size}
                    .zero    4
                    .zero    4
                    .size    .L__asm_profd_{name}, 40
                """))

                # https://llvm.org/docs/CoverageMappingFormat.html#function-record
                outfile.write(textwrap.dedent(f"""
                    .hidden  __covrec_{name_hash:016X}u
                    .type    __covrec_{name_hash:016X}u,@object
                    .section __llvm_covfun,"GR",@progbits,__covrec_{name_hash:016X}u,comdat
                    .weak    __covrec_{name_hash:016X}u
                    .p2align 3, 0x0
                __covrec_{name_hash:016X}u:
                    .quad    0x{name_hash:016X}
                    .word    {len(covrec)}
                    .quad    31337  /* Unused structural hash */
                    .quad    0x{covmap_hash:016X}
                    .ascii   {encode_oct(covrec)}
                    .size    __covrec_{name_hash:016X}u, {len(covrec) + 28}
                """))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='emit LLVM coverage mapping data.')
    parser.add_argument('--input', nargs='+', type=Path, help='Input ASM file to be process')
    parser.add_argument('--output', nargs='+', type=Path, help='Output ASM file with coverage mapping appended')
    args = parser.parse_args()
    main(args)
