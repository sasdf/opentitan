import json
import sys
import subprocess
import argparse

"""
This script lists all source files (.c, .h, .s, .inc) that are inputs to a specified Bazel target (an ELF file).
The target is provided as a command-line argument.
It uses `bazel aquery` to get dependency information, then parses the JSON output to trace back to the original source files.
It handles nested dependency sets and resolves artifact and path IDs.

Example: python3 list_sources.py //sw/device/silicon_creator/rom_ext:rom_ext_dice_cwt_slot_virtual
"""

parser = argparse.ArgumentParser(description="List source files for a Bazel target.")
parser.add_argument(
    "bazel_label",
    help="The Bazel target label (e.g., //sw/device/silicon_creator/rom_ext:rom_ext_dice_cwt_slot_virtual)")
parser.add_argument(
    "--pattern",
    help="A pattern to filter the final list of ELF targets (e.g., '_silicon_creator.elf').")
args = parser.parse_args()

proc = subprocess.run([
      './bazelisk.sh', 'cquery', args.bazel_label,
  ], stdout=subprocess.PIPE, check=True)
targets = proc.stdout.decode().splitlines()
targets = set(t for t in targets if t.endswith('.elf'))
if args.pattern is not None:
  targets = set(t for t in targets if args.pattern in t)

proc = subprocess.run([
      './bazelisk.sh', 'aquery', f'deps({args.bazel_label})', '--output=jsonproto',
  ], stdout=subprocess.PIPE, check=True)
data = json.loads(proc.stdout.decode())

actions = data['actions']
artifacts = {e['id']: e for e in data['artifacts']}
pathFragments = {e['id']: e for e in data['pathFragments']}
depSetOfFiles = {e['id']: e for e in data['depSetOfFiles']}

actionsByOutput = {}
for a in actions:
  for o in a.get('outputIds', []):
    if o in actionsByOutput and actionsByOutput[o] != a:
      print('Failed:', o)
      exit(-1)
    actionsByOutput[o] = a


# fragment_id -> path str
def get_frag_path(frag_id):
  result = []
  while True:
    e = pathFragments[frag_id]
    result.append(e['label'])
    if 'parentId' not in e:
      break
    frag_id = e['parentId']
  return '/'.join(result[::-1])


# artifact_id -> path str
def get_artifact_path(artifact_id):
  return get_frag_path(artifacts[artifact_id]['pathFragmentId'])

# artifact_id -> str
def get_artifact_name(artifact_id):
  return pathFragments[artifacts[artifact_id]['pathFragmentId']]['label']

# dep_set_id -> list[artifact_id]
def get_dep(dep_set_id):
  result = set()
  e = depSetOfFiles[dep_set_id]
  result.update(e.get('directArtifactIds', []))
  for dep_set_id in e.get('transitiveDepSetIds', []):
    result.update(get_dep(dep_set_id))
  return result


def get_artifact_sources(artifact_id, seen=None):
  seen = seen if seen is not None else set()

  if artifact_id in seen:
    return set()

  seen.add(artifact_id)

  name = get_artifact_name(artifact_id)
  ext = ''
  if '.' in name:
    ext = name.rsplit('.', 1)[-1].lower()

  # Leaf sources
  if ext in {'c', 'h', 's', 'inc'}:
    return {artifact_id}

  # Dive into intermediate outputs
  if ext not in {'elf', 'o', 'a', 'bin', 'bash', 'lo', 'lib'}:
    return set()

  if artifact_id in actionsByOutput:
    result = set()
    action = actionsByOutput[artifact_id]
    for dep_id in action.get('inputDepSetIds', []):
      for artifact_id in sorted(get_dep(dep_id)):
        result.update(get_artifact_sources(artifact_id, seen=seen))
    return result

  return set()

target_ids = set()
for artifact_id in artifacts.keys():
  path = get_artifact_path(artifact_id)
  if path in targets:
    target_ids.add(artifact_id)
print(target_ids)

if len(target_ids) != len(targets):
  raise FileNotFoundError(f'Some artifact not found')

sources = set()
for artifact_id in target_ids:
  print('Artifact id:', artifact_id, file=sys.stderr)
  print('Path:', get_artifact_path(artifact_id), file=sys.stderr)
    
  for artifact_id in get_artifact_sources(artifact_id):
    p = get_artifact_path(artifact_id)
    sources.add(p)

for p in sorted(sources):
  print(p)
