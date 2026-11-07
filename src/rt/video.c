/* Display: a Windows window (SDL2 for the window and input) showing the
 * game's 8-bit screen through Direct3D 9.
 *
 * The game draws every pixel itself at 320x200 (its software 3D renderer
 * and the FMV decoders), so what can be improved is how that picture is
 * brought to the screen.  Each frame: palette -> 32-bit texture, optional
 * FXAA at the game's resolution, upscale to the output size with the chosen
 * filter (nearest, bilinear, Catmull-Rom bicubic, or sharp bilinear), then
 * optional contrast-adaptive sharpening at output resolution - the same
 * post chain as the Xbox ports.  In the 3D game the scenes themselves are
 * rendered at the chosen internal resolution (hires3d.c).  Options live in
 * a Video / View / Help menu bar on the window, like our Xbox ports, and in
 * odyssey_pc.ini [video].
 *
 * VESA banked modes keep the full picture in a 4 MB frame buffer at guest
 * address VRAM_GUEST; the 64 KB window at A0000h holds the selected bank. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
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

extern int opt_fullscreen, opt_filter, opt_sharpen, opt_fxaa, opt_aspect, opt_vsync, opt_scale, opt_showfps;
extern int hires3d_frame(const u8 **pix, const u8 **mask, int *w, int *h);
static u32 *rgb_hi;
static int rgb_hi_n;
static HMENU g_menu;
static HMENU build_menu(void);
static void set_client_size(int w, int h);
static void update_checks(void);
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
static IDirect3DPixelShader9 *ps_copy, *ps_bicubic, *ps_sharpbil, *ps_fxaa, *ps_sharpen, *ps_over, *ps_xbr;
static IDirect3DTexture9 *ov_tex;           /* high-res 3D layer (alpha = mask) */
static UINT ov_w, ov_h;
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
/* edge-directed pixel-art upscaler (own implementation of the xBR level-2
   rules: corner/edge detection on luma in a 5x5 neighbourhood, 30/45/60
   degree edges redrawn as smooth lines, blended over about one output
   pixel) - for 2D screens, fonts and the game's sprites */
static const char *PS_XBR =
    "sampler2D s : register(s0);\n"
    "float4 ts : register(c1);   // w, h, 1/w, 1/h of the source\n"
    "float4 k : register(c2);    // x = output pixels per source pixel\n"
    "static const float3 Y = float3(0.2126, 0.7152, 0.0722);\n"
    "static const float4 Ao = float4(1.0, -1.0, -1.0, 1.0);\n"
    "static const float4 Bo = float4(1.0, 1.0, -1.0, -1.0);\n"
    "static const float4 Co = float4(1.5, 0.5, -0.5, 0.5);\n"
    "static const float4 Ax = float4(1.0, -1.0, -1.0, 1.0);\n"
    "static const float4 Bx = float4(0.5, 2.0, -0.5, -2.0);\n"
    "static const float4 Cx = float4(1.0, 1.0, -0.5, 0.0);\n"
    "static const float4 Ay = float4(1.0, -1.0, -1.0, 1.0);\n"
    "static const float4 By = float4(2.0, 0.5, -2.0, -0.5);\n"
    "static const float4 Cy = float4(2.0, 0.0, -1.0, 0.5);\n"
    "float4 df(float4 a, float4 b) { return abs(a - b); }\n"
    "float cdf(float3 a, float3 b) { float3 d = abs(a - b); return d.r + d.g + d.b; }\n"
    "float4 neq(float4 a, float4 b) { return step(0.002, abs(a - b)); }\n"
    "float4 eqv(float4 a, float4 b) { return 1.0 - step(0.06, abs(a - b)); }\n"
    "float4 wdist(float4 a, float4 b, float4 c, float4 d, float4 e, float4 f, float4 g, float4 h)\n"
    "{\n"
    "    return df(a, b) + df(a, c) + df(d, e) + df(d, f) + 4.0 * df(g, h);\n"
    "}\n"
    "float3 T(float2 c, float x, float y) { return tex2D(s, c + float2(x, y) * ts.zw).rgb; }\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "    float2 p = uv * ts.xy;\n"
    "    float2 fp = frac(p);\n"
    "    float2 c = (floor(p) + 0.5) * ts.zw;\n"
    "    float3 A1 = T(c, -1, -2), B1 = T(c, 0, -2), C1 = T(c, 1, -2);\n"
    "    float3 A0 = T(c, -2, -1), A = T(c, -1, -1), B = T(c, 0, -1), C = T(c, 1, -1), C4 = T(c, 2, -1);\n"
    "    float3 D0 = T(c, -2, 0), D = T(c, -1, 0), E = T(c, 0, 0), F = T(c, 1, 0), F4 = T(c, 2, 0);\n"
    "    float3 G0 = T(c, -2, 1), G = T(c, -1, 1), H = T(c, 0, 1), I = T(c, 1, 1), I4 = T(c, 2, 1);\n"
    "    float3 G5 = T(c, -1, 2), H5 = T(c, 0, 2), I5 = T(c, 1, 2);\n"
    "    float4 b = float4(dot(B, Y), dot(D, Y), dot(H, Y), dot(F, Y));\n"
    "    float4 cc = float4(dot(C, Y), dot(A, Y), dot(G, Y), dot(I, Y));\n"
    "    float ee = dot(E, Y);\n"
    "    float4 e = float4(ee, ee, ee, ee);\n"
    "    float4 d = b.yzwx, f = b.wxyz, g = cc.zwxy, h = b.zwxy, i = cc.wxyz;\n"
    "    float4 i4 = float4(dot(I4, Y), dot(C1, Y), dot(A0, Y), dot(G5, Y));\n"
    "    float4 i5 = float4(dot(I5, Y), dot(C4, Y), dot(A1, Y), dot(G0, Y));\n"
    "    float4 h5 = float4(dot(H5, Y), dot(F4, Y), dot(B1, Y), dot(D0, Y));\n"
    "    float4 f4 = h5.yzwx;\n"
    "    float4 fx = Ao * fp.y + Bo * fp.x;\n"
    "    float4 fxl = Ax * fp.y + Bx * fp.x;\n"
    "    float4 fxu = Ay * fp.y + By * fp.x;\n"
    "    float4 lv0 = neq(e, f) * neq(e, h);\n"
    "    float4 lv1 = lv0 * saturate((1 - eqv(f, b)) * (1 - eqv(h, d)) + eqv(e, i) * (1 - eqv(f, i4)) * (1 - eqv(h, i5)) + eqv(e, g) + eqv(e, cc));\n"
    "    float4 lv2l = neq(e, g) * neq(d, g);\n"
    "    float4 lv2u = neq(e, cc) * neq(b, cc);\n"
    "    float dl = clamp(1.0 / k.x, 0.08, 0.5);   // blend width: about one output pixel\n"
    "    float4 delta = float4(dl, dl, dl, dl);\n"
    "    float4 fx45 = saturate((fx + delta - Co) / (2 * delta));\n"
    "    float4 fx30 = saturate((fxl + delta - Cx) / (2 * delta));\n"
    "    float4 fx60 = saturate((fxu + delta - Cy) / (2 * delta));\n"
    "    float4 wd1 = wdist(e, cc, g, i, h5, f4, h, f);\n"
    "    float4 wd2 = wdist(h, d, i5, f, i4, b, e, i);\n"
    "    float4 edr = step(wd1, wd2) * (1 - step(wd2, wd1)) * lv1;   // wd1 < wd2\n"
    "    float4 edrl = step(2.0 * df(f, g), df(h, cc)) * lv2l * edr;\n"
    "    float4 edru = step(2.0 * df(h, cc), df(f, g)) * lv2u * edr;\n"
    "    fx45 *= edr; fx30 *= edrl; fx60 *= edru;\n"
    "    float4 px = step(df(e, f), df(e, h));\n"
    "    float4 mx = max(max(fx30, fx60), fx45);\n"
    "    float3 r1 = lerp(E, lerp(H, F, px.x), mx.x);\n"
    "    r1 = lerp(r1, lerp(B, D, px.z), mx.z);\n"
    "    float3 r2 = lerp(E, lerp(F, B, px.y), mx.y);\n"
    "    r2 = lerp(r2, lerp(D, H, px.w), mx.w);\n"
    "    float3 r = lerp(r1, r2, step(cdf(E, r1), cdf(E, r2)));\n"
    "    return float4(r, 1);\n"
    "}\n";
