#!/usr/bin/env python3
"""HPFS image inspector (development aid): hpfsdump.py IMG [--part N|--offset S] [cmd]"""
import struct, sys, time

M = 0xffffffff
def cksum(d):
    x = 0
    for b in d:
        x = (x + b) & M
        x = ((x << 7) | (x >> 25)) & M
    return x

class Vol:
    def __init__(self, path, off=None):
        self.f = open(path, 'rb')
        if off is None:
            mbr = self.raw(0)
            off = 0
            if mbr[510:512] == b'\x55\xaa' and mbr[3:11] != b'MSDOS5.0' and mbr[54:62] != b'HPFS    ':
                for i in range(4):
                    e = mbr[446+16*i:462+16*i]
                    if e[4] == 7:
                        off = struct.unpack_from('<I', e, 8)[0]; break
        self.off = off
        self.sb = self.sec(16); self.sp = self.sec(17)
        (m0, m1, self.ver, self.fver, _, self.root, self.nsec, self.nbad, self.bmlist, _, self.badlist, _,
         self.chk, self.opt, self.ndb, self.dbstart, self.dbend, self.dbbmp) = struct.unpack_from('<IIBBHIIIIIIIIIIIII', self.sb)
        self.uidtab = struct.unpack_from('<I', self.sb, 96)[0]
        assert (m0, m1) == (0xF995E849, 0xFA53E9C5), 'bad superblock'
        (n0, n1, self.spflags, self.hotmap, self.hused, self.hmax, self.dsfree, self.dsn, self.cpdir, self.ncp,
         self.scrc, self.pcrc) = struct.unpack_from('<IIIIIIIIIIII', self.sp)
        self.spdn = struct.unpack_from('<%dI' % self.dsn, self.sp, 0x6c)
        self.load_cp()
    def raw(self, n, c=1):
        self.f.seek(n*512); return self.f.read(512*c)
    def sec(self, n, c=1):
        return self.raw(self.off+n, c)
    def load_cp(self):
        d = self.sec(self.cpdir)
        mg, n = struct.unpack_from('<II', d)
        self.cps = []
        for i in range(n):
            ix, cpn, bounds, data, idx, unk = struct.unpack_from('<HHIIHH', d, 16+16*i)
            dd = self.sec(data)
            mg2, used = struct.unpack_from('<II', dd)
            offs = struct.unpack_from('<3H', dd, 0x14)
            o = offs[idx]
            dix, dcp, dunk = struct.unpack_from('<HHH', dd, o)  # ix, cp, ?
            upmap = dd[o+6:o+6+128]
            self.cps.append(dict(ix=ix, cp=cpn, bounds=bounds, sec=data, idx=idx, unk=unk, map=upmap, dhdr=(hex(mg2),used,offs,dix,dcp,dunk)))
    def fnode(self, n):
        d = self.sec(n)
        r = {}
        (r['magic'],) = struct.unpack_from('<I', d, 0)
        r['len'] = d[12]; r['name'] = d[13:13+min(15, d[12])]
        (r['up'], r['acl_l'], r['acl_sec'], r['acl_s'], r['acl_an'], _, r['ea_l'], r['ea_sec'], r['ea_s'], r['flags']) = \
            struct.unpack_from('<IIIHBBIIHH', d, 28)
        r['bt'] = d[0x38:0x40]
        r['ext'] = self.btree(d, 0x38, 8, 12)
        r['size'], r['needea'] = struct.unpack_from('<II', d, 0xA0)
        r['uid'] = d[0xA8:0xB8]
        r['ea_offs'], r['dthr'], r['ddel'], r['dlim'], r['duse'] = struct.unpack_from('<HBBII', d, 0xB8)
        r['raw'] = d
        return r
    def btree(self, d, o, nleaf, nint):
        fl, _, _, _, nfree, nused, ffree = struct.unpack_from('<BBBBBBH', d, o)
        ents = []
        if fl & 0x80:
            for i in range(nused):
                fs, down = struct.unpack_from('<II', d, o+8+8*i)
                ents.append(('int', fs, down))
        else:
            for i in range(nused):
                fs, ln, ds = struct.unpack_from('<III', d, o+8+12*i)
                ents.append(('ext', fs, ln, ds))
        return dict(flags=fl, nfree=nfree, nused=nused, ffree=ffree, ents=ents)
    def extents(self, fn):
        out = []
        def walk(bt):
            for e in bt['ents']:
                if e[0] == 'ext': out.append(e[1:])
                else:
                    a = self.sec(e[2])
                    mg, me, up = struct.unpack_from('<III', a)
                    assert mg == 0x37E40AAE, hex(mg)
                    walk(self.btree(a, 0x0C, 40, 60))
        walk(fn['ext'])
        return out
    def dnode(self, n):
        d = self.sec(n, 4)
        mg, ff, rt, up, me = struct.unpack_from('<IIIII', d)
        assert mg == 0x77E40AAE, (n, hex(mg))
        ents = []; o = 20
        while True:
            ln, fl, at, fn, wd, sz, rd, cd, eas, nacl, ix, nl = struct.unpack_from('<HBBIIIIIIBBB', d, o)
            name = d[o+31:o+31+nl]
            down = struct.unpack_from('<I', d, o+ln-4)[0] if fl & 4 else None
            ents.append(dict(off=o, len=ln, flags=fl, attr=at, fnode=fn, wdate=wd, size=sz, rdate=rd, cdate=cd, easz=eas, nacl=nacl, ix=ix, name=name, down=down))
            if fl & 8: break
            o += ln
        return dict(first_free=ff, root=rt, up=up, self=me, ents=ents, used=o+ln)
    def walkdir(self, dn, depth=0, out=None):
        d = self.dnode(dn)
        for e in d['ents']:
            if e['down']: self.walkdir(e['down'], depth+1, out)
            if not e['flags'] & 8: out.append((depth, dn, e))
        return out

