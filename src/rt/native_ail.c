/* Native Miles Sound System (AIL 3.x) for Alien Odyssey.
 *
 * The six programs are built against the Miles "Audio Interface Library",
 * which drives DOS sound cards through loadable .DIG/.MDI drivers.  Its
 * digital-sound and XMIDI APIs are replaced here at the AIL_API_* level
 * (the functions the AIL_xxx wrappers call; addresses per program in
 * tools/native_<module>.txt, found by tools/ail_api.py):
 *   - samples (one-shot, looping and double-buffered streams such as the
 *     FMV soundtracks) are mixed on SDL audio straight out of guest memory;
 *   - XMIDI sequences are played by an XMIDI interpreter (120 Hz, FOR/NEXT
 *     loops, branch points, callback triggers) on the Windows MIDI synth.
 * AIL's own timer services stay as they are (recompiled).
 *
 * All AIL_API functions are cdecl: arguments at [esp+4], [esp+8] ..., the
 * result in eax, the caller pops the arguments. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <string.h>
#include <stdlib.h>
#include <SDL2/SDL.h>
#include "rt.h"

#define ARG(n) RD32(cpu.r[R_ESP] + 4 + 4 * (n))
#define RET_V(v) do { cpu.r[R_EAX] = (u32)(v); cpu.r[R_ESP] += 4; return; } while (0)
#define RET0() do { cpu.r[R_ESP] += 4; return; } while (0)

#define OUT_RATE 44100
#define NSAMP 32
#define NSEQ 8

/* sample / sequence status values */
#define SMP_FREE 1
#define SMP_DONE 2
#define SMP_PLAYING 4
#define SMP_STOPPED 8
#define SEQ_FREE 1
#define SEQ_DONE 2
#define SEQ_PLAYING 4
#define SEQ_STOPPED 8

extern void rt_guest_callback(u32 fn, u32 arg0, u32 arg1);

/* ---- guest handles: small blocks in guest memory, so the program gets
   real pointers; everything lives in the native tables below ---- */
static u32 handle_pool, dig_handle, mdi_handle;

static u32 pool_addr(int kind, int idx)   /* kind 0 = sample, 1 = sequence */
{
    if (!handle_pool) {
        handle_pool = heap_alloc(0x2000);
        dig_handle = handle_pool;
        mdi_handle = handle_pool + 0x40;
        WR32(dig_handle, 0x44494721);  /* "!GID" */
        WR32(mdi_handle, 0x49444d21);
    }
    return handle_pool + 0x100 + kind * 0x800 + idx * 0x40;
}

/* ================= digital ================= */
typedef struct {
    int used, status;
    u32 addr, len;          /* one-shot / looping data */
    int format;             /* 0 mono8, 1 mono16, 2 stereo8, 3 stereo16 */
    int sign8;              /* 8-bit data is signed */
    int rate, vol, pan, loops;   /* loops: 0 = forever, else count */
    u64 pos, step;          /* 32.32 frames */
    /* double-buffered streaming */
    int stream;
    u32 baddr[2], blen[2];
    int bfull[2], bcur, bnext_fill, bend;
    u32 eos_cb, sob_cb, eob_cb;
    int eos_pending, eob_pending;
    s32 user[8];
} Samp;

static Samp samp[NSAMP];
static SDL_AudioDeviceID adev;
static SDL_mutex *amx;
static int dig_master = 127;
static int muted;

static int samp_index(u32 h)
{
    u32 b = pool_addr(0, 0);
    if (h < b || h >= b + NSAMP * 0x40 || (h - b) % 0x40) return -1;
    return (int)((h - b) / 0x40);
}

static int frame_bytes(int format) { return (format & 1 ? 2 : 1) * (format & 2 ? 2 : 1); }

static void update_step(Samp *s)
{
    int r = s->rate > 0 ? s->rate : 11025;
    s->step = (u64)((double)r / OUT_RATE * 4294967296.0);
}

static int read_frame(Samp *s, u32 addr, u32 f, s32 *l, s32 *r)
{
    const u8 *p = MEM + addr;
    switch (s->format & 3) {
    case 0: *l = *r = s->sign8 ? (s8)p[f] << 8 : (p[f] - 128) << 8; break;
    case 1: *l = *r = ((const s16 *)p)[f]; break;
    case 2:
        *l = s->sign8 ? (s8)p[f * 2] << 8 : (p[f * 2] - 128) << 8;
        *r = s->sign8 ? (s8)p[f * 2 + 1] << 8 : (p[f * 2 + 1] - 128) << 8;
        break;
    default: *l = ((const s16 *)p)[f * 2]; *r = ((const s16 *)p)[f * 2 + 1]; break;
    }
    return 1;
}

