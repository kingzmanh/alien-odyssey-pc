/* The install itself: copies the user's Alien Odyssey CD (it is then always
 * "in the drive"), lays out what the original DOS installer put on the hard
 * disk, and puts the PC executable in place with an icon made from the
 * game's own emblem picture on the CD. */
#include "setup.h"
#include <stdlib.h>
#include <string.h>
#include <shlobj.h>
#include <objbase.h>
#include <shobjidl.h>
#include "install.h"

static int fail(const char *msg) { setup_log("ERROR: %s", msg); return 0; }

/* files the DOS installer copied to C:\ODYSSEY besides the programs */
static const char *hd_sound[] = {"ALWAYS", "ARMOURY", "COMMBUNK", "DISK", "INCAPACI", "LIZARD", "SOLCENBL",
                                 "STAFFESC", "TERMINAL", "WEAPCOL", "DAKMASTE"};

int is_odyssey_cd(CdSrc *c)
{
    return c && cd_exists(c, "3DGAME.EXE") && cd_exists(c, "MCP.EXE") && cd_exists(c, "VILLAGE1.EXE") &&
           cd_exists(c, "BRENDER\\INTRO.DAT");
}

/* ---------- game icon: the emblem from INTRO\DATA\A.LBM ---------- */
static u32 be32(const u8 *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* Deluxe Paint PBM (chunky) -> RGBA; black is transparent */
static u32 *decode_lbm(const u8 *d, size_t n, int *w, int *h)
{
    if (n < 12 || memcmp(d, "FORM", 4) || memcmp(d + 8, "PBM ", 4)) return NULL;
    size_t p = 12, end = 8 + be32(d + 4);
    if (end > n) end = n;
    const u8 *pal = NULL, *body = NULL;
    size_t blen = 0;
    int comp = 0;
    *w = *h = 0;
    while (p + 8 <= end) {
        u32 len = be32(d + p + 4);
        const u8 *c = d + p + 8;
        if (!memcmp(d + p, "BMHD", 4)) { *w = c[0] << 8 | c[1]; *h = c[2] << 8 | c[3]; comp = c[10]; }
        else if (!memcmp(d + p, "CMAP", 4)) pal = c;
        else if (!memcmp(d + p, "BODY", 4)) { body = c; blen = len; }
        p += 8 + len + (len & 1);
    }
    if (!*w || !*h || !pal || !body) return NULL;
    int row = *w + (*w & 1);
    u8 *pix = calloc(row, *h);
    if (comp == 1) {
        size_t i = 0, o = 0, tot = (size_t)row * *h;
        while (o < tot && i < blen) {
            u8 b = body[i++];
            if (b < 128) { for (int k = 0; k <= b && o < tot && i < blen; k++) pix[o++] = body[i++]; }
            else if (b > 128) { u8 v = body[i++]; for (int k = 0; k < 257 - b && o < tot; k++) pix[o++] = v; }
        }
    } else memcpy(pix, body, blen < (size_t)row * *h ? blen : (size_t)row * *h);
    u32 *rgba = calloc((size_t)*w * *h, 4);
    for (int y = 0; y < *h; y++)
        for (int x = 0; x < *w; x++) {
            const u8 *c = pal + pix[y * row + x] * 3;
            int lum = c[0] + c[1] + c[2];
            rgba[y * *w + x] = lum < 60 ? 0 : (0xffu << 24) | (c[0] << 16) | (c[1] << 8) | c[2];
        }
    free(pix);
    return rgba;
}

static u8 *icon_image(const u32 *sq, int side, int size, u32 *len)
{
    u32 mask_row = ((size + 31) / 32) * 4;
    *len = 40 + size * size * 4 + mask_row * size;
    u8 *b = calloc(1, *len);
    u32 *px = (u32 *)(b + 40);
    b[0] = 40;
    *(int *)(b + 4) = size;
    *(int *)(b + 8) = size * 2;
    b[12] = 1;
    b[14] = 32;
    /* box filter down from the large source */
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            int x0 = x * side / size, x1 = (x + 1) * side / size, y0 = y * side / size, y1 = (y + 1) * side / size;
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            u32 a = 0, r = 0, g = 0, bl = 0, n = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    u32 v = sq[yy * side + xx], al = v >> 24;
                    a += al; r += ((v >> 16) & 255) * al; g += ((v >> 8) & 255) * al; bl += (v & 255) * al; n++;
                }
            u32 out = 0;
            if (a) out = ((a / n) << 24) | ((r / a) << 16) | ((g / a) << 8) | (bl / a);
            px[(size - 1 - y) * size + x] = out;
        }
    return b;
}

