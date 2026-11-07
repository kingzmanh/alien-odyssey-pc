#pragma once
#include "setup.h"

#define GAME_TITLE "Alien Odyssey"
#define UNINSTALL_KEY "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\AlienOdyssey_PCNative"

typedef struct {
    char cd[MAX_PATH * 2], dir[MAX_PATH * 2];
    int desktop, startmenu;
} InstallOpts;

int do_install(const InstallOpts *o, void (*cb)(u64 done, u64 total, void *user), void *user);
int do_uninstall(const char *dir);
int prebuilt_available(void);
int is_odyssey_cd(CdSrc *c);
int inject_icon_lbm(const char *exe, const u8 *lbm, size_t n);
void shortcut_paths(char *startmenu, char *desktop);
