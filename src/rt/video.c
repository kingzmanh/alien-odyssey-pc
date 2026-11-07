/* Display: a Windows window (SDL2) showing the game's 8-bit screen.
 *
 * VESA banked modes keep the full picture in a 4 MB frame buffer at guest
 * address VRAM_GUEST; the 64 KB window at A0000h holds the selected bank and
 * is swapped in and out when the game selects another bank. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include <stdlib.h>
#include <SDL2/SDL.h>
#include "rt.h"

#define VRAM_GUEST 0xe0000000u
#define VRAM_SIZE  (4u << 20)

int video_w, video_h, video_bank_cur;
u8 vga_pal[768];
static int video_bpp;
static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static int tex_w, tex_h;
static u32 pal32[256];
static int pal_dirty = 1;
static u64 last_present;

extern int opt_fullscreen;

void video_init(void)
{
    if (!VirtualAlloc(MEM + VRAM_GUEST, VRAM_SIZE, MEM_COMMIT, PAGE_READWRITE)) rt_fatal("cannot commit video memory");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");        /* window icon from the exe */
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
    win = SDL_CreateWindow("Alien Odyssey", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           1280, 960, SDL_WINDOW_RESIZABLE | (getenv("AO_HIDDEN") ? SDL_WINDOW_HIDDEN :
                                                              opt_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!win) rt_fatal("SDL window: %s", SDL_GetError());
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderPresent(ren);
    for (int i = 0; i < 256; i++) video_set_pal(i, i >> 2, i >> 2, i >> 2);
    SDL_ShowCursor(SDL_DISABLE);   /* the game draws its own pointer */
}

void video_set_mode(int w, int h, int bpp)
{
    video_w = w; video_h = h; video_bpp = bpp;
    video_bank_cur = 0;
    memset(MEM + VRAM_GUEST, 0, VRAM_SIZE);
    memset(MEM + VGA_WINDOW, 0, 0x10000);
    if (w && (w != tex_w || h != tex_h)) {
        if (tex) SDL_DestroyTexture(tex);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        tex_w = w; tex_h = h;
        SDL_RenderSetLogicalSize(ren, w, h * (w == 320 ? 6 : 5) / (w == 320 ? 5 : 5));
    }
    video_present(1);
}

void video_bank(u32 bank)
{
    if (bank * 0x10000u >= VRAM_SIZE) return;
    if ((int)bank == video_bank_cur) return;
    memcpy(MEM + VRAM_GUEST + video_bank_cur * 0x10000u, MEM + VGA_WINDOW, 0x10000);
    memcpy(MEM + VGA_WINDOW, MEM + VRAM_GUEST + bank * 0x10000u, 0x10000);
    video_bank_cur = (int)bank;
}

void video_set_pal(int idx, int r, int g, int b)
{
    idx &= 255;
    vga_pal[idx * 3] = (u8)(r & 63);
    vga_pal[idx * 3 + 1] = (u8)(g & 63);
    vga_pal[idx * 3 + 2] = (u8)(b & 63);
    pal_dirty = 1;
}

