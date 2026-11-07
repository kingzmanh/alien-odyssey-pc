/* DOS services (int 21h) on Windows files.
 *
 * Drive C: is <root>\hd (the installed game, writable; the programs run
 * in its root).  The CD drive (D:) is <root>\cd, a copy of the game CD.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <stdlib.h>
#include <SDL2/SDL.h>
#include "rt.h"

extern u32 ptr_es(u32 off);
extern u32 ptr_ds(u32 off);
extern void set_es(u16 v);

#define CD_DRIVE 3   /* D: */
static int cur_drive = 2;             /* C: */
static char cwd[26][128];
static u32 dta = 0x1080;              /* inside the PSP by default */
static HANDLE handles[64];
static int last_ret;

static const char *drive_root(int drv, int which)
{
    static char buf[2][600];
    if (drv == 2) { snprintf(buf[0], 600, "%s\\hd", rt_root); return buf[0]; }
    if (drv == CD_DRIVE && which == 0) { snprintf(buf[1], 600, "%s\\cd", rt_root); return buf[1]; }
    return NULL;
}

static void read_gstr(u32 a, char *out, int max)
{
    int i = 0;
    for (; i < max - 1; i++) {
        char c = (char)RD8(a + i);
        if (!c) break;
        out[i] = c;
    }
    out[i] = 0;
}

/* DOS path -> canonical "D:\DIR\FILE" (upper case, '.'/'..' resolved) */
static int canon(const char *in, char *out, int *drv)
{
    int d = cur_drive;
    const char *p = in;
    while (*p == ' ') p++;
    if (p[0] && p[1] == ':') { d = toupper((unsigned char)p[0]) - 'A'; p += 2; }
    if (d < 0 || d >= 26) return 0;
    char tmp[300];
    if (*p == '\\' || *p == '/') snprintf(tmp, sizeof tmp, "%s", p);
    else snprintf(tmp, sizeof tmp, "%s\\%s", cwd[d], p);
    /* split and resolve */
    char comp[32][64];
    int n = 0;
    char *tok = tmp;
    while (*tok) {
        while (*tok == '\\' || *tok == '/') tok++;
        if (!*tok) break;
        char *e = tok;
        while (*e && *e != '\\' && *e != '/') e++;
        int len = (int)(e - tok);
        if (len == 1 && tok[0] == '.') {}
        else if (len == 2 && tok[0] == '.' && tok[1] == '.') { if (n) n--; }
        else if (n < 32) {
            if (len > 63) len = 63;
            for (int i = 0; i < len; i++) comp[n][i] = (char)toupper((unsigned char)tok[i]);
            comp[n][len] = 0;
            n++;
        }
        tok = e;
    }
    int o = 0;
    out[o++] = (char)('A' + d);
    out[o++] = ':';
    if (!n) out[o++] = '\\';
    for (int i = 0; i < n; i++) o += sprintf(out + o, "\\%s", comp[i]);
    out[o] = 0;
    *drv = d;
    return 1;
}

/* host path for a canonical guest path; for the CD, the first disc where it exists */
static int host_path(const char *guest, char *host, int must_exist)
{
    int d;
    char c[300];
    if (!canon(guest, c, &d)) return 0;
    if (d == CD_DRIVE) {
        for (int w = 0; w < 1; w++) {
            snprintf(host, 700, "%s%s", drive_root(d, w), c + 2);
            if (!must_exist || GetFileAttributesA(host) != INVALID_FILE_ATTRIBUTES) return 2;
        }
        return must_exist ? 0 : 2;
    }
    const char *r = drive_root(d, 0);
    if (!r) return 0;
    snprintf(host, 700, "%s%s", r, c + 2);
    return 1;
}

/* guest DOS path -> existing host file (used by the native music player) */
int dos_resolve(const char *guest, char *host) { return host_path(guest, host, 1) != 0; }

void dos_init(const char *root)
{
    (void)root;
    for (int i = 0; i < 26; i++) strcpy(cwd[i], "\\");
    strcpy(cwd[2], "\\");
    strcpy(cwd[CD_DRIVE], "\\");
    char hd[600];
    snprintf(hd, sizeof hd, "%s\\hd", rt_root);
    CreateDirectoryA(hd, NULL);
}

void dos_reset(void)
{
    for (int i = 5; i < 64; i++) if (handles[i]) { CloseHandle(handles[i]); handles[i] = NULL; }
    cur_drive = 2;
    for (int i = 0; i < 26; i++) strcpy(cwd[i], "\\");
    dta = 0x1080;
}

