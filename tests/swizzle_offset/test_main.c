/* Texture addresses must stay identical: compare with a bit-by-bit reference
 * and with the separate row-walking swizzle/unswizzle routines. */
#ifdef _WIN32
#include <windows.h>
#include <rpc.h>  /* RPC defines 'small'; the header must also work after it. */
#endif
#include "d3d8_swizzle.h"
#include <stdio.h>

static uint64_t checks;
static uint32_t rng = 0x81930421u;
static uint32_t random32(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

static uint32_t reference(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t src, dst = 1, result = 0;
    for (src = 1; src && (src < w || src < h); src <<= 1) {
        if (src < w) { if (x & src) result |= dst; dst <<= 1; }
        if (src < h) { if (y & src) result |= dst; dst <<= 1; }
    }
    return result;
}

static int check(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t want = reference(x, y, w, h), got = swizzle_offset(x, y, w, h);
    checks++;
    if (got == want) return 1;
    printf("%ux%u (%u,%u): got %08X, expected %08X\n", w,h,x,y,got,want);
    return 0;
}

static int roundtrip(void)
{
    static const uint32_t shapes[][2] = {
        {1,1}, {1,256}, {256,1}, {4,64}, {64,4}, {512,512}, {1024,128}
    };
    unsigned shape, bpp;
    for (shape = 0; shape < sizeof shapes / sizeof shapes[0]; shape++) {
        uint32_t w = shapes[shape][0], h = shapes[shape][1], x, y;
        for (bpp = 1; bpp <= 4; bpp *= 2) {
            size_t size = (size_t)w * h * bpp, i;
            uint8_t *linear = (uint8_t *)malloc(size);
            uint8_t *swizzled = (uint8_t *)malloc(size);
            uint8_t *decoded = (uint8_t *)malloc(size);
            if (!linear || !swizzled || !decoded) return 0;
            for (i = 0; i < size; i++) linear[i] = (uint8_t)random32();
            xbox_swizzle_rect(swizzled, linear, w, h, bpp);
            for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
                size_t at = (size_t)swizzle_offset(x,y,w,h) * bpp;
                if (at + bpp > size || memcmp(swizzled + at,
                        linear + ((size_t)y*w+x)*bpp, bpp)) return 0;
                checks++;
            }
            xbox_unswizzle_rect(decoded, swizzled, w, h, bpp);
            if (memcmp(decoded, linear, size)) return 0;
            free(decoded); free(swizzled); free(linear);
        }
    }
    return 1;
}

int main(int argc, char **argv)
{
    uint32_t w, h, x, y, i;
    static const uint32_t fallback[][2] = {
        {0,0}, {0,8}, {8,0}, {3,5}, {17,31}, {131072,8}, {8,131072}
    };
    if (argc > 1 && !strcmp(argv[1], "roundtrip")) {
        if (!roundtrip()) return 1;
    } else {
        /* Exhaust every coordinate for every power-of-two shape through 1K. */
        for (w = 1; w <= 1024; w <<= 1) for (h = 1; h <= 1024; h <<= 1)
            for (y = 0; y < h; y++) for (x = 0; x < w; x++)
                if (!check(x,y,w,h)) return 1;
        /* Large shapes and out-of-range coordinates, including a 32-bit result. */
        for (i = 0; i < 200000; i++) {
            w = 1u << (random32() % 17); h = 1u << (random32() % 17);
            x = random32(); y = random32();
            if (!check(x,y,w,h)) return 1;
        }
        for (i = 0; i < sizeof fallback / sizeof fallback[0]; i++)
            for (x = 0; x < 100; x++)
                if (!check(random32(),random32(),fallback[i][0],fallback[i][1])) return 1;
        if (!check(0xFFFFFFFFu,0xFFFFFFFFu,65536,65536)) return 1;
    }
    printf("%llu texture-address checks passed\n", (unsigned long long)checks);
    return 0;
}
