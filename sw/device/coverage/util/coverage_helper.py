import re
import json
import bisect
import itertools as it
import sys
import zipfile
import multiprocessing as mp
import numpy as np
from collections import namedtuple, defaultdict
from pathlib import Path


FileProfile = namedtuple('FileProfile', ['sf', 'fn', 'fnda', 'da'])
MISSING = FileProfile(sf='', fn={}, fnda={}, da={})

def parse_dis_file(lines):
  files = defaultdict(list)
  for m in re.finditer(r'^/proc/self/cwd/(.*):(\d+)', lines, re.M):
    path, lineno = m.groups()
    lineno = int(lineno)
    while path.startswith('./'):
      path = path.removeprefix('./')
    sf = 'SF:' + path
    files[sf].append(lineno)

  return {
    sf: FileProfile(
      sf=sf,
      fn={},
      fnda={},
      da=dict.fromkeys(lines, 1),
    )
    for sf, lines in files.items()
  }

Region = namedtuple('Region', 'LineStart,ColumnStart,LineEnd,ColumnEnd,ExecutionCount,FileID,ExpandedFileID,Kind')
Function = namedtuple('Function', 'sf,name,start,end,regions,files')

def parse_llvm_json(lines):
  cov = json.loads(lines)

  # Group function regions by source file
  function_by_sf = defaultdict(list)
  for info in cov['data'][0]['functions']:
    sf = 'SF:' + info['filenames'][0]

    regions = [Region(*r) for r in info['regions']]
    # Ensure the first region is the main function body.
    assert regions[0].Kind == 0, regions[0]
    assert regions[0].FileID == 0, regions[0]

    if regions[0].ExecutionCount == 0:
      # If the function is not reachable, all expansions should be unreachable
      # too.
      assert all(r.ExecutionCount == 0 for r in regions), regions
      continue

    assert all(r.Kind in {0, 1, 2, 3} for r in regions), regions
    func = Function(
      sf=sf,
      name=info['name'],
      start=regions[0].LineStart,
      end=regions[0].LineEnd,
      regions=regions,
      files=info['filenames'],
    )
    function_by_sf[sf].append(func)

  # Segment the source file by function regions
  result = {}
  for sf, funcs in function_by_sf.items():
    # Collect all boundary of the segments
    segments = []
    boundaries = []
    for i, f in enumerate(funcs):
      # (line, closing, func_id)
      boundaries.append((f.start, False, i))
      boundaries.append((f.end+1, True, i))
    boundaries.sort()

    # Find the functions overlapped with each segment
    last_line, active_funcs = -1, set()
    for line, group in it.groupby(boundaries, key=lambda x: x[0]):
      group = list(group)
      segments.append((last_line, line, [funcs[i] for i in active_funcs]))
      last_line = line
      for _, close, func_id in group:
        if close:
          active_funcs.remove(func_id)
        else:
          active_funcs.add(func_id)
    result[sf] = segments

  return result

# Use normal view coverage for the following files.
# e.g.
#   macro defined functions can't be detected by disassembly.
SKIP_DIS = {
  'SF:sw/device/silicon_creator/lib/manifest.h',
}

def expand_dis_region(dis, segments):
  keys = set(dis.keys()) & set(segments.keys())
  da_by_sf = defaultdict(set)
  fn_by_sf = defaultdict(dict)
  for sf in keys:
    if sf in SKIP_DIS:
      continue
    segs = segments[sf]
    for line in dis[sf].da:
      line = int(line)

      # Find the segment containing the `line`.
      idx = bisect.bisect_right(segs, (line, float('inf'), [])) -1
      if idx == -1:
        print('NOT FOUND', sf, idx, line, segs)
        continue
      start, end, funcs = segs[idx]
      if not (start <= line < end):
        print('RangeError', sf, idx, line, segs[idx])
        continue

      # Each instr should belong to at least one function.
      assert len(funcs) > 0, (sf, idx, line)

      # Add these functions and their dependencies.
      for func in funcs:
        # Add the function for function coverage.
        fn_by_sf[sf][func.name] = func.start

        # Add all the lines for line coverage.
        for r in func.regions:
          if r.Kind != 0 or r.ExecutionCount == 0:
            # Skip non-source or non-reachable regions
            continue
          assert r.ExpandedFileID == 0, 'ExpandedFileID with source region'
          f = 'SF:' + func.files[r.FileID]
          da_by_sf[f].update(range(r.LineStart, r.LineEnd+1))

  all_sf = set(da_by_sf.keys()) | set(fn_by_sf.keys())
  return {
    sf: FileProfile(
        sf=sf,
        fn=fn_by_sf[sf],
        fnda=dict.fromkeys(fn_by_sf[sf].keys(), 1),
        da=dict.fromkeys(da_by_sf[sf], 1),
    )
    for sf in all_sf
  }

