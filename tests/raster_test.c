/* raster_test.c: the zoomed drawing (src/raster.c) against a plain reference -- each operation drawn at WA's own scale
 * into a region of palette indices, then every output pixel the exact average of the region pixels it covers. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "../src/raster.h"

static uint32_t pal[256];
static int failures;
#define RW 200
#define RH 160
#define OW 160
#define OH 120
static uint8_t region[RH][RW];
static uint32_t out[OH * OW];

static int ref_bg;
static unsigned rng = 12345;
static unsigned rnd(void) { rng = rng * 1103515245u + 12345u; return (rng >> 16) & 0x7fff; }

static uint8_t nearest(uint32_t c) {
    int k, best = 0;
    long bd = 0x7fffffff;
    for (k = 0; k < 256; k++) {
        long dr = (long)((pal[k] >> 16) & 255) - (long)((c >> 16) & 255), dg = (long)((pal[k] >> 8) & 255) - (long)((c >> 8) & 255);
        long db = (long)(pal[k] & 255) - (long)(c & 255), d = dr * dr + dg * dg + db * db;
        if (d < bd) { bd = d; best = k; }
    }
    return (uint8_t)best;
}

/* the reference: the op's rule per region pixel (as wkVista's `shade`) */
static void ref_draw(const rop *op) {
    int X, Y;
    for (Y = op->t; Y < op->b; Y++)
        for (X = op->l; X < op->r; X++) {
            int s, c, r;
            if (op->kind == ROP_FILL) { region[Y][X] = op->index; continue; }
            c = op->col0 + op->dir * X; r = op->row0 + Y;
            s = c >= 0 && r >= 0 && c < op->src.w && r < op->src.h ? op->src.data[r * op->src.stride + c] : 0;
            if (op->kind == ROP_BITMAP) { if (!op->keyed || s != op->key) region[Y][X] = (uint8_t)s; continue; }
            if (op->mode == 0 || s != 0) region[Y][X] = (uint8_t)s;
        }
}

/* the reference output: exact box average of the region */
static uint32_t ref_px(const rview *v, int X, int Y) {
    double s0 = v->bx + (X - v->ox) / v->z, s1 = s0 + 1.0 / v->z, t0 = v->by + (Y - v->oy) / v->z, t1 = t0 + 1.0 / v->z;
    double r = 0, g = 0, b = 0, wsum = 0;
    int x, y;
    for (y = (int)floor(t0); y < (int)ceil(t1); y++)
        for (x = (int)floor(s0); x < (int)ceil(s1); x++) {
            double wx = fmin(s1, x + 1) - fmax(s0, x), wy = fmin(t1, y + 1) - fmax(t0, y), w;
            uint32_t c;
            if (wx <= 0 || wy <= 0) continue;
            w = wx * wy;
            c = x >= 0 && y >= 0 && x < RW && y < RH ? pal[region[y][x]] : pal[ref_bg];
            r += ((c >> 16) & 255) * w; g += ((c >> 8) & 255) * w; b += (c & 255) * w; wsum += w;
        }
    if (wsum <= 0) return 0;
    return (uint32_t)floor(r / wsum + 0.5) << 16 | (uint32_t)floor(g / wsum + 0.5) << 8 | (uint32_t)floor(b / wsum + 0.5);
}

static rtap gxs[RASTER_LEVELS + 1][OW], gys[RASTER_LEVELS + 1][OH];

/* the view's taps for the frame: level 0, and land copy `level` for a land offset (offx, offy) */
static void frame_taps(rview *v, int level, double offx, double offy) {
    raster_axis(gxs[0], v->x1, v->x2, v->ox, v->bx, v->z, 0.0, 0, v->point);
    raster_axis(gys[0], v->y1, v->y2, v->oy, v->by, v->z, 0.0, 0, v->point);
    v->gx[0] = gxs[0]; v->gy[0] = gys[0];
    if (level) {
        raster_axis(gxs[level], v->x1, v->x2, v->ox, v->bx, v->z, offx, level, 0);
        raster_axis(gys[level], v->y1, v->y2, v->oy, v->by, v->z, offy, level, 0);
        v->gx[level] = gxs[level]; v->gy[level] = gys[level]; v->gox[level] = offx; v->goy[level] = offy;
    }
}

static void draw(const rop *op, const rview *v) {
    rprep p;
    if (raster_prepare(op, v, &p)) raster_draw(op, v, &p, 0, OH);
}

static int cdiff(uint32_t a, uint32_t b) {
    int k, m = 0;
    for (k = 0; k < 24; k += 8) {
        int d = abs((int)((a >> k) & 255) - (int)((b >> k) & 255));
        if (d > m) m = d;
    }
    return m;
}

/* an output pixel on an edge the drawing snaps to whole pixels (an operation at least a pixel across along that axis):
 * there it is all the operation's or none of it, by design, where the reference shares it */
