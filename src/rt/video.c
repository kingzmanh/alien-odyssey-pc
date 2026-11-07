/* Display: a Windows window (SDL2 for the window and input) showing the
 * game's 8-bit screen through Direct3D 9.
 *
 * The game draws every pixel itself at 320x200 (its software 3D renderer
 * and the FMV decoders), so what can be improved is how that picture is
 * brought to the screen.  Each frame: palette -> 32-bit texture, optional
 * FXAA at the game's resolution, upscale to the output size with the chosen
 * filter (nearest, bilinear, Catmull-Rom bicubic, or sharp bilinear), then
 * optional contrast-adaptive sharpening at output resolution - the same
 * post chain as the Xbox ports.  Options in odyssey_pc.ini [video], hotkeys
 * F9 filter, F10 sharpening, F11 FXAA, F12 aspect (shown on screen).
 *
 * VESA banked modes keep the full picture in a 4 MB frame buffer at guest
 * address VRAM_GUEST; the 64 KB window at A0000h holds the selected bank. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <string.h>
#include <stdlib.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include "rt.h"

#define VRAM_GUEST 0xe0000000u
#define VRAM_SIZE  (4u << 20)

int video_w, video_h, video_bank_cur;
u8 vga_pal[768];
static int video_bpp;
static SDL_Window *win;
static HWND hwnd;
static u32 pal32[256];
static int pal_dirty = 1;
static u64 last_present;
static u32 *rgb;                    /* converted frame */

extern int opt_fullscreen, opt_filter, opt_sharpen, opt_fxaa, opt_aspect, opt_vsync;
extern void settings_save_video(void);

/* ---------------- Direct3D 9 ---------------- */
static IDirect3D9 *d3d;
static IDirect3DDevice9 *dev;
static D3DPRESENT_PARAMETERS pp;
static IDirect3DTexture9 *src_tex;          /* game frame, video_w x video_h */
static IDirect3DTexture9 *aa_tex;           /* FXAA result at game resolution */
static IDirect3DSurface9 *aa_surf;
static IDirect3DTexture9 *up_tex;           /* upscaled picture at output size */
static IDirect3DSurface9 *up_surf;
static UINT src_w, src_h, up_w, up_h;
static IDirect3DVertexShader9 *vs_blit;
static IDirect3DPixelShader9 *ps_copy, *ps_bicubic, *ps_sharpbil, *ps_fxaa, *ps_sharpen;
static IDirect3DVertexDeclaration9 *blit_decl;
static int d3d_ok;

typedef HRESULT (WINAPI *D3DCompile_t)(const void *, SIZE_T, const char *, const void *, void *, const char *, const char *, UINT, UINT, void **, void **);
typedef struct { void **vt; } Blob;   /* ID3DBlob: QueryInterface, AddRef, Release, GetBufferPointer, GetBufferSize */

static void *compile(const char *src, const char *profile, SIZE_T *n)
{
    static D3DCompile_t comp;
    if (!comp) {
        HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
        if (!m) m = LoadLibraryA("d3dcompiler_43.dll");
        if (m) comp = (D3DCompile_t)(void *)GetProcAddress(m, "D3DCompile");
        if (!comp) { rt_log("video: no HLSL compiler (d3dcompiler_47.dll)\n"); return NULL; }
    }
    Blob *code = NULL, *err = NULL;
    if (FAILED(comp(src, strlen(src), "post", NULL, NULL, "main", profile, 0, 0, (void **)&code, (void **)&err))) {
        if (err) { rt_log("video: shader error: %s\n", (const char *)((void *(WINAPI *)(void *))err->vt[3])(err)); ((ULONG(WINAPI *)(void *))err->vt[2])(err); }
        return NULL;
    }
    if (err) ((ULONG(WINAPI *)(void *))err->vt[2])(err);
    *n = ((SIZE_T(WINAPI *)(void *))code->vt[4])(code);
    void *p = malloc(*n);
    memcpy(p, ((void *(WINAPI *)(void *))code->vt[3])(code), *n);
    ((ULONG(WINAPI *)(void *))code->vt[2])(code);
    return p;
}

