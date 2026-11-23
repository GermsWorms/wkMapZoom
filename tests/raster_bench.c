/* raster_bench.c: how long src/raster.c takes for a full 2560 x 1440 frame, on one thread, with the fast paths
 * (raster_fast 1) and without (0). Not part of build.ps1. Build and run from the repo:
 *   gcc -O2 -Wall -Wextra -std=c99 -msse2 -mfpmath=sse -fno-math-errno -fno-trapping-math tests/raster_bench.c
 *       src/raster.c -lgdi32 -o raster_bench.exe && raster_bench.exe [runs]
 * Cases: (a) keyed land tiles of 296 x 496 (8-bit, about 60% solid, textured) covering the screen, at 111% and 73%
 * (drawn from the tiles) and 35% (from the land's half-size copy); (b) opaque 64 x 65 bitmap pieces (WA's background)
 * covering the screen at 73% and 35%. The camera sits at a fraction of a pixel. Each case's two outputs must be bit for
 * bit the same. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "../src/raster.h"

extern int raster_fast;

#define SW 2560
#define SH 1440
#define TW 296
#define TH 496
#define LCOLS 26
#define LROWS 10
#define LW (TW * LCOLS)
#define LH (TH * LROWS)
#define LX0 8                   /* the land's (0, 0) in the region */
#define LY0 8

static uint32_t pal[256];
static uint32_t *out, *out_plain;
static uint8_t *landpx, *now;
static uint8_t piece[64 * 65];
static rtap *gx[RASTER_LEVELS + 1], *gy[RASTER_LEVELS + 1];
static rland L;
static unsigned rng = 2026;
static unsigned rnd(void) { rng = rng * 1103515245u + 12345u; return (rng >> 16) & 0x7fff; }
static uint8_t nearest(uint32_t c) { (void)c; return 0; }