static int snapped_edge(const rop *op, const rview *v, int X, int Y) {
    const double fx1 = (op->l - v->bx) * v->z + v->ox, fx2 = (op->r - v->bx) * v->z + v->ox;
    const double fy1 = (op->t - v->by) * v->z + v->oy, fy2 = (op->b - v->by) * v->z + v->oy;
    const int inx = X >= (int)floor(fx1) && X < (int)ceil(fx2), iny = Y >= (int)floor(fy1) && Y < (int)ceil(fy2);
    const int ex = (X >= (int)floor(fx1) && X < (int)ceil(fx1)) || (X >= (int)floor(fx2) && X < (int)ceil(fx2));
    const int ey = (Y >= (int)floor(fy1) && Y < (int)ceil(fy1)) || (Y >= (int)floor(fy2) && Y < (int)ceil(fy2));
    return (fx2 - fx1 >= 1.0 && ex && iny) || (fy2 - fy1 >= 1.0 && ey && inx);
}

/* one op over a background of index bg: the output against the reference (snapped edges aside); max and mean error.
 * And the fast paths (raster_fast) against the plain ones: bit for bit the same. */
extern int raster_fast;
static uint32_t out_plain[OH * OW];
static void check(const char *name, const rop *op, rview *v, uint8_t bg, int max_ok, double mean_ok) {
    int X, Y, worst = 0, wx = 0, wy = 0;
    double sum = 0;
    ref_bg = bg;
    memset(region, bg, sizeof(region));
    for (X = 0; X < OW * OH; X++) out[X] = pal[bg];
    raster_fast = 0;
    draw(op, v);
    raster_fast = 1;
    memcpy(out_plain, out, sizeof(out));
    for (X = 0; X < OW * OH; X++) out[X] = pal[bg];
    ref_draw(op);
    draw(op, v);
    for (X = 0; X < OW * OH; X++)
        if (out[X] != out_plain[X]) {
            printf("FAIL %s z=%.4f: the fast path differs at %d,%d (%06x, plain %06x)\n", name, v->z, X % OW, X / OW, out[X], out_plain[X]);
            failures++;
            break;
        }
    for (Y = v->y1; Y < v->y2; Y++)
        for (X = v->x1; X < v->x2; X++) {
            int d;
            if (snapped_edge(op, v, X, Y)) continue;
            d = cdiff(out[Y * OW + X], ref_px(v, X, Y));
            sum += d;
            if (d > worst) { worst = d; wx = X; wy = Y; }
        }
    sum /= (double)(v->x2 - v->x1) * (v->y2 - v->y1);
    if (worst > max_ok || sum > mean_ok) {
        printf("FAIL %s z=%.4f: worst %d at %d,%d (got %06x, want %06x), mean %.3f\n", name, v->z, worst, wx, wy,
               out[wy * OW + wx], ref_px(v, wx, wy), sum);
        failures++;
    }
}

static void view(rview *v, double z, double bx, double by) {
    memset(v, 0, sizeof(*v));
    v->z = z; v->bx = bx; v->by = by; v->ox = OW / 2; v->oy = OH / 2;
    v->x1 = 0; v->y1 = 0; v->x2 = OW; v->y2 = OH;
    v->out = out; v->stride = OW; v->pal = pal; v->nearest = nearest; v->gen = 1;
    frame_taps(v, 0, 0.0, 0.0);
}

/* ---- the fast paths against the plain ones, bit for bit, over random cases --------------------------------------- */
static uint32_t bgbuf[OH * OW];
static int fuzz_cases, fuzz_fail;
static unsigned rnd32(void) { return (unsigned)rnd() << 17 ^ (unsigned)rnd() << 2 ^ (unsigned)rnd() >> 13; }
static double rndu(void) { return (rnd() + 0.5) / 32768.0; }            /* (0, 1) */

/* ops[0..n) drawn with raster_fast 0 and 1 over the same background of random 32-bit pixels (4th bytes too), each in
 * the same random bands of rows (as the threads share them out): the outputs must be the same, every byte */
static void fast_vs_plain(const char *name, const rop *ops, int n, const rview *v) {
    int cut[4], X, pass, i, b;
    uint32_t *res[2] = {out_plain, out};
    for (X = 0; X < OW * OH; X++) bgbuf[X] = rnd32();
    cut[0] = 0; cut[3] = OH;
    cut[1] = (int)(rnd() % (OH + 1)); cut[2] = (int)(rnd() % (OH + 1));
    if (cut[1] > cut[2]) { int t = cut[1]; cut[1] = cut[2]; cut[2] = t; }
    for (pass = 0; pass < 2; pass++) {
        memcpy(out, bgbuf, sizeof(out));
        raster_fast = pass;
        for (i = 0; i < n; i++) {
            rprep p;
            if (!raster_prepare(&ops[i], v, &p)) continue;
            for (b = 0; b < 3; b++) raster_draw(&ops[i], v, &p, cut[b], cut[b + 1]);
        }
        if (!pass) memcpy(out_plain, out, sizeof(out));
    }
    raster_fast = 1;
    fuzz_cases++;
    for (X = 0; X < OW * OH; X++)
        if (res[0][X] != res[1][X]) {
            if (fuzz_fail < 12)
                printf("FAIL fast vs plain (%s) z=%.4f b=%.3f,%.3f point %d: differs at %d,%d (%08x, plain %08x)\n", name, v->z,
                       v->bx, v->by, v->point, X % OW, X / OW, res[1][X], res[0][X]);
            fuzz_fail++;
            failures++;
            break;
        }
}

