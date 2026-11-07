"""Map Miles AIL names to their AIL_API_* implementation functions.
Each AIL_xxx wrapper pushes its debug format string, prints it, then calls
the implementation.  usage: ail_api.py GAME.EXE  -> "addr name" lines"""
import sys, os, re, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from x86an import Prog

p = Prog(sys.argv[1])
mem, lo = bytes(p.le.mem), p.le.lo
strs = {}
for m in re.finditer(rb'AIL_([a-zA-Z0-9_]+)\(', mem):
    strs[m.start() + lo] = 'AIL_' + m.group(1).decode()
sites = sorted((s, strs[t]) for s, t in p.fix.items() if t in strs and p.in_code(s))
res = []
calls_after = []
for s, name in sites:
    a = s - 4
    # find instruction containing the fixup
    for back in range(1, 8):
        i = p.insn(s - back)
        if i and s - back + i.size > s:
            a = s - back
            break
    cs = []
    for _ in range(40):
        i = p.insn(a)
        if i is None:
            break
        if i.mnemonic == 'call' and i.operands[0].type == 2:
            cs.append(i.operands[0].imm & 0xffffffff)
        if i.mnemonic in ('jmp', 'ret') and cs:
            break
        a += i.size
    calls_after.append((name, cs))
trace = collections.Counter(c[0] for n, c in calls_after if c).most_common(1)[0][0]
for name, cs in calls_after:
    api = [c for c in cs if c != trace]
    if api:
        print('%x %s' % (api[0], name))
