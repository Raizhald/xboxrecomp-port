/**
 * NV2A register combiners. See nv2a_combiner.h.
 *
 * Register numbers (an input or output byte's low nibble):
 *   0 zero/discard  1 c0  2 c1  3 fog  4 v0  5 v1  8-11 t0-t3  12 r0  13 r1
 *   14 v1+r0 sum and 15 E*F product (final combiner only)
 * An input byte is reg | alpha<<4 | mapping<<5. In the RGB portion the alpha
 * bit replicates .a; in the alpha portion it picks .a over .b.
 */
#include "nv2a_combiner.h"

#include <string.h>

/* The rasteriser evaluates pixels on several threads. */
#if defined(_MSC_VER)
#define NV_RC_TLS __declspec(thread)
#else
#define NV_RC_TLS __thread
#endif

enum { R_ZERO = 0, R_C0 = 1, R_C1 = 2, R_FOG = 3, R_V0 = 4, R_V1 = 5,
       R_T0 = 8, R_R0 = 12, R_R1 = 13, R_SUM = 14, R_EF = 15 };

/* n / 255.0f for every byte n, built on first use. The rasteriser unpacks
 * four texels per bilinear sample, so this ran sixteen float divisions per
 * pixel; the table holds the same quotients, bit for bit. Filling it twice
 * from two threads writes the same values, so no lock. */
static float s_unorm8[256];
static volatile int s_unorm8_ready;

static const float *unorm8(void)
{
    if (!s_unorm8_ready) {
        int i;
        for (i = 0; i < 256; i++)
            s_unorm8[i] = (float)i / 255.0f;
        s_unorm8_ready = 1;
    }
    return s_unorm8;
}

void nv2a_rc_unpack(uint32_t c, float o[4])
{
    const float *t = unorm8();
    o[0] = t[(c >> 16) & 0xFF];
    o[1] = t[(c >>  8) & 0xFF];
    o[2] = t[ c        & 0xFF];
    o[3] = t[ c >> 24        ];
}

/* D3DCOLOR to floats, memoised: a program's constants are the same for
 * every pixel of a batch, and unpacking them per pixel was a measurable
 * share of the executor. */
static const float *unpack_cached(uint32_t c)
{
    static NV_RC_TLS uint32_t key[16];
    static NV_RC_TLS float val[16][4];
    static NV_RC_TLS int valid[16];
    unsigned slot = (c ^ (c >> 8) ^ (c >> 16) ^ (c >> 24)) & 15u;
    if (!valid[slot] || key[slot] != c) {
        nv2a_rc_unpack(c, val[slot]);
        key[slot] = c;
        valid[slot] = 1;
    }
    return val[slot];
}