/* a random view: zoom 0.12 .. 8, a fractional camera, sometimes only part of the output, sometimes the point filter */
static void random_view(rview *v, double zlo, double zhi) {
    const double z = exp(log(zlo) + (log(zhi) - log(zlo)) * rndu());
    view(v, z, 60 + 80 * rndu(), 50 + 60 * rndu());
    if (rnd() % 4 == 0) {
        v->x1 = (int)(rnd() % (OW / 3)); v->x2 = OW - (int)(rnd() % (OW / 3));
        v->y1 = (int)(rnd() % (OH / 3)); v->y2 = OH - (int)(rnd() % (OH / 3));
    }
    if (rnd() % 3 == 0) { v->ox = (int)(rnd() % OW); v->oy = (int)(rnd() % OH); }
    v->point = rnd() % 5 == 0;
    frame_taps(v, 0, 0.0, 0.0);
}

/* sources for the random bitmaps and sprites */
#define NSRC 6
static uint8_t srcpx[NSRC][96 * 80];
static const int srcw[NSRC] = {48, 1, 64, 7, 96, 13}, srch[NSRC] = {40, 1, 65, 80, 9, 13};

static void fuzz_ops(int count) {
    int k;
    for (k = 0; k < count; k++) {
        rview v;
        rop op;
        const int si = (int)(rnd() % NSRC), sw = srcw[si], sh = srch[si];
        double s0, s1, t0, t1;
        int w, h, cx, cy;
        random_view(&v, 0.12, 8.0);
        s0 = v.bx + (v.x1 - v.ox) / v.z; s1 = v.bx + (v.x2 - v.ox) / v.z;     /* (the region the view shows) */
        t0 = v.by + (v.y1 - v.oy) / v.z; t1 = v.by + (v.y2 - v.oy) / v.z;
        memset(&op, 0, sizeof(op));
        op.land_x = -1;
        op.src.data = srcpx[si]; op.src.w = sw; op.src.h = sh; op.src.stride = 96;
        switch (rnd() % 4) {                /* (its size: the source's, a pixel, or anything up to past the view) */
        case 0: w = sw; h = sh; break;
        case 1: w = 1 + (int)(rnd() % 2); h = 1 + (int)(rnd() % (2 * sh + 1)); break;
        case 2: w = 1 + (int)(rnd() % (2 * sw + 1)); h = 1 + (int)(rnd() % 2); break;
        default: w = 1 + (int)(rnd() % (int)(1.6 * (s1 - s0) + 2)); h = 1 + (int)(rnd() % (int)(1.6 * (t1 - t0) + 2)); break;
        }
        cx = (int)floor(s0 + (s1 - s0) * (rndu() * 1.4 - 0.2)); cy = (int)floor(t0 + (t1 - t0) * (rndu() * 1.4 - 0.2));
        op.l = cx - w / 2; op.r = op.l + w; op.t = cy - h / 2; op.b = op.t + h;
        op.dir = rnd() % 3 == 0 ? -1 : 1;
        /* (source column 0 at the op's left, or right if mirrored, give or take: sometimes it reaches past the source) */
        op.col0 = (op.dir > 0 ? -op.l : op.r - 1) + (rnd() % 3 == 0 ? (int)(rnd() % 9) - 4 : 0);
        op.row0 = -op.t + (rnd() % 3 == 0 ? (int)(rnd() % 9) - 4 : 0);
        if (rnd() % 2) {
            op.kind = ROP_BITMAP; op.keyed = rnd() % 3 != 0; op.key = rnd() % 2 ? 0 : srcpx[si][0];
        } else {
            op.kind = ROP_GRID; op.mode = (int)(rnd() % 2);
        }
        fast_vs_plain(op.kind == ROP_GRID ? (op.mode ? "sprite" : "sprite copy") : op.keyed ? "keyed bitmap" : "bitmap",
                      &op, 1, &v);
    }
}

/* the land drawn from its shrunk copies (and from its tiles, zoomed in): random tile sizes, the land anywhere in the
 * region, holes; the copies first filled with rubbish (WA's memory), only some tiles brought up to date -- so texels
 * of coverage 0 with colour, and colour over coverage, are drawn as row_mip draws them too */
