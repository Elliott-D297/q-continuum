/* vizcolor.h -- colour maps for Results > Visualization (docs/VISUALIZATION_PLAN.md): presets as control points, a value
 * mapped to the 0..1 of the map (linear or log) and to the texture coordinate of a 1-D colour texture, so the colour is
 * looked up per pixel (smooth: 256 texels, linear filter; banded: N texels, nearest) and bands stay crisp. Two extra
 * texels hold what is below and above the range: the map's ends (clamp) or nothing (hide). Plain C, no drawing. */
#ifndef VIZCOLOR_H
#define VIZCOLOR_H

#include <stdbool.h>

enum { VC_BLUERED = 0, VC_COOLWARM, VC_VIRIDIS, VC_MAGMA, VC_INFERNO, VC_PLASMA, VC_TURBO, VC_JET, VC_SPECTRUM, VC_GRAY,
       VC_BWR, VC_BLACKBODY, VC__N };
extern const char *const VC_NAME[VC__N];

enum { VC_SMOOTH = 0, VC_BANDED, VC_CELL };          /* contour styles: smooth, banded in N levels, each cell its own value */
enum { VC_CLAMP = 0, VC_HIDE };                      /* beyond the range: the end colours, or not drawn */

/* the map's colour at t in 0..1 (clamped), 0..255 */
void vc_rgb(int map, double t, bool reverse, unsigned char rgb[3]);
/* value to t: linear or log over lo..hi (log needs lo > 0); outside 0..1 when out of range, NAN for NAN */
double vc_norm(double v, double lo, double hi, bool logScale);
/* the texture width for a style (smooth 256 + 2, banded levels + 2) and t to its u coordinate */
int    vc_tex_width(int style, int levels);
float  vc_u(double t, int style, int levels);
/* the texture's texels (RGBA, vc_tex_width of them): the map, then below / above the range by clip mode */
void   vc_texels(int map, bool reverse, int style, int levels, int clip, unsigned char *rgba);
/* ticks for a scalar bar: n "nice" values between lo and hi (log: decades), returns how many */
int    vc_ticks(double lo, double hi, bool logScale, int want, double *out);
/* a number for a scalar bar: 4 significant figures, an exponent when tiny or huge */
void   vc_fmt(double v, char *out, int cap);

#endif
