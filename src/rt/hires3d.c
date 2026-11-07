/* Real higher internal resolution for the 3D game (3DGAME.EXE).
 *
 * The game renders its 3D view with BRender's software z-buffer renderer
 * into a 320x200 8-bit back buffer, whose background (pre-rendered rooms
 * with their own depth maps) is copied in first; then it draws the HUD and
 * sprites on top and copies the changed 16x10 tiles to the screen.
 *
 * BRender takes the target size from the pixelmaps it is given (up to
 * 2048x2048), so the same scene can be rendered at N times the size:
 *   - BrZbSceneRender (wrapped): after the game's own 320x200 render, the
 *     scene is rendered again into N x size colour and depth pixelmaps,
 *     seeded with the upscaled background and background depth, so the
 *     pre-rendered rooms keep hiding the characters correctly;
 *   - end of frame (the tile copy, wrapped): the high-resolution render
 *     becomes an overlay layer with a mask - every pixel the 3D pass drew,
 *     where the finished 320x200 frame still shows the game's own 3D
 *     render (not the HUD/sprites drawn over it).  The presenter (video.c)
 *     upscales the normal picture with the chosen filter and blends this
 *     layer on top, as long as the screen still shows that frame.
 * Videos and 2D screens carry only their original pixels; they go through
 * the upscale filter as before. */
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "rt.h"

extern int opt_scale;

/* BRender pixelmap (this version): +4 pixels, +0x10 row_bytes, +0x12 type,
   +0x18 width, +0x1a height, +0x1c/+0x1e origin; 0x30 bytes */
#define PM_PIXELS 0x04
#define PM_ROWB   0x10
#define PM_TYPE   0x12
#define PM_W      0x18
#define PM_H      0x1a
#define PM_OX     0x1c
#define PM_OY     0x1e
#define PM_SIZE   0x30
#define PMT_INDEX_8 3
#define PMT_DEPTH_16 11
#define G3D_BOUNDS_CB 0x4eb46cu        /* BRender zb render-bounds callback (BrZbSetRenderBoundsCallback 451728) */

static u32 g_pm_col, g_pm_dep;         /* our pixelmap structs (guest) */
static u32 g_px_col, g_px_dep;         /* their pixels (guest), sized for g_cap */
static int g_cap;                      /* scale the buffers were made for */
static u32 g_heap_owner;               /* heap of the program they belong to */

static int r_valid, r_w, r_h, r_s;     /* last high-res render */
static u32 r_back;                     /* the game's back buffer pixels */
static int r_rowb;
static u8 *r_snap;                     /* the game's own render, 320x200 */
static u8 *r_bg;                       /* the back buffer before rendering (background) */

static u8 *c_buf;                      /* high-res 3D layer: colour index per pixel */
static u8 *c_mask;                     /* 1 where the 3D pass drew and nothing covered it */
static int c_w, c_h, c_valid;
static u32 c_hash;
static u64 c_time;
static int frames_hi;

