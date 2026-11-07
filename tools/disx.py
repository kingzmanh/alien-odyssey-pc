"""dis.py EXE addr [count]  - disassemble from a linear address."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from le import LE

le = LE(sys.argv[1])
a = int(sys.argv[2], 16)
n = int(sys.argv[3]) if len(sys.argv) > 3 else 40
md = Cs(CS_ARCH_X86, CS_MODE_32)
fx = {s: t for (s, k, t, ob) in le.fixups}
for ins in md.disasm(bytes(le.mem[a - le.lo:a - le.lo + n * 15]), a):
    print('%08x  %-24s %s %s' % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str))
    n -= 1
    if n <= 0:
        break
