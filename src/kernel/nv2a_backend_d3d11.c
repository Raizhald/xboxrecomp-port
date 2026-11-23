/*
 * NV2A render back end on Direct3D 11 (see nv2a_backend.h).
 *
 * The pushbuffer executor decodes, transforms and clips on the CPU; this
 * draws the resulting triangles on the GPU instead of in the software
 * rasteriser, which measured ~37 FPS against a ~163 FPS ceiling with nothing
 * drawn at all (Fallout: Brotherhood of Steel, Carbon, standing still).
 *
 * What it reproduces of the software rasteriser (xf_rows in nv2a_pb_exec.c):
 *   - the register combiners, evaluated per pixel by a generic interpreter in
 *     the pixel shader -- a line-for-line port of nv2a_combiner.c, driven by
 *     the raw ICW/OCW words in a constant buffer, so no per-state shader
 *     compilation and no hitches;
 *   - texture stages 0-3 (none, 2D/3D projective, passthrough, clip plane;
 *     cube maps sample face 0 as the software path does without a cube),
 *     nearest or bilinear at level 0, wrap/clamp/mirror;
 *   - screen-linear colours and fog, perspective-correct texture coordinates;
 *   - blend, alpha test, depth test/write, colour mask, clip rectangle.
 * Like the software path it does not cull and ignores stencil.
 *
 * Memory model. Each colour surface the title draws into lives on the GPU as
 * a render target keyed by guest address; when one is created it starts from
 * what guest memory holds. A texture whose address is such a surface samples
 * the render target (render-to-texture). Everything else is decoded from
 * guest memory on the CPU (nv2a_backend_decode_texture, palettes included)
 * and cached; a cached texture is re-hashed the first time it is bound in
 * each frame and re-uploaded when its bytes or palette changed
 * (RECOMP_GPU_TEX_CHECK=batch: on every bind). At a flip the presented
 * surface is copied back into guest memory (resolve), so the window, the
 * frame dumps and everything else that reads guest memory work unchanged.
 *
 * ponytail: stencil, culling, mip levels, cube faces, bump-env and
 * dot-product stages, and point/line primitives are not drawn here either;
 * the software rasteriser does not have them yet.
 */
#include "nv2a_backend.h"

#if defined(_WIN32)

#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

/* ---- Shaders ----------------------------------------------------------- */

static const char k_hlsl[] =
"cbuffer CB : register(b0) {\n"
"  float4 g_vp;\n"          /* 2/W, 2/H, 1/zmax, perspective */
"  uint4  g_mode;\n"        /* use_rc, tex0 (no rc), alpha func (0 off), alpha ref */
"  uint4  g_fin;\n"         /* final0, final1, control, stage_program */
"  uint4  g_misc;\n"        /* clip_plane_mode */
"  float4 g_tscale[4];\n"
"  float4 g_fogc;\n"
"  float4 g_fc0;\n"
"  float4 g_fc1;\n"
"  uint4  g_cw[8];\n"       /* color_icw, color_ocw, alpha_icw, alpha_ocw */
"  float4 g_f0[8];\n"
"  float4 g_f1[8];\n"
"  float4 g_wsx;\n"        /* x: added to pos.x * g_vp.x (widescreen offset) */
"};\n"
"Texture2D g_t0 : register(t0); Texture2D g_t1 : register(t1);\n"
"Texture2D g_t2 : register(t2); Texture2D g_t3 : register(t3);\n"
"SamplerState g_s0 : register(s0); SamplerState g_s1 : register(s1);\n"
"SamplerState g_s2 : register(s2); SamplerState g_s3 : register(s3);\n"
"struct VSIn {\n"
"  float4 pos : POSITION; float4 d0 : COLOR0; float4 d1 : COLOR1;\n"
"  float4 fog : TEXCOORD4; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1;\n"
"  float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;\n"
"};\n"
"struct PSIn {\n"
"  float4 pos : SV_Position;\n"
"  noperspective float4 d0 : COLOR0; noperspective float4 d1 : COLOR1;\n"
"  noperspective float4 fog : TEXCOORD4;\n"
"  float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1;\n"
"  float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;\n"
"};\n"
"PSIn VSMain(VSIn i) {\n"
"  PSIn o;\n"
"  float w = 1.0;\n"
"  if (g_vp.w != 0.0 && i.pos.w > 0.0) w = 1.0 / i.pos.w;\n"
"  float x = i.pos.x * g_vp.x + g_wsx.x;\n"
"  float y = 1.0 - i.pos.y * g_vp.y;\n"
"  float z = i.pos.z * g_vp.z;\n"
"  o.pos = float4(x * w, y * w, z * w, w);\n"
"  o.d0 = i.d0; o.d1 = i.d1; o.fog = i.fog;\n"
"  o.t0 = i.t0; o.t1 = i.t1; o.t2 = i.t2; o.t3 = i.t3;\n"
"  return o;\n"
"}\n"
/* nv2a_combiner.c, register numbers as there. */
"static float4 R[16];\n"
"float3 mapv(float3 x, uint m) {\n"
"  float3 p = max(x, 0.0);\n"
"  if (m == 0) return p;\n"
"  if (m == 1) return 1.0 - saturate(x);\n"
"  if (m == 2) return 2.0 * p - 1.0;\n"
"  if (m == 3) return -2.0 * p + 1.0;\n"
"  if (m == 4) return p - 0.5;\n"
"  if (m == 5) return -p + 0.5;\n"
"  if (m == 6) return x;\n"
"  return -x;\n"
"}\n"
"float3 rdc(uint b) {\n"
"  float4 v = R[b & 15];\n"
"  float3 x = (b & 0x10) ? v.aaa : v.rgb;\n"
"  return mapv(x, (b >> 5) & 7);\n"
"}\n"
"float rda(uint b) {\n"
"  float4 v = R[b & 15];\n"
"  float x = (b & 0x10) ? v.a : v.b;\n"
"  return mapv(float3(x, x, x), (b >> 5) & 7).x;\n"
"}\n"
"void omap(uint m, out float bias, out float scale) {\n"
"  bias = 0.0; scale = 1.0;\n"
"  if (m == 0x08) { bias = 0.5; }\n"
"  else if (m == 0x10) { scale = 2.0; }\n"
"  else if (m == 0x18) { bias = 0.5; scale = 2.0; }\n"
"  else if (m == 0x20) { scale = 4.0; }\n"
"  else if (m == 0x30) { scale = 0.5; }\n"
"}\n"
"float4 combine(float4 v0, float4 v1, float4 fog, float4 t0, float4 t1,\n"
"               float4 t2, float4 t3) {\n"
"  uint n = min(g_fin.z & 0xFF, 8u), flags = g_fin.z >> 8;\n"
"  [unroll] for (uint k = 0; k < 16; k++) R[k] = float4(0, 0, 0, 0);\n"
"  R[3] = fog; R[4] = v0; R[5] = v1;\n"
"  R[8] = t0; R[9] = t1; R[10] = t2; R[11] = t3;\n"
"  R[12].a = (g_fin.w & 0x1F) ? t0.a : 1.0;\n"
"  [loop] for (uint s = 0; s < n; s++) {\n"
"    uint icw = g_cw[s].x, ocw = g_cw[s].y, aicw = g_cw[s].z, aocw = g_cw[s].w;\n"
"    uint fl = ocw >> 12, afl = aocw >> 12;\n"
"    R[1] = g_f0[(flags & 0x010) ? s : 0];\n"
"    R[2] = g_f1[(flags & 0x100) ? s : 0];\n"
"    bool mux = (flags & 1) ? (R[12].a >= 0.5) : ((((int)(R[12].a * 255.0)) & 1) != 0);\n"
"    float3 A = rdc(icw >> 24), B = rdc(icw >> 16), C = rdc(icw >> 8), D = rdc(icw);\n"
"    float aA = rda(aicw >> 24), aB = rda(aicw >> 16), aC = rda(aicw >> 8), aD = rda(aicw);\n"
"    float ob, os, aob, aos;\n"
"    omap(fl & 0x38, ob, os);\n"
"    omap(afl & 0x38, aob, aos);\n"
"    float dab = dot(A, B), dcd = dot(C, D);\n"
"    float3 pab = (fl & 2) ? float3(dab, dab, dab) : A * B;\n"
"    float3 pcd = (fl & 1) ? float3(dcd, dcd, dcd) : C * D;\n"
"    float3 sum = (fl & 4) ? (mux ? pcd : pab) : pab + pcd;\n"
"    float3 ab = clamp((pab - ob) * os, -1.0, 1.0);\n"
"    float3 cd = clamp((pcd - ob) * os, -1.0, 1.0);\n"
"    float3 ms = clamp((sum - ob) * os, -1.0, 1.0);\n"
"    float qab = aA * aB, qcd = aC * aD;\n"
"    float qsum = (afl & 4) ? (mux ? qcd : qab) : qab + qcd;\n"
"    float aab = clamp((qab - aob) * aos, -1.0, 1.0);\n"
"    float acd = clamp((qcd - aob) * aos, -1.0, 1.0);\n"
"    float ams = clamp((qsum - aob) * aos, -1.0, 1.0);\n"
"    uint d;\n"
"    d = (ocw >> 4) & 15; if (d) { R[d].rgb = ab; if (fl & 0x80) R[d].a = ab.b; }\n"
"    d = ocw & 15;        if (d) { R[d].rgb = cd; if (fl & 0x40) R[d].a = cd.b; }\n"
"    d = (ocw >> 8) & 15; if (d) { R[d].rgb = ms; }\n"
"    d = (aocw >> 4) & 15; if (d) R[d].a = aab;\n"
"    d = aocw & 15;        if (d) R[d].a = acd;\n"
"    d = (aocw >> 8) & 15; if (d) R[d].a = ams;\n"
"    R[0] = float4(0, 0, 0, 0);\n"
"  }\n"
"  float4 o;\n"
"  if (g_fin.x != 0 || g_fin.y != 0) {\n"
"    uint f0 = g_fin.x, f1 = g_fin.y, ff = f1 & 0xFF;\n"
"    R[1] = g_fc0; R[2] = g_fc1;\n"
"    float3 a = (ff & 0x40) ? 1.0 - R[5].rgb : R[5].rgb;\n"
"    float3 b = (ff & 0x20) ? 1.0 - R[12].rgb : R[12].rgb;\n"
"    R[14] = float4((ff & 0x80) ? saturate(a + b) : a + b, 0.0);\n"
"    float3 E = rdc(f1 >> 24), F = rdc(f1 >> 16);\n"
"    R[15] = float4(E * F, 0.0);\n"
"    float3 A = rdc(f0 >> 24), B = rdc(f0 >> 16), C = rdc(f0 >> 8), D = rdc(f0);\n"
"    o.rgb = D + C * (1.0 - A) + B * A;\n"
"    o.a = rda(f1 >> 8);\n"
"  } else {\n"
"    o = R[12];\n"
"  }\n"
"  return saturate(o);\n"
"}\n"
/* rc_stage_fetch: what texture stage st contributes. */
/* Where stage st samples, worked out before any branch so its screen-space
 * derivatives can pick a mip level (SampleGrad; level 0 when the sampler's
 * MaxLOD is 0, which is every filter mode but RECOMP_GPU_TEXFILTER=2). */
"float2 suv(float4 c, uint st) {\n"
"  uint mode = (g_fin.w >> (st * 5)) & 0x1F;\n"
"  float q = (c.w != 0.0) ? c.w : 1.0;\n"
"  if (mode == 3) return c.xy * g_tscale[st].xy + g_tscale[st].zw;\n"
"  return float2(c.x / q, c.y / q) * g_tscale[st].xy + g_tscale[st].zw;\n"
"}\n"
/* B-spline bicubic from four bilinear taps, where the texture is magnified
 * (RECOMP_GPU_TEXFILTER=2): a 32x32 ground texture spread over a quarter of
 * the screen stops looking like blocks. Minified, the mips do the work. */
