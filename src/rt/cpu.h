/* Register file and helpers used by the recompiled game code (src/gen).
 *
 * Guest memory is a 4 GB reservation (MEM); a guest linear address is an
 * offset into it, so 32-bit wrap-around needs no bounds checks.  Segments
 * are flat except selectors handed out by the native DPMI replacement
 * (DOS memory), whose base is kept in cpu.sbase[].
 */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

typedef u16 __attribute__((aligned(1), may_alias)) u16u;
typedef u32 __attribute__((aligned(1), may_alias)) u32u;
typedef u64 __attribute__((aligned(1), may_alias)) u64u;
typedef float __attribute__((aligned(1), may_alias)) f32u;
typedef double __attribute__((aligned(1), may_alias)) f64u;

enum { R_EAX, R_ECX, R_EDX, R_EBX, R_ESP, R_EBP, R_ESI, R_EDI };
enum { S_ES, S_CS, S_SS, S_DS, S_FS, S_GS };

typedef struct CPU {
    u32 r[8];
    u32 cf, zf, sf, of, pf, af;
    u32 df;
    u16 sreg[6];
    u32 sbase[6];
    long double fst[8];
    u32 ftop;
    u16 fsw, fcw;
} CPU;

extern CPU cpu;
extern u8 *MEM;
extern u8 rt_parity_tab[256];
extern volatile int rt_irq_pending;
extern u32 rt_tail_ret; /* caller's return address, passed along a tail jump */
void irq_poll(void);

#define RD8(a)  (*(u8 *)(MEM + (u32)(a)))
#define RD16(a) (*(u16u *)(MEM + (u32)(a)))
#define RD32(a) (*(u32u *)(MEM + (u32)(a)))
#define RD64(a) (*(u64u *)(MEM + (u32)(a)))
#define WR8(a, v)  (*(u8 *)(MEM + (u32)(a)) = (u8)(v))
#define WR16(a, v) (*(u16u *)(MEM + (u32)(a)) = (u16)(v))
#define WR32(a, v) (*(u32u *)(MEM + (u32)(a)) = (u32)(v))
#define WR64(a, v) (*(u64u *)(MEM + (u32)(a)) = (u64)(v))
#define PTR(a) ((void *)(MEM + (u32)(a)))

#define PARITY(x) (rt_parity_tab[(u8)(x)])
/* service timer / input at loop back-edges (the game busy-waits on timer ticks) */
#define POLL do { if (rt_irq_pending) { SYNC_OUT; irq_poll(); SYNC_IN; } } while (0)
#define DSTEP(n) (cpu.df ? (u32)-(n) : (u32)(n))

#define LOCALS \
    u32 eax = cpu.r[0], ecx = cpu.r[1], edx = cpu.r[2], ebx = cpu.r[3], \
        esp = cpu.r[4], ebp = cpu.r[5], esi = cpu.r[6], edi = cpu.r[7]; \
    u32 cf = cpu.cf, zf = cpu.zf, sf = cpu.sf, of = cpu.of, pf = cpu.pf, af = cpu.af; \
    (void)eax; (void)ecx; (void)edx; (void)ebx; (void)esp; (void)ebp; (void)esi; (void)edi; \
    (void)cf; (void)zf; (void)sf; (void)of; (void)pf; (void)af

#define SYNC_OUT do { cpu.r[0] = eax; cpu.r[1] = ecx; cpu.r[2] = edx; cpu.r[3] = ebx; \
    cpu.r[4] = esp; cpu.r[5] = ebp; cpu.r[6] = esi; cpu.r[7] = edi; \
    cpu.cf = cf; cpu.zf = zf; cpu.sf = sf; cpu.of = of; cpu.pf = pf; cpu.af = af; } while (0)
#define SYNC_IN do { eax = cpu.r[0]; ecx = cpu.r[1]; edx = cpu.r[2]; ebx = cpu.r[3]; \
    esp = cpu.r[4]; ebp = cpu.r[5]; esi = cpu.r[6]; edi = cpu.r[7]; \
    cf = cpu.cf; zf = cpu.zf; sf = cpu.sf; of = cpu.of; pf = cpu.pf; af = cpu.af; } while (0)

#define PACK_FLAGS() (0x202u | cf | (pf << 2) | (af << 4) | (zf << 6) | (sf << 7) | (cpu.df << 10) | (of << 11))
#define UNPACK_FLAGS(v) do { u32 _v = (v); cf = _v & 1; pf = (_v >> 2) & 1; af = (_v >> 4) & 1; \
    zf = (_v >> 6) & 1; sf = (_v >> 7) & 1; cpu.df = (_v >> 10) & 1; of = (_v >> 11) & 1; } while (0)

/* rep string ops: forward, flat fast paths */
/* forward rep movs is a memcpy unless the destination starts inside the
   source run: then x86 copies element by element and repeats the pattern
   (LZ decompressors rely on that) */
