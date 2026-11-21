#!/usr/bin/env python3
"""A binary STL past the old 64 MB read limit: a finely tessellated sphere (radius 0.05 m), N x 2N quads split in two.
Usage: gen_big_stl.py out.stl [N]   (N = 900 -> 3.24 M triangles, 162 MB)"""
import math, struct, sys

out = sys.argv[1]
n = int(sys.argv[2]) if len(sys.argv) > 2 else 900
r = 0.05
m = 2 * n
pts = [[(r * math.sin(math.pi * i / n) * math.cos(2 * math.pi * j / m),
         r * math.sin(math.pi * i / n) * math.sin(2 * math.pi * j / m),
         r * math.cos(math.pi * i / n)) for j in range(m)] for i in range(n + 1)]
rec = struct.Struct("<12fH")
tris = []
with open(out, "wb") as f:
    f.write(b"gen_big_stl".ljust(80, b" "))
    f.write(struct.pack("<I", 0))
    count = 0
    buf = bytearray()
    for i in range(n):
        for j in range(m):
            a, b = pts[i][j], pts[i][(j + 1) % m]
            c, d = pts[i + 1][j], pts[i + 1][(j + 1) % m]
            if i > 0:          # the top row's upper triangle has two corners at the pole
                buf += rec.pack(0, 0, 0, *a, *c, *b, 0); count += 1
            if i < n - 1:      # so has the bottom row's lower one
                buf += rec.pack(0, 0, 0, *b, *c, *d, 0); count += 1
        if len(buf) > 1 << 24:
            f.write(buf); buf = bytearray()
    f.write(buf)
    f.seek(80)
    f.write(struct.pack("<I", count))
print(count)