static const char *VS_BLIT =
    "float4 hp : register(c0);\n"
    "struct V { float4 p : POSITION; float2 t : TEXCOORD0; };\n"
    "V main(float4 p : POSITION, float2 t : TEXCOORD0) { V o; o.p = float4(p.xy + hp.xy, 0, 1); o.t = t; return o; }\n";
static const char *PS_COPY =
    "sampler2D s : register(s0);\n"
    "float4 main(float2 t : TEXCOORD0) : COLOR { return float4(tex2D(s, t).rgb, 1); }\n";
/* Catmull-Rom bicubic in 9 bilinear taps */
static const char *PS_BICUBIC =
    "sampler2D s : register(s0);\n"
    "float4 ts : register(c1);\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "  float2 sp = uv * ts.xy, t1 = floor(sp - 0.5) + 0.5, f = sp - t1;\n"
    "  float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
    "  float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);\n"
    "  float2 w12 = w1 + w2, o12 = w2 / w12;\n"
    "  float2 p0 = (t1 - 1) * ts.zw, p3 = (t1 + 2) * ts.zw, p12 = (t1 + o12) * ts.zw;\n"
    "  float3 r = 0;\n"
    "  r += tex2D(s, float2(p0.x, p0.y)).rgb * w0.x * w0.y;  r += tex2D(s, float2(p12.x, p0.y)).rgb * w12.x * w0.y;  r += tex2D(s, float2(p3.x, p0.y)).rgb * w3.x * w0.y;\n"
    "  r += tex2D(s, float2(p0.x, p12.y)).rgb * w0.x * w12.y; r += tex2D(s, float2(p12.x, p12.y)).rgb * w12.x * w12.y; r += tex2D(s, float2(p3.x, p12.y)).rgb * w3.x * w12.y;\n"
    "  r += tex2D(s, float2(p0.x, p3.y)).rgb * w0.x * w3.y;  r += tex2D(s, float2(p12.x, p3.y)).rgb * w12.x * w3.y;  r += tex2D(s, float2(p3.x, p3.y)).rgb * w3.x * w3.y;\n"
    "  return float4(saturate(r), 1);\n"
    "}\n";
/* sharp bilinear: nearest inside each source pixel, a one-output-pixel
   bilinear seam at its edges (crisp pixels without uneven sizes) */
static const char *PS_SHARPBIL =
    "sampler2D s : register(s0);\n"
    "float4 ts : register(c1);\n"
    "float4 k : register(c2);   // xy = output pixels per source pixel\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "  float2 t = uv * ts.xy, fl = floor(t), c = t - fl - 0.5;\n"
    "  float2 r = 0.5 - 0.5 / k.xy;\n"
    "  float2 f = (c - clamp(c, -r, r)) * k.xy + 0.5;\n"
    "  return float4(tex2D(s, (fl + f) * ts.zw).rgb, 1);\n"
    "}\n";
static const char *PS_FXAA =
    "sampler2D s : register(s0);\n"
    "float4 ts : register(c1);\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "  float2 rcp = ts.zw;\n"
    "  float3 nw = tex2D(s, uv + float2(-1, -1) * rcp).rgb, ne = tex2D(s, uv + float2(1, -1) * rcp).rgb;\n"
    "  float3 sw = tex2D(s, uv + float2(-1, 1) * rcp).rgb, se = tex2D(s, uv + float2(1, 1) * rcp).rgb, m = tex2D(s, uv).rgb;\n"
    "  float3 L = float3(0.299, 0.587, 0.114);\n"
    "  float lnw = dot(nw, L), lne = dot(ne, L), lsw = dot(sw, L), lse = dot(se, L), lm = dot(m, L);\n"
    "  float lmin = min(lm, min(min(lnw, lne), min(lsw, lse))), lmax = max(lm, max(max(lnw, lne), max(lsw, lse)));\n"
    "  float2 dir = float2(-((lnw + lne) - (lsw + lse)), (lnw + lsw) - (lne + lse));\n"
    "  float red = max((lnw + lne + lsw + lse) * (0.25 / 8.0), 1.0 / 128.0);\n"
    "  float rmin = 1.0 / (min(abs(dir.x), abs(dir.y)) + red);\n"
    "  dir = clamp(dir * rmin, -8.0, 8.0) * rcp;\n"
    "  float3 a = 0.5 * (tex2D(s, uv + dir * (1.0 / 3.0 - 0.5)).rgb + tex2D(s, uv + dir * (2.0 / 3.0 - 0.5)).rgb);\n"
    "  float3 b = a * 0.5 + 0.25 * (tex2D(s, uv - dir * 0.5).rgb + tex2D(s, uv + dir * 0.5).rgb);\n"
    "  float lb = dot(b, L);\n"
    "  return float4((lb < lmin || lb > lmax) ? a : b, 1);\n"
    "}\n";
