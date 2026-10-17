#!/usr/bin/env python3
"""pexports.py DLL: the names a PE image exports, one per line"""
import struct, sys
d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
nsec = struct.unpack_from('<H', d, pe + 6)[0]
osz = struct.unpack_from('<H', d, pe + 20)[0]
opt = pe + 24
magic = struct.unpack_from('<H', d, opt)[0]
exp = struct.unpack_from('<I', d, opt + (112 if magic == 0x20b else 96))[0]
secs = [struct.unpack_from('<8sIIII', d, opt + osz + 40 * i) for i in range(nsec)]
def off(rva):
    for n, vs, va, rs, ro in secs:
        if va <= rva < va + max(vs, rs):
            return rva - va + ro
n = struct.unpack_from('<I', d, off(exp) + 24)[0]
names = struct.unpack_from('<I', d, off(exp) + 32)[0]
for i in range(n):
    o = off(struct.unpack_from('<I', d, off(names) + 4 * i)[0])
    print(d[o:d.index(b'\0', o)].decode())
