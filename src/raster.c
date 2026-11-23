/* raster.c: see raster.h. The rules per pixel follow wkVista's rasterizer (MIT, Copyright (c) 2026 ropahektic,
 * github.com/ropahektic/wkVista); the area averaging, the RGB blending and the land's shrunk copies are wkMapZoom's. */
#include <math.h>
#include <string.h>
#include "raster.h"

/* ---- blending ---------------------------------------------------------------------------------------------------- */
/* o under premultiplied (r, g, b) of coverage a, all in 1/65536 */
static inline uint32_t mix(uint32_t o, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    uint32_t ia = a >= 65536u ? 0u : 65536u - a;
    uint32_t ro = (((o >> 16) & 255u) * ia + r + 32768u) >> 16;
    uint32_t go = (((o >> 8) & 255u) * ia + g + 32768u) >> 16;
    uint32_t bo = ((o & 255u) * ia + b + 32768u) >> 16;
    if (ro > 255u) ro = 255u;
    if (go > 255u) go = 255u;
    if (bo > 255u) bo = 255u;
    return ro << 16 | go << 8 | bo;
}

/* colour c (0x00RRGGBB) covering w (1/65536) of o */
static inline uint32_t over(uint32_t o, uint32_t c, uint32_t w) {
    if (w >= 65536u) return c & 0xffffffu;
    return mix(o, ((c >> 16) & 255u) * w, ((c >> 8) & 255u) * w, (c & 255u) * w, w);
}

/* ---- taps -------------------------------------------------------------------------------------------------------- */
/* One output pixel p along an axis (o, b, z): the drawn part [lo, hi) of the region it covers, as texels of
 * u = (s + off) / S. Weights rounded so that they add up exactly (256 when it is fully covered); barely touched texels
 * at the ends (weight 0 after rounding) left out. */
static void tap_one(rtap *t, int p, int o, double b, double z, double lo, double hi, double off, int S, int point) {
    const double inv = 1.0 / z, scale = z * 256.0 * S;
    double s0 = b + (p - o) * inv, s1 = s0 + inv, u0, u1, cum = 0.0;
    int q0, q1, k, prev = 0;
    t->n = 0;
    t->sum = 0;
    t->k0 = 0;
    if (point) {                            /* the region pixel under the output pixel's centre, all of it */
        const double s = b + (p + 0.5 - o) * inv;
        if (s < lo || s >= hi) return;
        t->k0 = (int32_t)floor((s + off) / S);
        t->w[0] = 256;
        t->sum = 256;
        t->n = 1;
        return;
    }
    if (s0 < lo) s0 = lo;
    if (s1 > hi) s1 = hi;
    if (s1 <= s0) return;
    u0 = (s0 + off) / S;
    u1 = (s1 + off) / S;
    q0 = (int)floor(u0);
    q1 = (int)ceil(u1);
    if (q1 <= q0) q1 = q0 + 1;
    if (q1 - q0 > RASTER_MAX_TAPS) q1 = q0 + RASTER_MAX_TAPS;
    for (k = q0; k < q1; k++) {
        double a = k > u0 ? k : u0, e = k + 1 < u1 ? k + 1 : u1;
        int w;
        if (e > a) cum += (e - a) * scale;
        w = (int)floor(cum + 0.5) - prev;
        if (w <= 0) { if (!t->n) continue; w = 0; }
        if (!t->n) t->k0 = k;
        prev += w;
        t->w[t->n++] = (int16_t)w;
    }
    while (t->n && !t->w[t->n - 1]) t->n--;
    t->sum = (int16_t)(prev > 256 ? 256 : prev);
}

/* a snapped edge pixel's tap: its weights scaled to add up to 256 (all of the pixel, its colour the average of the part
 * the operation covers) */
static void tap_whole(rtap *t) {
    int k, sum = 0, prev = 0;
    double cum = 0.0;
    for (k = 0; k < t->n; k++) sum += t->w[k];
    if (sum <= 0 || sum >= 256) return;
    for (k = 0; k < t->n; k++) {
        int w;
        cum += t->w[k] * 256.0 / sum;
        w = (int)floor(cum + 0.5) - prev;
        prev += w;
        t->w[k] = (int16_t)w;
    }
    t->sum = 256;
}

void raster_axis(rtap *t, int p0, int p1, int o, double b, double z, double off, int level, int point) {
    int p;
    for (p = p0; p < p1; p++) tap_one(t + (p - p0), p, o, b, z, -1e18, 1e18, off, 1 << level, point);
}

int raster_level(const rland *L, double z) {
    int l = 0;
    if (!L) return 0;
    while (l < L->levels && L->mip[l + 1] && z * (double)(2 << l) <= 1.0 + 1e-9) l++;
    return l;
}

