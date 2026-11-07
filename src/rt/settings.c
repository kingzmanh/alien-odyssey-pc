/* PC-side options: odyssey_pc.ini next to odyssey.exe (created with defaults
 * on first run).  These only change how the PC presents the game. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include "rt.h"

int opt_fullscreen = 1;      /* start in fullscreen (Alt+Enter toggles, remembered) */
int opt_filter = 2;          /* upscale: 0 nearest, 1 bilinear, 2 bicubic, 3 sharp bilinear */
int opt_sharpen = 2;         /* contrast-adaptive sharpening: 0 off, 1 low, 2 medium, 3 high */
int opt_fxaa = 0;            /* FXAA on the game's picture before upscaling */
int opt_aspect = 0;          /* 0 4:3, 1 stretch, 2 whole multiples of the game's pixels */
int opt_vsync = 1;

static char ini_path[700];

static const char default_ini[] =
    "; Alien Odyssey - PC options\r\n"
    "\r\n"
    "[video]\r\n"
    "; 1 = start in fullscreen, 0 = in a window (Alt+Enter switches and is remembered)\r\n"
    "Fullscreen=1\r\n"
    "; The game draws at 320x200; this is how that picture is brought up to your screen.\r\n"
    "; Filter: 0 nearest (blocky), 1 bilinear (soft), 2 bicubic (smooth), 3 sharp bilinear (crisp pixels)   [F9]\r\n"
    "Filter=2\r\n"
    "; Sharpen: contrast-adaptive sharpening after upscaling, 0 off, 1 low, 2 medium, 3 high   [F10]\r\n"
    "Sharpen=2\r\n"
    "; FXAA: smooth jagged edges of the game's picture before upscaling, 0/1   [F11]\r\n"
    "FXAA=0\r\n"
    "; Aspect: 0 4:3 like the original monitors, 1 stretch to the screen, 2 whole multiples (square pixels)   [F12]\r\n"
    "Aspect=0\r\n"
    "VSync=1\r\n";

static int ini_int(const char *sec, const char *key, int def, int lo, int hi)
{
    int v = GetPrivateProfileIntA(sec, key, def, ini_path);
    return v < lo ? lo : v > hi ? hi : v;
}

void settings_load(const char *exe_dir)
{
    snprintf(ini_path, sizeof ini_path, "%s\\odyssey_pc.ini", exe_dir);
    if (GetFileAttributesA(ini_path) == INVALID_FILE_ATTRIBUTES) {
        FILE *f = fopen(ini_path, "wb");
        if (f) { fwrite(default_ini, 1, sizeof default_ini - 1, f); fclose(f); }
    }
    opt_fullscreen = ini_int("video", "Fullscreen", 1, 0, 1);
    opt_filter = ini_int("video", "Filter", 2, 0, 3);
    opt_sharpen = ini_int("video", "Sharpen", 2, 0, 3);
    opt_fxaa = ini_int("video", "FXAA", 0, 0, 1);
    opt_aspect = ini_int("video", "Aspect", 0, 0, 2);
    opt_vsync = ini_int("video", "VSync", 1, 0, 1);
    rt_log("options: Fullscreen=%d Filter=%d Sharpen=%d FXAA=%d Aspect=%d VSync=%d\n",
           opt_fullscreen, opt_filter, opt_sharpen, opt_fxaa, opt_aspect, opt_vsync);
}

static void put(const char *key, int v)
{
    char s[16];
    snprintf(s, sizeof s, "%d", v);
    WritePrivateProfileStringA("video", key, s, ini_path);
}

void settings_save_fullscreen(void) { put("Fullscreen", opt_fullscreen); }

void settings_save_video(void)
{
    put("Filter", opt_filter);
    put("Sharpen", opt_sharpen);
    put("FXAA", opt_fxaa);
    put("Aspect", opt_aspect);
}
