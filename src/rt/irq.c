/* Interrupt services: the game's timer and keyboard handlers are called from
 * the PC side (driven by the Windows clock and window events), software
 * interrupts are routed to the native DOS/DPMI/BIOS services, and the few
 * remaining port accesses are logged. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include <stdlib.h>
#include <SDL2/SDL.h>
#include "rt.h"

volatile int rt_irq_pending;
static u32 vectors[256];
static u64 qpf, next_tick, tick_period;
static u32 pit_div = 65536;
int mouse_x, mouse_y, mouse_buttons;

void irq_set_vector(u8 n, u32 h)
{
    if (h < MAGIC_IRET || h >= MAGIC_IRET + 0x100)
        rt_log("irq: vector %02x -> %08x\n", n, h);
    vectors[n] = h;
}

u32 irq_get_vector(u8 n) { return vectors[n]; }

static Uint32 tick_cb(Uint32 iv, void *p) { (void)p; rt_irq_pending = 1; return iv; }

void timer_set_divisor(u32 div)
{
    if (!div) div = 65536;
    pit_div = div;
    if (!qpf) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); qpf = f.QuadPart; }
    tick_period = qpf * div / 1193182;
    rt_log("timer: %u Hz\n", 1193182 / div);
}

/* ---- keyboard ---- */
static u8 kbd_q[64];
static int kbd_r, kbd_w;
static u8 kbd_cur;

void kbd_push_scancode(u8 sc)
{
    int n = (kbd_w + 1) & 63;
    if (n != kbd_r) { kbd_q[kbd_w] = sc; kbd_w = n; }
}

u8 kbd_read_port(void) { return kbd_cur; }

extern void bios_key_put(u16 k);