int raster_prepare(const rop *op, const rview *v, rprep *p) {
    const double z = v->z;
    const double fx1 = (op->l - v->bx) * z + v->ox, fx2 = (op->r - v->bx) * z + v->ox;
    const double fy1 = (op->t - v->by) * z + v->oy, fy2 = (op->b - v->by) * z + v->oy;
    /* Edges at least a pixel apart are snapped to whole output pixels: an edge pixel is all the operation's or none of
     * it. Blending each edge pixel by the share an operation covers let what was under it show through where two
     * operations meet (A over B over what was there: c(1 - c) of it left), a dark line between the sky gradient's
     * strips, the background's pieces and the land's tiles (a friend's screenshot, 2026-11-22). Thinner than a pixel
     * along an axis, an operation keeps its share there, so thin lines stay visible zoomed out. */
    const int snapx = !v->point && fx2 - fx1 >= 1.0, snapy = !v->point && fy2 - fy1 >= 1.0;
    double offx = 0.0, offy = 0.0;
    int level = 0, S;
    if (op->r <= op->l || op->b <= op->t || !v->gx[0] || !v->gy[0]) return 0;
    p->x1 = snapx ? (int)floor(fx1 + 0.5) : (int)floor(fx1);
    p->x2 = snapx ? (int)floor(fx2 + 0.5) : (int)ceil(fx2);
    p->y1 = snapy ? (int)floor(fy1 + 0.5) : (int)floor(fy1);
    p->y2 = snapy ? (int)floor(fy2 + 0.5) : (int)ceil(fy2);
    if (p->x1 < v->x1) p->x1 = v->x1;
    if (p->y1 < v->y1) p->y1 = v->y1;
    if (p->x2 > v->x2) p->x2 = v->x2;
    if (p->y2 > v->y2) p->y2 = v->y2;
    if (p->x1 >= p->x2 || p->y1 >= p->y2) return 0;
    if (op->kind == ROP_BITMAP && op->land_x >= 0 && v->land && !v->point && !op->no_mip) {
        level = raster_level(v->land, z);
        offx = (double)op->col0 + op->land_x;
        offy = (double)op->row0 + op->land_y;
        /* (the frame's taps of a copy are made for the land's place this frame; a tile somewhere else: from the tile) */
        if (level && (!v->gx[level] || !v->gy[level] || v->gox[level] != offx || v->goy[level] != offy)) level = 0;
    }
    if (!level) offx = offy = 0.0;
    S = 1 << level;
    p->level = level;
    p->gx = v->gx[level];
    p->gy = v->gy[level];
    tap_one(&p->ex[0], p->x1, v->ox, v->bx, z, op->l, op->r, offx, S, v->point);
    tap_one(&p->ex[1], p->x2 - 1, v->ox, v->bx, z, op->l, op->r, offx, S, v->point);
    tap_one(&p->ey[0], p->y1, v->oy, v->by, z, op->t, op->b, offy, S, v->point);
    tap_one(&p->ey[1], p->y2 - 1, v->oy, v->by, z, op->t, op->b, offy, S, v->point);
    if (snapx) { tap_whole(&p->ex[0]); tap_whole(&p->ex[1]); }
    if (snapy) { tap_whole(&p->ey[0]); tap_whole(&p->ey[1]); }
    if (level) {
        p->xbase = 0; p->xdir = 1; p->xn = v->land->mw[level];
        p->ybase = 0; p->yn = v->land->mh[level];
    } else {
        p->xbase = op->col0; p->xdir = op->dir; p->xn = op->src.w;
        p->ybase = op->row0; p->yn = op->src.h;
    }
    return 1;
}

#define TAPX(X) ((X) == p->x1 ? &p->ex[0] : (X) == p->x2 - 1 ? &p->ex[1] : &p->gx[(X) - v->x1])
#define TAPY(Y) ((Y) == p->y1 ? &p->ey[0] : (Y) == p->y2 - 1 ? &p->ey[1] : &p->gy[(Y) - v->y1])

/* ---- drawing ----------------------------------------------------------------------------------------------------- */
/* the source rows of a row's taps (NULL: outside the source, it reads 0) */
static void tap_rows(const rop *op, const rprep *p, const rtap *ty, const uint8_t **rp) {
    int ky;
    for (ky = 0; ky < ty->n; ky++) {
        const int r = p->ybase + ty->k0 + ky;
        rp[ky] = (unsigned)r < (unsigned)p->yn ? op->src.data + (size_t)r * op->src.stride : NULL;
    }
}
static inline int rd(const uint8_t *rp, int c, int n) { return rp && (unsigned)c < (unsigned)n ? rp[c] : 0; }

/* fills, pixels and lines: one colour, covering each output pixel by its area's share */
static void row_fill(const rop *op, const rview *v, const rprep *p, const rtap *ty, uint32_t *row) {
    const uint32_t c = v->pal[op->index], wy = (uint32_t)ty->sum;
    int X;
    if (wy == 256u) {                       /* (most rows: inside it, only its edge columns partly covered) */
        for (X = p->x1 + 1; X < p->x2 - 1; X++) row[X] = c;
        row[p->x1] = over(row[p->x1], c, (uint32_t)p->ex[0].sum * 256u);
        if (p->x2 - 1 > p->x1) row[p->x2 - 1] = over(row[p->x2 - 1], c, (uint32_t)p->ex[1].sum * 256u);
        return;
    }
    for (X = p->x1; X < p->x2; X++) {
        const uint32_t wx = (uint32_t)TAPX(X)->sum;
        if (wx) row[X] = over(row[X], c, wx * wy);
    }
}

/* bitmaps and sprites whose see-through test is one colour (or none): most of what WA draws */
static void row_plain(const rop *op, const rview *v, const rprep *p, const rtap *ty, uint32_t *row) {
    const uint8_t *rp[RASTER_MAX_TAPS];
    const uint32_t *pal = v->pal;
    const int all = op->kind == ROP_BITMAP ? !op->keyed : op->mode == 0;
    const int key = op->kind == ROP_BITMAP ? op->key : 0, ny = ty->n, xb = p->xbase, xd = p->xdir, xn = p->xn;
    int X, ky, kx;
    tap_rows(op, p, ty, rp);
    for (X = p->x1; X < p->x2; X++) {
        const rtap *tx = TAPX(X);
        const int nx = tx->n, c0 = xb + xd * tx->k0;
        int first;
        uint32_t R = 0, G = 0, B = 0, A = 0;
        if (!nx) continue;
        first = rd(rp[0], c0, xn);
        if (nx == 1 && ny == 1) {
            if (all || first != key) row[X] = over(row[X], pal[first], (uint32_t)tx->w[0] * (uint32_t)ty->w[0]);
            continue;
        }
        for (ky = 0; ky < ny; ky++)
            for (kx = 0; kx < nx; kx++)
                if (rd(rp[ky], c0 + xd * kx, xn) != first) goto mixed;
        if (all || first != key) row[X] = over(row[X], pal[first], (uint32_t)tx->sum * (uint32_t)ty->sum);
        continue;
    mixed:
        for (ky = 0; ky < ny; ky++) {
            const uint32_t wy = (uint32_t)ty->w[ky];
            for (kx = 0; kx < nx; kx++) {
                const int s = rd(rp[ky], c0 + xd * kx, xn);
                if (all || s != key) {
                    const uint32_t c = pal[s], w = wy * (uint32_t)tx->w[kx];
                    R += ((c >> 16) & 255u) * w; G += ((c >> 8) & 255u) * w; B += (c & 255u) * w; A += w;
                }
            }
        }
        if (A) row[X] = mix(row[X], R, G, B, A);
    }
}

/* At 100% (z == 1, whole-pixel camera and edges): each output pixel is exactly one source pixel of weight 256 x 256,
 * so row_plain's work comes down to this copy (bit for bit the same: over() with a full weight returns the colour). */
