"""Find code from one program in the others: the bytes of a routine with its
fixed-up absolute addresses masked.  usage: sigfind.py REF.EXE addr len OTHER.EXE..."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from x86an import Prog

ref = Prog(sys.argv[1])
a, n = int(sys.argv[2], 16), int(sys.argv[3], 16)
lo = ref.le.lo
sig = bytearray(ref.le.mem[a - lo:a - lo + n])
mask = bytearray([1] * n)
for s in ref.fix:
    if a <= s < a + n:
        for k in range(4):
            if s + k < a + n:
                mask[s + k - a] = 0
first = next(i for i in range(n) if mask[i])
for exe in sys.argv[4:]:
    p = Prog(exe)
    m = bytes(p.le.mem)
    hits = []
    for o in range(p.clo - p.le.lo, p.chi - p.le.lo - n):
        if m[o + first] != sig[first]:
            continue
        if all(not mask[i] or m[o + i] == sig[i] for i in range(n)):
            hits.append(o + p.le.lo)
    print(os.path.basename(exe), [hex(h) for h in hits])
