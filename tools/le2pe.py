"""Wrap the relocated LE image in a minimal PE32 (image base 0, sections at
their LE linear addresses) so REA's Windows Ghidra provider can analyze it.
usage: le2pe.py ALIENSP2.EXE out.exe"""
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from le import LE

le = LE(sys.argv[1])
FA, SA = 0x200, 0x1000
secs = []
for o in le.objs:
    vsize = max(o.vsize, o.npages * le.page_size)
    raw = bytes(le.mem[o.base - le.lo:o.base - le.lo + min(o.npages * le.page_size, vsize)])
    raw = raw.rstrip(b'\0') if not o.exec else raw
    secs.append(('.obj%d' % o.index, o.base, vsize, raw, o.exec))

nsec = len(secs)
hdr_size = 0x40 + 4 + 20 + 224 + 40 * nsec
hdr_raw = (hdr_size + FA - 1) // FA * FA
off = hdr_raw
table = b''
blobs = b''
for name, va, vs, raw, ex in secs:
    rs = (len(raw) + FA - 1) // FA * FA
    ch = (0x60000020 if ex else 0xC0000040)
    table += struct.pack('<8sIIIIIIHHI', name.encode(), vs, va, rs, off if rs else 0, 0, 0, 0, 0, ch)
    blobs += raw + b'\0' * (rs - len(raw))
    off += rs
size_image = max(va + vs for _, va, vs, _, _ in secs)
size_image = (size_image + SA - 1) // SA * SA
dos = bytearray(0x40)
dos[0:2] = b'MZ'
struct.pack_into('<I', dos, 0x3c, 0x40)
coff = struct.pack('<HHIIIHH', 0x14c, nsec, 0, 0, 0, 224, 0x0103)
opt = struct.pack('<HBBIIIIIIIIIHHHHHHIIIIHHIIIIII',
                  0x10b, 1, 0, 0, 0, 0, le.entry, 0x10000, 0x90000,
                  0, SA, FA, 4, 0, 0, 0, 4, 0, 0, size_image, hdr_raw, 0, 3, 0,
                  0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
opt += b'\0' * (16 * 8)
out = bytes(dos) + b'PE\0\0' + coff + opt + table
out += b'\0' * (hdr_raw - len(out)) + blobs
open(sys.argv[2], 'wb').write(out)
print('wrote', sys.argv[2], len(out))