int raster_fast = 1;                        /* (0: no fast paths -- the test draws both ways and compares) */
static int copy1_ok(const rview *v, const rprep *p, const rtap *ty) {
    const rtap *a = TAPX(p->x1), *b = TAPX(p->x2 - 1);
    return raster_fast && v->z == 1.0 && !p->level && ty->n == 1 && ty->w[0] == 256 && a->n == 1 && a->w[0] == 256 &&
           b->n == 1 && b->w[0] == 256 && b->k0 - a->k0 == p->x2 - 1 - p->x1;
}
static void row_copy1(const rop *op, const rview *v, const rprep *p, const rtap *ty, uint32_t *row) {
    const uint8_t *rp[RASTER_MAX_TAPS];
    const uint32_t *pal = v->pal;
    const int all = op->kind == ROP_BITMAP ? !op->keyed : op->mode == 0;
    const int key = op->kind == ROP_BITMAP ? op->key : 0, xd = p->xdir, xn = p->xn;
    int X, c = p->xbase + xd * TAPX(p->x1)->k0;
    tap_rows(op, p, ty, rp);
    for (X = p->x1; X < p->x2; X++, c += xd) {
        const int s = rd(rp[0], c, xn);
        if (all || s != key) row[X] = pal[s] & 0xffffffu;
    }
}

/* everything else, tap by tap: blend tables, modes 2 and 3 (they look at what is under them), full-colour land */
static void row_general(const rop *op, const rview *v, const rprep *p, const rtap *ty, uint32_t *row) {
    const uint8_t *rp[RASTER_MAX_TAPS];
    const uint32_t *pal = v->pal;
    const rland *L = v->land;
    const int ny = ty->n, fullc = op->kind == ROP_BITMAP && op->land_x >= 0 && L && L->full;
    const int xb = p->xbase, xd = p->xdir, xn = p->xn;
    int X, ky, kx;
    tap_rows(op, p, ty, rp);
    for (X = p->x1; X < p->x2; X++) {
        const rtap *tx = TAPX(X);
        const int nx = tx->n, c0 = xb + xd * tx->k0;
        int dst = -1;
        uint32_t R = 0, G = 0, B = 0, A = 0;
        if (!nx) continue;
        for (ky = 0; ky < ny; ky++) {
            const uint32_t wy = (uint32_t)ty->w[ky];
            for (kx = 0; kx < nx; kx++) {
                const int ci = c0 + xd * kx, s = rd(rp[ky], ci, xn);
                uint32_t c, w;
                if (op->kind == ROP_BITMAP) {
                    if (op->keyed && s == op->key) continue;
                    c = pal[s];
                    if (fullc && rp[ky] && (unsigned)ci < (unsigned)xn) {
                        const int lx = op->land_x + ci, ly = op->land_y + p->ybase + ty->k0 + ky;
                        if (lx >= 0 && ly >= 0 && lx < L->w && ly < L->h) {
                            const size_t li = (size_t)ly * L->w + lx;
                            if (L->full[li] && L->snap[li] == s) c = L->full[li] & 0xffffffu;
                        }
                    }
                } else {
                    if (op->mode != 0 && s == 0) continue;
                    if ((op->mode == 2 || op->mode == 3 || op->lut) && dst < 0) dst = v->nearest(row[X]);
                    if (op->mode == 2 && dst == 0) continue;
                    if (op->mode == 3 && dst != 0) continue;
                    c = op->mode == 1 && op->lut ? pal[op->lut[(dst << 8) | s]] : pal[s];
                }
                w = wy * (uint32_t)tx->w[kx];
                R += ((c >> 16) & 255u) * w; G += ((c >> 8) & 255u) * w; B += (c & 255u) * w; A += w;
            }
        }
        if (A) row[X] = mix(row[X], R, G, B, A);
    }
}

/* the land from a shrunk copy (premultiplied texels) */
static void row_mip(const rop *op, const rview *v, const rprep *p, const rtap *ty, uint32_t *row) {
    const rland *L = v->land;
    const uint32_t *M = L->mip[p->level], *mp[RASTER_MAX_TAPS];
    const int mw = L->mw[p->level], mh = L->mh[p->level], ny = ty->n;
    int X, ky, kx;
    (void)op;
    for (ky = 0; ky < ny; ky++) {
        const int r = ty->k0 + ky;
        mp[ky] = (unsigned)r < (unsigned)mh ? M + (size_t)r * mw : NULL;
    }
    for (X = p->x1; X < p->x2; X++) {
        const rtap *tx = TAPX(X);
        const int nx = tx->n, c0 = tx->k0;
        uint32_t first, R = 0, G = 0, B = 0, A = 0, W;
        if (!nx) continue;
        first = mp[0] && (unsigned)c0 < (unsigned)mw ? mp[0][c0] : 0;
        for (ky = 0; ky < ny; ky++)
            for (kx = 0; kx < nx; kx++)
                if ((mp[ky] && (unsigned)(c0 + kx) < (unsigned)mw ? mp[ky][c0 + kx] : 0) != first) goto mixed;
        if (!(first >> 24)) continue;
        W = (uint32_t)tx->sum * (uint32_t)ty->sum;
        if (first >> 24 == 255u) { row[X] = over(row[X], first, W); continue; }
        R = ((first >> 16) & 255u) * W; G = ((first >> 8) & 255u) * W; B = (first & 255u) * W; A = (first >> 24) * W;
        row[X] = mix(row[X], R, G, B, A / 255u);
        continue;
    mixed:
        for (ky = 0; ky < ny; ky++) {
            const uint32_t wy = (uint32_t)ty->w[ky];
            if (!mp[ky]) continue;
            for (kx = 0; kx < nx; kx++) {
                uint32_t t, w;
                if ((unsigned)(c0 + kx) >= (unsigned)mw) continue;
                t = mp[ky][c0 + kx];
                if (!(t >> 24)) continue;
                w = wy * (uint32_t)tx->w[kx];
                R += ((t >> 16) & 255u) * w; G += ((t >> 8) & 255u) * w; B += (t & 255u) * w; A += (t >> 24) * w;
            }
        }
        if (A) row[X] = mix(row[X], R, G, B, A / 255u);
    }
}