static double now_ms(void) {
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

static void make_view(rview *v, double z, double bx, double by) {
    int l;
    memset(v, 0, sizeof(*v));
    v->z = z; v->bx = bx; v->by = by; v->ox = 0; v->oy = 0;
    v->x1 = 0; v->y1 = 0; v->x2 = SW; v->y2 = SH;
    v->out = out; v->stride = SW; v->pal = pal; v->nearest = nearest; v->gen = 1; v->land = &L;
    raster_axis(gx[0], 0, SW, 0, bx, z, 0.0, 0, 0);
    raster_axis(gy[0], 0, SH, 0, by, z, 0.0, 0, 0);
    v->gx[0] = gx[0]; v->gy[0] = gy[0];
    l = raster_level(&L, z);
    if (l) {                                /* (the land's tiles: source column = region x - LX0 - tile's land x) */
        raster_axis(gx[l], 0, SW, 0, bx, z, -LX0, l, 0);
        raster_axis(gy[l], 0, SH, 0, by, z, -LY0, l, 0);
        v->gx[l] = gx[l]; v->gy[l] = gy[l]; v->gox[l] = -LX0; v->goy[l] = -LY0;
    }
}

static void draw_op(const rop *op, const rview *v) {
    rprep p;
    if (raster_prepare(op, v, &p)) raster_draw(op, v, &p, p.y1, p.y2);
}

static void draw_land(const rview *v) {
    int tx, ty;
    for (ty = 0; ty < LROWS; ty++)
        for (tx = 0; tx < LCOLS; tx++) {
            rop op;
            memset(&op, 0, sizeof(op));
            op.kind = ROP_BITMAP; op.keyed = 1; op.key = 0;
            op.src.data = landpx + (size_t)ty * TH * LW + (size_t)tx * TW; op.src.w = TW; op.src.h = TH; op.src.stride = LW;
            op.l = LX0 + tx * TW; op.t = LY0 + ty * TH; op.r = op.l + TW; op.b = op.t + TH;
            op.col0 = -op.l; op.dir = 1; op.row0 = -op.t; op.land_x = tx * TW; op.land_y = ty * TH;
            draw_op(&op, v);
        }
}

static void draw_pieces(const rview *v) {
    const int nx = (int)(SW / v->z) / 64 + 3, ny = (int)(SH / v->z) / 65 + 3;
    int px, py;
    for (py = 0; py < ny; py++)
        for (px = 0; px < nx; px++) {
            rop op;
            memset(&op, 0, sizeof(op));
            op.kind = ROP_BITMAP; op.keyed = 0; op.land_x = -1;
            op.src.data = piece; op.src.w = 64; op.src.h = 65; op.src.stride = 64;
            op.l = (int)v->bx - 32 + px * 64; op.t = (int)v->by - 32 + py * 65; op.r = op.l + 64; op.b = op.t + 65;
            op.col0 = -op.l; op.dir = 1; op.row0 = -op.t;
            draw_op(&op, v);
        }
}

static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

static double time_case(int land, double z, int fast, int runs) {
    double t[64];
    rview v;
    int r;
    raster_fast = fast;
    make_view(&v, z, 123.37, 77.61);
    for (r = 0; r < runs; r++) {
        double t0;
        int i;
        for (i = 0; i < SW * SH; i++) out[i] = 0x203040;
        t0 = now_ms();
        if (land) draw_land(&v); else draw_pieces(&v);
        t[r] = now_ms() - t0;
    }
    raster_fast = 1;
    qsort(t, (size_t)runs, sizeof(double), cmp_d);
    return t[runs / 2];
}

int main(int argc, char **argv) {
    static const struct { int land; double z; const char *name; } cases[] = {
        {1, 1.11, "land 111% (tiles)"}, {1, 0.73, "land 73% (tiles)"}, {1, 0.35, "land 35% (1/2 copy)"},
        {1, 1.6, "land 160% (tiles)"}, {1, 2.5, "land 250% (tiles)"},
        {0, 0.73, "pieces 73%"}, {0, 0.35, "pieces 35%"}, {0, 1.6, "pieces 160%"},
    };
    int runs = argc > 1 ? atoi(argv[1]) : 9, k, i, x, y, bad = 0;
    if (runs < 1) runs = 1;
    if (runs > 64) runs = 64;
    out = (uint32_t *)malloc(sizeof(uint32_t) * SW * SH);
    out_plain = (uint32_t *)malloc(sizeof(uint32_t) * SW * SH);
    landpx = (uint8_t *)malloc((size_t)LW * LH);
    now = (uint8_t *)malloc((size_t)LW * LH);
    for (k = 0; k <= RASTER_LEVELS; k++) {
        gx[k] = (rtap *)malloc(sizeof(rtap) * SW);
        gy[k] = (rtap *)malloc(sizeof(rtap) * SH);
    }
    for (k = 0; k < 256; k++) pal[k] = (uint32_t)(rnd() & 255) << 16 | (uint32_t)(rnd() & 255) << 8 | (uint32_t)(rnd() & 255);
    /* the land: rolling shapes of solid (about 60%), textured pixel by pixel; 0 is see-through */
    {
        long solid = 0;
        for (y = 0; y < LH; y++)
            for (x = 0; x < LW; x++) {
                const double f = sin(x / 97.0) + sin(y / 61.0 + x / 150.0) + sin((x + y) / 43.0) + 0.5 * sin(x / 13.0 + y / 17.0);
                const int s = f > -0.33;
                solid += s;
                landpx[(size_t)y * LW + x] = s ? (uint8_t)(16 + ((x >> 1) + (y >> 2) + (int)(rnd() % 6)) % 160) : 0;
            }
        printf("land: %dx%d, %.0f%% solid\n", LW, LH, 100.0 * solid / ((double)LW * LH));
    }
    for (i = 0; i < 64 * 65; i++) piece[i] = (uint8_t)(40 + (i % 64) / 4 + (i / 64) / 3 + (int)(rnd() % 3));
    memset(&L, 0, sizeof(L));
    L.w = LW; L.h = LH; L.tw = TW; L.th = TH; L.cols = LCOLS; L.rows = LROWS; L.levels = RASTER_LEVELS; L.keyed = 1; L.key = 0;
    L.now = now; L.stamp = (uint32_t *)calloc(LCOLS * LROWS, sizeof(uint32_t));
    for (k = 1; k <= RASTER_LEVELS; k++) {
        L.mw[k] = (LW + (1 << k) - 1) >> k; L.mh[k] = (LH + (1 << k) - 1) >> k;
        L.mip[k] = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)L.mw[k] * L.mh[k]);
    }
    for (y = 0; y < LROWS; y++)
        for (x = 0; x < LCOLS; x++) {
            rsrc t;
            t.data = landpx + (size_t)y * TH * LW + (size_t)x * TW; t.w = TW; t.h = TH; t.stride = LW;
            raster_land_sync(&L, &t, x * TW, y * TH, pal, 1, 0, NULL);
        }
    printf("%-22s %10s %10s %8s  (median of %d, one thread, %dx%d)\n", "case", "plain ms", "fast ms", "speed-up", runs, SW, SH);
    for (k = 0; k < (int)(sizeof(cases) / sizeof(cases[0])); k++) {
        const double tp = time_case(cases[k].land, cases[k].z, 0, runs);
        double tf;
        memcpy(out_plain, out, sizeof(uint32_t) * SW * SH);
        tf = time_case(cases[k].land, cases[k].z, 1, runs);
        if (memcmp(out_plain, out, sizeof(uint32_t) * SW * SH)) { printf("FAIL %s: fast and plain differ\n", cases[k].name); bad++; }
        printf("%-22s %10.2f %10.2f %7.2fx\n", cases[k].name, tp, tf, tp / tf);
    }
    return bad ? 1 : 0;
}
