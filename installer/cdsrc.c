/* CD sources for the installer: an ISO 9660 image (.iso, raw .bin or .cue),
 * a CD drive or a folder holding a copy of the disc. */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "setup.h"

typedef struct { char name[64]; u32 lba, size; int dir; } IsoEnt;

struct CdSrc {
    int image;
    char path[MAX_PATH * 2];      /* image file or root folder */
    HANDLE h;
    u32 sector, data_off;         /* bytes per sector, user data offset in it */
    u32 root_lba, root_size;
    char desc[MAX_PATH * 2 + 32];
};

/* ---------- image access ---------- */
static int read_sectors(CdSrc *c, u32 lba, u32 count, u8 *out)
{
    for (u32 i = 0; i < count; i++) {
        LARGE_INTEGER pos;
        pos.QuadPart = (LONGLONG)(lba + i) * c->sector + c->data_off;
        DWORD r = 0;
        if (!SetFilePointerEx(c->h, pos, NULL, FILE_BEGIN) || !ReadFile(c->h, out + i * 2048, 2048, &r, NULL) || r != 2048)
            return 0;
    }
    return 1;
}

static int probe(CdSrc *c, u32 sector, u32 off)
{
    u8 pvd[2048];
    c->sector = sector;
    c->data_off = off;
    if (!read_sectors(c, 16, 1, pvd)) return 0;
    if (pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5)) return 0;
    const u8 *root = pvd + 156;
    c->root_lba = root[2] | root[3] << 8 | root[4] << 16 | (u32)root[5] << 24;
    c->root_size = root[10] | root[11] << 8 | root[12] << 16 | (u32)root[13] << 24;
    return 1;
}

/* list one directory of the image */
static IsoEnt *iso_dir(CdSrc *c, u32 lba, u32 size, int *count)
{
    u32 ns = (size + 2047) / 2048;
    u8 *buf = malloc(ns * 2048);
    int cap = 64, n = 0;
    IsoEnt *e = malloc(cap * sizeof *e);
    if (!read_sectors(c, lba, ns, buf)) { free(buf); *count = 0; return e; }
    u32 p = 0;
    while (p < size) {
        u8 len = buf[p];
        if (len == 0) { p = (p / 2048 + 1) * 2048; continue; }
        u8 nl = buf[p + 32];
        const char *nm = (const char *)buf + p + 33;
        if (!(nl == 1 && (nm[0] == 0 || nm[0] == 1))) {
            if (n == cap) { cap *= 2; e = realloc(e, cap * sizeof *e); }
            IsoEnt *x = &e[n++];
            int k = 0;
            for (int i = 0; i < nl && nm[i] != ';' && k < 63; i++) x->name[k++] = nm[i];
            if (k && x->name[k - 1] == '.') k--;
            x->name[k] = 0;
            const u8 *r = buf + p;
            x->lba = r[2] | r[3] << 8 | r[4] << 16 | (u32)r[5] << 24;
            x->size = r[10] | r[11] << 8 | r[12] << 16 | (u32)r[13] << 24;
            x->dir = (r[25] & 2) != 0;
        }
        p += len;
    }
    free(buf);
    *count = n;
    return e;
}

static int iso_find(CdSrc *c, const char *rel, IsoEnt *out)
{
    u32 lba = c->root_lba, size = c->root_size;
    char part[64];
    const char *s = rel;
    while (*s) {
        int k = 0;
        while (*s && *s != '\\' && *s != '/' && k < 63) part[k++] = (char)toupper((u8)*s++);
        part[k] = 0;
        while (*s == '\\' || *s == '/') s++;
        int n;
        IsoEnt *e = iso_dir(c, lba, size, &n);
        int found = -1;
        for (int i = 0; i < n; i++) if (!_stricmp(e[i].name, part)) { found = i; break; }
        if (found < 0) { free(e); return 0; }
        *out = e[found];
        lba = e[found].lba;
        size = e[found].size;
        free(e);
    }
    return 1;
}