/* ---- the fast rows (raster_fast): SSE2, bit for bit what row_plain and row_mip make -------------------------------
 * Both add up w_x * w_y * texel over an output pixel's taps, see-through texels left out, in integers, so the order
 * does not matter: first down (per texel of a row's taps, V = the sum over the tap rows of w_y * texel: channels
 * B, G, R and the coverage A, 16 bits each), then across (per output pixel, the sum over its tap columns of w_x * V, 32
 * bits), then mix() as row_plain / row_mip do. Why it is the same bit for bit:
 *  - V fits 16 bits exactly: a channel <= 255 and the weights down add up to <= 256, so V <= 65280. V is kept less
 *    32768 (pmaddwd multiplies signed 16 bits) and 32768 * sum(w_x) added back after.
 *  - row_plain's texels come through a copy of the palette with the coverage as its 4th byte (1; the see-through
 *    colour all 0): a texel left out == a texel adding 0. row_mip's texels of coverage 0 are made 0 (row_mip skips
 *    them).
 *  - row_plain / row_mip first look whether all of a pixel's texels are the same, and then use over(o, c, W) /
 *    mix(o, c * W, a * W / 255) with W = sum_x * sum_y. With W <= 65536 over(o, c, W) == mix(o, c * W, W) (over()
 *    returns c at W = 65536, and so does mix() at a coverage of 65536), so that is what the sums make too -- as long as
 *    each tap's sum is its weights' total: checked for every tap used, else the scalar rows draw it.
 *  - mix(): o * (65536 - coverage) exact as pmaddwd of (4 o, o) by ((65536 - coverage) >> 2, (65536 - coverage) & 3)
 *    for any coverage 0 .. 65536; row_mip's coverage A / 255 by the same multiply and shift as the compiler's; the same
 *    32-bit sums and rounding; the clamp to 255 by the packs' saturation; a pixel of coverage A == 0 left as it was (its
 *    4th byte too: row_plain / row_mip do not write it).
 *  - an opaque bitmap's row whose pixels are all fully covered (every tap sum 256): mix() is (sum + 32768) >> 16 there,
 *    so it is that, with no tests.
 * A row is drawn in parts of at most FAST_SPAN texels and FAST_COLS output columns; each part's columns get their
 * weights ready once (a plan), then its rows are drawn, two pixels at a time. Columns, rows and parts the fast rows
 * cannot take (a tap whose sum is not its weights' total, a direction other than +-1, more than FAST_PARTS parts) and
 * the 100% copy rows (row_copy1) are drawn by the scalar rows. */
#include <emmintrin.h>

#define FAST_SPAN 512
#define FAST_COLS 128
#define FAST_PLAN 3                         /* tap column pairs a plan holds (more: the taps read as they are) */
#define FAST_PARTS 64
static const uint8_t zero_row[FAST_SPAN];
/* a part: output columns [xa, xe) (its first and last with taps), texels kmin .. kmin + cnt - 1, at most `pairs`
 * pairs of tap columns a column */
typedef struct { int xa, xe, kmin, cnt, inside, pairs, full; } fpart;   /* (full: every column's sum 256) */
/* a part's columns: the first tap's V (in uint16_t), the weights by pairs, and the sums' start (32768 * the weights'
 * total, so V less 32768 adds up right, and B, G, R's rounding) */
typedef struct {
    uint32_t w[FAST_COLS][FAST_PLAN][4], c[FAST_COLS][4];   /* (read as __m128i: 16-byte aligned) */
    int off[FAST_COLS];
} __attribute__((aligned(16))) fplan;

/* a tap the fast rows can use: its sum its weights' total (at most 256), and not texels of weight 0 only (the module
 * never makes such a tap; the plain rows would still clear the pixel's 4th byte there -- an independent check) */
static int tap_ok(const rtap *t) {
    int k, s = 0;
    if (t->n < 0 || t->n > RASTER_MAX_TAPS) return 0;
    for (k = 0; k < t->n; k++) {
        if (t->w[k] < 0) return 0;
        s += t->w[k];
    }
    return s == t->sum && s <= 256 && (s > 0 || !t->n);
}

/* down, row_plain: V for texels 0 .. cnt - 1 of the part; sp[ky]: the tap row's palette indices, pp: the palette with
 * coverage, 4 x 16 bits a colour. Row by row (the first one sets V). */
static void down_plain(const uint16_t *pp, const uint8_t *const *sp, const __m128i *wv, int ny, int cnt, int pad,
                       uint16_t *V) {
    const __m128i bias = _mm_set1_epi16((short)0x8000);
    int i, ky;
#define PAL(I) _mm_loadl_epi64((const __m128i *)(pp + 4 * (I)))
#define DOWN_PLAIN(A0, A1, A)                                                                                     \
        for (i = 0; i + 4 <= cnt; i += 4, d += 16) {                                                             \
            uint32_t q;                                                                                          \
            __m128i c01, c23;                                                                                    \
            memcpy(&q, s + i, 4);                                                                                \
            c01 = _mm_unpacklo_epi64(PAL(q & 255u), PAL((q >> 8) & 255u));                                       \
            c23 = _mm_unpacklo_epi64(PAL((q >> 16) & 255u), PAL(q >> 24));                                       \
            _mm_storeu_si128((__m128i *)d, _mm_add_epi16(A0, _mm_mullo_epi16(c01, w)));                          \
            _mm_storeu_si128((__m128i *)(d + 8), _mm_add_epi16(A1, _mm_mullo_epi16(c23, w)));                    \
        }                                                                                                        \
        for (; i < cnt; i++, d += 4) _mm_storel_epi64((__m128i *)d, _mm_add_epi16(A, _mm_mullo_epi16(PAL(s[i]), w)));
    for (ky = 0; ky < ny; ky++) {
        const uint8_t *s = sp[ky];
        const __m128i w = wv[ky];
        uint16_t *d = V;
        if (!ky) { DOWN_PLAIN(bias, bias, bias) }
        else { DOWN_PLAIN(_mm_loadu_si128((const __m128i *)d), _mm_loadu_si128((const __m128i *)(d + 8)),
                          _mm_loadl_epi64((const __m128i *)d)) }
    }
#undef DOWN_PLAIN
#undef PAL
    for (i = cnt; i < cnt + pad; i++) _mm_storel_epi64((__m128i *)(V + 4 * i), bias);
}