static u32 vga_hash(void)
{
    const u32 *p = (const u32 *)(MEM + VGA_WINDOW);
    u32 h = 2166136261u;
    for (int i = 0; i < 16000; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

void hires3d_reset(void)
{
    g_pm_col = g_pm_dep = g_px_col = g_px_dep = 0;
    g_cap = 0;
    r_valid = c_valid = 0;
}

static int ensure_buffers(int w, int h, int s)
{
    if (g_px_col && s <= g_cap) return 1;
    u32 cs = (u32)w * s * h * s;
    g_pm_col = heap_alloc(0x100);
    g_px_col = heap_alloc(cs);
    g_px_dep = heap_alloc(cs * 2);
    if (!g_pm_col || !g_px_col || !g_px_dep) { g_px_col = 0; return 0; }
    g_pm_dep = g_pm_col + 0x80;
    g_cap = s;
    return 1;
}

static void make_pm(u32 dst, u32 src, u32 pixels, int bpp, int s)
{
    memcpy(MEM + dst, MEM + src, PM_SIZE);
    int w = RD16(src + PM_W), h = RD16(src + PM_H);
    WR32(dst + PM_PIXELS, pixels);
    WR16(dst + PM_ROWB, w * s * bpp);
    WR16(dst + 0x14, 0);
    WR16(dst + 0x16, 0);
    WR16(dst + PM_W, w * s);
    WR16(dst + PM_H, h * s);
    WR16(dst + PM_OX, (s16)RD16(src + PM_OX) * s);
    WR16(dst + PM_OY, (s16)RD16(src + PM_OY) * s);
}

/* 0x4516fc BrZbSceneRender(world eax, camera edx, colour ebx, depth ecx) */
void N_g3d_scene_render(void)
{
    static void (*orig)(void);
    if (!orig) orig = rt_wrap_orig("N_g3d_scene_render", NULL);
    if (!orig) rt_fatal("scene render original missing");
    u32 world = cpu.r[R_EAX], cam = cpu.r[R_EDX], col = cpu.r[R_EBX], dep = cpu.r[R_ECX];
    int s = opt_scale;
    int w = col ? RD16(col + PM_W) : 0, h = col ? RD16(col + PM_H) : 0;
    int use = s > 1 && col && dep && RD8(col + PM_TYPE) == PMT_INDEX_8 && RD8(dep + PM_TYPE) == PMT_DEPTH_16 &&
              RD16(dep + PM_W) == w && RD16(dep + PM_H) == h && w * s <= 2048 && h * s <= 2048 &&
              RD16(col + 0x14) == 0 && RD16(col + 0x16) == 0 && w * h <= 320 * 240;
    if (use && !ensure_buffers(w, h, s)) use = 0;
    {
        static int n;
        if (n++ < 1) rt_log("hires3d: [%d] scene render %dx%d types %d/%d scale %d -> %s\n", n, w, h, col ? RD8(col + PM_TYPE) : -1,
                            dep ? RD8(dep + PM_TYPE) : -1, s, use ? "high-res" : "native only");
    }
    if (use) {
        /* seed: the background and its depth, scaled up */
        u32 cp = RD32(col + PM_PIXELS), dp = RD32(dep + PM_PIXELS);
        int crb = (s16)RD16(col + PM_ROWB), drb = (s16)RD16(dep + PM_ROWB);
        int W = w * s;
        if (!r_bg) r_bg = malloc(320 * 240);
        for (int y = 0; y < h; y++) memcpy(r_bg + y * w, MEM + cp + y * crb, w);
        for (int y = 0; y < h; y++) {
            const u8 *sc = MEM + cp + y * crb;
            const u16 *sd = (const u16 *)(MEM + dp + y * drb);
            u8 *dc = MEM + g_px_col + (u32)y * s * W;
            u16 *dd = (u16 *)(MEM + g_px_dep) + (u32)y * s * W;
            for (int x = 0; x < w; x++)
                for (int k = 0; k < s; k++) { dc[x * s + k] = sc[x]; dd[x * s + k] = sd[x]; }
            for (int k = 1; k < s; k++) {
                memcpy(dc + k * W, dc, W);
                memcpy(dd + k * W, dd, W * 2);
            }
        }
    }
    if (use) {
        /* the high-resolution pass first, so that everything BRender leaves
           behind for the game (screen bounds of the models, which the game
           uses for picking) comes from the game's own pass below */
        CPU entry = cpu;
        u32 rt = rt_tail_ret;
        rt_tail_ret = 0;
        make_pm(g_pm_col, col, g_px_col, 1, s);
        make_pm(g_pm_dep, dep, g_px_dep, 2, s);
        cpu.r[R_EAX] = world;
        cpu.r[R_EDX] = cam;
        cpu.r[R_EBX] = g_pm_col;
        cpu.r[R_ECX] = g_pm_dep;
        cpu.r[R_ESP] = (entry.r[R_ESP] - 64) & ~3u;
        WR32(cpu.r[R_ESP], MAGIC_IRET + 0xfb);
        /* no game code inside our pass: the render-bounds callback (it marks
           the game's 20x20 dirty-tile grid and would write far past it with
           high-res coordinates) is switched off, and interrupts wait */
        extern int rt_irq_hold;
        u32 bounds_cb = RD32(G3D_BOUNDS_CB);
        WR32(G3D_BOUNDS_CB, 0);
        rt_irq_hold = 1;
        orig();
        rt_irq_hold = 0;
        WR32(G3D_BOUNDS_CB, bounds_cb);
        cpu = entry;
        rt_tail_ret = rt;
    }
    orig();                                  /* the game's own render; returns to its caller's frame */
    if (!use) return;
    if (!r_snap) r_snap = malloc(320 * 240);
    {
        u32 cp = RD32(col + PM_PIXELS);
        int crb = (s16)RD16(col + PM_ROWB);
        for (int y = 0; y < h; y++) memcpy(r_snap + y * w, MEM + cp + y * crb, w);
        r_back = cp;
        r_rowb = crb;
    }
    r_valid = 1;
    r_w = w; r_h = h; r_s = s;
}

/* 0x41108a end-of-frame tile copy back buffer -> screen (al = buffer set) */
void N_g3d_frame_done(void)
{
    static void (*orig)(void);
    if (!orig) orig = rt_wrap_orig("N_g3d_frame_done", NULL);
    if (!orig) rt_fatal("frame copy original missing");
    orig();
    if (!r_valid) return;
    r_valid = 0;
    int w = r_w, h = r_h, s = r_s, W = w * s, H = h * s;
    if (w != 320 || h > 240) return;
    if (!c_buf || c_w * c_h < W * H) {
        free(c_buf); free(c_mask);
        c_buf = malloc((size_t)W * H);
        c_mask = malloc((size_t)W * H);
    }
    c_w = W; c_h = H;
    memcpy(c_buf, MEM + g_px_col, (size_t)W * H);
    /* the layer covers what the 3D pass drew (it differs from the seeded
       background) where the finished frame still shows the game's own 3D
       render (the HUD and sprites drawn later stay on the normal picture) */
    for (int y = 0; y < h; y++) {
        const u8 *bk = MEM + r_back + y * r_rowb, *sn = r_snap + y * w, *bg = r_bg + y * w;
        for (int k = 0; k < s; k++) {
            const u8 *hr = c_buf + (size_t)(y * s + k) * W;
            u8 *m = c_mask + (size_t)(y * s + k) * W;
            for (int x = 0; x < w; x++) {
                int keep = bk[x] == sn[x];
                for (int j = 0; j < s; j++) m[x * s + j] = keep && hr[x * s + j] != bg[x];
            }
        }
    }
    c_hash = vga_hash();
    c_time = SDL_GetTicks64();
    c_valid = 1;
    if (!frames_hi++) rt_log("hires3d: first %dx%d frame (screen %s the back buffer)\n", W, H,
                             memcmp(MEM + VGA_WINDOW, MEM + r_back, 64000) ? "differs from" : "matches");
}

/* for the presenter: the high-res picture of what the screen shows now */
int hires3d_frame(const u8 **pix, const u8 **mask, int *w, int *h)
{
    if (!c_valid || opt_scale <= 1) return 0;
    if (SDL_GetTicks64() - c_time > 1000) { c_valid = 0; return 0; }
    if (vga_hash() != c_hash) return 0;
    *pix = c_buf; *mask = c_mask; *w = c_w; *h = c_h;
    return 1;
}
