/* PC-side options: odyssey_pc.ini next to odyssey.exe (created with defaults
 * on first run).  These only change how the PC presents the game. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include "rt.h"

int opt_fullscreen = 1;      /* start in fullscreen (Alt+Enter toggles, remembered) */
int opt_filter = 4;          /* upscale: 0 nearest, 1 bilinear, 2 bicubic, 3 sharp bilinear, 4 xBR */
int opt_sharpen = 1;         /* contrast-adaptive sharpening: 0 off, 1 low, 2 medium, 3 high */
int opt_fxaa = 0;            /* FXAA on the game's picture before upscaling */
int opt_aspect = 0;          /* 0 4:3, 1 stretch, 2 whole multiples of the game's pixels */
int opt_vsync = 1;
int opt_scale = 4;           /* internal resolution of the 3D game's scenes (x 320x200) */
int opt_showfps = 0;
int opt_mouse_sens = 100;     /* % of the Windows mouse counts given to the game */
int opt_mouse_invert = 0;

static char ini_path[700];

static const char default_ini[] =
    "; Alien Odyssey - PC options\r\n"
    "\r\n"
    "[video]\r\n"
    "; 1 = start in fullscreen, 0 = in a window (Alt+Enter switches and is remembered)\r\n"
    "Fullscreen=1\r\n"
    "; The game draws at 320x200; this is how that picture is brought up to your screen.\r\n"
    "; Filter: 4 xBR (smooth edges, sharp), 0 nearest (blocky), 1 bilinear (soft), 2 bicubic (smooth), 3 sharp bilinear\r\n"
    "Filter=4\r\n"
    "; Sharpen: contrast-adaptive sharpening after upscaling, 0 off, 1 low, 2 medium, 3 high\r\n"
    "Sharpen=1\r\n"
    "; FXAA: smooth jagged edges of the game's picture before upscaling, 0/1\r\n"
    "FXAA=0\r\n"
    "; Aspect: 0 4:3 like the original monitors, 1 stretch to the screen, 2 whole multiples (square pixels)\r\n"
    "Aspect=0\r\n"
    "; RenderScale: the 3D game renders its scenes at this multiple of 320x200 (1..6)\r\n"
    "RenderScale=4\r\n"
    "ShowFPS=0\r\n"
    "SettingsVersion=2\r\n";

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
    opt_filter = ini_int("video", "Filter", 4, 0, 4);
    opt_sharpen = ini_int("video", "Sharpen", 1, 0, 3);
    opt_fxaa = ini_int("video", "FXAA", 0, 0, 1);
    opt_aspect = ini_int("video", "Aspect", 0, 0, 2);
    opt_vsync = ini_int("video", "VSync", 1, 0, 1);
    opt_scale = ini_int("video", "RenderScale", 4, 1, 6);
    opt_showfps = ini_int("video", "ShowFPS", 0, 0, 1);
    if (ini_int("video", "SettingsVersion", 1, 0, 99) < 2) {
        /* v2: xBR became the upscaler; FXAA before it only smears the edges
           xBR needs, and strong sharpening adds halos */
        opt_filter = 4; opt_sharpen = 1; opt_fxaa = 0;
        WritePrivateProfileStringA("video", "Filter", "4", ini_path);
        WritePrivateProfileStringA("video", "Sharpen", "1", ini_path);
        WritePrivateProfileStringA("video", "FXAA", "0", ini_path);
        WritePrivateProfileStringA("video", "SettingsVersion", "2", ini_path);
    }
    opt_mouse_sens = ini_int("input", "MouseSensitivity", 100, 10, 800);
    opt_mouse_invert = ini_int("input", "InvertMouseY", 0, 0, 1);
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
    put("RenderScale", opt_scale);
    put("ShowFPS", opt_showfps);
    {
        char v[16];
        snprintf(v, sizeof v, "%d", opt_mouse_sens);
        WritePrivateProfileStringA("input", "MouseSensitivity", v, ini_path);
        WritePrivateProfileStringA("input", "InvertMouseY", opt_mouse_invert ? "1" : "0", ini_path);
    }
}