def parse_single_file(lines):
  path = lines.pop().strip()
  assert path.startswith('SF:'), path

  profile = FileProfile(
    sf=path,
    fn={},
    fnda=defaultdict(int),
    da=defaultdict(int),
  )

  while True:
    line = lines.pop().strip()
    tag, params, *_ = line.split(':', 1) + ['']
    if tag == 'FN':
      lineno, name = params.split(',')
      profile.fn[name] = int(lineno)
    elif tag == 'FNDA':
      count, name = params.split(',')
      profile.fnda[name] += int(count)
    elif tag == 'BRDA':
      pass
      # raise NotImplementedError('BRDA is not supported yet')
    elif tag == 'DA':
      lineno, count, *_ = params.split(',')
      profile.da[int(lineno)] += int(count)
    elif tag in {'LH', 'LF', 'BRH', 'BRF', 'FNH', 'FNF'}:
      # These are summary lines, we don't care.
      pass
    elif tag == 'end_of_record':
      break
    else:
      raise ValueError(f'Unexpected line: {line}')

  assert set(profile.fn) == set(profile.fnda)

  return profile

def parse_lcov(lines):
  lines = lines[::-1]
  files = {}
  while len(lines):
    profile = parse_single_file(lines)
    files[profile.sf] = profile
  return files

def strip_discarded(coverage):
  stripped = {}
  for sf, base in coverage.items():
    # Keep functions that can be hit
    fnda = {n: c for n, c in base.fnda.items() if c > 0}
    fn = {n: l for n, l in base.fn.items() if n in fnda}

    # Keep lines that can be hit
    da = {l: c for l, c in base.da.items() if c > 0}

    if len(fnda) or len(da):
      stripped[sf] = FileProfile(
        sf=sf,
        fn=fn,
        fnda=fnda,
        da=da,
        )

  return stripped

def merge_inlined_copies(coverage):
  """
  Dedup static inlined copies

  e.g. asn1.c:bitfield_bit32_copy and bitfield.c:bitfield_bit32_copy
  """
  coverage = coverage.copy()
  for sf, cov in coverage.items():
    fn = defaultdict(int)
    for name, lineno in cov.fn.items():
      name = name.split(':')[-1]
      fn[name] = max(fn[name], lineno)
    fn = dict(fn)

    fnda = defaultdict(int)
    for name, count in cov.fnda.items():
      name = name.split(':')[-1]
      fnda[name] = max(fnda[name], count)
    fnda = dict(fnda)

    coverage[sf] = cov._replace(fnda=fnda, fn=fn)
  return coverage

def and_dict(a, b):
  keys = set(a.keys()) & set(b.keys())
  return {k: a[k] for k in keys}

def add_dict(a, b):
  keys = set(a.keys()) | set(b.keys())
  return {k: a.get(k, 0) + b.get(k, 0) for k in keys}

def and_coverage(a, b):
  keys = set(a.keys()) & set(b.keys())
  return {
    sf: FileProfile(
      sf=sf,
      fn=and_dict(a[sf].fn, b[sf].fn),
      fnda=and_dict(a[sf].fnda, b[sf].fnda),
      da=and_dict(a[sf].da, b[sf].da),
    )
    for sf in keys
  }

def or_dict(a, b):
  keys = set(a.keys()) | set(b.keys())
  return {k: a.get(k, b.get(k, None)) for k in keys}

def or_coverage(a, b):
  keys = set(a.keys()) | set(b.keys())
  a = defaultdict(lambda: MISSING, a)
  b = defaultdict(lambda: MISSING, b)
  return {
    sf: FileProfile(
      sf=sf,
      fn=or_dict(a[sf].fn, b[sf].fn),
      fnda=or_dict(a[sf].fnda, b[sf].fnda),
      da=or_dict(a[sf].da, b[sf].da),
    )
    for sf in keys
  }