static int new_handle(HANDLE h)
{
    for (int i = 5; i < 64; i++) if (!handles[i]) { handles[i] = h; return i; }
    CloseHandle(h);
    return -1;
}

#define AX (cpu.r[R_EAX] & 0xffff)
#define AL (cpu.r[R_EAX] & 0xff)
#define AH ((cpu.r[R_EAX] >> 8) & 0xff)
#define BX (cpu.r[R_EBX] & 0xffff)
#define CX (cpu.r[R_ECX] & 0xffff)
#define DX (cpu.r[R_EDX] & 0xffff)
#define DL (cpu.r[R_EDX] & 0xff)
#define SETAX(v) (cpu.r[R_EAX] = (cpu.r[R_EAX] & 0xffff0000u) | ((v) & 0xffff))
#define SETAL(v) (cpu.r[R_EAX] = (cpu.r[R_EAX] & 0xffffff00u) | ((v) & 0xff))
#define SETBX(v) (cpu.r[R_EBX] = (cpu.r[R_EBX] & 0xffff0000u) | ((v) & 0xffff))
#define SETCX(v) (cpu.r[R_ECX] = (cpu.r[R_ECX] & 0xffff0000u) | ((v) & 0xffff))
#define SETDX(v) (cpu.r[R_EDX] = (cpu.r[R_EDX] & 0xffff0000u) | ((v) & 0xffff))

static void fail(int code) { cpu.cf = 1; SETAX(code); }

static int win_err(void)
{
    DWORD e = GetLastError();
    if (e == ERROR_FILE_NOT_FOUND) return 2;
    if (e == ERROR_PATH_NOT_FOUND) return 3;
    if (e == ERROR_ACCESS_DENIED) return 5;
    return 5;
}

/* ---- find first / next ---- */
#define MAXFIND 2048
typedef struct { char name[13]; u8 attr; u32 size; u16 time, date; } FEnt;
static struct { FEnt *e; int n, pos; } finds[16];
static int find_next_slot;

static void dos_time(const FILETIME *ft, u16 *t, u16 *d)
{
    FILETIME lt;
    WORD dd = 0, tt = 0;
    FileTimeToLocalFileTime(ft, &lt);
    FileTimeToDosDateTime(&lt, &dd, &tt);
    *d = dd; *t = tt;
}

static int match83(const char *pat, const char *name)
{
    /* split into 8.3 blank-padded forms, expand '*' to '?' */
    char P[11], N[11];
    memset(P, ' ', 11); memset(N, ' ', 11);
    const char *d = strchr(pat, '.');
    int i, j;
    for (i = 0, j = 0; pat[i] && pat + i != d && j < 8; i++) {
        if (pat[i] == '*') { while (j < 8) P[j++] = '?'; break; }
        P[j++] = (char)toupper((unsigned char)pat[i]);
    }
    if (d) for (i = 1, j = 8; d[i] && j < 11; i++) {
        if (d[i] == '*') { while (j < 11) P[j++] = '?'; break; }
        P[j++] = (char)toupper((unsigned char)d[i]);
    }
    if (!d && strchr(pat, '*')) for (j = 8; j < 11; j++) P[j] = '?';
    d = strrchr(name, '.');
    if (!strcmp(name, ".") || !strcmp(name, "..")) d = NULL;
    for (i = 0, j = 0; name[i] && name + i != d && j < 8; i++) N[j++] = (char)toupper((unsigned char)name[i]);
    if (d) for (i = 1, j = 8; d[i] && j < 11; i++) N[j++] = (char)toupper((unsigned char)d[i]);
    for (i = 0; i < 11; i++) if (P[i] != '?' && P[i] != N[i]) return 0;
    return 1;
}

static void fill_dta(int slot)
{
    FEnt *f = &finds[slot].e[finds[slot].pos++];
    WR8(dta, (u8)slot);
    WR8(dta + 0x15, f->attr);
    WR16(dta + 0x16, f->time);
    WR16(dta + 0x18, f->date);
    WR32(dta + 0x1a, f->size);
    memset(MEM + dta + 0x1e, 0, 13);
    memcpy(MEM + dta + 0x1e, f->name, strlen(f->name));
}