static void fuzz_land(int count) {
    static const int tws[] = {32, 24, 40, 16, 56}, ths[] = {32, 40, 16, 48, 24};
    static uint8_t lp[200 * 160], lnow[200 * 160];
    static uint32_t m1[100 * 80], m2[50 * 40], m3[25 * 20], lstamp[128];
    static rop ops[128];
    int k;
    for (k = 0; k < count; k++) {
        rland L;
        rview v;
        const int tw = tws[rnd() % 5], th = ths[rnd() % 5], cols = 1 + (int)(rnd() % (200 / tw)), rows = 1 + (int)(rnd() % (160 / th));
        const int lw = cols * tw, lh = rows * th, lx0 = (int)(rnd() % 61) - 30, ly0 = (int)(rnd() % 61) - 30, rubbish = rnd() % 2;
        const double zhi = rnd() % 4 ? 0.5 : 2.0, zlo = rnd() % 5 ? 0.12 : 0.06;   /* (0.06: many taps a texel) */
        const int levels = rnd() % 3 ? 3 : 1 + (int)(rnd() % 2);
        int i, x, y, tx, ty, n = 0, level;
        for (y = 0; y < lh; y++)
            for (x = 0; x < lw; x++) {
                const int hole = ((x / 5 + y / 7 + (int)(rnd() % 3)) % 4 == 0) || (x > lw / 2 && y < lh / 3);
                lp[y * lw + x] = hole ? 0 : (uint8_t)(1 + (x * 3 + y * 5 + (int)(rnd() % 7)) % 250);
            }
        memset(&L, 0, sizeof(L));
        L.w = lw; L.h = lh; L.tw = tw; L.th = th; L.cols = cols; L.rows = rows; L.levels = levels; L.keyed = 1; L.key = 0;
        L.now = lnow; L.stamp = lstamp;
        memset(lstamp, 0, sizeof(lstamp));
        L.mip[1] = m1; L.mw[1] = (lw + 1) >> 1; L.mh[1] = (lh + 1) >> 1;
        L.mip[2] = m2; L.mw[2] = (lw + 3) >> 2; L.mh[2] = (lh + 3) >> 2;
        L.mip[3] = m3; L.mw[3] = (lw + 7) >> 3; L.mh[3] = (lh + 7) >> 3;
        if (rubbish) {
            for (i = 0; i < 100 * 80; i++) m1[i] = rnd32();
            for (i = 0; i < 50 * 40; i++) m2[i] = rnd() % 3 ? rnd32() : rnd32() & 0xffffffu;
            for (i = 0; i < 25 * 20; i++) m3[i] = rnd32();
        }
        for (ty = 0; ty < rows; ty++)
            for (tx = 0; tx < cols; tx++) {
                rsrc t;
                rop *op = &ops[n++];
                t.data = lp + ty * th * lw + tx * tw; t.w = tw; t.h = th; t.stride = lw;
                if (!rubbish || rnd() % 3) { int ph; for (ph = 0; ph < 4; ph++) raster_land_sync(&L, &t, tx * tw, ty * th, pal, 1, ph, NULL); }
                memset(op, 0, sizeof(*op));
                op->kind = ROP_BITMAP; op->src = t; op->keyed = 1; op->key = 0;
                op->l = lx0 + tx * tw; op->t = ly0 + ty * th; op->r = op->l + tw; op->b = op->t + th;
                op->col0 = -op->l; op->dir = 1; op->row0 = -op->t; op->land_x = tx * tw; op->land_y = ty * th;
                op->no_mip = rnd() % 6 == 0;    /* (copies not ready: this tile from itself) */
            }
        if (rnd() % 4 == 0) {               /* (the first and last tiles reaching past the land: texels outside it read 0) */
            ops[0].l -= (int)(rnd() % 20); ops[0].t -= (int)(rnd() % 20);
            ops[n - 1].r += (int)(rnd() % 20); ops[n - 1].b += (int)(rnd() % 20);
        }
        random_view(&v, zlo, zhi);
        v.bx = lx0 + lw * (rndu() * 0.8 + 0.1); v.by = ly0 + lh * (rndu() * 0.8 + 0.1);
        if (rnd() % 3 == 0) { v.bx = floor(v.bx); v.by = floor(v.by); }
        if (rnd() % 4 == 0) {               /* (aligned: 1/2, 1/4, 1/8 with the copies' texels on whole screen pixels) */
            v.z = 1.0 / (double)(2 << (rnd() % 3));
            v.bx = lx0 + 8.0 * (int)(lw * rndu() / 8.0); v.by = ly0 + 8.0 * (int)(lh * rndu() / 8.0);
        }
        v.land = &L;
        level = v.point ? 0 : raster_level(&L, v.z);
        frame_taps(&v, level, -lx0, -ly0);
        fast_vs_plain(level ? "land from its copies" : "land from its tiles", ops, n, &v);
    }
}

/* the taps a fast row cannot use (a sum that is not its weights' total): drawn by the scalar rows, so the same */
static void odd_taps(void) {
    static rtap gx2[OW];
    rview v;
    rop op;
    int si;
    for (si = 0; si < 4; si += 3) {         /* (a textured source, and one of one colour: there the scalar rows use sum) */
        memset(&op, 0, sizeof(op));
        op.kind = ROP_BITMAP; op.l = 70; op.t = 60; op.r = 70 + 7; op.b = 60 + 40; op.land_x = -1;
        op.src.data = srcpx[si]; op.src.w = 7; op.src.h = 40; op.src.stride = 96; op.col0 = -70; op.dir = 1; op.row0 = -60;
        op.keyed = 1;
        view(&v, 0.83, 72.3, 78.6);
        memcpy(gx2, gxs[0], sizeof(gx2));
        gx2[OW / 2].sum = (int16_t)(gx2[OW / 2].sum - 1);
        v.gx[0] = gx2;
        fast_vs_plain("odd column taps", &op, 1, &v);
        view(&v, 0.83, 72.3, 78.6);
        gys[0][OH / 2].sum = (int16_t)(gys[0][OH / 2].sum - 1);
        fast_vs_plain("odd row taps", &op, 1, &v);
        frame_taps(&v, 0, 0.0, 0.0);
    }
}