void video_present(int force)
{
    u64 now = SDL_GetTicks64();
    if (!force && now - last_present < 15) return;
    last_present = now;
    if (pal_dirty) {
        for (int i = 0; i < 256; i++) {
            u32 r = vga_pal[i * 3], g = vga_pal[i * 3 + 1], b = vga_pal[i * 3 + 2];
            pal32[i] = 0xff000000u | ((r << 2 | r >> 4) << 16) | ((g << 2 | g >> 4) << 8) | (b << 2 | b >> 4);
        }
        pal_dirty = 0;
    }
    {   /* AO_FPS: log how many distinct frames the game drew per second (testing) */
        static int on = -1, frames;
        static u32 last_hash;
        static u64 t0;
        if (on < 0) on = getenv("AO_FPS") != NULL;
        if (on && video_w == 320) {
            u32 h = 2166136261u;
            const u32 *p = (const u32 *)(MEM + VGA_WINDOW);
            for (int i = 0; i < 16000; i += 7) h = (h ^ p[i]) * 16777619u;
            if (h != last_hash) { frames++; last_hash = h; }
            if (now - t0 >= 5000) { if (t0) rt_log("fps: %.1f\n", frames * 1000.0 / (now - t0)); t0 = now; frames = 0; }
        }
    }
    SDL_RenderClear(ren);
    if (video_w && tex) {
        const u8 *src;
        if (video_w == 320) src = MEM + VGA_WINDOW;
        else {
            memcpy(MEM + VRAM_GUEST + video_bank_cur * 0x10000u, MEM + VGA_WINDOW, 0x10000);
            src = MEM + VRAM_GUEST;
        }
        void *pix;
        int pitch;
        if (SDL_LockTexture(tex, NULL, &pix, &pitch) == 0) {
            for (int y = 0; y < video_h; y++) {
                u32 *d = (u32 *)((u8 *)pix + y * pitch);
                const u8 *s = src + y * video_w;
                for (int x = 0; x < video_w; x++) d[x] = pal32[s[x]];
            }
            SDL_UnlockTexture(tex);
        }
        SDL_RenderCopy(ren, tex, NULL, NULL);
        /* AO_SHOT=<ms>: save the screen to shots/ every <ms> (testing) */
        static int shot_ms = -1;
        static u64 next_shot;
        static int shot_n;
        if (shot_ms < 0) { const char *e = getenv("AO_SHOT"); shot_ms = e ? atoi(e) : 0; }
        if (shot_ms > 0 && now >= next_shot) {
            next_shot = now + shot_ms;
            SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, video_w, video_h, 32, SDL_PIXELFORMAT_ARGB8888);
            {   /* AO_SHOTMEM=<hex addr>: shoot that guest buffer instead (debugging) */
                const char *e = getenv("AO_SHOTMEM");
                if (e) src = MEM + (u32)strtoul(e, NULL, 16);
            }
            if (sf) {
                for (int y = 0; y < video_h; y++) {
                    u32 *d = (u32 *)((u8 *)sf->pixels + y * sf->pitch);
                    for (int x = 0; x < video_w; x++) d[x] = pal32[src[y * video_w + x]];
                }
                char fn[64];
                CreateDirectoryA("shots", NULL);
                snprintf(fn, sizeof fn, "shots/shot_%04d.bmp", shot_n++);
                SDL_SaveBMP(sf, fn);
                SDL_FreeSurface(sf);
            }
        }
    }
    SDL_RenderPresent(ren);
}

/* pixel write into the current mode's frame (banked VESA modes keep the
   selected bank in the A000h window, the rest in VRAM_GUEST) */
void video_put_pixel(int x, int y, u8 c, int op)
{
    if (x < 0 || y < 0 || x >= video_w || y >= video_h) return;
    u8 *p;
    if (video_w == 320) p = MEM + VGA_WINDOW + y * 320 + x;
    else {
        u32 off = (u32)y * video_w + x;
        if ((int)(off >> 16) == video_bank_cur) p = MEM + VGA_WINDOW + (off & 0xffff);
        else p = MEM + VRAM_GUEST + off;
    }
    switch (op) {
    /* the graph library's internal plot codes (set by _setplotaction at 4640fe) */
    case 1: *p ^= c; break;          /* _GXOR */
    case 2: *p &= c; break;          /* _GAND */
    case 3: *p |= c; break;          /* _GOR */
    default: *p = c; break;          /* 0: _GPSET / _GPRESET */
    }
}

/* put the Windows pointer where the game moved its cursor (logical coords);
   only while our window has the focus, never in background test runs */
void video_warp_mouse(int x, int y)
{
    if (getenv("AO_HIDDEN") || !win) return;
    if (!(SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS)) return;
    SDL_Rect vp;
    float sx, sy;
    SDL_RenderGetViewport(ren, &vp);
    SDL_RenderGetScale(ren, &sx, &sy);
    SDL_WarpMouseInWindow(win, (int)((vp.x + x) * sx), (int)((vp.y + y) * sy));
}

/* short status message in the window title (option toggles) */
static u64 status_until;
void video_status(const char *msg)
{
    char t[160];
    snprintf(t, sizeof t, "Alien Odyssey  -  %s", msg);
    if (win) SDL_SetWindowTitle(win, t);
    status_until = SDL_GetTicks64() + 3000;
    rt_log("status: %s\n", msg);
}

void video_status_tick(void)
{
    if (status_until && SDL_GetTicks64() > status_until) {
        status_until = 0;
        if (win) SDL_SetWindowTitle(win, "Alien Odyssey");
    }
}
