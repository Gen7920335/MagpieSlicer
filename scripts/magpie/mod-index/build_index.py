#!/usr/bin/env python3
"""Builds index.json for a Magpie mod index repository.

Each mod is mods/<id>.json (the definition the app installs) plus an optional
mods/<id>.meta.json with "description", "author", "tags" and "trust". The index
records each definition's SHA-256, so the app refuses a file that changed after
the index was built. URLs are <base-url>/mods/<id>.json.

    python build_index.py --base-url https://raw.githubusercontent.com/<owner>/<repo>/main
    python build_index.py --base-url file:///D:/mods --check   # validate only
"""
import argparse
import hashlib
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ID = re.compile(r'^[a-z_][a-z0-9_]*$')
TRUST = ('community', 'verified', 'official')


def strip_comments(text):
    """Removes // and /* */ comments outside strings, as the app's parser accepts them."""
    out, i, n, in_str = [], 0, len(text), False
    while i < n:
        c = text[i]
        if in_str:
            out.append(c)
            if c == chr(92) and i + 1 < n:  # backslash escape
                out.append(text[i + 1]); i += 2; continue
            if c == '"':
                in_str = False
            i += 1
        elif c == '"':
            in_str = True; out.append(c); i += 1
        elif text.startswith('//', i):
            j = text.find('\n', i); i = n if j < 0 else j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2); i = n if j < 0 else j + 2
        else:
            out.append(c); i += 1
    return ''.join(out)


def build(base_url, mods_dir):
    entries, errors = [], []
    for name in sorted(os.listdir(mods_dir)):
        if not name.endswith('.json') or name.endswith('.meta.json'):
            continue
        path = os.path.join(mods_dir, name)
        raw = open(path, 'rb').read()
        try:
            mod = json.loads(strip_comments(raw.decode('utf-8')))
        except (UnicodeDecodeError, json.JSONDecodeError) as e:
            errors.append(f'{name}: not valid JSON ({e})'); continue
        mod_id = mod.get('id', '')
        if not ID.match(mod_id) or name != mod_id + '.json':
            errors.append(f'{name}: id must match the file name and use a-z, 0-9 and _'); continue
        if not mod.get('version'):
            errors.append(f'{name}: needs a "version"'); continue
        meta_path = os.path.join(mods_dir, mod_id + '.meta.json')
        meta = json.load(open(meta_path, encoding='utf-8')) if os.path.exists(meta_path) else {}
        trust = meta.get('trust', 'community')
        if trust not in TRUST:
            errors.append(f'{name}: trust must be one of {TRUST}'); continue
        entries.append({
            'id': mod_id,
            'name': mod.get('name', mod_id),
            'version': mod['version'],
            'description': meta.get('description', ''),
            'author': meta.get('author', ''),
            'tags': meta.get('tags', []),
            'trust': trust,
            'url': base_url.rstrip('/') + '/mods/' + name,
            'sha256': hashlib.sha256(raw).hexdigest(),
        })
    return {'version': 1, 'mods': entries}, errors


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--base-url', required=True, help='address the mods/ folder is served under')
    p.add_argument('--mods', default=os.path.join(HERE, 'mods'))
    p.add_argument('--out', default=os.path.join(HERE, 'index.json'))
    p.add_argument('--check', action='store_true', help='validate without writing index.json')
    a = p.parse_args()
    index, errors = build(a.base_url, a.mods)
    for e in errors:
        print('error:', e, file=sys.stderr)
    if errors:
        return 1
    if not a.check:
        with open(a.out, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(index, f, indent=2)
            f.write('\n')
    print(f'{len(index["mods"])} mod(s)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
