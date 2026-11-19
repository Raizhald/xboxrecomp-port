/* Render to texture while the workers are still drawing.
 *
 * Switching the colour surface no longer drains the raster pipeline: each
 * job carries its own surface, and worker k owns rows y % n == k of every
 * surface. That is safe until a job samples a surface an earlier job is
 * still drawing -- worker k reading texel row y' of it can run ahead of the
 * worker that owns y'. pipe_check_hazards must drain then.
 *
 * Surface A gets a heavy batch: layers of full-surface quads, then 32 bands
 * of 8 rows, each its own colour. Then the surface switches to B and one
 * quad samples A upside down: B(x, y) = A(x, H - 1 - y). Every pixel of B
 * must be the band colour of its mirrored row. Run with several workers and
 * RECOMP_SURFACE_DRAIN=0 (tracking) or 1 (default drain);
 * RECOMP_SURFACE_DRAIN=unsafe turns the drain off and should make it fail
 * (on Windows, where the workers exist).
 *
 * Harness as tests/nv2a_raster_batch. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_backend.h"

#define W 256
#define H 256
#define SURF_A   0x010000u
#define SURF_B   0x050000u
#define POS_AT   0x090000u
#define DIFF_AT  0x0A0000u
#define TEX_AT   0x0B0000u
#define MEM_BYTES 0x0C0000u
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
static void vtx(float x, float y, uint32_t c, float u, float v)
{
    float p[3] = { x, y, 0.0f }, t[2] = { u, v };
    memcpy(s_mem + POS_AT + 12u * s_n, p, 12);
    memcpy(s_mem + DIFF_AT + 4u * s_n, &c, 4);
    memcpy(s_mem + TEX_AT + 8u * s_n, t, 8);
    s_n++;
}
static void quad(float x0, float y0, float x1, float y1, uint32_t c)
{
    vtx(x0, y0, c, 0, 0); vtx(x1, y0, c, 0, 0); vtx(x1, y1, c, 0, 0);
    vtx(x0, y0, c, 0, 0); vtx(x1, y1, c, 0, 0); vtx(x0, y1, c, 0, 0);
}
static void surface(uint32_t at)
{
    m(0x0200, (uint32_t)W << 16);            /* CLIP_HORIZONTAL */
    m(0x0204, (uint32_t)H << 16);            /* CLIP_VERTICAL */
    m(0x0208, 0x0008);                       /* FORMAT: A8R8G8B8, no AA */
    m(0x020C, W * 4);                        /* PITCH */
    m(0x0210, at);                           /* COLOR_OFFSET */
}
static void draw(uint32_t first, uint32_t count)
{
    uint32_t start;
    m(0x17FC, 5);                            /* BEGIN_END: triangles */
    for (start = first; start < first + count; start += 240)
        m(0x1810, ((first + count - start > 240 ? 240 : first + count - start) - 1) << 24 | start);
    m(0x17FC, 0);
}
static uint32_t band(int y) { return 0xFF000000u | (uint32_t)(y / 8) * 0x070503u + 0x102030u; }

int main(int argc, char **argv)
{
    static const float I[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    const char *threads = argc > 1 ? argv[1] : "4";
    uint32_t heavy, i;
    int x, y, bad = 0;

#ifdef _WIN32
    _putenv_s("RECOMP_RASTER_THREADS", threads);
    if (!getenv("RECOMP_SURFACE_DRAIN"))
        _putenv_s("RECOMP_SURFACE_DRAIN", "0");   /* test the tracking */
#else
    setenv("RECOMP_RASTER_THREADS", threads, 1);
    setenv("RECOMP_SURFACE_DRAIN", "0", 0);
#endif
    for (i = 0; i < 16; i++)
        mf(0x0680 + 4 * i, I[i]);            /* composite: positions are pixels */
    m(0x1E94, 0);                            /* fixed-function transform */
    m(0x1720, POS_AT);  m(0x1760, 2 | (3 << 4) | (12 << 8));       /* v0: float3 */
    m(0x172C, DIFF_AT); m(0x176C, 0 | (4 << 4) | (4 << 8));        /* v3: D3DCOLOR */
    m(0x1744, TEX_AT);  m(0x1784, 2 | (2 << 4) | (8 << 8));        /* v9: float2 */

    /* A: work for the workers, then the bands. */
    surface(SURF_A);
    for (i = 0; i < 160; i++)
        quad(0, 0, W, H, 0xFF000000u | (i * 0x010203u));
    for (y = 0; y < H; y += 8)
        quad(0, (float)y, W, (float)(y + 8), band(y));
    heavy = s_n;
    draw(0, heavy);

    /* B: one textured quad reading A upside down. Texture stage 0 is A as
     * LIN_A8R8G8B8 (texel coordinates). */
    surface(SURF_B);
    m(0x1B00, SURF_A);                        /* TEXTURE_OFFSET */
    m(0x1B04, 0x12u << 8);                    /* FORMAT: LIN_A8R8G8B8 */
    m(0x1B08, 0x0303);                        /* ADDRESS: clamp */
    m(0x1B10, (uint32_t)(W * 4) << 16);       /* CONTROL1: pitch */
    m(0x1B1C, (uint32_t)W << 16 | H);         /* IMAGE_RECT */
    vtx(0, 0, 0xFFFFFFFFu, 0, (float)H);
    vtx(W, 0, 0xFFFFFFFFu, (float)W, (float)H);
    vtx(W, H, 0xFFFFFFFFu, (float)W, 0);
    vtx(0, 0, 0xFFFFFFFFu, 0, (float)H);
    vtx(W, H, 0xFFFFFFFFu, (float)W, 0);
    vtx(0, H, 0xFFFFFFFFu, 0, 0);
    draw(heavy, 6);
    nv2a_pb_exec_sync();

    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            uint32_t got, want = band(H - 1 - y) & 0xFFFFFFu;
            memcpy(&got, s_mem + SURF_B + 4u * (uint32_t)(y * W + x), 4);
            if ((got & 0xFFFFFFu) != want && bad++ < 6)
                printf("  B %d,%d: got %06X want %06X\n", x, y, got & 0xFFFFFFu, want);
        }
    printf("threads %s: %d of %d pixels wrong\n", threads, bad, W * H);
    printf("%s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
