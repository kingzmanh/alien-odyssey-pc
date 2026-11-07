"""x86-32 -> C static recompiler for ALIENSP2.EXE (Aliens 1.03US, Watcom/PMODE-W).

usage: recomp.py GAME.EXE ghidra_functions.tsv native.txt outdir module

Alien Odyssey has six programs in one Windows exe: every module's functions
are named <module>_<addr> and its table is <module>_functab.

Every discovered function becomes a C function `void F_<addr>(void)` working
on the global register file `cpu` (src/rt/cpu.h).  Registers and flags are
cached in C locals inside a function and synced at calls / returns, so the C
compiler can keep them in host registers.  Flag computations are emitted only
where a later instruction can read them (per-function liveness, plus an
interprocedural pass for flags returned to callers).

native.txt lists functions that are replaced by hand-written PC code
(DOS, DPMI, VESA, sound library ...): `<hex addr> <C name>` per line.
"""
import sys, os, re, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capstone import x86 as X
from x86an import Prog, analyze, JCC

CF, ZF, SF, OF, PF, AF = 1, 2, 4, 8, 16, 32
ALL = 63
FNAME = {CF: 'cf', ZF: 'zf', SF: 'sf', OF: 'of', PF: 'pf', AF: 'af'}
R32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
R16 = {'ax': 'eax', 'cx': 'ecx', 'dx': 'edx', 'bx': 'ebx', 'sp': 'esp', 'bp': 'ebp', 'si': 'esi', 'di': 'edi'}
R8L = {'al': 'eax', 'cl': 'ecx', 'dl': 'edx', 'bl': 'ebx'}
R8H = {'ah': 'eax', 'ch': 'ecx', 'dh': 'edx', 'bh': 'ebx'}
SEGS = {'es': 0, 'cs': 1, 'ss': 2, 'ds': 3, 'fs': 4, 'gs': 5}
MASK = {1: '0xffu', 2: '0xffffu', 4: '0xffffffffu'}
SBIT = {1: '0x80u', 2: '0x8000u', 4: '0x80000000u'}
CTYPE = {1: 'u8', 2: 'u16', 4: 'u32'}
SCTYPE = {1: 's8', 2: 's16', 4: 's32'}

COND = {
    'o': ('of', OF), 'no': ('!of', OF), 'b': ('cf', CF), 'ae': ('!cf', CF),
    'e': ('zf', ZF), 'ne': ('!zf', ZF), 'be': ('(cf|zf)', CF | ZF), 'a': ('!(cf|zf)', CF | ZF),
    's': ('sf', SF), 'ns': ('!sf', SF), 'p': ('pf', PF), 'np': ('!pf', PF),
    'l': ('(sf!=of)', SF | OF), 'ge': ('(sf==of)', SF | OF),
    'le': ('(zf||(sf!=of))', ZF | SF | OF), 'g': ('(!zf&&(sf==of))', ZF | SF | OF),
}


def cond_of(m, prefix):
    return COND[m[len(prefix):]]


def flag_use_def(p, ins, rets_live):
    """(use, def) flag masks for an instruction"""
    m = ins.mnemonic.replace('rep ', '').replace('repne ', '').replace('repe ', '').replace('lock ', '')
    if m in JCC:
        if m.startswith('loop'):
            return (ZF if m in ('loope', 'loopne') else 0), 0
        if m in ('jcxz', 'jecxz'):
            return 0, 0
        return cond_of(m, 'j')[1], 0
    if m.startswith('set'):
        return cond_of(m, 'set')[1], 0
    if m.startswith('cmov'):
        return cond_of(m, 'cmov')[1], 0
    if m in ('add', 'sub', 'cmp', 'neg', 'scasb', 'scasw', 'scasd', 'cmpsb', 'cmpsw', 'cmpsd',
             'and', 'or', 'xor', 'test', 'mul', 'imul', 'div', 'idiv', 'xadd', 'cmpxchg'):
        if m in ('scasb', 'scasw', 'scasd', 'cmpsb', 'cmpsw', 'cmpsd') and (ins.bytes[0] in (0xf2, 0xf3) or ins.prefix[0] in (0xf2, 0xf3)):
            return ALL, ALL  # may execute zero times
        return 0, ALL
    if m in ('adc', 'sbb'):
        return CF, ALL
    if m in ('inc', 'dec'):
        return 0, ALL & ~CF
    if m in ('shl', 'sal', 'shr', 'sar', 'shld', 'shrd'):
        if ins.operands[-1].type == X.X86_OP_IMM:
            return 0, ALL
        return ALL, ALL
    if m in ('rol', 'ror'):
        if ins.operands[-1].type == X.X86_OP_IMM:
            return 0, CF | OF
        return CF | OF, CF | OF
    if m in ('rcl', 'rcr'):
        return CF | OF, CF | OF
    if m in ('bt', 'bts', 'btr', 'btc'):
        return 0, CF
    if m in ('bsr', 'bsf'):
        return 0, ZF
    if m in ('stc', 'clc'):
        return 0, CF
    if m == 'cmc':
        return CF, CF
    if m == 'sahf':
        return 0, CF | ZF | SF | PF | AF
    if m == 'lahf':
        return CF | ZF | SF | PF | AF, 0
    if m in ('pushfd', 'pushf'):
        return ALL, 0
    if m in ('popfd', 'popf'):
        return 0, ALL
    if m in ('daa', 'das', 'aaa', 'aas'):
        return CF | AF, ALL
    if m == 'salc':
        return CF, 0
    if m in ('call', 'lcall'):
        return 0, ALL
    if m == 'int':
        return ALL, ALL  # handlers may look at / return flags
    if m in ('ret', 'retf'):
        return rets_live, 0
    if m == 'jmp':
        return ALL, 0
    return 0, 0


# functions whose entry/exit registers are logged (tools/trace.txt, debugging)
TRACE = set()
_tp = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'trace.txt')
if os.path.exists(_tp):
    for _l in open(_tp):
        _l = _l.split('#')[0].strip()
        if _l:
            TRACE.add(int(_l, 16))


def is_far(ins):
    """FF /3 (call m16:32) or FF /5 (jmp m16:32)"""
    b = list(ins.bytes)
    i = 0
    while i < len(b) and b[i] in (0xf0, 0xf2, 0xf3, 0x2e, 0x36, 0x3e, 0x26, 0x64, 0x65, 0x66, 0x67):
        i += 1
    if i + 1 < len(b) and b[i] == 0xff:
        reg = (b[i + 1] >> 3) & 7
        return reg in (3, 5)
    return False