static void mix(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    s16 *out = (s16 *)stream;
    int frames = len / 4;
    static s32 acc[16384 * 2];
    if (frames > 16384) frames = 16384;
    memset(acc, 0, frames * 2 * sizeof(s32));
    SDL_LockMutex(amx);
    for (int v = 0; v < NSAMP; v++) {
        Samp *s = &samp[v];
        if (!s->used || s->status != SMP_PLAYING) continue;
        int fb = frame_bytes(s->format);
        int gain = s->vol * dig_master;               /* 0..127*127 */
        int pl = s->pan > 64 ? (127 - s->pan) * 2 : 127;
        int pr = s->pan < 64 ? s->pan * 2 : 127;
        for (int i = 0; i < frames; i++) {
            u32 addr, nfr;
            if (s->stream) {
                if (!s->bfull[s->bcur]) {
                    /* nothing queued: like AIL, the sample stops (it is done
                       when the program stopped feeding it); the next
                       load_sample_buffer starts it again */
                    s->status = SMP_DONE;
                    s->eos_pending = 1;
                    break;
                }
                addr = s->baddr[s->bcur];
                nfr = s->blen[s->bcur] / fb;
            } else {
                addr = s->addr;
                nfr = s->len / fb;
            }
            u32 f = (u32)(s->pos >> 32);
            if (f >= nfr) {
                if (s->stream) {
                    s->bfull[s->bcur] = 0;
                    s->eob_pending = 1;
                    s->bcur ^= 1;
                    s->pos = 0;
                    i--;
                    continue;
                }
                if (s->loops != 1) {
                    if (s->loops > 1) s->loops--;
                    s->pos -= (u64)nfr << 32;
                    if ((u32)(s->pos >> 32) >= nfr) s->pos = 0;
                    f = (u32)(s->pos >> 32);
                    if (!nfr) { s->status = SMP_DONE; s->eos_pending = 1; break; }
                } else { s->status = SMP_DONE; s->eos_pending = 1; break; }
            }
            s32 l, r;
            read_frame(s, addr, f, &l, &r);
            acc[i * 2] += (s32)((s64)l * gain / (127 * 127) * pl / 127);
            acc[i * 2 + 1] += (s32)((s64)r * gain / (127 * 127) * pr / 127);
            s->pos += s->step;
        }
    }
    SDL_UnlockMutex(amx);
    for (int i = 0; i < frames * 2; i++) {
        s32 x = acc[i];
        out[i] = (s16)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
    }
    if (frames * 4 < len) memset(stream + frames * 4, 0, len - frames * 4);
    {   /* AO_WAVOUT=<file>: record the mix (raw 44.1 kHz s16 stereo) for checking */
        static FILE *wf;
        static int tried;
        if (!tried) { const char *e = getenv("AO_WAVOUT"); if (e) wf = fopen(e, "wb"); tried = 1; }
        if (wf) { fwrite(stream, 1, len, wf); fflush(wf); }
    }
    if (muted) memset(stream, 0, len);
}

static void audio_open(void)
{
    if (adev) return;
    if (!amx) amx = SDL_CreateMutex();
    SDL_AudioSpec want = {0}, have;
    want.freq = OUT_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = mix;
    adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!adev) { rt_log("sound: cannot open audio: %s\n", SDL_GetError()); return; }
    muted = getenv("AO_HIDDEN") != NULL;   /* background test runs stay silent */
    SDL_PauseAudioDevice(adev, 0);
    rt_log("sound: SDL audio %d Hz\n", have.freq);
}

static Samp *S_arg(int n, int *idx)
{
    int i = samp_index(ARG(n));
    if (idx) *idx = i;
    return i >= 0 ? &samp[i] : NULL;
}

static void samp_init(Samp *s)
{
    memset(s, 0, sizeof *s);
    s->used = 1;
    s->status = SMP_DONE;
    s->rate = 11025;
    s->vol = 127;
    s->pan = 64;
    s->loops = 1;
    update_step(s);
}

/* AIL_install_DIG_INI(HDIGDRIVER *dig) -> 0 = success */
void N_AIL_install_DIG_INI(void)
{
    if (getenv("AO_NODIG")) RET_V(1);   /* debugging: no digital driver */
    audio_open();
    pool_addr(0, 0);
    u32 p = ARG(0);
    if (p) WR32(p, dig_handle);
    rt_log("sound: digital driver installed (native)\n");
    RET_V(0);
}

/* AIL_install_DIG_driver_file(char *name, IO_PARMS *io) / _image(...) -> HDIGDRIVER */
void N_AIL_install_DIG_driver(void) { audio_open(); pool_addr(0, 0); RET_V(dig_handle); }

void N_AIL_uninstall_DIG_driver(void)
{
    if (amx) SDL_LockMutex(amx);
    for (int i = 0; i < NSAMP; i++) samp[i].used = 0;
    if (amx) SDL_UnlockMutex(amx);
    RET0();
}

/* AIL_allocate_sample_handle(dig) -> HSAMPLE */
void N_AIL_allocate_sample_handle(void)
{
    SDL_LockMutex(amx);
    for (int i = 0; i < NSAMP; i++)
        if (!samp[i].used) {
            samp_init(&samp[i]);
            SDL_UnlockMutex(amx);
            RET_V(pool_addr(0, i));
        }
    SDL_UnlockMutex(amx);
    RET_V(0);
}

void N_AIL_release_sample_handle(void)
{
    Samp *s = S_arg(0, NULL);
    if (s) { SDL_LockMutex(amx); s->used = 0; s->status = SMP_FREE; SDL_UnlockMutex(amx); }
    RET0();
}

void N_AIL_init_sample(void)
{
    Samp *s = S_arg(0, NULL);
    if (s) { SDL_LockMutex(amx); samp_init(s); SDL_UnlockMutex(amx); }
    RET0();
}

