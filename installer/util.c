/* file helpers for the installer */
#include <stdlib.h>
#include <string.h>
#include "setup.h"

void progress_add(Progress *p, u64 n)
{
    p->done += n;
    if (p->cb) p->cb(p->done, p->total, p->user);
}

int path_exists(const char *p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }

int is_dir(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

int make_dirs(const char *path)
{
    char buf[MAX_PATH * 2];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 3; *p; p++) {
        if (*p == '\\' || *p == '/') {
            char c = *p;
            *p = 0;
            CreateDirectoryA(buf, NULL);
            *p = c;
        }
    }
    CreateDirectoryA(buf, NULL);
    return is_dir(buf);
}

int write_file(const char *path, const void *data, size_t n)
{
    char dir[MAX_PATH * 2];
    snprintf(dir, sizeof dir, "%s", path);
    char *s = strrchr(dir, '\\');
    if (s) { *s = 0; make_dirs(dir); }
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { setup_log("cannot write %s (error %lu)", path, GetLastError()); return 0; }
    DWORD w = 0;
    BOOL ok = WriteFile(h, data, (DWORD)n, &w, NULL) && w == n;
    CloseHandle(h);
    if (!ok) setup_log("write failed: %s", path);
    return ok;
}

u8 *read_file(const char *path, size_t *n)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    u8 *d = malloc((size_t)sz.QuadPart + 1);
    DWORD r = 0;
    if (!d || !ReadFile(h, d, (DWORD)sz.QuadPart, &r, NULL) || r != sz.QuadPart) { free(d); CloseHandle(h); return NULL; }
    CloseHandle(h);
    *n = (size_t)sz.QuadPart;
    return d;
}

int copy_file(const char *src, const char *dst, Progress *pr)
{
    char dir[MAX_PATH * 2];
    snprintf(dir, sizeof dir, "%s", dst);
    char *s = strrchr(dir, '\\');
    if (s) { *s = 0; make_dirs(dir); }
    HANDLE in = CreateFileA(src, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (in == INVALID_HANDLE_VALUE) { setup_log("cannot read %s", src); return 0; }
    HANDLE out = CreateFileA(dst, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) { CloseHandle(in); setup_log("cannot write %s", dst); return 0; }
    static u8 buf[1 << 20];
    DWORD r, w;
    int ok = 1;
    while (ReadFile(in, buf, sizeof buf, &r, NULL) && r) {
        if (!WriteFile(out, buf, r, &w, NULL) || w != r) { ok = 0; break; }
        if (pr) progress_add(pr, r);
    }
    CloseHandle(in);
    CloseHandle(out);
    return ok;
}

static u32 crc_tab[256];
u32 crc32_buf(const void *d, size_t n)
{
    if (!crc_tab[1])
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            crc_tab[i] = c;
        }
    u32 c = 0xffffffffu;
    const u8 *p = d;
    while (n--) c = crc_tab[(c ^ *p++) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

int delete_tree(const char *dir)
{
    char pat[MAX_PATH * 2];
    snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            char p[MAX_PATH * 2];
            snprintf(p, sizeof p, "%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {   /* junction: unlink only */
                RemoveDirectoryA(p);
                continue;
            }
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) delete_tree(p);
            else { SetFileAttributesA(p, FILE_ATTRIBUTE_NORMAL); DeleteFileA(p); }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryA(dir);
}