"float4 bicubic(Texture2D t, SamplerState sm, float2 uv, float2 gx, float2 gy) {\n"
"  float w, h, lv; t.GetDimensions(0, w, h, lv);\n"
"  float2 sz = float2(w, h);\n"
"  float2 d = max(abs(gx * sz), abs(gy * sz));\n"
"  if (max(d.x, d.y) >= 1.0) return t.SampleGrad(sm, uv, gx, gy);\n"
"  float2 p = uv * sz - 0.5, i = floor(p), f = p - i;\n"
"  float2 f2 = f * f, f3 = f2 * f;\n"
"  float2 w0 = (-f3 + 3.0 * f2 - 3.0 * f + 1.0) / 6.0;\n"
"  float2 w1 = (3.0 * f3 - 6.0 * f2 + 4.0) / 6.0;\n"
"  float2 w2 = (-3.0 * f3 + 3.0 * f2 + 3.0 * f + 1.0) / 6.0;\n"
"  float2 w3 = f3 / 6.0;\n"
"  float2 g0 = w0 + w1, g1 = w2 + w3;\n"
"  float2 h0 = (i - 0.5 + w1 / g0) / sz, h1 = (i + 1.5 + w3 / g1) / sz;\n"
"  float4 a = t.SampleGrad(sm, float2(h0.x, h0.y), gx, gy);\n"
"  float4 b = t.SampleGrad(sm, float2(h1.x, h0.y), gx, gy);\n"
"  float4 c = t.SampleGrad(sm, float2(h0.x, h1.y), gx, gy);\n"
"  float4 e = t.SampleGrad(sm, float2(h1.x, h1.y), gx, gy);\n"
"  return g0.y * (g0.x * a + g1.x * b) + g1.y * (g0.x * c + g1.x * e);\n"
"}\n"
"float4 stage(Texture2D t, SamplerState sm, uint st, float4 c, float2 uv, float2 gx,\n"
"             float2 gy, inout bool keep) {\n"
"  uint mode = (g_fin.w >> (st * 5)) & 0x1F;\n"
"  if (mode == 0) return float4(0, 0, 0, 1);\n"
"  if (mode == 1 || mode == 2 || mode == 3)\n"
"    return ((g_misc.y >> st) & 1) ? bicubic(t, sm, uv, gx, gy) : t.SampleGrad(sm, uv, gx, gy);\n"
"  if (mode == 4) return saturate(c);\n"
"  if (mode == 5) {\n"
"    [unroll] for (uint j = 0; j < 4; j++) {\n"
"      bool ge = ((g_misc.x >> (st * 4 + j)) & 1) != 0;\n"
"      if (ge ? (c[j] >= 0.0) : (c[j] < 0.0)) keep = false;\n"
"    }\n"
"    return float4(0, 0, 0, 0);\n"
"  }\n"
"  return float4(0, 0, 0, 0);\n"
"}\n"
"bool alpha_pass(float a) {\n"
"  int v = (int)(a * 255.0 + 0.5), r = (int)(g_mode.w & 0xFF);\n"
"  uint f = g_mode.z;\n"
"  if (f == 0x200) return false;\n"
"  if (f == 0x201) return v < r;\n"
"  if (f == 0x202) return v == r;\n"
"  if (f == 0x203) return v <= r;\n"
"  if (f == 0x204) return v > r;\n"
"  if (f == 0x205) return v != r;\n"
"  if (f == 0x206) return v >= r;\n"
"  return true;\n"
"}\n"
"float4 PSMain(PSIn i) : SV_Target {\n"
"  if (g_misc.z != 0) return g_fc0;\n"   /* RECOMP_SILHOUETTE pass: flat colour */
"  float4 o;\n"
"  float2 u0 = suv(i.t0, 0), u1 = suv(i.t1, 1), u2 = suv(i.t2, 2), u3 = suv(i.t3, 3);\n"
"  float2 un = i.t0.xy * g_tscale[0].xy + g_tscale[0].zw;\n"
"  float2 x0 = ddx(u0), y0 = ddy(u0), x1 = ddx(u1), y1 = ddy(u1);\n"
"  float2 x2 = ddx(u2), y2 = ddy(u2), x3 = ddx(u3), y3 = ddy(u3);\n"
"  float2 xn = ddx(un), yn = ddy(un);\n"
"  if (g_mode.x != 0) {\n"
"    bool keep = true;\n"
"    float4 t0 = stage(g_t0, g_s0, 0, i.t0, u0, x0, y0, keep);\n"
"    float4 t1 = stage(g_t1, g_s1, 1, i.t1, u1, x1, y1, keep);\n"
"    float4 t2 = stage(g_t2, g_s2, 2, i.t2, u2, x2, y2, keep);\n"
"    float4 t3 = stage(g_t3, g_s3, 3, i.t3, u3, x3, y3, keep);\n"
"    if (!keep) discard;\n"
"    o = combine(i.d0, i.d1, float4(g_fogc.rgb, i.fog.x), t0, t1, t2, t3);\n"
"  } else {\n"
"    o = i.d0;\n"
"    if (g_mode.y != 0) o *= g_t0.SampleGrad(g_s0, un, xn, yn);\n"
"    o = saturate(o);\n"
"  }\n"
"  if (g_mode.z != 0 && !alpha_pass(o.a)) discard;\n"
"  return o;\n"
"}\n";

/* ---- State ------------------------------------------------------------- */

typedef struct { float pos[4], d0[4], d1[4], fog[4], t[4][4]; } GVert;

typedef struct {
    float    vp[4];
    uint32_t mode[4];
    uint32_t fin[4];
    uint32_t misc[4];
    float    tscale[4][4];
    float    fogc[4];
    float    fc0[4], fc1[4];
    uint32_t cw[8][4];
    float    f0[8][4], f1[8][4];
    float    wsx[4];
} GCB;

#define G_MAX_RT    16
#define G_MAX_DEPTH 8
#define G_TEX_SLOTS 2048            /* open addressing; flushed at 3/4 full */
#define G_VB_BYTES  (16u << 20)
#define G_MAX_DIM   4096

typedef struct {
    uint32_t va, w, h, bpp, pitch;
    /* RECOMP_WIDESCREEN: the target is ew guest pixels wide (w when off);
     * the guest's w-wide picture sits centred, ox pixels in. */
    uint32_t ew;
    float ox;
    /* What guest memory under the target held when the GPU copy was made
     * (or last written back). Drawing never changes guest memory, so a
     * different fingerprint means the CPU has put something else there. */
    uint64_t guest_fp;
    uint32_t fp_checked;                /* frame of the last comparison */
    ID3D11Texture2D *tex;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv;
    uint32_t last_frame;
} GRt;

typedef struct {
    uint32_t va, w, h;
    ID3D11Texture2D *tex;
    ID3D11DepthStencilView *dsv;
    uint32_t last_frame;
} GDepth;

typedef struct {
    uint32_t used;                      /* slot occupied */
    uint32_t offset, color, w, h, pitch, palette;
    uint64_t hash;
    uint32_t checked;                   /* frame of the last hash check */
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;      /* NULL: could not decode (white) */
} GTex;

/* RECOMP_GPU_READBACK=2 (default): the flipped frame is copied back to guest
 * memory one or two flips late, through a ring of staging textures mapped
 * only once the GPU has finished them. Mapping at once (=1, the old way)
 * made the CPU wait for the whole frame to be drawn at every flip: in 16:9
 * that halved the frame rate (bar 45 -> 59 FPS without read-back). */
#define G_RB_N 3
typedef struct {
    ID3D11Texture2D *tex;
    uint32_t w, h;                      /* staging size */
    int pending;
    uint32_t va, pitch, bpp, cw, ch, frame;
    uint64_t mem_fp;                    /* guest memory there at the flip */
} GRb;

static struct {
    int tried, ok;
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    ID3D11DeviceContext1 *ctx1;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11InputLayout *layout;
    ID3D11Buffer *cb, *vb;
    uint32_t vb_pos;
    ID3D11RasterizerState *raster;
    ID3D11ShaderResourceView *white;
    GRt rt[G_MAX_RT];
    GDepth depth[G_MAX_DEPTH];
    GTex *tex;
    uint32_t tex_count;
    /* Copy of a render target the batch both draws into and samples. */
    ID3D11Texture2D *copy_tex;
    ID3D11ShaderResourceView *copy_srv;
    uint32_t copy_w, copy_h;
    /* Read-back ring (gb_resolve, rb_finish). */
    GRb rb[G_RB_N];
    uint32_t rb_next;
    /* RECOMP_GPU_SCALE: render targets and depth buffers are this many times
     * the guest size in each direction. Everything the guest sees (vertex
     * positions, texel scales, sizes in logs, read-back) stays in guest
     * pixels; only the textures, viewports, scissors and clear rectangles
     * grow. Read-back goes through a 1x copy (down_*). */
    uint32_t scale;
    /* RECOMP_GPU_TEXFILTER: 0 as the title asks (nearest without
     * combiners, like the software path); 1 "Smooth": textures the title
     * filters linearly get mipmaps and trilinear filtering; 2 "Best": that
     * plus bicubic (B-spline) magnification. Textures the title samples
     * nearest -- HUD strips, lookup tables -- stay nearest in every mode. */
    uint32_t texfilter;
    /* RECOMP_SILHOUETTE: the player's batches of this frame, drawn again
     * where something hides them (depth GREATER, stencil != 1). */
    int sil;
    float sil_color[4];
    GVert *sil_v;
    uint32_t sil_nv, sil_n, sil_frame;
    struct { uint32_t first, count; float vp[4], wsx[4]; } sil_b[64];
    Nv2aSurface sil_s;
    uint32_t sil_zeta;
    void *sil_rt;
    ID3D11Buffer *sil_vb;
    ID3D11BlendState *sil_bs;
    /* RECOMP_WIDESCREEN: full-screen targets (480 lines, 4:3 or wider) are
     * this much wider than the guest asks; 1.0 off. 16:9 is 4/3. */
    float ws;
    ID3D11Texture2D *down_tex;
    ID3D11RenderTargetView *down_rtv;
    uint32_t down_w, down_h;
    struct { uint32_t key; ID3D11SamplerState *s; } samp[64];
    struct { uint32_t key[3]; ID3D11BlendState *s; } blend[64];
    struct { uint32_t key; ID3D11DepthStencilState *s; } dss[32];
    uint32_t frame;
    int check_every_bind;
    uint32_t trace_frame;               /* RECOMP_GPU_TRACE: frame to log */
    /* Statistics, printed every 600 frames. */
    uint64_t t_backend, t_freq;
    uint32_t n_draws, n_clears, n_uploads, n_selfcopy, n_resolves, n_rt_dropped;
    uint64_t n_verts, n_hashed, n_upload_bytes;
} G;

static int g_blit_init(void);
static void g_blit(ID3D11ShaderResourceView *src, ID3D11RenderTargetView *dst,
                   float x, float y, float w, float h, int linear);

static void g_log_hr(const char *what, HRESULT hr)
{
    fprintf(stderr, "[GPUB] %s failed: 0x%08lX\n", what, (unsigned long)hr);
    fflush(stderr);
}

#define REL(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)

/* ---- Setup ------------------------------------------------------------- */

static int g_compile(const char *entry, const char *profile, ID3DBlob **out)
{
    ID3DBlob *err = NULL;
    HRESULT hr = D3DCompile(k_hlsl, sizeof k_hlsl - 1, "nv2a_backend_d3d11",
                            NULL, NULL, entry, profile,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[GPUB] %s (%s) does not compile: 0x%08lX\n%s\n",
                entry, profile, (unsigned long)hr,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
        fflush(stderr);
        REL(err);
        return 0;
    }
    REL(err);
    return 1;
}

static int g_init(void)
{
    static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1,
                                                D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got;
    ID3DBlob *vsb = NULL, *psb = NULL;
    HRESULT hr;
    const char *e;

    /* Laptops (Optimus/hybrid): the default adapter is often the integrated
       GPU. Prefer the high-performance one; RECOMP_GPU_ADAPTER=default keeps
       the system default. */
    IDXGIAdapter1 *adapter = NULL;
    e = getenv("RECOMP_GPU_ADAPTER");
    if (!e || strcmp(e, "default") != 0) {
        IDXGIFactory6 *f6 = NULL;
        if (SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory6, (void **)&f6))) {
            if (FAILED(IDXGIFactory6_EnumAdapterByGpuPreference(
                    f6, 0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                    &IID_IDXGIAdapter1, (void **)&adapter)))
                adapter = NULL;
            IDXGIFactory6_Release(f6);
        }
    }
    {
        D3D_DRIVER_TYPE dt = adapter ? D3D_DRIVER_TYPE_UNKNOWN
                                     : D3D_DRIVER_TYPE_HARDWARE;
        IDXGIAdapter *ad = (IDXGIAdapter *)adapter;
        hr = D3D11CreateDevice(ad, dt, NULL, 0, levels, 2,
                               D3D11_SDK_VERSION, &G.dev, &got, &G.ctx);
        if (FAILED(hr))
            hr = D3D11CreateDevice(ad, dt, NULL, 0, levels + 1, 1,
                                   D3D11_SDK_VERSION, &G.dev, &got, &G.ctx);
        if (FAILED(hr) && adapter)
            hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                                   levels, 2, D3D11_SDK_VERSION, &G.dev, &got,
                                   &G.ctx);
    }
    if (SUCCEEDED(hr)) {
        IDXGIDevice *xd = NULL;
        IDXGIAdapter *used = NULL;
        DXGI_ADAPTER_DESC desc;
        if (SUCCEEDED(ID3D11Device_QueryInterface(G.dev, &IID_IDXGIDevice,
                                                  (void **)&xd)) &&
            SUCCEEDED(IDXGIDevice_GetAdapter(xd, &used)) &&
            SUCCEEDED(IDXGIAdapter_GetDesc(used, &desc))) {
            fprintf(stderr, "[GPUB] adapter: %ls (%u MB VRAM)\n",
                    desc.Description,
                    (unsigned)(desc.DedicatedVideoMemory >> 20));
            fflush(stderr);
        }
        if (used) IDXGIAdapter_Release(used);
        if (xd) IDXGIDevice_Release(xd);
    }
    if (adapter) IDXGIAdapter1_Release(adapter);
    if (FAILED(hr)) {
        g_log_hr("D3D11CreateDevice", hr);
        return 0;
    }
    if (FAILED(ID3D11DeviceContext_QueryInterface(G.ctx, &IID_ID3D11DeviceContext1,
                                                  (void **)&G.ctx1)))
        G.ctx1 = NULL;

    if (!g_compile("VSMain", "vs_5_0", &vsb) || !g_compile("PSMain", "ps_5_0", &psb))
        return 0;
    hr = ID3D11Device_CreateVertexShader(G.dev, ID3D10Blob_GetBufferPointer(vsb),
                                         ID3D10Blob_GetBufferSize(vsb), NULL, &G.vs);
    if (FAILED(hr)) { g_log_hr("CreateVertexShader", hr); return 0; }
    hr = ID3D11Device_CreatePixelShader(G.dev, ID3D10Blob_GetBufferPointer(psb),
                                        ID3D10Blob_GetBufferSize(psb), NULL, &G.ps);
    if (FAILED(hr)) { g_log_hr("CreatePixelShader", hr); return 0; }
    {
        static const D3D11_INPUT_ELEMENT_DESC el[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,   0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 4, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  48, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  64, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  80, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  96, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 112, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        hr = ID3D11Device_CreateInputLayout(G.dev, el, 8,
                                            ID3D10Blob_GetBufferPointer(vsb),
                                            ID3D10Blob_GetBufferSize(vsb), &G.layout);
        if (FAILED(hr)) { g_log_hr("CreateInputLayout", hr); return 0; }
    }
    REL(vsb);
    REL(psb);
    {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = sizeof(GCB);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = ID3D11Device_CreateBuffer(G.dev, &bd, NULL, &G.cb);
        if (FAILED(hr)) { g_log_hr("CreateBuffer(cb)", hr); return 0; }
        bd.ByteWidth = G_VB_BYTES;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        hr = ID3D11Device_CreateBuffer(G.dev, &bd, NULL, &G.vb);
        if (FAILED(hr)) { g_log_hr("CreateBuffer(vb)", hr); return 0; }
        G.vb_pos = G_VB_BYTES;          /* first map discards */
    }
    {
        D3D11_RASTERIZER_DESC rd;
        memset(&rd, 0, sizeof rd);
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = FALSE;
        rd.ScissorEnable = TRUE;
        hr = ID3D11Device_CreateRasterizerState(G.dev, &rd, &G.raster);
        if (FAILED(hr)) { g_log_hr("CreateRasterizerState", hr); return 0; }
    }
    {
        /* An unusable stage reads white (rc_texel). */
        static const uint32_t px = 0xFFFFFFFFu;
        D3D11_TEXTURE2D_DESC td;
        D3D11_SUBRESOURCE_DATA sd;
        ID3D11Texture2D *t = NULL;
        memset(&td, 0, sizeof td);
        td.Width = td.Height = 1;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        sd.pSysMem = &px; sd.SysMemPitch = 4; sd.SysMemSlicePitch = 4;
        hr = ID3D11Device_CreateTexture2D(G.dev, &td, &sd, &t);
        if (FAILED(hr)) { g_log_hr("CreateTexture2D(white)", hr); return 0; }
        hr = ID3D11Device_CreateShaderResourceView(G.dev, (ID3D11Resource *)t,
                                                   NULL, &G.white);
        REL(t);
        if (FAILED(hr)) { g_log_hr("CreateShaderResourceView(white)", hr); return 0; }
    }
    G.tex = (GTex *)calloc(G_TEX_SLOTS, sizeof *G.tex);
    if (!G.tex)
        return 0;
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        G.t_freq = (uint64_t)f.QuadPart;
    }
    e = getenv("RECOMP_GPU_TEX_CHECK");
    G.check_every_bind = e && strcmp(e, "batch") == 0;
    e = getenv("RECOMP_GPU_TRACE");
    G.trace_frame = e ? (uint32_t)atoi(e) : 0;
    e = getenv("RECOMP_GPU_TEXFILTER");
    G.texfilter = e ? (uint32_t)atoi(e) : 0;
    if (G.texfilter > 2) G.texfilter = 2;
    if (G.texfilter) {
        fprintf(stderr, "[GPUB] texture filtering: %s\n",
                G.texfilter == 2 ? "mipmaps, trilinear, bicubic magnification"
                                 : "mipmaps, trilinear");
        fflush(stderr);
    }
    e = getenv("RECOMP_SILHOUETTE");
    if (e && *e && *e != '0') {
        uint32_t argb = 0x9050B0FFu;    /* light blue, about half see-through */
        const char *c = getenv("RECOMP_SILHOUETTE_COLOR");
        if (c && *c)
            argb = (uint32_t)strtoul(c, NULL, 16);
        /* 2: debugging -- the whole player in the colour, hidden or not,
         * to see which batches count as the player. */
        G.sil = atoi(e) == 2 ? 2 : 1;
        G.sil_color[0] = (float)((argb >> 16) & 0xFF) / 255.0f;
        G.sil_color[1] = (float)((argb >> 8) & 0xFF) / 255.0f;
        G.sil_color[2] = (float)(argb & 0xFF) / 255.0f;
        G.sil_color[3] = (float)(argb >> 24) / 255.0f;
        fprintf(stderr, "[GPUB] player silhouette behind walls (colour %08X)\n", argb);
    }
    G.ws = 1.0f;
    e = getenv("RECOMP_WIDESCREEN");
    if (e && *e && strcmp(e, "0") != 0) {
        float a = 16.0f / 9.0f;
        const char *c = strchr(e, ':');
        if (c && atof(e) > 0.0 && atof(c + 1) > 0.0)
            a = (float)(atof(e) / atof(c + 1));
        if (a > 4.0f / 3.0f + 0.01f && a < 4.0f) {
            G.ws = a * 3.0f / 4.0f;
            fprintf(stderr, "[GPUB] widescreen: %.3f:1, full-screen targets %.3fx wider\n",
                    a, G.ws);
            fflush(stderr);
        }
    }
    e = getenv("RECOMP_GPU_SCALE");
    G.scale = e ? (uint32_t)atoi(e) : 1;
    if (G.scale < 1) G.scale = 1;
    if (G.scale > 4) G.scale = 4;
    if (G.scale > 1) {
        fprintf(stderr, "[GPUB] rendering at %ux the guest resolution\n", G.scale);
        fflush(stderr);
    }
    fprintf(stderr, "[GPUB] Direct3D 11 device, feature level %x.%x%s\n",
            (unsigned)got >> 12, ((unsigned)got >> 8) & 0xF,
            G.ctx1 ? "" : " (no ClearView: colour clears ignore the clip rect)");
    fflush(stderr);
    return 1;
}