class Gen:
    def __init__(self, p, funcs, native, noreturn):
        self.p = p
        self.funcs = funcs
        self.native = native
        self.noreturn = noreturn
        self.rlive = collections.defaultdict(int)  # flags live after return, per function
        self.unimpl = collections.Counter()
        self.ds_based = False

    # ---------- operands ----------
    def reg_rd(self, name):
        if name in R32:
            return name
        if name in R16:
            return '((u16)%s)' % R16[name]
        if name in R8L:
            return '((u8)%s)' % R8L[name]
        if name in R8H:
            return '((u8)(%s>>8))' % R8H[name]
        if name in SEGS:
            return 'cpu.sreg[%d]' % SEGS[name]
        raise KeyError(name)

    def reg_wr(self, name, v):
        if name in R32:
            return '%s = (u32)(%s);' % (name, v)
        if name in R16:
            r = R16[name]
            return '%s = (%s & 0xffff0000u) | ((u32)(%s) & 0xffffu);' % (r, r, v)
        if name in R8L:
            r = R8L[name]
            return '%s = (%s & 0xffffff00u) | ((u32)(%s) & 0xffu);' % (r, r, v)
        if name in R8H:
            r = R8H[name]
            return '%s = (%s & 0xffff00ffu) | (((u32)(%s) & 0xffu) << 8);' % (r, r, v)
        if name in SEGS:
            return 'rt_load_seg(%d, (u16)(%s));' % (SEGS[name], v)
        raise KeyError(name)

    def addr(self, ins, mem):
        parts = []
        if mem.base:
            parts.append(self.reg_rd(ins.reg_name(mem.base)))
        if mem.index:
            ix = self.reg_rd(ins.reg_name(mem.index))
            parts.append('%s*%d' % (ix, mem.scale) if mem.scale != 1 else ix)
        d = mem.disp & 0xffffffff
        if d or not parts:
            parts.append('0x%xu' % d)
        e = '(u32)(' + '+'.join(parts) + ')'
        sn = ins.reg_name(mem.segment) if mem.segment else None
        if sn in ('es', 'fs', 'gs'):
            e = '(u32)(%s+cpu.sbase[%d])' % (e, SEGS[sn])
        elif self.ds_based and (sn == 'ds' or (sn is None and not (
                mem.base and ins.reg_name(mem.base) in ('esp', 'ebp')))):
            # this function loads ds with a non-flat selector (startup code)
            e = '(u32)(%s+cpu.sbase[3])' % e
        return e

    def rd(self, ins, op, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            return self.reg_rd(ins.reg_name(op.reg))
        if op.type == X.X86_OP_IMM:
            return '0x%xu' % (op.imm & int(MASK[size][:-1], 16))
        if op.type == X.X86_OP_MEM:
            return 'RD%d(%s)' % (size * 8, self.addr(ins, op.mem))
        raise ValueError

    def wr(self, ins, op, v, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            return self.reg_wr(ins.reg_name(op.reg), v)
        if op.type == X.X86_OP_MEM:
            return 'WR%d(%s, %s);' % (size * 8, self.addr(ins, op.mem), v)
        raise ValueError

    # ---------- flags ----------
    @staticmethod
    def szp(live, r, size):
        s = []
        if live & ZF:
            s.append('zf = ((%s) == 0);' % r)
        if live & SF:
            s.append('sf = (((%s) & %s) != 0);' % (r, SBIT[size]))
        if live & PF:
            s.append('pf = PARITY(%s);' % r)
        return ' '.join(s)

    def arith(self, ins, live, kind, a_op, b_expr, write=True, carry_in=None):
        size = a_op.size
        M = MASK[size]
        a = self.rd(ins, a_op)
        out = ['{ u32 a = %s, b = (u32)(%s) & %s;' % (a, b_expr, M)]
        cin = carry_in or '0'
        if kind == 'add':
            out.append('u64 t = (u64)a + b + %s; u32 r = (u32)t & %s;' % (cin, M))
        else:
            out.append('u64 t = (u64)a - b - %s; u32 r = (u32)t & %s;' % (cin, M))
        if write:
            out.append(self.wr(ins, a_op, 'r'))
        if live & CF:
            out.append('cf = (u32)(t >> %d) & 1;' % (size * 8))
        if live & OF:
            if kind == 'add':
                out.append('of = (((a ^ r) & (b ^ r) & %s) != 0);' % SBIT[size])
            else:
                out.append('of = (((a ^ b) & (a ^ r) & %s) != 0);' % SBIT[size])
        if live & AF:
            out.append('af = (((a ^ b ^ r) & 0x10) != 0);')
        out.append(self.szp(live, 'r', size))
        out.append('}')
        return ' '.join(out)

    def logic(self, ins, live, op, a_op, b_expr, write=True):
        size = a_op.size
        out = ['{ u32 r = (u32)(%s) %s (u32)(%s);' % (self.rd(ins, a_op), op, b_expr)]
        if write:
            out.append(self.wr(ins, a_op, 'r'))
        if live & CF:
            out.append('cf = 0;')
        if live & OF:
            out.append('of = 0;')
        if live & AF:
            out.append('af = 0;')
        out.append(self.szp(live, 'r & %s' % MASK[size] if size < 4 else 'r', size))
        out.append('}')
        return ' '.join(out)

    # ---------- per instruction ----------
    def call_direct(self, t, ret_addr):
        name = self.fname(t)
        return 'esp -= 4; WR32(esp, 0x%xu); SYNC_OUT; %s(); SYNC_IN;' % (ret_addr, name)

    def fname(self, t):
        if t in self.native:
            return self.native[t]
        if t in self.funcs:
            return PFX + '%x' % t
        return None

    def string_op(self, ins, m, rep):
        base = m[:-1]
        sz = {'b': 1, 'w': 2, 'd': 4}[m[-1]]
        bits = sz * 8
        step = 'DSTEP(%d)' % sz
        es = '(u32)(edi+cpu.sbase[0])'
        dsrc = '(u32)(esi+cpu.sbase[3])' if self.ds_based else 'esi'
        seg = None
        for op in ins.operands:
            if op.type == X.X86_OP_MEM and op.mem.segment and ins.reg_name(op.mem.segment) in ('fs', 'gs', 'es') \
                    and op.mem.base == X.X86_REG_ESI:
                seg = ins.reg_name(op.mem.segment)
        if seg:
            dsrc = '(u32)(esi+cpu.sbase[%d])' % SEGS[seg]
        acc = {1: 'al', 2: 'ax', 4: 'eax'}[sz]
        if base == 'movs':
            body = 'WR%d(%s, RD%d(%s)); esi += %s; edi += %s;' % (bits, es, bits, dsrc, step, step)
            if rep and sz == 4 and not self.ds_based:
                return 'REP_MOVSD(%s);' % ('cpu.sbase[0]',) if not seg else 'while (ecx) { %s ecx--; }' % body
            if rep and sz == 1 and not seg and not self.ds_based:
                return 'REP_MOVSB();'
        elif base == 'stos':
            body = 'WR%d(%s, %s); edi += %s;' % (bits, es, self.reg_rd(acc), step)
            if rep and sz == 4:
                return 'REP_STOSD();'
            if rep and sz == 1:
                return 'REP_STOSB();'
        elif base == 'lods':
            body = '%s esi += %s;' % (self.reg_wr(acc, 'RD%d(%s)' % (bits, dsrc)), step)
        elif base == 'scas':
            body = ('{ u32 a = %s, b = RD%d(%s); u32 r = (a - b) & %s; cf = a < b; zf = r == 0; sf = (r & %s) != 0; '
                    'of = (((a ^ b) & (a ^ r) & %s) != 0); pf = PARITY(r); af = (((a ^ b ^ r) & 0x10) != 0); edi += %s; }'
                    % (self.reg_rd(acc), bits, es, MASK[sz], SBIT[sz], SBIT[sz], step))
        elif base == 'cmps':
            body = ('{ u32 a = RD%d(%s), b = RD%d(%s); u32 r = (a - b) & %s; cf = a < b; zf = r == 0; sf = (r & %s) != 0; '
                    'of = (((a ^ b) & (a ^ r) & %s) != 0); pf = PARITY(r); af = (((a ^ b ^ r) & 0x10) != 0); esi += %s; edi += %s; }'
                    % (bits, dsrc, bits, es, MASK[sz], SBIT[sz], SBIT[sz], step, step))
        else:
            return None
        if not rep:
            return body
        if base in ('scas', 'cmps'):
            stop = 'if (!zf) break;' if rep == 'repe' else 'if (zf) break;'
            return 'while (ecx) { %s ecx--; %s }' % (body, stop)
        return 'while (ecx) { %s ecx--; }' % body

    def insn(self, f, ins, live_after, next_addr):
        """C code for one instruction. live_after: flags live after it."""
        p = self.p
        mn = ins.mnemonic
        rep = None
        for pre in ('rep ', 'repe ', 'repne ', 'lock '):
            if mn.startswith(pre):
                rep = pre.strip()
                mn = mn[len(pre):]
        ops = ins.operands
        L = live_after
        a = ins.address

        if mn == 'mov' and any(o.type == X.X86_OP_REG and ins.reg_name(o.reg).startswith(('cr', 'dr')) for o in ops):
            if ops[0].type == X.X86_OP_REG and ins.reg_name(ops[0].reg).startswith(('cr', 'dr')):
                return '; /* write to %s ignored */' % ins.reg_name(ops[0].reg)
            return self.wr(ins, ops[0], '0x%xu' % (0x31 if ins.reg_name(ops[1].reg) == 'cr0' else 0))
        if mn == 'mov' or mn == 'movabs':
            return self.wr(ins, ops[0], self.rd(ins, ops[1], ops[0].size if ops[1].type == X.X86_OP_IMM else None))
        if mn == 'movzx':
            return self.wr(ins, ops[0], self.rd(ins, ops[1]))
        if mn == 'movsx':
            s = ops[1].size
            return self.wr(ins, ops[0], '(u32)(s32)(%s)(%s)' % (SCTYPE[s], self.rd(ins, ops[1])))
        if mn == 'lea':
            v = self.addr(ins, ops[1].mem)
            return self.wr(ins, ops[0], v)
        if mn == 'push':
            o = ops[0]
            if o.type == X.X86_OP_REG and ins.reg_name(o.reg) in SEGS:
                return 'esp -= 4; WR32(esp, %s);' % self.reg_rd(ins.reg_name(o.reg))
            if o.size == 2:
                return '{ u32 v = %s; esp -= 2; WR16(esp, v); }' % self.rd(ins, o)
            if o.type == X.X86_OP_IMM:
                return 'esp -= 4; WR32(esp, 0x%xu);' % (o.imm & 0xffffffff)
            return '{ u32 v = %s; esp -= 4; WR32(esp, v); }' % self.rd(ins, o, 4)
        if mn == 'pop':
            o = ops[0]
            if o.type == X.X86_OP_REG and ins.reg_name(o.reg) in SEGS:
                return '{ u32 v = RD32(esp); esp += 4; %s }' % self.reg_wr(ins.reg_name(o.reg), 'v')
            if o.size == 2:
                return '{ u32 v = RD16(esp); esp += 2; %s }' % self.wr(ins, o, 'v')
            if o.type == X.X86_OP_REG and ins.reg_name(o.reg) == 'esp':
                return 'esp = RD32(esp);'
            return '{ u32 v = RD32(esp); esp += 4; %s }' % self.wr(ins, o, 'v')
        if mn in ('pushal', 'pushad'):
            return ('{ u32 t = esp; esp -= 32; WR32(esp+28, eax); WR32(esp+24, ecx); WR32(esp+20, edx); WR32(esp+16, ebx);'
                    ' WR32(esp+12, t); WR32(esp+8, ebp); WR32(esp+4, esi); WR32(esp, edi); }')
        if mn in ('popal', 'popad'):
            return ('{ edi = RD32(esp); esi = RD32(esp+4); ebp = RD32(esp+8); ebx = RD32(esp+16); edx = RD32(esp+20);'
                    ' ecx = RD32(esp+24); eax = RD32(esp+28); esp += 32; }')
        if mn in ('pushfd', 'pushf'):
            return 'esp -= 4; WR32(esp, PACK_FLAGS());'
        if mn in ('popfd', 'popf'):
            return '{ u32 v = RD32(esp); esp += 4; UNPACK_FLAGS(v); }'
        if mn == 'lahf':
            return 'eax = (eax & 0xffff00ffu) | ((PACK_FLAGS() & 0xff) << 8);'
        if mn == 'sahf':
            return '{ u32 v = (eax >> 8) & 0xff; cf = v & 1; pf = (v >> 2) & 1; af = (v >> 4) & 1; zf = (v >> 6) & 1; sf = (v >> 7) & 1; }'
        if mn in ('add', 'sub', 'cmp'):
            kind = 'add' if mn == 'add' else 'sub'
            if mn == 'sub' and ops[1].type == X.X86_OP_REG and ops[0].type == X.X86_OP_REG and ops[0].reg == ops[1].reg:
                s = self.wr(ins, ops[0], '0')
                fl = []
                if L & CF: fl.append('cf = 0;')
                if L & OF: fl.append('of = 0;')
                if L & AF: fl.append('af = 0;')
                if L & ZF: fl.append('zf = 1;')
                if L & SF: fl.append('sf = 0;')
                if L & PF: fl.append('pf = 1;')
                return s + ' ' + ' '.join(fl)
            if not L and mn != 'cmp':
                M = MASK[ops[0].size]
                opch = '+' if kind == 'add' else '-'
                return self.wr(ins, ops[0], '(%s %s %s) & %s' % (self.rd(ins, ops[0]), opch, self.rd(ins, ops[1], ops[0].size), M))
            if not L and mn == 'cmp':
                return ';'
            return self.arith(ins, L, kind, ops[0], self.rd(ins, ops[1], ops[0].size), write=(mn != 'cmp'))
        if mn in ('adc', 'sbb'):
            kind = 'add' if mn == 'adc' else 'sub'
            return self.arith(ins, L, kind, ops[0], self.rd(ins, ops[1], ops[0].size), True, 'cf')
        if mn in ('and', 'or', 'xor', 'test'):
            op = {'and': '&', 'or': '|', 'xor': '^', 'test': '&'}[mn]
            if mn == 'xor' and ops[1].type == X.X86_OP_REG and ops[0].type == X.X86_OP_REG and ops[0].reg == ops[1].reg:
                fl = [self.wr(ins, ops[0], '0')]
                if L & CF: fl.append('cf = 0;')
                if L & OF: fl.append('of = 0;')
                if L & AF: fl.append('af = 0;')
                if L & ZF: fl.append('zf = 1;')
                if L & SF: fl.append('sf = 0;')
                if L & PF: fl.append('pf = 1;')
                return ' '.join(fl)
            if not L:
                if mn == 'test':
                    return ';'
                return self.wr(ins, ops[0], '%s %s %s' % (self.rd(ins, ops[0]), op, self.rd(ins, ops[1], ops[0].size)))
            return self.logic(ins, L, op, ops[0], self.rd(ins, ops[1], ops[0].size), write=(mn != 'test'))
        if mn in ('inc', 'dec'):
            size = ops[0].size
            M = MASK[size]
            if not L:
                return self.wr(ins, ops[0], '(%s %s 1) & %s' % (self.rd(ins, ops[0]), '+' if mn == 'inc' else '-', M))
            o = ['{ u32 a = %s; u32 r = (a %s 1) & %s;' % (self.rd(ins, ops[0]), '+' if mn == 'inc' else '-', M),
                 self.wr(ins, ops[0], 'r')]
            if L & OF:
                o.append('of = (r == %s);' % (SBIT[size] if mn == 'inc' else '(%s - 1u)' % SBIT[size]))
            if L & AF:
                o.append('af = (((a ^ 1 ^ r) & 0x10) != 0);')
            o.append(self.szp(L, 'r', size))
            o.append('}')
            return ' '.join(o)
        if mn == 'neg':
            size = ops[0].size
            M = MASK[size]
            o = ['{ u32 b = %s; u32 r = (0u - b) & %s;' % (self.rd(ins, ops[0]), M), self.wr(ins, ops[0], 'r')]
            if L & CF: o.append('cf = (b != 0);')
            if L & OF: o.append('of = (b == %s);' % SBIT[size])
            if L & AF: o.append('af = (((b ^ r) & 0x10) != 0);')
            o.append(self.szp(L, 'r', size))
            o.append('}')
            return ' '.join(o)
        if mn == 'not':
            return self.wr(ins, ops[0], '~%s' % self.rd(ins, ops[0]))
        if mn in ('shl', 'sal', 'shr', 'sar', 'rol', 'ror', 'rcl', 'rcr'):
            size = ops[0].size
            bits = size * 8
            M = MASK[size]
            if len(ops) == 1:
                cnt = '1'
            else:
                cnt = '(%s & 31)' % self.rd(ins, ops[1], 1)
            T = CTYPE[size]
            o = ['{ u32 c = %s; if (c) { u32 a = %s, r;' % (cnt, self.rd(ins, ops[0]))]
            if mn in ('shl', 'sal'):
                o.append('r = (u32)((u64)a << c) & %s;' % M)
                if L & CF: o.append('cf = (c <= %d) ? ((a >> (%d - c)) & 1) : 0;' % (bits, bits))
                if L & OF: o.append('of = (((r & %s) != 0) != cf);' % SBIT[size] if L & CF else
                                    'of = (((r ^ (a << 1)) & %s) != 0);' % SBIT[size])
            elif mn == 'shr':
                o.append('r = (c < %d) ? (a >> c) : 0;' % bits)
                if L & CF: o.append('cf = (c <= %d) ? ((a >> (c - 1)) & 1) : 0;' % bits)
                if L & OF: o.append('of = ((a & %s) != 0);' % SBIT[size])
            elif mn == 'sar':
                o.append('{ s32 sa = (s32)(%s)a; r = (u32)(sa >> (c < %d ? c : %d)) & %s; ' % (SCTYPE[size], bits, bits - 1, M))
                if L & CF: o.append('cf = (u32)(sa >> (c - 1 < %d ? c - 1 : %d)) & 1;' % (bits, bits - 1))
                o.append('}')
                if L & OF: o.append('of = 0;')
            elif mn == 'rol':
                o.append('{ u32 k = c %% %d; r = k ? (((a << k) | (a >> (%d - k))) & %s) : a; }' % (bits, bits, M))
                if L & CF: o.append('cf = r & 1;')
                if L & OF: o.append('of = (((r & %s) != 0) ^ (r & 1));' % SBIT[size])
            elif mn == 'ror':
                o.append('{ u32 k = c %% %d; r = k ? (((a >> k) | (a << (%d - k))) & %s) : a; }' % (bits, bits, M))
                if L & CF: o.append('cf = (r & %s) != 0;' % SBIT[size])
                if L & OF: o.append('of = (((r ^ (r << 1)) & %s) != 0);' % SBIT[size])
            elif mn in ('rcl', 'rcr'):
                o.append('{ u32 k = c %% %d; r = a; u32 cc = cf; while (k--) {' % (bits + 1))
                if mn == 'rcl':
                    o.append('u32 nc = (r & %s) != 0; r = ((r << 1) | cc) & %s; cc = nc; } cf = cc;' % (SBIT[size], M))
                    if L & OF: o.append('of = (((r & %s) != 0) ^ cf);' % SBIT[size])
                else:
                    o.append('u32 nc = r & 1; r = (r >> 1) | (cc ? %s : 0); cc = nc; } cf = cc;' % SBIT[size])
                    if L & OF: o.append('of = (((r ^ (r << 1)) & %s) != 0);' % SBIT[size])
                o.append('}')
            o.append(self.wr(ins, ops[0], 'r'))
            if mn in ('shl', 'sal', 'shr', 'sar'):
                if L & AF: o.append('af = 0;')
                o.append(self.szp(L, 'r', size))
            o.append('} }')
            return ' '.join(o)
        if mn in ('shld', 'shrd'):
            size = ops[0].size
            bits = size * 8
            cnt = '(%s & 31)' % self.rd(ins, ops[2], 1)
            d = self.rd(ins, ops[0]); s = self.rd(ins, ops[1])
            if size == 4:
                if mn == 'shld':
                    e = '(u32)((((u64)a << 32) | b) << c >> 32)'
                    cfx = '(a >> (32 - c)) & 1'
                else:
                    e = '(u32)((((u64)b << 32) | a) >> c)'
                    cfx = '(a >> (c - 1)) & 1'
            else:   # 16-bit: counts above 16 are undefined on x86, keep them in range
                if mn == 'shld':
                    e = '(u32)(((((u32)a << 16) | b) << (c > 16 ? 16 : c)) >> 16) & 0xffffu'
                    cfx = '(c <= 16 ? (a >> (16 - c)) & 1 : 0)'
                else:
                    e = '(u32)(((((u32)b << 16) | a) >> (c > 16 ? 16 : c))) & 0xffffu'
                    cfx = '((a >> (c - 1)) & 1)'
            o = ['{ u32 c = %s; if (c) { u32 a = %s, b = %s; u32 r = %s;' % (cnt, d, s, e)]
            if L & CF: o.append('cf = %s;' % cfx)
            if L & OF: o.append('of = ((a ^ r) >> %d) & 1;' % (bits - 1))
            o.append(self.szp(L, 'r', size))
            o.append(self.wr(ins, ops[0], 'r'))
            o.append('} }')
            return ' '.join(o)
        if mn in ('mul', 'imul'):
            if len(ops) == 1:
                size = ops[0].size
                v = self.rd(ins, ops[0])
                if mn == 'mul':
                    if size == 1:
                        return '{ u32 r = (u32)(u8)eax * (u32)(%s); eax = (eax & 0xffff0000u) | (r & 0xffff); cf = of = (r >> 8) != 0; }' % v
                    if size == 2:
                        return '{ u32 r = (u32)(u16)eax * (u32)(%s); eax = (eax & 0xffff0000u) | (r & 0xffff); edx = (edx & 0xffff0000u) | (r >> 16); cf = of = (r >> 16) != 0; }' % v
                    return '{ u64 r = (u64)eax * (u64)(%s); eax = (u32)r; edx = (u32)(r >> 32); cf = of = edx != 0; }' % v
                else:
                    if size == 1:
                        return '{ s32 r = (s32)(s8)eax * (s32)(s8)(%s); eax = (eax & 0xffff0000u) | ((u32)r & 0xffff); cf = of = (r != (s8)r); }' % v
                    if size == 2:
                        return '{ s32 r = (s32)(s16)eax * (s32)(s16)(%s); eax = (eax & 0xffff0000u) | ((u32)r & 0xffff); edx = (edx & 0xffff0000u) | (((u32)r >> 16) & 0xffff); cf = of = (r != (s16)r); }' % v
                    return '{ s64 r = (s64)(s32)eax * (s64)(s32)(%s); eax = (u32)r; edx = (u32)((u64)r >> 32); cf = of = (r != (s32)r); }' % v
            size = ops[0].size
            if len(ops) == 2:
                a1, b1 = self.rd(ins, ops[0]), self.rd(ins, ops[1], size)
            else:
                a1, b1 = self.rd(ins, ops[1]), self.rd(ins, ops[2], size)
            if size == 4:
                o = '{ s64 r = (s64)(s32)(%s) * (s64)(s32)(%s); %s' % (a1, b1, self.wr(ins, ops[0], '(u32)r'))
                if L & (CF | OF): o += ' cf = of = (r != (s32)r);'
            else:
                o = '{ s32 r = (s32)(s16)(%s) * (s32)(s16)(%s); %s' % (a1, b1, self.wr(ins, ops[0], '(u32)r'))
                if L & (CF | OF): o += ' cf = of = (r != (s16)r);'
            if L & (ZF | SF | PF):
                o += ' ' + self.szp(L, '((u32)r & %s)' % MASK[size], size)
            return o + ' }'
        if mn in ('div', 'idiv'):
            size = ops[0].size
            v = self.rd(ins, ops[0])
            if mn == 'div':
                if size == 4:
                    return ('{ u32 d = %s; u64 n = ((u64)edx << 32) | eax; if (!d || n / d > 0xffffffffu) rt_divide_error(0x%x); '
                            'eax = (u32)(n / d); edx = (u32)(n %% d); }' % (v, a))
                if size == 2:
                    return ('{ u32 d = %s; u32 n = ((edx & 0xffff) << 16) | (eax & 0xffff); if (!d || n / d > 0xffff) rt_divide_error(0x%x); '
                            'eax = (eax & 0xffff0000u) | (n / d); edx = (edx & 0xffff0000u) | (n %% d); }' % (v, a))
                return ('{ u32 d = %s; u32 n = eax & 0xffff; if (!d || n / d > 0xff) rt_divide_error(0x%x); '
                        'eax = (eax & 0xffff0000u) | ((n %% d) << 8) | (n / d); }' % (v, a))
            if size == 4:
                return ('{ s32 d = (s32)%s; s64 n = (s64)(((u64)edx << 32) | eax); if (!d) rt_divide_error(0x%x); '
                        's64 q = n / d; if (q != (s32)q) rt_divide_error(0x%x); eax = (u32)q; edx = (u32)(s32)(n %% d); }' % (v, a, a))
            if size == 2:
                return ('{ s32 d = (s16)%s; s32 n = (s32)(((edx & 0xffff) << 16) | (eax & 0xffff)); if (!d) rt_divide_error(0x%x); '
                        's32 q = n / d; eax = (eax & 0xffff0000u) | ((u32)q & 0xffff); edx = (edx & 0xffff0000u) | ((u32)(n %% d) & 0xffff); }' % (v, a))
            return ('{ s32 d = (s8)%s; s32 n = (s16)eax; if (!d) rt_divide_error(0x%x); '
                    's32 q = n / d; eax = (eax & 0xffff0000u) | (((u32)(n %% d) & 0xff) << 8) | ((u32)q & 0xff); }' % (v, a))
        if mn == 'cwde':
            return 'eax = (u32)(s32)(s16)eax;'
        if mn == 'cbw':
            return 'eax = (eax & 0xffff0000u) | ((u32)(s16)(s8)eax & 0xffff);'
        if mn == 'cwd':
            return 'edx = (edx & 0xffff0000u) | (((s16)eax < 0) ? 0xffffu : 0);'
        if mn == 'cdq':
            return 'edx = ((s32)eax < 0) ? 0xffffffffu : 0;'
        if mn == 'xchg':
            if ops[0].type == X.X86_OP_REG and ops[1].type == X.X86_OP_REG and ops[0].reg == ops[1].reg:
                return ';'
            return '{ u32 t = %s; %s %s }' % (self.rd(ins, ops[0]), self.wr(ins, ops[0], self.rd(ins, ops[1])), self.wr(ins, ops[1], 't'))
        if mn == 'xadd':
            return '{ u32 t = %s; %s %s }' % (self.rd(ins, ops[0]), self.arith(ins, L, 'add', ops[0], self.rd(ins, ops[1])), self.wr(ins, ops[1], 't'))
        if mn in ('nop', 'wait', 'fwait', 'cli', 'sti', 'hlt'):
            return ';'
        if mn == 'cld':
            return 'cpu.df = 0;'
        if mn == 'std':
            return 'cpu.df = 1;'
        if mn == 'stc':
            return 'cf = 1;'
        if mn == 'clc':
            return 'cf = 0;'
        if mn == 'cmc':
            return 'cf ^= 1;'
        if mn == 'salc':
            return 'eax = (eax & 0xffffff00u) | (cf ? 0xff : 0);'
        if mn == 'leave':
            return 'esp = ebp; ebp = RD32(esp); esp += 4;'
        if mn == 'enter':
            sz = ops[0].imm
            lvl = ops[1].imm
            if lvl != 0:
                return 'rt_unimpl(0x%x, "enter level");' % a
            return 'esp -= 4; WR32(esp, ebp); ebp = esp; esp -= %d;' % sz
        if mn.startswith('set'):
            c = cond_of(mn, 'set')[0]
            return self.wr(ins, ops[0], '(%s) ? 1 : 0' % c)
        if mn.startswith('cmov'):
            c = cond_of(mn, 'cmov')[0]
            return 'if (%s) { %s }' % (c, self.wr(ins, ops[0], self.rd(ins, ops[1])))
        if mn in ('bsr', 'bsf'):
            v = self.rd(ins, ops[0 + 1])
            fn = '31 - __builtin_clz' if mn == 'bsr' else '__builtin_ctz'
            return '{ u32 v = %s; zf = (v == 0); if (v) { %s } }' % (v, self.wr(ins, ops[0], '%s(v)' % fn))
        if mn in ('bt', 'bts', 'btr', 'btc'):
            size = ops[0].size
            bitn = '(%s & %d)' % (self.rd(ins, ops[1]), size * 8 - 1)
            if ops[0].type == X.X86_OP_MEM and ops[1].type == X.X86_OP_REG:
                return 'rt_unimpl(0x%x, "bt mem,reg");' % a
            o = '{ u32 n = %s; u32 v = %s; cf = (v >> n) & 1;' % (bitn, self.rd(ins, ops[0]))
            if mn == 'bts': o += self.wr(ins, ops[0], 'v | (1u << n)')
            if mn == 'btr': o += self.wr(ins, ops[0], 'v & ~(1u << n)')
            if mn == 'btc': o += self.wr(ins, ops[0], 'v ^ (1u << n)')
            return o + ' }'
        if mn in ('movsb', 'movsw', 'movsd', 'stosb', 'stosw', 'stosd', 'lodsb', 'lodsw', 'lodsd',
                  'scasb', 'scasw', 'scasd', 'cmpsb', 'cmpsw', 'cmpsd'):
            # take the repeat prefix from the encoding: capstone drops it from
            # the mnemonic for F2 A5 ("repne movsd", which acts as rep movsd)
            p0 = 0
            for bt in ins.bytes:
                if bt in (0xf2, 0xf3):
                    p0 = bt
                elif bt not in (0xf0, 0x2e, 0x36, 0x3e, 0x26, 0x64, 0x65, 0x66, 0x67):
                    break
            if p0 in (0xf2, 0xf3):
                if mn.startswith(('scas', 'cmps')):
                    rep = 'repne' if p0 == 0xf2 else 'repe'
                else:
                    rep = 'rep'
            else:
                rep = None
            r = self.string_op(ins, mn, rep)
            if r:
                return r
        if mn in ('lsl', 'lar'):
            sel = self.rd(ins, ops[1], 2)
            v = 'rt_sel_limit(%s)' % sel if mn == 'lsl' else '0x00cff300u'
            return 'zf = 1; %s' % self.wr(ins, ops[0], v)
        if mn == 'xlatb':
            return 'eax = (eax & 0xffffff00u) | RD8((u32)(ebx + (u8)eax));'
        if mn in ('lgs', 'lfs', 'les', 'lds', 'lss'):
            seg = mn[1:]
            ad = self.addr(ins, ops[1].mem)
            return '{ u32 ad = %s; %s %s }' % (ad, self.wr(ins, ops[0], 'RD32(ad)'), self.reg_wr(seg, 'RD16(ad + 4)'))
        if mn == 'int':
            n = ops[0].imm & 0xff
            return 'SYNC_OUT; rt_int(0x%x, 0x%x); SYNC_IN;' % (n, a)
        if mn in ('outsb', 'outsw', 'outsd'):
            sz = {'b': 1, 'w': 2, 'd': 4}[mn[-1]]
            body = 'rt_out%d((u16)edx, RD%d(esi), 0x%x); esi += DSTEP(%d);' % (sz * 8, sz * 8, a, sz)
            return ('while (ecx) { %s ecx--; }' % body) if rep else body
        if mn == 'in':
            size = ops[0].size
            port = self.rd(ins, ops[1], 2) if ops[1].type == X.X86_OP_REG else '0x%x' % ops[1].imm
            return self.wr(ins, ops[0], 'rt_in%d(%s, 0x%x)' % (size * 8, port, a))
        if mn == 'out':
            size = ops[1].size
            port = self.rd(ins, ops[0], 2) if ops[0].type == X.X86_OP_REG else '0x%x' % ops[0].imm
            return 'rt_out%d(%s, %s, 0x%x);' % (size * 8, port, self.rd(ins, ops[1]), a)
        if mn.startswith('f'):
            r = self.fpu(ins, mn, ops)
            if r:
                return r
        self.unimpl[mn] += 1
        return 'rt_unimpl(0x%x, "%s %s");' % (a, ins.mnemonic, ins.op_str)

    # ---------- x87 ----------
    def fpu(self, ins, mn, ops):
        def src(op):
            if op.type == X.X86_OP_REG:
                n = ins.reg_name(op.reg)
                return 'ST(%s)' % n[3:-1] if n.startswith('st(') else 'ST(0)'
            if op.type == X.X86_OP_MEM:
                ad = self.addr(ins, op.mem)
                if op.size == 4: return 'RDF32(%s)' % ad
                if op.size == 8: return 'RDF64(%s)' % ad
                if op.size == 10: return 'RDF80(%s)' % ad
            return None

        def isrc(op):
            ad = self.addr(ins, op.mem)
            return '(long double)(%s)RD%d(%s)' % (SCTYPE[op.size] if op.size < 8 else 's64', op.size * 8, ad)

        def stidx(op):
            n = ins.reg_name(op.reg)
            return n[3:-1] if n.startswith('st(') else '0'
        n = len(ops)
        if mn == 'fld':
            return 'FPUSH(%s);' % src(ops[0])
        if mn == 'fild':
            return 'FPUSH(%s);' % isrc(ops[0])
        if mn in ('fst', 'fstp'):
            o = ops[0]
            if o.type == X.X86_OP_REG:
                s = 'ST(%s) = ST(0);' % stidx(o)
            else:
                ad = self.addr(ins, o.mem)
                s = {4: 'WRF32(%s, ST(0));', 8: 'WRF64(%s, ST(0));', 10: 'WRF80(%s, ST(0));'}[o.size] % ad
            return s + (' FPOP();' if mn == 'fstp' else '')
        if mn in ('fist', 'fistp'):
            o = ops[0]
            ad = self.addr(ins, o.mem)
            s = 'WR%d(%s, (u%d)FIST%d(ST(0)));' % (o.size * 8, ad, o.size * 8 if o.size < 8 else 64, o.size * 8)
            if o.size == 8:
                s = 'WR64(%s, (u64)FIST64(ST(0)));' % ad
            return s + (' FPOP();' if mn == 'fistp' else '')
        if mn == 'fxch':
            # capstone lists st(0) first ("fxch st(1)" -> ops st(0), st(1))
            i = '1'
            for o in ops:
                if o.type == X.X86_OP_REG and stidx(o) != '0':
                    i = stidx(o)
            return '{ long double t = ST(0); ST(0) = ST(%s); ST(%s) = t; }' % (i, i)
        if mn in ('fld1', 'fldz', 'fldpi', 'fldl2e', 'fldl2t', 'fldlg2', 'fldln2'):
            v = {'fld1': '1.0L', 'fldz': '0.0L', 'fldpi': '3.14159265358979323846264338327950288L',
                 'fldl2e': '1.44269504088896340735992468100189214L', 'fldl2t': '3.32192809488736234787031942948939018L',
                 'fldlg2': '0.30102999566398119521373889472449303L', 'fldln2': '0.69314718055994530941723212145817657L'}[mn]
            return 'FPUSH(%s);' % v
        ar = {'fadd': '+', 'fsub': '-', 'fmul': '*', 'fdiv': '/', 'fsubr': 'r-', 'fdivr': 'r/'}
        base = mn[:-1] if mn.endswith('p') and mn[:-1] in ar else mn
        if base in ar or (mn.startswith('fi') and 'f' + mn[2:] in ar):
            integer = mn.startswith('fi')
            if integer:
                base = 'f' + mn[2:]
            op = ar[base]
            rev = op.startswith('r')
            op = op[-1]
            pop = mn.endswith('p') and not integer
            if integer:
                s = isrc(ops[0])
                return 'ST(0) = %s;' % (('%s %s ST(0)' % (s, op)) if rev else ('ST(0) %s %s' % (op, s)))
            if n == 0:  # faddp with no operands = st(1) op= st(0), pop
                return 'ST(1) = %s; FPOP();' % (('ST(0) %s ST(1)' % op) if rev else ('ST(1) %s ST(0)' % op))
            if n == 1 and pop:
                # "faddp st(i)": st(i) = st(i) op st(0), then pop
                d = stidx(ops[0])
                e = ('ST(0) %s ST(%s)' % (op, d)) if rev else ('ST(%s) %s ST(0)' % (d, op))
                return 'ST(%s) = %s; FPOP();' % (d, e)
            if n == 1:
                s = src(ops[0])
                return 'ST(0) = %s;' % (('%s %s ST(0)' % (s, op)) if rev else ('ST(0) %s %s' % (op, s)))
            d = stidx(ops[0]); s = stidx(ops[1])
            e = ('ST(%s) %s ST(%s)' % (s, op, d)) if rev else ('ST(%s) %s ST(%s)' % (d, op, s))
            return 'ST(%s) = %s;%s' % (d, e, ' FPOP();' if pop else '')
        if mn in ('fcom', 'fcomp', 'fcompp', 'fucom', 'fucomp', 'fucompp', 'ficom', 'ficomp', 'ftst'):
            if mn == 'ftst':
                s = '0.0L'
            elif mn.startswith('fi'):
                s = isrc(ops[0])
            elif n:
                s = src(ops[0])
            else:
                s = 'ST(1)'
            pops = 2 if mn.endswith('pp') else (1 if mn.endswith('p') else 0)
            return 'FCOM(ST(0), %s);%s' % (s, ' FPOP();' * pops)
        if mn in ('fnstsw', 'fstsw'):
            if ops[0].type == X.X86_OP_REG:
                return 'eax = (eax & 0xffff0000u) | FPU_SW();'
            return 'WR16(%s, FPU_SW());' % self.addr(ins, ops[0].mem)
        if mn in ('fnstcw', 'fstcw'):
            return 'WR16(%s, cpu.fcw);' % self.addr(ins, ops[0].mem)
        if mn == 'fldcw':
            return 'cpu.fcw = RD16(%s);' % self.addr(ins, ops[0].mem)
        if mn in ('fninit', 'finit'):
            return 'FPU_INIT();'
        if mn == 'fchs':
            return 'ST(0) = -ST(0);'
        if mn == 'fabs':
            return 'ST(0) = fabsl(ST(0));'
        if mn == 'fsqrt':
            return 'ST(0) = sqrtl(ST(0));'
        if mn == 'frndint':
            return 'ST(0) = FROUND(ST(0));'
        if mn == 'fpatan':
            return 'ST(1) = atan2l(ST(1), ST(0)); FPOP();'
        if mn == 'fyl2x':
            return 'ST(1) = ST(1) * log2l(ST(0)); FPOP();'
        if mn == 'fsin':
            return 'ST(0) = sinl(ST(0));'
        if mn == 'fcos':
            return 'ST(0) = cosl(ST(0));'
        if mn == 'fptan':
            return 'ST(0) = tanl(ST(0)); FPUSH(1.0L);'
        if mn == 'f2xm1':
            return 'ST(0) = exp2l(ST(0)) - 1.0L;'
        if mn in ('fprem', 'fprem1'):
            # complete in one step: C2 (incomplete) cleared, C0/C3/C1 = quotient bits
            f = 'fmodl' if mn == 'fprem' else 'remainderl'
            return ('{ long double q = truncl(ST(0) / ST(1)); long long iq = (long long)fabsl(q); ST(0) = %s(ST(0), ST(1)); '
                    'cpu.fsw = (cpu.fsw & ~0x4700) | ((iq & 1) ? 0x200 : 0) | ((iq & 2) ? 0x4000 : 0) | ((iq & 4) ? 0x100 : 0); }' % f)
        if mn == 'fscale':
            return 'ST(0) = ldexpl(ST(0), (int)truncl(ST(1)));'
        if mn in ('fnsave', 'fsave'):
            return 'FPU_SAVE(%s);' % self.addr(ins, ops[0].mem)
        if mn == 'frstor':
            return 'FPU_RESTORE(%s);' % self.addr(ins, ops[0].mem)
        if mn in ('fnclex', 'fclex', 'fdecstp', 'fincstp', 'ffree', 'fnop'):
            return {'fdecstp': 'cpu.ftop = (cpu.ftop - 1) & 7;', 'fincstp': 'cpu.ftop = (cpu.ftop + 1) & 7;'}.get(mn, ';')
        return None

    # ---------- function level ----------
    def liveness(self, f):
        """per-instruction live-after flag sets"""
        p = self.p
        rl = self.rlive[f.entry]
        blocks = f.blocks
        use_def = {}
        for b, l in blocks.items():
            for a in l:
                ins = p.insn(a)
                u, d = flag_use_def(p, ins, rl)
                if a in f.tailcalls:
                    u, d = self.rlive[f.tailcalls[a]] | rl, 0
                use_def[a] = (u, d)
        live_in = {b: 0 for b in blocks}
        changed = True
        while changed:
            changed = False
            for b in sorted(blocks, reverse=True):
                out = 0
                for s in f.succ.get(b, []):
                    out |= live_in.get(s, ALL)
                l = blocks[b]
                if not l:
                    continue
                last = p.insn(l[-1])
                if last.mnemonic in ('ret', 'retf'):
                    out |= rl
                cur = out
                for a in reversed(l):
                    u, d = use_def[a]
                    cur = (cur & ~d) | u
                if cur != live_in[b]:
                    live_in[b] = cur
                    changed = True
        la = {}
        for b, l in blocks.items():
            out = 0
            for s in f.succ.get(b, []):
                out |= live_in.get(s, ALL)
            if l and p.insn(l[-1]).mnemonic in ('ret', 'retf'):
                out |= rl
            cur = out
            for a in reversed(l):
                la[a] = cur
                u, d = use_def[a]
                cur = (cur & ~d) | u
        return la

    def interproc(self):
        """flags live after a call -> must be produced by the callee's ret"""
        p = self.p
        for it in range(6):
            changed = False
            for e, f in self.funcs.items():
                la = self.liveness(f)
                for b, l in f.blocks.items():
                    for a in l:
                        ins = p.insn(a)
                        if ins.mnemonic == 'call' and ins.operands[0].type == X.X86_OP_IMM:
                            t = ins.operands[0].imm & 0xffffffff
                            # live after the call = what the next instruction needs
                            nxt = a + ins.size
                            need = self.live_at(f, la, nxt)
                            if need & ~self.rlive[t]:
                                self.rlive[t] |= need
                                changed = True
            if not changed:
                break

    def live_at(self, f, la, addr):
        """flags live before instruction at addr"""
        p = self.p
        ins = p.insn(addr)
        if ins is None or addr not in la:
            return 0
        u, d = flag_use_def(p, ins, self.rlive[f.entry])
        return (la[addr] & ~d) | u

    def writes_ds(self, f):
        for l in f.blocks.values():
            for a in l:
                ins = self.p.insn(a)
                m = ins.mnemonic
                if m == 'lds':
                    return True
                if m in ('mov', 'pop') and ins.operands and ins.operands[0].type == X.X86_OP_REG                         and ins.reg_name(ins.operands[0].reg) == 'ds':
                    return True
        return False

    def gen_func(self, f):
        p = self.p
        self.ds_based = self.writes_ds(f)
        la = self.liveness(f)
        # multi-entry function (computed jump into its own code): every
        # instruction is a jump target, all flags live everywhere
        allins = sorted(set(a for l in f.blocks.values() for a in l)) if f.multi else []
        if f.multi:
            la = {}
        labels = set()
        out = ['void ' + PFX + '%x(void)' % f.entry, '{', '    LOCALS;',
               '    u32 ret_expect = rt_tail_ret ? rt_tail_ret : RD32(esp); rt_tail_ret = 0; (void)ret_expect;']
        if f.entry in TRACE:
            out.append('    rt_trace_enter(0x%x);' % f.entry)
        order = sorted(f.blocks)
        if order[0] != f.entry:
            out.append('    goto L_%x;' % f.entry)
        for bi, b in enumerate(order):
            l = f.blocks[b]
            if b not in labels:
                out.append('L_%x:' % b)
                labels.add(b)
            if not l:
                out.append('    rt_bad_code(0x%x);' % b)
                continue
            for a in l:
                ins = p.insn(a)
                m = ins.mnemonic
                nxt = a + ins.size
                L = la.get(a, ALL)
                if f.multi and a not in labels:
                    out.append('L_%x:' % a)
                    labels.add(a)
                if m in JCC:
                    t = ins.operands[0].imm & 0xffffffff
                    if m in ('jcxz', 'jecxz'):
                        c = '!(%s)' % ('(u16)ecx' if m == 'jcxz' else 'ecx')
                    elif m == 'loop':
                        out.append('    ecx--;')
                        c = 'ecx != 0'
                    elif m == 'loope':
                        out.append('    ecx--;')
                        c = '(ecx != 0 && zf)'
                    elif m == 'loopne':
                        out.append('    ecx--;')
                        c = '(ecx != 0 && !zf)'
                    else:
                        c = cond_of(m, 'j')[0]
                    out.append('    if (%s) { %sgoto L_%x; }' % (c, 'POLL; ' if t <= a else '', t))
                    out.append('    goto L_%x;' % nxt)
                    continue
                if m == 'jmp':
                    op = ins.operands[0]
                    if a in f.tailcalls:
                        out.append('    SYNC_OUT; rt_tail_ret = ret_expect; %s(); return; /* tail */' % self.fname(f.tailcalls[a]))
                    elif op.type == X.X86_OP_IMM:
                        t = op.imm & 0xffffffff
                        out.append('    %sgoto L_%x;' % ('POLL; ' if t <= a else '', t))
                    elif a in f.tables:
                        out.append('    { u32 t = %s; switch (t) {' % self.rd(ins, op, 4))
                        for t in sorted(set(f.tables[a])):
                            out.append('    case 0x%xu: goto L_%x;' % (t, t))
                        out.append('    default: SYNC_OUT; rt_tail_ret = ret_expect; rt_jump(t, 0x%x); return; } }' % a)
                    elif f.multi:
                        out.append('    { u32 t = %s; switch (t) {' % self.rd(ins, op, 4))
                        for t in allins:
                            out.append('    case 0x%xu: goto L_%x;' % (t, t))
                        out.append('    default: SYNC_OUT; rt_tail_ret = ret_expect; rt_jump(t, 0x%x); return; } }' % a)
                    else:
                        out.append('    { u32 t = %s; SYNC_OUT; rt_tail_ret = ret_expect; rt_jump(t, 0x%x); return; }' % (self.rd(ins, op, 4), a))
                    continue
                if m in ('ret', 'retf', 'iret', 'iretd'):
                    n = ins.operands[0].imm if ins.operands else 0
                    if m == 'ret':
                        # a ret to anything but our caller is a computed jump
                        # (Watcom's int386 does push addr / ret into a stub table)
                        tr = ' rt_trace_leave(0x%x);' % f.entry if f.entry in TRACE else ''
                        out.append('    { u32 ra = RD32(esp); esp += %d; SYNC_OUT;%s if (ra != ret_expect) rt_jump(ra, 0x%x); return; }' % (4 + n, tr, a))
                    elif m == 'retf':
                        # a retf into an old handler (MAGIC address) is an interrupt chain:
                        # the runtime does what that handler did, including its iretd
                        out.append('    { u32 ra = RD32(esp); esp += %d; SYNC_OUT; if (ra - 0xfff00000u < 0x100u) rt_magic_chain(ra); return; } /* retf */' % (8 + n))
                    elif m == 'iretd':
                        # interrupt handler exit: the runtime built a 12-byte frame
                        out.append('    esp += 12; SYNC_OUT; return; /* iretd */')
                    else:
                        out.append('    rt_unimpl(0x%x, "%s"); SYNC_OUT; return;' % (a, m))
                    continue
                if m == 'call':
                    op = ins.operands[0]
                    if op.type == X.X86_OP_IMM:
                        t = op.imm & 0xffffffff
                        name = self.fname(t)
                        if name:
                            out.append('    ' + self.call_direct(t, nxt))
                        else:
                            out.append('    esp -= 4; WR32(esp, 0x%xu); SYNC_OUT; rt_call(0x%xu, 0x%x); SYNC_IN;' % (nxt, t, a))
                        if t in self.noreturn:
                            out.append('    rt_noreturn(0x%x); return;' % a)
                    elif is_far(ins):
                        # call m16:32 - far call, callee returns with retf
                        out.append('    { u32 t = RD32(%s); esp -= 4; WR32(esp, 0x%x); esp -= 4; WR32(esp, 0x%xu); SYNC_OUT; rt_call(t, 0x%x); SYNC_IN; }'
                                   % (self.addr(ins, op.mem), 0x10, nxt, a))
                    else:
                        out.append('    { u32 t = %s; esp -= 4; WR32(esp, 0x%xu); SYNC_OUT; rt_call(t, 0x%x); SYNC_IN; }'
                                   % (self.rd(ins, op, 4), nxt, a))
                    continue
                if m in ('hlt', 'ud2'):
                    out.append('    rt_unimpl(0x%x, "%s");' % (a, m))
                    continue
                try:
                    c = self.insn(f, ins, L, nxt)
                except Exception as ex:
                    c = 'rt_unimpl(0x%x, "gen error %s: %s");' % (a, ins.mnemonic, str(ex).replace('"', "'"))
                out.append('    %s /* %x: %s %s */' % (c, a, ins.mnemonic, ins.op_str.replace('*/', '* /')))
                if m == 'int' and ins.operands[0].imm == 0x21:
                    pass
            # fallthrough out of the block
            last = p.insn(l[-1])
            lm = last.mnemonic
            if lm not in JCC and lm not in ('jmp', 'ret', 'retf', 'iret', 'iretd') and not (
                    lm == 'call' and last.operands[0].type == X.X86_OP_IMM and (last.operands[0].imm & 0xffffffff) in self.noreturn):
                s = f.succ.get(b, [])
                if s:
                    out.append('    goto L_%x;' % s[0])
                else:
                    out.append('    rt_fell_off(0x%x); return;' % l[-1])
        out.append('}')
        return out


PFX = 'F_'


def main():
    global PFX
    exe, gfuncs, native_txt, outdir, mod = sys.argv[1:6]
    PFX = mod + '_'
    p = Prog(exe)
    def rel(a):  # Ghidra / REA work on the original object addresses
        for o in p.le.objs:
            if o.orig_base <= a < o.orig_base + max(o.vsize, o.npages * p.le.page_size):
                return a - o.orig_base + o.base
        return a
    extra = [rel(int(l.split('\t')[0], 16)) for l in open(gfuncs) if l.strip()]
    native = {}
    wrap = set()   # "addr name wrap": calls go to the native, which can still run the original
    if os.path.exists(native_txt):
        for line in open(native_txt):
            line = line.split('#')[0].strip()
            if line:
                w = line.split()
                native[int(w[0], 16)] = w[1]
                if len(w) > 2 and w[2] == 'wrap':
                    wrap.add(int(w[0], 16))
    # 'int N; ret' stub tables (Watcom int386): every stub is a jump target
    mem = p.le.mem
    lo = p.le.lo
    a = p.clo
    while a < p.chi - 3:
        o = a - lo
        if mem[o] == 0xcd and mem[o + 2] == 0xc3 and mem[o + 3] == 0xcd and mem[o + 5] == 0xc3:
            while a < p.chi - 3 and mem[a - lo] == 0xcd and mem[a - lo + 2] == 0xc3:
                extra.append(a)
                a += 3
        a += 1
    funcs, noreturn, rejected = analyze(p, extra, returning=set(native))
    for a in native:
        if a not in wrap:
            funcs.pop(a, None)
    g = Gen(p, funcs, native, noreturn)
    g.interproc()
    os.makedirs(outdir, exist_ok=True)
    ents = sorted(funcs)
    files = []
    # one file per 8 KB of code address space, so a change stays local to one
    # file and the build stays incremental
    groups = collections.OrderedDict()
    for e in ents:
        groups.setdefault((e - p.clo) >> 13, []).append(e)
    for key, chunk in groups.items():
        fn = os.path.join(outdir, 'gen_%03d.c' % key)
        body = []
        for e in chunk:
            body += g.gen_func(funcs[e])
            body.append('')
        btxt = '\n'.join(body)
        # declare only what this file calls, so adding a function elsewhere
        # does not force every file to recompile
        names = sorted(set(re.findall(r'\b(' + PFX + r'[0-9a-f]+)\(\);', btxt)) |
                       set(n for n in native.values() if (n + '();') in btxt))
        lines = ['/* generated by tools/recomp.py - do not edit */', '#include "cpu.h"', '']
        lines += ['void %s(void);' % n for n in names]
        txt = '\n'.join(lines) + '\n\n' + btxt + '\n'
        if not os.path.exists(fn) or open(fn).read() != txt:
            open(fn, 'w').write(txt)
        files.append(fn)
    # remove stale files
    for fn in os.listdir(outdir):
        full = os.path.join(outdir, fn)
        if fn.startswith('gen_') and fn.endswith('.c') and full not in files:
            os.remove(full)
    hdr = ['/* generated */', '#pragma once', '#include "cpu.h"']
    for e in ents:
        hdr.append('void ' + PFX + '%x(void);' % e)
    for a, n in sorted(native.items()):
        hdr.append('void %s(void); /* native replacement for %x */' % (n, a))
    txt = '\n'.join(hdr) + '\n'
    hp = os.path.join(outdir, 'funcs.h')
    if not os.path.exists(hp) or open(hp).read() != txt:
        open(hp, 'w').write(txt)
    tab = ['/* generated */', '#include "cpu.h"', '#include "funcs.h"', 'const FuncEnt %s_functab[] = {' % mod]
    allf = sorted(set(ents) | set(native))
    for e in allf:
        tab.append('    {0x%xu, %s},' % (e, native.get(e, PFX + '%x' % e)))
    tab.append('};')
    tab.append('const int %s_nfuncs = %d;' % (mod, len(allf)))
    tab.append('const WrapEnt %s_wraptab[] = {' % mod)
    for a in sorted(wrap):
        if a in funcs:
            tab.append('    {0x%xu, %s%x, "%s"},' % (a, PFX, a, native[a]))
    tab.append('    {0, 0, 0}')
    tab.append('};')
    open(os.path.join(outdir, 'functab.c'), 'w').write('\n'.join(tab) + '\n')
    print('functions', len(funcs), 'native', len(native), 'files', len(files))
    print('unimplemented:', dict(g.unimpl.most_common()))


if __name__ == '__main__':
    main()