static const char *PS_OVER =
    "sampler2D s : register(s0);\n"
    "float4 main(float2 t : TEXCOORD0) : COLOR { return tex2D(s, t); }\n";
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

static IDirect3DTexture9 *tx_tex;      /* HD text layer */
static UINT tx_w, tx_h;
static unsigned tx_gen = ~0u;
static float tx_v0 = -1, tx_v1 = -1;

static void release_targets(void)
{
    if (src_tex) { IDirect3DTexture9_Release(src_tex); src_tex = NULL; }
    if (aa_surf) { IDirect3DSurface9_Release(aa_surf); aa_surf = NULL; }
    if (aa_tex) { IDirect3DTexture9_Release(aa_tex); aa_tex = NULL; }
    if (up_surf) { IDirect3DSurface9_Release(up_surf); up_surf = NULL; }
    if (up_tex) { IDirect3DTexture9_Release(up_tex); up_tex = NULL; }
    if (ov_tex) { IDirect3DTexture9_Release(ov_tex); ov_tex = NULL; }
    if (tx_tex) { IDirect3DTexture9_Release(tx_tex); tx_tex = NULL; }
    src_w = src_h = up_w = up_h = ov_w = ov_h = tx_w = tx_h = 0;
    tx_gen = ~0u;
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
    /* never wait for the vertical blank: presenting runs on the game's own
       thread (a blocking Present slowed the whole game down); windowed and
       borderless output is composited by Windows, so there is no tearing */
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
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
    ps_over = mk_ps(PS_OVER);
    ps_xbr = mk_ps(PS_XBR);
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

/* widescreen: the rows of the game's picture that are shown (fractions of
   its height); passes that read the game's picture use [bv0, bv1] */
static float g_v0 = 0, g_v1 = 1, bv0 = 0, bv1 = 1;
extern int opt_widescreen, opt_hdtext;

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
    float q[4][4] = {{-1, 1, 0, bv0}, {1, 1, 1, bv0}, {-1, -1, 0, bv1}, {1, -1, 1, bv1}};
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
    if (opt_aspect == 0) {          /* 4:3, as large as fits (widescreen: only the rows shown, same pixel shape) */
        double a = (double)aw / ah / (g_v1 - g_v0);
        if (cw > ch * a) { *dw = (UINT)(ch * a + 0.5); if (*dw > cw) *dw = cw; *dx = (cw - *dw) / 2; }
        else { *dh = (UINT)(cw / a + 0.5); if (*dh > ch) *dh = ch; *dy = (ch - *dh) / 2; }
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

/* ---------------- HD text ----------------
   The game's characters drawn with a TrueType font at output resolution
   into a premultiplied-alpha layer that goes over the finished picture. */

typedef struct { HFONT f; int h, w; double cap; } FontC;
static FontC fcache[24];
static int fcache_n;
static HDC tdc;

static HFONT get_font(int h, int w)
{
    for (int i = 0; i < fcache_n; i++) if (fcache[i].h == h && fcache[i].w == w) return fcache[i].f;
    if (fcache_n == 24) { for (int i = 0; i < 24; i++) DeleteObject(fcache[i].f); fcache_n = 0; }
    HFONT f = CreateFontA(-h, w, 0, 0, FW_SEMIBOLD, 0, 0, 0, ANSI_CHARSET, OUT_TT_ONLY_PRECIS, 0, ANTIALIASED_QUALITY,
                          VARIABLE_PITCH | FF_SWISS, "Bahnschrift SemiCondensed");
    fcache[fcache_n].f = f; fcache[fcache_n].h = h; fcache[fcache_n].w = w;
    fcache_n++;
    return f;
}

static const MAT2 mat_id = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};

/* cap height of the font as a fraction of its em size */
static double font_cap_ratio(void)
{
    static double r;
    if (r) return r;
    SelectObject(tdc, get_font(200, 0));
    GLYPHMETRICS gm;
    r = GetGlyphOutlineA(tdc, 'H', GGO_METRICS, &gm, 0, NULL, &mat_id) != GDI_ERROR && gm.gmBlackBoxY ? gm.gmBlackBoxY / 200.0 : 0.7;
    return r;
}

static void build_text(const HdChar *c, int n, UINT dw, UINT dh)
{
    static HBITMAP bmp;
    static u32 *bits, *lay;
    static UINT bw, bh;
    if (!tdc) tdc = CreateCompatibleDC(NULL);
    if (bw != dw || bh != dh) {
        if (bmp) DeleteObject(bmp);
        BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), (LONG)dw, -(LONG)dh, 1, 32, BI_RGB}};
        bmp = CreateDIBSection(tdc, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
        SelectObject(tdc, bmp);
        free(lay);
        lay = malloc((size_t)dw * dh * 4);
        bw = dw; bh = dh;
    }
    if (!bits || !lay) return;
    memset(bits, 0, (size_t)dw * dh * 4);
    memset(lay, 0, (size_t)dw * dh * 4);
    SetBkMode(tdc, TRANSPARENT);
    SetTextAlign(tdc, TA_BASELINE | TA_LEFT);
    SetTextColor(tdc, RGB(255, 255, 255));
    double sx = (double)dw / 320.0, sy = (double)dh / ((g_v1 - g_v0) * 200.0), top = g_v0 * 200.0;
    /* the cap height of each font (fonts told apart by their cell height):
       the tallest capital letter on screen in that font */
    int capg[49] = {0}, anyg[49] = {0};
    for (int i = 0; i < n; i++) {
        int h = c[i].h > 48 ? 48 : c[i].h;
        if (c[i].ch >= 'A' && c[i].ch <= 'Z' && c[i].ch != 'Q' && c[i].ch != 'J' && c[i].ih > capg[h]) capg[h] = c[i].ih;
        if (c[i].ih > anyg[h]) anyg[h] = c[i].ih;
    }
    double cr = font_cap_ratio();
    for (int i = 0; i < n; i++) {
        const HdChar *k = &c[i];
        if (k->ch <= ' ' || k->ch > '~') continue;
        double iy0 = (k->iy - top) * sy, iy1 = (k->iy + k->ih - top) * sy;
        if (iy1 < 0 || iy0 > dh) continue;
        int hh = k->h > 48 ? 48 : k->h;
        double cap = capg[hh] ? capg[hh] : anyg[hh] * 0.8;
        if (cap < 3) cap = k->h * 0.7;
        int fh = (int)(cap * sy / cr + 0.5);
        if (fh < 6) fh = 6;
        char t = (char)k->ch;
        HFONT f = get_font(fh, 0);
        SelectObject(tdc, f);
        GLYPHMETRICS gm;
        if (GetGlyphOutlineA(tdc, (UINT)(u8)t, GGO_METRICS, &gm, 0, NULL, &mat_id) == GDI_ERROR) continue;
        double inkw = k->iw * sx;
        if (gm.gmBlackBoxX > inkw * 1.2 + 2) {   /* wider than the original: a narrower cut of the font */
            TEXTMETRICA tm;
            GetTextMetricsA(tdc, &tm);
            int fw = (int)(tm.tmAveCharWidth * (inkw * 1.1 + 1) / gm.gmBlackBoxX);
            if (fw < 1) fw = 1;
            SelectObject(tdc, get_font(fh, fw));
            GetGlyphOutlineA(tdc, (UINT)(u8)t, GGO_METRICS, &gm, 0, NULL, &mat_id);
        }
        /* the font's ink box goes where the original's was: centred
           horizontally, standing on the same bottom line */
        double cx = (k->ix + k->iw * 0.5) * sx;
        int ox = (int)(cx - (gm.gmptGlyphOrigin.x + gm.gmBlackBoxX * 0.5) + 0.5);
        int by = (int)(iy1 + gm.gmptGlyphOrigin.y - (int)gm.gmBlackBoxY + 0.5);
        int rx0 = ox + gm.gmptGlyphOrigin.x - 2, ry0 = by - gm.gmptGlyphOrigin.y - 2;
        int rx1 = rx0 + (int)gm.gmBlackBoxX + 5, ry1 = ry0 + (int)gm.gmBlackBoxY + 5;
        if (rx0 < 0) rx0 = 0;
        if (ry0 < 0) ry0 = 0;
        if (rx1 > (int)dw) rx1 = dw;
        if (ry1 > (int)dh) ry1 = dh;
        if (rx0 >= rx1 || ry0 >= ry1) continue;
        for (int y = ry0; y < ry1; y++) memset(bits + (size_t)y * dw + rx0, 0, (rx1 - rx0) * 4);
        TextOutA(tdc, ox, by, &t, 1);
        GdiFlush();
        /* colour per pixel, as the game drew it: its font colour over each
           background colour (translucent fonts take the background's tint);
           backgrounds it never drew over get the nearest known one */
        u32 tab[256];
        {
            int known[256], nk = 0;
            for (int v = 0; v < 256; v++) if (k->map[v] >= 0) known[nk++] = v;
            for (int v = 0; v < 256; v++) {
                int m = k->map[v];
                if (m < 0 && nk) {
                    u32 pv = pal32[v];
                    int bd = 1 << 30;
                    for (int q = 0; q < nk; q++) {
                        u32 pq = pal32[known[q]];
                        int dr = (int)((pv >> 16) & 255) - (int)((pq >> 16) & 255), dg = (int)((pv >> 8) & 255) - (int)((pq >> 8) & 255), db = (int)(pv & 255) - (int)(pq & 255);
                        int d = dr * dr + dg * dg + db * db;
                        if (d < bd) { bd = d; m = k->map[known[q]]; }
                    }
                }
                tab[v] = pal32[m >= 0 ? m : k->colour];
            }
        }
        for (int y = ry0; y < ry1; y++) {
            const u32 *src = bits + (size_t)y * dw;
            u32 *d = lay + (size_t)y * dw;
            int gy = (int)(y / sy + top) - k->y;
            if (gy < 0) gy = 0;
            if (gy >= k->h) gy = k->h - 1;
            for (int x = rx0; x < rx1; x++) {
                u32 a = src[x] & 255;
                if (!a) continue;
                int gx = (int)(x / sx) - k->x;
                if (gx < 0) gx = 0;
                if (gx >= k->w) gx = k->w - 1;
                u32 col = tab[k->pre[gy * k->w + gx]];
                u32 cr8 = (col >> 16) & 255, cg8 = (col >> 8) & 255, cb8 = col & 255;
                u32 v = d[x], ia = 255 - a;
                u32 na = a + (((v >> 24) * ia) >> 8);
                u32 nr = ((cr8 * a) >> 8) + ((((v >> 16) & 255) * ia) >> 8);
                u32 ng = ((cg8 * a) >> 8) + ((((v >> 8) & 255) * ia) >> 8);
                u32 nb = ((cb8 * a) >> 8) + (((v & 255) * ia) >> 8);
                d[x] = (na > 255 ? 255 : na) << 24 | nr << 16 | ng << 8 | nb;
            }
        }
    }
    if (tx_w != dw || tx_h != dh) {
        if (tx_tex) { IDirect3DTexture9_Release(tx_tex); tx_tex = NULL; }
        tx_w = tx_h = 0;
        if (SUCCEEDED(IDirect3DDevice9_CreateTexture(dev, dw, dh, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tx_tex, NULL))) { tx_w = dw; tx_h = dh; }
    }
    D3DLOCKED_RECT lr;
    if (tx_tex && SUCCEEDED(IDirect3DTexture9_LockRect(tx_tex, 0, &lr, NULL, D3DLOCK_DISCARD))) {
        for (UINT y = 0; y < dh; y++) memcpy((u8 *)lr.pBits + y * lr.Pitch, lay + (size_t)y * dw, dw * 4);
        IDirect3DTexture9_UnlockRect(tx_tex, 0);
    }
}

