#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)

find "$ROOT" -type f -name '*.sh' -print0 | xargs -0 -n1 bash -n

python3 - "$ROOT" <<'PY'
from __future__ import annotations
import ast, json, pathlib, re, sys, urllib.parse, xml.etree.ElementTree as ET

root = pathlib.Path(sys.argv[1])
errors: list[str] = []

for path in root.rglob('*'):
    if not path.is_file() or any(p.startswith('build') for p in path.parts):
        continue
    rel = path.relative_to(root)
    if path.suffix == '.json':
        try: json.loads(path.read_text())
        except Exception as exc: errors.append(f'{rel}: invalid JSON: {exc}')
    if path.suffix == '.py':
        try: ast.parse(path.read_text(), filename=str(rel))
        except Exception as exc: errors.append(f'{rel}: invalid Python: {exc}')
    if path.suffix in {'.svg', '.xml'}:
        try: ET.parse(path)
        except Exception as exc: errors.append(f'{rel}: invalid XML: {exc}')
    if path.suffix == '.md':
        text = path.read_text(errors='replace')
        for line_no, line in enumerate(text.splitlines(), 1):
            if line.rstrip() != line:
                errors.append(f'{rel}:{line_no}: trailing whitespace')
        for target in re.findall(r'(?<!!)\[[^]]*\]\(([^)]+)\)', text):
            target = target.strip().split()[0].strip('<>')
            if target.startswith(('http://', 'https://', 'mailto:', '#')):
                continue
            local = urllib.parse.unquote(target.split('#', 1)[0])
            if local and not (path.parent / local).resolve().exists():
                errors.append(f'{rel}: broken link: {target}')

private = re.compile(rb'(?:/' + b'home/[A-Za-z0-9._-]+/|/mnt/' + b'data/|sandbox:' + b'/)')
for path in root.rglob('*'):
    if not path.is_file() or '.git' in path.parts or any(p.startswith('build') for p in path.parts):
        continue
    if private.search(path.read_bytes()):
        errors.append(f'{path.relative_to(root)}: absolute/private local path')

if errors:
    print('\n'.join(errors), file=sys.stderr)
    raise SystemExit(1)
print('Python, JSON, XML/SVG, Markdown links, whitespace, and local-path audit: PASS')
PY

if command -v ruby >/dev/null; then
  find "$ROOT/.github" -type f \( -name '*.yml' -o -name '*.yaml' \) -print0 |
    xargs -0 -n1 ruby -e 'require "yaml"; YAML.load_file(ARGV.fetch(0), aliases: true)'
fi

printf 'repository validation: PASS\n'
