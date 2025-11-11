from bs4 import BeautifulSoup
from pathlib import Path
import difflib
import sys

dir_a = Path(sys.argv[1])
dir_b = Path(sys.argv[2])

def load(path):
    with open(path) as f:
        soup = BeautifulSoup(f.read(), 'html.parser')
    soup = soup.find('pre', class_='source')

    for tag in soup.find_all('span', class_='lineNum'):
        tag.decompose()

    out = []
    for line in soup.find_all('span', recursive=False):
        count, line = line.get_text().split(':', 1)
        count = count.strip()
        if count == '':
            line = f'S:{line}'
        elif count == '0':
            line = f'U:{line}'
        else:
            line = f'C:{line}'
        out.append(line + '\n')
    return ''.join(out)

def find_files(dir_x):
    files_x = sorted(dir_x.glob('**/*.gcov.html'))
    return {p.relative_to(dir_x): p for p in files_x}


files_a, files_b = find_files(dir_a), find_files(dir_b)

# show diff between files_a and files_b
matched_files = set(files_a.keys()) & set(files_b.keys())
for f in matched_files:
    file_a = files_a[f]
    file_b = files_b[f]

    content_a = load(file_a)
    content_b = load(file_b)

    diff = difflib.unified_diff(content_a.splitlines(keepends=True),
                                content_b.splitlines(keepends=True),
                                fromfile=str(file_a),
                                tofile=str(file_b))
    sys.stdout.writelines(diff)

# show filepath only in one set
for f in sorted(set(files_a.keys()) - set(files_b.keys())):
    sys.stdout.write(f'Only in A: {f}\n')

for f in sorted(set(files_b.keys()) - set(files_a.keys())):
    sys.stdout.write(f'Only in B: {f}\n')
