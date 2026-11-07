"""Which AIL wrappers does the game code call?  usage: ail_used.py GAME.EXE"""
import sys, os, re, bisect, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from x86an import Prog

p = Prog(sys.argv[1])
mem, lo = bytes(p.le.mem), p.le.lo
strs = {}
for m in re.finditer(rb'AIL_([a-zA-Z0-9_]+)\(', mem):
    strs[m.start() + lo] = 'AIL_' + m.group(1).decode()
sites = sorted((s, strs[t]) for s, t in p.fix.items() if t in strs and p.in_code(s))
saddr = [s for s, n in sites]
wlo, whi = sites[0][0] - 0x80, sites[-1][0] + 0x80
used = collections.Counter()
for o in range(p.clo, p.chi - 5):
    if mem[o - lo] != 0xe8:
        continue
    if wlo <= o < whi:
        continue
    t = (o + 5 + int.from_bytes(mem[o - lo + 1:o - lo + 5], 'little', signed=True)) & 0xffffffff
    if wlo <= t < whi:
        i = bisect.bisect_left(saddr, t)
        if i < len(sites):
            used[(t, sites[i][1])] += 1
for (t, n), c in sorted(used.items()):
    print('%x %s %d' % (t, n, c))
