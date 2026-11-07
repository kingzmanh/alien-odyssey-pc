/* HD text: the game's text redrawn with a smooth font at screen resolution.
 *
 * All six programs draw text with the same glyph blitter (a plain and a
 * colour-table variant, tools/native_<module>.txt "wrap"): eax = glyph
 * index, edx/ebx = x/y; a glyph is width, height and 8-bit pixels (0 =
 * transparent), and the character code is glyph index + 21h in every font
 * of the game.  After the game's own blit we record the character, its
 * cell and the colours it left in the frame.  The presenter (video.c) asks
 * for the characters that are still on the screen, paints their low-res
 * pixels over with the cell's background and draws the characters with a
 * TrueType font on top, at output resolution. */
#include <stdlib.h>
#include <string.h>
#include "rt.h"

/* globals of the blitter, read from its code (same code in every program):
   +23h mov esi,[eax*4+GLYPHS]  +34h mov [W],ecx  +3Ah mov [H],edx
   +60h mov [X],eax  +65h mov [Y],ebx  +7Eh mov edi,[edi*4+ROWS]
   +8Bh mov ebx,[PM] (destination pixelmap, pixels at +4)
   +01h test word ptr [MODE],0FFFFh (0 = set up only, <0 = draw only) */
typedef struct { u32 glyphs, w, h, x, y, rows, pm; } Blit;
static Blit bl;
static u32 bl_addr;

#define MAXT 768
typedef struct {
    u16 x, y, w, h;
    u8 ch, used;
    u8 px[48 * 48];     /* what the glyph left in the frame (0 where transparent) */
    u8 pre[48 * 48];    /* the frame there before the glyph was drawn */
    short map[256];     /* background colour -> colour drawn over it (translucent fonts) */
} TextCell;
static TextCell cells[MAXT];
static int ncells;
static unsigned gen;   /* changes whenever the set of cells changes */

void hdtext_reset(void) { ncells = 0; bl_addr = 0; gen++; }

static void blit_setup(void)
{
    u32 dummy;
    void (*o)(void) = rt_wrap_orig("N_glyph_draw", &dummy);
    (void)o;
    bl_addr = dummy;
    bl.glyphs = RD32(bl_addr + 0x23 + 3);
    bl.w = RD32(bl_addr + 0x34 + 2);
    bl.h = RD32(bl_addr + 0x3a + 2);
    bl.x = RD32(bl_addr + 0x60 + 1);
    bl.y = RD32(bl_addr + 0x65 + 2);
    bl.rows = RD32(bl_addr + 0x7e + 3);
    bl.pm = RD32(bl_addr + 0x8b + 2);
}

static u8 pre_buf[48 * 48];
static int pre_ok;

/* the screen under the glyph, before the game draws it */
static void capture_pre(u32 w, u32 h, s32 x, s32 y)
{
    pre_ok = 0;
    if (w == 0 || h == 0 || w > 48 || h > 48 || x < 0 || y < 0 || x + w > 320 || y + h > 200) return;
    u32 pm = RD32(bl.pm);
    if (!pm) return;
    u32 dst = RD32(pm + 4) + RD32(bl.rows + y * 4) + x;
    for (u32 j = 0; j < h; j++)
        for (u32 i = 0; i < w; i++) pre_buf[j * w + i] = RD8(dst + j * 320 + i);
    pre_ok = 1;
}

