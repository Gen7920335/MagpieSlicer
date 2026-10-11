#!/usr/bin/env python3
"""Recolor OrcaSlicer's teal theme to the Magpie blue theme.

Run after every upstream merge: new upstream icons, pages and widgets arrive in
Orca colors. The map in theme_colors.json was derived from the Magpie 2.5.0.x
fork, where every listed color was replaced everywhere it occurred, so a global
replacement reproduces that fork's theme. Colors Magpie only changed in some
places are deliberately absent and must be edited by hand.

Usage: python3 scripts/magpie/apply_theme.py [--check]
--check exits 1 and lists files when anything would change.
"""
import json, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
COLORS = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'theme_colors.json')))
SCOPES = ['resources/images', 'resources/web', 'src/slic3r/GUI']
EXTS = ('.svg', '.css', '.html', '.js', '.cpp', '.hpp', '.h')

RGB = {tuple(int(v[i:i + 2], 16) for i in (1, 3, 5)): v for v in COLORS}

def hex_sub(m):
    text = m.group(0)
    prefix, digits = (text[:1], text[1:]) if text.startswith('#') else (text[:2], text[2:])
    key = '#' + (''.join(c * 2 for c in digits) if len(digits) == 3 else digits).lower()
    new = COLORS.get(key)
    if new is None:
        return text
    new = new[1:]
    if digits.isupper() or (any(c.isalpha() for c in digits) and digits == digits.upper()):
        new = new.upper()
    return prefix + new

# Integer triples are only recolored inside a color constructor, never as bare numbers.
TRIPLE = re.compile(r'(\b(?:wxColour|wxColor|ImColor|IM_COL32|rgba?)\(\s*)(\d{1,3})(\s*,\s*)(\d{1,3})(\s*,\s*)(\d{1,3})')

def triple_sub(m):
    rgb = (int(m.group(2)), int(m.group(4)), int(m.group(6)))
    old = '#%02x%02x%02x' % rgb if all(c < 256 for c in rgb) else None
    new = COLORS.get(old) if old else None
    if not new:
        return m.group(0)
    r, g, b = (int(new[i:i + 2], 16) for i in (1, 3, 5))
    return f'{m.group(1)}{r}{m.group(3)}{g}{m.group(5)}{b}'

HEX = re.compile(r'#[0-9a-fA-F]{6}\b|#[0-9a-fA-F]{3}\b(?![0-9a-fA-F])|0x[0-9a-fA-F]{6}\b')

def recolor(text):
    text = HEX.sub(hex_sub, text)
    return TRIPLE.sub(triple_sub, text)

def main():
    check = '--check' in sys.argv
    changed = []
    for scope in SCOPES:
        for dirpath, _, names in os.walk(os.path.join(ROOT, scope)):
            for name in names:
                if not name.endswith(EXTS):
                    continue
                path = os.path.join(dirpath, name)
                with open(path, encoding='utf-8', errors='surrogateescape', newline='') as f:
                    old = f.read()
                new = recolor(old)
                if new != old:
                    changed.append(os.path.relpath(path, ROOT))
                    if not check:
                        with open(path, 'w', encoding='utf-8', errors='surrogateescape', newline='') as f:
                            f.write(new)
    for p in changed:
        print(p)
    if check and changed:
        sys.exit(1)

if __name__ == '__main__':
    main()
