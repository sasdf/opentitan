import argparse
import hashlib
from pathlib import Path
from typing import Any, Dict, List
from bs4 import BeautifulSoup


def get_uncovered_lines(path: Path) -> List[Dict[str, Any]]:
  """Parses a gcov HTML file to find lines marked as uncovered (tlaUNC).

  Returns a list of dictionaries containing line numbers and surrounding
  context.
  """
  try:
    with open(path, 'r', encoding='utf-8') as f:
      soup = BeautifulSoup(f, 'html.parser')
  except (IOError, UnicodeDecodeError) as e:
    print(f'Error reading {path}: {e}')
    return []

  source_container = soup.find('pre', class_='source')
  if not source_container:
    return []

  # Using recursive=False to ensure we only get the direct span children representing lines
  all_lines = source_container.find_all('span', recursive=False)
  uncovered_lines = []

  for i, line in enumerate(all_lines):
    if line.find('span', class_='tlaUNC'):
      line_num = i + 1
      # Define context window boundaries
      start = max(0, i - 2)
      end = min(len(all_lines), i + 3)

      context = [all_lines[j].get_text() for j in range(start, end)]
      uncovered_lines.append({'line': line_num, 'context': context})

  return uncovered_lines


def get_uncovered_chunks(path: Path) -> List[str]:
  """Generates a deterministic hash based on the content of uncovered code blocks.

  Context lines are stripped of line numbers to allow for minor shifts.
  """
  chunks = []
  for item in get_uncovered_lines(path):
    # Extract the source code part, ignoring the line number prefix (usually separated by ':')
    processed_context = []
    for line_text in item['context']:
      parts = line_text.split(':', 1)
      code_part = parts[-1].strip() if parts else ''
      processed_context.append(code_part)

    chunks.append('\n'.join(processed_context))

  # Sort chunks to ensure file-wide order changes don't affect the hash if the logic is the same
  chunks.sort()
  return chunks


def load_report_chunks(dirpath: Path) -> Dict[Path, List[str]]:
  """Recursively scans a directory for gcov HTML reports and computes chunks for uncovered lines."""
  if not dirpath.is_dir():
    print(f'Warning: {dirpath} is not a valid directory.')
    return {}

  print(f'Scanning reports in {dirpath}...')
  report_chunks = {}
  for path in dirpath.rglob('*.gcov.html'):
    rel_path = path.relative_to(dirpath)
    report_chunks[rel_path] = get_uncovered_chunks(path)
  return report_chunks


def compare_reports(
    chunks_0: Dict[Path, List[str]], chunks_1: Dict[Path, List[str]]
):
  """Compares two sets of report chunks and prints the differences."""
  pathes_0 = set(chunks_0.keys())
  pathes_1 = set(chunks_1.keys())

  for path in sorted(pathes_0 - pathes_1):
    print(f'Only in dir_0: {path}')
  for path in sorted(pathes_1 - pathes_0):
    print(f'Only in dir_1: {path}')
  same_amount = []
  for path in sorted(pathes_0 & pathes_1):
    if len(chunks_0[path]) != len(chunks_1[path]):
      print(
          f'Uncovered {len(chunks_0[path])} lines'
          f' -> {len(chunks_1[path])} lines: {path}'
      )
    else:
      same_amount.append(path)
  for path in same_amount:
    if chunks_0[path] != chunks_1[path]:
      print(f'Uncovered context changed: {path}')


def main():
  parser = argparse.ArgumentParser(
      description=(
          'Compare uncovered code regions between two LCOV HTML reports.'
      )
  )
  parser.add_argument(
      'dir_0', type=Path, help='Path to the baseline coverage report directory'
  )
  parser.add_argument(
      'dir_1', type=Path, help='Path to the new coverage report directory'
  )
  args = parser.parse_args()

  chunks_0 = load_report_chunks(args.dir_0)
  chunks_1 = load_report_chunks(args.dir_1)

  if not chunks_0 and not chunks_1:
    print('No reports found to compare.')
    exit(-1)

  compare_reports(chunks_0, chunks_1)


if __name__ == '__main__':
  main()
