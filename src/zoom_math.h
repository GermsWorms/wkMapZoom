/* wkMapZoom's arithmetic: no game, Windows or Direct3D calls (native test: zoom_math_test.c). */
#ifndef WKMAPZOOM_MATH_H
#define WKMAPZOOM_MATH_H
#include <math.h>

typedef struct {
    double max_out;       /* the world buffer is this many times the game resolution each way: the most zoomed out */
    double max_in;        /* the most zoomed in */
    double step;          /* zoom per wheel notch */
    int smooth;           /* ease to a new zoom (else jump) */
} zoom_cfg;
#define ZOOM_CFG_DEFAULT {4.0, 8.0, 1.15, 1}
#define ZOOM_MAX_FRAME 16384  /* the world buffer's largest side */
#define ZOOM_EASE_S 0.06      /* the easing's time constant */

static double zoom_clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

static void zoom_cfg_sane(zoom_cfg *c) {
    if (!(c->max_out == c->max_out)) c->max_out = 4.0;   /* (NaN) */
    if (!(c->max_in == c->max_in)) c->max_in = 8.0;
    if (!(c->step == c->step)) c->step = 1.15;
    c->max_out = zoom_clampd(c->max_out, 1.0, 8.0);
    c->max_in = zoom_clampd(c->max_in, 1.0, 16.0);
    c->step = zoom_clampd(c->step, 1.01, 2.0);
}

static int zoom_even(double v) { int i = (int)floor(v / 2.0 + 0.5) * 2; return i < 2 ? 2 : i; }

/* the world buffer for a game resolution sw x sh: max_out times it, even, at most ZOOM_MAX_FRAME a side */
static void zoom_frame(int sw, int sh, double max_out, int *gw, int *gh) {
    double k = max_out, kw = (double)ZOOM_MAX_FRAME / sw, kh = (double)ZOOM_MAX_FRAME / sh;
    if (k > kw) k = kw;
    if (k > kh) k = kh;
    if (k < 1.0) k = 1.0;
    *gw = zoom_even(sw * k);
    *gh = zoom_even(sh * k);
    if (*gw > ZOOM_MAX_FRAME && sw <= ZOOM_MAX_FRAME) *gw = ZOOM_MAX_FRAME & ~1;
    if (*gh > ZOOM_MAX_FRAME && sh <= ZOOM_MAX_FRAME) *gh = ZOOM_MAX_FRAME & ~1;
    if (*gw < sw) *gw = sw;
    if (*gh < sh) *gh = sh;
}

/* the zoom range: out as far as the world buffer holds the screen's view, in to max_in */
static void zoom_limits(int sw, int sh, int gw, int gh, const zoom_cfg *c, double *lo, double *hi) {
    double out_w = (double)sw / gw, out_h = (double)sh / gh;
    *lo = out_w > out_h ? out_w : out_h;
    if (*lo > 1.0) *lo = 1.0;
    *hi = c->max_in < 1.0 ? 1.0 : c->max_in;
}

static double zoom_clamp(double z, double lo, double hi) { return zoom_clampd(z, lo, hi); }

/* a wheel turn of `notches` (part notches allowed): 100% is a stop on the way through */
static double zoom_step(double z, double notches, double step) {
    double n = z * pow(step, notches);
    if ((z < 1.0 && n > 1.0) || (z > 1.0 && n < 1.0) || fabs(log(n)) < 0.02) n = 1.0;
    return n;
}

/* the zoom eased toward the goal over dt seconds (even steps in log scale), landing on it exactly */
static double zoom_ease(double now, double goal, double dt) {
    double a, b;
    if (now <= 0.0 || goal <= 0.0) return goal;
    if (dt <= 0.0) return now;
    if (dt > 0.25) dt = 0.25;
    a = log(now); b = log(goal);
    a += (b - a) * (1.0 - exp(-dt / ZOOM_EASE_S));
    return fabs(a - b) < 0.003 ? goal : exp(a);
}

/* The world's view at zoom z. WA shows the world in the screen rectangle [cx1, cx2) x [cy1, cy2) with the camera's
 * centre at screen point (ox, oy). Zoomed, WA draws the world at its own scale into a buffer region of rw x rh (the
 * rectangle's size / z, at most gw x gh) with the camera's centre at (bx, by) there; screen pixel x shows buffer
 * column zoom_src(x, ox, bx, z, rw). */
static void zoom_region(int cx1, int cy1, int cx2, int cy2, int ox, int oy, double z, int gw, int gh,
                        int *rw, int *rh, int *bx, int *by) {
    *rw = (int)ceil((cx2 - cx1) / z);
    *rh = (int)ceil((cy2 - cy1) / z);
    if (*rw > gw) *rw = gw;
    if (*rh > gh) *rh = gh;
    if (*rw < 1) *rw = 1;
    if (*rh < 1) *rh = 1;
    *bx = (int)floor((ox - cx1) / z + 0.5);
    *by = (int)floor((oy - cy1) / z + 0.5);
}

/* the buffer column (or row) screen pixel p shows: nearest, kept inside [0, r) */
static int zoom_src(int p, int o, int b, double z, int r) {
    int q = (int)floor(b + (p + 0.5 - o) / z);
    return q < 0 ? 0 : q >= r ? r - 1 : q;
}
#endif