/* contrast-adaptive sharpening (own implementation of the CAS idea) */
static const char *PS_SHARPEN =
    "sampler2D s : register(s0);\n"
    "float4 ts : register(c1);\n"
    "float4 k : register(c2);   // x = sharpness 0..1\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "  float2 d = ts.zw;\n"
    "  float3 c = tex2D(s, uv).rgb, n = tex2D(s, uv + float2(0, -d.y)).rgb, so = tex2D(s, uv + float2(0, d.y)).rgb;\n"
    "  float3 e = tex2D(s, uv + float2(d.x, 0)).rgb, w = tex2D(s, uv + float2(-d.x, 0)).rgb;\n"
    "  float3 mn = min(c, min(min(n, so), min(e, w))), mx = max(c, max(max(n, so), max(e, w)));\n"
    "  float3 amp = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 1e-5)));\n"
    "  float3 wt = -amp / lerp(8.0, 5.0, k.x);\n"
    "  return float4(saturate((c + (n + so + e + w) * wt) / (1.0 + 4.0 * wt)), 1);\n"
    "}\n";

static IDirect3DPixelShader9 *mk_ps(const char *src)
{
    SIZE_T n;
    void *bc = compile(src, "ps_3_0", &n);
    IDirect3DPixelShader9 *ps = NULL;
    if (bc) { IDirect3DDevice9_CreatePixelShader(dev, (const DWORD *)bc, &ps); free(bc); }
    return ps;
}

static void release_targets(void)
{
    if (src_tex) { IDirect3DTexture9_Release(src_tex); src_tex = NULL; }
    if (aa_surf) { IDirect3DSurface9_Release(aa_surf); aa_surf = NULL; }
    if (aa_tex) { IDirect3DTexture9_Release(aa_tex); aa_tex = NULL; }
    if (up_surf) { IDirect3DSurface9_Release(up_surf); up_surf = NULL; }
    if (up_tex) { IDirect3DTexture9_Release(up_tex); up_tex = NULL; }
    src_w = src_h = up_w = up_h = 0;
}

static void desktop_size(UINT *w, UINT *h)
{
    /* back buffer as large as the biggest screen: resizing and fullscreen
       never need a device reset (Present takes the client-sized part) */
    *w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    *h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (*w < 640) *w = 640;
    if (*h < 480) *h = 480;
}

static int d3d_init(void)
{
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) return 0;
    memset(&pp, 0, sizeof pp);
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_COPY;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    desktop_size(&pp.BackBufferWidth, &pp.BackBufferHeight);
    pp.hDeviceWindow = hwnd;
    pp.PresentationInterval = opt_vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
    if (FAILED(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                       D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &dev)) &&
        FAILED(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                       D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &dev))) {
        IDirect3D9_Release(d3d);
        d3d = NULL;
        return 0;
    }
    SIZE_T n;
    void *bc = compile(VS_BLIT, "vs_3_0", &n);
    if (bc) { IDirect3DDevice9_CreateVertexShader(dev, (const DWORD *)bc, &vs_blit); free(bc); }
    ps_copy = mk_ps(PS_COPY);
    ps_bicubic = mk_ps(PS_BICUBIC);
    ps_sharpbil = mk_ps(PS_SHARPBIL);
    ps_fxaa = mk_ps(PS_FXAA);
    ps_sharpen = mk_ps(PS_SHARPEN);
    D3DVERTEXELEMENT9 el[] = {{0, 0, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_POSITION, 0},
                              {0, 8, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0}, D3DDECL_END()};
    IDirect3DDevice9_CreateVertexDeclaration(dev, el, &blit_decl);
    if (!vs_blit || !ps_copy) { rt_log("video: post shaders unavailable, plain scaling only\n"); }
    rt_log("video: Direct3D 9, back buffer %ux%u\n", pp.BackBufferWidth, pp.BackBufferHeight);
    return 1;
}

