"""Linear-sweep scan of the code object: list INT / IN / OUT sites and
strings near them.  Quick survey tool for the DOS/hardware boundary."""
import sys, collections
from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_MODE_16
sys.path.insert(0, __file__.rsplit('\\', 1)[0].rsplit('/', 1)[0])
from le import LE

le = LE(sys.argv[1])
md = Cs(CS_ARCH_X86, CS_MODE_32)
md.skipdata = True
o = le.objs[0]
code = bytes(le.mem[o.base - le.lo:o.base - le.lo + o.vsize])
ints = collections.Counter()
sites = collections.defaultdict(list)
for ins in md.disasm(code, o.base):
    m = ins.mnemonic
    if m == 'int' or m in ('in', 'out', 'insb', 'insw', 'insd', 'outsb', 'outsw', 'outsd', 'cli', 'sti', 'lidt', 'lgdt'):
        key = m + ' ' + ins.op_str
        ints[key] += 1
        sites[key].append(ins.address)
for k, n in ints.most_common():
    print('%5d  %-28s %s' % (n, k, ' '.join('%x' % a for a in sites[k][:12])))