static void record(u32 glyph)
{
    if (!bl_addr) blit_setup();
    u32 w = RD32(bl.w), h = RD32(bl.h);
    s32 x = (s32)RD32(bl.x), y = (s32)RD32(bl.y);
    if (w == 0 || h == 0 || w > 48 || h > 48 || x < 0 || y < 0 || x + w > 320 || y + h > 200) return;
    u32 pm = RD32(bl.pm);
    if (!pm) return;
    u32 dst = RD32(pm + 4) + RD32(bl.rows + y * 4) + x;
    u32 gp = RD32(bl.glyphs + glyph * 4) + 8;
    /* replace an older character in the same cell, else take a free slot */
    int k = -1;
    for (int i = 0; i < ncells; i++)
        if (cells[i].x == x && cells[i].y == y) { k = i; break; }
    if (k < 0) {
        for (int i = 0; i < ncells; i++) if (!cells[i].used) { k = i; break; }
        if (k < 0) { if (ncells >= MAXT) return; k = ncells++; }
    }
    TextCell *c = &cells[k];
    u8 px[48 * 48];
    for (u32 j = 0; j < h; j++)
        for (u32 i = 0; i < w; i++)
            px[j * w + i] = RD8(gp + j * w + i) ? RD8(dst + j * 320 + i) : 0;
    /* the same character redrawn the same way (HUDs every frame): no change */
    if (c->used && c->w == w && c->h == h && c->ch == (u8)(glyph + 0x21) && !memcmp(c->px, px, w * h)) return;
    if (pre_ok) memcpy(c->pre, pre_buf, w * h);
    else for (u32 j = 0; j < w * h; j++) c->pre[j] = 0;
    for (int v = 0; v < 256; v++) c->map[v] = -1;
    for (u32 j = 0; j < w * h; j++) if (px[j]) c->map[c->pre[j]] = px[j];
    c->x = x; c->y = y; c->w = w; c->h = h;
    c->ch = (u8)(glyph + 0x21);
    c->used = 1;
    memcpy(c->px, px, w * h);
    gen++;
}

static void wrap_call(const char *name)
{
    void (*orig)(void) = rt_wrap_orig(name, NULL);
    u32 glyph = cpu.r[R_EAX];
    if (!bl_addr) blit_setup();
    /* +1: test word [MODE],-1: 0 = only measure, else draw */
    s16 mode = (s16)RD16(RD32(bl_addr + 1 + 3));
    if (mode > 0) {   /* sets up from the glyph (w, h at its start) and edx/ebx */
        u32 g = RD32(bl.glyphs + glyph * 4);
        capture_pre(RD32(g), RD32(g + 4), (s32)cpu.r[R_EDX], (s32)cpu.r[R_EBX]);
    } else if (mode < 0) capture_pre(RD32(bl.w), RD32(bl.h), (s32)RD32(bl.x), (s32)RD32(bl.y));
    orig();
    if (mode) record(glyph);
}

/* plain glyph blit / colour-table glyph blit */
void N_glyph_draw(void) { wrap_call("N_glyph_draw"); }
void N_glyph_draw_col(void) { wrap_call("N_glyph_draw_col"); }

/* for the presenter: characters whose pixels are still on the screen
   (frame = what is shown now, 320x200 indices); writes the cell background
   over the glyph pixels in `base`.  Returns the number of cells in out[]. */
int hdtext_visible(const u8 *frame, u8 *base, HdChar *out, int max, unsigned *g)
{
    int n = 0;
    for (int i = 0; i < ncells && n < max; i++) {
        TextCell *c = &cells[i];
        if (!c->used) continue;
        int ok = 1, opaque = 0;
        for (int j = 0; j < c->h && ok; j++)
            for (int x = 0; x < c->w; x++) {
                u8 v = c->px[j * c->w + x];
                if (v && frame[(c->y + j) * 320 + c->x + x] != v) { ok = 0; break; }
                if (v) opaque++;
            }
        if (!ok || !opaque) { c->used = 0; gen++; continue; }
        if (c->h < 8) continue;   /* tiny fonts (HUD readouts) stay as drawn: too small for a font to beat the pixels */
        /* the glyph's pixels get back what was under them; its colour: the
           most common one it drew */
        int h2[256] = {0}, b1 = 0, col = 255;
        int x0 = 99, y0 = 99, x1 = -1, y1 = -1;
        for (int j = 0; j < c->h; j++)
            for (int x = 0; x < c->w; x++) {
                u8 v = c->px[j * c->w + x];
                if (!v) continue;
                base[(c->y + j) * 320 + c->x + x] = c->pre[j * c->w + x];
                if (++h2[v] > b1) { b1 = h2[v]; col = v; }
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (j < y0) y0 = j;
                if (j > y1) y1 = j;
            }
        HdChar *o = &out[n++];
        o->x = c->x; o->y = c->y; o->w = c->w; o->h = c->h;
        o->ix = c->x + x0; o->iy = c->y + y0; o->iw = x1 - x0 + 1; o->ih = y1 - y0 + 1;
        o->ch = c->ch; o->colour = (u8)col;
        o->pre = c->pre; o->map = c->map;
    }
    *g = gen;
    return n;
}
