/* Video BIOS / VESA, mouse driver, MSCDEX and keyboard BIOS services,
 * answered by the PC side (video.c, input in irq.c). */
#include <string.h>
#include "rt.h"

extern u32 ptr_es(u32 off);
extern void set_es(u16 v);

#define AX (cpu.r[R_EAX] & 0xffff)
#define AL (cpu.r[R_EAX] & 0xff)
#define AH ((cpu.r[R_EAX] >> 8) & 0xff)
#define BX (cpu.r[R_EBX] & 0xffff)
#define BL (cpu.r[R_EBX] & 0xff)
#define BH ((cpu.r[R_EBX] >> 8) & 0xff)
#define CX (cpu.r[R_ECX] & 0xffff)
#define DX (cpu.r[R_EDX] & 0xffff)
#define SETAX(v) (cpu.r[R_EAX] = (cpu.r[R_EAX] & 0xffff0000u) | ((v) & 0xffff))
#define SETAL(v) (cpu.r[R_EAX] = (cpu.r[R_EAX] & 0xffffff00u) | ((v) & 0xff))
#define SETAH(v) (cpu.r[R_EAX] = (cpu.r[R_EAX] & 0xffff00ffu) | (((v) & 0xff) << 8))
#define SETBX(v) (cpu.r[R_EBX] = (cpu.r[R_EBX] & 0xffff0000u) | ((v) & 0xffff))
#define SETBL(v) (cpu.r[R_EBX] = (cpu.r[R_EBX] & 0xffffff00u) | ((v) & 0xff))
#define SETBH(v) (cpu.r[R_EBX] = (cpu.r[R_EBX] & 0xffff00ffu) | (((v) & 0xff) << 8))
#define SETCX(v) (cpu.r[R_ECX] = (cpu.r[R_ECX] & 0xffff0000u) | ((v) & 0xffff))
#define SETDX(v) (cpu.r[R_EDX] = (cpu.r[R_EDX] & 0xffff0000u) | ((v) & 0xffff))

static int cur_mode = 3, cur_vesa = 0;

typedef struct { u16 mode, w, h; } VMode;
static const VMode vmodes[] = {
    {0x100, 640, 400}, {0x101, 640, 480}, {0x103, 800, 600}, {0x105, 1024, 768},
};
#define NVM (int)(sizeof vmodes / sizeof vmodes[0])

/* static data the VESA info block points at (real-mode far pointers) */
#define VBE_STR   0x0500u
#define VBE_MODES 0x0540u

static void vbe_info(u32 d)
{
    memset(MEM + d, 0, 256);
    int v2 = RD32(d) == 0x32454256; /* caller asked for VBE2 info */
    memcpy(MEM + d, "VESA", 4);
    WR16(d + 4, 0x0102);
    strcpy((char *)MEM + VBE_STR, "PC Native Video");
    WR16(d + 6, VBE_STR & 15); WR16(d + 8, VBE_STR >> 4);
    WR32(d + 10, 0);
    for (int i = 0; i < NVM; i++) WR16(VBE_MODES + i * 2, vmodes[i].mode);
    WR16(VBE_MODES + NVM * 2, 0xffff);
    WR16(d + 14, VBE_MODES & 15); WR16(d + 16, VBE_MODES >> 4);
    WR16(d + 18, 64); /* 4 MB */
    (void)v2;
}

static int vbe_mode_info(u32 mode, u32 d)
{
    mode &= 0x1ff;
    for (int i = 0; i < NVM; i++) if (vmodes[i].mode == mode) {
        memset(MEM + d, 0, 256);
        WR16(d + 0, 0x001b);        /* supported, color, graphics */
        WR8(d + 2, 7);              /* window A: exists, readable, writable */
        WR8(d + 3, 0);
        WR16(d + 4, 64);            /* granularity KB */
        WR16(d + 6, 64);            /* window size KB */
        WR16(d + 8, 0xa000);
        WR16(d + 10, 0);
        WR32(d + 12, 0);            /* no window function: use int 10h 4F05 */
        WR16(d + 16, vmodes[i].w);
        WR16(d + 18, vmodes[i].w);
        WR16(d + 20, vmodes[i].h);
        WR8(d + 22, 8); WR8(d + 23, 16);
        WR8(d + 24, 1); WR8(d + 25, 8); WR8(d + 26, 1);
        WR8(d + 27, 4); WR8(d + 28, 0);
        WR8(d + 29, (u8)(4 * 1024 * 1024 / ((u32)vmodes[i].w * vmodes[i].h) - 1));
        WR8(d + 30, 1);
        return 1;
    }
    return 0;
}