/* scan code set 1 for SDL scancodes, with ASCII for the BIOS buffer */
static const struct { SDL_Scancode s; u8 sc; char lo, hi; } keymap[] = {
    {SDL_SCANCODE_ESCAPE, 0x01, 27, 27}, {SDL_SCANCODE_1, 0x02, '1', '!'}, {SDL_SCANCODE_2, 0x03, '2', '@'},
    {SDL_SCANCODE_3, 0x04, '3', '#'}, {SDL_SCANCODE_4, 0x05, '4', '$'}, {SDL_SCANCODE_5, 0x06, '5', '%'},
    {SDL_SCANCODE_6, 0x07, '6', '^'}, {SDL_SCANCODE_7, 0x08, '7', '&'}, {SDL_SCANCODE_8, 0x09, '8', '*'},
    {SDL_SCANCODE_9, 0x0a, '9', '('}, {SDL_SCANCODE_0, 0x0b, '0', ')'}, {SDL_SCANCODE_MINUS, 0x0c, '-', '_'},
    {SDL_SCANCODE_EQUALS, 0x0d, '=', '+'}, {SDL_SCANCODE_BACKSPACE, 0x0e, 8, 8}, {SDL_SCANCODE_TAB, 0x0f, 9, 9},
    {SDL_SCANCODE_Q, 0x10, 'q', 'Q'}, {SDL_SCANCODE_W, 0x11, 'w', 'W'}, {SDL_SCANCODE_E, 0x12, 'e', 'E'},
    {SDL_SCANCODE_R, 0x13, 'r', 'R'}, {SDL_SCANCODE_T, 0x14, 't', 'T'}, {SDL_SCANCODE_Y, 0x15, 'y', 'Y'},
    {SDL_SCANCODE_U, 0x16, 'u', 'U'}, {SDL_SCANCODE_I, 0x17, 'i', 'I'}, {SDL_SCANCODE_O, 0x18, 'o', 'O'},
    {SDL_SCANCODE_P, 0x19, 'p', 'P'}, {SDL_SCANCODE_LEFTBRACKET, 0x1a, '[', '{'}, {SDL_SCANCODE_RIGHTBRACKET, 0x1b, ']', '}'},
    {SDL_SCANCODE_RETURN, 0x1c, 13, 13}, {SDL_SCANCODE_LCTRL, 0x1d, 0, 0}, {SDL_SCANCODE_A, 0x1e, 'a', 'A'},
    {SDL_SCANCODE_S, 0x1f, 's', 'S'}, {SDL_SCANCODE_D, 0x20, 'd', 'D'}, {SDL_SCANCODE_F, 0x21, 'f', 'F'},
    {SDL_SCANCODE_G, 0x22, 'g', 'G'}, {SDL_SCANCODE_H, 0x23, 'h', 'H'}, {SDL_SCANCODE_J, 0x24, 'j', 'J'},
    {SDL_SCANCODE_K, 0x25, 'k', 'K'}, {SDL_SCANCODE_L, 0x26, 'l', 'L'}, {SDL_SCANCODE_SEMICOLON, 0x27, ';', ':'},
    {SDL_SCANCODE_APOSTROPHE, 0x28, '\'', '"'}, {SDL_SCANCODE_GRAVE, 0x29, '`', '~'}, {SDL_SCANCODE_LSHIFT, 0x2a, 0, 0},
    {SDL_SCANCODE_BACKSLASH, 0x2b, '\\', '|'}, {SDL_SCANCODE_Z, 0x2c, 'z', 'Z'}, {SDL_SCANCODE_X, 0x2d, 'x', 'X'},
    {SDL_SCANCODE_C, 0x2e, 'c', 'C'}, {SDL_SCANCODE_V, 0x2f, 'v', 'V'}, {SDL_SCANCODE_B, 0x30, 'b', 'B'},
    {SDL_SCANCODE_N, 0x31, 'n', 'N'}, {SDL_SCANCODE_M, 0x32, 'm', 'M'}, {SDL_SCANCODE_COMMA, 0x33, ',', '<'},
    {SDL_SCANCODE_PERIOD, 0x34, '.', '>'}, {SDL_SCANCODE_SLASH, 0x35, '/', '?'}, {SDL_SCANCODE_RSHIFT, 0x36, 0, 0},
    {SDL_SCANCODE_KP_MULTIPLY, 0x37, '*', '*'}, {SDL_SCANCODE_LALT, 0x38, 0, 0}, {SDL_SCANCODE_SPACE, 0x39, ' ', ' '},
    {SDL_SCANCODE_CAPSLOCK, 0x3a, 0, 0}, {SDL_SCANCODE_F1, 0x3b, 0, 0}, {SDL_SCANCODE_F2, 0x3c, 0, 0},
    {SDL_SCANCODE_F3, 0x3d, 0, 0}, {SDL_SCANCODE_F4, 0x3e, 0, 0}, {SDL_SCANCODE_F5, 0x3f, 0, 0},
    {SDL_SCANCODE_F6, 0x40, 0, 0}, {SDL_SCANCODE_F7, 0x41, 0, 0}, {SDL_SCANCODE_F8, 0x42, 0, 0},
    {SDL_SCANCODE_F9, 0x43, 0, 0}, {SDL_SCANCODE_F10, 0x44, 0, 0}, {SDL_SCANCODE_HOME, 0x47, 0, 0},
    {SDL_SCANCODE_UP, 0x48, 0, 0}, {SDL_SCANCODE_PAGEUP, 0x49, 0, 0}, {SDL_SCANCODE_LEFT, 0x4b, 0, 0},
    {SDL_SCANCODE_RIGHT, 0x4d, 0, 0}, {SDL_SCANCODE_END, 0x4f, 0, 0}, {SDL_SCANCODE_DOWN, 0x50, 0, 0},
    {SDL_SCANCODE_PAGEDOWN, 0x51, 0, 0}, {SDL_SCANCODE_INSERT, 0x52, 0, 0}, {SDL_SCANCODE_DELETE, 0x53, 0, 0},
    {SDL_SCANCODE_F11, 0x57, 0, 0}, {SDL_SCANCODE_F12, 0x58, 0, 0}, {SDL_SCANCODE_KP_ENTER, 0x1c, 13, 13},
    {SDL_SCANCODE_RCTRL, 0x1d, 0, 0}, {SDL_SCANCODE_RALT, 0x38, 0, 0},
};

extern void mouse_motion(int dx, int dy);

int mouse_buttons_reported(void) { return mouse_buttons; }
static void mouse_press_time(int b) { (void)b; }
static int quit_requested;