/* ---- Render targets ---------------------------------------------------- */

static uint64_t g_hash(const uint8_t *p, uint32_t n, uint64_t h);

static uint64_t g_rt_fp(const GRt *r)
{
    uint32_t bytes = r->pitch ? r->pitch * r->h : r->w * r->h * r->bpp;
    if (!nv2a_backend_surface_ok(r->va, bytes))
        return 0;
    G.n_hashed += bytes;
    return g_hash(nv2a_backend_guest(r->va), bytes, 0);
}

static void g_unpack_565(uint16_t t, uint8_t *bgra)
{
    bgra[0] = (uint8_t)((t & 0x001F) << 3);
    bgra[1] = (uint8_t)((t & 0x07E0) >> 3);
    bgra[2] = (uint8_t)((t & 0xF800) >> 8);
    bgra[3] = 0xFF;
}

/* RECOMP_WIDESCREEN: how wide a w x h target is drawn. Only the frame
 * itself and the title's 2x anti-aliasing target (640 or 1280 x 480) widen;
 * shadow maps, render-to-texture and anything smaller keep their size. */
static uint32_t g_ws_width(uint32_t w, uint32_t h)
{
    if (G.ws <= 1.0f || h < 448 || w * 3 < h * 4)
        return w;
    w = (uint32_t)((float)w * G.ws + 0.5f);
    return (w + 1) & ~1u;               /* even: the picture stays centred */
}

static int g_rt_create(GRt *r, uint32_t va, uint32_t w, uint32_t h,
                       uint32_t bpp, uint32_t pitch)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sd, *init = NULL;
    uint8_t *px = NULL;
    HRESULT hr;

    /* Start from what guest memory holds: a title may draw over a surface
     * it did not clear, or over what the CPU wrote there. */
    if (pitch && nv2a_backend_surface_ok(va, pitch * h)
        && (px = (uint8_t *)malloc((size_t)w * h * 4)) != NULL) {
        const uint8_t *src = nv2a_backend_guest(va);
        uint32_t y, x;
        for (y = 0; y < h; y++) {
            const uint8_t *row = src + (size_t)y * pitch;
            uint8_t *dst = px + (size_t)y * w * 4;
            if (bpp == 4)
                memcpy(dst, row, (size_t)w * 4);
            else
                for (x = 0; x < w; x++)
                    g_unpack_565(((const uint16_t *)row)[x], dst + x * 4);
        }
        sd.pSysMem = px;
        sd.SysMemPitch = w * 4;
        sd.SysMemSlicePitch = w * h * 4;
        init = &sd;
    }
    r->ew = g_ws_width(w, h);
    r->ox = (float)(r->ew - w) * 0.5f;
    memset(&td, 0, sizeof td);
    td.Width = r->ew * G.scale;
    td.Height = h * G.scale;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    hr = ID3D11Device_CreateTexture2D(G.dev, &td,
                                      G.scale == 1 && r->ew == w ? init : NULL, &r->tex);
    if (FAILED(hr)) { g_log_hr("CreateTexture2D(rt)", hr); free(px); return 0; }
    hr = ID3D11Device_CreateRenderTargetView(G.dev, (ID3D11Resource *)r->tex,
                                             NULL, &r->rtv);
    if (FAILED(hr)) { g_log_hr("CreateRenderTargetView", hr); REL(r->tex); return 0; }
    hr = ID3D11Device_CreateShaderResourceView(G.dev, (ID3D11Resource *)r->tex,
                                               NULL, &r->srv);
    if (FAILED(hr)) { g_log_hr("CreateShaderResourceView(rt)", hr);
                      REL(r->rtv); REL(r->tex); free(px); return 0; }
    if (G.scale > 1 || r->ew != w) {
        /* Guest contents at 1x, stretched (nearest) to the scaled target
         * (centred in a wide one, black either side); otherwise a scaled
         * target starts black. */
        ID3D11Texture2D *tmp = NULL;
        ID3D11ShaderResourceView *tsrv = NULL;
        float clr[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        ID3D11DeviceContext_ClearRenderTargetView(G.ctx, r->rtv, clr);
        if (init) {
            td.Width = w;
            td.Height = h;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (SUCCEEDED(ID3D11Device_CreateTexture2D(G.dev, &td, init, &tmp)) &&
                SUCCEEDED(ID3D11Device_CreateShaderResourceView(G.dev,
                              (ID3D11Resource *)tmp, NULL, &tsrv)) &&
                g_blit_init())
                g_blit(tsrv, r->rtv, r->ox * (float)G.scale, 0.0f,
                       (float)(w * G.scale), (float)(h * G.scale), 0);
            REL(tsrv);
            REL(tmp);
        }
    }
    free(px);
    r->va = va; r->w = w; r->h = h; r->bpp = bpp; r->pitch = pitch;
    r->last_frame = G.frame;
    r->guest_fp = g_rt_fp(r);
    r->fp_checked = G.frame;
    fprintf(stderr, "[GPUB] render target 0x%08X %ux%u %ubpp\n", va, w, h, bpp * 8);
    return 1;
}

static void g_rt_release(GRt *r)
{
    REL(r->srv);
    REL(r->rtv);
    REL(r->tex);
    memset(r, 0, sizeof *r);
}

static GRt *g_rt_find(uint32_t va)
{
    int i;
    for (i = 0; i < G_MAX_RT; i++)
        if (G.rt[i].tex && G.rt[i].va == va)
            return &G.rt[i];
    return NULL;
}

/* The render target for a surface, created or grown as needed. */
static GRt *g_rt_get(const Nv2aSurface *s)
{
    uint32_t bpp = s->bytes_per_pixel;
    uint32_t w = s->pitch ? s->pitch / bpp : s->clip_x + s->width;
    uint32_t h = s->clip_y + s->height;
    GRt *r;
    int i, oldest = 0;

    if ((bpp != 2 && bpp != 4) || !s->color_va || !w || !h)
        return NULL;
    if (w > G_MAX_DIM) w = G_MAX_DIM;
    if (h > G_MAX_DIM) h = G_MAX_DIM;
    r = g_rt_find(s->color_va);
    if (r && r->bpp == bpp && r->w == w && r->h >= h) {
        r->last_frame = G.frame;
        return r;
    }
    if (r) {
        /* Same address, another shape: keep what overlaps. */
        GRt n;
        memset(&n, 0, sizeof n);
        if (h < r->h && r->w == w) h = r->h;
        if (!g_rt_create(&n, s->color_va, w, h, bpp, s->pitch))
            return NULL;
        if (r->bpp == bpp) {
            D3D11_BOX box;
            box.left = 0; box.top = 0; box.front = 0;
            box.right = (r->ew < n.ew ? r->ew : n.ew) * G.scale;
            box.bottom = (r->h < h ? r->h : h) * G.scale;
            box.back = 1;
            ID3D11DeviceContext_CopySubresourceRegion(G.ctx, (ID3D11Resource *)n.tex,
                                                      0, 0, 0, 0,
                                                      (ID3D11Resource *)r->tex, 0, &box);
        }
        g_rt_release(r);
        *r = n;
        return r;
    }
    for (i = 0; i < G_MAX_RT; i++) {
        if (!G.rt[i].tex)
            break;
        if (G.rt[i].last_frame < G.rt[oldest].last_frame)
            oldest = i;
    }
    if (i == G_MAX_RT) {
        i = oldest;
        g_rt_release(&G.rt[i]);
    }
    if (!g_rt_create(&G.rt[i], s->color_va, w, h, bpp, s->pitch))
        return NULL;
    return &G.rt[i];
}