static float clampf(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

/* PS_INPUTMAPPING, psh.c get_input_var. */
static float map_in(float x, uint32_t byte)
{
    float p = x < 0.0f ? 0.0f : x;
    switch ((byte >> 5) & 7) {
    case 0:  return p;                               /* unsigned identity */
    case 1:  return 1.0f - clampf(x, 0.0f, 1.0f);    /* unsigned invert   */
    case 2:  return 2.0f * p - 1.0f;                 /* expand normal     */
    case 3:  return -2.0f * p + 1.0f;                /* expand negate     */
    case 4:  return p - 0.5f;                        /* half-bias normal  */
    case 5:  return -p + 0.5f;                       /* half-bias negate  */
    case 6:  return x;                               /* signed identity   */
    default: return -x;                              /* signed negate     */
    }
}

/* PS_COMBINEROUTPUT scale/bias (psh.c get_output) as (x - bias) * scale:
 * x - 0.5, x * 2, (x - 0.5) * 2, x * 4, x * 0.5 or x. Each is exact in that
 * form, since x - 0 and x * 1 are x itself. */
static void out_map(uint32_t mapping, float *bias, float *scale)
{
    switch (mapping) {
    case 0x08: *bias = 0.5f; *scale = 1.0f; return;
    case 0x10: *bias = 0.0f; *scale = 2.0f; return;
    case 0x18: *bias = 0.5f; *scale = 2.0f; return;
    case 0x20: *bias = 0.0f; *scale = 4.0f; return;
    case 0x30: *bias = 0.0f; *scale = 0.5f; return;
    default:   *bias = 0.0f; *scale = 1.0f; return;
    }
}

static void read_rgb(float R[16][4], uint32_t byte, float v[3])
{
    const float *r = R[byte & 0xF];
    float x[3];
    int i;

    if (byte & 0x10) {
        x[0] = x[1] = x[2] = r[3];
    } else {
        x[0] = r[0]; x[1] = r[1]; x[2] = r[2];
    }
    switch ((byte >> 5) & 7) {
    case 0:
        for (i = 0; i < 3; i++) v[i] = x[i] < 0.0f ? 0.0f : x[i];
        return;
    case 1:
        for (i = 0; i < 3; i++) v[i] = 1.0f - clampf(x[i], 0.0f, 1.0f);
        return;
    case 2:
        for (i = 0; i < 3; i++) v[i] = 2.0f * (x[i] < 0.0f ? 0.0f : x[i]) - 1.0f;
        return;
    case 3:
        for (i = 0; i < 3; i++) v[i] = -2.0f * (x[i] < 0.0f ? 0.0f : x[i]) + 1.0f;
        return;
    case 4:
        for (i = 0; i < 3; i++) v[i] = (x[i] < 0.0f ? 0.0f : x[i]) - 0.5f;
        return;
    case 5:
        for (i = 0; i < 3; i++) v[i] = -(x[i] < 0.0f ? 0.0f : x[i]) + 0.5f;
        return;
    case 6:
        for (i = 0; i < 3; i++) v[i] = x[i];
        return;
    default:
        for (i = 0; i < 3; i++) v[i] = -x[i];
        return;
    }
}

static float read_alpha(float R[16][4], uint32_t byte)
{
    const float *r = R[byte & 0xF];
    return map_in((byte & 0x10) ? r[3] : r[2], byte);
}

void nv2a_rc_eval(const Nv2aCombiner *rc, const float v0[4],
                  const float v1[4], const float fog[4],
                  const float t[4][4], float out[4])
{
    float R[16][4];
    uint32_t n = rc->control & 0xFF, flags = rc->control >> 8, s;
    int i;

    if (n > 8)
        n = 8;
    /* Only the registers a program can read before writing need a value:
     * zero, r0, r1 and the final-combiner specials. */
    for (i = 0; i < 4; i++) {
        R[0][i] = R[6][i] = R[7][i] = 0.0f;
        R[12][i] = R[13][i] = R[14][i] = R[15][i] = 0.0f;
    }
    for (i = 0; i < 4; i++) {
        R[R_FOG][i] = fog[i];
        R[R_V0][i] = v0[i];
        R[R_V1][i] = v1[i];
        R[R_T0 + 0][i] = t[0][i];
        R[R_T0 + 1][i] = t[1][i];
        R[R_T0 + 2][i] = t[2][i];
        R[R_T0 + 3][i] = t[3][i];
    }
    /* r0.a starts as texture 0's alpha, or 1 with stage 0 off (psh.c). */
    R[R_R0][3] = (rc->stage_program & 0x1F) ? t[0][3] : 1.0f;

    for (s = 0; s < n; s++) {
        uint32_t icw = rc->color_icw[s], ocw = rc->color_ocw[s];
        uint32_t aicw = rc->alpha_icw[s], aocw = rc->alpha_ocw[s];
        uint32_t fl = ocw >> 12, afl = aocw >> 12;
        float A[3], B[3], C[3], D[3], ab[3], cd[3], ms[3];
        float aA, aB, aC, aD, aab, acd, ams;
        int mux_cd;

        /* One c0/c1 for every stage, or one per stage (COMBINERCOUNT). */
        memcpy(R[R_C0], unpack_cached(rc->factor0[(flags & 0x010) ? s : 0]), 16);
        memcpy(R[R_C1], unpack_cached(rc->factor1[(flags & 0x100) ? s : 0]), 16);
        mux_cd = (flags & 1) ? R[R_R0][3] >= 0.5f
                             : ((int)(R[R_R0][3] * 255.0f) & 1);

        /* Both portions read before either writes (psh.c emits all the
         * arithmetic, then the assignments). */
        read_rgb(R, icw >> 24, A); read_rgb(R, icw >> 16, B);
        read_rgb(R, icw >> 8, C);  read_rgb(R, icw, D);
        aA = read_alpha(R, aicw >> 24); aB = read_alpha(R, aicw >> 16);
        aC = read_alpha(R, aicw >> 8);  aD = read_alpha(R, aicw);

        {
            /* The dot products and the output mapping are the same for all
             * three channels; work them out once (out_map: the same values the per-channel
             * scale/bias switch gave). */
            float dab = A[0] * B[0] + A[1] * B[1] + A[2] * B[2];
            float dcd = C[0] * D[0] + C[1] * D[1] + C[2] * D[2];
            float ob, os;
            out_map(fl & 0x38, &ob, &os);
            for (i = 0; i < 3; i++) {
                float pab = (fl & 2) ? dab : A[i] * B[i];
                float pcd = (fl & 1) ? dcd : C[i] * D[i];
                float sum = (fl & 4) ? (mux_cd ? pcd : pab) : pab + pcd;
                ab[i] = clampf((pab - ob) * os, -1.0f, 1.0f);
                cd[i] = clampf((pcd - ob) * os, -1.0f, 1.0f);
                ms[i] = clampf((sum - ob) * os, -1.0f, 1.0f);
            }
        }
        {
            float pab = aA * aB, pcd = aC * aD, ob, os;
            float sum = (afl & 4) ? (mux_cd ? pcd : pab) : pab + pcd;
            out_map(afl & 0x38, &ob, &os);
            aab = clampf((pab - ob) * os, -1.0f, 1.0f);
            acd = clampf((pcd - ob) * os, -1.0f, 1.0f);
            ams = clampf((sum - ob) * os, -1.0f, 1.0f);
        }

        /* RGB destinations; blue-to-alpha also writes the destination's .a. */
        if ((ocw >> 4) & 0xF) {
            float *d = R[(ocw >> 4) & 0xF];
            d[0] = ab[0]; d[1] = ab[1]; d[2] = ab[2];
            if (fl & 0x80) d[3] = ab[2];
        }
        if (ocw & 0xF) {
            float *d = R[ocw & 0xF];
            d[0] = cd[0]; d[1] = cd[1]; d[2] = cd[2];
            if (fl & 0x40) d[3] = cd[2];
        }
        if ((ocw >> 8) & 0xF) {
            float *d = R[(ocw >> 8) & 0xF];
            d[0] = ms[0]; d[1] = ms[1]; d[2] = ms[2];
        }
        if ((aocw >> 4) & 0xF) R[(aocw >> 4) & 0xF][3] = aab;
        if (aocw & 0xF)        R[aocw & 0xF][3] = acd;
        if ((aocw >> 8) & 0xF) R[(aocw >> 8) & 0xF][3] = ams;
        /* Register 0 is zero whatever was "written" to it. */
        R[R_ZERO][0] = R[R_ZERO][1] = R[R_ZERO][2] = R[R_ZERO][3] = 0.0f;
    }

    if (rc->final0 || rc->final1) {
        uint32_t f0 = rc->final0, f1 = rc->final1, fflags = f1 & 0xFF;
        float A[3], B[3], C[3], D[3], E[3], F[3];

        memcpy(R[R_C0], unpack_cached(rc->final_c0), 16);
        memcpy(R[R_C1], unpack_cached(rc->final_c1), 16);
        /* V1R0 sum: optional complements (0x40 v1, 0x20 r0), clamp (0x80). */
        for (i = 0; i < 3; i++) {
            float a = (fflags & 0x40) ? 1.0f - R[R_V1][i] : R[R_V1][i];
            float b = (fflags & 0x20) ? 1.0f - R[R_R0][i] : R[R_R0][i];
            R[R_SUM][i] = (fflags & 0x80) ? clampf(a + b, 0.0f, 1.0f) : a + b;
        }
        R[R_SUM][3] = 0.0f;
        read_rgb(R, f1 >> 24, E);
        read_rgb(R, f1 >> 16, F);
        for (i = 0; i < 3; i++)
            R[R_EF][i] = E[i] * F[i];
        R[R_EF][3] = 0.0f;
        read_rgb(R, f0 >> 24, A); read_rgb(R, f0 >> 16, B);
        read_rgb(R, f0 >> 8, C);  read_rgb(R, f0, D);
        /* D + mix(C, B, A), alpha from G. */
        for (i = 0; i < 3; i++)
            out[i] = D[i] + C[i] * (1.0f - A[i]) + B[i] * A[i];
        out[3] = read_alpha(R, f1 >> 8);
    } else {
        for (i = 0; i < 4; i++)
            out[i] = R[R_R0][i];
    }
    for (i = 0; i < 4; i++)
        out[i] = clampf(out[i], 0.0f, 1.0f);
}