void input_pump(void)
{
    SDL_Event ev;
    static u64 last;
    u64 now = SDL_GetTicks64();
    if (now == last) return;
    last = now;
    {   /* AO_CLICK=x,y,ms[;x,y,ms...]: scripted left clicks for test runs */
        static char script[256];
        static int init, idx;
        if (!init) { const char *e = getenv("AO_CLICK"); if (e) snprintf(script, sizeof script, "%s", e); init = 1; }
        const char *p = script;
        for (int k = 0; k < idx / 2 && p; k++) { p = strchr(p, ';'); if (p) p++; }
        int cx, cy, cms;
        if (p && *p && sscanf(p, "%d,%d,%d", &cx, &cy, &cms) == 3) {
            if (!(idx & 1) && now >= (u64)cms) { mouse_x = cx; mouse_y = cy; mouse_buttons |= 1; mouse_press_time(1); idx++; rt_log("test: click %d,%d\n", cx, cy); }
            else if ((idx & 1) && now >= (u64)cms + 300) { mouse_buttons &= ~1; idx++; }
        }
    }
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            rt_log("window closed\n");
            SDL_Quit();
            exit(0);
        case SDL_KEYDOWN: case SDL_KEYUP: {
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_RETURN && (ev.key.keysym.mod & KMOD_ALT)) {
                SDL_Window *w = SDL_GetWindowFromID(ev.key.windowID);
                u32 fl = SDL_GetWindowFlags(w);
                extern int opt_fullscreen;
                extern void settings_save_fullscreen(void);
                opt_fullscreen = !(fl & SDL_WINDOW_FULLSCREEN_DESKTOP);
                SDL_SetWindowFullscreen(w, opt_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                settings_save_fullscreen();
                break;
            }
            for (size_t i = 0; i < sizeof keymap / sizeof keymap[0]; i++)
                if (keymap[i].s == ev.key.keysym.scancode) {
                    u8 sc = keymap[i].sc;
                    if (ev.key.repeat && ev.type == SDL_KEYUP) break;
                    kbd_push_scancode(ev.type == SDL_KEYDOWN ? sc : (u8)(sc | 0x80));
                    if (ev.type == SDL_KEYDOWN) {
                        int sh = (ev.key.keysym.mod & KMOD_SHIFT) != 0;
                        bios_key_put((u16)(sc << 8 | (u8)(sh ? keymap[i].hi : keymap[i].lo)));
                    }
                    rt_irq_pending = 1;
                    break;
                }
            break;
        }
        case SDL_MOUSEMOTION: {
            /* window -> logical coordinates are already applied by SDL_RenderSetLogicalSize */
            mouse_x = ev.motion.x; mouse_y = ev.motion.y;
            if (video_h && video_w == 320) mouse_y = mouse_y * 200 / 240;
            if (mouse_x < 0) mouse_x = 0;
            if (mouse_y < 0) mouse_y = 0;
            if (video_w && mouse_x >= video_w) mouse_x = video_w - 1;
            if (video_h && mouse_y >= video_h) mouse_y = video_h - 1;
            mouse_motion(ev.motion.xrel, ev.motion.yrel);
            break;
        }
        case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
            int b = ev.button.button == SDL_BUTTON_LEFT ? 1 : ev.button.button == SDL_BUTTON_RIGHT ? 2 : ev.button.button == SDL_BUTTON_MIDDLE ? 4 : 0;
            if (ev.type == SDL_MOUSEBUTTONDOWN) { mouse_buttons |= b; mouse_press_time(b); } else mouse_buttons &= ~b;
            break;
        }
        default: break;
        }
    }
    (void)quit_requested;
}

/* call a protected-mode interrupt handler the game installed */
static void call_handler(u32 h)
{
    CPU save = cpu;
    cpu.r[R_ESP] -= 12;
    WR32(cpu.r[R_ESP] + 8, 0x202);
    WR32(cpu.r[R_ESP] + 4, SEL_CODE);
    WR32(cpu.r[R_ESP], MAGIC_IRET + 0xfd);
    void (*fn)(void) = rt_lookup(h);
    if (!fn) rt_fatal("interrupt handler %08x has no code", h);
    fn();
    if (cpu.r[R_ESP] != save.r[R_ESP]) rt_log("irq: handler %08x left esp off by %d\n", h, (int)(cpu.r[R_ESP] - save.r[R_ESP]));
    memcpy(cpu.r, save.r, sizeof save.r);
    cpu.cf = save.cf; cpu.zf = save.zf; cpu.sf = save.sf; cpu.of = save.of; cpu.pf = save.pf; cpu.af = save.af;
    cpu.df = save.df;
    for (int i = 0; i < 6; i++) { cpu.sreg[i] = save.sreg[i]; cpu.sbase[i] = save.sbase[i]; }
}