static int ensure_targets(UINT sw, UINT sh, UINT ow, UINT oh)
{
    if (sw != src_w || sh != src_h) {
        if (src_tex) { IDirect3DTexture9_Release(src_tex); src_tex = NULL; }
        if (aa_surf) { IDirect3DSurface9_Release(aa_surf); aa_surf = NULL; }
        if (aa_tex) { IDirect3DTexture9_Release(aa_tex); aa_tex = NULL; }
        if (FAILED(IDirect3DDevice9_CreateTexture(dev, sw, sh, 1, D3DUSAGE_DYNAMIC, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &src_tex, NULL))) return 0;
        if (SUCCEEDED(IDirect3DDevice9_CreateTexture(dev, sw, sh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &aa_tex, NULL)))
            IDirect3DTexture9_GetSurfaceLevel(aa_tex, 0, &aa_surf);
        src_w = sw; src_h = sh;
    }
    if (ow != up_w || oh != up_h) {
        if (up_surf) { IDirect3DSurface9_Release(up_surf); up_surf = NULL; }
        if (up_tex) { IDirect3DTexture9_Release(up_tex); up_tex = NULL; }
        if (SUCCEEDED(IDirect3DDevice9_CreateTexture(dev, ow, oh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &up_tex, NULL)))
            IDirect3DTexture9_GetSurfaceLevel(up_tex, 0, &up_surf);
        up_w = ow; up_h = oh;
    }
    return 1;
}

/* draw texture src (sw x sh) into the viewport of the current target with ps */
static void blit(IDirect3DTexture9 *src, UINT sw, UINT sh, IDirect3DPixelShader9 *ps, D3DVIEWPORT9 vp, int linear)
{
    IDirect3DDevice9_SetViewport(dev, &vp);
    float hp[4] = {-1.0f / vp.Width, 1.0f / vp.Height, 0, 0};
    float ts[4] = {(float)sw, (float)sh, 1.0f / sw, 1.0f / sh};
    IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, hp, 1);
    IDirect3DDevice9_SetPixelShaderConstantF(dev, 1, ts, 1);
    IDirect3DDevice9_SetPixelShader(dev, ps);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)src);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    float q[4][4] = {{-1, 1, 0, 0}, {1, 1, 1, 0}, {-1, -1, 0, 1}, {1, -1, 1, 1}};
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, 16);
}

/* where the picture goes in a cw x ch client area */
static void dest_rect(UINT cw, UINT ch, UINT *dx, UINT *dy, UINT *dw, UINT *dh)
{
    UINT sw = video_w ? (UINT)video_w : 320, sh = video_h ? (UINT)video_h : 200;
    /* the 320x200 modes were shown at 4:3 on a CRT (non-square pixels) */
    UINT aw = 4, ah = 3;
    if (sw * 3 == sh * 4) { aw = 4; ah = 3; }
    *dx = 0; *dy = 0; *dw = cw; *dh = ch;
    if (opt_aspect == 0) {          /* 4:3, as large as fits */
        if (cw * ah > ch * aw) { *dw = ch * aw / ah; *dx = (cw - *dw) / 2; }
        else { *dh = cw * ah / aw; *dy = (ch - *dh) / 2; }
    } else if (opt_aspect == 2) {   /* whole multiples of the game's pixels (square pixels) */
        UINT k = cw / sw < ch / sh ? cw / sw : ch / sh;
        if (k < 1) k = 1;
        *dw = sw * k; *dh = sh * k;
        if (*dw > cw) *dw = cw;
        if (*dh > ch) *dh = ch;
        *dx = (cw - *dw) / 2; *dy = (ch - *dh) / 2;
    }                               /* 1: stretch to the window */
    if (!*dw) *dw = 1;
    if (!*dh) *dh = 1;
}

/* on-screen note after an option hotkey (drawn with GDI on the back buffer) */
static char osd[96];
static u64 osd_until;
static void osd_show(const char *s) { snprintf(osd, sizeof osd, "%s", s); osd_until = SDL_GetTicks64() + 2500; rt_log("video: %s\n", s); }