/* the tile claims (raster.c): a tile's copies made by one thread at a time, once a palette generation, never made again
 * with an older palette than they have; a tile another thread is at (or past this frame's budget) drawn from itself */
#define CW 96
#define CH 64
static rland CL;
static uint8_t cl_px[CW * CH], cl_now[CW * CH];
static uint32_t cl_m1[48 * 32], cl_m2[24 * 16], cl_m3[12 * 8], cl_stamp[6];
static uint32_t cl_r1[48 * 32], cl_r2[24 * 16], cl_r3[12 * 8];
static volatile LONG cl_go;
static uint32_t cl_live = 1;
static long cl_built;
static void cl_fresh(void) {
    int i;
    memcpy(cl_now, cl_px, sizeof(cl_now));          /* (as at the land's setup: the copy of its pixels made first) */
    memset(cl_stamp, 0, sizeof(cl_stamp));
    for (i = 0; i < 48 * 32; i++) cl_m1[i] = rnd32();
    for (i = 0; i < 24 * 16; i++) cl_m2[i] = rnd32();
    for (i = 0; i < 12 * 8; i++) cl_m3[i] = rnd32();
}
static void cl_tile(rsrc *t, int ti) {
    t->data = cl_px + (ti / 3) * 32 * CW + (ti % 3) * 32; t->w = 32; t->h = 32; t->stride = CW;
}
static int cl_same(void) {
    return !memcmp(cl_m1, cl_r1, sizeof(cl_r1)) && !memcmp(cl_m2, cl_r2, sizeof(cl_r2)) && !memcmp(cl_m3, cl_r3, sizeof(cl_r3));
}
static DWORD WINAPI cl_builder(LPVOID arg) {
    int ti;
    (void)arg;
    while (!cl_go) ;
    for (ti = 0; ti < 6; ti++) cl_built += raster_land_build(&CL, pal, 1, ti, &cl_live);
    return 0;
}
static void land_claims(void) {
    uint32_t two = 2;
    long budget;
    rsrc t;
    int i, ti, round, bad = 0;
    for (i = 0; i < CW * CH; i++) cl_px[i] = (i % 7 == 0) ? 0 : (uint8_t)(1 + (i * 13) % 200);
    memset(&CL, 0, sizeof(CL));
    CL.w = CW; CL.h = CH; CL.tw = 32; CL.th = 32; CL.cols = 3; CL.rows = 2; CL.levels = 3; CL.keyed = 1; CL.key = 0;
    CL.now = cl_now; CL.stamp = cl_stamp;
    CL.mip[1] = cl_m1; CL.mw[1] = 48; CL.mh[1] = 32;
    CL.mip[2] = cl_m2; CL.mw[2] = 24; CL.mh[2] = 16;
    CL.mip[3] = cl_m3; CL.mw[3] = 12; CL.mh[3] = 8;
    /* the reference: every tile made on one thread */
    cl_fresh();
    for (ti = 0; ti < 6; ti++) if (raster_land_build(&CL, pal, 1, ti, NULL) != 1) bad |= 1;
    for (ti = 0; ti < 6; ti++) if (raster_land_build(&CL, pal, 1, ti, NULL) != 0 || cl_stamp[ti] != 1) bad |= 2;   /* (once) */
    memcpy(cl_r1, cl_m1, sizeof(cl_r1)); memcpy(cl_r2, cl_m2, sizeof(cl_r2)); memcpy(cl_r3, cl_m3, sizeof(cl_r3));
    /* the drawing's sync makes the same copies */
    cl_fresh();
    for (ti = 0; ti < 6; ti++) { cl_tile(&t, ti); if (raster_land_sync(&CL, &t, (ti % 3) * 32, (ti / 3) * 32, pal, 1, 0, NULL) != 1) bad |= 4; }
    if (!cl_same()) bad |= 8;
    /* a tile another thread is at: left alone by both */
    cl_fresh();
    cl_stamp[2] = RASTER_BUSY;
    cl_tile(&t, 2);
    if (raster_land_sync(&CL, &t, 64, 0, pal, 1, 0, NULL) != 0 || raster_land_build(&CL, pal, 1, 2, NULL) != 0 || cl_stamp[2] != RASTER_BUSY)
        bad |= 16;
    /* the budget: one tile from scratch, the next drawn from itself (and made the frame after) */
    cl_fresh();
    budget = 1;
    cl_tile(&t, 0);
    if (raster_land_sync(&CL, &t, 0, 0, pal, 1, 0, &budget) != 1 || cl_stamp[0] != 1) bad |= 32;
    cl_tile(&t, 1);
    if (raster_land_sync(&CL, &t, 32, 0, pal, 1, 0, &budget) != 0 || cl_stamp[1] != 0) bad |= 64;
    budget = 1;
    if (raster_land_sync(&CL, &t, 32, 0, pal, 1, 0, &budget) != 1 || cl_stamp[1] != 1) bad |= 128;
    /* the palette moved on: the builder's old one never claims (nor overwrites a tile made with the new one) */
    cl_fresh();
    if (raster_land_build(&CL, pal, 1, 0, &two) != 0 || cl_stamp[0] != 0) bad |= 256;
    if (raster_land_build(&CL, pal, 2, 0, &two) != 1 || cl_stamp[0] != 2) bad |= 512;
    if (raster_land_build(&CL, pal, 1, 0, &two) != 0 || cl_stamp[0] != 2) bad |= 1024;
    if (bad) { printf("FAIL land claims (single thread): %#x\n", bad); failures++; }
    /* the builder and the drawing at the same tiles at once: each made once, none left claimed, copies as the reference */
    for (round = 0; round < 300; round++) {
        HANDLE h;
        int made = 0, drawn_self = 0;
        cl_fresh();
        cl_go = 0;
        cl_built = 0;
        h = CreateThread(NULL, 0, cl_builder, NULL, 0, NULL);
        if (!h) { printf("FAIL land claims: no thread\n"); failures++; return; }
        cl_go = 1;
        for (i = 0; i < 6; i++) {
            ti = (round & 1) ? 5 - i : (i * 5 + round) % 6;
            cl_tile(&t, ti);
            if (raster_land_sync(&CL, &t, (ti % 3) * 32, (ti / 3) * 32, pal, 1, round & 3, NULL)) made++; else drawn_self++;
        }
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
        for (ti = 0; ti < 6; ti++) {          /* (the drawing's next frame: the ones it drew from themselves) */
            cl_tile(&t, ti);
            if (cl_stamp[ti] != 1 && !raster_land_sync(&CL, &t, (ti % 3) * 32, (ti / 3) * 32, pal, 1, 0, NULL)) bad |= 1;
        }
        for (ti = 0; ti < 6; ti++) if (cl_stamp[ti] != 1) bad |= 2;
        if (!cl_same()) bad |= 4;
        if (cl_built > 6 || made + drawn_self != 6) bad |= 8;
        if (bad) { printf("FAIL land claims (two threads, round %d): %#x (builder made %ld)\n", round, bad, cl_built); failures++; return; }
    }
}

