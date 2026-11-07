/* PC-side options: odyssey_pc.ini next to odyssey.exe (created with defaults
 * on first run).  These only change how the PC presents the game; the
 * defaults keep the original behaviour. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include "rt.h"

int opt_fullscreen = 1;      /* start in fullscreen (Alt+Enter toggles, remembered) */

static char ini_path[700];

static const char default_ini[] =
    "; Alien Odyssey - PC options\r\n"
    "\r\n"
    "[video]\r\n"
    "; 1 = start in fullscreen, 0 = in a window (Alt+Enter switches and is remembered)\r\n"
    "Fullscreen=1\r\n";

void settings_load(const char *exe_dir)
{
    snprintf(ini_path, sizeof ini_path, "%s\\odyssey_pc.ini", exe_dir);
    if (GetFileAttributesA(ini_path) == INVALID_FILE_ATTRIBUTES) {
        FILE *f = fopen(ini_path, "wb");
        if (f) { fwrite(default_ini, 1, sizeof default_ini - 1, f); fclose(f); }
    }
    opt_fullscreen = GetPrivateProfileIntA("video", "Fullscreen", 1, ini_path) != 0;
    rt_log("options: Fullscreen=%d\n", opt_fullscreen);
}

void settings_save_fullscreen(void)
{
    WritePrivateProfileStringA("video", "Fullscreen", opt_fullscreen ? "1" : "0", ini_path);
}
