/* raster_test.c: the zoomed drawing (src/raster.c) against a plain reference -- each operation drawn at WA's own scale
 * into a region of palette indices, then every output pixel the exact average of the region pixels it covers. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* one op over a background of index bg: the output against the reference (snapped edges aside); max and mean error */
static void check(const char *name, const rop *op, rview *v, uint8_t bg, int max_ok, double mean_ok) {
    int X, Y, worst = 0, wx = 0, wy = 0;
    double sum = 0;
    ref_bg = bg;
    memset(region, bg, sizeof(region));
    for (X = 0; X < OW * OH; X++) out[X] = pal[bg];
    ref_draw(op);
    draw(op, v);
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
                        { int ph; for (ph = 0; ph < 4; ph++) raster_land_sync(&L, &t, tx * 32, ty * 32, pal, 1, ph); }
                        op.kind = ROP_BITMAP; op.src = t; op.keyed = 1; op.key = 0;
                        op.l = 16 + tx * 32; op.t = 16 + ty * 32; op.r = op.l + 32; op.b = op.t + 32;
                        op.col0 = -op.l; op.dir = 1; op.row0 = -op.t; op.land_x = tx * 32; op.land_y = ty * 32;
                        ref_draw(&op);
                        draw(&op, &v);
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
            }
        }
    }
    if (failures) { printf("raster_test: %d failures\n", failures); return 1; }
    printf("raster_test: ok\n");
    return 0;
}