/* down, row_mip: texels kmin + [lo, hi) of the copy's rows mr[0..nr) (nr >= 1; the other texels 0) */
static void down_mip(const uint32_t *const *mr, const __m128i *wv, int nr, int kmin, int lo, int hi, int cnt, int pad,
                     uint16_t *V) {
    const __m128i z = _mm_setzero_si128(), bias = _mm_set1_epi16((short)0x8000);
    int i, r;
    for (i = 0; i < lo; i++) _mm_storel_epi64((__m128i *)(V + 4 * i), bias);
#define DOWN_MIP(A0, A1, A)                                                                                       \
        for (i = lo; i + 4 <= hi; i += 4, d += 16) {                                                             \
            __m128i t = _mm_loadu_si128((const __m128i *)(s + i));                                               \
            t = _mm_andnot_si128(_mm_cmpeq_epi32(_mm_srli_epi32(t, 24), z), t);                                  \
            _mm_storeu_si128((__m128i *)d, _mm_add_epi16(A0, _mm_mullo_epi16(_mm_unpacklo_epi8(t, z), w)));    \
            _mm_storeu_si128((__m128i *)(d + 8), _mm_add_epi16(A1, _mm_mullo_epi16(_mm_unpackhi_epi8(t, z), w))); \
        }                                                                                                        \
        for (; i < hi; i++, d += 4) {                                                                            \
            const uint32_t t = s[i] >> 24 ? s[i] : 0u;                                                           \
            _mm_storel_epi64((__m128i *)d,                                                                       \
                             _mm_add_epi16(A, _mm_mullo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128((int)t), z), w))); \
        }
    for (r = 0; r < nr; r++) {
        const uint32_t *s = mr[r] + kmin;
        const __m128i w = wv[r];
        uint16_t *d = V + 4 * lo;
        if (!r) { DOWN_MIP(bias, bias, bias) }
        else { DOWN_MIP(_mm_loadu_si128((const __m128i *)d), _mm_loadu_si128((const __m128i *)(d + 8)),
                        _mm_loadl_epi64((const __m128i *)d)) }
    }
#undef DOWN_MIP
    for (i = hi > lo ? hi : lo; i < cnt + pad; i++) _mm_storel_epi64((__m128i *)(V + 4 * i), bias);
}

/* (two pixels' 24-bit colours from their sums' B, G, R >> 16, saturated at 255 -- what mix() makes at full coverage) */
#define FAST_INLINE static inline __attribute__((always_inline))
FAST_INLINE __m128i pack2(__m128i s0, __m128i s1) {
    const __m128i r = _mm_packs_epi32(_mm_srli_epi32(s0, 16), _mm_srli_epi32(s1, 16));
    return _mm_and_si128(_mm_packus_epi16(r, _mm_setzero_si128()), _mm_set_epi32(0, 0, 0xffffff, 0xffffff));
}
/* mix() of two pixels' sums (lanes B, G, R with the rounding, A) over the two pixels in o's low half */
FAST_INLINE __m128i mix_core(__m128i o, __m128i s0, __m128i s1, int mip) {
    const __m128i z = _mm_setzero_si128(), o16 = _mm_unpacklo_epi8(o, z), o4 = _mm_slli_epi16(o16, 2);
    __m128i a = _mm_srli_epi64(_mm_unpackhi_epi64(s0, s1), 32), keep, ia, w;   /* (A0, 0, A1, 0) */
    keep = _mm_and_si128(_mm_shuffle_epi32(_mm_cmpeq_epi32(a, z), 0x08),
                         _mm_set_epi32(0, 0, (int)0xff000000u, (int)0xff000000u));
    if (mip) a = _mm_srli_epi64(_mm_mul_epu32(a, _mm_set1_epi32((int)0x80808081u)), 39);   /* (A / 255) */
    ia = _mm_sub_epi32(_mm_set1_epi32(65536), a);
    w = _mm_or_si128(_mm_srli_epi32(ia, 2), _mm_slli_epi32(_mm_and_si128(ia, _mm_set1_epi32(3)), 16));
    s0 = _mm_add_epi32(s0, _mm_madd_epi16(_mm_unpacklo_epi16(o4, o16), _mm_shuffle_epi32(w, 0x00)));
    s1 = _mm_add_epi32(s1, _mm_madd_epi16(_mm_unpackhi_epi16(o4, o16), _mm_shuffle_epi32(w, 0xaa)));
    return _mm_or_si128(pack2(s0, s1), _mm_and_si128(o, keep));
}
/* (two pixels both fully covered -- most of an opaque bitmap, the inside of the land -- are their sums; both not
 * covered at all -- the land's holes -- stay as they are) */
FAST_INLINE void mix2(uint32_t *dst, __m128i s0, __m128i s1, int mip) {
    const __m128i a = _mm_unpackhi_epi32(s0, s1);   /* (lanes 2, 3: A0, A1) */
    const int f = _mm_movemask_epi8(_mm_cmpeq_epi32(a, _mm_set1_epi32(mip ? 255 * 65536 : 65536))) >> 8;
    const int e = _mm_movemask_epi8(_mm_cmpeq_epi32(a, _mm_setzero_si128())) >> 8;
    if (f == 0xff) _mm_storel_epi64((__m128i *)dst, pack2(s0, s1));
    else if (e != 0xff) _mm_storel_epi64((__m128i *)dst, mix_core(_mm_loadl_epi64((const __m128i *)dst), s0, s1, mip));
}
FAST_INLINE void mix1(uint32_t *dst, __m128i s, int mip) {
    *dst = (uint32_t)_mm_cvtsi128_si32(mix_core(_mm_cvtsi32_si128((int)*dst), s, s, mip));
}
/* a pixel pair / a pixel of a row: mode 0 row_plain, 1 row_mip, 2 row_plain with every pixel fully covered (an
 * opaque bitmap: their sums, no tests) */
FAST_INLINE void put2(uint32_t *dst, __m128i s0, __m128i s1, int mode) {
    if (mode == 2) _mm_storel_epi64((__m128i *)dst, pack2(s0, s1));
    else mix2(dst, s0, s1, mode);
}
FAST_INLINE void put1(uint32_t *dst, __m128i s, int mode) {
    if (mode == 2) *dst = (uint32_t)_mm_cvtsi128_si32(pack2(s, s));
    else mix1(dst, s, mode);
}

