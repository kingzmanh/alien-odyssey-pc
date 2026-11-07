"""Write every code-object fixup target (one hex address per line) for Ghidra's
SeedFixups.java pre-script.  usage: fixtargets.py GAME.EXE out.txt"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from x86an import Prog

p = Prog(sys.argv[1])
with open(sys.argv[2], 'w') as f:
    for t in sorted(set(p.fix.values())):
        if p.in_code(t):
            f.write('%x\n' % t)