/* ---------- open ---------- */
static int open_image(CdSrc *c, const char *file)
{
    c->h = CreateFileA(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (c->h == INVALID_HANDLE_VALUE) return 0;
    if (probe(c, 2048, 0) || probe(c, 2352, 16) || probe(c, 2352, 24)) return 1;
    CloseHandle(c->h);
    c->h = INVALID_HANDLE_VALUE;
    return 0;
}

CdSrc *cd_open(const char *path)
{
    CdSrc *c = calloc(1, sizeof *c);
    c->h = INVALID_HANDLE_VALUE;
    char p[MAX_PATH * 2];
    snprintf(p, sizeof p, "%s", path);
    size_t L = strlen(p);
    while (L > 3 && (p[L - 1] == '\\' || p[L - 1] == '/')) p[--L] = 0;
    if (L == 2 && p[1] == ':') { p[2] = '\\'; p[3] = 0; }
    if (is_dir(p)) {
        snprintf(c->path, sizeof c->path, "%s", p);
        snprintf(c->desc, sizeof c->desc, "folder %s", p);
        return c;
    }
    const char *ext = strrchr(p, '.');
    if (ext && !_stricmp(ext, ".cue")) {
        /* FILE "name.bin" BINARY */
        size_t n;
        char *cue = (char *)read_file(p, &n);
        if (cue) {
            cue[n] = 0;
            char *f = strstr(cue, "FILE");
            char *q1 = f ? strchr(f, '"') : NULL;
            char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            if (q2) {
                *q2 = 0;
                char dir[MAX_PATH * 2];
                snprintf(dir, sizeof dir, "%s", p);
                char *bs = strrchr(dir, '\\');
                if (bs) bs[1] = 0; else dir[0] = 0;
                snprintf(p, sizeof p, "%s%s", strchr(q1 + 1, ':') ? "" : dir, q1 + 1);
            }
            free(cue);
        }
    }
    if (!open_image(c, p)) { free(c); return NULL; }
    c->image = 1;
    snprintf(c->path, sizeof c->path, "%s", p);
    snprintf(c->desc, sizeof c->desc, "image %s", p);
    return c;
}

void cd_close(CdSrc *c)
{
    if (!c) return;
    if (c->h != INVALID_HANDLE_VALUE) CloseHandle(c->h);
    free(c);
}

const char *cd_describe(CdSrc *c) { return c->desc; }

int cd_exists(CdSrc *c, const char *rel)
{
    if (c->image) { IsoEnt e; return iso_find(c, rel, &e); }
    char p[MAX_PATH * 2];
    snprintf(p, sizeof p, "%s\\%s", c->path, rel);
    return path_exists(p);
}

u8 *cd_read(CdSrc *c, const char *rel, size_t *n)
{
    if (!c->image) {
        char p[MAX_PATH * 2];
        snprintf(p, sizeof p, "%s\\%s", c->path, rel);
        return read_file(p, n);
    }
    IsoEnt e;
    if (!iso_find(c, rel, &e) || e.dir) return NULL;
    u32 ns = (e.size + 2047) / 2048;
    u8 *d = malloc(ns * 2048 + 1);
    if (!read_sectors(c, e.lba, ns, d)) { free(d); return NULL; }
    *n = e.size;
    return d;
}

/* ---------- walking ---------- */
static u64 iso_total(CdSrc *c, u32 lba, u32 size, int depth)
{
    int n;
    IsoEnt *e = iso_dir(c, lba, size, &n);
    u64 t = 0;
    for (int i = 0; i < n; i++)
        t += e[i].dir ? (depth < 16 ? iso_total(c, e[i].lba, e[i].size, depth + 1) : 0) : e[i].size;
    free(e);
    return t;
}

static u64 dir_total(const char *dir, int depth)
{
    char pat[MAX_PATH * 2];
    snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    u64 t = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            char p[MAX_PATH * 2];
            snprintf(p, sizeof p, "%s\\%s", dir, fd.cFileName);
            if (depth < 16) t += dir_total(p, depth + 1);
        } else t += ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return t;
}

u64 cd_total_bytes(CdSrc *c)
{
    return c->image ? iso_total(c, c->root_lba, c->root_size, 0) : dir_total(c->path, 0);
}

static int iso_copy(CdSrc *c, u32 lba, u32 size, const char *dst, Progress *pr, int depth)
{
    int n;
    IsoEnt *e = iso_dir(c, lba, size, &n);
    make_dirs(dst);
    static u8 buf[32 * 2048];
    int ok = 1;
    for (int i = 0; i < n && ok; i++) {
        char p[MAX_PATH * 2];
        snprintf(p, sizeof p, "%s\\%s", dst, e[i].name);
        if (e[i].dir) { if (depth < 16) ok = iso_copy(c, e[i].lba, e[i].size, p, pr, depth + 1); continue; }
        HANDLE out = CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (out == INVALID_HANDLE_VALUE) { setup_log("cannot write %s", p); ok = 0; break; }
        u32 left = e[i].size, l = e[i].lba;
        while (left && ok) {
            u32 ns = (left + 2047) / 2048;
            if (ns > 32) ns = 32;
            if (!read_sectors(c, l, ns, buf)) { setup_log("read error in %s", e[i].name); ok = 0; break; }
            u32 chunk = ns * 2048 < left ? ns * 2048 : left;
            DWORD w;
            if (!WriteFile(out, buf, chunk, &w, NULL) || w != chunk) { ok = 0; break; }
            left -= chunk;
            l += ns;
            if (pr) progress_add(pr, chunk);
        }
        CloseHandle(out);
    }
    free(e);
    return ok;
}

static int dir_copy(const char *src, const char *dst, Progress *pr, int depth)
{
    char pat[MAX_PATH * 2];
    snprintf(pat, sizeof pat, "%s\\*", src);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    make_dirs(dst);
    if (h == INVALID_HANDLE_VALUE) return 1;
    int ok = 1;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char s[MAX_PATH * 2], d[MAX_PATH * 2];
        snprintf(s, sizeof s, "%s\\%s", src, fd.cFileName);
        snprintf(d, sizeof d, "%s\\%s", dst, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth < 16) ok = dir_copy(s, d, pr, depth + 1); }
        else {
            ok = copy_file(s, d, pr);
            SetFileAttributesA(d, FILE_ATTRIBUTE_NORMAL);   /* CD files come read-only */
        }
    } while (ok && FindNextFileA(h, &fd));
    FindClose(h);
    return ok;
}

int cd_copy_all(CdSrc *c, const char *dst, Progress *pr)
{
    return c->image ? iso_copy(c, c->root_lba, c->root_size, dst, pr, 0) : dir_copy(c->path, dst, pr, 0);
}