void irq_call_handler(u32 h) { call_handler(h); }

/* what the original BIOS handlers did, for vectors the game chains to */
void irq_magic(u32 n)
{
    if (n == 8) {
        WR32(0x46c, RD32(0x46c) + 1);
        if (vectors[0x1c] < MAGIC_IRET || vectors[0x1c] >= MAGIC_IRET + 0x100) call_handler(vectors[0x1c]);
    } else if (n == 9) {
        /* BIOS buffer already filled from the window events */
    }
}

static int in_irq;
void irq_poll(void)
{
    rt_irq_pending = 0;
    if (in_irq) return;
    in_irq = 1;
    if (!tick_period) timer_set_divisor(65536);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    u64 now = c.QuadPart;
    if (!next_tick) next_tick = now + tick_period;
    int n = 0;
    while ((s64)(now - next_tick) >= 0 && n < 8) {
        next_tick += tick_period;
        n++;
        u32 h = vectors[8];
        if (h >= MAGIC_IRET && h < MAGIC_IRET + 0x100) irq_magic(8);
        else call_handler(h);
    }
    if ((s64)(now - next_tick) > (s64)tick_period * 8) next_tick = now + tick_period;
    {   /* AO_WATCH_AT=<ms>: arm the AO_WATCH write watch at that time */
        static int armed;
        const char *e = getenv("AO_WATCH_AT");
        if (!armed && e && SDL_GetTicks64() >= (u64)atoi(e)) { extern void rt_watch_arm(void); armed = 1; rt_watch_arm(); }
    }
    {   /* AO_DUMP=<ms>: log where the game is, every <ms> */
        static u64 next_dump;
        static int dump_ms = -1;
        if (dump_ms < 0) { const char *e = getenv("AO_DUMP"); dump_ms = e ? atoi(e) : 0; }
        if (dump_ms > 0 && SDL_GetTicks64() >= next_dump) {
            extern void rt_backtrace(const char *);
            next_dump = SDL_GetTicks64() + dump_ms;
            rt_log("dump: eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x\n",
                   cpu.r[0], cpu.r[3], cpu.r[1], cpu.r[2], cpu.r[6], cpu.r[7], cpu.r[5], cpu.r[4]);
            rt_backtrace("dump");
            {   /* AO_PEEK=addr,addr,...: guest dwords to show with each dump */
                const char *pk = getenv("AO_PEEK");
                while (pk && *pk) {
                    char *end;
                    u32 a = (u32)strtoul(pk, &end, 16);
                    rt_log("  [%08x]=%08x", a, RD32(a));
                    pk = *end ? end + 1 : end;
                }
                rt_log("\n");
            }
        }
    }
    input_pump();
    while (kbd_r != kbd_w) {
        kbd_cur = kbd_q[kbd_r];
        kbd_r = (kbd_r + 1) & 63;
        u32 h = vectors[9];
        if (h < MAGIC_IRET || h >= MAGIC_IRET + 0x100) call_handler(h);
    }
    video_present(0);
    { extern void video_status_tick(void); video_status_tick(); }
    in_irq = 0;
}

