/* Alien Odyssey - PC Native setup */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef struct {
    u64 done, total;
    void (*cb)(u64 done, u64 total, void *user);
    void *user;
} Progress;

void progress_add(Progress *p, u64 n);
void setup_log(const char *fmt, ...);

/* util.c */
int write_file(const char *path, const void *data, size_t n);
u8 *read_file(const char *path, size_t *n);
int make_dirs(const char *path);           /* creates every missing directory */
int copy_file(const char *src, const char *dst, Progress *pr);
int path_exists(const char *p);
int is_dir(const char *p);
u32 crc32_buf(const void *d, size_t n);
int delete_tree(const char *dir);

/* cd sources: an ISO / BIN+CUE image, a drive letter or a folder */
typedef struct CdSrc CdSrc;
CdSrc *cd_open(const char *path);
void cd_close(CdSrc *c);
int cd_exists(CdSrc *c, const char *rel);          /* "3DGAME.EXE" */
u64 cd_total_bytes(CdSrc *c);
int cd_copy_all(CdSrc *c, const char *dst, Progress *pr);
u8 *cd_read(CdSrc *c, const char *rel, size_t *n);
const char *cd_describe(CdSrc *c);