int inject_icon_lbm(const char *exe, const u8 *lbm, size_t n)
{
    int w, h;
    u32 *src = decode_lbm(lbm, n, &w, &h);
    if (!src) return fail("game emblem picture not understood");
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (src[y * w + x] >> 24) { if (x < x0) x0 = x; if (y < y0) y0 = y; if (x > x1) x1 = x; if (y > y1) y1 = y; }
    if (x1 < 0) { free(src); return fail("game emblem picture is empty"); }
    int cw = x1 - x0 + 1, ch = y1 - y0 + 1, side = cw > ch ? cw : ch;
    side += side / 16;
    u32 *sq = calloc((size_t)side * side, 4);
    for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++)
            sq[(y + (side - ch) / 2) * side + x + (side - cw) / 2] = src[(y0 + y) * w + x0 + x];
    free(src);
    static const int sizes[] = {16, 24, 32, 48, 64, 128, 256};
    const int ns = 7;
    HANDLE u = BeginUpdateResourceA(exe, FALSE);
    if (!u) { free(sq); return fail("cannot update the game executable's icon"); }
    u8 grp[6 + 7 * 14];
    memset(grp, 0, sizeof grp);
    grp[2] = 1;
    grp[4] = (u8)ns;
    for (int i = 0; i < ns; i++) {
        u32 len;
        u8 *img = icon_image(sq, side, sizes[i], &len);
        UpdateResourceA(u, (LPCSTR)RT_ICON, MAKEINTRESOURCEA(1 + i), MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL), img, len);
        free(img);
        u8 *e = grp + 6 + i * 14;
        e[0] = sizes[i] == 256 ? 0 : (u8)sizes[i];
        e[1] = e[0];
        e[4] = 1;
        e[6] = 32;
        memcpy(e + 8, &len, 4);
        u16 id = (u16)(1 + i);
        memcpy(e + 12, &id, 2);
    }
    UpdateResourceA(u, (LPCSTR)RT_GROUP_ICON, MAKEINTRESOURCEA(1), MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL), grp, sizeof grp);
    free(sq);
    if (!EndUpdateResourceA(u, FALSE)) return fail("cannot write the game executable's icon");
    return 1;
}

/* ---------- shortcuts / uninstall entry ---------- */
static int make_link(const char *lnk, const char *target, const char *dir, const char *desc)
{
    IShellLinkA *sl = NULL;
    IPersistFile *pf = NULL;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkA, (void **)&sl))) return 0;
    sl->lpVtbl->SetPath(sl, target);
    sl->lpVtbl->SetWorkingDirectory(sl, dir);
    sl->lpVtbl->SetDescription(sl, desc);
    sl->lpVtbl->SetIconLocation(sl, target, 0);
    int ok = 0;
    if (SUCCEEDED(sl->lpVtbl->QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) {
        WCHAR w[MAX_PATH * 2];
        MultiByteToWideChar(CP_ACP, 0, lnk, -1, w, MAX_PATH * 2);
        ok = SUCCEEDED(pf->lpVtbl->Save(pf, w, TRUE));
        pf->lpVtbl->Release(pf);
    }
    sl->lpVtbl->Release(sl);
    return ok;
}

