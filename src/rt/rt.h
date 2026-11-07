/* Internal interface of the PC runtime that replaces DOS, the DOS extender,
 * the BIOS and the hardware under the recompiled game code. */
#pragma once
#include <stdio.h>
#include <stdarg.h>
#include "cpu.h"

/* guest memory layout */
#define GUEST_RESERVE   0x100000000ull   /* whole 32-bit space reserved */
#define GUEST_COMMIT    0x10000000u      /* first 256 MB committed */
#define DOSMEM_BASE     0x00010000u      /* conventional memory pool */
#define DOSMEM_END      0x0009f000u
#define VGA_WINDOW      0x000a0000u
#define HEAP_BASE       0x01000000u      /* DPMI / sbrk memory */
#define HEAP_END        0x0f000000u
#define MAGIC_IRET      0xfff00000u      /* fake "previous handler" addresses */

/* selectors we hand out */
#define SEL_CODE 0x0010
#define SEL_DATA 0x0018
#define SEL_PSP  0x0024
#define SEL_ENV  0x002c

void rt_log(const char *fmt, ...);
void rt_fatal(const char *fmt, ...);
extern FILE *rt_logf;

/* loader */
typedef struct {
    u32 entry, stack;
    u32 obj_base[8], obj_size[8], nobj;
} LoadInfo;
int le_load(const char *path, LoadInfo *li);

/* function table */
void (*rt_lookup(u32 addr))(void);
void rt_call_guest(u32 addr);           /* call guest function with a fake return address */

/* dpmi / selectors */
void dpmi_init(void);
void rt_int31(void);
u32 sel_base(u16 sel);
u16 sel_alloc(u32 base, u32 limit);
u32 dosmem_alloc(u32 paras, u16 *seg, u16 *sel);
u32 heap_alloc(u32 size);
void real_int(u8 n, u32 regs_addr);

/* dos */
void dos_init(const char *root);
void dos_reset(void);                   /* new program: close its files */
void rt_exit(int code);                 /* program exit -> launcher */
extern const char *rt_module;           /* program running now ("INTRO" ...) */
void sound_reset(void);
void settings_load(const char *exe_dir);
void rt_int21(void);
extern char rt_root[512];

/* bios */
void rt_int10(void);
void rt_int33(void);
void rt_int2f(void);
void rt_int16(void);

/* video */
void video_init(void);
void video_set_mode(int w, int h, int bpp);
void video_bank(u32 bank);
void video_set_pal(int idx, int r, int g, int b); /* 6-bit */
void video_present(int force);
extern int video_w, video_h, video_bank_cur;
extern u8 vga_pal[768];

/* input */
void input_pump(void);
extern int mouse_x, mouse_y, mouse_buttons;

/* interrupts / timer */
extern volatile int rt_irq_pending;
void irq_set_vector(u8 n, u32 handler);
u32 irq_get_vector(u8 n);
void irq_poll(void);
void timer_set_divisor(u32 div);
void kbd_push_scancode(u8 sc);
u8 kbd_read_port(void);

static inline void rt_poll(void) { if (rt_irq_pending) irq_poll(); }