void rt_int10(void)
{
    u32 ah = AH;
    if (ah == 0x4f) {
        switch (AL) {
        case 0x00: vbe_info(ptr_es(cpu.r[R_EDI])); SETAX(0x004f); return;
        case 0x01: SETAX(vbe_mode_info(CX, ptr_es(cpu.r[R_EDI])) ? 0x004f : 0x014f); return;
        case 0x02: {
            u32 m = BX & 0x1ff;
            for (int i = 0; i < NVM; i++) if (vmodes[i].mode == m) {
                video_set_mode(vmodes[i].w, vmodes[i].h, 8);
                cur_vesa = m;
                SETAX(0x004f);
                rt_log("video: VESA mode %03x (%dx%d)\n", m, vmodes[i].w, vmodes[i].h);
                return;
            }
            if (m < 0x100) { cpu.r[R_EAX] = m; rt_int10(); SETAX(0x004f); return; }
            SETAX(0x014f);
            return;
        }
        case 0x03: SETBX(cur_vesa ? cur_vesa : cur_mode); SETAX(0x004f); return;
        case 0x05:
            if (BH == 0) video_bank(DX); else SETDX(video_bank_cur);
            SETAX(0x004f);
            return;
        case 0x06: rt_log("vbe 4f06 bl=%x cx=%u\n", BL, CX); SETBX(video_w); SETCX(video_w); SETDX(video_h); SETAX(0x004f); return;
        case 0x07: { static int n; if (n++ < 20) rt_log("vbe 4f07 bl=%x cx=%u dx=%u\n", BL, CX, DX); }
            SETAX(0x004f); if (BL == 1) { SETCX(0); SETDX(0); } return;
        case 0x08: SETBH(6); SETAX(0x004f); return;
        case 0x09: {
            u32 a = ptr_es(cpu.r[R_EDI]);
            for (u32 i = 0; i < CX; i++) {
                u32 e = a + i * 4;
                video_set_pal(DX + i, RD8(e + 2), RD8(e + 1), RD8(e));
            }
            SETAX(0x004f);
            return;
        }
        default: SETAX(0x014f); return;
        }
    }
    switch (ah) {
    case 0x00:
        cur_mode = AL & 0x7f;
        cur_vesa = 0;
        if (cur_mode == 0x13) video_set_mode(320, 200, 8);
        else video_set_mode(0, 0, 0);
        WR8(0x449, cur_mode);
        rt_log("video: BIOS mode %02x\n", cur_mode);
        break;
    case 0x01: case 0x02: case 0x05: case 0x06: case 0x07: case 0x09: case 0x0b: break;
    case 0x03: SETCX(0x0607); SETDX(0); break;
    case 0x08: SETAX(0x0720); break;
    case 0x0e: rt_log("%c", AL); break;
    case 0x0f: SETAL(cur_mode); SETAH(cur_mode == 0x13 ? 40 : 80); SETBH(0); break;
    case 0x10:
        switch (AL) {
        case 0x10: video_set_pal(BX, (DX >> 8) & 0xff, (CX >> 8) & 0xff, CX & 0xff); break;
        case 0x12: {
            u32 a = ptr_es(cpu.r[R_EDX]);
            for (u32 i = 0; i < CX; i++) video_set_pal(BX + i, RD8(a + i * 3), RD8(a + i * 3 + 1), RD8(a + i * 3 + 2));
            break;
        }
        case 0x15: { u32 i = BX & 0xff; SETDX((vga_pal[i * 3] << 8) | (DX & 0xff));
            SETCX((vga_pal[i * 3 + 1] << 8) | vga_pal[i * 3 + 2]); break; }
        case 0x17: {
            u32 a = ptr_es(cpu.r[R_EDX]);
            for (u32 i = 0; i < CX; i++) for (int k = 0; k < 3; k++) WR8(a + i * 3 + k, vga_pal[((BX + i) & 0xff) * 3 + k]);
            break;
        }
        default: break;
        }
        break;
    case 0x11: break;
    case 0x12:
        if (BL == 0x10) { SETBH(0); SETBL(3); SETCX(0x0009); }
        else SETAL(0x12);
        break;
    case 0x1a: if (AL == 0) { SETAL(0x1a); SETBX(0x0008); } break;
    case 0x6f: case 0x5f: break; /* other vendors' extensions: not present */
    default:
        rt_log("video bios: ah=%02x al=%02x not provided\n", ah, AL);
    }
}