static void draw_osd(IDirect3DSurface9 *bb, UINT ch)
{
    if (!osd_until || SDL_GetTicks64() > osd_until) { osd_until = 0; return; }
    HDC dc;
    if (FAILED(IDirect3DSurface9_GetDC(bb, &dc))) return;
    int px = (int)ch / 24 < 18 ? 18 : (int)ch / 24;
    HFONT f = CreateFontA(-px, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
    HGDIOBJ of = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0, 0, 0));
    TextOutA(dc, px + 2, px + 2, osd, (int)strlen(osd));
    SetTextColor(dc, RGB(255, 230, 120));
    TextOutA(dc, px, px, osd, (int)strlen(osd));
    SelectObject(dc, of);
    DeleteObject(f);
    IDirect3DSurface9_ReleaseDC(bb, dc);
}

static void d3d_present(const u32 *pix, int w, int h)
{
    HRESULT hr = IDirect3DDevice9_TestCooperativeLevel(dev);
    if (hr == D3DERR_DEVICELOST) return;
    if (hr == D3DERR_DEVICENOTRESET) {
        release_targets();
        desktop_size(&pp.BackBufferWidth, &pp.BackBufferHeight);
        if (FAILED(IDirect3DDevice9_Reset(dev, &pp))) return;
    }
    RECT cr;
    GetClientRect(hwnd, &cr);
    UINT cw = cr.right > 0 ? (UINT)cr.right : 1, ch = cr.bottom > 0 ? (UINT)cr.bottom : 1;
    if (cw > pp.BackBufferWidth || ch > pp.BackBufferHeight) {   /* a bigger screen appeared */
        release_targets();
        desktop_size(&pp.BackBufferWidth, &pp.BackBufferHeight);
        if (cw > pp.BackBufferWidth) pp.BackBufferWidth = cw;
        if (ch > pp.BackBufferHeight) pp.BackBufferHeight = ch;
        if (FAILED(IDirect3DDevice9_Reset(dev, &pp))) return;
    }
    UINT dx, dy, dw, dh;
    dest_rect(cw, ch, &dx, &dy, &dw, &dh);
    if (!ensure_targets((UINT)w, (UINT)h, dw, dh)) return;
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(IDirect3DTexture9_LockRect(src_tex, 0, &lr, NULL, D3DLOCK_DISCARD))) {
        for (int y = 0; y < h; y++) memcpy((u8 *)lr.pBits + y * lr.Pitch, pix + y * w, w * 4);
        IDirect3DTexture9_UnlockRect(src_tex, 0);
    }
    IDirect3DSurface9 *bb = NULL;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    IDirect3DDevice9_BeginScene(dev);
    int shaders = vs_blit && ps_copy && up_surf;
    if (shaders) {
        IDirect3DDevice9_SetVertexShader(dev, vs_blit);
        IDirect3DDevice9_SetVertexDeclaration(dev, blit_decl);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        IDirect3DTexture9 *cur = src_tex;
        D3DVIEWPORT9 vs = {0, 0, (DWORD)w, (DWORD)h, 0, 1};
        if (opt_fxaa && ps_fxaa && aa_surf) {
            IDirect3DDevice9_SetRenderTarget(dev, 0, aa_surf);
            blit(cur, w, h, ps_fxaa, vs, 1);
            cur = aa_tex;
        }
        /* upscale into the output-sized target */
        IDirect3DDevice9_SetRenderTarget(dev, 0, up_surf);
        D3DVIEWPORT9 vu = {0, 0, dw, dh, 0, 1};
        if (opt_filter == 2 && ps_bicubic) blit(cur, w, h, ps_bicubic, vu, 1);
        else if (opt_filter == 3 && ps_sharpbil) {
            float k[4] = {(float)dw / w, (float)dh / h, 0, 0};
            IDirect3DDevice9_SetPixelShaderConstantF(dev, 2, k, 1);
            blit(cur, w, h, ps_sharpbil, vu, 1);
        } else blit(cur, w, h, ps_copy, vu, opt_filter != 0);
        /* sharpen at output resolution, straight into the back buffer */
        IDirect3DDevice9_SetRenderTarget(dev, 0, bb);
        D3DRECT all = {0, 0, (LONG)cw, (LONG)ch};
        IDirect3DDevice9_Clear(dev, 1, &all, D3DCLEAR_TARGET, 0, 1, 0);
        D3DVIEWPORT9 vd = {dx, dy, dw, dh, 0, 1};
        if (opt_sharpen && ps_sharpen) {
            float k[4] = {opt_sharpen == 1 ? 0.3f : opt_sharpen == 2 ? 0.6f : 1.0f, 0, 0, 0};
            IDirect3DDevice9_SetPixelShaderConstantF(dev, 2, k, 1);
            blit(up_tex, dw, dh, ps_sharpen, vd, 0);
        } else blit(up_tex, dw, dh, ps_copy, vd, 0);
    } else {
        D3DRECT all = {0, 0, (LONG)cw, (LONG)ch};
        IDirect3DDevice9_Clear(dev, 1, &all, D3DCLEAR_TARGET, 0, 1, 0);
        IDirect3DSurface9 *s = NULL;
        IDirect3DTexture9_GetSurfaceLevel(src_tex, 0, &s);
        RECT r = {(LONG)dx, (LONG)dy, (LONG)(dx + dw), (LONG)(dy + dh)};
        IDirect3DDevice9_StretchRect(dev, s, NULL, bb, &r, opt_filter ? D3DTEXF_LINEAR : D3DTEXF_POINT);
        IDirect3DSurface9_Release(s);
    }
    IDirect3DDevice9_EndScene(dev);
    draw_osd(bb, ch);
    IDirect3DSurface9_Release(bb);
    RECT sr = {0, 0, (LONG)cw, (LONG)ch};
    IDirect3DDevice9_Present(dev, &sr, NULL, NULL, NULL);
}

