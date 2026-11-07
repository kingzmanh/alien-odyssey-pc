"""Linear Executable (LE) loader for the PMODE/W build of ALIENSP2.EXE.

Parses the object table, page map and fixup records, then builds a flat
memory image with every object placed at its preferred base address and
all internal fixups applied.  Used by the recompiler and the analysis tools.
"""
import struct, sys


# Objects are moved above 1 MB so the low megabyte stays free for DOS memory
# and the VGA window.  Object 3 is the linker's absolute A000h segment (the
# VGA window itself) and stays where it is.
RELOC = {1: 0x400000, 2: 0x400000, 3: 0x400000}


class Obj:
    pass


def find_mz(d):
    """File offset of the MZ header whose e_lfanew points at the LE header.
    4GWSTUB programs have it at 0; programs with DOS/4GW bound in front
    (3DGAME.EXE) have a chain of MZ/BW headers first."""
    o = 0
    for _ in range(16):
        sig = d[o:o + 2]
        if sig == b'MZ':
            lf = struct.unpack_from('<I', d, o + 0x3c)[0]
            if o + lf + 2 <= len(d) and d[o + lf:o + lf + 2] == b'LE':
                return o
            cblp, cp = struct.unpack_from('<HH', d, o + 2)
            o += cp * 512 - ((512 - cblp) if cblp else 0)
        elif sig == b'BW':
            o = struct.unpack_from('<I', d, o + 0x1c)[0]
        else:
            break
    raise ValueError('no LE header found')


class LE:
    def __init__(self, path, reloc=None):
        if reloc is None:
            reloc = RELOC
        d = open(path, 'rb').read()
        self.data = d
        self.mz = find_mz(d)
        le = self.mz + struct.unpack_from('<I', d, self.mz + 0x3c)[0]
        self.le = le
        h = lambda off, fmt='<I': struct.unpack_from(fmt, d, le + off)[0]
        self.page_size = h(0x28)
        self.last_page = h(0x2c)
        self.eip_obj = h(0x18)
        self.eip = h(0x1c)
        self.esp_obj = h(0x20)
        self.esp = h(0x24)
        nobj = h(0x44)
        obj_off = h(0x40)
        npages = h(0x14)
        pagemap_off = h(0x48)
        fixpage_off = h(0x68)
        fixrec_off = h(0x6c)
        data_pages = h(0x80) + self.mz  # relative to the MZ header that holds the LE
        self.npages = npages
        self.objs = []
        for i in range(nobj):
            o = Obj()
            (o.vsize, o.base, o.flags, o.pmi, o.npages, _) = struct.unpack_from('<6I', d, le + obj_off + i * 24)
            o.index = i + 1
            o.exec = bool(o.flags & 4)
            o.orig_base = o.base
            o.base += reloc.get(o.index, 0)
            self.objs.append(o)
        # page map: LE uses 4-byte entries (3-byte page number, 1-byte flags)
        self.page_file = []
        for i in range(npages):
            ent = d[le + pagemap_off + i * 4: le + pagemap_off + i * 4 + 4]
            pnum = (ent[0] << 16) | (ent[1] << 8) | ent[2]
            self.page_file.append(data_pages + (pnum - 1) * self.page_size)
        fixpage = [struct.unpack_from('<I', d, le + fixpage_off + i * 4)[0] for i in range(npages + 1)]
        self.fixups = []  # (src_linear, kind, target_linear)
        self.image = {}
        self.lo = min(o.base for o in self.objs)
        self.hi = max(o.base + max(o.vsize, o.npages * self.page_size) for o in self.objs)
        self.mem = bytearray(self.hi - self.lo)
        for o in self.objs:
            for p in range(o.npages):
                gp = o.pmi - 1 + p
                size = self.page_size if gp != npages - 1 else self.last_page
                src = self.page_file[gp]
                dst = o.base + p * self.page_size - self.lo
                self.mem[dst:dst + size] = d[src:src + size]
        for o in self.objs:
            for p in range(o.npages):
                gp = o.pmi - 1 + p
                pos = le + fixrec_off + fixpage[gp]
                end = le + fixrec_off + fixpage[gp + 1]
                page_lin = o.base + p * self.page_size
                while pos < end:
                    src_t = d[pos]; flg = d[pos + 1]; pos += 2
                    if src_t & 0x20:
                        cnt = d[pos]; pos += 1
                        offs = None
                    else:
                        offs = struct.unpack_from('<h', d, pos)[0]; pos += 2
                    if flg & 3 != 0:
                        raise ValueError('import fixups not supported (flags %x)' % flg)
                    if flg & 0x40:
                        tobj = struct.unpack_from('<H', d, pos)[0]; pos += 2
                    else:
                        tobj = d[pos]; pos += 1
                    stype = src_t & 0xf
                    toff = 0
                    if stype != 2:  # not a 16-bit selector fixup
                        if flg & 0x10:
                            toff = struct.unpack_from('<I', d, pos)[0]; pos += 4
                        else:
                            toff = struct.unpack_from('<H', d, pos)[0]; pos += 2
                    if src_t & 0x20:
                        lst = []
                        for _ in range(cnt):
                            lst.append(struct.unpack_from('<h', d, pos)[0]); pos += 2
                    else:
                        lst = [offs]
                    tgt = self.objs[tobj - 1].base + toff
                    for so in lst:
                        sl = page_lin + so
                        self.fixups.append((sl, stype, tgt, tobj))
                        self._apply(sl, stype, tgt, tobj)
        self.entry = self.objs[self.eip_obj - 1].base + self.eip
        self.stack = self.objs[self.esp_obj - 1].base + self.esp

    def _apply(self, sl, stype, tgt, tobj):
        i = sl - self.lo
        if stype == 7:  # 32-bit offset
            if 0 <= i and i + 4 <= len(self.mem):
                struct.pack_into('<I', self.mem, i, tgt & 0xffffffff)
        elif stype == 8:  # 32-bit self-relative
            if 0 <= i and i + 4 <= len(self.mem):
                struct.pack_into('<i', self.mem, i, tgt - (sl + 4))
        elif stype == 5:  # 16-bit offset
            if 0 <= i and i + 2 <= len(self.mem):
                struct.pack_into('<H', self.mem, i, tgt & 0xffff)
        elif stype == 2:  # 16-bit selector: the runtime's flat selectors (src/rt/rt.h)
            if 0 <= i and i + 2 <= len(self.mem):
                struct.pack_into('<H', self.mem, i, 0x10 if tobj in (1, 2) else 0x18)
        else:
            raise ValueError('fixup type %d unsupported' % stype)

    def u8(self, a):
        return self.mem[a - self.lo]

    def u32(self, a):
        return struct.unpack_from('<I', self.mem, a - self.lo)[0]

    def obj_of(self, a):
        for o in self.objs:
            if o.base <= a < o.base + max(o.vsize, o.npages * self.page_size):
                return o
        return None


if __name__ == '__main__':
    le = LE(sys.argv[1])
    for o in le.objs:
        print('obj%d base=%08x vsize=%08x flags=%04x pages=%d %s' % (
            o.index, o.base, o.vsize, o.flags, o.npages, 'CODE' if o.exec else 'DATA'))
    print('entry %08x stack %08x  fixups %d' % (le.entry, le.stack, len(le.fixups)))
    from collections import Counter
    print(Counter((f[1], f[3]) for f in le.fixups))
