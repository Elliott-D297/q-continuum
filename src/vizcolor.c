/* vizcolor.c -- colour maps (see vizcolor.h) */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "vizcolor.h"

const char *const VC_NAME[VC__N] = { "Blue to red", "Cool to warm", "Viridis", "Magma", "Inferno", "Plasma", "Turbo", "Jet",
                                     "Spectrum", "Grayscale", "Blue, white, red", "Black body" };

/* control points: t, r, g, b (0..1) */
typedef struct { int n; float p[12][4]; } Map;
static const Map MAPS[VC__N] = {
    [VC_BLUERED]  = { 5, { { 0, 0.23f, 0.30f, 0.75f }, { 0.25f, 0.20f, 0.65f, 0.95f }, { 0.5f, 0.30f, 0.85f, 0.45f }, { 0.75f, 0.98f, 0.80f, 0.20f }, { 1, 0.85f, 0.15f, 0.12f } } },
    [VC_COOLWARM] = { 5, { { 0, 0.230f, 0.299f, 0.754f }, { 0.25f, 0.552f, 0.690f, 0.996f }, { 0.5f, 0.865f, 0.865f, 0.865f }, { 0.75f, 0.957f, 0.604f, 0.482f }, { 1, 0.706f, 0.016f, 0.150f } } },
    [VC_VIRIDIS]  = { 6, { { 0, 0.267f, 0.005f, 0.329f }, { 0.2f, 0.254f, 0.265f, 0.530f }, { 0.4f, 0.164f, 0.471f, 0.558f }, { 0.6f, 0.134f, 0.659f, 0.518f }, { 0.8f, 0.478f, 0.821f, 0.318f }, { 1, 0.993f, 0.906f, 0.144f } } },
    [VC_MAGMA]    = { 6, { { 0, 0.001f, 0.000f, 0.014f }, { 0.2f, 0.232f, 0.059f, 0.437f }, { 0.4f, 0.550f, 0.161f, 0.506f }, { 0.6f, 0.868f, 0.288f, 0.409f }, { 0.8f, 0.994f, 0.624f, 0.427f }, { 1, 0.987f, 0.991f, 0.750f } } },
    [VC_INFERNO]  = { 6, { { 0, 0.001f, 0.000f, 0.014f }, { 0.2f, 0.258f, 0.039f, 0.406f }, { 0.4f, 0.578f, 0.148f, 0.404f }, { 0.6f, 0.865f, 0.317f, 0.226f }, { 0.8f, 0.988f, 0.645f, 0.040f }, { 1, 0.988f, 0.998f, 0.645f } } },
    [VC_PLASMA]   = { 6, { { 0, 0.050f, 0.030f, 0.528f }, { 0.2f, 0.417f, 0.001f, 0.658f }, { 0.4f, 0.693f, 0.165f, 0.565f }, { 0.6f, 0.881f, 0.393f, 0.383f }, { 0.8f, 0.988f, 0.652f, 0.211f }, { 1, 0.940f, 0.975f, 0.131f } } },
    [VC_TURBO]    = { 7, { { 0, 0.190f, 0.072f, 0.232f }, { 0.17f, 0.252f, 0.493f, 0.937f }, { 0.33f, 0.110f, 0.840f, 0.769f }, { 0.5f, 0.565f, 0.993f, 0.295f }, { 0.67f, 0.969f, 0.787f, 0.188f }, { 0.83f, 0.937f, 0.353f, 0.067f }, { 1, 0.480f, 0.016f, 0.010f } } },
    [VC_JET]      = { 6, { { 0, 0, 0, 0.5f }, { 0.125f, 0, 0, 1 }, { 0.375f, 0, 1, 1 }, { 0.625f, 1, 1, 0 }, { 0.875f, 1, 0, 0 }, { 1, 0.5f, 0, 0 } } },
    [VC_SPECTRUM] = { 6, { { 0, 0.40f, 0, 0.60f }, { 0.2f, 0, 0, 1 }, { 0.4f, 0, 0.8f, 1 }, { 0.6f, 0, 1, 0.2f }, { 0.8f, 1, 1, 0 }, { 1, 1, 0, 0 } } },
    [VC_GRAY]     = { 2, { { 0, 0.05f, 0.05f, 0.05f }, { 1, 0.97f, 0.97f, 0.97f } } },
    [VC_BWR]      = { 3, { { 0, 0.10f, 0.25f, 0.85f }, { 0.5f, 1, 1, 1 }, { 1, 0.85f, 0.10f, 0.10f } } },
    [VC_BLACKBODY] = { 5, { { 0, 0, 0, 0 }, { 0.39f, 0.70f, 0.13f, 0.12f }, { 0.58f, 0.89f, 0.46f, 0.03f }, { 0.84f, 0.90f, 0.86f, 0.17f }, { 1, 1, 1, 1 } } },
};