#define REP_MOVSB() do { u32 _d = edi + cpu.sbase[0];     if (!cpu.df && (u32)(_d - esi) >= ecx) { memmove(MEM + _d, MEM + esi, ecx); esi += ecx; edi += ecx; ecx = 0; }     else while (ecx) { WR8(edi + cpu.sbase[0], RD8(esi)); esi += DSTEP(1); edi += DSTEP(1); ecx--; } } while (0)
#define REP_MOVSD(b) do { u32 _d = edi + cpu.sbase[0];     if (!cpu.df && (u32)(_d - esi) >= ecx * 4u) { memmove(MEM + _d, MEM + esi, (size_t)ecx * 4); esi += ecx * 4; edi += ecx * 4; ecx = 0; }     else while (ecx) { WR32(edi + cpu.sbase[0], RD32(esi)); esi += DSTEP(4); edi += DSTEP(4); ecx--; } } while (0)
#define REP_STOSB() do { if (!cpu.df) { memset(MEM + edi + cpu.sbase[0], (u8)eax, ecx); edi += ecx; ecx = 0; } \
    else while (ecx) { WR8(edi + cpu.sbase[0], eax); edi--; ecx--; } } while (0)
#define REP_STOSD() do { while (ecx) { WR32(edi + cpu.sbase[0], eax); edi += DSTEP(4); ecx--; } } while (0)

/* x87 */
#define ST(i) (cpu.fst[(cpu.ftop + (i)) & 7])
#define FPUSH(v) do { long double _fv = (v); cpu.ftop = (cpu.ftop - 1) & 7; cpu.fst[cpu.ftop] = _fv; } while (0)
#define FPOP() (cpu.ftop = (cpu.ftop + 1) & 7)
#define RDF32(a) ((long double)*(f32u *)(MEM + (u32)(a)))
#define RDF64(a) ((long double)*(f64u *)(MEM + (u32)(a)))
#define WRF32(a, v) (*(f32u *)(MEM + (u32)(a)) = (float)(v))
#define WRF64(a, v) (*(f64u *)(MEM + (u32)(a)) = (double)(v))
static inline long double RDF80(u32 a) { long double v = 0; memcpy(&v, MEM + a, 10); return v; }
static inline void WRF80(u32 a, long double v) { memcpy(MEM + a, &v, 10); }
#define FCOM(a, b) do { long double _a = (a), _b = (b); cpu.fsw &= ~0x4500; \
    if (_a != _a || _b != _b) cpu.fsw |= 0x4500; else if (_a < _b) cpu.fsw |= 0x0100; else if (_a == _b) cpu.fsw |= 0x4000; } while (0)
#define FPU_SW() ((u16)((cpu.fsw & ~0x3800) | ((cpu.ftop & 7) << 11)))
#define FPU_INIT() do { cpu.ftop = 0; cpu.fsw = 0; cpu.fcw = 0x37f; } while (0)
long double rt_fround(long double v);
s64 rt_fist(long double v, int bits);
#define FROUND(v) rt_fround(v)
#define FIST16(v) ((s16)rt_fist((v), 16))
#define FIST32(v) ((s32)rt_fist((v), 32))
#define FIST64(v) (rt_fist((v), 64))
void rt_fpu_save(u32 a);
void rt_fpu_restore(u32 a);
#define FPU_SAVE(a) rt_fpu_save(a)
#define FPU_RESTORE(a) rt_fpu_restore(a)

/* runtime services (src/rt) */
void rt_int(u32 n, u32 at);
u32 rt_in8(u32 port, u32 at);
u32 rt_in16(u32 port, u32 at);
u32 rt_in32(u32 port, u32 at);
void rt_out8(u32 port, u32 v, u32 at);
void rt_out16(u32 port, u32 v, u32 at);
void rt_out32(u32 port, u32 v, u32 at);
void rt_load_seg(int idx, u16 sel);
u32 rt_sel_limit(u16 sel);
void rt_call(u32 target, u32 at);
void rt_jump(u32 target, u32 at);
void rt_unimpl(u32 at, const char *what);
void rt_trace_enter(u32 f);
void rt_trace_leave(u32 f);
void rt_bad_code(u32 at);
void rt_fell_off(u32 at);
void rt_noreturn(u32 at);
void rt_divide_error(u32 at);
void rt_magic_chain(u32 ra);   /* retf into a pre-installed (BIOS) handler */

typedef struct { u32 addr; void (*fn)(void); } FuncEnt;
/* natives marked "wrap" in tools/native_<module>.txt: the original stays callable */
typedef struct { u32 addr; void (*orig)(void); const char *native; } WrapEnt;
void (*rt_wrap_orig(const char *native, u32 *addr))(void);
/* function table of the program (module) currently loaded */
extern const FuncEnt *rt_functab;
extern int rt_nfuncs;
