"""strrefs.py EXE fileoff... : which functions reference the string at each file offset"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from x86an import Prog, analyze
from capstone import x86 as X
p = Prog(sys.argv[1])
le = p.le
data = le.data
def off2lin(off):
    for o in le.objs:
        for pg in range(o.npages):
            fo = le.page_file[o.pmi - 1 + pg]
            if fo <= off < fo + le.page_size:
                return o.base + pg * le.page_size + (off - fo)
    return None
ext = [int(l.split('\t')[0], 16) + 0x400000 for l in open('re/ghidra/functions.tsv')]
funcs, nr, rej = analyze(p, ext)
want = {}
for s in sys.argv[2:]:
    want[off2lin(int(s, 16))] = s
for e, f in sorted(funcs.items()):
    for l in f.blocks.values():
        for a in l:
            ins = p.insn(a)
            for op in ins.operands:
                v = None
                if op.type == X.X86_OP_IMM: v = op.imm & 0xffffffff
                elif op.type == X.X86_OP_MEM: v = op.mem.disp & 0xffffffff
                if v in want:
                    print('str@%s (%x) used at %x in F_%x' % (want[v], v, a, e))