/* parse a .WAV or .VOC file image in guest memory into s */
static int load_file(Samp *s, u32 img)
{
    if (RD32(img) == 0x46464952 && RD32(img + 8) == 0x45564157) {   /* RIFF WAVE */
        u32 end = img + 8 + RD32(img + 4), p = img + 12;
        int fmt_ok = 0;
        while (p + 8 <= end) {
            u32 id = RD32(p), n = RD32(p + 4);
            if (id == 0x20746d66) {   /* "fmt " */
                int ch = RD16(p + 10), bits = RD16(p + 22);
                s->rate = (int)RD32(p + 12);
                s->format = (bits == 16 ? 1 : 0) | (ch == 2 ? 2 : 0);
                s->sign8 = 0;
                fmt_ok = 1;
            } else if (id == 0x61746164 && fmt_ok) {   /* "data" */
                s->addr = p + 8;
                s->len = n;
                update_step(s);
                return 1;
            }
            p += 8 + n + (n & 1);
        }
        return 0;
    }
    if (!memcmp(MEM + img, "Creative Voice File", 19)) {
        u32 p = img + RD16(img + 20);
        while (RD8(p)) {
            u8 t = RD8(p);
            u32 n = RD32(p) >> 8;
            if (t == 1) {
                s->rate = 1000000 / (256 - RD8(p + 4));
                s->format = 0;
                s->sign8 = 0;
                s->addr = p + 6;
                s->len = n - 2;
                update_step(s);
                return 1;
            }
            p += 4 + n;
        }
    }
    return 0;
}

/* AIL_set_sample_file(S, file_image, block) -> 1 ok */
void N_AIL_set_sample_file(void)
{
    Samp *s = S_arg(0, NULL);
    int ok = 0;
    if (s) { SDL_LockMutex(amx); s->stream = 0; ok = load_file(s, ARG(1)); SDL_UnlockMutex(amx); }
    RET_V(ok);
}

/* AIL_allocate_file_sample(dig, file_image, block) -> HSAMPLE */
void N_AIL_allocate_file_sample(void)
{
    u32 img = ARG(1);
    SDL_LockMutex(amx);
    for (int i = 0; i < NSAMP; i++)
        if (!samp[i].used) {
            samp_init(&samp[i]);
            int ok = load_file(&samp[i], img);
            if (!ok) samp[i].used = 0;
            SDL_UnlockMutex(amx);
            RET_V(ok ? pool_addr(0, i) : 0);
        }
    SDL_UnlockMutex(amx);
    RET_V(0);
}

void N_AIL_set_sample_address(void)
{
    Samp *s = S_arg(0, NULL);
    if (s) { SDL_LockMutex(amx); s->stream = 0; s->addr = ARG(1); s->len = ARG(2); s->pos = 0; SDL_UnlockMutex(amx); }
    RET0();
}

/* AIL_set_sample_type(S, format, flags): DIG_PCM_SIGN = 1 */
void N_AIL_set_sample_type(void)
{
    Samp *s = S_arg(0, NULL);
    if (s) { s->format = (int)ARG(1) & 3; s->sign8 = ARG(2) & 1; }
    RET0();
}

void N_AIL_start_sample(void)
{
    Samp *s = S_arg(0, NULL);
    if (s) {
        SDL_LockMutex(amx);
        s->pos = 0;
        s->eos_pending = 0;
        s->status = SMP_PLAYING;
        SDL_UnlockMutex(amx);
    }
    RET0();
}

void N_AIL_stop_sample(void)
{
    Samp *s = S_arg(0, NULL);
    if (s && s->status == SMP_PLAYING) s->status = SMP_STOPPED;
    RET0();
}

void N_AIL_resume_sample(void)
{
    Samp *s = S_arg(0, NULL);
    if (s && s->status == SMP_STOPPED) s->status = SMP_PLAYING;
    RET0();
}

void N_AIL_end_sample(void)
{
    Samp *s = S_arg(0, NULL);
    if (s) {
        SDL_LockMutex(amx);
        if (s->status == SMP_PLAYING || s->status == SMP_STOPPED) s->status = SMP_DONE;
        s->bfull[0] = s->bfull[1] = 0;
        s->bend = 0;
        SDL_UnlockMutex(amx);
    }
    RET0();
}

void N_AIL_set_sample_playback_rate(void)
{
    Samp *s = S_arg(0, NULL);
    if (s) { s->rate = (int)ARG(1); update_step(s); }
    RET0();
}

void N_AIL_set_sample_volume(void) { Samp *s = S_arg(0, NULL); if (s) { int v = (int)ARG(1); s->vol = v < 0 ? 0 : v > 127 ? 127 : v; } RET0(); }
void N_AIL_set_sample_pan(void) { Samp *s = S_arg(0, NULL); if (s) { int v = (int)ARG(1); s->pan = v < 0 ? 0 : v > 127 ? 127 : v; } RET0(); }
void N_AIL_set_sample_loop_count(void) { Samp *s = S_arg(0, NULL); if (s) s->loops = (int)ARG(1); RET0(); }
void N_AIL_sample_status(void) { Samp *s = S_arg(0, NULL); RET_V(s ? (u32)s->status : SMP_FREE); }
void N_AIL_sample_playback_rate(void) { Samp *s = S_arg(0, NULL); RET_V(s ? s->rate : 0); }
void N_AIL_sample_volume(void) { Samp *s = S_arg(0, NULL); RET_V(s ? s->vol : 0); }
void N_AIL_sample_pan(void) { Samp *s = S_arg(0, NULL); RET_V(s ? s->pan : 64); }
void N_AIL_sample_loop_count(void) { Samp *s = S_arg(0, NULL); RET_V(s ? s->loops : 0); }
void N_AIL_set_digital_master_volume(void) { int v = (int)ARG(1); dig_master = v < 0 ? 0 : v > 127 ? 127 : v; RET0(); }
void N_AIL_digital_master_volume(void) { RET_V(dig_master); }

