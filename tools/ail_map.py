"""Map Miles AIL API wrappers by their debug-trace format strings, using the
recompiler's function list.  usage: ail_map.py GAME.EXE functions.tsv
prints: addr name callers(outside the AIL wrapper block)"""
import sys, os, re, bisect, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from x86an import Prog, analyze

p = Prog(sys.argv[1])
extra = [int(l.split('\t')[0], 16) for l in open(sys.argv[2]) if l.strip()]
funcs, noreturn, rej = analyze(p, extra)
ents = sorted(funcs)
mem, lo = bytes(p.le.mem), p.le.lo
strs = {}
for m in re.finditer(rb'AIL_([a-zA-Z0-9_]+)\(', mem):
    strs[m.start() + lo] = 'AIL_' + m.group(1).decode()
ref = {}
for s, t in p.fix.items():
    if t in strs and p.in_code(s):
        f = ents[bisect.bisect_right(ents, s) - 1]
        ref[f] = strs[t]
lo_w, hi_w = min(ref), max(ref) + 0x200
callers = collections.defaultdict(set)
for e, f in funcs.items():
    for c in f.calls:
        callers[c].add(e)
for f in sorted(ref):
    ext = [c for c in callers[f] if not (lo_w <= c < hi_w)]
    print('%x %s %d' % (f, ref[f], len(ext)))
