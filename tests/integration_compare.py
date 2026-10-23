#!/usr/bin/env python3
# integration_compare.py <ref case> <app case> <tol>: the latest time directory must be the same in both, and every
# ascii field there (and in region subdirectories) must agree: max |app - ref| / max |ref| <= tol, per field.
# Prints one line per field and a verdict; exit 0 on agreement, 1 on a difference, 2 when there is nothing to compare.
import os, re, sys

NUM = re.compile(r'[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?')

def latest(case):
    ts = []
    for d in os.listdir(case):
        try: v = float(d)
        except ValueError: continue
        if v > 0 and os.path.isdir(os.path.join(case, d)): ts.append((v, d))
    return max(ts)[1] if ts else None

def internal(path):
    """the internalField's numbers, or None (binary, or not a field)"""
    with open(path, 'rb') as f: raw = f.read()
    if b'\0' in raw: return None
    t = raw.decode('latin-1')
    i = t.find('\ninternalField')
    if i < 0: return None
    head = t[i:i + 200]
    if 'nonuniform' not in head.split(';')[0]:
        m = re.search(r'uniform\s+(\([^)]*\)|[^;]+);', t[i:])
        return [float(x) for x in NUM.findall(m.group(1))] if m else None
    a = t.find('(', t.find('>', i))                     # the list's own "(" after "List<type> N"
    b = t.find('\n)', a)
    return [float(x) for x in NUM.findall(t[a + 1:b])] if a > 0 and b > a else None

def fields(case, time):
    out = {}
    root = os.path.join(case, time)
    for dp, dn, fn in os.walk(root):
        if 'uniform' in dp.split(os.sep) or 'polyMesh' in dp.split(os.sep): continue
        for f in fn:
            v = internal(os.path.join(dp, f))
            if v is not None: out[os.path.relpath(os.path.join(dp, f), root)] = v
    return out

ref, app, tol = sys.argv[1], sys.argv[2], float(sys.argv[3])
tr, ta = latest(ref), latest(app)
if not tr or not ta: print(f"nothing to compare: ref latest={tr} app latest={ta}"); sys.exit(2)
if abs(float(tr) - float(ta)) > 1e-9 * max(1, abs(float(tr))):
    print(f"DIFFERENT end time: ref {tr}, app {ta}"); sys.exit(1)
fr, fa = fields(ref, tr), fields(app, ta)
bad = 0
for name in sorted(fr):
    if name not in fa: print(f"  MISSING in app: {name}"); bad += 1; continue
    a, r = fa[name], fr[name]
    if len(a) != len(r) and len(r) != 1 and len(a) != 1:
        print(f"  {name:28s} size {len(a)} vs ref {len(r)}"); bad += 1; continue
    if len(a) != len(r): a, r = (a * len(r), r) if len(a) == 1 else (a, r * len(a))
    scale = max(1e-30, max(abs(x) for x in r))
    d = max(abs(x - y) for x, y in zip(a, r)) / scale
    flag = 'ok' if d <= tol else 'DIFF'
    if d > tol: bad += 1
    print(f"  {name:28s} rel {d:.2e}  {flag}")
print(f"t={tr}: {len(fr)} fields, {bad} outside {tol:g}")
sys.exit(1 if bad or not fr else 0)