def add_coverage(a, b):
  keys = set(a.keys()) | set(b.keys())
  a = defaultdict(lambda: MISSING, a)
  b = defaultdict(lambda: MISSING, b)
  return {
    sf: FileProfile(
      sf=sf,
      fn=or_dict(a[sf].fn, b[sf].fn),
      fnda=add_dict(a[sf].fnda, b[sf].fnda),
      da=add_dict(a[sf].da, b[sf].da),
    )
    for sf in keys
  }

def filter_dict(a, b):
  return {k: a.get(k, 0) for k in b.keys()}

def filter_coverage(a, b):
  a = defaultdict(lambda: MISSING, a)
  return {
    sf: FileProfile(
      sf=sf,
      fn=b[sf].fn,
      fnda=filter_dict(a[sf].fnda, b[sf].fnda),
      da=filter_dict(a[sf].da, b[sf].da),
    )
    for sf in b.keys()
  }

def generate_lcov(coverage):
  output = []
  for sf, cov in coverage.items():
    # skip pre-generated constant headers.
    if sf.startswith('SF:hw/top_earlgrey/sw/autogen/top_earlgrey.h'):
      continue

    # skip hardware autogen constant headers.
    # e.g. SF:bazel-out/k8-fastbuild-ST-98e8226209fe/bin/hw/...
    if re.match(r'^SF:bazel-out/[^/]+/bin/hw/', sf):
      continue

    output.append(sf + '\n')
    for name, lineno in cov.fn.items():
      output.append(f'FN:{lineno},{name}\n')

    fnh = 0
    for name, count in cov.fnda.items():
      if count > 0:
        fnh += 1
      output.append(f'FNDA:{count},{name}\n')
    output.append(f'FNH:{fnh}\n')
    output.append(f'FNF:{len(cov.fnda)}\n')

    lh = 0
    for lineno, count in cov.da.items():
      if count > 0:
        lh += 1
      output.append(f'DA:{lineno},{count}\n')
    output.append(f'LH:{lh}\n')
    output.append(f'LF:{len(cov.da)}\n')

    output.append('end_of_record\n')
  return output

TestLcov = namedtuple('TestLcov', 'name,path,duration,coverage')
def iter_lcov_paths(lcov_files_path):
  with open(lcov_files_path) as files:
    files = sorted(map(Path, files.read().splitlines()))
    files = [p for p in files if p.name == 'coverage.dat']
    for i, path in enumerate(files):
      # Get the path after the first parent named 'testlogs'
      parts = path.parent.parts
      testlogs_index = parts.index('testlogs')
      test_dir = '/'.join(parts[testlogs_index + 1:-1])
      test_name = f'//{test_dir}:{parts[-1]}'

      print(f'Loading [{i+1} / {len(files)}] {test_name}', file=sys.stderr)
      duration = 0
      with open(path.parent / 'test.xml') as f:
        for m in re.findall(r'status="run" duration="(\d+)"', f.read()):
          duration += int(m)

      yield TestLcov(test_name, path, duration, None)

def _process_lcov_contents(lcov):
  coverage = parse_lcov(lcov.coverage.decode().splitlines())
  coverage = merge_inlined_copies(coverage)
  return lcov._replace(coverage=coverage)

def iter_lcov_files(lcov_files_path):
  for lcov in iter_lcov_paths(lcov_files_path):
    with open(lcov.path, 'rb') as f:
      lcov = lcov._replace(coverage=f.read())
    yield _process_lcov_contents(lcov)

def _iter_raw_lcov_contents(lcov_files_path):
  for lcov in iter_lcov_paths(lcov_files_path):
    test_dir = lcov.path.parent / 'test.outputs'
    for dat in test_dir.glob('*.dat'):
      yield lcov._replace(coverage=dat.read_bytes())

    # The output could be zipped when the folllowing flag is set.
    # https://bazel.build/reference/command-line-reference#flag--zip_undeclared_test_outputs
    test_zip = test_dir / 'outputs.zip'
    if test_zip.exists(): # e.g. skip unit tests
      with zipfile.ZipFile(test_zip, 'r') as zip:
        for name in zip.namelist():
          if name.endswith('.dat'):
            with zip.open(name) as f:
              yield lcov._replace(coverage=f.read())