def ts(t):
    return time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime(t)) if t else '-'

def main():
    a = sys.argv[1:]
    off = None
    if '--offset' in a:
        i = a.index('--offset'); off = int(a[i+1], 0); del a[i:i+2]
    v = Vol(a[0], off)
    cmd = a[1] if len(a) > 1 else 'info'
    if cmd == 'info':
        print('off', v.off, 'ver', v.ver, v.fver, 'root', hex(v.root), 'nsec', v.nsec, 'bmlist', hex(v.bmlist), 'bad', hex(v.badlist), 'uidtab', hex(v.uidtab))
        print('chkdsk', ts(v.chk), 'optimize', ts(v.opt))
        print('dirband n', v.ndb, 'start', hex(v.dbstart), 'end', hex(v.dbend), 'bmp', hex(v.dbbmp))
        print('spare flags', hex(v.spflags), 'hotfix', hex(v.hotmap), v.hused, v.hmax, 'dnode spares', v.dsfree, v.dsn, [hex(x) for x in v.spdn[:4]])
        print('cpdir', hex(v.cpdir), v.ncp, 'crc super', hex(v.scrc), hex(cksum(v.sb)), 'spare', hex(v.pcrc), hex(cksum(v.sp[:0x2c]+b'\0'*4+v.sp[0x30:])))
        for c in v.cps: print('cp', {k: (val if k != 'map' else val.hex()) for k, val in c.items()})
        fn = v.fnode(v.root); print('root fnode', {k: fn[k] for k in fn if k != 'raw'})
    elif cmd == 'fnode':
        fn = v.fnode(int(a[2], 0)); print({k: fn[k] for k in fn if k != 'raw'}); print(fn['raw'].hex())
    elif cmd == 'dnode':
        d = v.dnode(int(a[2], 0)); print({k: d[k] for k in d if k != 'ents'})
        for e in d['ents']: print(e)
    elif cmd == 'ls':
        fn = v.fnode(int(a[2], 0) if len(a) > 2 else v.root)
        dn = struct.unpack_from('<I', fn['raw'], 0x48)[0]
        for depth, d, e in v.walkdir(dn, 0, []):
            print('  '*depth, hex(d), '%02x %02x' % (e['flags'], e['attr']), hex(e['fnode']), e['size'], ts(e['wdate']), e['name'])
    elif cmd == 'sec':
        print(v.sec(int(a[2], 0), int(a[3], 0) if len(a) > 3 else 1).hex())

if __name__ == '__main__':
    main()
