/* DOS-extender services (DPMI, int 31h) implemented on the PC side:
 * selectors, memory blocks, DOS memory, interrupt vectors and the
 * "simulate real-mode interrupt" call, which is routed straight to the
 * native BIOS/DOS/mouse/CD services in bios.c and dos.c. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include <stdlib.h>
#include "rt.h"

#define NSEL 8192
static struct { u32 base, limit; u8 used; } seltab[NSEL];

/* real-mode context while servicing int31/300 */
int rm_active;
u16 rm_es, rm_ds;

u32 sel_base(u16 sel)
{
    u32 i = sel >> 3;
    if (i >= NSEL) return 0;
    return seltab[i].base;
}

u16 sel_alloc(u32 base, u32 limit)
{
    for (u32 i = 16; i < NSEL; i++)
        if (!seltab[i].used) {
            seltab[i].used = 1;
            seltab[i].base = base;
            seltab[i].limit = limit;
            return (u16)(i << 3 | 7);
        }
    rt_fatal("out of selectors");
    return 0;
}

static void sel_set(u16 sel, u32 base, u32 limit)
{
    u32 i = sel >> 3;
    seltab[i].used = 1;
    seltab[i].base = base;
    seltab[i].limit = limit;
}

u32 rt_sel_limit(u16 sel)
{
    u32 i = sel >> 3;
    return i < NSEL && seltab[i].used ? seltab[i].limit : 0xffffffffu;
}

void rt_load_seg(int idx, u16 sel)
{
    cpu.sreg[idx] = sel;
    cpu.sbase[idx] = sel_base(sel);
    if (cpu.sbase[idx] && (idx == S_ES || idx == S_DS)) {
        static int n;
        extern void rt_backtrace(const char *);
        if (n++ < 10) { rt_log("seg: %s = %04x base %08x\n", idx == S_ES ? "es" : "ds", sel, cpu.sbase[idx]); rt_backtrace("seg"); }
    }
}

/* ---- memory ---- */
static u32 dos_next = DOSMEM_BASE;
u32 dosmem_alloc(u32 paras, u16 *seg, u16 *sel)
{
    u32 bytes = paras * 16;
    if (dos_next + bytes > DOSMEM_END) return 0;
    u32 a = dos_next;
    dos_next = (dos_next + bytes + 15) & ~15u;
    memset(MEM + a, 0, bytes);
    *seg = (u16)(a >> 4);
    if (sel) *sel = sel_alloc(a, bytes - 1);
    return a;
}

static u32 heap_next = 0x04000000u;
u32 heap_alloc(u32 size)
{
    size = (size + 0xfff) & ~0xfffu;
    if (heap_next + size > HEAP_END) return 0;
    u32 a = heap_next;
    heap_next += size;
    return a;
}

static void rm_vec_init(void);

void dpmi_init(void)
{
    memset(seltab, 0, sizeof seltab);
    dos_next = DOSMEM_BASE;
    heap_next = 0x04000000u;
    sel_set(SEL_CODE, 0, 0xffffffff);
    sel_set(SEL_DATA, 0, 0xffffffff);
    sel_set(SEL_PSP, 0x1000, 0xff);
    sel_set(SEL_ENV, 0x1200, 0xfff);
    sel_set(0x0030, 0, 0xffffffff); /* spare flat selectors */
    sel_set(0x0038, 0, 0xffffffff);
    /* BIOS data area: equipment, video mode, timer ticks */
    WR16(0x410, 0x0021);
    WR8(0x449, 3);
    WR16(0x44a, 80);
    WR8(0x484, 24);
    { extern void irq_reset(void); irq_reset(); }
    rm_vec_init();
}

/* ---- real-mode register structure (int31/300) ---- */
typedef struct {
    u32 edi, esi, ebp, rsv, ebx, edx, ecx, eax;
    u16 flags, es, ds, fs, gs, ip, cs, sp, ss;
} __attribute__((packed)) RMRegs;