static void d3d_present(const u32 *pix, int w, int h, const u32 *ov, int ow, int oh,
                        const HdChar *chars, int nchars, unsigned cgen)
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
    if (ov && ((UINT)ow != ov_w || (UINT)oh != ov_h)) {
        if (ov_tex) { IDirect3DTexture9_Release(ov_tex); ov_tex = NULL; }
        if (SUCCEEDED(IDirect3DDevice9_CreateTexture(dev, ow, oh, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &ov_tex, NULL))) { ov_w = ow; ov_h = oh; }
    }
    if (ov && ov_tex && SUCCEEDED(IDirect3DTexture9_LockRect(ov_tex, 0, &lr, NULL, D3DLOCK_DISCARD))) {
        for (int y = 0; y < oh; y++) memcpy((u8 *)lr.pBits + y * lr.Pitch, ov + (size_t)y * ow, ow * 4);
        IDirect3DTexture9_UnlockRect(ov_tex, 0);
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
        float vscale = (float)(dh / (h * (double)(g_v1 - g_v0)));
        bv0 = g_v0; bv1 = g_v1;
        if (opt_filter == 4 && ps_xbr) {
            float k[4] = {(float)dw / w, vscale, 0, 0};
            IDirect3DDevice9_SetPixelShaderConstantF(dev, 2, k, 1);
            blit(cur, w, h, ps_xbr, vu, 0);
        } else if (opt_filter == 2 && ps_bicubic) blit(cur, w, h, ps_bicubic, vu, 1);
        else if (opt_filter == 3 && ps_sharpbil) {
            float k[4] = {(float)dw / w, vscale, 0, 0};
            IDirect3DDevice9_SetPixelShaderConstantF(dev, 2, k, 1);
            blit(cur, w, h, ps_sharpbil, vu, 1);
        } else blit(cur, w, h, ps_copy, vu, opt_filter != 0);
        if (ov && ov_tex && ps_over) {   /* the high-res 3D layer on top, edges antialiased by its mask */
            IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
            IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            blit(ov_tex, ow, oh, ps_over, vu, 1);
            IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
        }
        bv0 = 0; bv1 = 1;
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
        if (nchars && ps_over) {   /* HD text on top, after sharpening */
            if (cgen != tx_gen || dw != tx_w || dh != tx_h || g_v0 != tx_v0 || g_v1 != tx_v1) {
                build_text(chars, nchars, dw, dh);
                tx_gen = cgen; tx_v0 = g_v0; tx_v1 = g_v1;
            }
            if (tx_tex) {
                IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
                IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_ONE);
                IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
                blit(tx_tex, dw, dh, ps_over, vd, 0);
                IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
            }
        }
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
    g_menu = build_menu();
    update_checks();
    if (getenv("AO_NOMENU")) { rt_log("video: menu disabled\n"); return; }
    if (!opt_fullscreen) {
        SetMenu(hwnd, g_menu);
        SDL_SetWindowSize(win, 1280, 960);
    }
    {   /* AO_WINSIZE=WxH: client size for test runs */
        int ww, wh;
        const char *e = getenv("AO_WINSIZE");
        if (e && sscanf(e, "%dx%d", &ww, &wh) == 2) set_client_size(ww, wh);
    }
    SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);   /* menu commands arrive as WM_COMMAND */
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