/* ---- software interrupts ---- */
void rt_int(u32 n, u32 at)
{
    static int started;
    if (!started) { started = 1; SDL_AddTimer(1, tick_cb, NULL); }
    u32 h = vectors[n];
    if (n != 0x21 && n != 0x31 && n != 0x10 && n != 0x33 && n != 0x2f && n != 0x16 &&
        h && (h < MAGIC_IRET || h >= MAGIC_IRET + 0x100)) {
        call_handler(h);
        return;
    }
    switch (n) {
    case 0x21: rt_int21(); break;
    case 0x31: rt_int31(); break;
    case 0x10: rt_int10(); break;
    case 0x33: rt_int33(); break;
    case 0x2f: rt_int2f(); break;
    case 0x16: rt_int16(); break;
    case 0x1a:
        if (((cpu.r[R_EAX] >> 8) & 0xff) == 0) {
            u32 t = RD32(0x46c);
            cpu.r[R_ECX] = (cpu.r[R_ECX] & 0xffff0000u) | (t >> 16);
            cpu.r[R_EDX] = (cpu.r[R_EDX] & 0xffff0000u) | (t & 0xffff);
            cpu.r[R_EAX] &= 0xffffff00u;
        }
        break;
    case 0x4b: cpu.cf = 1; break; /* no virtual DMA services */
    default: {
        static int logged[256];
        if (!logged[n]++) rt_log("int %02x (ax=%04x) at %08x not provided\n", n, cpu.r[R_EAX] & 0xffff, at);
        if (n == 8 || n == 0x1c) irq_magic(n);
    }
    }
}

/* ---- port I/O that still reaches the runtime ---- */
static int dac_w_idx, dac_r_idx, dac_comp;
static u8 dac_tmp[3];
static int pit_latch_state;
static u32 pit_new;
static u8 retrace;

static void port_note(u32 port, u32 at, int out)
{
    static u32 seen[64];
    static int nseen;
    for (int i = 0; i < nseen; i++) if (seen[i] == (port << 1 | out)) return;
    if (nseen < 64) seen[nseen++] = port << 1 | out;
    rt_log("port %s %04x first used at %08x\n", out ? "out" : "in", port, at);
}

u32 rt_in8(u32 port, u32 at)
{
    rt_poll();
    port &= 0xffff;
    switch (port) {
    case 0x60: return kbd_cur;
    case 0x61: return 0x20;
    case 0x64: return 0x1c;
    case 0x3da: retrace ^= 9; return retrace;
    case 0x3c9: {
        u8 v = vga_pal[dac_r_idx * 3 + dac_comp];
        if (++dac_comp == 3) { dac_comp = 0; dac_r_idx = (dac_r_idx + 1) & 255; }
        return v;
    }
    default: port_note(port, at, 0); return 0xff;
    }
}

u32 rt_in16(u32 port, u32 at) { return rt_in8(port, at) | (rt_in8(port + 1, at) << 8); }
u32 rt_in32(u32 port, u32 at) { port_note(port, at, 0); return 0xffffffff; }

void rt_out8(u32 port, u32 v, u32 at)
{
    port &= 0xffff;
    v &= 0xff;
    switch (port) {
    case 0x20: case 0xa0: case 0x21: case 0xa1: return;
    case 0x43: pit_latch_state = (v & 0x30) == 0x30 ? 1 : 0; return;
    case 0x40:
        if (pit_latch_state == 1) { pit_new = v; pit_latch_state = 2; }
        else if (pit_latch_state == 2) { pit_new |= v << 8; pit_latch_state = 1; timer_set_divisor(pit_new); }
        return;
    case 0x3c8: dac_w_idx = v; dac_comp = 0; return;
    case 0x3c7: dac_r_idx = v; dac_comp = 0; return;
    case 0x3c9:
        dac_tmp[dac_comp++] = (u8)v;
        if (dac_comp == 3) { video_set_pal(dac_w_idx, dac_tmp[0], dac_tmp[1], dac_tmp[2]); dac_comp = 0; dac_w_idx = (dac_w_idx + 1) & 255; }
        return;
    default: port_note(port, at, 1); return;
    }
}

void rt_out16(u32 port, u32 v, u32 at) { rt_out8(port, v & 0xff, at); rt_out8(port + 1, (v >> 8) & 0xff, at); }
void rt_out32(u32 port, u32 v, u32 at) { (void)v; port_note(port, at, 1); }

/* a new program starts: no handlers, default timer rate, empty queues */
void irq_reset(void)
{
    for (int i = 0; i < 256; i++) vectors[i] = MAGIC_IRET + i;
    pit_div = 65536;
    tick_period = 0;
    next_tick = 0;
    kbd_r = kbd_w = 0;
    kbd_cur = 0;
    in_irq = 0;
}