void N_AIL_set_sample_position(void)
{
    Samp *s = S_arg(0, NULL);
    if (s) s->pos = (u64)(ARG(1) / frame_bytes(s->format)) << 32;
    RET0();
}

void N_AIL_sample_position(void)
{
    Samp *s = S_arg(0, NULL);
    RET_V(s ? (u32)(s->pos >> 32) * frame_bytes(s->format) : 0);
}

/* AIL_minimum_sample_buffer_size(dig, rate, format) */
void N_AIL_minimum_sample_buffer_size(void) { RET_V(0x1000); }

/* AIL_sample_buffer_ready(S) -> buffer (0/1) the program may fill, or -1 */
void N_AIL_sample_buffer_ready(void)
{
    Samp *s = S_arg(0, NULL);
    int r = -1;
    if (s) {
        SDL_LockMutex(amx);
        if (!s->stream) { s->stream = 1; s->bfull[0] = s->bfull[1] = 0; s->bnext_fill = 0; s->bcur = 0; }
        if (!s->bfull[s->bnext_fill]) r = s->bnext_fill;
        if (getenv("AO_SNDLOG")) rt_log("snd %u: ready -> %d (full %d%d cur %d st %d pos %u)\n", SDL_GetTicks(), r, s->bfull[0], s->bfull[1], s->bcur, s->status, (u32)(s->pos >> 32));
        SDL_UnlockMutex(amx);
    }
    RET_V(r);
}

/* AIL_load_sample_buffer(S, buff_num, buffer, len): len 0 = end of stream */
void N_AIL_load_sample_buffer(void)
{
    Samp *s = S_arg(0, NULL);
    u32 n = ARG(1) & 1, buf = ARG(2), len = ARG(3);
    if (s) {
        SDL_LockMutex(amx);
        if (!s->stream) { s->stream = 1; s->bfull[0] = s->bfull[1] = 0; s->bcur = n; }
        if (!len) s->bend = 1;
        else {
            s->baddr[n] = buf;
            s->blen[n] = len;
            s->bfull[n] = 1;
            s->bend = 0;
        }
        s->bnext_fill = n ^ 1;
        if (getenv("AO_SNDLOG")) rt_log("snd %u: load %u buf %08x len %u\n", SDL_GetTicks(), n, buf, len);
        if (s->status != SMP_PLAYING && len) {
            s->bcur = n;
            s->pos = 0;
            s->status = SMP_PLAYING;
        }
        SDL_UnlockMutex(amx);
    }
    RET0();
}

static void set_cb(u32 *slot) { u32 old = *slot; *slot = ARG(1); RET_V(old); }
void N_AIL_register_SOB_callback(void) { Samp *s = S_arg(0, NULL); if (!s) RET_V(0); set_cb(&s->sob_cb); }
void N_AIL_register_EOB_callback(void) { Samp *s = S_arg(0, NULL); if (!s) RET_V(0); set_cb(&s->eob_cb); }
void N_AIL_register_EOS_callback(void) { Samp *s = S_arg(0, NULL); if (!s) RET_V(0); set_cb(&s->eos_cb); }
void N_AIL_set_sample_user_data(void) { Samp *s = S_arg(0, NULL); if (s && ARG(1) < 8) s->user[ARG(1)] = (s32)ARG(2); RET0(); }
void N_AIL_sample_user_data(void) { Samp *s = S_arg(0, NULL); RET_V(s && ARG(1) < 8 ? s->user[ARG(1)] : 0); }

void N_AIL_active_sample_count(void)
{
    int n = 0;
    for (int i = 0; i < NSAMP; i++) if (samp[i].used && samp[i].status == SMP_PLAYING) n++;
    RET_V(n);
}

/* ================= XMIDI ================= */
typedef struct {
    int used, status;
    u32 evnt, evnt_len;     /* guest address of the EVNT data */
    u32 rbrn;               /* guest address of RBRN chunk data (0 = none) */
    u32 pos;                /* offset in EVNT */
    double next_t;          /* time of the next event (seconds) */
    int vol, vol_to; double vol_t0, vol_ms; int vol_from;
    int tempo;              /* percent */
    int loops;              /* sequence loop count (0 = forever) */
    struct { u32 pos; int count; } fors[4];
    int nfor;
    u32 trig_cb, beat_cb, prefix_cb, seq_cb, event_cb, timbre_cb;
    s32 user[8];
    u8 chmap[16];
    u8 cc_vol[16];
    int beat, measure;
    /* pending note-offs */
    struct { double t; u8 ch, note; } offs[256];
    int noffs;
} Seq;

static Seq seq[NSEQ];
static HMIDIOUT mout;
static int midi_tried;
static int xmi_master = 127;
static u8 locked_ch[16];