def iter_raw_lcov_files(lcov_files_path):
  with mp.Pool() as pool:
    iterator = _iter_raw_lcov_contents(lcov_files_path)
    for lcov in pool.imap_unordered(_process_lcov_contents, iterator):
      yield lcov

def collect_single_vector(coverage, sf_keys):
  keys, values = [], []
  for sf in sf_keys:
    cov = coverage[sf]
    for name, count in sorted(cov.fnda.items()):
      keys.append(f'{sf}:fn:{name}')
      values.append(1 if count > 0 else 0)
    for lineno, count in sorted(cov.da.items()):
      keys.append(f'{sf}:line:{lineno}')
      values.append(1 if count > 0 else 0)
  return keys, np.array(values, dtype=int)

def collect_test_vectors(view_path, lcov_files_path):
  print(f'Loading {view_path}', file=sys.stderr)
  with open(view_path) as f:
    view = parse_lcov(f.readlines())
    view = merge_inlined_copies(view)

  sf_keys = sorted(view.keys())

  # ASM coverage are expanded after bazel coverage, and should be ignored
  # when calculate the min cover set.
  sf_keys = [k for k in sf_keys if not k.endswith('.S')]

  # TODO: Remove this workaround once coverage for generated files is properly handled.
  # The current lcov iterator does not load coverage for generated files,
  # while the min set already has full coverage on them.
  sf_keys = [k for k in sf_keys if not k.startswith('SF:bazel-out/')]

  view_keys, view_values = collect_single_vector(view, sf_keys)

  tests = {}
  for test in iter_lcov_files(lcov_files_path):
    coverage = test.coverage
    coverage = filter_coverage(coverage, view)
    coverage_keys, coverage_values = collect_single_vector(coverage, sf_keys)
    assert coverage_keys == view_keys
    tests[test.name] = coverage_values, test.duration

  test_names, test_values = zip(*sorted(tests.items()))
  test_values, test_durations = zip(*test_values)
  test_values = np.stack(test_values)
  test_durations = np.array(test_durations, dtype=float)
  expected_view = (test_values.sum(0) > 0).astype(int)
  if not (expected_view == view_values).all():
    for k, v in zip(view_keys, expected_view != view_values):
      if v:
        print(k, file=sys.stderr)
    print('View values mismatch', file=sys.stderr)
    exit(-1)

  test_values = test_values[:, view_values > 0]
  view_values = view_values[view_values > 0]
  assert (view_values == 1).all()
  assert (test_values.sum(0) > 0).all()

  assert len(test_names) == len(test_values)

  return test_names, test_values, test_durations

def add_tests(dst, src):
  for key, value in src.items():
    if not dst.get(key, False):
      dst[key] = value


def extract_tests(path):
  tests = {}
  with open(path) as f:
    for line in f:
      line = line.strip()
      enabled = True
      if line.startswith('#'):
        enabled = False
        line = line.lstrip('#').strip()
      for quote in ['"', "'"]:
        if line.startswith(quote) and line.endswith(quote):
          line = line[1:-1]
      if line.startswith('//'):
        tests[line] = enabled
  return dict(sorted(tests.items()))

def load_view_zip(zip_path, use_disassembly):
  print(f'Loading {zip_path}', file=sys.stderr)
  with zipfile.ZipFile(zip_path, 'r') as view_zip:
    with view_zip.open('coverage.dat', 'r') as f:
      view = parse_lcov(f.read().decode().splitlines())
    # Ignore objects that are discarded in the final firmware
    view = strip_discarded(view)

    if use_disassembly:
      with view_zip.open('test.dis', 'r') as f:
        compiled = parse_dis_file(f.read().decode())
      with view_zip.open('coverage.json', 'r') as f:
        segments = parse_llvm_json(f.read().decode())
      compiled = expand_dis_region(compiled, segments)

      # Use normal view coverage for these files.
      for sf in SKIP_DIS:
        compiled[sf] = view[sf]

      # Compiled functions lines includes comments,
      # apply a and filter here to remove them.
      view = and_coverage(compiled, view)
  return view