/* a pixel's sums from V (texel kmin first) by its plan (column j of the part) */
FAST_INLINE __m128i sum_plan(const uint16_t *V, const fplan *pl, int j, int P) {
    const uint16_t *vp = V + pl->off[j];
    __m128i s = _mm_load_si128((const __m128i *)pl->c[j]);
    int q;
    for (q = 0; q < P; q++) {
        __m128i d = _mm_loadu_si128((const __m128i *)(vp + 8 * q));
        d = _mm_unpacklo_epi16(d, _mm_shuffle_epi32(d, 0x4e));
        s = _mm_add_epi32(s, _mm_madd_epi16(d, _mm_load_si128((const __m128i *)pl->w[j][q])));
    }
    return s;
}
/* ... and straight from its taps (columns of more than 2 * FAST_PLAN taps) */
static const uint32_t wmask[19] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0xffffu, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
FAST_INLINE __m128i sum_taps(const uint16_t *V, int kmin, const rtap *tx, int P) {
    const uint16_t *vp;
    __m128i s;
    int q;
    if (!tx->n) return _mm_set_epi32(0, 32768, 32768, 32768);
    vp = V + 4 * (tx->k0 - kmin);
    s = _mm_add_epi32(_mm_shuffle_epi32(_mm_cvtsi32_si128(tx->sum << 15), 0), _mm_set_epi32(0, 32768, 32768, 32768));
    for (q = 0; q < P; q++) {
        uint32_t wp;
        __m128i d = _mm_loadu_si128((const __m128i *)(vp + 8 * q));
        memcpy(&wp, tx->w + 2 * q, 4);
        wp &= wmask[tx->n - 2 * q + 8];
        d = _mm_unpacklo_epi16(d, _mm_shuffle_epi32(d, 0x4e));
        s = _mm_add_epi32(s, _mm_madd_epi16(d, _mm_shuffle_epi32(_mm_cvtsi32_si128((int)wp), 0)));
    }
    return s;
}

/* a part's plan: its columns [xa, xe) */
static void make_plan(const rview *v, const rprep *p, int xa, int xe, int kmin, int P, fplan *pl) {
    int X, q;
    for (X = xa; X < xe; X++) {
        const rtap *t = TAPX(X);
        const int j = X - xa, n = t->n;
        const uint32_t c = (uint32_t)t->sum << 15;
        pl->off[j] = n ? 4 * (t->k0 - kmin) : 0;
        pl->c[j][0] = pl->c[j][1] = pl->c[j][2] = c + 32768u;
        pl->c[j][3] = c;
        for (q = 0; q < P; q++) {
            const uint32_t a = 2 * q < n ? (uint32_t)t->w[2 * q] : 0u;
            const uint32_t b = 2 * q + 1 < n ? (uint32_t)t->w[2 * q + 1] : 0u;
            pl->w[j][q][0] = pl->w[j][q][1] = pl->w[j][q][2] = pl->w[j][q][3] = a | b << 16;
        }
    }
}

/* a part's output columns [xa, xe) of a row */
FAST_INLINE void across_plan(const uint16_t *V, const fplan *pl, uint32_t *row, int xa, int xe, int P, int mode) {
    int j = 0, n = xe - xa;
    for (; j + 1 < n; j += 2) put2(row + xa + j, sum_plan(V, pl, j, P), sum_plan(V, pl, j + 1, P), mode);
    if (j < n) put1(row + xa + j, sum_plan(V, pl, j, P), mode);
}
FAST_INLINE void across_taps(const uint16_t *V, int kmin, const rview *v, const rprep *p, uint32_t *row, int xa, int xe,
                             int P, int mode) {
    int X = xa;
    for (; X + 1 < xe; X += 2) put2(row + X, sum_taps(V, kmin, TAPX(X), P), sum_taps(V, kmin, TAPX(X + 1), P), mode);
    if (X < xe) put1(row + X, sum_taps(V, kmin, TAPX(X), P), mode);
}
/* (each mode and number of pairs its own loop) */
#define ACROSS(MODE)                                                                                              \
    do {                                                                                                         \
        if (P == 1) across_plan(V, pl, row, xa, xe, 1, MODE);                                                    \
        else if (P == 2) across_plan(V, pl, row, xa, xe, 2, MODE);                                               \
        else if (P <= FAST_PLAN) across_plan(V, pl, row, xa, xe, FAST_PLAN, MODE);                               \
        else across_taps(V, kmin, v, p, row, xa, xe, P, MODE);                                                   \
    } while (0)
static void across(const uint16_t *V, int kmin, const fplan *pl, const rview *v, const rprep *p, uint32_t *row, int xa,
                   int xe, int P, int mode) {
    if (mode == 1) ACROSS(1);
    else if (mode == 2) ACROSS(2);
    else ACROSS(0);
}
#undef ACROSS

