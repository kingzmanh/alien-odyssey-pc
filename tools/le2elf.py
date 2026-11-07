"""Write the relocated LE image as a static ELF32/i386 so Ghidra can load it
with proper code/data blocks.  Also writes fixup_targets.txt (code-object
fixup targets, one hex address per line) for the Ghidra seed script.
usage: le2elf.py ALIENSP2.EXE out.elf out_dir"""
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from le import LE

le = LE(sys.argv[1])
out = sys.argv[2]
outdir = sys.argv[3]
segs = []
for o in le.objs:
    size = max(o.vsize, o.npages * le.page_size)
    filesz = min(o.npages * le.page_size, size)
    data = bytes(le.mem[o.base - le.lo:o.base - le.lo + filesz])
    segs.append((o.base, size, data, o.exec))

# section header string table
names = [b'', b'.shstrtab'] + [('.obj%d' % (i + 1)).encode() for i in range(len(segs))]
shstr = b''
name_off = []
for n in names:
    name_off.append(len(shstr)); shstr += n + b'\0'

ehsize, phsize, shsize = 52, 32, 40
nph = len(segs)
off = ehsize + nph * phsize
blob = b''
seg_off = []
for (base, size, data, ex) in segs:
    pad = (-(off + len(blob))) % 0x1000
    blob += b'\0' * pad
    seg_off.append(off + len(blob))
    blob += data
shstr_off = off + len(blob)
blob += shstr
pad = (-(off + len(blob))) % 4
blob += b'\0' * pad
shoff = off + len(blob)
nsh = 2 + len(segs)

eh = b'\x7fELF' + bytes([1, 1, 1, 0]) + b'\0' * 8
eh += struct.pack('<HHIIIIIHHHHHH', 2, 3, 1, le.entry, ehsize, shoff, 0, ehsize, phsize, nph, shsize, nsh, 1)
ph = b''
for (base, size, data, ex), so in zip(segs, seg_off):
    flags = 5 if ex else 6
    ph += struct.pack('<IIIIIIII', 1, so, base, base, len(data), size, flags, 0x1000)
sh = b'\0' * shsize
sh += struct.pack('<IIIIIIIIII', name_off[1], 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0)
for i, ((base, size, data, ex), so) in enumerate(zip(segs, seg_off)):
    typ = 1 if data else 8
    flags = 2 | (4 if ex else 1)
    sh += struct.pack('<IIIIIIIIII', name_off[2 + i], typ, flags, base, so, size if typ == 8 else len(data), 0, 0, 16, 0)
    if typ == 1 and len(data) < size:
        pass
with open(out, 'wb') as f:
    f.write(eh + ph + blob + sh)

code = le.objs[0]
tg = sorted(set(t for (s, k, t, ob) in le.fixups if ob == 1 and k in (7, 8)))
with open(os.path.join(outdir, 'fixup_targets.txt'), 'w') as f:
    for t in tg:
        f.write('%x\n' % t)
print('wrote', out, 'entry %x' % le.entry, len(tg), 'code targets')