static GDepth *g_depth_get(uint32_t va, uint32_t w, uint32_t h, int *fresh)
{
    D3D11_TEXTURE2D_DESC td;
    HRESULT hr;
    int i, oldest = 0;

    *fresh = 0;
    for (i = 0; i < G_MAX_DEPTH; i++)
        if (G.depth[i].tex && G.depth[i].va == va && G.depth[i].w == w
            && G.depth[i].h == h) {
            G.depth[i].last_frame = G.frame;
            return &G.depth[i];
        }
    for (i = 0; i < G_MAX_DEPTH; i++) {
        if (!G.depth[i].tex)
            break;
        if (G.depth[i].last_frame < G.depth[oldest].last_frame)
            oldest = i;
    }
    if (i == G_MAX_DEPTH) {
        i = oldest;
        REL(G.depth[i].dsv);
        REL(G.depth[i].tex);
    }
    memset(&td, 0, sizeof td);
    td.Width = w * G.scale;
    td.Height = h * G.scale;
    td.MipLevels = td.ArraySize = 1;
    /* A stencil too for RECOMP_SILHOUETTE (which pixels show the player). */
    td.Format = G.sil ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    hr = ID3D11Device_CreateTexture2D(G.dev, &td, NULL, &G.depth[i].tex);
    if (FAILED(hr)) { g_log_hr("CreateTexture2D(depth)", hr); return NULL; }
    hr = ID3D11Device_CreateDepthStencilView(G.dev, (ID3D11Resource *)G.depth[i].tex,
                                             NULL, &G.depth[i].dsv);
    if (FAILED(hr)) { g_log_hr("CreateDepthStencilView", hr); REL(G.depth[i].tex); return NULL; }
    G.depth[i].va = va;
    G.depth[i].w = w;
    G.depth[i].h = h;
    G.depth[i].last_frame = G.frame;
    /* The software path starts a new depth buffer at "infinitely far". */
    ID3D11DeviceContext_ClearDepthStencilView(G.ctx, G.depth[i].dsv,
                                              D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    *fresh = 1;
    return &G.depth[i];
}

/* ---- Textures ---------------------------------------------------------- */

/* Change detection only (never stored): four independent lanes so the
 * multiply chains overlap -- one chain ran at ~6 GB/s, and 16:9 hashes
 * ~8 MB of textures and targets a frame. */
static uint64_t g_hash(const uint8_t *p, uint32_t n, uint64_t h)
{
    uint32_t i;
    uint64_t h0, h1, h2, h3;
    h ^= 0x9E3779B97F4A7C15ull + n;
    h0 = h; h1 = h ^ 0x6A09E667F3BCC909ull;
    h2 = h ^ 0xBB67AE8584CAA73Bull; h3 = h ^ 0x3C6EF372FE94F82Bull;
    for (i = 0; i + 32 <= n; i += 32) {
        uint64_t w0, w1, w2, w3;
        memcpy(&w0, p + i, 8); memcpy(&w1, p + i + 8, 8);
        memcpy(&w2, p + i + 16, 8); memcpy(&w3, p + i + 24, 8);
        h0 = (h0 ^ w0) * 0xFF51AFD7ED558CCDull; h0 ^= h0 >> 32;
        h1 = (h1 ^ w1) * 0xFF51AFD7ED558CCDull; h1 ^= h1 >> 32;
        h2 = (h2 ^ w2) * 0xFF51AFD7ED558CCDull; h2 ^= h2 >> 32;
        h3 = (h3 ^ w3) * 0xFF51AFD7ED558CCDull; h3 ^= h3 >> 32;
    }
    h = h0 ^ (h1 * 0x9E3779B97F4A7C15ull) ^ (h2 * 0xC2B2AE3D27D4EB4Full)
          ^ (h3 * 0x165667B19E3779F9ull);
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

static uint32_t g_tex_slot(const Nv2aTexture *t)
{
    uint32_t k = t->offset * 0x9E3779B1u ^ t->color * 0x85EBCA6Bu
               ^ (t->width << 16 | t->height) * 0xC2B2AE35u ^ t->palette;
    return (k ^ (k >> 15)) & (G_TEX_SLOTS - 1);
}

static void g_tex_flush(void)
{
    uint32_t i;
    for (i = 0; i < G_TEX_SLOTS; i++) {
        REL(G.tex[i].srv);
        REL(G.tex[i].tex);
    }
    memset(G.tex, 0, sizeof(GTex) * G_TEX_SLOTS);
    G.tex_count = 0;
}

static uint64_t g_tex_content(const Nv2aTexture *t, uint32_t bytes)
{
    uint64_t h = g_hash(nv2a_backend_guest(t->offset), bytes, 0);
    if (t->palette)
        h = g_hash(nv2a_backend_guest(t->palette), 256 * 4, h);
    G.n_hashed += bytes + (t->palette ? 1024 : 0);
    return h;
}

static void g_tex_upload(GTex *e, const Nv2aTexture *t)
{
    uint32_t *px = (uint32_t *)malloc((size_t)t->width * t->height * 4);
    D3D11_TEXTURE2D_DESC td;
    HRESULT hr;

    if (!px)
        return;
    if (!nv2a_backend_decode_texture(t, px)) {
        free(px);
        REL(e->srv);
        REL(e->tex);
        return;
    }
    if (e->tex) {
        ID3D11DeviceContext_UpdateSubresource(G.ctx, (ID3D11Resource *)e->tex, 0,
                                              NULL, px, t->width * 4, 0);
        if (G.texfilter && e->srv)
            ID3D11DeviceContext_GenerateMips(G.ctx, e->srv);
    } else if (G.texfilter) {
        /* Full mip chain, built on the GPU from level 0. */
        memset(&td, 0, sizeof td);
        td.Width = t->width;
        td.Height = t->height;
        td.MipLevels = 0;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        hr = ID3D11Device_CreateTexture2D(G.dev, &td, NULL, &e->tex);
        if (SUCCEEDED(hr))
            hr = ID3D11Device_CreateShaderResourceView(G.dev, (ID3D11Resource *)e->tex,
                                                       NULL, &e->srv);
        if (SUCCEEDED(hr)) {
            ID3D11DeviceContext_UpdateSubresource(G.ctx, (ID3D11Resource *)e->tex, 0,
                                                  NULL, px, t->width * 4, 0);
            ID3D11DeviceContext_GenerateMips(G.ctx, e->srv);
        } else {
            g_log_hr("texture (mips)", hr);
            REL(e->srv);
            REL(e->tex);
        }
    } else {
        D3D11_SUBRESOURCE_DATA sd;
        memset(&td, 0, sizeof td);
        td.Width = t->width;
        td.Height = t->height;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        sd.pSysMem = px;
        sd.SysMemPitch = t->width * 4;
        sd.SysMemSlicePitch = 0;
        hr = ID3D11Device_CreateTexture2D(G.dev, &td, &sd, &e->tex);
        if (SUCCEEDED(hr))
            hr = ID3D11Device_CreateShaderResourceView(G.dev, (ID3D11Resource *)e->tex,
                                                       NULL, &e->srv);
        if (FAILED(hr)) {
            g_log_hr("texture", hr);
            REL(e->tex);
        }
    }
    free(px);
    G.n_uploads++;
    G.n_upload_bytes += (uint64_t)t->width * t->height * 4;
}

/* The view a stage samples and the size of what it samples (for turning
 * coordinates into [0,1]). `rt_out` is set when that is a render target. */
static ID3D11ShaderResourceView *g_tex_srv(const Nv2aTexture *t, float *tw,
                                           float *th, GRt **rt_out)
{
    GRt *rt;
    GTex *e;
    uint32_t slot, n, bytes;

    *rt_out = NULL;
    *tw = (float)(t->width ? t->width : 1);
    *th = (float)(t->height ? t->height : 1);
    if ((rt = g_rt_find(t->offset)) != NULL && rt->fp_checked != G.frame) {
        /* Sampled as a texture: still the GPU's, or has the CPU reused the
         * memory (a texture streamed into a freed surface)? */
        rt->fp_checked = G.frame;
        if (g_rt_fp(rt) != rt->guest_fp) {
            if (G.n_rt_dropped++ < 20)
                fprintf(stderr, "[GPUB] render target 0x%08X %ux%u: guest memory"
                                " rewritten, sampling it from memory again\n",
                        rt->va, rt->w, rt->h);
            g_rt_release(rt);
            rt = NULL;
        }
    }
    if (rt) {
        *tw = (float)rt->w;
        *th = (float)rt->h;
        *rt_out = rt;
        return rt->srv;
    }
    if (!t->offset || !t->width || !t->height || t->width > G_MAX_DIM
        || t->height > G_MAX_DIM)
        return G.white;
    bytes = nv2a_backend_texture_bytes(t);
    if (!bytes)
        return G.white;

    slot = g_tex_slot(t);
    for (n = 0; n < G_TEX_SLOTS; n++, slot = (slot + 1) & (G_TEX_SLOTS - 1)) {
        e = &G.tex[slot];
        if (!e->used)
            break;
        if (e->offset == t->offset && e->color == t->color && e->w == t->width
            && e->h == t->height && e->pitch == t->pitch
            && e->palette == t->palette) {
            if (e->checked != G.frame || G.check_every_bind) {
                uint64_t h = g_tex_content(t, bytes);
                e->checked = G.frame;
                if (h != e->hash) {
                    e->hash = h;
                    g_tex_upload(e, t);
                }
            }
            return e->srv ? e->srv : G.white;
        }
    }
    if (G.tex_count >= G_TEX_SLOTS * 3 / 4) {
        fprintf(stderr, "[GPUB] texture cache full (%u), flushed\n", G.tex_count);
        g_tex_flush();
        return g_tex_srv(t, tw, th, rt_out);
    }
    e = &G.tex[slot];
    memset(e, 0, sizeof *e);
    e->used = 1;
    e->offset = t->offset; e->color = t->color; e->w = t->width; e->h = t->height;
    e->pitch = t->pitch; e->palette = t->palette;
    e->hash = g_tex_content(t, bytes);
    e->checked = G.frame;
    G.tex_count++;
    g_tex_upload(e, t);
    return e->srv ? e->srv : G.white;
}

/* ---- Fixed-function state ---------------------------------------------- */

static ID3D11SamplerState *g_sampler(int linear, uint32_t au, uint32_t av)
{
    uint32_t key = (uint32_t)linear << 16 | (au & 0xFF) << 8 | (av & 0xFF), i;
    D3D11_SAMPLER_DESC sd;
    static const D3D11_TEXTURE_ADDRESS_MODE map[] = {
        D3D11_TEXTURE_ADDRESS_CLAMP,  /* 0 */
        D3D11_TEXTURE_ADDRESS_WRAP,   /* 1 wrap */
        D3D11_TEXTURE_ADDRESS_MIRROR, /* 2 mirror */
        D3D11_TEXTURE_ADDRESS_CLAMP,  /* 3 clamp to edge */
        D3D11_TEXTURE_ADDRESS_BORDER, /* 4 border */
        D3D11_TEXTURE_ADDRESS_CLAMP,  /* 5 clamp */
    };
    for (i = 0; i < 64 && G.samp[i].s; i++)
        if (G.samp[i].key == key)
            return G.samp[i].s;
    if (i == 64)
        return G.samp[0].s;
    memset(&sd, 0, sizeof sd);
    /* linear: 0 nearest, 1 bilinear (level 0), 2 trilinear over the mips.
     * (D3D11_FILTER_ANISOTROPIC was tried for 2: on the RTX 5070 Laptop it
     * magnified with nearest texels, which looked like no filtering at all;
     * the camera here is nearly top-down, so anisotropy buys little.) */
    sd.Filter = linear ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = map[au < 6 ? au : 0];
    sd.AddressV = map[av < 6 ? av : 0];
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxAnisotropy = 1;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MinLOD = 0.0f;
    sd.MaxLOD = linear == 2 ? D3D11_FLOAT32_MAX : 0.0f;
    {
        HRESULT hr = ID3D11Device_CreateSamplerState(G.dev, &sd, &G.samp[i].s);
        if (FAILED(hr)) {
            g_log_hr("CreateSamplerState", hr);
            return G.samp[0].s;
        }
    }
    G.samp[i].key = key;
    return G.samp[i].s;
}

/* NV2A (OpenGL) blend factor to D3D11; `alpha` for the alpha channel, which
 * takes no colour factors. */
static D3D11_BLEND g_blend_factor(uint32_t f, int alpha)
{
    switch (f) {
    case 0x0000: return D3D11_BLEND_ZERO;
    case 0x0001: return D3D11_BLEND_ONE;
    case 0x0300: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x0301: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D11_BLEND_SRC_ALPHA;
    case 0x0303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D11_BLEND_DEST_ALPHA;
    case 0x0305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x0306: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x0307: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x0308: return alpha ? D3D11_BLEND_ONE : D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default:     return D3D11_BLEND_ONE;
    }
}

static D3D11_BLEND_OP g_blend_op(uint32_t eq)
{
    switch (eq) {
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: case 0xF005: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    default:     return D3D11_BLEND_OP_ADD;
    }
}

static ID3D11BlendState *g_blend(const Nv2aRenderState *rs)
{
    uint32_t on = rs->blend_enable && !(rs->blend_src == 1 && rs->blend_dst == 0);
    uint32_t mask = ((rs->color_mask & 0x00010000u) ? D3D11_COLOR_WRITE_ENABLE_RED : 0)
                  | ((rs->color_mask & 0x00000100u) ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0)
                  | ((rs->color_mask & 0x00000001u) ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0)
                  | ((rs->color_mask & 0x01000000u) ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0);
    uint32_t key[3], i;
    D3D11_BLEND_DESC bd;

    key[0] = on << 8 | mask;
    key[1] = on ? (rs->blend_src << 16 | (rs->blend_dst & 0xFFFF)) : 0;
    key[2] = on ? rs->blend_eq : 0;
    for (i = 0; i < 64 && G.blend[i].s; i++)
        if (!memcmp(G.blend[i].key, key, sizeof key))
            return G.blend[i].s;
    if (i == 64) {
        REL(G.blend[63].s);
        i = 63;
    }
    memset(&bd, 0, sizeof bd);
    bd.RenderTarget[0].BlendEnable = on ? TRUE : FALSE;
    bd.RenderTarget[0].SrcBlend = g_blend_factor(rs->blend_src, 0);
    bd.RenderTarget[0].DestBlend = g_blend_factor(rs->blend_dst, 0);
    bd.RenderTarget[0].BlendOp = g_blend_op(rs->blend_eq);
    bd.RenderTarget[0].SrcBlendAlpha = g_blend_factor(rs->blend_src, 1);
    bd.RenderTarget[0].DestBlendAlpha = g_blend_factor(rs->blend_dst, 1);
    bd.RenderTarget[0].BlendOpAlpha = g_blend_op(rs->blend_eq);
    bd.RenderTarget[0].RenderTargetWriteMask = (UINT8)mask;
    if (FAILED(ID3D11Device_CreateBlendState(G.dev, &bd, &G.blend[i].s)))
        return NULL;
    memcpy(G.blend[i].key, key, sizeof key);
    return G.blend[i].s;
}

/* smode (RECOMP_SILHOUETTE): 0 stencil untouched; 1 the stencil takes the
 * reference (1 player, 0 anything else) where the batch passes the depth
 * test; 2 the silhouette pass -- depth GREATER, no writes, stencil != 1. */
static ID3D11DepthStencilState *g_dss_s(const Nv2aRenderState *rs, int has_depth, int smode);
static ID3D11DepthStencilState *g_dss(const Nv2aRenderState *rs, int has_depth)
{
    return g_dss_s(rs, has_depth, 0);
}

static ID3D11DepthStencilState *g_dss_s(const Nv2aRenderState *rs, int has_depth, int smode)
{
    /* The software path writes depth whenever DEPTH_MASK is set, tested or
     * not; D3D writes only with the test on, so "not tested" is ALWAYS. */
    uint32_t test = has_depth && rs && rs->depth_test_enable;
    uint32_t write = has_depth && rs && rs->depth_write;
    uint32_t func = test ? rs->depth_func : 0x207;
    uint32_t key, i;
    D3D11_DEPTH_STENCIL_DESC dd;

    if (smode == 2) {
        test = 1;
        write = 0;
        func = G.sil == 2 ? 0x207 : 0x204;  /* GREATER (2: ALWAYS) */
    }
    key = (uint32_t)smode << 20 | (test || write) << 16 | write << 12 | (func & 0xFFF);

    for (i = 0; i < 32 && G.dss[i].s; i++)
        if (G.dss[i].key == key)
            return G.dss[i].s;
    if (i == 32) {
        REL(G.dss[31].s);
        i = 31;
    }
    memset(&dd, 0, sizeof dd);
    dd.DepthEnable = (test || write) ? TRUE : FALSE;
    dd.DepthWriteMask = write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = (func >= 0x200 && func <= 0x207)
                 ? (D3D11_COMPARISON_FUNC)(func - 0x200 + 1) : D3D11_COMPARISON_ALWAYS;
    dd.StencilEnable = FALSE;
    if (smode) {
        dd.StencilEnable = TRUE;
        dd.StencilReadMask = dd.StencilWriteMask = 0xFF;
        dd.FrontFace.StencilFailOp = dd.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
        dd.FrontFace.StencilPassOp = smode == 1 ? D3D11_STENCIL_OP_REPLACE : D3D11_STENCIL_OP_KEEP;
        dd.FrontFace.StencilFunc = smode == 1 || G.sil == 2 ? D3D11_COMPARISON_ALWAYS
                                                             : D3D11_COMPARISON_NOT_EQUAL;
        dd.BackFace = dd.FrontFace;
    }
    if (FAILED(ID3D11Device_CreateDepthStencilState(G.dev, &dd, &G.dss[i].s)))
        return NULL;
    G.dss[i].key = key;
    return G.dss[i].s;
}

static void g_unpack(uint32_t argb, float o[4])
{
    o[0] = (float)((argb >> 16) & 0xFF) / 255.0f;
    o[1] = (float)((argb >> 8) & 0xFF) / 255.0f;
    o[2] = (float)(argb & 0xFF) / 255.0f;
    o[3] = (float)(argb >> 24) / 255.0f;
}

static void g_time_begin(LARGE_INTEGER *t0) { QueryPerformanceCounter(t0); }
static void g_time_end(const LARGE_INTEGER *t0)
{
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    G.t_backend += (uint64_t)(t1.QuadPart - t0->QuadPart);
}

/* ---- Back end ---------------------------------------------------------- */

static nv2a_focus_fn s_focus_hook;
void nv2a_backend_set_focus_hook(nv2a_focus_fn fn) { s_focus_hook = fn; }
static struct { uint32_t frame; int ok; float v[4]; } s_focus = { 0xFFFFFFFFu, 0, { 0 } };

static int g_focus(void)
{
    if (s_focus.frame != G.frame) {
        s_focus.frame = G.frame;
        s_focus.ok = s_focus_hook ? s_focus_hook(s_focus.v) : 0;
    }
    return s_focus.ok;
}

/* RECOMP_FOCUS_DEBUG: every 300th frame, each batch drawn into the frame near
 * the player -- for tuning which batches are the player. */
static void g_focus_debug(const GRt *rt, const Nv2aBatch *b)
{
    static int on = -1;
    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, w0 = 1e30f, w1 = -1e30f;
    float fx, fy;
    uint32_t i;
    if (on < 0) on = getenv("RECOMP_FOCUS_DEBUG") != NULL;
    if (!on || G.frame % 300 || rt->h < 448 || !g_focus())
        return;
    for (i = 0; i < b->count; i++) {
        const Nv2aVertex *v = &b->vertices[i];
        float w = v->rhw > 0.0f ? 1.0f / v->rhw : 0.0f;
        if (v->x < x0) x0 = v->x;
        if (v->x > x1) x1 = v->x;
        if (v->y < y0) y0 = v->y;
        if (v->y > y1) y1 = v->y;
        if (w < w0) w0 = w;
        if (w > w1) w1 = w;
    }
    fx = s_focus.v[0] * (float)rt->w;
    fy = s_focus.v[1] * (float)rt->h;
    if (fx < x0 - 40 || fx > x1 + 40 || fy < y0 - 40 || fy > y1 + 40)
        return;
    fprintf(stderr, "[FOCUS] f%u rt %ux%u focus %.0f,%.0f w %.1f h %.3f | n %u x %.0f..%.0f"
                    " y %.0f..%.0f w %.1f..%.1f dw %u dt %u ext %d tex %08X\n",
            G.frame, rt->w, rt->h, fx, fy, s_focus.v[2], s_focus.v[3], b->count,
            x0, x1, y0, y1, w0, w1, b->state->depth_write, b->state->depth_test_enable,
            b->ext != NULL, b->stage[0] ? b->stage[0]->offset : (b->texture ? b->texture->offset : 0));
}

/* The surface's clip rectangle in target pixels. In a widened target a
 * clip covering the guest's whole width covers the whole target; a smaller
 * one moves with the centred picture. */
static void g_ws_rect(const GRt *rt, const Nv2aSurface *s, D3D11_RECT *r)
{
    float l = (float)s->clip_x, rr = (float)(s->clip_x + s->width);
    if (rt->ew != rt->w) {
        /* "Whole width" allows a pixel or two: this title clips its
         * 1280-wide anti-aliased frame to 1278. */
        uint32_t slack = rt->w / 128;
        if (s->clip_x <= slack && s->clip_x + s->width + slack >= rt->w) {
            rr = (float)rt->ew;
        } else {
            l += rt->ox;
            rr += rt->ox;
        }
    }
    r->left = (LONG)(l * (float)G.scale);
    r->top = (LONG)(s->clip_y * G.scale);
    r->right = (LONG)(rr * (float)G.scale);
    r->bottom = (LONG)((s->clip_y + s->height) * G.scale);
}

static void g_bind_target(GRt *rt, ID3D11DepthStencilView *dsv,
                          const Nv2aSurface *s)
{
    D3D11_VIEWPORT vp;
    D3D11_RECT sc;
    static ID3D11ShaderResourceView *const none[4] = { NULL, NULL, NULL, NULL };

    /* Unbind textures first: the target may have been sampled last batch. */
    ID3D11DeviceContext_PSSetShaderResources(G.ctx, 0, 4, none);
    ID3D11DeviceContext_OMSetRenderTargets(G.ctx, 1, &rt->rtv, dsv);
    vp.TopLeftX = 0.0f; vp.TopLeftY = 0.0f;
    vp.Width = (float)(rt->ew * G.scale); vp.Height = (float)(rt->h * G.scale);
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(G.ctx, 1, &vp);
    g_ws_rect(rt, s, &sc);
    ID3D11DeviceContext_RSSetScissorRects(G.ctx, 1, &sc);
}

static void gb_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    LARGE_INTEGER t0;
    GRt *rt;

    g_time_begin(&t0);
    rt = g_rt_get(s);
    if (!rt) {
        g_time_end(&t0);
        return;
    }
    G.n_clears++;
    if (G.trace_frame && G.frame == G.trace_frame)
        fprintf(stderr, "[GPUT] clear %08X flags %X argb %08X zs %08X zeta %08X clip %u,%u %ux%u\n",
                s->color_va, flags, argb, zstencil, rs->zeta_va, s->clip_x,
                s->clip_y, s->width, s->height);
    if (flags & 0xF0) {
        float c[4];
        g_unpack(argb, c);
        if (s->bytes_per_pixel == 2) {
            /* What a 5:6:5 surface stores of the clear colour. */
            c[0] = (float)((argb >> 19) & 0x1F) * 8.0f / 255.0f;
            c[1] = (float)((argb >> 10) & 0x3F) * 4.0f / 255.0f;
            c[2] = (float)((argb >> 3) & 0x1F) * 8.0f / 255.0f;
            c[3] = 1.0f;
        }
        if (G.ctx1) {
            D3D11_RECT r;
            g_ws_rect(rt, s, &r);
            ID3D11DeviceContext1_ClearView(G.ctx1, (ID3D11View *)rt->rtv, c, &r, 1);
        } else {
            ID3D11DeviceContext_ClearRenderTargetView(G.ctx, rt->rtv, c);
        }
    }
    if (flags & 0x01) {
        int fresh;
        GDepth *d = g_depth_get(rs->zeta_va, rt->ew, rt->h, &fresh);
        if (d) {
            float z = rs->zmax == 65535.0f ? (float)(zstencil & 0xFFFF)
                                           : (float)(zstencil >> 8);
            z /= rs->zmax > 0.0f ? rs->zmax : 16777215.0f;
            ID3D11DeviceContext_ClearDepthStencilView(G.ctx, d->dsv,
                                                      D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, z, 0);
        }
    }
    g_time_end(&t0);
}

/* A render target the batch is drawing into, as a texture it can sample. */
static ID3D11ShaderResourceView *g_self_copy(GRt *rt)
{
    if (!G.copy_tex || G.copy_w != rt->ew || G.copy_h != rt->h) {
        D3D11_TEXTURE2D_DESC td;
        REL(G.copy_srv);
        REL(G.copy_tex);
        memset(&td, 0, sizeof td);
        td.Width = rt->ew * G.scale;
        td.Height = rt->h * G.scale;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(G.dev, &td, NULL, &G.copy_tex))
            || FAILED(ID3D11Device_CreateShaderResourceView(G.dev,
                          (ID3D11Resource *)G.copy_tex, NULL, &G.copy_srv))) {
            REL(G.copy_tex);
            return G.white;
        }
        G.copy_w = rt->ew;
        G.copy_h = rt->h;
    }
    ID3D11DeviceContext_CopyResource(G.ctx, (ID3D11Resource *)G.copy_tex,
                                     (ID3D11Resource *)rt->tex);
    G.n_selfcopy++;
    return G.copy_srv;
}