void rt_int(u32 n, u32 at);

void real_int(u8 n, u32 regs_addr)
{
    RMRegs rr;
    memcpy(&rr, MEM + regs_addr, sizeof rr);
    u32 save[8];
    memcpy(save, cpu.r, sizeof save);
    u32 scf = cpu.cf, szf = cpu.zf;
    cpu.r[R_EDI] = rr.edi; cpu.r[R_ESI] = rr.esi; cpu.r[R_EBP] = rr.ebp;
    cpu.r[R_EBX] = rr.ebx; cpu.r[R_EDX] = rr.edx; cpu.r[R_ECX] = rr.ecx; cpu.r[R_EAX] = rr.eax;
    cpu.cf = rr.flags & 1;
    cpu.zf = (rr.flags >> 6) & 1;
    int prev = rm_active;
    u16 pes = rm_es, pds = rm_ds;
    rm_active = 1; rm_es = rr.es; rm_ds = rr.ds;
    switch (n) {
    case 0x10: rt_int10(); break;
    case 0x21: rt_int21(); break;
    case 0x2f: rt_int2f(); break;
    case 0x33: rt_int33(); break;
    case 0x16: rt_int16(); break;
    default: rt_log("real-mode int %02x ax=%04x not provided\n", n, rr.eax & 0xffff); break;
    }
    rr.edi = cpu.r[R_EDI]; rr.esi = cpu.r[R_ESI]; rr.ebp = cpu.r[R_EBP];
    rr.ebx = cpu.r[R_EBX]; rr.edx = cpu.r[R_EDX]; rr.ecx = cpu.r[R_ECX]; rr.eax = cpu.r[R_EAX];
    rr.flags = (u16)((rr.flags & ~0x41) | cpu.cf | (cpu.zf << 6));
    rr.es = rm_es; rr.ds = rm_ds;
    rm_active = prev; rm_es = pes; rm_ds = pds;
    memcpy(MEM + regs_addr, &rr, sizeof rr);
    memcpy(cpu.r, save, sizeof save);
    cpu.cf = scf; cpu.zf = szf;
}

/* pointer helpers for services: protected mode uses selector:32-bit offset,
   the simulated real-mode path uses segment*16 + 16-bit offset */
u32 ptr_es(u32 off) { return rm_active ? (u32)rm_es * 16 + (off & 0xffff) : cpu.sbase[S_ES] + off; }
u32 ptr_ds(u32 off) { return rm_active ? (u32)rm_ds * 16 + (off & 0xffff) : off; }
void set_es(u16 v) { if (rm_active) rm_es = v; else rt_load_seg(S_ES, v); }

#define AX (cpu.r[R_EAX] & 0xffff)
#define BX (cpu.r[R_EBX] & 0xffff)
#define CX (cpu.r[R_ECX] & 0xffff)
#define DX (cpu.r[R_EDX] & 0xffff)
#define SETAX(v) (cpu.r[R_EAX] = (cpu.r[R_EAX] & 0xffff0000u) | ((v) & 0xffff))
#define SETBX(v) (cpu.r[R_EBX] = (cpu.r[R_EBX] & 0xffff0000u) | ((v) & 0xffff))
#define SETCX(v) (cpu.r[R_ECX] = (cpu.r[R_ECX] & 0xffff0000u) | ((v) & 0xffff))
#define SETDX(v) (cpu.r[R_EDX] = (cpu.r[R_EDX] & 0xffff0000u) | ((v) & 0xffff))
#define SETSI(v) (cpu.r[R_ESI] = (cpu.r[R_ESI] & 0xffff0000u) | ((v) & 0xffff))
#define SETDI(v) (cpu.r[R_EDI] = (cpu.r[R_EDI] & 0xffff0000u) | ((v) & 0xffff))

static struct { u32 addr, size; } blocks[256];
static u32 nblocks;
static u32 rm_vec[256];

