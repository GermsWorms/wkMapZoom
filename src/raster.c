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
    double offx = 0.0, offy = 0.0;
    int level = 0, S;
    if (op->r <= op->l || op->b <= op->t || !v->gx[0] || !v->gy[0]) return 0;
    p->x1 = (int)floor((op->l - v->bx) * z + v->ox);
    p->x2 = (int)ceil((op->r - v->bx) * z + v->ox);
    p->y1 = (int)floor((op->t - v->by) * z + v->oy);
    p->y2 = (int)ceil((op->b - v->by) * z + v->oy);
    if (p->x1 < v->x1) p->x1 = v->x1;
    if (p->y1 < v->y1) p->y1 = v->y1;
    if (p->x2 > v->x2) p->x2 = v->x2;
    if (p->y2 > v->y2) p->y2 = v->y2;
    if (p->x1 >= p->x2 || p->y1 >= p->y2) return 0;
    if (op->kind == ROP_BITMAP && op->land_x >= 0 && v->land && !v->point) {
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

void raster_draw(const rop *op, const rview *v, const rprep *p, int y0, int y1) {
    int Y, general;
    if (y0 < p->y1) y0 = p->y1;
    if (y1 > p->y2) y1 = p->y2;
    general = op->kind == ROP_GRID ? op->mode >= 2 || (op->mode == 1 && op->lut)
            : op->kind == ROP_BITMAP && op->land_x >= 0 && v->land && v->land->full;
    for (Y = y0; Y < y1; Y++) {
        const rtap *ty = TAPY(Y);
        uint32_t *row = v->out + (size_t)Y * v->stride;
        if (!ty->n) continue;
        if (op->kind == ROP_FILL) row_fill(op, v, p, ty, row);
        else if (p->level) row_mip(op, v, p, ty, row);
        else if (general) row_general(op, v, p, ty, row);
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

void raster_land_sync(rland *L, const rsrc *tile, int lx, int ly, const uint32_t *pal, uint32_t gen, int phase) {
    const int A = 1 << L->levels;
    int w = tile->w, h = tile->h, ti, y, s0;
    if (!L->levels || !L->now || lx < 0 || ly < 0 || lx >= L->w || ly >= L->h) return;
    if (w > L->w - lx) w = L->w - lx;
    if (h > L->h - ly) h = L->h - ly;
    if (w <= 0 || h <= 0) return;
    ti = (ly / L->th) * L->cols + lx / L->tw;
    if (ti < 0 || ti >= L->cols * L->rows) return;
    if (L->stamp[ti] != gen) {              /* new, or the palette changed: all of it */
        for (y = 0; y < h; y++) memcpy(L->now + (size_t)(ly + y) * L->w + lx, tile->data + (size_t)y * tile->stride, (size_t)w);
        land_mips(L, pal, lx, ly, lx + w, ly + h);
        L->stamp[ti] = gen;
        return;
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
}
