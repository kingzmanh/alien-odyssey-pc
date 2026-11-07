/* MSCDEX services that address the CD by sector: the FMV programs look up a
 * video file's directory entry (int 2Fh/150Fh) and then stream it with
 * READ LONG device requests (int 2Fh/1510h).  The CD is a folder of files
 * here, so every file that is looked up gets its own range of virtual
 * sectors, and sector reads are served from that file. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include <stdlib.h>
#include "rt.h"

extern u32 ptr_es(u32 off);
extern int dos_resolve(const char *guest, char *host);

#define SECTOR 2048
#define FIRST_LBA 1000

typedef struct { char host[700]; u32 lba, nsec, size; } CdFile;
static CdFile files[256];
static int nfiles;
static u32 next_lba = FIRST_LBA;
static HANDLE open_h = INVALID_HANDLE_VALUE;
static int open_idx = -1;

static CdFile *lookup(const char *guest)
{
    char host[700];
    if (!dos_resolve(guest, host)) return NULL;
    for (int i = 0; i < nfiles; i++) if (!_stricmp(files[i].host, host)) return &files[i];
    if (nfiles >= 256) return NULL;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(host, GetFileExInfoStandard, &fa)) return NULL;
    CdFile *f = &files[nfiles++];
    snprintf(f->host, sizeof f->host, "%s", host);
    f->size = fa.nFileSizeLow;
    f->nsec = (f->size + SECTOR - 1) / SECTOR;
    f->lba = next_lba;
    next_lba += f->nsec + 16;
    return f;
}

/* 150Fh: CX = drive, ES:BX = path, SI:DI = buffer for the directory record.
   Returns AX = 1 (ISO 9660) or CF + error. */
int mscdex_dir_entry(u32 path_addr, u32 buf_addr)
{
    char path[300];
    int i = 0;
    for (; i < 299; i++) { char c = (char)RD8(path_addr + i); if (!c) break; path[i] = c; }
    path[i] = 0;
    char g[310];
    if (path[0] && path[1] == ':') snprintf(g, sizeof g, "%s", path);
    else snprintf(g, sizeof g, "D:%s", path);
    CdFile *f = lookup(g);
    if (!f) { rt_log("cd: dir entry '%s' not found\n", path); return 0; }
    const char *nm = strrchr(path, '\\');
    nm = nm ? nm + 1 : path;
    char name[40];
    snprintf(name, sizeof name, "%s;1", nm);
    int nl = (int)strlen(name);
    u8 *d = MEM + buf_addr;
    memset(d, 0, 34 + nl);
    d[0] = (u8)(33 + nl + ((33 + nl) & 1));
    d[2] = f->lba; d[3] = f->lba >> 8; d[4] = f->lba >> 16; d[5] = f->lba >> 24;
    d[6] = f->lba >> 24; d[7] = f->lba >> 16; d[8] = f->lba >> 8; d[9] = f->lba;
    d[10] = f->size; d[11] = f->size >> 8; d[12] = f->size >> 16; d[13] = f->size >> 24;
    d[14] = f->size >> 24; d[15] = f->size >> 16; d[16] = f->size >> 8; d[17] = f->size;
    d[18] = 95; d[19] = 10; d[20] = 12;
    d[28] = 1; d[31] = 1;
    d[32] = (u8)nl;
    memcpy(d + 33, name, nl);
    rt_log("cd: dir entry '%s' -> lba %u size %u\n", path, f->lba, f->size);
    return 1;
}

static int read_sector(u32 lba, u8 *out)
{
    for (int i = 0; i < nfiles; i++) {
        CdFile *f = &files[i];
        if (lba < f->lba || lba >= f->lba + f->nsec) continue;
        if (open_idx != i) {
            if (open_h != INVALID_HANDLE_VALUE) CloseHandle(open_h);
            open_h = CreateFileA(f->host, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
            open_idx = open_h == INVALID_HANDLE_VALUE ? -1 : i;
            if (open_idx < 0) return 0;
        }
        LARGE_INTEGER pos;
        pos.QuadPart = (LONGLONG)(lba - f->lba) * SECTOR;
        SetFilePointerEx(open_h, pos, NULL, FILE_BEGIN);
        DWORD got = 0;
        ReadFile(open_h, out, SECTOR, &got, NULL);
        if (got < SECTOR) memset(out + got, 0, SECTOR - got);
        return 1;
    }
    memset(out, 0, SECTOR);
    return 0;
}

/* 1510h: CX = drive, ES:BX = device driver request header */
void mscdex_request(u32 rq)
{
    u8 cmd = RD8(rq + 2);
    u16 status = 0x0100;  /* done */
    switch (cmd) {
    case 128: {   /* READ LONG */
        u8 mode = RD8(rq + 0x0d);
        u32 fp = RD32(rq + 0x0e);
        u32 dst = (fp >> 16) * 16 + (fp & 0xffff);
        u32 n = RD16(rq + 0x12);
        u32 lba = RD32(rq + 0x14);
        if (mode == 1) {  /* Red Book minute/second/frame */
            u32 m = (lba >> 16) & 0xff, s = (lba >> 8) & 0xff, fr = lba & 0xff;
            lba = (m * 60 + s) * 75 + fr - 150;
        }
        for (u32 k = 0; k < n; k++) read_sector(lba + k, MEM + dst + k * SECTOR);
        if (getenv("AO_CDLOG")) rt_log("cd: read %u sectors at %u -> %08x\n", n, lba, dst);
        break;
    }
    case 130:     /* READ LONG PREFETCH: data is always at hand */
    case 131:     /* SEEK */
    case 133:     /* STOP AUDIO */
    case 136:     /* RESUME AUDIO */
    case 13:      /* DEVICE OPEN */
    case 14:      /* DEVICE CLOSE */
    case 7:       /* INPUT FLUSH */
        break;
    case 3: {     /* IOCTL INPUT */
        u32 fp = RD32(rq + 0x0e);
        u32 cb = (fp >> 16) * 16 + (fp & 0xffff);
        u8 code = RD8(cb);
        switch (code) {
        case 6: WR32(cb + 1, 0x00000002u); break;   /* device status: door closed, cooked+raw */
        case 9: WR8(cb + 1, 1); break;              /* media not changed */
        case 7: WR16(cb + 1, SECTOR); break;        /* sector size */
        case 8: WR32(cb + 1, next_lba + 1000); break; /* volume size */
        case 1: WR32(cb + 2, 0); break;             /* head location */
        default: rt_log("cd: ioctl input %u\n", code); break;
        }
        break;
    }
    default:
        rt_log("cd: device request %u not provided\n", cmd);
        status = 0x8103;  /* error: unknown command */
        break;
    }
    WR16(rq + 3, status);
}

void cdrom_reset(void)
{
    if (open_h != INVALID_HANDLE_VALUE) CloseHandle(open_h);
    open_h = INVALID_HANDLE_VALUE;
    open_idx = -1;
}
