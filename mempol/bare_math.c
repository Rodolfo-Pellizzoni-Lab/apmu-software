/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 University of Waterloo
 */

/* bare_math.c — software math stubs for bare-metal SIFT
 *
 * Provides exp, pow, expf, log2f (and helpers) so the linker does not pull
 * in the prebuilt libm.a objects whose large lookup tables cause
 * R_RISCV_HI20 relocation-truncated errors when loaded at 0x80000000.
 *
 * Accuracy: ~3-6 ULP — sufficient for SIFT Gaussian/scale-space math.
 */

#include <stdint.h>
#include <string.h>

/* ---- bit-cast helpers ---- */
static inline uint32_t f2u(float  f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float    u2f(uint32_t u) { float  f; memcpy(&f, &u, 4); return f; }

/* ---- expf ------------------------------------------------------------ */
/* Range-reduce x = k*ln2 + r, |r|<=ln2/2, then compute e^r via Horner,
 * finally scale result by 2^k via IEEE exponent field.                  */
float expf(float x)
{
    if (x >  88.7228f) return 3.4028235e38f;
    if (x < -87.3365f) return 0.0f;

    /* Cody-Waite two-constant reduction for accuracy */
    const float inv_ln2 = 1.44269504088896340f;
    const float ln2_hi  = 6.93147182e-1f;  /* 0x3F317218 */
    const float ln2_lo  = -1.90465430e-9f; /* compensates rounding */

    float kf = x * inv_ln2;
    int   k  = (int)kf;
    if (kf < 0.0f && (float)k != kf) k--;  /* floor */

    float r = (x - (float)k * ln2_hi) - (float)k * ln2_lo;

    /* 6th-order Horner for e^r, r in [-0.347, 0.347] */
    float p = 1.0f + r * (1.0f
            + r * (5.00000000e-1f
            + r * (1.66666672e-1f
            + r * (4.16666679e-2f
            + r * (8.33333443e-3f
            + r * 1.38888892e-3f)))));

    /* Scale by 2^k: inject k into IEEE exponent field */
    int e = k + 127;
    if (e <= 0)   return 0.0f;
    if (e >= 255) return 3.4028235e38f;
    return p * u2f((uint32_t)e << 23);
}

/* double wrapper — cast to float; sufficient for SIFT's scale-space uses */
double exp(double x) { return (double)expf((float)x); }

/* ---- log2f ----------------------------------------------------------- */
/* Extract IEEE exponent, reduce mantissa to [1/sqrt2, sqrt2], then use
 * the atanh identity: ln(m) = 2*atanh((m-1)/(m+1)) truncated to 5 terms. */
float log2f(float x)
{
    if (x <= 0.0f) return -3.4028235e38f;

    uint32_t ix = f2u(x);
    int e = (int)((ix >> 23) & 0xFF) - 127;

    /* Set exponent to 0 so mantissa is in [1, 2) */
    ix = (ix & 0x007FFFFFu) | 0x3F800000u;
    float m = u2f(ix);

    /* Shift range to [1/sqrt2, sqrt2] for faster convergence */
    if (m > 1.41421356f) { m *= 0.5f; e++; }

    /* atanh series: ln(m) = 2*t*(1 + t^2/3 + t^4/5 + t^6/7 + t^8/9)
     * where t = (m-1)/(m+1), |t| < 0.172 here                         */
    float t  = (m - 1.0f) / (m + 1.0f);
    float t2 = t * t;
    float ln_m = 2.0f * t * (1.0f
               + t2 * (3.33333334e-1f
               + t2 * (2.00000003e-1f
               + t2 * (1.42857149e-1f
               + t2 *  1.11111112e-1f))));

    /* log2(m) = ln(m) * log2(e) = ln(m) / ln(2) */
    return (float)e + ln_m * 1.44269504f;
}

/* ---- logf (needed internally by pow) --------------------------------- */
float logf(float x)
{
    return log2f(x) * 6.93147181e-1f;  /* * ln(2) */
}

double log(double x) { return (double)logf((float)x); }

/* ---- powf / pow ------------------------------------------------------ */
float powf(float base, float exponent)
{
    if (base == 0.0f)  return 0.0f;
    if (base  < 0.0f)  return 0.0f;   /* not needed by SIFT */
    return expf(exponent * logf(base));
}

double pow(double base, double exponent)
{
    return (double)powf((float)base, (float)exponent);
}
