/* Native replacements for game/library functions that cannot be recompiled
 * as they are (self-modifying code) or that talk to PC hardware.  Each one
 * stands in for the whole original function (listed per program in
 * tools/native_<module>.txt) and works on the guest register file like the
 * original: arguments in registers (Watcom register convention), result in
 * eax, and it pops its own return address. */
#include <stdlib.h>
#include "rt.h"

#define RET(n) (cpu.r[R_ESP] += 4 + (n))

/* BRP video frame decoders that draw at double size (FOREST, VILLAGE1/2,
 * MINE, 3DGAME, INTRO).  The originals patch the table address, the row
 * length and the line pitch into their own instructions through a code
 * alias selector before running.
 *   eax = destination, edx = parameter block:
 *     +0 table: one dword per colour index = 2x2 pixels (low word = top row)
 *     +4 change bitmap (delta frames, one bit per 2x2 block, msb first)
 *     +8 colour index stream
 *     +0Ch rows, +10h blocks per row (bytes; 0 = 256)
 *     +14h line pitch (also added after each row) */
void N_brp_key2x(void)
{
    u32 dst = cpu.r[R_EAX], blk = cpu.r[R_EDX];
    u32 table = RD32(blk), src = RD32(blk + 8), pitch = RD32(blk + 0x14);
    u8 rows = RD8(blk + 0xc), cols = RD8(blk + 0x10);
    u8 ch = rows;
    do {
        u8 cl = cols;
        do {
            u32 v = RD32(table + RD8(src) * 4u);
            src++;
            WR16(dst, v);
            WR16(dst + pitch, v >> 16);
            dst += 2;
        } while (--cl);
        dst += pitch;
    } while (--ch);
    cpu.r[R_EAX] = 1;
    cpu.r[R_ECX] &= 0xffff0000u;
    RET(0);
}

void N_brp_delta2x(void)
{
    u32 dst = cpu.r[R_EAX], blk = cpu.r[R_EDX];
    u32 table = RD32(blk), bits = RD32(blk + 4), src = RD32(blk + 8), pitch = RD32(blk + 0x14);
    u8 rows = RD8(blk + 0xc), cols = RD8(blk + 0x10);
    u8 dl = 0, dh = 0;
    u8 ch = rows;
    do {
        u8 cl = cols;
        do {
            if (!dh) { dl = RD8(bits); bits++; dh = 8; }
            int draw = dl & 0x80;
            dl <<= 1;
            if (draw) {
                u32 v = RD32(table + RD8(src) * 4u);
                src++;
                WR16(dst, v);
                WR16(dst + pitch, v >> 16);
            }
            dst += 2;
            dh--;
        } while (--cl);
        dst += pitch;
    } while (--ch);
    cpu.r[R_EAX] = 1;
    cpu.r[R_ECX] &= 0xffff0000u;
    cpu.r[R_EDX] = (cpu.r[R_EDX] & 0xffff0000u) | ((u32)dh << 8) | dl;
    RET(0);
}

/* MINE.EXE 460000: rotated texture floor (the cart ride's ground plane).
 * The original is hand-written asm that jumps into the middle of two
 * unrolled loops (one stepping left from the screen centre, one stepping
 * right), with the entry point per row taken from two tables - i.e. the
 * span length per row.  Same arithmetic here: 8.16 fixed point texture
 * coordinates in a 256x256 texture, carries/borrows as in the asm.
 *   eax = destination (row 0), ebx/ecx = texture start u/v, edx = angle */
#define MN_SINTAB   0x480a7cu   /* -> dword table, 1024 entries (+256 = cos) */
#define MN_TEXPM    0x47b338u   /* -> texture pixelmap, pixels at +4 */
#define MN_LEFT     0x47a0beu   /* per row: entry offset into the left loop */
#define MN_RIGHT    0x47a2eeu   /* per row: entry offset into the right loop */
#define MN_VARS     0x479e20u
void N_mine_floor(void)
{
    u32 dst = cpu.r[R_EAX] + 0xa0;
    u32 U = cpu.r[R_EBX] << 16, V = cpu.r[R_ECX] << 16;
    u32 a = cpu.r[R_EDX];
    u32 tab = RD32(MN_SINTAB);
    u32 c = RD32(tab + a * 4), s = RD32(tab + a * 4 + 0x400);
    WR32(MN_VARS + 8, U);
    WR32(MN_VARS + 12, V);
    WR32(MN_VARS + 0x10, c);
    WR32(MN_VARS + 0x14, s);
    U -= c * 75u;
    V -= s * 75u;
    u32 b = (a - 0x100) & 0x3ff;
    u32 du = 0u - RD32(tab + b * 4), dv = 0u - RD32(tab + b * 4 + 0x400);
    u8 du_i = (u8)(du >> 16), dv_i = (u8)(dv >> 16);
    u16 du_f = (u16)du, dv_f = (u16)dv;
    u32 tex = RD32(RD32(MN_TEXPM) + 4);
    for (int row = 0x8b; row >= 0; row--) {
        u32 lo = RD32(MN_LEFT + row * 4), ro = RD32(MN_RIGHT + row * 4);
        /* left loop: 128 steps of 14 bytes, each: step back, then draw at --edi */
        {
            u8 ui = (u8)(U >> 16), vi = (u8)(V >> 16);
            u16 uf = (u16)U, vf = (u16)V;
            u32 d = dst;
            for (u32 k = lo / 14; k < 128; k++) {
                u32 br = uf < du_f; uf -= du_f; ui -= du_i + br;
                br = vf < dv_f; vf -= dv_f; vi -= dv_i + br;
                WR8(--d, RD8(tex + ((u32)vi << 8 | ui)));
            }
        }
        /* right loop: draw at edi, then 127 steps of (edi++, step, draw); the
           row's entry lands either on the first draw or on a later draw */
        {
            u8 ui = (u8)(U >> 16), vi = (u8)(V >> 16);
            u16 uf = (u16)U, vf = (u16)V;
            u32 d = dst;
            u32 k0 = ro ? (ro - 5 - 9) / 14 + 1 : 0;   /* steps already skipped */
            WR8(d, RD8(tex + ((u32)vi << 8 | ui)));
            for (u32 k = k0; k < 127; k++) {
                d++;
                u32 cy = (u32)uf + du_f > 0xffff; uf += du_f; ui += du_i + cy;
                cy = (u32)vf + dv_f > 0xffff; vf += dv_f; vi += dv_i + cy;
                WR8(d, RD8(tex + ((u32)vi << 8 | ui)));
            }
        }
        dst += 320;
        U += c;
        V += s;
    }
    WR32(MN_VARS, U);
    WR32(MN_VARS + 4, V);
    WR32(MN_VARS + 0x20, 0xffffffffu);
    RET(0);
}