/* ---------------- menu bar (like the Xbox ports) ---------------- */
enum {
    ID_SCALE = 1000,                        /* +1..+6 */
    ID_ASPECT = 1100,                       /* +0..2 */
    ID_FILTER = 1200,                       /* +0..3 */
    ID_FXAA = 1300, ID_SHARP = 1310,        /* +0..3 */
    ID_WIN = 1500,                          /* +1..3 */
    ID_FULLSCREEN = 1510, ID_SHOWFPS, ID_VSYNC,
    ID_CONTROLS = 1700, ID_ABOUT,
    ID_INVERTY = 1800, ID_MOUSEAIM, ID_SENS = 1810,   /* +index into g_sens */
    ID_WIDE = 1900,                         /* +0..2 */
    ID_HDTEXT = 1910,
};
static const int g_sens[] = {25, 50, 75, 100, 150, 200, 300, 400};
extern int opt_mouse_sens, opt_mouse_invert;

int video_has_focus(void) { return win && (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS); }

/* the game's own control setting, "Controller N" in C:\GAME.CFG (0 = mouse,
   1 joystick, 2 keyboard, 3 joypad); each part of the game reads it when it
   starts, and the game's options menu writes it */
static int game_controller(int set)
{
    char p[700], line[256], out[8192] = "";
    snprintf(p, sizeof p, "%s\\hd\\GAME.CFG", rt_root);
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    int cur = -1;
    size_t o = 0;
    while (fgets(line, sizeof line, f)) {
        int v;
        if (sscanf(line, "Controller %d", &v) == 1) {
            cur = v;
            if (set >= 0) snprintf(line, sizeof line, "Controller %d\r\n", set);
        }
        size_t n = strlen(line);
        if (o + n < sizeof out) { memcpy(out + o, line, n); o += n; }
    }
    fclose(f);
    if (set >= 0 && cur >= 0 && (f = fopen(p, "wb"))) { fwrite(out, 1, o, f); fclose(f); cur = set; }
    return cur;
}