static int seq_index(u32 h)
{
    u32 b = pool_addr(1, 0);
    if (h < b || h >= b + NSEQ * 0x40 || (h - b) % 0x40) return -1;
    return (int)((h - b) / 0x40);
}

static Seq *Q_arg(int n)
{
    int i = seq_index(ARG(n));
    return i >= 0 ? &seq[i] : NULL;
}

static double now_s(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

static void midi_open(void)
{
    if (midi_tried) return;
    midi_tried = 1;
    if (getenv("AO_HIDDEN")) return;   /* silent test runs */
    if (midiOutOpen(&mout, MIDI_MAPPER, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        rt_log("music: no MIDI output\n");
        mout = NULL;
        return;
    }
    rt_log("music: Windows MIDI synthesizer\n");
}

static u32 midi_msgs;
static void out3(u8 a, u8 b, u8 c) { midi_msgs++; if (mout) midiOutShortMsg(mout, a | (b << 8) | (c << 16)); }

static int cur_vol(Seq *q)
{
    if (q->vol_to < 0) return q->vol;
    double t = (now_s() - q->vol_t0) * 1000.0;
    if (t >= q->vol_ms) { q->vol = q->vol_to; q->vol_to = -1; return q->vol; }
    return q->vol_from + (int)((q->vol_to - q->vol_from) * t / q->vol_ms);
}

static void send_vol(Seq *q, int ch)
{
    int v = q->cc_vol[ch] * cur_vol(q) / 127 * xmi_master / 127;
    out3(0xb0 | q->chmap[ch], 7, (u8)v);
}

static void seq_notes_off(Seq *q)
{
    for (int i = 0; i < q->noffs; i++) out3(0x80 | q->offs[i].ch, q->offs[i].note, 0);
    q->noffs = 0;
    for (int ch = 0; ch < 16; ch++) { out3(0xb0 | q->chmap[ch], 123, 0); out3(0xb0 | q->chmap[ch], 64, 0); }
}

static u32 vlq(u32 a, u32 *p)
{
    u32 v = 0;
    for (int i = 0; i < 4; i++) {
        u8 b = RD8(a + (*p)++);
        v = (v << 7) | (b & 0x7f);
        if (!(b & 0x80)) break;
    }
    return v;
}

/* find sequence number n (FORM XMID) in an XMIDI file image */
static int find_xmid(u32 img, int n, u32 *evnt, u32 *elen, u32 *rbrn)
{
    u32 p = img, end;
    if (RD32(p) == 0x4d524f46 && RD32(p + 8) == 0x52494458) {   /* FORM XDIR */
        u32 l = __builtin_bswap32(RD32(p + 4));
        p += 8 + l + (l & 1);
    }
    if (RD32(p) == 0x20544143) {   /* "CAT " */
        end = p + 8 + __builtin_bswap32(RD32(p + 4));
        p += 12;                    /* skip "CAT ", size, "XMID" */
    } else end = p + 8 + __builtin_bswap32(RD32(p + 4));
    int k = 0;
    while (p + 12 <= end) {
        u32 id = RD32(p), l = __builtin_bswap32(RD32(p + 4));
        if (id == 0x4d524f46 && RD32(p + 8) == 0x44494d58) {   /* FORM XMID */
            if (k == n) {
                u32 q = p + 12, fe = p + 8 + l;
                *rbrn = 0;
                while (q + 8 <= fe) {
                    u32 cid = RD32(q), cl = __builtin_bswap32(RD32(q + 4));
                    if (cid == 0x4e524252) *rbrn = q + 8;          /* RBRN */
                    if (cid == 0x544e5645) { *evnt = q + 8; *elen = cl; return 1; }   /* EVNT */
                    q += 8 + cl + (cl & 1);
                }
                return 0;
            }
            k++;
        }
        p += 8 + l + (l & 1);
    }
    return 0;
}

static void seq_rewind(Seq *q)
{
    q->pos = 0;
    q->nfor = 0;
    q->beat = q->measure = 0;
    q->next_t = now_s();
    for (int ch = 0; ch < 16; ch++) q->cc_vol[ch] = 100;
}

/* run sequence events due by time t; called from the main thread */
static void seq_run(Seq *q, int qi, double t)
{
    double tick = 1.0 / 120.0 * 100.0 / (q->tempo > 0 ? q->tempo : 100);
    for (int i = 0; i < q->noffs; ) {
        if (q->offs[i].t <= t) { out3(0x80 | q->offs[i].ch, q->offs[i].note, 0); q->offs[i] = q->offs[--q->noffs]; }
        else i++;
    }
    int guard = 0;
    while (q->status == SEQ_PLAYING && q->next_t <= t && guard++ < 4096) {
        if (q->pos >= q->evnt_len) goto end_of_seq;
        u8 b = RD8(q->evnt + q->pos);
        if (b < 0x80) {   /* interval: sum of bytes < 0x80 */
            u32 d = 0;
            while (q->pos < q->evnt_len && (b = RD8(q->evnt + q->pos)) < 0x80) { d += b; q->pos++; }
            q->next_t += d * tick;
            continue;
        }
        q->pos++;
        u8 hi = b & 0xf0, ch = b & 15, pch = q->chmap[ch];
        if (hi == 0x90) {
            u8 note = RD8(q->evnt + q->pos++), vel = RD8(q->evnt + q->pos++);
            u32 dur = vlq(q->evnt, &q->pos);
            out3(0x90 | pch, note, vel);
            if (q->noffs < 256) { q->offs[q->noffs].t = q->next_t + dur * tick; q->offs[q->noffs].ch = pch; q->offs[q->noffs].note = note; q->noffs++; }
        } else if (hi == 0x80 || hi == 0xa0 || hi == 0xe0) {
            u8 d1 = RD8(q->evnt + q->pos++), d2 = RD8(q->evnt + q->pos++);
            out3(hi | pch, d1, d2);
        } else if (hi == 0xc0 || hi == 0xd0) {
            out3(hi | pch, RD8(q->evnt + q->pos++), 0);
        } else if (hi == 0xb0) {
            u8 cc = RD8(q->evnt + q->pos++), v = RD8(q->evnt + q->pos++);
            switch (cc) {
            case 7: q->cc_vol[ch] = v; send_vol(q, ch); break;
            case 116:   /* FOR loop */
                if (q->nfor < 4) { q->fors[q->nfor].pos = q->pos; q->fors[q->nfor].count = v; q->nfor++; }
                break;
            case 117:   /* NEXT / BREAK */
                if (q->nfor && v >= 64) {
                    int k = q->nfor - 1;
                    if (q->fors[k].count == 0 || --q->fors[k].count > 0) q->pos = q->fors[k].pos;
                    else q->nfor--;
                }
                break;
            case 119:   /* callback trigger */
                if (q->trig_cb) rt_guest_callback(q->trig_cb, pool_addr(1, qi), v);
                break;
            case 110: case 111: case 112: case 113: case 114: case 115: case 118: case 120:
                break;   /* AIL private controllers (120 = branch point marker) */
            default: out3(0xb0 | pch, cc, v); break;
            }
        } else if (b == 0xff) {
            u8 type = RD8(q->evnt + q->pos++);
            u32 l = vlq(q->evnt, &q->pos);
            q->pos += l;
            if (type == 0x2f) goto end_of_seq;
        } else if (b == 0xf0 || b == 0xf7) {
            u32 l = vlq(q->evnt, &q->pos);
            q->pos += l;
        }
        continue;
end_of_seq:
        if (q->loops == 0 || --q->loops > 0) { double nt = q->next_t; seq_rewind(q); q->next_t = nt; continue; }
        seq_notes_off(q);
        q->status = SEQ_DONE;
        if (q->seq_cb) rt_guest_callback(q->seq_cb, pool_addr(1, qi), 0);
        break;
    }
}

/* AIL_install_MDI_INI(HMDIDRIVER *mdi) -> 0 */
void N_AIL_install_MDI_INI(void)
{
    midi_open();
    pool_addr(0, 0);
    u32 p = ARG(0);
    if (p) WR32(p, mdi_handle);
    RET_V(0);
}

void N_AIL_install_MDI_driver(void) { midi_open(); pool_addr(0, 0); RET_V(mdi_handle); }

void N_AIL_uninstall_MDI_driver(void)
{
    for (int i = 0; i < NSEQ; i++) if (seq[i].used) { seq_notes_off(&seq[i]); seq[i].used = 0; }
    RET0();
}

/* MDIDRVRTYPE_GM = 0: the programs then use their General MIDI arrangements */
void N_AIL_MDI_driver_type(void) { RET_V(0); }
void N_AIL_nop(void) { RET0(); }
void N_AIL_ret1(void) { RET_V(1); }
void N_AIL_ret0(void) { RET_V(0); }

void N_AIL_allocate_sequence_handle(void)
{
    for (int i = 0; i < NSEQ; i++)
        if (!seq[i].used) {
            memset(&seq[i], 0, sizeof seq[i]);
            seq[i].used = 1;
            seq[i].status = SEQ_DONE;
            seq[i].vol = 127;
            seq[i].vol_to = -1;
            seq[i].tempo = 100;
            seq[i].loops = 1;
            for (int ch = 0; ch < 16; ch++) seq[i].chmap[ch] = (u8)ch;
            RET_V(pool_addr(1, i));
        }
    RET_V(0);
}

void N_AIL_release_sequence_handle(void)
{
    Seq *q = Q_arg(0);
    if (q) { seq_notes_off(q); q->used = 0; }
    RET0();
}

/* AIL_init_sequence(S, start, sequence_num) -> 1 ok, 0 failed */
void N_AIL_init_sequence(void)
{
    Seq *q = Q_arg(0);
    if (!q) RET_V(0);
    if (q->status == SEQ_PLAYING) seq_notes_off(q);
    u32 ev, el, rb;
    if (!find_xmid(ARG(1), (int)ARG(2), &ev, &el, &rb)) { rt_log("music: bad XMIDI image %08x\n", ARG(1)); RET_V(0); }
    q->evnt = ev;
    q->evnt_len = el;
    q->rbrn = rb;
    q->status = SEQ_STOPPED;
    q->loops = 1;
    q->tempo = 100;
    rt_log("music: sequence %d of %08x, %u bytes of events%s\n", (int)ARG(2), ARG(1), el, rb ? ", branch points" : "");
    for (int ch = 0; ch < 16; ch++) q->chmap[ch] = (u8)ch;
    seq_rewind(q);
    RET_V(1);
}

void N_AIL_start_sequence(void)
{
    Seq *q = Q_arg(0);
    if (q && q->evnt) {
        midi_open();
        seq_notes_off(q);
        seq_rewind(q);
        q->status = SEQ_PLAYING;
        for (int ch = 0; ch < 16; ch++) send_vol(q, ch);
    }
    RET0();
}

static double stop_at[NSEQ];
void N_AIL_stop_sequence(void)
{
    Seq *q = Q_arg(0);
    if (q && q->status == SEQ_PLAYING) { q->status = SEQ_STOPPED; stop_at[q - seq] = now_s(); seq_notes_off(q); }
    RET0();
}

void N_AIL_resume_sequence(void)
{
    Seq *q = Q_arg(0);
    if (q && q->status == SEQ_STOPPED && q->evnt) {
        q->next_t += now_s() - (stop_at[q - seq] ? stop_at[q - seq] : now_s());
        q->status = SEQ_PLAYING;
    }
    RET0();
}

void N_AIL_end_sequence(void)
{
    Seq *q = Q_arg(0);
    if (q && (q->status == SEQ_PLAYING || q->status == SEQ_STOPPED)) { seq_notes_off(q); q->status = SEQ_DONE; }
    RET0();
}

void N_AIL_set_sequence_tempo(void) { Seq *q = Q_arg(0); if (q) q->tempo = (int)ARG(1); RET0(); }

void N_AIL_set_sequence_volume(void)
{
    Seq *q = Q_arg(0);
    if (q) {
        int v = (int)ARG(1), ms = (int)ARG(2);
        v = v < 0 ? 0 : v > 127 ? 127 : v;
        if (ms <= 0) { q->vol = v; q->vol_to = -1; }
        else { q->vol_from = cur_vol(q); q->vol_to = v; q->vol_t0 = now_s(); q->vol_ms = ms; }
        for (int ch = 0; ch < 16; ch++) send_vol(q, ch);
    }
    RET0();
}

void N_AIL_set_sequence_loop_count(void) { Seq *q = Q_arg(0); if (q) q->loops = (int)ARG(1); RET0(); }
void N_AIL_sequence_status(void) { Seq *q = Q_arg(0); RET_V(q ? (u32)q->status : SEQ_FREE); }
void N_AIL_sequence_tempo(void) { Seq *q = Q_arg(0); RET_V(q ? q->tempo : 100); }
void N_AIL_sequence_volume(void) { Seq *q = Q_arg(0); RET_V(q ? cur_vol(q) : 0); }
void N_AIL_sequence_loop_count(void) { Seq *q = Q_arg(0); RET_V(q ? q->loops : 0); }

void N_AIL_set_XMIDI_master_volume(void)
{
    int v = (int)ARG(1);
    xmi_master = v < 0 ? 0 : v > 127 ? 127 : v;
    for (int i = 0; i < NSEQ; i++) if (seq[i].used && seq[i].status == SEQ_PLAYING) for (int ch = 0; ch < 16; ch++) send_vol(&seq[i], ch);
    RET0();
}

void N_AIL_XMIDI_master_volume(void) { RET_V(xmi_master); }

void N_AIL_active_sequence_count(void)
{
    int n = 0;
    for (int i = 0; i < NSEQ; i++) if (seq[i].used && seq[i].status == SEQ_PLAYING) n++;
    RET_V(n);
}

/* AIL_sequence_position(S, S32 *beat, S32 *measure) */
void N_AIL_sequence_position(void)
{
    Seq *q = Q_arg(0);
    if (ARG(1)) WR32(ARG(1), q ? q->beat : 0);
    if (ARG(2)) WR32(ARG(2), q ? q->measure : 0);
    RET0();
}

/* AIL_branch_index(S, marker): continue at the RBRN entry with that id */
void N_AIL_branch_index(void)
{
    Seq *q = Q_arg(0);
    u32 marker = ARG(1);
    if (q && q->rbrn) {
        u32 n = RD16(q->rbrn);
        for (u32 i = 0; i < n; i++) {
            u32 e = q->rbrn + 2 + i * 6;
            if (RD16(e) == marker) {
                seq_notes_off(q);
                q->pos = RD32(e + 2);
                q->nfor = 0;
                q->next_t = now_s();
                break;
            }
        }
    }
    RET0();
}

static void set_qcb(u32 *slot) { u32 old = *slot; *slot = ARG(1); RET_V(old); }
void N_AIL_register_trigger_callback(void) { Seq *q = Q_arg(0); if (!q) RET_V(0); set_qcb(&q->trig_cb); }
void N_AIL_register_sequence_callback(void) { Seq *q = Q_arg(0); if (!q) RET_V(0); set_qcb(&q->seq_cb); }
void N_AIL_register_beat_callback(void) { Seq *q = Q_arg(0); if (!q) RET_V(0); set_qcb(&q->beat_cb); }
void N_AIL_register_prefix_callback(void) { Seq *q = Q_arg(0); if (!q) RET_V(0); set_qcb(&q->prefix_cb); }
/* event / timbre callbacks are per driver: ARG(0) = mdi */
static u32 mdi_event_cb, mdi_timbre_cb;
void N_AIL_register_event_callback(void) { u32 o = mdi_event_cb; mdi_event_cb = ARG(1); RET_V(o); }
void N_AIL_register_timbre_callback(void) { u32 o = mdi_timbre_cb; mdi_timbre_cb = ARG(1); RET_V(o); }
void N_AIL_set_sequence_user_data(void) { Seq *q = Q_arg(0); if (q && ARG(1) < 8) q->user[ARG(1)] = (s32)ARG(2); RET0(); }
void N_AIL_sequence_user_data(void) { Seq *q = Q_arg(0); RET_V(q && ARG(1) < 8 ? q->user[ARG(1)] : 0); }

/* AIL_lock_channel(mdi) -> physical channel 1..16 for the program's own notes */
void N_AIL_lock_channel(void)
{
    for (int ch = 8; ch >= 1; ch--) if (!locked_ch[ch] && ch != 9) { locked_ch[ch] = 1; RET_V(ch + 1); }
    RET_V(0);
}

void N_AIL_release_channel(void)
{
    u32 ch = ARG(1);
    if (ch >= 1 && ch <= 16) { locked_ch[ch - 1] = 0; out3(0xb0 | (ch - 1), 123, 0); }
    RET0();
}

/* AIL_map_sequence_channel(S, seq_channel, phys_channel) - channels 1..16 */
void N_AIL_map_sequence_channel(void)
{
    Seq *q = Q_arg(0);
    u32 sc = ARG(1), pc = ARG(2);
    if (q && sc >= 1 && sc <= 16 && pc >= 1 && pc <= 16) q->chmap[sc - 1] = (u8)(pc - 1);
    RET0();
}

void N_AIL_true_sequence_channel(void)
{
    Seq *q = Q_arg(0);
    u32 sc = ARG(1);
    RET_V(q && sc >= 1 && sc <= 16 ? q->chmap[sc - 1] + 1 : sc);
}

/* AIL_send_channel_voice_message(mdi, S, status, data_1, data_2) */
void N_AIL_send_channel_voice_message(void)
{
    Seq *q = seq_index(ARG(1)) >= 0 ? &seq[seq_index(ARG(1))] : NULL;
    u8 st = (u8)ARG(2), ch = st & 15;
    if (q) ch = q->chmap[ch];
    midi_open();
    out3((st & 0xf0) | ch, (u8)ARG(3), (u8)ARG(4));
    RET0();
}

/* AIL_send_sysex_message(mdi, buffer) */
void N_AIL_send_sysex_message(void)
{
    u32 b = ARG(1);
    if (mout && RD8(b) == 0xf0) {
        static u8 buf[512];
        u32 n = 0;
        while (n < sizeof buf) { buf[n] = RD8(b + n); if (buf[n++] == 0xf7) break; }
        MIDIHDR h = {0};
        h.lpData = (LPSTR)buf;
        h.dwBufferLength = n;
        if (midiOutPrepareHeader(mout, &h, sizeof h) == MMSYSERR_NOERROR) {
            midiOutLongMsg(mout, &h, sizeof h);
            while (midiOutUnprepareHeader(mout, &h, sizeof h) == MIDIERR_STILLPLAYING) Sleep(1);
        }
    }
    RET0();
}

/* AIL_controller_value(S, channel, controller) */
void N_AIL_controller_value(void)
{
    Seq *q = Q_arg(0);
    u32 ch = ARG(1), cc = ARG(2);
    RET_V(q && cc == 7 && ch >= 1 && ch <= 16 ? q->cc_vol[ch - 1] : 0);
}

/* ================= main-thread service ================= */
/* called from the interrupt poll: XMIDI timing and sample callbacks */
void sound_poll(void)
{
    double t = now_s();
    {
        static double last_log;
        if (getenv("AO_MIDILOG") && t - last_log > 5) { last_log = t; rt_log("music: %u MIDI messages so far\n", midi_msgs); }
    }
    for (int i = 0; i < NSEQ; i++)
        if (seq[i].used) {
            if (seq[i].vol_to >= 0) for (int ch = 0; ch < 16; ch++) send_vol(&seq[i], ch);
            if (seq[i].status == SEQ_PLAYING) seq_run(&seq[i], i, t);
        }
    for (int i = 0; i < NSAMP; i++) {
        Samp *s = &samp[i];
        if (!s->used) continue;
        if (s->eob_pending) { s->eob_pending = 0; if (s->eob_cb) rt_guest_callback(s->eob_cb, pool_addr(0, i), 0); }
        if (s->eos_pending) { s->eos_pending = 0; if (s->eos_cb) rt_guest_callback(s->eos_cb, pool_addr(0, i), 0); }
    }
}

/* a new program starts: silence, forget all handles */
void sound_reset(void)
{
    if (amx) SDL_LockMutex(amx);
    memset(samp, 0, sizeof samp);
    if (amx) SDL_UnlockMutex(amx);
    for (int i = 0; i < NSEQ; i++) if (seq[i].used) seq_notes_off(&seq[i]);
    memset(seq, 0, sizeof seq);
    memset(locked_ch, 0, sizeof locked_ch);
    if (mout) for (int ch = 0; ch < 16; ch++) { out3(0xb0 | ch, 121, 0); out3(0xb0 | ch, 123, 0); }
    handle_pool = 0;
    dig_master = xmi_master = 127;
    mdi_event_cb = mdi_timbre_cb = 0;
}
