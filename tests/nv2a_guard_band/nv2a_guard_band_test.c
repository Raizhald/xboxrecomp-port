/* Triangles whose vertices land far outside the surface.
 *
 * A vertex just in front of the eye (w small but positive) projects to
 * screen coordinates of 1e7..1e17. The rasteriser evaluates its edge
 * functions in float, and at those magnitudes a pixel centre's position is
 * below one ulp of the vertex coordinate: coverage, colour and depth of a
 * partly visible triangle come out as noise. Hardware avoids that with a
 * guard band, and so must the clipper: such a triangle is clipped to a band
 * around the surface before it is rasterised.
 *
 * The test: huge triangles whose one visible edge is the line y = 2x on a
 * 256x256 surface (and, with perspective, the same edge built from very
 * different w), drawn in one colour over a black surface. No pixel centre
 * (x + .5, y + .5) lies on that line, so coverage is exact: covered iff
 * y > 2x (or y <= 2x for the second case; the third adds a vertex behind
 * the eye, so the near plane and the guard band clip together).
 *
 * Harness as tests/nv2a_raster_batch: fixed-function transform with an
 * identity composite, positions are float4 (x, y, z, w) in clip space. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_backend.h"

#define W 256
#define H 256
#define COLOR_AT 0x10000u
#define POS_AT   0x90000u
#define DIFF_AT  0x98000u
#define MEM_BYTES 0xA0000u
static uint8_t s_mem[MEM_BYTES];

ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)s_mem; }
uint32_t g_xbox_image_lo, g_xbox_image_hi;
uint32_t g_xbox_stack_base, g_xbox_stack_size;
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }
void xbox_FramebufferWindowSet(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }
uint32_t xbox_Nv2aFenceWordVa(void) { return 0; }
int nv2a_pb_dma_walk(void) { return 1; }
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_Nv2aFrameCounterFlip(void) {}
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset) { (void)offset; return NULL; }

extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern void nv2a_pb_exec_sync(void);
static void m(uint32_t method, uint32_t param) { nv2a_pb_exec_method(0, method, param); }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void mf(uint32_t method, float f) { m(method, f2u(f)); }

static uint32_t s_n;

/* A vertex at screen (sx, sy), depth z, given as clip coordinates with w. */
static void vtx(float sx, float sy, float z, float w, uint32_t c)
{
    float p[4] = { sx * w, sy * w, z * w, w };
    memcpy(s_mem + POS_AT + 16u * s_n, p, 16);
    memcpy(s_mem + DIFF_AT + 4u * s_n, &c, 4);
    s_n++;
}

static void draw(uint32_t first, uint32_t count)
{
    m(0x17FC, 5);                            /* BEGIN_END: triangles */
    m(0x1810, (count - 1) << 24 | first);
    m(0x17FC, 0);
    nv2a_pb_exec_sync();
}

static uint32_t px(int x, int y)
{
    uint32_t v;
    memcpy(&v, s_mem + COLOR_AT + 4u * (uint32_t)(y * W + x), 4);
    return v & 0xFFFFFFu;
}

static void clear(void)
{
    memset(s_mem + COLOR_AT, 0, W * H * 4);
}

/* Count pixels whose colour disagrees with want(x, y). */
static int check(const char *name, uint32_t (*want)(int, int))
{
    int x, y, bad = 0;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            if (px(x, y) != want(x, y) && bad++ < 4)
                printf("  %s: pixel %d,%d got %06X want %06X\n",
                       name, x, y, px(x, y), want(x, y));
    printf("%s: %d wrong\n", name, bad);
    return bad;
}

static uint32_t want_upper(int x, int y) { return y > 2 * x ? 0xFF0000u : 0; }
static uint32_t want_lower(int x, int y) { return y <= 2 * x ? 0x00FF00u : 0; }

int main(void)
{
    static const float I[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    const float L = 1e8f;
    int i, bad = 0;

    m(0x0208, 0x0008);                       /* SET_SURFACE_FORMAT: A8R8G8B8 */
    m(0x0200, (uint32_t)W << 16);            /* CLIP_HORIZONTAL */
    m(0x0204, (uint32_t)H << 16);            /* CLIP_VERTICAL */
    m(0x020C, W * 4);                        /* SET_SURFACE_PITCH */
    m(0x0210, COLOR_AT);                     /* SET_SURFACE_COLOR_OFFSET */
    for (i = 0; i < 16; i++)
        mf(0x0680 + 4 * i, I[i]);            /* composite: identity */
    m(0x1E94, 0);                            /* TRANSFORM_EXECUTION_MODE: fixed */
    m(0x1720, POS_AT);  m(0x1760, 2 | (4 << 4) | (16 << 8));       /* attr 0: float4 */
    m(0x172C, DIFF_AT); m(0x176C, 0 | (4 << 4) | (4 << 8));        /* attr 3: D3DCOLOR */

    /* 1. Orthographic (w = 1), coordinates of 1e8: covered iff y > 2x. */
    clear();
    vtx(-L, -2 * L, 0.5f, 1.0f, 0xFFFF0000u);
    vtx( L,  2 * L, 0.5f, 1.0f, 0xFFFF0000u);
    vtx(-L,  2 * L, 0.5f, 1.0f, 0xFFFF0000u);
    draw(0, 3);
    bad += check("orthographic 1e8", want_upper);

    /* 2. The same kind of edge from perspective: the far vertices land far
     * away because their w is tiny. Covered iff y <= 2x. */
    clear();
    vtx(-L, -2 * L, 0.5f, 1e-2f, 0xFF00FF00u);
    vtx( L,  2 * L, 0.5f, 1e-3f, 0xFF00FF00u);
    vtx( L,     -L, 0.5f, 5e-3f, 0xFF00FF00u);
    draw(3, 3);
    bad += check("perspective tiny w", want_lower);

    /* 3. One vertex behind the eye as well: near-plane and guard-band
     * clipping together. Same line, covered iff y > 2x. */
    clear();
    vtx(-L, -2 * L, 0.5f, 1.0f, 0xFFFF0000u);
    vtx( L,  2 * L, 0.5f, 1.0f, 0xFFFF0000u);
    {   /* behind the eye (w = -1), on the y > 2x side: the near plane cuts
         * AC and BC at points that project to about 1e11 on that side. */
        float p[4] = { -L, 2 * L, 0.0f, -1.0f };
        uint32_t c = 0xFFFF0000u;
        memcpy(s_mem + POS_AT + 16u * s_n, p, 16);
        memcpy(s_mem + DIFF_AT + 4u * s_n, &c, 4);
        s_n++;
    }
    draw(6, 3);
    bad += check("near and guard", want_upper);

    printf("%s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
