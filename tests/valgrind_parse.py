#!/usr/bin/env python3
# valgrind_parse.py <src dir> <memcheck.*.txt ...>: count Memcheck errors in our code; exit 1 if any, 2 if the app's log is
# missing or unfinished. Self-test: tests/valgrind_fixture (ctest valgrind_parse).
# Text logs, one per process (the app and anything it forks before exec). Error blocks start with "==pid== <kind>"
# after a blank "==pid==" line; a block is ours when its innermost frame with source info is a file in src/. The app's log
# (the one that ran main()) must exist and end with an ERROR SUMMARY, or the check FAILS: an unread log is not a pass.
import sys, os, re
src = set(os.listdir(sys.argv[1]))
ours = other = 0; main_seen = False
logs = {}
for fn in sys.argv[2:]:
    lines = [re.sub(r'^==\d+== ?', '', l.rstrip('\n')) for l in open(fn, errors='replace')]
    if any('Command:' in l and 'q-continuum' in l for l in lines[:12]):   # not a child that exec'd something else
        pid = re.search(r'memcheck\.(\d+)\.txt$', fn).group(1)
        ppid = next((l.split()[-1] for l in lines[:12] if l.startswith('Parent PID:')), '')
        logs[pid] = (fn, ppid, lines)
for pid, (fn, ppid, lines) in logs.items():
    summary = any(l.startswith('ERROR SUMMARY:') for l in lines)
    if ppid in logs and not summary:
        continue                                    # a fork of the app (the GPU probe) that _exit()ed: no summary
    if not summary:
        print(f"FAIL  {fn}: no ERROR SUMMARY (killed or truncated)"); sys.exit(2)
    main_seen = True
    blocks, cur = [], []
    for l in lines:
        if l == '':
            if cur: blocks.append(cur); cur = []
        else: cur.append(l)
    if cur: blocks.append(cur)
    for b in blocks:
        head = b[0]
        if not re.match(r'(Invalid|Conditional|Use of uninit|Syscall param|Mismatched|Source and destination|Argument|\d[\d,]* bytes in .* definitely lost)', head):
            continue
        # the innermost frame with source info decides; Valgrind's own malloc/str* wrappers (vg_replace_*) are skipped,
        # so a leak or a bad memcpy is charged to the code that called it, and driver errors under InitWindow -> main
        # (no source info in Mesa/dxcore, then raylib's glfw) are not ours
        frames = [(m.group(1), l) for l in b[1:] for m in [re.search(r'\((\w[\w.+-]*\.[ch]):(\d+)\)', l)] if m]
        frames = [f for f in frames if not f[0].startswith('vg_replace')]
        if frames and frames[0][0] in src:
            ours += 1
            print(f"OURS  {head}\n        {frames[0][1].strip()}")
        else: other += 1
if not main_seen:
    print("FAIL  no Valgrind log from the app itself"); sys.exit(2)
print(f"memcheck: {ours} errors in our code, {other} in libraries")
sys.exit(1 if ours else 0)