/* rows [y0, y1) of a row_plain or row_mip operation (0: not done, the scalar rows draw it) */
static int fast_draw(const rop *op, const rview *v, const rprep *p, int y0, int y1) {
    uint16_t V[(FAST_SPAN + RASTER_MAX_TAPS) * 4];      /* (2 * pairs <= RASTER_MAX_TAPS texels read past a part) */
    uint16_t pp[256 * 4];
    uint8_t tmp[RASTER_MAX_TAPS][FAST_SPAN];
    fplan pl;
    fpart part[FAST_PARTS];
    const int mip = p->level != 0, xb = p->xbase, xd = p->xdir, xn = p->xn;
    const int all = !mip && (op->kind == ROP_BITMAP ? !op->keyed : op->mode == 0);   /* (no see-through colour) */
    const rland *L = v->land;
    const uint32_t *M = mip ? L->mip[p->level] : NULL;
    const int mw = mip ? L->mw[p->level] : 0, mh = mip ? L->mh[p->level] : 0;
    int np = 0, X, Y, k, c, kmin = 0, kmax = 0;
    if (xd != 1 && xd != -1) return 0;
    for (X = p->x1; X < p->x2; X++) {       /* the parts, and the taps checked */
        const rtap *t = TAPX(X);
        if (!tap_ok(t)) return 0;
        if (!t->n) continue;                /* (columns with no taps: never drawn) */
        if (np && X - part[np - 1].xa < FAST_COLS &&
            (t->k0 + t->n > kmax ? t->k0 + t->n : kmax) - (t->k0 < kmin ? t->k0 : kmin) <= FAST_SPAN) {
            if (t->k0 < kmin) kmin = t->k0;
            if (t->k0 + t->n > kmax) kmax = t->k0 + t->n;
            if ((t->n + 1) >> 1 > part[np - 1].pairs) part[np - 1].pairs = (t->n + 1) >> 1;
            part[np - 1].xe = X + 1;
            continue;
        }
        if (np) { part[np - 1].kmin = kmin; part[np - 1].cnt = kmax - kmin; }
        if (np == FAST_PARTS) return 0;
        part[np].pairs = (t->n + 1) >> 1;
        part[np].xa = X;
        part[np++].xe = X + 1;
        kmin = t->k0; kmax = t->k0 + t->n;
    }
    if (!np) return 1;                      /* (no column covers any texel: nothing drawn) */
    part[np - 1].kmin = kmin; part[np - 1].cnt = kmax - kmin;
    for (k = 0; k < np; k++) {
        const int a = part[k].kmin, b = part[k].kmin + part[k].cnt;
        part[k].inside = mip ? a >= 0 && b <= mw
                       : xd > 0 ? xb + a >= 0 && xb + b <= xn : xb - b + 1 >= 0 && xb - a < xn;
        part[k].full = all;
        for (X = part[k].xa; X < part[k].xe && part[k].full; X++) part[k].full = TAPX(X)->sum == 256;
    }
    if (!mip) {                             /* the palette with coverage: 1, the see-through colour (or 0) 0 */
        const __m128i m = _mm_set1_epi32(0xffffff), one = _mm_set1_epi32(0x1000000), z = _mm_setzero_si128();
        for (k = 0; k < 256; k += 4) {
            const __m128i c = _mm_or_si128(_mm_and_si128(_mm_loadu_si128((const __m128i *)(v->pal + k)), m), one);
            _mm_storeu_si128((__m128i *)(pp + 4 * k), _mm_unpacklo_epi8(c, z));
            _mm_storeu_si128((__m128i *)(pp + 4 * k + 8), _mm_unpackhi_epi8(c, z));
        }
        if (!all) _mm_storel_epi64((__m128i *)(pp + 4 * (op->kind == ROP_BITMAP ? op->key : 0)), z);
    }
    for (c = 0; c < np; c++) {
        const int kmn = part[c].kmin, cnt = part[c].cnt, xa = part[c].xa, xe = part[c].xe, P = part[c].pairs;
        if (P <= FAST_PLAN) make_plan(v, p, xa, xe, kmn, P, &pl);
        for (Y = y0; Y < y1; Y++) {
            const rtap *ty = TAPY(Y);
            uint32_t *row = v->out + (size_t)Y * v->stride;
            __m128i wv[RASTER_MAX_TAPS];
            const uint8_t *rp[RASTER_MAX_TAPS], *sp[RASTER_MAX_TAPS];
            const uint32_t *mr[RASTER_MAX_TAPS];
            int ny = 0, ky;
            if (!ty->n) continue;
            if (!tap_ok(ty) || (!mip && copy1_ok(v, p, ty))) {   /* (all of the row, with the first part) */
                if (c) continue;
                if (mip) row_mip(op, v, p, ty, row);
                else if (copy1_ok(v, p, ty)) row_copy1(op, v, p, ty, row);
                else row_plain(op, v, p, ty, row);
                continue;
            }
            if (mip) {
                int lo = 0, hi = cnt;
                for (ky = 0; ky < ty->n; ky++) {
                    const int r = ty->k0 + ky;
                    if ((unsigned)r >= (unsigned)mh) continue;
                    mr[ny] = M + (size_t)r * mw;
                    wv[ny++] = _mm_set1_epi16(ty->w[ky]);
                }
                if (!ny) continue;          /* (all 0: nothing drawn) */
                if (!part[c].inside) {
                    if (kmn < 0) lo = -kmn < cnt ? -kmn : cnt;
                    if (kmn + cnt > mw) hi = mw - kmn > lo ? mw - kmn : lo;
                }
                down_mip(mr, wv, ny, kmn, lo, hi, cnt, 2 * P, V);
            } else {
                tap_rows(op, p, ty, rp);
                ny = ty->n;
                for (ky = 0; ky < ny; ky++) {
                    const uint8_t *r = rp[ky];
                    int i;
                    wv[ky] = _mm_set1_epi16(ty->w[ky]);
                    if (!r) { sp[ky] = zero_row; continue; }
                    if (part[c].inside && xd > 0) { sp[ky] = r + xb + kmn; continue; }
                    for (i = 0; i < cnt; i++) {
                        const int col = xb + xd * (kmn + i);
                        tmp[ky][i] = (unsigned)col < (unsigned)xn ? r[col] : 0;
                    }
                    sp[ky] = tmp[ky];
                }
                down_plain(pp, sp, wv, ny, cnt, 2 * P, V);
            }
            /* (an opaque bitmap's rows all of whose pixels are fully covered: no tests) */
            across(V, kmn, &pl, v, p, row, xa, xe, P, mip ? 1 : part[c].full && ty->sum == 256 ? 2 : 0);
        }
    }
    return 1;
}

void raster_draw(const rop *op, const rview *v, const rprep *p, int y0, int y1) {
    int Y, general;
    if (y0 < p->y1) y0 = p->y1;
    if (y1 > p->y2) y1 = p->y2;
    general = op->kind == ROP_GRID ? op->mode >= 2 || (op->mode == 1 && op->lut)
            : op->kind == ROP_BITMAP && op->land_x >= 0 && v->land && v->land->full;
    /* (the fast rows' setup costs ~130 ns a call: an operation under 64 output pixels is quicker on the plain rows) */
    if (raster_fast && y0 < y1 && op->kind != ROP_FILL && (p->level || !general) && (long)(p->x2 - p->x1) * (y1 - y0) >= 64 &&
        fast_draw(op, v, p, y0, y1)) return;
    for (Y = y0; Y < y1; Y++) {
        const rtap *ty = TAPY(Y);
        uint32_t *row = v->out + (size_t)Y * v->stride;
        if (!ty->n) continue;
        if (op->kind == ROP_FILL) row_fill(op, v, p, ty, row);
        else if (p->level) row_mip(op, v, p, ty, row);
        else if (general) row_general(op, v, p, ty, row);
        else if (copy1_ok(v, p, ty)) row_copy1(op, v, p, ty, row);
        else row_plain(op, v, p, ty, row);
    }
}

/* ---- the land's shrunk copies ------------------------------------------------------------------------------------ */
static inline uint32_t land_px(const rland *L, const uint32_t *pal, int x, int y) {
    const size_t i = (size_t)y * L->w + x;
    const uint8_t s = L->now[i];
    if (L->keyed && s == L->key) return 0;
    if (L->full && L->full[i] && L->snap[i] == s) return 0xff000000u | (L->full[i] & 0xffffffu);
    return 0xff000000u | (pal[s] & 0xffffffu);
}