void shortcut_paths(char *startmenu, char *desktop)
{
    char p[MAX_PATH];
    SHGetFolderPathA(NULL, CSIDL_PROGRAMS, NULL, 0, p);
    snprintf(startmenu, MAX_PATH * 2, "%s\\%s.lnk", p, GAME_TITLE);
    SHGetFolderPathA(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, p);
    snprintf(desktop, MAX_PATH * 2, "%s\\%s.lnk", p, GAME_TITLE);
}

static void register_uninstall(const char *dir, u64 bytes)
{
    HKEY k;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, UNINSTALL_KEY, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL)) return;
    char s[MAX_PATH * 3];
    RegSetValueExA(k, "DisplayName", 0, REG_SZ, (const BYTE *)GAME_TITLE " (PC Native)", (DWORD)strlen(GAME_TITLE " (PC Native)") + 1);
    snprintf(s, sizeof s, "\"%s\\uninstall.exe\" /uninstall", dir);
    RegSetValueExA(k, "UninstallString", 0, REG_SZ, (const BYTE *)s, (DWORD)strlen(s) + 1);
    snprintf(s, sizeof s, "%s\\odyssey.exe,0", dir);
    RegSetValueExA(k, "DisplayIcon", 0, REG_SZ, (const BYTE *)s, (DWORD)strlen(s) + 1);
    RegSetValueExA(k, "InstallLocation", 0, REG_SZ, (const BYTE *)dir, (DWORD)strlen(dir) + 1);
    const char *pub = "PC Native port (fan project)";
    RegSetValueExA(k, "Publisher", 0, REG_SZ, (const BYTE *)pub, (DWORD)strlen(pub) + 1);
    DWORD kb = (DWORD)(bytes / 1024), one = 1;
    RegSetValueExA(k, "EstimatedSize", 0, REG_DWORD, (const BYTE *)&kb, 4);
    RegSetValueExA(k, "NoModify", 0, REG_DWORD, (const BYTE *)&one, 4);
    RegSetValueExA(k, "NoRepair", 0, REG_DWORD, (const BYTE *)&one, 4);
    RegCloseKey(k);
}

/* ---------- prebuilt game ---------- */
static int extract_prebuilt(const char *dst)
{
    HRSRC r = FindResourceA(NULL, "GAMEEXE", (LPCSTR)RT_RCDATA);
    if (!r) return fail("this setup does not contain the game executable");
    HGLOBAL g = LoadResource(NULL, r);
    DWORD n = SizeofResource(NULL, r);
    return write_file(dst, LockResource(g), n);
}

int prebuilt_available(void) { return FindResourceA(NULL, "GAMEEXE", (LPCSTR)RT_RCDATA) != NULL; }