/* ---- mouse ---- */
static int mx_min = 0, mx_max = 639, my_min = 0, my_max = 479;
static int mick_x, mick_y;
static int last_raw_x, last_raw_y;
static int press_n[3], release_n[3];
u64 mouse_mickey_poll_time;   /* when the game last read relative motion (aiming) */

/* relative motion for int 33h/0Bh: raw Windows counts scaled by the
   sensitivity option.  The game moves its crosshair one screen pixel per
   count, made for ~400 cpi mice of 1995; today's mice report 4x as many
   counts, so 100% gives the game a quarter of them */
void mouse_motion(int dx, int dy)
{
    extern int opt_mouse_sens, opt_mouse_invert;
    static int fx, fy;   /* fractions, in 1/100 counts */
    fx += dx * opt_mouse_sens;
    fy += (opt_mouse_invert ? -dy : dy) * opt_mouse_sens;
    mick_x += fx / 400; fx %= 400;
    mick_y += fy / 400; fy %= 400;
}

/* button press / release counters for int 33h/05h, 06h */
void mouse_button_event(int b, int down)
{
    for (int i = 0; i < 3; i++)
        if (b & (1 << i)) { if (down) press_n[i]++; else release_n[i]++; }
}

/* move the cursor (driver coordinates) - also moves the Windows pointer */
static void mouse_set_pos(int x, int y)
{
    extern void video_warp_mouse(int x, int y);
    int w = video_w ? video_w : 640, h = video_h ? video_h : 480;
    int lx = (x - mx_min) * w / (mx_max - mx_min + 1);
    int ly = (y - my_min) * h / (my_max - my_min + 1);
    if (lx < 0) lx = 0;
    if (ly < 0) ly = 0;
    if (lx >= w) lx = w - 1;
    if (ly >= h) ly = h - 1;
    mouse_x = lx;
    mouse_y = ly;
    video_warp_mouse(lx, ly);
}

static void mouse_pos(int *x, int *y)
{
    *x = mx_min + mouse_x * (mx_max - mx_min + 1) / (video_w ? video_w : 640);
    *y = my_min + mouse_y * (my_max - my_min + 1) / (video_h ? video_h : 480);
    if (*x > mx_max) *x = mx_max;
    if (*y > my_max) *y = my_max;
}

