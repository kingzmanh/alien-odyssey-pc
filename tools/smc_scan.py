"""Find self-modifying functions: code that makes a code alias selector
(int 31h ax=000Ah) and writes through it.  usage: smc_scan.py GAME.EXE functions.tsv"""
import sys, os, bisect
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from x86an import Prog

p = Prog(sys.argv[1])
fs = sorted(int(l.split('\t')[0], 16) for l in open(sys.argv[2]) if l.strip())
mem, lo = p.le.mem, p.le.lo
hits = set()
for a in range(p.clo, p.chi - 8):
    o = a - lo
    if mem[o:o + 4] == b'\x66\xb8\x0a\x00':
        for k in range(4, 16):
            if mem[o + k:o + k + 2] == b'\xcd\x31':
                i = bisect.bisect_right(fs, a) - 1
                hits.add(fs[i])
                break
for f in sorted(hits):
    # patched locations: fs:[imm32] stores in the function
    a, pts = f, []
    for _ in range(400):
        ins = p.insn(a)
        if ins is None: break
        if 'fs:[0x' in ins.op_str and ins.mnemonic == 'mov' and ins.op_str.startswith(('dword ptr fs', 'byte ptr fs', 'word ptr fs')):
            pts.append(ins.op_str.split('[')[1].split(']')[0])
        if ins.mnemonic == 'ret' and pts and a > f + 8:
            break
        a += ins.size
    print('%x end %x patches %s' % (f, a, ' '.join(pts)))