static void find_first(void)
{
    char spec[300], c[300];
    int d;
    read_gstr(ptr_ds(cpu.r[R_EDX]), spec, sizeof spec);
    u32 attr = CX;
    if (!canon(spec, c, &d)) { fail(3); return; }
    char *bs = strrchr(c, '\\');
    char pat[64];
    snprintf(pat, sizeof pat, "%s", bs + 1);
    *bs = 0;
    int slot = find_next_slot++ & 15;
    free(finds[slot].e);
    finds[slot].e = calloc(MAXFIND, sizeof(FEnt));
    finds[slot].n = finds[slot].pos = 0;
    int nroots = 1;
    for (int w = 0; w < nroots; w++) {
        const char *r = drive_root(d, w);
        if (!r) break;
        char hp[800];
        snprintf(hp, sizeof hp, "%s%s\\*", r, c + 2);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(hp, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            const char *nm = fd.cAlternateFileName[0] ? fd.cAlternateFileName : fd.cFileName;
            if (strlen(nm) > 12) continue;
            int isdir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (isdir && !(attr & 0x10)) continue;
            if (!match83(pat, nm)) continue;
            int dup = 0;
            for (int k = 0; k < finds[slot].n; k++) if (!_stricmp(finds[slot].e[k].name, nm)) dup = 1;
            if (dup || finds[slot].n >= MAXFIND) continue;
            FEnt *f = &finds[slot].e[finds[slot].n++];
            for (int k = 0; nm[k] && k < 12; k++) f->name[k] = (char)toupper((unsigned char)nm[k]);
            f->attr = isdir ? 0x10 : ((fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? 1 : 0) | 0x20;
            if (d == CD_DRIVE) f->attr |= 1;
            f->size = fd.nFileSizeLow;
            dos_time(&fd.ftLastWriteTime, &f->time, &f->date);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (!finds[slot].n) { fail(0x12); return; }
    fill_dta(slot);
    cpu.cf = 0;
    SETAX(0);
}

static void find_next(void)
{
    int slot = RD8(dta) & 15;
    if (!finds[slot].e || finds[slot].pos >= finds[slot].n) { fail(0x12); return; }
    fill_dta(slot);
    cpu.cf = 0;
    SETAX(0);
}

/* ---- file I/O ---- */
static void dos_open(int create)
{
    char g[300], h[800];
    read_gstr(ptr_ds(cpu.r[R_EDX]), g, sizeof g);
    int mode = AL & 3;
    int where = host_path(g, h, !create);
    if (!where) { rt_log("dos: open '%s' -> not found\n", g); fail(2); return; }
    if (where == 2 && (create || mode)) { mode = 0; if (create) { fail(5); return; } }
    DWORD acc = create ? GENERIC_READ | GENERIC_WRITE : mode == 0 ? GENERIC_READ : mode == 1 ? GENERIC_WRITE : GENERIC_READ | GENERIC_WRITE;
    HANDLE fh = CreateFileA(h, acc, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            create ? CREATE_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) { int e = win_err(); rt_log("dos: open '%s' failed (%d)\n", g, e); fail(e); return; }
    int n = new_handle(fh);
    if (n < 0) { fail(4); return; }
    rt_log("dos: open '%s' -> %d\n", g, n);
    {
        extern void rt_backtrace(const char *);
        static int bt;
        if (getenv("AO_BT") && bt++ < 40) rt_backtrace("open");
    }
    cpu.cf = 0;
    cpu.r[R_EAX] = n;
}

static HANDLE get_h(int n) { return (n >= 0 && n < 64) ? handles[n] : NULL; }

void rt_int21(void)
{
    u32 ah = AH;
    cpu.cf = 0;
    switch (ah) {
    case 0x01: case 0x07: case 0x08: { /* character input */
        u16 k;
        extern int bios_key_get(u16 *k, int wait);
        bios_key_get(&k, 1);
        SETAL(k & 0xff);
        break;
    }
    case 0x02: rt_log("%c", DL); break;
    case 0x06:
        if (DL == 0xff) {
            u16 k;
            extern int bios_key_get(u16 *k, int wait);
            if (bios_key_get(&k, 0)) { SETAL(k & 0xff); cpu.zf = 0; } else { SETAL(0); cpu.zf = 1; }
        } else rt_log("%c", DL);
        break;
    case 0x09: { u32 a = ptr_ds(cpu.r[R_EDX]); char c; while ((c = (char)RD8(a++)) != '$') rt_log("%c", c); break; }
    case 0x0b: { extern int bios_key_avail(void); SETAL(bios_key_avail() ? 0xff : 0); break; }
    case 0x0e: if (DL < 26) cur_drive = DL; SETAL(26); break;
    case 0x19: SETAL(cur_drive); break;
    case 0x1a: dta = ptr_ds(cpu.r[R_EDX]); break;
    case 0x25: irq_set_vector(AL, cpu.r[R_EDX]); break;
    case 0x2a: { time_t t = time(NULL); struct tm *tm = localtime(&t);
        SETCX(tm->tm_year + 1900); SETDX(((tm->tm_mon + 1) << 8) | tm->tm_mday); SETAL(tm->tm_wday); break; }
    case 0x2b: SETAL(0xff); break; /* set date: refused (also the DESQview probe) */
    case 0x2c: { SYSTEMTIME st; GetLocalTime(&st);
        SETCX((st.wHour << 8) | st.wMinute); SETDX((st.wSecond << 8) | (st.wMilliseconds / 10)); break; }
    case 0x2f: set_es(SEL_DATA); cpu.r[R_EBX] = dta; break;
    case 0x30:
        cpu.r[R_EAX] = 0x0007; /* DOS 7.0 */
        SETBX(0); SETCX(0);
        break;
    case 0x33: cpu.r[R_EDX] &= ~0xff; break;
    case 0x35: set_es(SEL_CODE); cpu.r[R_EBX] = irq_get_vector(AL); break;
    case 0x36: SETAX(64); SETBX(0xffff); SETCX(512); SETDX(0xffff); break;
    case 0x37: if (AL == 0) cpu.r[R_EDX] = (cpu.r[R_EDX] & ~0xff) | '/'; break;
    case 0x39: case 0x3a: case 0x3b: {
        char g[300], c[300], h[800];
        int d;
        read_gstr(ptr_ds(cpu.r[R_EDX]), g, sizeof g);
        if (!canon(g, c, &d)) { fail(3); break; }
        if (ah == 0x39) {
            if (!host_path(g, h, 0) || !CreateDirectoryA(h, NULL)) { if (GetLastError() != ERROR_ALREADY_EXISTS) fail(5); }
            break;
        }
        if (ah == 0x3a) { if (!host_path(g, h, 1) || !RemoveDirectoryA(h)) fail(5); break; }
        if (!host_path(g, h, 1)) { rt_log("dos: chdir '%s' -> not found\n", g); fail(3); break; }
        snprintf(cwd[d], sizeof cwd[d], "%s", c[2] ? c + 2 : "\\");
        if (!cwd[d][0]) strcpy(cwd[d], "\\");
        break;
    }
    case 0x3c: dos_open(1); break;
    case 0x3d: dos_open(0); break;
    case 0x3e: {
        HANDLE fh = get_h(BX);
        if (BX < 5) break;
        if (!fh) { fail(6); break; }
        CloseHandle(fh);
        handles[BX] = NULL;
        break;
    }
    case 0x3f: {
        u32 n = cpu.r[R_ECX], buf = ptr_ds(cpu.r[R_EDX]);
        if (BX == 0) { cpu.r[R_EAX] = 0; break; }
        HANDLE fh = get_h(BX);
        if (!fh) { fail(6); break; }
        DWORD got = 0;
        {   /* DOS reads "up to n": the game passes huge counts meaning "until
               end of file", which ReadFile would reject outright */
            LARGE_INTEGER sz, cur, zero = {0};
            if (GetFileSizeEx(fh, &sz) && SetFilePointerEx(fh, zero, &cur, FILE_CURRENT)) {
                u64 left = sz.QuadPart > cur.QuadPart ? (u64)(sz.QuadPart - cur.QuadPart) : 0;
                if (n > left) n = (u32)left;
            }
        }
        if (!ReadFile(fh, MEM + buf, n, &got, NULL)) { fail(5); break; }
        if (getenv("AO_IOLOG")) rt_log("dos: read h%u %u bytes -> %u at %08x\n", BX, n, (u32)got, buf);
        cpu.r[R_EAX] = got;
        break;
    }
    case 0x40: {
        u32 n = cpu.r[R_ECX], buf = ptr_ds(cpu.r[R_EDX]);
        if (BX == 1 || BX == 2) {
            rt_log("%.*s", (int)n, (char *)MEM + buf);
            cpu.r[R_EAX] = n;
            break;
        }
        HANDLE fh = get_h(BX);
        if (!fh) { fail(6); break; }
        DWORD put = 0;
        if (n == 0) { SetEndOfFile(fh); cpu.r[R_EAX] = 0; break; }
        if (!WriteFile(fh, MEM + buf, n, &put, NULL)) { fail(5); break; }
        cpu.r[R_EAX] = put;
        break;
    }
    case 0x41: {
        char g[300], h[800];
        read_gstr(ptr_ds(cpu.r[R_EDX]), g, sizeof g);
        int w = host_path(g, h, 1);
        if (!w) { fail(2); break; }
        if (w == 2 || !DeleteFileA(h)) fail(5);
        break;
    }
    case 0x42: {
        HANDLE fh = get_h(BX);
        if (!fh) { fail(6); break; }
        LARGE_INTEGER li, np;
        li.QuadPart = (s32)(((u32)CX << 16) | DX);
        DWORD how = AL == 0 ? FILE_BEGIN : AL == 1 ? FILE_CURRENT : FILE_END;
        if (!SetFilePointerEx(fh, li, &np, how)) { fail(0x19); break; }
        SETAX((u32)np.QuadPart); SETDX((u32)np.QuadPart >> 16);
        cpu.r[R_EAX] = (cpu.r[R_EAX] & 0xffff) | 0; /* high half unused */
        break;
    }
    case 0x43: {
        char g[300], h[800];
        read_gstr(ptr_ds(cpu.r[R_EDX]), g, sizeof g);
        int w = host_path(g, h, 1);
        if (!w) { fail(2); break; }
        if (AL == 0) {
            DWORD a = GetFileAttributesA(h);
            SETCX(((a & FILE_ATTRIBUTE_DIRECTORY) ? 0x10 : 0x20) | (w == 2 ? 1 : 0));
        }
        break;
    }
    case 0x44:
        if (AL == 0) {
            if (BX < 5) SETDX(0x80d3);
            else if (get_h(BX)) SETDX(0x0002);
            else fail(6);
        } else if (AL == 8) SETAX(1);
        else if (AL == 9) SETDX(0);
        else if (AL == 0x0e) SETAL(0);
        else rt_log("dos: ioctl %02x\n", AL);
        break;
    case 0x45: {
        HANDLE fh = get_h(BX), nh;
        if (!fh) { fail(6); break; }
        DuplicateHandle(GetCurrentProcess(), fh, GetCurrentProcess(), &nh, 0, FALSE, DUPLICATE_SAME_ACCESS);
        int n = new_handle(nh);
        if (n < 0) fail(4); else cpu.r[R_EAX] = n;
        break;
    }
    case 0x47: {
        int d = DL ? DL - 1 : cur_drive;
        u32 a = ptr_ds(cpu.r[R_ESI]);
        const char *s = cwd[d][0] == '\\' ? cwd[d] + 1 : cwd[d];
        for (int i = 0;; i++) { WR8(a + i, s[i]); if (!s[i]) break; }
        break;
    }
    case 0x48: {
        u16 seg, sel;
        if (!dosmem_alloc(BX, &seg, &sel)) { fail(8); SETBX(0); break; }
        SETAX(sel);
        break;
    }
    case 0x49: break;
    case 0x4a: break; /* the program's memory is always large enough */
    case 0x4b: rt_log("dos: exec refused\n"); fail(2); break;
    case 0x4c:
        rt_exit(AL);
        break;
    case 0x4d: SETAX(last_ret); break;
    case 0x4e: find_first(); break;
    case 0x4f: find_next(); break;
    case 0x56: {
        char g1[300], g2[300], h1[800], h2[800];
        read_gstr(ptr_ds(cpu.r[R_EDX]), g1, sizeof g1);
        read_gstr(ptr_es(cpu.r[R_EDI]), g2, sizeof g2);
        if (host_path(g1, h1, 1) != 1 || host_path(g2, h2, 0) != 1 || !MoveFileA(h1, h2)) fail(5);
        break;
    }
    case 0x57: {
        HANDLE fh = get_h(BX);
        if (!fh) { fail(6); break; }
        FILETIME ft;
        if (AL == 0) {
            u16 t, d;
            GetFileTime(fh, NULL, NULL, &ft);
            dos_time(&ft, &t, &d);
            SETCX(t); SETDX(d);
        }
        break;
    }
    case 0x62: SETBX(SEL_PSP); break;
    case 0xff:
        if (AL == 0) { cpu.r[R_EAX] = 0xffff3447; break; } /* DOS/4G-compatible extender present */
        /* fallthrough */
    default:
        rt_log("dos: int21 ah=%02x al=%02x not provided\n", ah, AL);
        fail(1);
    }
}