int main(void) {
    static uint8_t sprite[48 * 40], landpx[128 * 96], now[128 * 96];
    static uint32_t mip1[64 * 48], mip2[32 * 24], mip3[16 * 12], stamp[4 * 3];
    static const double zooms[] = {0.125, 0.2, 0.25, 0.3, 0.37, 0.5, 0.71, 1.0, 1.3, 2.0, 3.7, 8.0};
    rview v;
    rop op;
    rland L;
    int k, i, x, y;
    for (k = 0; k < 256; k++) pal[k] = (uint32_t)(rnd() & 255) << 16 | (uint32_t)(rnd() & 255) << 8 | (uint32_t)(rnd() & 255);
    pal[0] = 0;
    for (i = 0; i < 48 * 40; i++) sprite[i] = (rnd() % 3) ? (uint8_t)(1 + rnd() % 200) : 0;
    for (k = 0; k < (int)(sizeof(zooms) / sizeof(zooms[0])); k++) {
        double z = zooms[k], bx = 100, by = 80;
        double offs[3] = {0.0, 0.37, -1.61};
        int o;
        for (o = 0; o < 3; o++) {
            view(&v, z, bx + offs[o], by + offs[o] * 0.7);
            memset(&op, 0, sizeof(op));
            op.kind = ROP_FILL; op.l = 61; op.t = 47; op.r = 139; op.b = 103; op.index = 7; op.land_x = -1;
            check("fill", &op, &v, 3, 2, 0.6);
            op.l = 100; op.r = 101; op.t = 20; op.b = 150;                 /* a 1-pixel line (a thin wall) */
            check("line", &op, &v, 3, 2, 0.6);
            memset(&op, 0, sizeof(op));
            op.kind = ROP_BITMAP; op.l = 77; op.t = 61; op.r = 77 + 48; op.b = 61 + 40; op.land_x = -1;
            op.src.data = sprite; op.src.w = 48; op.src.h = 40; op.src.stride = 48;
            op.col0 = -77; op.dir = 1; op.row0 = -61; op.keyed = 1; op.key = 0;
            check("keyed bitmap", &op, &v, 5, 2, 0.6);
            op.keyed = 0;
            check("bitmap", &op, &v, 5, 2, 0.6);
            op.kind = ROP_GRID; op.mode = 1; op.col0 = 77 + 48 - 1; op.dir = -1;  /* mirrored sprite */
            check("mirrored sprite", &op, &v, 9, 2, 0.6);
            op.mode = 0; op.col0 = -77; op.dir = 1;
            check("sprite copy", &op, &v, 9, 2, 0.6);
        }
    }
    /* The 100% copy path (raster.c row_copy1) bit for bit against the plain path, in the cases the checks above don't
     * reach: the nearest-pixel filter; operations cut by the view's edge; operations reaching past their source's
     * rows and columns (read as 0); a one-pixel-wide bitmap; mirrored sprites cut by the view */
    {
        static const struct { double l, t, r, b; int col0, row0, kind, keyed, dir; } cases[] = {
            {77, 61, 125, 101, -77, -61, ROP_BITMAP, 1, 1},     /* (whole, keyed) */
            {-40, 61, 90, 101, 40, -61, ROP_BITMAP, 0, 1},      /* (cut by the view's left edge) */
            {77, -20, 125, 300, -77, 20, ROP_BITMAP, 1, 1},     /* (past the source's rows, cut by the view's top) */
            {60, 61, 140, 101, -77, -61, ROP_BITMAP, 0, 1},     /* (past the source's columns on both sides) */
            {100, 30, 101, 150, -100, -30, ROP_BITMAP, 0, 1},   /* (one pixel wide) */
            {150, 61, 260, 101, 210, -61, ROP_GRID, 0, -1},     /* (mirrored sprite, cut by the view's right edge) */
        };
        int c, pt;
        for (pt = 0; pt < 2; pt++)
            for (c = 0; c < (int)(sizeof(cases) / sizeof(cases[0])); c++) {
                int X;
                view(&v, 1.0, 100, 80);
                v.point = pt;
                frame_taps(&v, 0, 0.0, 0.0);
                memset(&op, 0, sizeof(op));
                op.kind = cases[c].kind; op.l = cases[c].l; op.t = cases[c].t; op.r = cases[c].r; op.b = cases[c].b;
                op.land_x = -1; op.src.data = sprite; op.src.w = 48; op.src.h = 40; op.src.stride = 48;
                op.col0 = cases[c].col0; op.row0 = cases[c].row0; op.dir = cases[c].dir; op.keyed = cases[c].keyed; op.key = 0;
                op.mode = 1;
                for (X = 0; X < OW * OH; X++) out[X] = pal[5];
                raster_fast = 0; draw(&op, &v); raster_fast = 1;
                memcpy(out_plain, out, sizeof(out));
                for (X = 0; X < OW * OH; X++) out[X] = pal[5];
                draw(&op, &v);
                for (X = 0; X < OW * OH; X++)
                    if (out[X] != out_plain[X]) {
                        printf("FAIL copy path case %d (point %d): differs at %d,%d (%06x, plain %06x)\n", c, pt, X % OW, X / OW,
                               out[X], out_plain[X]);
                        failures++;
                        break;
                    }
            }
    }
    /* No seams: operations that meet (WA's sky gradient strips, the background's pieces, the land's tiles), here a grid
     * of 9 x 7 fills and then of opaque one-colour bitmaps, each tiling the next, edges at fractions of a screen pixel,
     * over another colour: every output pixel inside the grid is exactly the grid's colour (a friend's screenshot,
     * 2026-11-22: dark lines where the background showed through between strips) */
    for (k = 0; k < (int)(sizeof(zooms) / sizeof(zooms[0])); k++) {
        static uint8_t solid[16 * 16];
        double offs[3] = {0.0, 0.37, -1.61};
        int o, kind;
        memset(solid, 7, sizeof(solid));
        if (zooms[k] * 7.0 < 1.0) continue;     /* (strips thinner than a screen pixel keep their shares: see raster.c) */
        for (o = 0; o < 3; o++)
            for (kind = 0; kind < 2; kind++) {
                const double z = zooms[k];
                int X, Y, worst = 0, wx = 0, wy = 0, gx, gy;
                double ux1, ux2, uy1, uy2;
                view(&v, z, 100 + offs[o], 80 + offs[o] * 0.7);
                for (X = 0; X < OW * OH; X++) out[X] = pal[3];
                for (gy = 0; gy < 7; gy++)
                    for (gx = 0; gx < 9; gx++) {
                        memset(&op, 0, sizeof(op));
                        op.land_x = -1;
                        op.l = 64 + gx * 9; op.r = op.l + 9; op.t = 50 + gy * 7; op.b = op.t + 7;
                        if (kind == 0) { op.kind = ROP_FILL; op.index = 7; }
                        else {
                            op.kind = ROP_BITMAP; op.src.data = solid; op.src.w = 16; op.src.h = 16; op.src.stride = 16;
                            op.col0 = -(int)op.l; op.dir = 1; op.row0 = -(int)op.t;
                        }
                        draw(&op, &v);
                    }
                ux1 = (64 - v.bx) * z + v.ox; ux2 = (64 + 81 - v.bx) * z + v.ox;
                uy1 = (50 - v.by) * z + v.oy; uy2 = (50 + 49 - v.by) * z + v.oy;
                for (Y = (int)ceil(uy1); Y < (int)floor(uy2); Y++)
                    for (X = (int)ceil(ux1); X < (int)floor(ux2); X++) {
                        int d;
                        if (X < v.x1 || X >= v.x2 || Y < v.y1 || Y >= v.y2) continue;
                        d = cdiff(out[Y * OW + X], pal[7]);
                        if (d > worst) { worst = d; wx = X; wy = Y; }
                    }
                if (worst > 1) {
                    printf("FAIL seam (%s) z=%.4f offset %d: worst %d at %d,%d (got %06x, want %06x)\n", kind ? "bitmaps" : "fills",
                           z, o, worst, wx, wy, out[wy * OW + wx], pal[7]);
                    failures++;
                }
            }
    }
    /* the land: 128 x 96 in 32 x 32 tiles, drawn from its shrunk copies */
    for (y = 0; y < 96; y++)
        for (x = 0; x < 128; x++)
            landpx[y * 128 + x] = (x == 50 || (x > 70 && y > 40 && ((x ^ y) & 4))) ? (uint8_t)(10 + (x + y) % 30) : 0;
    memset(&L, 0, sizeof(L));
    L.w = 128; L.h = 96; L.tw = 32; L.th = 32; L.cols = 4; L.rows = 3; L.levels = 3; L.keyed = 1; L.key = 0;
    L.now = now; L.stamp = stamp;
    L.mip[1] = mip1; L.mw[1] = 64; L.mh[1] = 48;
    L.mip[2] = mip2; L.mw[2] = 32; L.mh[2] = 24;
    L.mip[3] = mip3; L.mw[3] = 16; L.mh[3] = 12;
    for (i = 0; i < 2; i++) {                                         /* second round: the land blown up a bit */
        int tx, ty;
        rop tiles[12];
        if (i) for (y = 30; y < 60; y++) for (x = 40; x < 90; x++) if ((x - 65) * (x - 65) + (y - 45) * (y - 45) < 150) landpx[y * 128 + x] = 0;
        for (k = 0; k < (int)(sizeof(zooms) / sizeof(zooms[0])); k++) {
            double z = zooms[k];
            int aligned;
            for (aligned = 1; aligned >= 0; aligned--) {
                double bx = aligned ? 0.0 : 0.43, by = aligned ? 0.0 : 0.77;
                int X, Y, worst = 0;
                double sum = 0;
                view(&v, z, bx, by);
                v.ox = 0; v.oy = 0; v.land = &L;
                frame_taps(&v, raster_level(&L, z), -16.0, -16.0);   /* (the land's tiles all lie 16 to the right and down) */
                ref_bg = 2;
                memset(region, 2, sizeof(region));
                for (X = 0; X < OW * OH; X++) out[X] = pal[2];
                for (ty = 0; ty < 3; ty++)
                    for (tx = 0; tx < 4; tx++) {
                        rsrc t;
                        memset(&op, 0, sizeof(op));
                        t.data = landpx + ty * 32 * 128 + tx * 32; t.w = 32; t.h = 32; t.stride = 128;
                        { int ph; for (ph = 0; ph < 4; ph++) raster_land_sync(&L, &t, tx * 32, ty * 32, pal, 1, ph, NULL); }
                        op.kind = ROP_BITMAP; op.src = t; op.keyed = 1; op.key = 0;
                        op.l = 16 + tx * 32; op.t = 16 + ty * 32; op.r = op.l + 32; op.b = op.t + 32;
                        op.col0 = -op.l; op.dir = 1; op.row0 = -op.t; op.land_x = tx * 32; op.land_y = ty * 32;
                        ref_draw(&op);
                        draw(&op, &v);
                        tiles[ty * 4 + tx] = op;
                    }
                for (Y = 0; Y < OH; Y++)
                    for (X = 0; X < OW; X++) {
                        int d, edge = 0;
                        for (ty = 0; ty < 3 && !edge; ty++)          /* (the tiles' snapped edges: see check) */
                            for (tx = 0; tx < 4 && !edge; tx++) {
                                rop e;
                                memset(&e, 0, sizeof(e));
                                e.l = 16 + tx * 32; e.t = 16 + ty * 32; e.r = e.l + 32; e.b = e.t + 32;
                                edge = snapped_edge(&e, &v, X, Y);
                            }
                        if (edge) continue;
                        d = cdiff(out[Y * OW + X], ref_px(&v, X, Y));
                        sum += d;
                        if (d > worst) worst = d;
                    }
                sum /= OW * OH;
                /* exact where the copies line up with the screen; elsewhere a shrunk copy smears a little */
                if ((aligned && z <= 1.0 && fabs(log2(z) - floor(log2(z) + 0.5)) < 1e-9 ? worst > 3 : worst > 90) || sum > 3.0) {
                    printf("FAIL land%s z=%.4f %s: worst %d, mean %.3f (level %d)\n", i ? " (blown up)" : "", z,
                           aligned ? "aligned" : "offset", worst, sum, raster_level(&L, z));
                    failures++;
                }
                fast_vs_plain("land", tiles, 12, &v);  /* (and the fast rows bit for bit the plain ones) */
            }
        }
    }
    /* the fast paths against the plain ones over random cases (bitmaps, sprites, the land) */
    for (k = 0; k < NSRC; k++)
        for (i = 0; i < 96 * 80; i++)
            srcpx[k][i] = k == 2 ? (uint8_t)(40 + (i % 96) / 4 + (int)(rnd() % 3))            /* (opaque, textured) */
                        : rnd() % 3 ? (uint8_t)(1 + rnd() % 200) : rnd() % 2 ? 0 : (uint8_t)(rnd() % 4);
    memset(srcpx[3], 9, sizeof(srcpx[3]));                                  /* (one colour) */
    fuzz_ops(4000);
    fuzz_land(400);
    odd_taps();
    land_claims();
    printf("raster_test: fast vs plain: %d cases, %d differ\n", fuzz_cases, fuzz_fail);
    if (failures) { printf("raster_test: %d failures\n", failures); return 1; }
    printf("raster_test: ok\n");
    return 0;
}