/* 4 texels (or pixels) averaged: premultiplied channels and coverage */
static inline uint32_t avg4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    uint32_t A = (a >> 24) + (b >> 24) + (c >> 24) + (d >> 24);
    uint32_t R = ((a >> 16) & 255u) + ((b >> 16) & 255u) + ((c >> 16) & 255u) + ((d >> 16) & 255u);
    uint32_t G = ((a >> 8) & 255u) + ((b >> 8) & 255u) + ((c >> 8) & 255u) + ((d >> 8) & 255u);
    uint32_t B = (a & 255u) + (b & 255u) + (c & 255u) + (d & 255u);
    A = (A + 2) >> 2; R = (R + 2) >> 2; G = (G + 2) >> 2; B = (B + 2) >> 2;
    if (R > A) R = A;
    if (G > A) G = A;
    if (B > A) B = A;
    return A << 24 | R << 16 | G << 8 | B;
}

/* the copies of land [x0, x1) x [y0, y1) (x0, y0 multiples of 2^levels) made again from `now` */
static void land_mips(rland *L, const uint32_t *pal, int x0, int y0, int x1, int y1) {
    int lev;
    for (lev = 1; lev <= L->levels; lev++) {
        uint32_t *M = L->mip[lev];
        const int mw = L->mw[lev], mh = L->mh[lev], u0 = x0 >> lev, v0 = y0 >> lev;
        const int u1 = (x1 + (1 << lev) - 1) >> lev, v1 = (y1 + (1 << lev) - 1) >> lev;
        int u, w;
        for (w = v0; w < v1 && w < mh; w++) {
            for (u = u0; u < u1 && u < mw; u++) {
                uint32_t q[4];
                int k;
                for (k = 0; k < 4; k++) {
                    const int cu = 2 * u + (k & 1), cv = 2 * w + (k >> 1);
                    if (lev == 1) q[k] = cu < L->w && cv < L->h ? land_px(L, pal, cu, cv) : 0;
                    else q[k] = cu < L->mw[lev - 1] && cv < L->mh[lev - 1] ? L->mip[lev - 1][(size_t)cv * L->mw[lev - 1] + cu] : 0;
                }
                M[(size_t)w * mw + u] = avg4(q[0], q[1], q[2], q[3]);
            }
        }
    }
}

/* A tile's copies are claimed before they are made (stamp: RASTER_BUSY, by compare-and-swap), so the background
 * builder and the drawing threads never make the same tile's at once; whoever finds a tile claimed leaves it (the
 * drawing draws it from the tile itself that frame). The stamp is set to the generation after the copies are written. */
static int land_claim(rland *L, int ti, uint32_t gen) {
    const uint32_t st = *(volatile uint32_t *)&L->stamp[ti];
    if (st == gen || st == RASTER_BUSY) return 0;
    return __sync_val_compare_and_swap(&L->stamp[ti], st, RASTER_BUSY) == st;
}
static void land_done(rland *L, int ti, uint32_t gen) {
    __sync_synchronize();
    *(volatile uint32_t *)&L->stamp[ti] = gen;
}

int raster_land_build(rland *L, const uint32_t *pal, uint32_t gen, int ti, const uint32_t *live) {
    int x0, y0, x1, y1;
    uint32_t st;
    if (!L->levels || !L->now || ti < 0 || ti >= L->cols * L->rows) return 0;
    if (live && *(const volatile uint32_t *)live != gen) return 0;
    st = *(volatile uint32_t *)&L->stamp[ti];
    if (st == gen || st == RASTER_BUSY || __sync_val_compare_and_swap(&L->stamp[ti], st, RASTER_BUSY) != st) return 0;
    if (live && *(const volatile uint32_t *)live != gen) {  /* (the palette changed as it was claimed: left as it was -- it
                                                               may already be made with the new one) */
        __sync_synchronize();
        *(volatile uint32_t *)&L->stamp[ti] = st;
        return 0;
    }
    x0 = (ti % L->cols) * L->tw; y0 = (ti / L->cols) * L->th;
    x1 = x0 + L->tw < L->w ? x0 + L->tw : L->w; y1 = y0 + L->th < L->h ? y0 + L->th : L->h;
    land_mips(L, pal, x0, y0, x1, y1);
    land_done(L, ti, gen);
    return 1;
}

int raster_land_sync(rland *L, const rsrc *tile, int lx, int ly, const uint32_t *pal, uint32_t gen, int phase, long *budget) {
    const int A = 1 << L->levels;
    int w = tile->w, h = tile->h, ti, y, s0;
    uint32_t st;
    if (!L->levels || !L->now || lx < 0 || ly < 0 || lx >= L->w || ly >= L->h) return 1;
    if (w > L->w - lx) w = L->w - lx;
    if (h > L->h - ly) h = L->h - ly;
    if (w <= 0 || h <= 0) return 1;
    ti = (ly / L->th) * L->cols + lx / L->tw;
    if (ti < 0 || ti >= L->cols * L->rows) return 1;
    st = *(volatile uint32_t *)&L->stamp[ti];
    if (st != gen) {                        /* new, or the palette changed: all of it -- unless another thread is at it, or
                                               this frame's budget of tiles made from scratch is spent */
        if (st == RASTER_BUSY || (budget && __sync_sub_and_fetch(budget, 1) < 0) || !land_claim(L, ti, gen)) return 0;
        for (y = 0; y < h; y++) memcpy(L->now + (size_t)(ly + y) * L->w + lx, tile->data + (size_t)y * tile->stride, (size_t)w);
        land_mips(L, pal, lx, ly, lx + w, ly + h);
        land_done(L, ti, gen);
        return 1;
    }
    for (s0 = 0; s0 < h; s0 += A) {         /* strips of 2^levels rows: only the changed ones made again */
        const int s1 = s0 + A < h ? s0 + A : h;
        int changed = 0;
        for (y = s0 + (((phase - (ly + s0)) % 4) + 4) % 4; y < s1 && !changed; y += 4)
            changed = memcmp(L->now + (size_t)(ly + y) * L->w + lx, tile->data + (size_t)y * tile->stride, (size_t)w) != 0;
        if (!changed) continue;
        for (y = s0; y < s1; y++) memcpy(L->now + (size_t)(ly + y) * L->w + lx, tile->data + (size_t)y * tile->stride, (size_t)w);
        land_mips(L, pal, lx, ly + s0, lx + w, ly + s1);
    }
    return 1;
}