static void g_vert(const Nv2aBatch *b, uint32_t k, GVert *o)
{
    const Nv2aVertex *v = &b->vertices[k];
    o->pos[0] = v->x; o->pos[1] = v->y; o->pos[2] = v->z; o->pos[3] = v->rhw;
    if (b->ext) {
        const Nv2aVertexEx *e = &b->ext[k];
        memcpy(o->d0, e->d0, 16);
        memcpy(o->d1, e->d1, 16);
        o->fog[0] = e->fog; o->fog[1] = o->fog[2] = o->fog[3] = 0.0f;
        memcpy(o->t, e->tex, 64);
    } else {
        g_unpack(v->diffuse, o->d0);
        memset(o->d1, 0, 16);
        o->fog[0] = 1.0f; o->fog[1] = o->fog[2] = o->fog[3] = 0.0f;
        memset(o->t, 0, 64);
        o->t[0][0] = v->u; o->t[0][1] = v->v; o->t[0][3] = 1.0f;
    }
}

/* RECOMP_SILHOUETTE: is this batch (part of) the player? Opaque, depth-
 * tested, drawn into the frame, small, around the player's point on screen
 * (nv2a_backend_set_focus_hook) and at the player's distance. Tuned on
 * Carbon (RECOMP_FOCUS_DEBUG): the body's pieces span ~70x50 of 1280x480
 * and sit 0..35 units nearer than the middle of the body; the ground, the
 * shadow and walls fail one test or another. Someone standing right next
 * to the player may pass too, which only outlines them as well. */
static int g_is_player(const GRt *rt, const Nv2aBatch *b)
{
    const Nv2aRenderState *rs = b->state;
    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, w0 = 1e30f, w1 = -1e30f;
    float fx, fy, fw, mx, my, sx;
    uint32_t i;
    if (!G.sil || !b->ext || !rs->depth_write || !rs->depth_test_enable
        || rt->h < 448 || b->count < 30 || !g_focus())
        return 0;
    sx = (float)rt->w / ((float)rt->h * 4.0f / 3.0f);   /* 2 on the 2x-wide frame */
    fx = s_focus.v[0] * (float)rt->w;
    fy = s_focus.v[1] * (float)rt->h;
    fw = s_focus.v[2];
    my = 0.08f * (float)rt->h;
    mx = my * sx;
    for (i = 0; i < b->count; i++) {
        const Nv2aVertex *v = &b->vertices[i];
        float w;
        if (!(v->rhw > 0.0f))
            return 0;
        w = 1.0f / v->rhw;
        if (v->x < x0) x0 = v->x;
        if (v->x > x1) x1 = v->x;
        if (v->y < y0) y0 = v->y;
        if (v->y > y1) y1 = v->y;
        if (w < w0) w0 = w;
        if (w > w1) w1 = w;
    }
    if (fx < x0 - mx || fx > x1 + mx || fy < y0 - my || fy > y1 + my)
        return 0;
    if (y1 - y0 > 0.25f * (float)rt->h || x1 - x0 > 0.25f * (float)rt->h * sx)
        return 0;
    return w1 > fw - 60.0f && w0 < fw + 5.0f;
}

static void g_sil_record(const GRt *rt, const Nv2aSurface *s, const Nv2aBatch *b,
                         const float vp[4], const float wsx[4])
{
    uint32_t k;
    if (G.sil_frame != G.frame || G.sil_rt != (void *)rt) {
        G.sil_n = G.sil_nv = 0;
        G.sil_frame = G.frame;
        G.sil_rt = (void *)rt;
    }
    if (!G.sil_v && !(G.sil_v = (GVert *)malloc(65536 * sizeof(GVert))))
        return;
    if (G.sil_n >= 64 || G.sil_nv + b->count > 65536)
        return;
    for (k = 0; k < b->count; k++)
        g_vert(b, k, &G.sil_v[G.sil_nv + k]);
    G.sil_b[G.sil_n].first = G.sil_nv;
    G.sil_b[G.sil_n].count = b->count;
    memcpy(G.sil_b[G.sil_n].vp, vp, sizeof G.sil_b[0].vp);
    memcpy(G.sil_b[G.sil_n].wsx, wsx, sizeof G.sil_b[0].wsx);
    G.sil_n++;
    G.sil_nv += b->count;
    G.sil_s = *s;
    G.sil_zeta = b->state->zeta_va;
}

