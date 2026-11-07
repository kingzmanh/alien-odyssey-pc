"""callers.py EXE lo hi : functions in [lo,hi) called from outside the range, with callers"""
import sys, os, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from x86an import Prog, analyze
from capstone import x86 as X
p = Prog(sys.argv[1])
lo, hi = int(sys.argv[2], 16), int(sys.argv[3], 16)
ext = [int(l.split('\t')[0], 16) + 0x400000 for l in open('re/ghidra/functions.tsv')]
funcs, nr, rej = analyze(p, ext)
callers = collections.defaultdict(set)
for e, f in funcs.items():
    for c in f.calls:
        callers[c].add(e)
    for t in f.tailcalls.values():
        callers[t].add(e)
for t in sorted(callers):
    if lo <= t < hi:
        out = sorted(c for c in callers[t] if not (lo <= c < hi))
        if out:
            print('%x  <- %s' % (t, ' '.join('%x' % c for c in out)))