static int copy_dir(const char *src, const char *dst, const char *pattern)
{
    char pat[MAX_PATH * 2];
    snprintf(pat, sizeof pat, "%s\\%s", src, pattern);
    make_dirs(dst);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char s[MAX_PATH * 3], d[MAX_PATH * 3];
        snprintf(s, sizeof s, "%s\\%s", src, fd.cFileName);
        snprintf(d, sizeof d, "%s\\%s", dst, fd.cFileName);
        if (copy_file(s, d, NULL)) n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

/* ---------- the whole install ---------- */
int do_install(const InstallOpts *o, void (*cb)(u64, u64, void *), void *user)
{
    char p[MAX_PATH * 2], q[MAX_PATH * 2], game[MAX_PATH * 2], hd[MAX_PATH * 2], cd[MAX_PATH * 2];
    setup_log("Install to %s", o->dir);
    if (!prebuilt_available()) return fail("this setup does not contain the game executable");
    CdSrc *c = cd_open(o->cd);
    if (!c) return fail("the CD is not a readable disc, image or folder");
    if (!is_odyssey_cd(c)) { cd_close(c); return fail("this is not the Alien Odyssey CD"); }

    Progress pr = {0};
    pr.cb = cb;
    pr.user = user;
    pr.total = cd_total_bytes(c) + (8ull << 20);
    u64 bytes = pr.total + (12ull << 20);

    snprintf(game, sizeof game, "%s\\game", o->dir);
    snprintf(hd, sizeof hd, "%s\\hd", game);
    snprintf(cd, sizeof cd, "%s\\cd", game);
    if (!make_dirs(hd)) { cd_close(c); return fail("cannot create the install folder"); }

    /* 1. the disc: drive D: of the game, always in the drive */
    setup_log("Copying %s ...", cd_describe(c));
    if (!cd_copy_all(c, cd, &pr)) { cd_close(c); return fail("copying the CD failed"); }
    cd_close(c);

    /* 2. what the DOS installer put in C:\ODYSSEY */
    setup_log("Setting up the game files ...");
    snprintf(p, sizeof p, "%s\\GAME.CFG", cd);
    snprintf(q, sizeof q, "%s\\GAME.CFG", hd);
    if (!path_exists(q) && !copy_file(p, q, NULL)) return fail("GAME.CFG missing on the CD");
    snprintf(p, sizeof p, "%s\\BRENDER", cd);
    snprintf(q, sizeof q, "%s\\BRENDER", hd);
    if (copy_dir(p, q, "*") <= 0) return fail("BRENDER folder missing on the CD");
    snprintf(q, sizeof q, "%s\\SOUND", hd);
    make_dirs(q);
    for (size_t i = 0; i < sizeof hd_sound / sizeof *hd_sound; i++) {
        char s[MAX_PATH * 3], d[MAX_PATH * 3];
        snprintf(s, sizeof s, "%s\\SOUND\\%s.ASL", cd, hd_sound[i]);
        snprintf(d, sizeof d, "%s\\%s.ASL", q, hd_sound[i]);
        copy_file(s, d, NULL);
    }
    snprintf(q, sizeof q, "%s\\SAVEGAME", hd);
    make_dirs(q);
    progress_add(&pr, 6ull << 20);

    /* 3. the game executable and its icon */
    char exe[MAX_PATH * 2];
    snprintf(exe, sizeof exe, "%s\\odyssey.exe", o->dir);
    if (!extract_prebuilt(exe)) return 0;
    snprintf(p, sizeof p, "%s\\INTRO\\DATA\\A.LBM", cd);
    size_t ln;
    u8 *lbm = read_file(p, &ln);
    if (lbm) { inject_icon_lbm(exe, lbm, ln); free(lbm); }

    /* 4. uninstaller, shortcuts */
    char me[MAX_PATH * 2];
    GetModuleFileNameA(NULL, me, sizeof me);
    snprintf(p, sizeof p, "%s\\uninstall.exe", o->dir);
    if (_stricmp(me, p)) CopyFileA(me, p, FALSE);
    register_uninstall(o->dir, bytes);
    char sm[MAX_PATH * 2], dt[MAX_PATH * 2];
    shortcut_paths(sm, dt);
    CoInitialize(NULL);
    if (o->startmenu) make_link(sm, exe, o->dir, GAME_TITLE);
    if (o->desktop) make_link(dt, exe, o->dir, GAME_TITLE);
    CoUninitialize();
    progress_add(&pr, 2ull << 20);
    setup_log("Done.");
    return 1;
}

int do_uninstall(const char *dir)
{
    char sm[MAX_PATH * 2], dt[MAX_PATH * 2], p[MAX_PATH * 2];
    shortcut_paths(sm, dt);
    DeleteFileA(sm);
    DeleteFileA(dt);
    RegDeleteKeyA(HKEY_CURRENT_USER, UNINSTALL_KEY);
    snprintf(p, sizeof p, "%s\\game", dir);
    delete_tree(p);
    static const char *files[] = {"odyssey.exe", "uninstall.exe", "odyssey_pc.ini", "odyssey.log", "setup.log"};
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        snprintf(p, sizeof p, "%s\\%s", dir, files[i]);
        DeleteFileA(p);
    }
    RemoveDirectoryA(dir);
    return 1;
}