/* Draw the recorded player batches flat where they are hidden. */
static void g_sil_flush(GRt *rt)
{
    static ID3D11ShaderResourceView *const none[4] = { NULL, NULL, NULL, NULL };
    GDepth *depth;
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t i, stride = sizeof(GVert), off = 0;
    int fresh;
    if (!G.sil_n || G.sil_frame != G.frame || G.sil_rt != (void *)rt)
        return;
    G.sil_n = 0;
    depth = g_depth_get(G.sil_zeta, rt->ew, rt->h, &fresh);
    if (!depth || fresh)
        return;
    if (!G.sil_vb) {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = 65536 * sizeof(GVert);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(G.dev, &bd, NULL, &G.sil_vb)))
            return;
    }
    if (!G.sil_bs) {
        D3D11_BLEND_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(ID3D11Device_CreateBlendState(G.dev, &bd, &G.sil_bs)))
            return;
    }
    if (FAILED(ID3D11DeviceContext_Map(G.ctx, (ID3D11Resource *)G.sil_vb, 0,
                                       D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return;
    memcpy(m.pData, G.sil_v, (size_t)G.sil_nv * sizeof(GVert));
    ID3D11DeviceContext_Unmap(G.ctx, (ID3D11Resource *)G.sil_vb, 0);
    g_bind_target(rt, depth->dsv, &G.sil_s);
    ID3D11DeviceContext_PSSetShaderResources(G.ctx, 0, 4, none);
    ID3D11DeviceContext_IASetInputLayout(G.ctx, G.layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(G.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(G.ctx, 0, 1, &G.sil_vb, &stride, &off);
    ID3D11DeviceContext_VSSetShader(G.ctx, G.vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(G.ctx, 0, 1, &G.cb);
    ID3D11DeviceContext_PSSetShader(G.ctx, G.ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(G.ctx, 0, 1, &G.cb);
    ID3D11DeviceContext_RSSetState(G.ctx, G.raster);
    ID3D11DeviceContext_OMSetBlendState(G.ctx, G.sil_bs, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(G.ctx, g_dss_s(NULL, 1, 2), 1);
    for (i = 0; i < 64 && G.sil_b[i].count; i++) {
        GCB *cb;
        if (FAILED(ID3D11DeviceContext_Map(G.ctx, (ID3D11Resource *)G.cb, 0,
                                           D3D11_MAP_WRITE_DISCARD, 0, &m)))
            break;
        cb = (GCB *)m.pData;
        memset(cb, 0, sizeof *cb);
        memcpy(cb->vp, G.sil_b[i].vp, sizeof cb->vp);
        memcpy(cb->wsx, G.sil_b[i].wsx, sizeof cb->wsx);
        cb->misc[2] = 1;
        memcpy(cb->fc0, G.sil_color, sizeof cb->fc0);
        ID3D11DeviceContext_Unmap(G.ctx, (ID3D11Resource *)G.cb, 0);
        ID3D11DeviceContext_Draw(G.ctx, G.sil_b[i].count, G.sil_b[i].first);
    }
    memset(G.sil_b, 0, sizeof G.sil_b);
    G.sil_nv = 0;
}

/* Does the batch cover exactly the guest's whole frame (one quad, corners
 * on the frame's corners) with nothing 4:3 on it? Such a quad -- a fade, a
 * full-screen tint, a pass over the widened frame itself -- is stretched
 * over a widened target. A picture (a movie frame, a menu backdrop) is not:
 * it stays 4:3 in the middle, black either side. */
static int g_ws_fullscreen(const Nv2aBatch *b, const GRt *rt, int use_rc)
{
    uint32_t i;
    int l = 0, r = 0, st;
    if (b->count < 3 || b->count > 6)
        return 0;
    for (st = 0; st < 4; st++) {
        const Nv2aTexture *t = use_rc ? b->stage[st] : (st == 0 ? b->texture : NULL);
        const GRt *src;
        if (!t)
            continue;
        if (use_rc && ((b->rc->stage_program >> (st * 5)) & 0x1F) == 0)
            continue;                   /* stage off */
        src = g_rt_find(t->offset);
        if (!src || src->ew == src->w)
            return 0;
    }
    for (i = 0; i < b->count; i++) {
        float x = b->vertices[i].x, y = b->vertices[i].y;
        if (y > 1.0f && y < (float)rt->h - 1.0f)
            return 0;
        if (x <= 1.0f) l = 1;
        else if (x >= (float)rt->w - 1.0f) r = 1;
        else return 0;
    }
    return l && r;
}

static void gb_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    LARGE_INTEGER t0;
    GRt *rt;
    GDepth *depth = NULL;
    const Nv2aRenderState *rs = b->state;
    ID3D11ShaderResourceView *srv[4];
    ID3D11SamplerState *smp[4];
    D3D11_MAPPED_SUBRESOURCE m;
    GCB *cb;
    float bf[4];
    uint32_t i, st, done;
    int use_rc = b->rc != NULL && b->ext != NULL, fresh, stretch, is_player = 0, smode = 0;
    float sil_vp[4], sil_wsx[4];

    if (!b->count)
        return;
    g_time_begin(&t0);
    rt = g_rt_get(s);
    if (!rt) {
        g_time_end(&t0);
        return;
    }
    /* The first undepth-tested draw after the player (the HUD, usually)
     * is where the 3D scene is complete: the silhouette goes in first. */
    if (G.sil && !rs->depth_test_enable && G.sil_n)
        g_sil_flush(rt);
    if (rs->depth_test_enable || rs->depth_write)
        depth = g_depth_get(rs->zeta_va, rt->ew, rt->h, &fresh);
    g_bind_target(rt, depth ? depth->dsv : NULL, s);
    is_player = g_is_player(rt, b);

    if (FAILED(ID3D11DeviceContext_Map(G.ctx, (ID3D11Resource *)G.cb, 0,
                                       D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        g_time_end(&t0);
        return;
    }
    cb = (GCB *)m.pData;
    memset(cb, 0, sizeof *cb);
    stretch = rt->ew != rt->w && g_ws_fullscreen(b, rt, use_rc);
    g_focus_debug(rt, b);

    if (rt->ew == rt->w) {
        cb->vp[0] = 2.0f / (float)rt->w;
        cb->wsx[0] = -1.0f;
    } else if (stretch) {
        /* A quad over the whole guest frame (a fade, the anti-aliasing
         * resolve, a backdrop) covers the whole wide target. */
        cb->vp[0] = 2.0f / (float)rt->w;
        cb->wsx[0] = -1.0f;
    } else {
        cb->vp[0] = 2.0f / (float)rt->ew;
        cb->wsx[0] = 2.0f * rt->ox / (float)rt->ew - 1.0f;
    }
    cb->vp[1] = 2.0f / (float)rt->h;
    cb->vp[2] = 1.0f / (rs->zmax > 0.0f ? rs->zmax : 16777215.0f);
    cb->vp[3] = b->ext ? 1.0f : 0.0f;  /* screen-space batches: linear */
    cb->mode[0] = (uint32_t)use_rc;
    cb->mode[1] = !use_rc && b->texture != NULL;
    cb->mode[2] = rs->alpha_test_enable ? (rs->alpha_func ? rs->alpha_func : 0x207) : 0;
    cb->mode[3] = rs->alpha_ref;
    cb->misc[0] = b->clip_plane_mode;
    cb->misc[1] = 0;                    /* stages sampled bicubic (TEXFILTER 2) */
    for (st = 0; st < 4; st++) {
        const Nv2aTexture *t = use_rc ? b->stage[st] : (st == 0 ? b->texture : NULL);
        GRt *src = NULL;
        float tw = 1.0f, th = 1.0f;
        srv[st] = G.white;
        smp[st] = g_sampler(0, 3, 3);
        cb->tscale[st][0] = cb->tscale[st][1] = 1.0f;
        if (!t)
            continue;
        srv[st] = g_tex_srv(t, &tw, &th, &src);
        if (src == rt)
            srv[st] = g_self_copy(rt);
        {
            uint32_t mag = (t->filter >> 24) & 0xF, min = (t->filter >> 16) & 0xFF;
            /* MAG 2 / MIN 2,4,6 are linear; MAG 4 / MIN 7 are the NV2A's
             * convolution (Gaussian) filter, used on the anti-aliased frame:
             * linear is the closest D3D11 has (it was nearest before). */
            int asked = mag == 2 || mag == 4 || min == 2 || min == 4 || min == 6 || min == 7;
            /* Without combiners the software path samples the nearest texel. */
            int linear = use_rc && asked;
            /* RECOMP_GPU_TEXFILTER: what the title filters linearly gets
             * mipmaps (1) and bicubic magnification too (2); what it asks to
             * be sampled nearest (HUD strips, lookup tables) stays nearest. */
            if (G.texfilter && linear && src == NULL) {
                linear = 2;
                if (G.texfilter == 2)
                    cb->misc[1] |= 1u << st;
            }
            smp[st] = g_sampler(linear, t->addr_u, t->addr_v);
        }
        /* To [0,1] over what is bound: raw normalised coordinates of a
         * transformed batch cover t->width texels; everything else is texels. */
        if (b->ext && t->normalized) {
            cb->tscale[st][0] = (float)t->width / tw;
            cb->tscale[st][1] = (float)t->height / th;
        } else {
            cb->tscale[st][0] = 1.0f / tw;
            cb->tscale[st][1] = 1.0f / th;
        }
        if (src && src->ew != src->w) {
            /* A widened target sampled: the guest's texels are the centred
             * part, or -- drawn by a full-frame quad -- all of it. */
            float k = (float)src->w / (float)src->ew;
            if (!stretch) {
                cb->tscale[st][0] *= k;
                cb->tscale[st][2] = src->ox / (float)src->ew;
            }
        }
    }
    if (use_rc) {
        const Nv2aCombiner *rc = b->rc;
        cb->fin[0] = rc->final0;
        cb->fin[1] = rc->final1;
        cb->fin[2] = rc->control;
        cb->fin[3] = rc->stage_program;
        g_unpack(rc->final_c0, cb->fc0);
        g_unpack(rc->final_c1, cb->fc1);
        for (i = 0; i < 8; i++) {
            cb->cw[i][0] = rc->color_icw[i];
            cb->cw[i][1] = rc->color_ocw[i];
            cb->cw[i][2] = rc->alpha_icw[i];
            cb->cw[i][3] = rc->alpha_ocw[i];
            g_unpack(rc->factor0[i], cb->f0[i]);
            g_unpack(rc->factor1[i], cb->f1[i]);
        }
        /* FOG_COLOR is R in bits 0-7, the reverse of a D3DCOLOR. */
        g_unpack(b->fog_color, cb->fogc);
        { float r = cb->fogc[2]; cb->fogc[2] = cb->fogc[0]; cb->fogc[0] = r; }
    }
    memcpy(sil_vp, cb->vp, sizeof sil_vp);
    memcpy(sil_wsx, cb->wsx, sizeof sil_wsx);
    ID3D11DeviceContext_Unmap(G.ctx, (ID3D11Resource *)G.cb, 0);

    ID3D11DeviceContext_IASetInputLayout(G.ctx, G.layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(G.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(G.ctx, G.vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(G.ctx, 0, 1, &G.cb);
    ID3D11DeviceContext_PSSetShader(G.ctx, G.ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(G.ctx, 0, 1, &G.cb);
    ID3D11DeviceContext_PSSetShaderResources(G.ctx, 0, 4, srv);
    ID3D11DeviceContext_PSSetSamplers(G.ctx, 0, 4, smp);
    ID3D11DeviceContext_RSSetState(G.ctx, G.raster);
    if (rs->blend_src == 0x8003 || rs->blend_src == 0x8004
        || rs->blend_dst == 0x8003 || rs->blend_dst == 0x8004) {
        bf[0] = bf[1] = bf[2] = bf[3] = (float)(rs->blend_color >> 24) / 255.0f;
    } else {
        g_unpack(rs->blend_color, bf);
    }
    ID3D11DeviceContext_OMSetBlendState(G.ctx, g_blend(rs), bf, 0xFFFFFFFFu);
    /* RECOMP_SILHOUETTE: every depth-writing draw into the frame marks its
     * visible pixels 1 if it is the player and 0 if not, so the stencil ends
     * up 1 exactly where the player is seen. */
    if (G.sil && depth && rs->depth_write && rs->depth_test_enable && rt->h >= 448)
        smode = 1;
    ID3D11DeviceContext_OMSetDepthStencilState(G.ctx, g_dss_s(rs, depth != NULL, smode),
                                               (UINT)is_player);
    if (is_player)
        g_sil_record(rt, s, b, sil_vp, sil_wsx);

    /* Vertices, through a ring: appended while they fit, discarded and
     * restarted when not; a batch bigger than the ring goes in pieces. */
    for (done = 0; done < b->count; ) {
        uint32_t room, n, stride = sizeof(GVert), off;
        GVert *out;
        D3D11_MAP how = D3D11_MAP_WRITE_NO_OVERWRITE;

        room = (G_VB_BYTES - G.vb_pos) / stride;
        n = b->count - done;
        if (room < 3 || (room < n && G.vb_pos)) {
            how = D3D11_MAP_WRITE_DISCARD;
            G.vb_pos = 0;
            room = G_VB_BYTES / stride;
        }
        if (n > room)
            n = room - room % 3;
        if (FAILED(ID3D11DeviceContext_Map(G.ctx, (ID3D11Resource *)G.vb, 0, how, 0, &m)))
            break;
        out = (GVert *)((uint8_t *)m.pData + G.vb_pos);
        for (i = 0; i < n; i++) {
            const Nv2aVertex *v = &b->vertices[done + i];
            GVert *o = &out[i];
            o->pos[0] = v->x; o->pos[1] = v->y; o->pos[2] = v->z; o->pos[3] = v->rhw;
            if (b->ext) {
                const Nv2aVertexEx *e = &b->ext[done + i];
                memcpy(o->d0, e->d0, 16);
                memcpy(o->d1, e->d1, 16);
                o->fog[0] = e->fog; o->fog[1] = o->fog[2] = o->fog[3] = 0.0f;
                memcpy(o->t, e->tex, 64);
            } else {
                g_unpack(v->diffuse, o->d0);
                memset(o->d1, 0, 16);
                o->fog[0] = 1.0f; o->fog[1] = o->fog[2] = o->fog[3] = 0.0f;
                memset(o->t, 0, 64);
                o->t[0][0] = v->u; o->t[0][1] = v->v; o->t[0][3] = 1.0f;
            }
        }
        ID3D11DeviceContext_Unmap(G.ctx, (ID3D11Resource *)G.vb, 0);
        off = G.vb_pos;
        ID3D11DeviceContext_IASetVertexBuffers(G.ctx, 0, 1, &G.vb, &stride, &off);
        ID3D11DeviceContext_Draw(G.ctx, n, 0);
        G.vb_pos += n * stride;
        done += n;
    }
    if (G.trace_frame && G.frame == G.trace_frame) {
        float zlo = 3e38f, zhi = -3e38f, wlo = 3e38f, whi = -3e38f;
        for (i = 0; i < b->count; i++) {
            float z = b->vertices[i].z, w = b->vertices[i].rhw;
            if (z < zlo) zlo = z;
            if (z > zhi) zhi = z;
            if (w < wlo) wlo = w;
            if (w > whi) whi = w;
        }
        fprintf(stderr, "[GPUT] draw %u rt %08X %ux%u clip %u,%u %ux%u n %u ext %d rc %d"
                        " z %.6g..%.6g rhw %.4g..%.4g zmax %.0f zeta %08X dt %u df %X dw %u"
                        " bl %u %X %X %X cm %08X at %u %X %u st %08X fin %08X/%08X ctl %X\n",
                G.n_draws, s->color_va, rt->w, rt->h, s->clip_x, s->clip_y,
                s->width, s->height, b->count, b->ext != NULL, use_rc, zlo, zhi,
                wlo, whi, rs->zmax, rs->zeta_va, rs->depth_test_enable,
                rs->depth_func, rs->depth_write, rs->blend_enable, rs->blend_src,
                rs->blend_dst, rs->blend_eq, rs->color_mask, rs->alpha_test_enable,
                rs->alpha_func, rs->alpha_ref, b->rc ? b->rc->stage_program : 0,
                b->rc ? b->rc->final0 : 0, b->rc ? b->rc->final1 : 0,
                b->rc ? b->rc->control : 0);
        for (st = 0; st < 4; st++) {
            const Nv2aTexture *t = use_rc ? b->stage[st] : (st == 0 ? b->texture : NULL);
            if (t)
                fprintf(stderr, "[GPUT]   stage %u tex %08X %ux%u fmt %02X pal %08X"
                                " pitch %u norm %u filt %08X addr %u/%u%s\n",
                        st, t->offset, t->width, t->height, t->color, t->palette,
                        t->pitch, t->normalized, t->filter, t->addr_u, t->addr_v,
                        g_rt_find(t->offset) ? " (render target)" : "");
        }
        {
            /* The batch's vertices, for offline inspection:
             * RECOMP_GPU_TRACE_DIR/draw-<n>.bin = Nv2aVertex then Nv2aVertexEx. */
            const char *dir = getenv("RECOMP_GPU_TRACE_DIR");
            if (dir) {
                char path[512];
                FILE *f;
                snprintf(path, sizeof path, "%s/draw-%03u.bin", dir, G.n_draws);
                if ((f = fopen(path, "wb")) != NULL) {
                    fwrite(b->vertices, sizeof *b->vertices, b->count, f);
                    if (b->ext)
                        fwrite(b->ext, sizeof *b->ext, b->count, f);
                    fclose(f);
                }
            }
        }
        if (b->ext) {
            const Nv2aVertexEx *e0 = &b->ext[0];
            fprintf(stderr, "[GPUT]   v0 %.1f %.1f %.6g w %.4g d0 %.2f %.2f %.2f %.2f"
                            " t0 %.3f %.3f %.3f %.3f fog %.2f\n",
                    b->vertices[0].x, b->vertices[0].y, b->vertices[0].z,
                    b->vertices[0].rhw, e0->d0[0], e0->d0[1], e0->d0[2], e0->d0[3],
                    e0->tex[0][0], e0->tex[0][1], e0->tex[0][2], e0->tex[0][3], e0->fog);
        }
    }
    G.n_draws++;
    G.n_verts += b->count;
    rt->last_frame = G.frame;
    g_time_end(&t0);
}

/* ---- Direct present ---------------------------------------------------- */
/* With a game window open (RECOMP_FB_WINDOW), the flipped buffer goes straight
 * from its render target to a DXGI swap chain on that window, scaled to the
 * window with the 4:3 picture kept (an anti-aliased 2x surface is averaged by
 * the linear filter). The window thread stops drawing through GDI once the
 * window carries the "XboxRecompGpuPresent" property. The window is found by
 * class, so the back end needs nothing linked from src/video.
 *   RECOMP_GPU_PRESENT=0   keep the GDI window (old behaviour)
 *   RECOMP_VSYNC=0         present without waiting for vblank (default 1)
 *   RECOMP_GPU_READBACK=0  skip copying the frame back to guest memory while
 *                          presenting directly (no latest.bmp / shots then);
 *                          1 = copy at once (waits for the GPU every flip);
 *                          2 = a flip or two late, no waiting (default) */
static const char k_present_hlsl[] =
    "Texture2D t : register(t0); SamplerState s : register(s0);\n"
    "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V VSBlit(uint id : SV_VertexID) {\n"
    "  V o; float2 uv = float2((id << 1) & 2, id & 2);\n"
    "  o.uv = uv; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "  return o; }\n"
    "float4 PSBlit(V i) : SV_Target { return float4(t.Sample(s, i.uv).rgb, 1); }\n";

static struct {
    int tried, ok, vsync, readback, searches, announced;
    HWND hwnd;
    IDXGISwapChain1 *sc;
    ID3D11RenderTargetView *rtv;
    UINT w, h;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11SamplerState *smp, *smp_pt;
    ID3D11RasterizerState *rs;
    ID3D11BlendState *bs;
    ID3D11DepthStencilState *dss;
} P;

static int p_compile(const char *entry, const char *profile, ID3DBlob **out)
{
    ID3DBlob *err = NULL;
    HRESULT hr = D3DCompile(k_present_hlsl, sizeof k_present_hlsl - 1,
                            "nv2a_present", NULL, NULL, entry, profile,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[GPUB] present shader %s does not compile: %s\n", entry,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
        fflush(stderr);
    }
    REL(err);
    return SUCCEEDED(hr);
}

/* The blit pipeline (present, up- and down-scaling), made once on first use. */
static int g_blit_init(void)
{
    static int tried, ok;
    ID3DBlob *vb = NULL, *pb = NULL;
    D3D11_SAMPLER_DESC sd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_BLEND_DESC bd;
    D3D11_DEPTH_STENCIL_DESC dd;

    if (tried)
        return ok;
    tried = 1;
    if (!p_compile("VSBlit", "vs_5_0", &vb) || !p_compile("PSBlit", "ps_5_0", &pb)) {
        REL(vb);
        REL(pb);
        return 0;
    }
    ok = SUCCEEDED(ID3D11Device_CreateVertexShader(G.dev, ID3D10Blob_GetBufferPointer(vb),
                       ID3D10Blob_GetBufferSize(vb), NULL, &P.vs)) &&
         SUCCEEDED(ID3D11Device_CreatePixelShader(G.dev, ID3D10Blob_GetBufferPointer(pb),
                       ID3D10Blob_GetBufferSize(pb), NULL, &P.ps));
    REL(vb);
    REL(pb);
    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    memset(&bd, 0, sizeof bd);
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    memset(&dd, 0, sizeof dd);
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ok = ok && SUCCEEDED(ID3D11Device_CreateSamplerState(G.dev, &sd, &P.smp));
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    ok = ok && SUCCEEDED(ID3D11Device_CreateSamplerState(G.dev, &sd, &P.smp_pt))
            && SUCCEEDED(ID3D11Device_CreateRasterizerState(G.dev, &rd, &P.rs))
            && SUCCEEDED(ID3D11Device_CreateBlendState(G.dev, &bd, &P.bs))
            && SUCCEEDED(ID3D11Device_CreateDepthStencilState(G.dev, &dd, &P.dss));
    if (!ok) {
        fprintf(stderr, "[GPUB] blit pipeline unavailable\n");
        fflush(stderr);
    }
    return ok;
}

static void g_blit(ID3D11ShaderResourceView *src, ID3D11RenderTargetView *dst,
                   float x, float y, float w, float h, int linear)
{
    static ID3D11ShaderResourceView *const none[1] = { NULL };
    D3D11_VIEWPORT vp;

    vp.TopLeftX = x;
    vp.TopLeftY = y;
    vp.Width = w;
    vp.Height = h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_OMSetRenderTargets(G.ctx, 1, &dst, NULL);
    ID3D11DeviceContext_RSSetViewports(G.ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(G.ctx, P.rs);
    ID3D11DeviceContext_OMSetBlendState(G.ctx, P.bs, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(G.ctx, P.dss, 0);
    ID3D11DeviceContext_IASetInputLayout(G.ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(G.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(G.ctx, P.vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(G.ctx, P.ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(G.ctx, 0, 1, &src);
    ID3D11DeviceContext_PSSetSamplers(G.ctx, 0, 1, linear ? &P.smp : &P.smp_pt);
    ID3D11DeviceContext_Draw(G.ctx, 3, 0);
    /* Every batch rebinds its whole pipeline, so only the hazards matter. */
    ID3D11DeviceContext_PSSetShaderResources(G.ctx, 0, 1, none);
    ID3D11DeviceContext_OMSetRenderTargets(G.ctx, 0, NULL, NULL);
}

static int p_init(void)
{
    const char *e = getenv("RECOMP_GPU_PRESENT");

    if (e && atoi(e) == 0)
        return 0;
    e = getenv("RECOMP_VSYNC");
    P.vsync = e ? atoi(e) : 1;
    e = getenv("RECOMP_GPU_READBACK");
    P.readback = e ? atoi(e) : 2;
    return g_blit_init();
}

static void p_drop_swapchain(void)
{
    if (G.ctx)
        ID3D11DeviceContext_OMSetRenderTargets(G.ctx, 0, NULL, NULL);
    REL(P.rtv);
    REL(P.sc);
    P.w = P.h = 0;
}

/* The game window of this process, if one is open. */
static HWND p_window(void)
{
    HWND h = NULL;

    if (P.hwnd && IsWindow(P.hwnd))
        return P.hwnd;
    if (P.hwnd) {                       /* closed: let go of its swap chain */
        p_drop_swapchain();
        P.hwnd = NULL;
    }
    if (P.searches++ % 60)              /* look about once a second */
        return NULL;
    while ((h = FindWindowExA(NULL, h, "XboxRecompFramebuffer", NULL)) != NULL) {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == GetCurrentProcessId())
            return P.hwnd = h;
    }
    return NULL;
}

static int p_swapchain(HWND hwnd, UINT w, UINT h)
{
    IDXGIDevice *xd = NULL;
    IDXGIAdapter *ad = NULL;
    IDXGIFactory2 *f = NULL;
    DXGI_SWAP_CHAIN_DESC1 d;
    HRESULT hr = E_FAIL;

    if (SUCCEEDED(ID3D11Device_QueryInterface(G.dev, &IID_IDXGIDevice, (void **)&xd)) &&
        SUCCEEDED(IDXGIDevice_GetAdapter(xd, &ad)) &&
        SUCCEEDED(IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory2, (void **)&f))) {
        memset(&d, 0, sizeof d);
        d.Width = w;
        d.Height = h;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.Scaling = DXGI_SCALING_STRETCH;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        hr = IDXGIFactory2_CreateSwapChainForHwnd(f, (IUnknown *)G.dev, hwnd, &d,
                                                  NULL, NULL, &P.sc);
        if (SUCCEEDED(hr))              /* the window handles Alt+Enter itself */
            IDXGIFactory2_MakeWindowAssociation(f, hwnd, DXGI_MWA_NO_ALT_ENTER);
    }
    REL(f);
    REL(ad);
    REL(xd);
    if (FAILED(hr))
        g_log_hr("CreateSwapChainForHwnd", hr);
    return SUCCEEDED(hr);
}

static void g_present(GRt *rt)
{
    HWND hwnd;
    RECT cr;
    UINT w, h;
    D3D11_VIEWPORT vp;
    float aw, ah;
    HRESULT hr;
    static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

    if (!P.tried) {
        P.tried = 1;
        P.ok = p_init();
    }
    if (!P.ok || !rt->srv || !(hwnd = p_window()))
        return;
    if (!GetClientRect(hwnd, &cr))
        return;
    w = (UINT)(cr.right - cr.left);
    h = (UINT)(cr.bottom - cr.top);
    if (!w || !h)
        return;                         /* minimised */
    if (!P.sc) {
        if (!p_swapchain(hwnd, w, h)) {
            P.ok = 0;                   /* the GDI window carries on */
            return;
        }
        P.w = w;
        P.h = h;
        SetPropA(hwnd, "XboxRecompGpuPresent", (HANDLE)1);
        if (!P.announced++) {
            fprintf(stderr, "[GPUB] presenting directly to the window (vsync %d,"
                            " readback %d)\n", P.vsync, P.readback);
            fflush(stderr);
        }
    } else if (w != P.w || h != P.h) {
        ID3D11DeviceContext_OMSetRenderTargets(G.ctx, 0, NULL, NULL);
        REL(P.rtv);
        hr = IDXGISwapChain1_ResizeBuffers(P.sc, 0, w, h, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) {
            g_log_hr("ResizeBuffers", hr);
            return;
        }
        P.w = w;
        P.h = h;
    }
    if (!P.rtv) {
        ID3D11Texture2D *bb = NULL;
        if (FAILED(IDXGISwapChain1_GetBuffer(P.sc, 0, &IID_ID3D11Texture2D, (void **)&bb)))
            return;
        hr = ID3D11Device_CreateRenderTargetView(G.dev, (ID3D11Resource *)bb, NULL, &P.rtv);
        REL(bb);
        if (FAILED(hr))
            return;
    }
    /* 4:3 (or the widened target's shape), centred, black bars. */
    {
        float ra = 4.0f / 3.0f * (float)rt->ew / (float)rt->w;
        aw = (float)w;
        ah = (float)h;
        if (aw > ah * ra)
            aw = ah * ra;
        else
            ah = aw / ra;
    }
    vp.TopLeftX = ((float)w - aw) * 0.5f;
    vp.TopLeftY = ((float)h - ah) * 0.5f;
    vp.Width = aw;
    vp.Height = ah;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_ClearRenderTargetView(G.ctx, P.rtv, black);
    g_blit(rt->srv, P.rtv, vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, 1);
    hr = IDXGISwapChain1_Present(P.sc, P.vsync ? 1u : 0u, 0);
    if (FAILED(hr)) {
        static int logged;
        if (!logged++)
            g_log_hr("Present", hr);
    }
}

/* RECOMP_GPU_RT_DUMP=<dir>, RECOMP_GPU_RT_DUMP_AT=<s>[,<s>...]: the flipped
 * render target at full (scaled) size as <dir>/rt-<s>.bmp, the first flip at
 * or after each time (seconds since the first flip). No window needed: for
 * comparing filtering and resolution settings headless. */
static void g_rt_dump(GRt *rt)
{
    static int init, n, next;
    static double at[16];
    static const char *dir;
    static LARGE_INTEGER t0, f;
    LARGE_INTEGER now;
    double secs;
    if (!init) {
        const char *e = getenv("RECOMP_GPU_RT_DUMP_AT");
        init = 1;
        dir = getenv("RECOMP_GPU_RT_DUMP");
        while (e && *e && n < 16) {
            at[n++] = atof(e);
            e = strchr(e, ',');
            if (e) e++;
        }
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
    }
    if (!dir || next >= n)
        return;
    QueryPerformanceCounter(&now);
    secs = (double)(now.QuadPart - t0.QuadPart) / (double)f.QuadPart;
    if (secs < at[next])
        return;
    {
        D3D11_TEXTURE2D_DESC td;
        ID3D11Texture2D *st = NULL;
        D3D11_MAPPED_SUBRESOURCE m;
        char path[512];
        FILE *fp;
        uint32_t y;
        ID3D11Texture2D_GetDesc(rt->tex, &td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        td.MiscFlags = 0;
        td.MipLevels = 1;
        if (FAILED(ID3D11Device_CreateTexture2D(G.dev, &td, NULL, &st)))
            return;
        ID3D11DeviceContext_CopyResource(G.ctx, (ID3D11Resource *)st, (ID3D11Resource *)rt->tex);
        if (SUCCEEDED(ID3D11DeviceContext_Map(G.ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
            snprintf(path, sizeof path, "%s/rt-%03d.bmp", dir, (int)at[next]);
            if ((fp = fopen(path, "wb")) != NULL) {
                uint8_t hd[54];
                uint32_t sz = 54 + td.Width * td.Height * 4;
                int32_t neg_h = -(int32_t)td.Height;
                memset(hd, 0, sizeof hd);
                hd[0] = 'B'; hd[1] = 'M';
                memcpy(hd + 2, &sz, 4);
                hd[10] = 54; hd[14] = 40;
                memcpy(hd + 18, &td.Width, 4);
                memcpy(hd + 22, &neg_h, 4);
                hd[26] = 1; hd[28] = 32;
                fwrite(hd, 1, 54, fp);
                for (y = 0; y < td.Height; y++)
                    fwrite((const uint8_t *)m.pData + (size_t)y * m.RowPitch, 4, td.Width, fp);
                fclose(fp);
                fprintf(stderr, "[GPUB] wrote %s (%ux%u)\n", path, td.Width, td.Height);
                fflush(stderr);
            }
            ID3D11DeviceContext_Unmap(G.ctx, (ID3D11Resource *)st, 0);
        }
        REL(st);
        next++;
    }
}

/* Copy a finished read-back slot into guest memory. With
 * D3D11_MAP_FLAG_DO_NOT_WAIT returns 0 while the GPU is still on it. */
static int rb_finish(GRb *s, UINT flags)
{
    D3D11_MAPPED_SUBRESOURCE m;
    HRESULT hr;
    GRt *rt;
    uint8_t *dst;
    uint32_t y, x, w = s->cw, h = s->ch, pitch = s->pitch, bpp = s->bpp;

    hr = ID3D11DeviceContext_Map(G.ctx, (ID3D11Resource *)s->tex, 0, D3D11_MAP_READ,
                                 flags, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
        return 0;
    s->pending = 0;
    if (FAILED(hr))
        return 1;
    /* The copy lands a flip or two late. Titles draw their frames on the GPU
     * and never write this memory themselves, so if it changed since the
     * flip the surface was freed and the memory reused -- during a level
     * load, for a pushbuffer -- and writing the old frame there would
     * corrupt it (a fault in nv2a_pb_scan_segment loading the bar). Then the
     * frame is dropped; so it is when the target itself is gone. */
    rt = g_rt_find(s->va);
    if (!rt || !nv2a_backend_surface_ok(s->va, pitch * h)
        || g_hash(nv2a_backend_guest(s->va), pitch * h, 0) != s->mem_fp) {
        static int logged;
        if (logged++ < 5) {
            fprintf(stderr, "[GPUB] read-back of %08X dropped: the memory %s since the flip\n",
                    s->va, rt ? "changed" : "is no render target");
            fflush(stderr);
        }
        ID3D11DeviceContext_Unmap(G.ctx, (ID3D11Resource *)s->tex, 0);
        return 1;
    }
    {
        dst = (uint8_t *)nv2a_backend_guest(s->va);
        for (y = 0; y < h; y++) {
            const uint8_t *src = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
            uint8_t *row = dst + (size_t)y * pitch;
            if (bpp == 4) {
                memcpy(row, src, (size_t)w * 4);
            } else {
                uint16_t *o = (uint16_t *)row;
                for (x = 0; x < w; x++) {
                    const uint8_t *p = src + x * 4;
                    o[x] = (uint16_t)(((p[2] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[0] >> 3));
                }
            }
        }
    }
    {
        /* RECOMP_GPU_SHOT_DIR (+ _EVERY, default 60): the presented target
         * as drawn, every n frames, at most 100 files. RECOMP_GPU_SHOT_FROM
         * = seconds after the first resolve before shots start. */
        static const char *dir = (const char *)-1;
        static int every, shots;
        static ULONGLONG shot_from, shot_ms;
        if (dir == (const char *)-1) {
            const char *e = getenv("RECOMP_GPU_SHOT_EVERY");
            const char *fr = getenv("RECOMP_GPU_SHOT_FROM");
            const char *ms = getenv("RECOMP_GPU_SHOT_EVERY_MS");
            dir = getenv("RECOMP_GPU_SHOT_DIR");
            every = e ? atoi(e) : 60;
            if (every < 1) every = 1;
            /* RECOMP_GPU_SHOT_EVERY_MS: by wall time instead of frames (menus
             * run >1000 FPS, so a frame interval burns the 100-shot cap). */
            shot_ms = ms ? (ULONGLONG)atoi(ms) : 0;
            if (shot_ms) every = 1;
            shot_from = GetTickCount64() + (fr ? (ULONGLONG)(atof(fr) * 1000.0) : 0);
        }
        if (dir && shots < 100 && s->frame % (uint32_t)every == 0 &&
            GetTickCount64() >= shot_from) {
            if (shot_ms) shot_from = GetTickCount64() + shot_ms;
            char path[512];
            FILE *f;
            snprintf(path, sizeof path, "%s/shot-%05u.bmp", dir, s->frame);
            if ((f = fopen(path, "wb")) != NULL) {
                uint8_t hd[54];
                uint32_t sz = 54 + s->w * s->h * 4;
                int32_t neg_h = -(int32_t)s->h;
                memset(hd, 0, sizeof hd);
                hd[0] = 'B'; hd[1] = 'M';
                memcpy(hd + 2, &sz, 4);
                hd[10] = 54; hd[14] = 40;
                memcpy(hd + 18, &s->w, 4);
                memcpy(hd + 22, &neg_h, 4);
                hd[26] = 1; hd[28] = 32;
                fwrite(hd, 1, 54, f);
                for (y = 0; y < s->h; y++)
                    fwrite((const uint8_t *)m.pData + (size_t)y * m.RowPitch, 4, s->w, f);
                fclose(f);
                shots++;
            }
        }
    }
    ID3D11DeviceContext_Unmap(G.ctx, (ID3D11Resource *)s->tex, 0);
    /* What the guest now holds of this target, for the CPU-overwrite check
     * in g_tex_srv. */
    if (rt->pitch == pitch)
        rt->guest_fp = g_rt_fp(rt);
    /* This write changed the memory: copies still pending for the same
     * place expect what is there now, not what was there at their flip. */
    {
        uint64_t now = g_hash(nv2a_backend_guest(s->va), pitch * h, 0);
        uint32_t k;
        for (k = 0; k < G_RB_N; k++) {
            GRb *o = &G.rb[k];
            if (o != s && o->pending && o->va == s->va && o->pitch == pitch && o->ch == h)
                o->mem_fp = now;
        }
    }
    return 1;
}

static void gb_resolve(uint32_t va, uint32_t pitch, uint32_t bpp)
{
    LARGE_INTEGER t0;
    GRt *rt = g_rt_find(va);
    GRb *s;
    uint32_t w, h;

    if (!rt || !pitch || (bpp != 2 && bpp != 4))
        return;
    g_time_begin(&t0);
    if (G.sil)
        g_sil_flush(rt);
    g_rt_dump(rt);
    g_present(rt);
    if (P.sc && !P.readback) {
        G.n_resolves++;
        g_time_end(&t0);
        return;
    }
    w = rt->w;
    h = rt->h;
    if (w * bpp > pitch)
        w = pitch / bpp;
    if (!nv2a_backend_surface_ok(va, pitch * h)) {
        g_time_end(&t0);
        return;
    }
    s = &G.rb[G.rb_next];
    if (s->pending)
        rb_finish(s, 0);                /* ring full: the oldest lands now */
    if (!s->tex || s->w != rt->w || s->h != rt->h) {
        D3D11_TEXTURE2D_DESC td;
        REL(s->tex);
        s->w = s->h = 0;
        memset(&td, 0, sizeof td);
        td.Width = rt->w;
        td.Height = rt->h;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(G.dev, &td, NULL, &s->tex))) {
            g_time_end(&t0);
            return;
        }
        s->w = rt->w;
        s->h = rt->h;
    }
    if (G.scale > 1 || rt->ew != rt->w) {
        /* Back to guest size first (linear filter averages the scaled
         * pixels), then into the staging copy as at 1x. */
        if (!G.down_tex || G.down_w != rt->w || G.down_h != rt->h) {
            D3D11_TEXTURE2D_DESC td;
            REL(G.down_rtv);
            REL(G.down_tex);
            memset(&td, 0, sizeof td);
            td.Width = rt->w;
            td.Height = rt->h;
            td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (FAILED(ID3D11Device_CreateTexture2D(G.dev, &td, NULL, &G.down_tex)) ||
                FAILED(ID3D11Device_CreateRenderTargetView(G.dev,
                           (ID3D11Resource *)G.down_tex, NULL, &G.down_rtv)) ||
                !g_blit_init()) {
                REL(G.down_rtv);
                REL(G.down_tex);
                g_time_end(&t0);
                return;
            }
            G.down_w = rt->w;
            G.down_h = rt->h;
        }
        /* A widened target gives the guest its centred part. */
        g_blit(rt->srv, G.down_rtv, -rt->ox, 0.0f, (float)rt->ew, (float)rt->h, 1);
        ID3D11DeviceContext_CopyResource(G.ctx, (ID3D11Resource *)s->tex,
                                         (ID3D11Resource *)G.down_tex);
    } else {
        ID3D11DeviceContext_CopyResource(G.ctx, (ID3D11Resource *)s->tex,
                                         (ID3D11Resource *)rt->tex);
    }
    s->pending = 1;
    s->va = va;
    s->pitch = pitch;
    s->bpp = bpp;
    s->cw = w;
    s->ch = h;
    s->frame = G.frame;
    s->mem_fp = g_hash(nv2a_backend_guest(va), pitch * h, 0);
    G.rb_next = (G.rb_next + 1) % G_RB_N;
    if (P.readback == 1) {
        rb_finish(s, 0);
    } else {
        /* Land what the GPU has finished, oldest first. */
        uint32_t k;
        for (k = 0; k < G_RB_N; k++) {
            GRb *o = &G.rb[(G.rb_next + k) % G_RB_N];
            if (o->pending && !rb_finish(o, D3D11_MAP_FLAG_DO_NOT_WAIT))
                break;
        }
    }
    G.n_resolves++;
    g_time_end(&t0);
}

/* RECOMP_MEMDUMP_DIR: a debugging aid for finding game variables. When a
 * file <dir>/req appears (a harness writes it), the first line names a dump:
 * the guest's 64 MB of RAM goes to <dir>/<name>.bin and req is removed.
 * Checked every 10 frames. */
static void g_memdump_poll(void)
{
    static const char *dir = (const char *)-1;
    char path[512], name[128], out[700];
    FILE *f;
    if (dir == (const char *)-1)
        dir = getenv("RECOMP_MEMDUMP_DIR");
    if (!dir || G.frame % 10)
        return;
    snprintf(path, sizeof path, "%s/req", dir);
    if (!(f = fopen(path, "r")))
        return;
    name[0] = 0;
    if (!fgets(name, sizeof name, f))
        name[0] = 0;
    fclose(f);
    DeleteFileA(path);
    name[strcspn(name, "\r\n")] = 0;
    if (!name[0])
        return;
    snprintf(out, sizeof out, "%s/%s.bin", dir, name);
    if ((f = fopen(out, "wb")) != NULL) {
        fwrite(nv2a_backend_guest(0), 1, 64u << 20, f);
        fclose(f);
        fprintf(stderr, "[GPUB] memdump %s (frame %u)\n", out, G.frame);
        fflush(stderr);
    }
}

static void gb_flip(void)
{
    int i;
    G.frame++;
    g_memdump_poll();
    /* A surface nothing has drawn into for ~10 s gives its address back to
     * guest memory: the title may have reused it for something else. */
    for (i = 0; i < G_MAX_RT; i++)
        if (G.rt[i].tex && G.frame - G.rt[i].last_frame > 600)
            g_rt_release(&G.rt[i]);
    for (i = 0; i < G_MAX_DEPTH; i++)
        if (G.depth[i].tex && G.frame - G.depth[i].last_frame > 600) {
            REL(G.depth[i].dsv);
            REL(G.depth[i].tex);
            memset(&G.depth[i], 0, sizeof G.depth[i]);
        }
    if (G.frame % 600 == 0) {
        double ms = G.t_freq ? (double)G.t_backend * 1000.0 / (double)G.t_freq : 0.0;
        fprintf(stderr, "[GPUB] frames %u: per frame %.2f ms in back end, %.1f draws,"
                        " %.0f verts, %.1f clears, %.2f uploads (%.0f KB),"
                        " %.0f KB hashed, %.2f self-copies; %u textures cached\n",
                G.frame, ms / 600.0, G.n_draws / 600.0, G.n_verts / 600.0,
                G.n_clears / 600.0, G.n_uploads / 600.0,
                G.n_upload_bytes / 600.0 / 1024.0, G.n_hashed / 600.0 / 1024.0,
                G.n_selfcopy / 600.0, G.tex_count);
        fflush(stderr);
        G.t_backend = 0;
        G.n_draws = G.n_clears = G.n_uploads = G.n_selfcopy = G.n_resolves = 0;
        G.n_verts = G.n_hashed = G.n_upload_bytes = 0;
    }
}

static const Nv2aBackend k_d3d11 = { gb_clear, gb_draw, gb_flip, gb_resolve };

const Nv2aBackend *nv2a_backend_d3d11(void)
{
    if (!G.tried) {
        G.tried = 1;
        G.ok = g_init();
        if (!G.ok)
            fprintf(stderr, "[GPUB] Direct3D 11 back end unavailable\n");
    }
    return G.ok ? &k_d3d11 : NULL;
}

#else  /* !_WIN32 */

const Nv2aBackend *nv2a_backend_d3d11(void)
{
    return 0;
}

#endif
