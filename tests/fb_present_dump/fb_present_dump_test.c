/* Capture the present path without a window or game data. Deliberately poison
 * the AA samples we must skip, and distinguish all four image corners so the
 * check catches row pitch, vertical origin and horizontal/vertical AA errors. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t memory[1280 * 960 * 4 + 4096];
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)memory; }
void xbox_InputKeySet(int vk, int down) { (void)vk; (void)down; }
void xbox_InputKeysClear(void) {}
void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch);
void xbox_FramebufferWindowSetAA(uint32_t sy);

static int exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static uint32_t read32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int check_bmp(const char *path, const uint32_t want[4])
{
    static const unsigned x[4] = {0, 639, 0, 639};
    static const unsigned y[4] = {0, 0, 479, 479};
    uint8_t header[54], bgr[3];
    FILE *f = fopen(path, "rb");
    unsigned i;
    if (!f) return 1;
    if (fread(header, 1, 54, f) != 54 || header[0] != 'B' || header[1] != 'M'
        || read32(header + 2) != 921654 || read32(header + 18) != 640
        || read32(header + 22) != 480 || header[28] != 24) {
        fclose(f);
        return 1;
    }
    for (i = 0; i < 4; i++) {
        uint32_t got;
        fseek(f, 54 + (long)(479 - y[i]) * 1920 + x[i] * 3, SEEK_SET);
        if (fread(bgr, 1, 3, f) != 3) { fclose(f); return 1; }
        got = (uint32_t)bgr[2] << 16 | (uint32_t)bgr[1] << 8 | bgr[0];
        if (got != want[i]) {
            printf("corner %u: %06X expected %06X\n", i, got, want[i]);
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    static const uint32_t rgb[4] = {0x112233, 0x445566, 0x778899, 0xAABBCC};
    static const uint32_t rgb16[4] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF};
    static const uint16_t packed16[4] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
    static const unsigned x[4] = {0, 639, 0, 639};
    static const unsigned y[4] = {0, 0, 479, 479};
    unsigned sx, sy, pitch, bpp, i;
    uint32_t va = 4096;
    if (argc != 3) return 2;
    remove(argv[2]);
    _putenv_s("RECOMP_FB_PRESENT_DUMP", "");
    if (!strcmp(argv[1], "disabled")) {
        /* An invalid address must not be read when both window and capture
         * are off. This guards the normal path's early return. */
        xbox_FramebufferWindowPresent(0xFFFFFF00, 2560);
        return exists(argv[2]);
    }
    _putenv_s("RECOMP_FB_PRESENT_DUMP", argv[2]);
    _putenv_s("RECOMP_FB_PRESENT_DUMP_EVERY", "2");
    sx = !strncmp(argv[1], "aa", 2) ? 2 : 1;
    sy = !strcmp(argv[1], "aa2x2") ? 2 : 1;
    bpp = !strcmp(argv[1], "rgb565") ? 2 : 4;
    pitch = 640 * sx * bpp;
    memset(memory, 0xE7, sizeof memory);
    for (i = 0; i < 4; i++) {
        uint8_t *at = memory + va + y[i] * sy * pitch + x[i] * sx * bpp;
        if (bpp == 2) memcpy(at, &packed16[i], 2);
        else memcpy(at, &rgb[i], 4);
    }
    xbox_FramebufferWindowSetAA(sy);
    xbox_FramebufferWindowPresent(va, pitch);
    if (exists(argv[2])) return 1;                 /* every two frames */
    xbox_FramebufferWindowPresent(va, pitch);
    if (check_bmp(argv[2], bpp == 2 ? rgb16 : rgb)) return 1;
    memset(memory, 0, sizeof memory);             /* next frame in progress */
    xbox_FramebufferWindowPresent(va, pitch);     /* no dump at flip three */
    if (check_bmp(argv[2], bpp == 2 ? rgb16 : rgb)) return 1;
    xbox_FramebufferWindowPresent(0, pitch);      /* ignored invalid surface */
    if (check_bmp(argv[2], bpp == 2 ? rgb16 : rgb)) return 1;
    remove(argv[2]);
    printf("present capture %s passed\n", argv[1]);
    return 0;
}