/* ---------------- window / modes ---------------- */
void video_init(void)
{
    if (!VirtualAlloc(MEM + VRAM_GUEST, VRAM_SIZE, MEM_COMMIT, PAGE_READWRITE)) rt_fatal("cannot commit video memory");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");        /* window icon from the exe */
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
    win = SDL_CreateWindow("Alien Odyssey", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           1280, 960, SDL_WINDOW_RESIZABLE | (getenv("AO_HIDDEN") ? SDL_WINDOW_HIDDEN :
                                                              opt_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!win) rt_fatal("SDL window: %s", SDL_GetError());
    SDL_SysWMinfo wi;
    SDL_VERSION(&wi.version);
    if (SDL_GetWindowWMInfo(win, &wi)) hwnd = wi.info.win.window;
    d3d_ok = hwnd && d3d_init();
    if (!d3d_ok) rt_fatal("Direct3D 9 is not available on this PC.");
    for (int i = 0; i < 256; i++) video_set_pal(i, i >> 2, i >> 2, i >> 2);
    SDL_ShowCursor(SDL_DISABLE);   /* the game draws its own pointer */
}

void video_set_mode(int w, int h, int bpp)
{
    video_w = w; video_h = h; video_bpp = bpp;
    video_bank_cur = 0;
    memset(MEM + VRAM_GUEST, 0, VRAM_SIZE);
    memset(MEM + VGA_WINDOW, 0, 0x10000);
    free(rgb);
    rgb = w ? malloc((size_t)w * h * 4) : NULL;
    video_present(1);
}

void video_bank(u32 bank)
{
    if (bank * 0x10000u >= VRAM_SIZE) return;
    if ((int)bank == video_bank_cur) return;
    memcpy(MEM + VRAM_GUEST + video_bank_cur * 0x10000u, MEM + VGA_WINDOW, 0x10000);
    memcpy(MEM + VGA_WINDOW, MEM + VRAM_GUEST + bank * 0x10000u, 0x10000);
    video_bank_cur = (int)bank;
}

void video_set_pal(int idx, int r, int g, int b)
{
    idx &= 255;
    vga_pal[idx * 3] = (u8)(r & 63);
    vga_pal[idx * 3 + 1] = (u8)(g & 63);
    vga_pal[idx * 3 + 2] = (u8)(b & 63);
    pal_dirty = 1;
}

/* option hotkeys (F9..F12), handled by the PC side, not passed to the game */
int video_hotkey(int sdl_scancode)
{
    static const char *fn[] = {"nearest", "bilinear", "bicubic", "sharp bilinear"};
    static const char *sn[] = {"off", "low", "medium", "high"};
    static const char *an[] = {"4:3", "stretch", "whole pixels"};
    char t[96];
    switch (sdl_scancode) {
    case SDL_SCANCODE_F9: opt_filter = (opt_filter + 1) % 4; snprintf(t, sizeof t, "Upscale filter: %s", fn[opt_filter]); break;
    case SDL_SCANCODE_F10: opt_sharpen = (opt_sharpen + 1) % 4; snprintf(t, sizeof t, "Sharpening: %s", sn[opt_sharpen]); break;
    case SDL_SCANCODE_F11: opt_fxaa = !opt_fxaa; snprintf(t, sizeof t, "FXAA: %s", opt_fxaa ? "on" : "off"); break;
    case SDL_SCANCODE_F12: opt_aspect = (opt_aspect + 1) % 3; snprintf(t, sizeof t, "Aspect: %s", an[opt_aspect]); break;
    default: return 0;
    }
    osd_show(t);
    settings_save_video();
    video_present(1);
    return 1;
}

/* window coordinates -> game screen coordinates */
void video_window_to_game(int wx, int wy, int *gx, int *gy)
{
    RECT cr;
    UINT dx, dy, dw, dh;
    if (!hwnd || !GetClientRect(hwnd, &cr) || !video_w) { *gx = wx; *gy = wy; return; }
    dest_rect(cr.right > 0 ? cr.right : 1, cr.bottom > 0 ? cr.bottom : 1, &dx, &dy, &dw, &dh);
    *gx = (int)(((s64)wx - dx) * video_w / dw);
    *gy = (int)(((s64)wy - dy) * video_h / dh);
}

void video_present(int force)
{
    u64 now = SDL_GetTicks64();
    if (!force && now - last_present < 15) return;
    last_present = now;
    if (pal_dirty) {
        for (int i = 0; i < 256; i++) {
            u32 r = vga_pal[i * 3], g = vga_pal[i * 3 + 1], b = vga_pal[i * 3 + 2];
            pal32[i] = 0xff000000u | ((r << 2 | r >> 4) << 16) | ((g << 2 | g >> 4) << 8) | (b << 2 | b >> 4);
        }
        pal_dirty = 0;
    }
    if (!video_w || !rgb) {
        if (d3d_ok && !force) {
            u32 black = 0xff000000u;
            d3d_present(&black, 1, 1);
        }
        return;
    }
    const u8 *src;
    if (video_w == 320) src = MEM + VGA_WINDOW;
    else {
        memcpy(MEM + VRAM_GUEST + video_bank_cur * 0x10000u, MEM + VGA_WINDOW, 0x10000);
        src = MEM + VRAM_GUEST;
    }
    {   /* AO_SHOTMEM=<hex addr>: show/shoot that guest buffer instead (debugging) */
        static const char *sm = (const char *)1;
        if (sm == (const char *)1) sm = getenv("AO_SHOTMEM");
        if (sm) src = MEM + (u32)strtoul(sm, NULL, 16);
    }
    {   /* AO_FPS: log how many distinct frames the game drew per second (testing) */
        static int on = -1, frames;
        static u32 last_hash;
        static u64 t0;
        if (on < 0) on = getenv("AO_FPS") != NULL;
        if (on) {
            u32 hh = 2166136261u;
            const u32 *p = (const u32 *)src;
            for (int i = 0; i < video_w * video_h / 4; i += 7) hh = (hh ^ p[i]) * 16777619u;
            if (hh != last_hash) { frames++; last_hash = hh; }
            if (now - t0 >= 5000) { if (t0) rt_log("fps: %.1f\n", frames * 1000.0 / (now - t0)); t0 = now; frames = 0; }
        }
    }
    int n = video_w * video_h;
    for (int i = 0; i < n; i++) rgb[i] = pal32[src[i]];
    if (d3d_ok) d3d_present(rgb, video_w, video_h);
    {   /* AO_SHOT=<ms>: save the game's screen to shots/ every <ms> (testing);
           AO_SHOTOUT=1 saves the final upscaled picture instead */
        static int shot_ms = -1;
        static u64 next_shot;
        static int shot_n;
        if (shot_ms < 0) { const char *e = getenv("AO_SHOT"); shot_ms = e ? atoi(e) : 0; }
        if (shot_ms > 0 && now >= next_shot) {
            next_shot = now + shot_ms;
            char fn[64];
            CreateDirectoryA("shots", NULL);
            snprintf(fn, sizeof fn, "shots/shot_%04d.bmp", shot_n++);
            if (getenv("AO_SHOTOUT") && d3d_ok && up_tex) {
                /* render target -> system memory copy of the upscaled frame (before sharpening) or the back buffer */
                IDirect3DSurface9 *bb = NULL, *sys = NULL;
                D3DSURFACE_DESC dd;
                if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
                    IDirect3DSurface9_GetDesc(bb, &dd);
                    if (SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, dd.Width, dd.Height, dd.Format, D3DPOOL_SYSTEMMEM, &sys, NULL)) &&
                        SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(dev, bb, sys))) {
                        RECT cr;
                        GetClientRect(hwnd, &cr);
                        int cw = cr.right, chh = cr.bottom;
                        D3DLOCKED_RECT lr;
                        if (cw > 0 && chh > 0 && SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
                            SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, cw, chh, 32, SDL_PIXELFORMAT_ARGB8888);
                            if (sf) {
                                for (int y = 0; y < chh; y++) memcpy((u8 *)sf->pixels + y * sf->pitch, (u8 *)lr.pBits + y * lr.Pitch, cw * 4);
                                SDL_SaveBMP(sf, fn);
                                SDL_FreeSurface(sf);
                            }
                            IDirect3DSurface9_UnlockRect(sys);
                        }
                    }
                    if (sys) IDirect3DSurface9_Release(sys);
                    IDirect3DSurface9_Release(bb);
                }
            } else {
                SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, video_w, video_h, 32, SDL_PIXELFORMAT_ARGB8888);
                if (sf) {
                    for (int y = 0; y < video_h; y++) memcpy((u8 *)sf->pixels + y * sf->pitch, rgb + y * video_w, video_w * 4);
                    SDL_SaveBMP(sf, fn);
                    SDL_FreeSurface(sf);
                }
            }
        }
    }
}

