"""callsites.py EXE target [n] : show the instructions before each call to target"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from x86an import Prog, analyze
p = Prog(sys.argv[1])
t = int(sys.argv[2], 16)
n = int(sys.argv[3]) if len(sys.argv) > 3 else 6
ext = [int(l.split('\t')[0], 16) + 0x400000 for l in open('re/ghidra/functions.tsv')]
funcs, nr, rej = analyze(p, ext)
seen = set()
for e, f in sorted(funcs.items()):
    for l in f.blocks.values():
        for i, a in enumerate(l):
            ins = p.insn(a)
            if ins.mnemonic in ('call', 'jmp') and ins.operands[0].type == 2 and (ins.operands[0].imm & 0xffffffff) == t and a not in seen:
                seen.add(a)
                print('--- F_%x' % e)
                for b in l[max(0, i - n):i + 2]:
                    q = p.insn(b)
                    print('  %x  %s %s' % (b, q.mnemonic, q.op_str))