static void update_checks(void)
{
    if (!g_menu) return;
    CheckMenuRadioItem(g_menu, ID_SCALE + 1, ID_SCALE + 6, ID_SCALE + opt_scale, MF_BYCOMMAND);
    CheckMenuRadioItem(g_menu, ID_ASPECT, ID_ASPECT + 2, ID_ASPECT + opt_aspect, MF_BYCOMMAND);
    CheckMenuRadioItem(g_menu, ID_FILTER, ID_FILTER + 4, ID_FILTER + opt_filter, MF_BYCOMMAND);
    CheckMenuRadioItem(g_menu, ID_SHARP, ID_SHARP + 3, ID_SHARP + opt_sharpen, MF_BYCOMMAND);
    CheckMenuItem(g_menu, ID_FXAA, MF_BYCOMMAND | (opt_fxaa ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuRadioItem(g_menu, ID_WIDE, ID_WIDE + 2, ID_WIDE + opt_widescreen, MF_BYCOMMAND);
    CheckMenuItem(g_menu, ID_HDTEXT, MF_BYCOMMAND | (opt_hdtext ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(g_menu, ID_FULLSCREEN, MF_BYCOMMAND | (opt_fullscreen ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(g_menu, ID_SHOWFPS, MF_BYCOMMAND | (opt_showfps ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(g_menu, ID_INVERTY, MF_BYCOMMAND | (opt_mouse_invert ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(g_menu, ID_MOUSEAIM, MF_BYCOMMAND | (game_controller(-1) == 0 ? MF_CHECKED : MF_UNCHECKED));
    for (int i = 0; i < 8; i++) CheckMenuItem(g_menu, ID_SENS + i, MF_BYCOMMAND | (g_sens[i] == opt_mouse_sens ? MF_CHECKED : MF_UNCHECKED));
}

static HMENU build_menu(void)
{
    HMENU bar = CreateMenu(), video = CreatePopupMenu(), res = CreatePopupMenu(), asp = CreatePopupMenu(),
          filt = CreatePopupMenu(), sharp = CreatePopupMenu(), view = CreatePopupMenu(), winm = CreatePopupMenu(),
          help = CreatePopupMenu(), input = CreatePopupMenu(), sens = CreatePopupMenu(), wide = CreatePopupMenu();
    static const wchar_t *cls[7] = {L"", L"  Native", L"  480p-class", L"  720p-class", L"  1080p-class", L"  1440p-class", L"  4K-class"};
    for (int s = 1; s <= 6; s++) {
        wchar_t t[96];
        swprintf(t, 96, L"%dx  (%dx%d)%ls", s, 320 * s, 200 * s, cls[s]);
        AppendMenuW(res, MF_STRING, ID_SCALE + s, t);
    }
    AppendMenuW(res, MF_SEPARATOR, 0, NULL);
    AppendMenuW(res, MF_STRING | MF_GRAYED, 0, L"3D scenes are rendered at this size; videos use the upscale filter");
    AppendMenuW(asp, MF_STRING, ID_ASPECT + 0, L"Keep aspect 4:3  (black bars if the window doesn't match)");
    AppendMenuW(asp, MF_STRING, ID_ASPECT + 1, L"Stretch to window");
    AppendMenuW(asp, MF_STRING, ID_ASPECT + 2, L"Integer scaling  (pixel-exact)");
    AppendMenuW(filt, MF_STRING, ID_FILTER + 4, L"xBR  (smooth edges, sharp - best for this game)");
    AppendMenuW(filt, MF_SEPARATOR, 0, NULL);
    AppendMenuW(filt, MF_STRING, ID_FILTER + 0, L"Nearest  (sharp pixels)");
    AppendMenuW(filt, MF_STRING, ID_FILTER + 1, L"Bilinear  (smooth)");
    AppendMenuW(filt, MF_STRING, ID_FILTER + 2, L"Bicubic  (sharp + smooth)");
    AppendMenuW(filt, MF_STRING, ID_FILTER + 3, L"Sharp bilinear  (crisp pixels, even sizes)");
    AppendMenuW(sharp, MF_STRING, ID_SHARP + 0, L"Off");
    AppendMenuW(sharp, MF_STRING, ID_SHARP + 1, L"Low");
    AppendMenuW(sharp, MF_STRING, ID_SHARP + 2, L"Medium");
    AppendMenuW(sharp, MF_STRING, ID_SHARP + 3, L"High");
    AppendMenuW(video, MF_POPUP, (UINT_PTR)res, L"&Internal Resolution");
    AppendMenuW(video, MF_POPUP, (UINT_PTR)asp, L"&Aspect Ratio");
    AppendMenuW(wide, MF_STRING, ID_WIDE + 0, L"Off  (the whole 4:3 picture)");
    AppendMenuW(wide, MF_STRING, ID_WIDE + 1, L"Zoom to the picture  (removes black bars, never cuts or stretches)");
    AppendMenuW(wide, MF_STRING, ID_WIDE + 2, L"Fill the screen  (cuts a little off the top and bottom, no stretching)");
    AppendMenuW(wide, MF_SEPARATOR, 0, NULL);
    AppendMenuW(wide, MF_STRING | MF_GRAYED, 0, L"Needs Aspect Ratio: Keep aspect 4:3");
    AppendMenuW(video, MF_POPUP, (UINT_PTR)wide, L"&Widescreen");
    AppendMenuW(video, MF_POPUP, (UINT_PTR)filt, L"Upscale &Filter");
    AppendMenuW(video, MF_STRING, ID_FXAA, L"Anti-aliasing (&FXAA)");
    AppendMenuW(video, MF_POPUP, (UINT_PTR)sharp, L"&Sharpening (contrast adaptive)");
    AppendMenuW(video, MF_SEPARATOR, 0, NULL);
    AppendMenuW(video, MF_STRING, ID_HDTEXT, L"&HD text  (the game's text in a smooth font)");
    for (int k = 1; k <= 3; k++) { wchar_t t[32]; swprintf(t, 32, L"%d x %d", 640 * k, 480 * k); AppendMenuW(winm, MF_STRING, ID_WIN + k, t); }
    AppendMenuW(view, MF_POPUP, (UINT_PTR)winm, L"&Window Size");
    AppendMenuW(view, MF_STRING, ID_FULLSCREEN, L"&Fullscreen\tAlt+Enter");
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    AppendMenuW(view, MF_STRING, ID_SHOWFPS, L"Show FPS in title");
    for (int i = 0; i < 8; i++) {
        wchar_t t[32];
        swprintf(t, 32, L"%d%%%ls", g_sens[i], g_sens[i] == 100 ? L"  (default)" : L"");
        AppendMenuW(sens, MF_STRING, ID_SENS + i, t);
    }
    AppendMenuW(input, MF_STRING, ID_MOUSEAIM, L"&Mouse aiming  (aim with the mouse, left button fires; from the next part of the game)");
    AppendMenuW(input, MF_SEPARATOR, 0, NULL);
    AppendMenuW(input, MF_POPUP, (UINT_PTR)sens, L"Mouse &sensitivity");
    AppendMenuW(input, MF_STRING, ID_INVERTY, L"&Invert mouse Y");
    AppendMenuW(help, MF_STRING, ID_CONTROLS, L"&Controls...");
    AppendMenuW(help, MF_STRING, ID_ABOUT, L"&About...");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)video, L"&Video");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)view, L"Vie&w");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)input, L"&Input");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"&Help");
    return bar;
}

/* fullscreen hides the menu bar, a window shows it */
void video_set_fullscreen(int on)
{
    extern void settings_save_fullscreen(void);
    opt_fullscreen = on;
    if (!getenv("AO_HIDDEN")) {
        if (on) { SetMenu(hwnd, NULL); SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP); }
        else { SDL_SetWindowFullscreen(win, 0); SetMenu(hwnd, g_menu); }
    }
    settings_save_fullscreen();
    update_checks();
}

static void set_client_size(int w, int h)
{
    if (opt_fullscreen) video_set_fullscreen(0);
    SDL_RestoreWindow(win);
    RECT r = {0, 0, w, h};
    AdjustWindowRect(&r, (DWORD)GetWindowLongPtrA(hwnd, GWL_STYLE), g_menu != NULL);
    SetWindowPos(hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
}

void video_menu_command(int id)
{
    extern void settings_save_video(void);
    if (id > ID_SCALE && id <= ID_SCALE + 6) opt_scale = id - ID_SCALE;
    else if (id >= ID_ASPECT && id <= ID_ASPECT + 2) opt_aspect = id - ID_ASPECT;
    else if (id >= ID_FILTER && id <= ID_FILTER + 4) opt_filter = id - ID_FILTER;
    else if (id >= ID_SHARP && id <= ID_SHARP + 3) opt_sharpen = id - ID_SHARP;
    else if (id == ID_FXAA) opt_fxaa = !opt_fxaa;
    else if (id >= ID_WIDE && id <= ID_WIDE + 2) { opt_widescreen = id - ID_WIDE; if (opt_widescreen) opt_aspect = 0; }
    else if (id == ID_HDTEXT) opt_hdtext = !opt_hdtext;
    else if (id == ID_INVERTY) opt_mouse_invert = !opt_mouse_invert;
    else if (id == ID_MOUSEAIM) game_controller(game_controller(-1) == 0 ? 2 : 0);
    else if (id >= ID_SENS && id < ID_SENS + 8) opt_mouse_sens = g_sens[id - ID_SENS];
    else if (id > ID_WIN && id <= ID_WIN + 3) { set_client_size(640 * (id - ID_WIN), 480 * (id - ID_WIN)); return; }
    else if (id == ID_FULLSCREEN) { video_set_fullscreen(!opt_fullscreen); return; }
    else if (id == ID_SHOWFPS) { opt_showfps = !opt_showfps; if (!opt_showfps) SDL_SetWindowTitle(win, "Alien Odyssey"); }
    else if (id == ID_CONTROLS) {
        MessageBoxA(hwnd,
                    "The game's own controls, as on DOS (GAME.CFG holds the key layout):\n\n"
                    "Mouse - aim on the bike and in the mine cart, left button fires\n"
                    "    (Input > Mouse aiming switches the game's own control setting)\n"
                    "Arrow keys - move / steer / aim\n"
                    "Ctrl, Space, Z, X, Shift, Alt - actions (see the manual)\n"
                    "Esc - menu / skip\n\n"
                    "PC: Alt+Enter switches fullscreen; in a window the menu bar has the video options.",
                    "Alien Odyssey - Controls", MB_ICONINFORMATION);
        return;
    } else if (id == ID_ABOUT) {
        MessageBoxA(hwnd,
                    "Alien Odyssey - PC Native\n\n"
                    "The DOS game's code converted to native Windows code, running on a PC\n"
                    "runtime that replaces DOS, the extender, and the sound and video hardware.\n\n"
                    "Alien Odyssey (c) Argonaut Software / Philips Media. Fan preservation project.",
                    "About", MB_ICONINFORMATION);
        return;
    } else return;
    settings_save_video();
    update_checks();
    video_present(1);
}

/* window coordinates -> game screen coordinates */
void video_window_to_game(int wx, int wy, int *gx, int *gy)
{
    RECT cr;
    UINT dx, dy, dw, dh;
    if (!hwnd || !GetClientRect(hwnd, &cr) || !video_w) { *gx = wx; *gy = wy; return; }
    dest_rect(cr.right > 0 ? cr.right : 1, cr.bottom > 0 ? cr.bottom : 1, &dx, &dy, &dw, &dh);
    *gx = (int)(((s64)wx - dx) * video_w / dw);
    *gy = (int)(g_v0 * video_h + ((double)wy - dy) * video_h * (g_v1 - g_v0) / dh);
}

/* Widescreen: which rows of the game's picture to show.
   1 "zoom to the picture": many scenes (videos, cut-scenes, the bike
     ride) have black bars above and below the picture; the shown rows close
     in on the picture, at most as far as needed to fill the screen's width,
     never cutting into it and never changing the shape of the pixels.
   2 "fill": always fill the width, rows are cut at the top and bottom
     (kept around the picture). */
static unsigned pal_gen;
static void widescreen_rows(const u8 *src, u64 now)
{
    static int c0 = 0, c1 = 200;            /* rows shown now */
    static int p0 = -1, p1 = -1;            /* proposed smaller range and since when */
    static u64 p_since;
    int H = video_h;
    RECT cr;
    if (!opt_widescreen || opt_aspect != 0 || !hwnd || !GetClientRect(hwnd, &cr) || cr.bottom <= 0 || video_w <= 0) {
        g_v0 = 0; g_v1 = 1; c0 = 0; c1 = H; return;
    }
    if (c1 > H || c0 < 0 || c1 - c0 < 1) { c0 = 0; c1 = H; }
    double a = (double)cr.right / cr.bottom;
    int need = (int)ceil(H * (4.0 / 3.0) / a);   /* rows that fill the width */
    if (need >= H) { g_v0 = 0; g_v1 = 1; c0 = 0; c1 = H; return; }
    /* the picture's rows: not (nearly) black */
    int t = -1, b = -1, W = video_w;
    for (int y = 0; y < H; y++) {
        const u8 *r = src + (size_t)y * W;
        int lit = 0;
        for (int x = 0; x < W; x += 2) {
            u32 v = pal32[r[x]];
            if (((v >> 16) & 255) + ((v >> 8) & 255) + (v & 255) > 36) { lit++; if (lit > 2) break; }
        }
        if (lit > 2) { if (t < 0) t = y; b = y + 1; }
    }
    if (t < 0) { t = c0; b = c1; }           /* all black (fade): keep */
    int r0, r1;
    if (opt_widescreen == 2) {
        /* pan and scan, like the Resident Evil HD remaster: the cut window
           follows where the picture moves (the player, the action),
           smoothly, and stays inside the picture */
        static u8 prev[320 * 240];
        static double cen = -1;
        double sum = 0, cnt = 0;
        if (W == 320 && H <= 240) {
            for (int y = t; y < b; y++) {
                const u8 *r = src + (size_t)y * W, *q = prev + (size_t)y * W;
                int d = 0;
                for (int x = 0; x < W; x += 2) d += r[x] != q[x];
                sum += (double)d * y; cnt += d;
            }
            memcpy(prev, src, (size_t)W * H);
        }
        double mid = (t + b) * 0.5;
        if (cen < 0 || cen < t || cen > b) cen = mid;
        double tgt = cnt > 40 && cnt < (b - t) * 120 ? sum / cnt : cen;   /* little or all moving: stay */
        cen += (tgt - cen) * 0.06;
        r0 = (int)(cen - need / 2.0 + 0.5);
        if (b - t >= need) {
            if (r0 < t) r0 = t;
            if (r0 + need > b) r0 = b - need;
        } else r0 = (int)(mid - need / 2.0);
    } else {
        int rows = b - t > need ? b - t : need;
        r0 = (t + b) / 2 - rows / 2;
        need = rows;
    }
    if (r0 < 0) r0 = 0;
    if (r0 + need > H) r0 = H - need;
    r1 = r0 + need;
    /* bigger or moved: at once; smaller: when it has stayed so a moment
       (no pumping when a scene briefly goes dark at the edges) */
    if (r0 < c0 || r1 > c1 || opt_widescreen == 2) { c0 = r0; c1 = r1; p0 = -1; }
    else if (r0 != c0 || r1 != c1) {
        if (r0 != p0 || r1 != p1) { p0 = r0; p1 = r1; p_since = now; }
        else if (now - p_since > 400) { c0 = r0; c1 = r1; p0 = -1; }
    } else p0 = -1;
    g_v0 = (float)c0 / H; g_v1 = (float)c1 / H;
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
        pal_gen++;
    }
    if (!video_w || !rgb) {
        if (d3d_ok && !force) {
            u32 black = 0xff000000u;
            g_v0 = 0; g_v1 = 1;
            d3d_present(&black, 1, 1, NULL, 0, 0, NULL, 0, 0);
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
        if (on || opt_showfps) {
            u32 hh = 2166136261u;
            const u32 *p = (const u32 *)src;
            for (int i = 0; i < video_w * video_h / 4; i += 7) hh = (hh ^ p[i]) * 16777619u;
            if (hh != last_hash) { frames++; last_hash = hh; }
            if (now - t0 >= (opt_showfps ? 1000u : 5000u)) {
                if (t0 && on) rt_log("fps: %.1f\n", frames * 1000.0 / (now - t0));
                if (t0 && opt_showfps) {
                    char t[96];
                    snprintf(t, sizeof t, "Alien Odyssey  -  %.0f fps", frames * 1000.0 / (now - t0));
                    SDL_SetWindowTitle(win, t);
                }
                t0 = now; frames = 0;
            }
        }
    }
    int n = video_w * video_h;
    widescreen_rows(src, now);
    /* HD text: the characters still on screen, their low-res pixels painted
       over with the background in a copy of the frame */
    static HdChar chars[768];
    static u8 base[320 * 200];
    int nchars = 0;
    unsigned cgen = 0;
    if (opt_hdtext && video_w == 320 && video_h == 200 && d3d_ok) {
        memcpy(base, src, 64000);
        nchars = hdtext_visible(src, base, chars, 768, &cgen);
        if (nchars) src = base;
        cgen = cgen * 2654435761u + pal_gen;
    }
    for (int i = 0; i < n; i++) rgb[i] = pal32[src[i]];
    {
        const u8 *hp;
        int hw, hh;
        const u8 *hm;
        if (d3d_ok && video_w == 320 && hires3d_frame(&hp, &hm, &hw, &hh)) {
            if (rgb_hi_n < hw * hh) { free(rgb_hi); rgb_hi = malloc((size_t)hw * hh * 4); rgb_hi_n = hw * hh; }
            for (int i = 0; i < hw * hh; i++) rgb_hi[i] = hm[i] ? pal32[hp[i]] : (pal32[hp[i]] & 0x00ffffffu);
            d3d_present(rgb, video_w, video_h, rgb_hi, hw, hh, chars, nchars, cgen);
        } else if (d3d_ok) d3d_present(rgb, video_w, video_h, NULL, 0, 0, chars, nchars, cgen);
    }
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
    SDL_WarpMouseInWindow(win, (int)(dx + (s64)x * dw / video_w), (int)(dy + (y - g_v0 * video_h) * dh / (video_h * (g_v1 - g_v0))));
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
