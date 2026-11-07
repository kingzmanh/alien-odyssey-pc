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