/* pixel write into the current mode's frame (banked VESA modes keep the
   selected bank in the A000h window, the rest in VRAM_GUEST) */
void video_put_pixel(int x, int y, u8 c, int op)
{
    if (x < 0 || y < 0 || x >= video_w || y >= video_h) return;
    u8 *p;
    if (video_w == 320) p = MEM + VGA_WINDOW + y * 320 + x;
    else {
        u32 off = (u32)y * video_w + x;
        if ((int)(off >> 16) == video_bank_cur) p = MEM + VGA_WINDOW + (off & 0xffff);
        else p = MEM + VRAM_GUEST + off;
    }
    switch (op) {
    case 1: *p ^= c; break;
    case 2: *p &= c; break;
    case 3: *p |= c; break;
    default: *p = c; break;
    }
}

/* put the Windows pointer where the game moved its cursor (game coords);
   only while our window has the focus, never in background test runs */
void video_warp_mouse(int x, int y)
{
    if (getenv("AO_HIDDEN") || !win || !hwnd || !video_w) return;
    if (!(SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS)) return;
    RECT cr;
    UINT dx, dy, dw, dh;
    GetClientRect(hwnd, &cr);
    dest_rect(cr.right > 0 ? cr.right : 1, cr.bottom > 0 ? cr.bottom : 1, &dx, &dy, &dw, &dh);
    SDL_WarpMouseInWindow(win, (int)(dx + (s64)x * dw / video_w), (int)(dy + (s64)y * dh / video_h));
}

/* short status message in the window title */
static u64 status_until;
void video_status(const char *msg)
{
    char t[160];
    snprintf(t, sizeof t, "Alien Odyssey  -  %s", msg);
    if (win) SDL_SetWindowTitle(win, t);
    status_until = SDL_GetTicks64() + 3000;
    rt_log("status: %s\n", msg);
}

void video_status_tick(void)
{
    if (status_until && SDL_GetTicks64() > status_until) {
        status_until = 0;
        if (win) SDL_SetWindowTitle(win, "Alien Odyssey");
    }
}
