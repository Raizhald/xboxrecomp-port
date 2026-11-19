/* The post-transform vertex cache (raster_batch_program) must not change
 * a pixel. An indexed batch names every vertex of a grid of quads several
 * times; drawn once with the cache and once with RECOMP_NO_VCACHE, the two
 * surfaces must be identical (the test writes the surface; compare two
 * runs). Harness as tests/nv2a_raster_batch. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_backend.h"

#define W 256
#define H 256
#define COLOR_AT 0x10000u
#define POS_AT   0x60000u
#define DIFF_AT  0x70000u
#define MEM_BYTES 0x80000u
static uint8_t s_mem[MEM_BYTES];

/* ---- what the executor expects the runtime to provide ------------------ */
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


static void put_vertex(uint32_t n, float x, float y, uint32_t c)
{
    float p[3] = { x, y, 0.0f };
    memcpy(s_mem + POS_AT + 12u * n, p, 12);
    memcpy(s_mem + DIFF_AT + 4u * n, &c, 4);
}

static uint8_t s_first[W * H * 4];

static void frame(void)
{
    static const float I[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    uint32_t i, x, y, idx[6 * 15 * 15], n = 0;

    memset(s_mem + COLOR_AT, 0, W * H * 4);
    m(0x0208, 0x0008); m(0x0200, (uint32_t)W << 16); m(0x0204, (uint32_t)H << 16);
    m(0x020C, W * 4);  m(0x0210, COLOR_AT);
    for (i = 0; i < 16; i++)
        mf(0x0680 + 4 * i, I[i]);
    m(0x1E94, 0);
    /* 16 x 16 shared grid vertices, each with its own colour */
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++)
            put_vertex(y * 16 + x, (float)x * 16.0f + (float)(y & 1),
                       (float)y * 16.0f, 0xFF000000u | (x * 16u << 16) | (y * 16u << 8) | ((x ^ y) * 8u));
    m(0x1720, POS_AT);  m(0x1760, 2 | (3 << 4) | (12 << 8));
    m(0x172C, DIFF_AT); m(0x176C, 0 | (4 << 4) | (4 << 8));
    for (y = 0; y < 15; y++)
        for (x = 0; x < 15; x++) {
            uint32_t a = y * 16 + x;
            idx[n++] = a; idx[n++] = a + 1; idx[n++] = a + 17;
            idx[n++] = a; idx[n++] = a + 17; idx[n++] = a + 16;
        }
    m(0x17FC, 5);
    for (i = 0; i + 1 < n; i += 2)
        m(0x1800, idx[i] | idx[i + 1] << 16);          /* ARRAY_ELEMENT16 */
    m(0x17FC, 0);
    nv2a_pb_exec_sync();
}

int main(void)
{
    /* Writes the surface to VCACHE_OUT (default vcache.bin). Run twice, once
     * with RECOMP_NO_VCACHE=1, and compare the files byte for byte. */
    const char *path = getenv("VCACHE_OUT") ? getenv("VCACHE_OUT") : "vcache.bin";
    int drawn = 0, k;
    FILE *f;

    frame();
    memcpy(s_first, s_mem + COLOR_AT, sizeof s_first);
    for (k = 0; k < W * H; k++)
        if (s_first[4 * k] | s_first[4 * k + 1] | s_first[4 * k + 2])
            drawn++;
    printf("%s: %d pixels drawn\n", getenv("RECOMP_NO_VCACHE") ? "uncached" : "cached", drawn);
    if (!(f = fopen(path, "wb")))
        return 2;
    fwrite(s_first, 1, sizeof s_first, f);
    fclose(f);
    return drawn > 40000 ? 0 : 1;
}
