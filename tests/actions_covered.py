#!/usr/bin/env python3
# actions_covered.py <q-continuum binary> <journeys dir> (ROADMAP 2.1): every action in the registry must be exercised by
# some journey: "action <id>", a click on the action's button, or its keyboard shortcut ("key S ctrl" for Ctrl+S).
# Fails listing the actions nothing tests.
import glob, os, re, subprocess, sys

binary, jdir = sys.argv[1], sys.argv[2]
out = subprocess.run([binary, '--list-actions'], capture_output=True, text=True, timeout=30).stdout
lines = []
for f in sorted(glob.glob(os.path.join(jdir, '*.qcs'))):
    for ln in open(f, encoding='utf-8'):
        ln = ln.strip()
        if ln and not ln.startswith('#'): lines.append((os.path.basename(f), ln))

def key_line(shortcut):
    """'Ctrl+Shift+Z' -> ('Z', {'ctrl','shift'})"""
    parts = shortcut.split('+')
    k = parts[-1] if parts[-1] else '+'
    return k.upper(), {p.lower() for p in parts[:-1]}

KEYNAMES = {',': 'COMMA', '.': 'PERIOD'}
missing, n = [], 0
for row in out.splitlines():
    aid, widget, shortcut = row.split('\t')
    n += 1
    how = None
    for f, ln in lines:
        w = ln.split()
        if w[:2] == ['action', aid]: how = f; break
        if widget != '-' and ln == f'click {widget}': how = f; break
        if shortcut != '-' and w[0] == 'key':
            k, mods = key_line(shortcut)
            k = KEYNAMES.get(k, k)
            if w[1].upper() == k and set(x.lower() for x in w[2:]) == mods: how = f; break
    if not how: missing.append(f'{aid} (button {widget}, shortcut {shortcut})')
for m in missing: print('NOT TESTED:', m)
print(f'actions: {n - len(missing)} of {n} exercised by a journey')
sys.exit(1 if missing or n == 0 else 0)