void vc_rgb(int map, double t, bool reverse, unsigned char rgb[3])
{
    const Map *m = &MAPS[map >= 0 && map < VC__N ? map : 0];
    if (isnan(t)) t = 0;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    if (reverse) t = 1 - t;
    float c[3];
    if (t <= m->p[0][0]) { for (int k = 0; k < 3; k++) c[k] = m->p[0][k + 1]; }
    else if (t >= m->p[m->n - 1][0]) { for (int k = 0; k < 3; k++) c[k] = m->p[m->n - 1][k + 1]; }
    else {
        int i = 0;
        while (i < m->n - 2 && t > m->p[i + 1][0]) i++;
        float a = m->p[i][0], b = m->p[i + 1][0], s = b > a ? (float)((t - a) / (b - a)) : 0;
        for (int k = 0; k < 3; k++) c[k] = m->p[i][k + 1] + s * (m->p[i + 1][k + 1] - m->p[i][k + 1]);
    }
    for (int k = 0; k < 3; k++) rgb[k] = (unsigned char)lroundf(fminf(1, fmaxf(0, c[k])) * 255);
}

double vc_norm(double v, double lo, double hi, bool logScale)
{
    if (isnan(v)) return NAN;
    if (logScale && lo > 0 && hi > lo) return v > 0 ? log(v / lo) / log(hi / lo) : -1;
    return hi > lo ? (v - lo) / (hi - lo) : 0.5;
}

int vc_tex_width(int style, int levels) { return style == VC_BANDED ? (levels < 2 ? 2 : levels) + 2 : 258; }

float vc_u(double t, int style, int levels)
{
    int W = vc_tex_width(style, levels), inner = W - 2;
    if (isnan(t)) return 0.5f / W;
    if (t < 0) return 0.5f / W;                           /* the below-range texel */
    if (t > 1) return (W - 0.5f) / W;                     /* the above-range texel */
    if (style == VC_BANDED) {                            /* nearest filtering: u straight across the inner texels bands per pixel */
        double tt = t >= 1 ? 1 - 1e-6 : t;
        return (float)((1 + tt * inner) / W);
    }
    return (float)((1.5 + t * (inner - 1)) / W);          /* smooth: texel centres 1.5 .. inner + 0.5 */
}

void vc_texels(int map, bool reverse, int style, int levels, int clip, unsigned char *rgba)
{
    int W = vc_tex_width(style, levels), inner = W - 2;
    for (int i = 0; i < inner; i++) {
        double t = style == VC_BANDED ? (i + 0.5) / inner : (double)i / (inner - 1);
        vc_rgb(map, t, reverse, &rgba[4 * (i + 1)]);
        rgba[4 * (i + 1) + 3] = 255;
    }
    memcpy(&rgba[0], &rgba[4], 4);
    memcpy(&rgba[4 * (W - 1)], &rgba[4 * (W - 2)], 4);
    if (clip == VC_HIDE) { rgba[3] = 0; rgba[4 * (W - 1) + 3] = 0; }
}

static double nice(double x, bool round_)
{
    double e = floor(log10(x)), f = x / pow(10, e), nf;
    if (round_) nf = f < 1.5 ? 1 : f < 3 ? 2 : f < 7 ? 5 : 10;
    else nf = f <= 1 ? 1 : f <= 2 ? 2 : f <= 5 ? 5 : 10;
    return nf * pow(10, e);
}

int vc_ticks(double lo, double hi, bool logScale, int want, double *out)
{
    if (!(hi > lo) || want < 2) { out[0] = lo; return 1; }
    int n = 0;
    if (logScale && lo > 0) {
        for (double d = pow(10, floor(log10(lo))); d <= hi * 1.0001 && n < 32; d *= 10) if (d >= lo * 0.9999) out[n++] = d;
        if (n >= 2) return n;
        n = 0;
    }
    double step = nice(nice(hi - lo, false) / (want - 1), true);
    for (double v = ceil(lo / step - 1e-9) * step; v <= hi + step * 1e-9 && n < 32; v += step) out[n++] = fabs(v) < step * 1e-9 ? 0 : v;
    return n;
}

void vc_fmt(double v, char *out, int cap)
{
    double a = fabs(v);
    if (v == 0) snprintf(out, (size_t)cap, "0");
    else if (a >= 1e5 || a < 1e-3) snprintf(out, (size_t)cap, "%.3g", v);
    else snprintf(out, (size_t)cap, "%.4g", v);
}