void rt_int33(void)
{
    { static int seen[64]; if (AX < 64 && !seen[AX]++) { rt_log("mouse: fn %02x first use\n", AX); extern void rt_backtrace(const char *); rt_backtrace("mouse"); } }
    switch (AX) {
    case 0x00: case 0x21: /* reset: like the DOS driver, centre the cursor */
        SETAX(0xffff); SETBX(2);
        mouse_set_pos((mx_min + mx_max + 1) / 2, (my_min + my_max + 1) / 2);
        break;
    case 0x01: case 0x02: break;
    case 0x03: { int x, y; input_pump(); mouse_pos(&x, &y); { extern int mouse_buttons_reported(void); SETBX(mouse_buttons_reported()); } SETCX(x); SETDX(y); break; }
    case 0x04: mouse_set_pos((s16)CX, (s16)DX); break;
    case 0x07: mx_min = (s16)CX; mx_max = (s16)DX; break;
    case 0x08: my_min = (s16)CX; my_max = (s16)DX; break;
    case 0x05: case 0x06: {   /* button press / release information */
        int b = BX & 3;
        int *cnt = AX == 5 ? press_n : release_n;
        int x, y;
        input_pump();
        mouse_pos(&x, &y);
        { extern int mouse_buttons_reported(void); SETAX(mouse_buttons_reported()); }
        SETBX(b < 3 ? cnt[b] : 0);
        if (b < 3) cnt[b] = 0;
        SETCX(x); SETDX(y);
        break;
    }
    case 0x0b: {
        extern u64 SDL_GetTicks64(void);
        input_pump();
        int cx = mick_x < -32768 ? -32768 : mick_x > 32767 ? 32767 : mick_x;
        int dy = mick_y < -32768 ? -32768 : mick_y > 32767 ? 32767 : mick_y;
        SETCX(cx); SETDX(dy);
        mick_x = mick_y = 0;
        mouse_mickey_poll_time = SDL_GetTicks64();
        break;
    }
    case 0x0c: case 0x14:
        rt_log("mouse: event handler %04x:%08x mask %04x (not used, polling instead)\n", cpu.sreg[S_ES], cpu.r[R_EDX], CX);
        break;
    case 0x0f: case 0x1a: case 0x1d: case 0x13: break;
    case 0x1b: SETBX(50); SETCX(50); SETDX(50); break;
    case 0x24: SETBX(0x0626); SETCX(0x0400); break;
    default: rt_log("mouse: function %04x not provided\n", AX);
    }
    (void)last_raw_x; (void)last_raw_y;
}

/* ---- MSCDEX / multiplex ---- */
void rt_int2f(void)
{
    switch (AX) {
    case 0x1500: SETBX(1); SETCX(3); break;            /* one CD drive: D: */
    case 0x150b: SETAX(CX == 3 ? 0x5ad8 : 0); SETBX(0xadad); break;
    case 0x150c: SETBX(0x0215); break;
    case 0x150d: WR8(ptr_es(cpu.r[R_EBX]), 3); break;
    case 0x150f: {   /* get directory entry: ES:BX path, SI:DI buffer */
        extern int mscdex_dir_entry(u32 path, u32 buf);
        extern int rm_active;
        u32 buf = rm_active ? (cpu.r[R_ESI] & 0xffff) * 16 + (cpu.r[R_EDI] & 0xffff) : cpu.r[R_EDI];
        if (mscdex_dir_entry(ptr_es(cpu.r[R_EBX]), buf)) { SETAX(1); cpu.cf = 0; }
        else { SETAX(2); cpu.cf = 1; }
        break;
    }
    case 0x1510: {   /* send device driver request: ES:BX */
        extern void mscdex_request(u32 rq);
        mscdex_request(ptr_es(cpu.r[R_EBX]));
        cpu.cf = 0;
        break;
    }
    case 0x1600: SETAL(0); break;                       /* no Windows */
    case 0x1686: SETAX(0); break;
    case 0x4300: SETAL(0); break;
    default: rt_log("int2f: ax=%04x not provided\n", AX); break;
    }
}

/* ---- keyboard BIOS ---- */
static u16 kq[64];
static int kq_r, kq_w;

void bios_key_put(u16 k)
{
    int n = (kq_w + 1) & 63;
    if (n != kq_r) { kq[kq_w] = k; kq_w = n; }
}

int bios_key_avail(void) { input_pump(); return kq_r != kq_w; }

int bios_key_get(u16 *k, int wait)
{
    for (;;) {
        input_pump();
        if (kq_r != kq_w) { *k = kq[kq_r]; kq_r = (kq_r + 1) & 63; return 1; }
        if (!wait) return 0;
        irq_poll();
        video_present(0);
    }
}

void rt_int16(void)
{
    u16 k;
    switch (AH) {
    case 0x00: case 0x10: bios_key_get(&k, 1); SETAX(k); break;
    case 0x01: case 0x11:
        input_pump();
        if (kq_r != kq_w) { SETAX(kq[kq_r]); cpu.zf = 0; } else cpu.zf = 1;
        break;
    case 0x02: case 0x12: SETAL(0); break;
    default: break;
    }
}