/* real-mode vectors as a DOS machine with BIOS, DOS, a mouse driver and
   MSCDEX would have them: programs check that a vector is set before they
   use the service (INTRO detects the mouse by its int 33h vector and falls
   back to keyboard control, saved in GAME.CFG, when it is 0) */
static void rm_vec_init(void)
{
    for (int i = 0; i < 256; i++) rm_vec[i] = 0xf000ff53u;       /* BIOS dummy iret */
    rm_vec[0x21] = 0x0fd00500u;   /* DOS */
    rm_vec[0x2f] = 0x0fd00600u;   /* multiplex / MSCDEX */
    rm_vec[0x33] = 0x0fe00100u;   /* mouse driver */
}

void rt_int31(void)
{
    u32 ax = AX;
    cpu.cf = 0;
    switch (ax) {
    case 0x0000: { /* allocate descriptors */
        u32 n = CX ? CX : 1;
        u16 first = sel_alloc(0, 0xffffffff);
        for (u32 i = 1; i < n; i++) sel_alloc(0, 0xffffffff);
        SETAX(first);
        break;
    }
    case 0x0001: seltab[(BX >> 3) % NSEL].used = 0; break;
    case 0x0002: { /* segment to descriptor */
        u16 s = sel_alloc((u32)BX * 16, 0xffff);
        SETAX(s);
        break;
    }
    case 0x0003: SETAX(8); break;
    case 0x0006: { u32 b = sel_base((u16)BX); SETCX(b >> 16); SETDX(b); break; }
    case 0x0007: { u32 i = (BX >> 3) % NSEL; seltab[i].base = ((u32)CX << 16) | DX; seltab[i].used = 1;
        for (int k = 0; k < 6; k++) if (cpu.sreg[k] == BX) cpu.sbase[k] = seltab[i].base; break; }
    case 0x0008: seltab[(BX >> 3) % NSEL].limit = ((u32)CX << 16) | DX; break;
    case 0x0009: break;
    case 0x000a: { u16 s = sel_alloc(sel_base((u16)BX), 0xffffffff); SETAX(s); break; }
    case 0x000b: { /* get descriptor */
        u32 b = sel_base((u16)BX), lim = seltab[(BX >> 3) % NSEL].limit, d = ptr_es(cpu.r[R_EDI]);
        u32 lm = lim > 0xfffff ? lim >> 12 : lim;
        WR16(d, lm); WR16(d + 2, b); WR8(d + 4, b >> 16); WR8(d + 5, 0xf2);
        WR8(d + 6, ((lm >> 16) & 0xf) | (lim > 0xfffff ? 0xc0 : 0x40)); WR8(d + 7, b >> 24);
        break;
    }
    case 0x000c: { u32 d = ptr_es(cpu.r[R_EDI]);
        u32 b = RD16(d + 2) | (RD8(d + 4) << 16) | (RD8(d + 7) << 24);
        u32 lim = RD16(d) | ((RD8(d + 6) & 0xf) << 16);
        if (RD8(d + 6) & 0x80) lim = (lim << 12) | 0xfff;
        sel_set((u16)BX, b, lim); break; }
    case 0x0100: { /* allocate DOS memory */
        u16 seg, sel;
        if (!dosmem_alloc(BX, &seg, &sel)) { cpu.cf = 1; SETAX(8); SETBX((DOSMEM_END - dos_next) >> 4); break; }
        SETAX(seg); SETDX(sel);
        break;
    }
    case 0x0101: break; /* free DOS memory (pool is not reused) */
    case 0x0102: break;
    case 0x0200: { u32 v = rm_vec[cpu.r[R_EBX] & 0xff]; SETCX(v >> 16); SETDX(v); break; }
    case 0x0201: rm_vec[cpu.r[R_EBX] & 0xff] = ((u32)CX << 16) | DX; break;
    case 0x0202: SETCX(SEL_CODE); cpu.r[R_EDX] = MAGIC_IRET; break;
    case 0x0203: break;
    case 0x0204: { u32 v = irq_get_vector(cpu.r[R_EBX] & 0xff); SETCX(SEL_CODE); cpu.r[R_EDX] = v; break; }
    case 0x0205: irq_set_vector(cpu.r[R_EBX] & 0xff, cpu.r[R_EDX]); break;
    case 0x0300: real_int(cpu.r[R_EBX] & 0xff, ptr_es(cpu.r[R_EDI])); break;
    case 0x0301: case 0x0302:
        rt_log("dpmi: call real-mode procedure (ax=%04x) not provided\n", ax);
        break;
    case 0x0303: { /* real-mode callback: hand out a unique fake address */
        static u16 next = 0xc000;
        SETCX(0xf000); SETDX(next); next += 0x10;
        rt_log("dpmi: real-mode callback to %08x requested\n", cpu.r[R_ESI]);
        break;
    }
    case 0x0304: break;
    case 0x0400: SETAX(0x005a); SETBX(0x0005); cpu.r[R_ECX] = (cpu.r[R_ECX] & ~0xff) | 5; SETDX(0x0870); break;
    case 0x0500: { u32 d = ptr_es(cpu.r[R_EDI]);
        u32 fr = HEAP_END - heap_next;
        for (int i = 0; i < 12; i++) WR32(d + i * 4, 0xffffffff);
        WR32(d, fr); WR32(d + 4, fr >> 12); WR32(d + 8, fr >> 12); WR32(d + 0x14, fr >> 12);
        WR32(d + 0x18, (HEAP_END - HEAP_BASE) >> 12); WR32(d + 0x1c, fr >> 12); WR32(d + 0x20, (HEAP_END - HEAP_BASE) >> 12);
        WR32(d + 0x24, 0);
        break; }
    case 0x0501: { /* allocate memory block */
        u32 sz = ((u32)BX << 16) | CX;
        u32 a = heap_alloc(sz);
        if (getenv("AO_MEMLOG")) rt_log("dpmi: alloc %u -> %08x\n", sz, a);
        if (!a) { cpu.cf = 1; SETAX(0x8013); break; }
        u32 h = nblocks < 256 ? nblocks++ : 255;
        blocks[h].addr = a; blocks[h].size = sz;
        SETBX(a >> 16); SETCX(a); SETSI(0x100 + h); SETDI(0);
        break;
    }
    case 0x0502: break;
    case 0x0503: { /* resize: allocate new and copy */
        u32 h = (cpu.r[R_ESI] & 0xffff) - 0x100;
        u32 sz = ((u32)BX << 16) | CX;
        if (h >= nblocks) { cpu.cf = 1; SETAX(0x8023); break; }
        u32 a = blocks[h].addr;
        if (sz > blocks[h].size) {
            a = heap_alloc(sz);
            if (!a) { cpu.cf = 1; SETAX(0x8013); break; }
            memcpy(MEM + a, MEM + blocks[h].addr, blocks[h].size);
            blocks[h].addr = a;
        }
        blocks[h].size = sz;
        SETBX(a >> 16); SETCX(a);
        break;
    }
    case 0x0600: case 0x0601: case 0x0602: case 0x0603: break;
    case 0x0604: SETBX(0); SETCX(0x1000); break;
    case 0x0800: { /* map physical address: the only device memory the game can
                      ask for is the video card's linear frame buffer */
        u32 phys = ((u32)BX << 16) | CX;
        rt_log("dpmi: map physical %08x\n", phys);
        SETBX(phys >> 16); SETCX(phys);
        break;
    }
    case 0x0801: break;
    case 0x0900: case 0x0901: cpu.r[R_EAX] = (cpu.r[R_EAX] & ~0xff) | 1; break;
    case 0x0902: cpu.r[R_EAX] = (cpu.r[R_EAX] & ~0xff) | 1; break;
    case 0x0e00: SETAX(0x4d); break;
    case 0x0e01: break;
    default:
        rt_log("dpmi: function %04x not provided\n", ax);
        cpu.cf = 1;
        SETAX(0x8001);
    }
}
