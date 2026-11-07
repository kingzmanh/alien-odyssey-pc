/* LE loader: maps a game program's objects into guest memory exactly as
 * tools/le.py does for the recompiler (same relocation), applying fixups.
 * 3DGAME.EXE has DOS/4GW bound in front: its LE sits behind a chain of
 * MZ/BW headers, and its page offsets count from the MZ that holds it. */
#include <stdlib.h>
#include <string.h>
#include "rt.h"

static const u32 reloc_delta[8] = {0, 0x400000, 0x400000, 0x400000, 0, 0, 0, 0}; /* by object index */

static u32 rd32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }

int le_load(const char *path, LoadInfo *li)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *d = malloc(n);
    if (fread(d, 1, n, f) != (size_t)n) { fclose(f); free(d); return 0; }
    fclose(f);
    u32 mz = 0, le = 0;
    for (int k = 0; k < 16 && mz + 0x40 < (u32)n; k++) {
        if (d[mz] == 'M' && d[mz + 1] == 'Z') {
            u32 lf = mz + rd32(d + mz + 0x3c);
            if (lf + 2 < (u32)n && d[lf] == 'L' && d[lf + 1] == 'E') { le = lf; break; }
            u32 cblp = rd16(d + mz + 2), cp = rd16(d + mz + 4);
            mz += cp * 512 - (cblp ? 512 - cblp : 0);
        } else if (d[mz] == 'B' && d[mz + 1] == 'W') mz = rd32(d + mz + 0x1c);
        else break;
    }
    if (!le) { free(d); return 0; }
    const u8 *h = d + le;
    u32 page_size = rd32(h + 0x28), last_page = rd32(h + 0x2c);
    u32 eip_obj = rd32(h + 0x18), eip = rd32(h + 0x1c), esp_obj = rd32(h + 0x20), esp = rd32(h + 0x24);
    u32 nobj = rd32(h + 0x44), obj_off = rd32(h + 0x40), npages = rd32(h + 0x14);
    u32 pagemap = rd32(h + 0x48), fixpage = rd32(h + 0x68), fixrec = rd32(h + 0x6c), data_pages = rd32(h + 0x80) + mz;
    u32 base[8], vsize[8], pmi[8], np[8];
    li->nobj = nobj;
    for (u32 i = 0; i < nobj && i < 7; i++) {
        const u8 *o = h + obj_off + i * 24;
        vsize[i] = rd32(o);
        base[i] = rd32(o + 4) + reloc_delta[i + 1];
        pmi[i] = rd32(o + 12);
        np[i] = rd32(o + 16);
        li->obj_base[i] = base[i];
        li->obj_size[i] = vsize[i] > np[i] * page_size ? vsize[i] : np[i] * page_size;
        memset(MEM + base[i], 0, li->obj_size[i]);
        for (u32 p = 0; p < np[i]; p++) {
            u32 gp = pmi[i] - 1 + p;
            const u8 *pm = h + pagemap + gp * 4;
            u32 pnum = (pm[0] << 16) | (pm[1] << 8) | pm[2];
            u32 src = data_pages + (pnum - 1) * page_size;
            u32 sz = gp == npages - 1 ? last_page : page_size;
            memcpy(MEM + base[i] + p * page_size, d + src, sz);
        }
    }
    /* fixups */
    for (u32 i = 0; i < nobj; i++) {
        for (u32 p = 0; p < np[i]; p++) {
            u32 gp = pmi[i] - 1 + p;
            u32 pos = le + fixrec + rd32(h + fixpage + gp * 4);
            u32 end = le + fixrec + rd32(h + fixpage + (gp + 1) * 4);
            u32 page_lin = base[i] + p * page_size;
            while (pos < end) {
                u8 st = d[pos], fl = d[pos + 1];
                pos += 2;
                u32 cnt = 0;
                s16 off1 = 0;
                if (st & 0x20) cnt = d[pos++];
                else { off1 = (s16)rd16(d + pos); pos += 2; }
                u32 tobj;
                if (fl & 0x40) { tobj = rd16(d + pos); pos += 2; } else tobj = d[pos++];
                u32 toff = 0;
                u32 stype = st & 0xf;
                if (stype != 2) {
                    if (fl & 0x10) { toff = rd32(d + pos); pos += 4; }
                    else { toff = rd16(d + pos); pos += 2; }
                }
                u32 tgt = base[tobj - 1] + toff;
                u32 k = (st & 0x20) ? cnt : 1;
                for (u32 j = 0; j < k; j++) {
                    s16 so;
                    if (st & 0x20) { so = (s16)rd16(d + pos); pos += 2; } else so = off1;
                    u32 sl = page_lin + so;
                    switch (stype) {
                    case 7: WR32(sl, tgt); break;
                    case 8: WR32(sl, tgt - (sl + 4)); break;
                    case 5: WR16(sl, tgt); break;
                    case 2: WR16(sl, (tobj == 1 || tobj == 2) ? SEL_CODE : SEL_DATA); break;
                    default: rt_log("le: fixup type %u ignored\n", stype);
                    }
                }
            }
        }
    }
    li->entry = base[eip_obj - 1] + eip;
    li->stack = base[esp_obj - 1] + esp;
    free(d);
    return 1;
}
