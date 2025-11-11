import sys
import bs4
import os

BREAKDOWN_FILE = sys.argv[1]

dirname = os.path.dirname(BREAKDOWN_FILE)

# Load the HTML content
with open(BREAKDOWN_FILE, 'r') as f:
    html_content = f.read()

# Parse the HTML with BeautifulSoup
soup = bs4.BeautifulSoup(html_content, 'html.parser')

# Find all <a> tags
links = soup.find_all('a')

# Iterate through the links and print their 'href' attribute
ok = True
for link in links:
    href = link.get('href')
    text = link.get_text().strip()
    if not text.endswith('.gcov.html'):
        print('[-] Other link:', link)
        continue

    if not href.endswith(text.split('//', 1)[-1]):
        print('[!] Wrong link:', href, text)
        ok = False

    # check href exist (file path relative to the html)
    if not os.path.exists(os.path.join(dirname, href)):
        print('[!] Broken link:', href)
        ok = False

if ok:
    print(f'[+] Success! Checked {len(links)} links.')
else:
    print('[!] Failed!!!!')
