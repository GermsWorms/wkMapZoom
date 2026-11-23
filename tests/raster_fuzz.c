/* raster_fuzz.c: an independent attempt to make src/raster.c's fast rows (raster_fast 1) differ from the plain ones
 * (raster_fast 0). Every case draws the same operations both ways over the same random 32-bit background (4th bytes
 * too) in the same random row bands and compares the WHOLE output buffer (outside the view and the ops too), every
 * byte. Sources, palette, the land's copies and the output sit right against no-access guard pages (in front or behind,
 * by turns), so any read or write past a buffer's end crashes. The fast pass is called through a stack misaligned by
 * 0, 4, 8 or 12 bytes (as WA's MSVC code may call it).
 *   Part A: realistic cases through raster_axis / raster_prepare: zooms 0.03 .. 12 (log-uniform, plus 100%, 1/2, 1/4,
 *           1/8 and other exact ones), cameras whole, fractional or a hair off whole, sub-views, the point filter,
 *           keyed / unkeyed bitmaps (any key), sprites (modes 0..3, lut), fills, mirrored, reaching past the source,
 *           1-px ops, big ops (up to 2600 output px wide: many parts), land tiles from copies of random contents
 *           (rubbish, premultiplied, alpha 0 with colour, alpha 255, uniform blocks), full-colour land.
 *   Part B: synthetic rprep with arbitrary taps (any n 0..10, zero weights anywhere, partial sums, k0 anywhere
 *           including far outside, non-monotonic k0, garbage past n, sometimes corrupted sums / weights), xdir +-1
 *           (sometimes 0 or 2), xbase / ybase anywhere.
 *   Part C: WA-like frames (sky 64 x 1024 strips, background 64 x 65 pieces, land 296 x 496 tiles with copies).
 * Build (from the repo): gcc <build.ps1 flags> [-mstackrealign] tests/raster_fuzz.c src/raster.c -lgdi32 -o fuzz.exe
 * Run: fuzz.exe [casesA] [casesB] [casesC] [seed] */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "../src/raster.h"

extern int raster_fast;

/* ---- random ------------------------------------------------------------------------------------------------------ */
static uint64_t rs = 0x9E3779B97F4A7C15ull;
static uint32_t r32(void) {
    rs ^= rs >> 12; rs ^= rs << 25; rs ^= rs >> 27;
    return (uint32_t)((rs * 2685821657736338717ull) >> 32);
}
static int ri(int n) { return n > 0 ? (int)(r32() % (uint32_t)n) : 0; }          /* [0, n) */
static int rr(int a, int b) { return b < a ? a : a + ri(b - a + 1); }            /* [a, b] */
static double ru(void) { return ((double)r32() + 0.5) / 4294967296.0; }          /* (0, 1) */
static int chance(int pct) { return ri(100) < pct; }

/* ---- guarded memory ---------------------------------------------------------------------------------------------- */
typedef struct { void *base; } gblock;
#define MAXG 64
static gblock gb[MAXG];
static int ngb;
static size_t page_size(void) { SYSTEM_INFO si; GetSystemInfo(&si); return si.dwPageSize; }
/* size bytes, flush against a no-access page in front (front = 1) or behind (front = 0) */
static void *galloc(size_t size, int front) {
    const size_t pg = page_size(), pages = (size + pg - 1) / pg + (size == 0);
    uint8_t *base;
    DWORD old;
    if (ngb >= MAXG) { printf("galloc: too many\n"); exit(2); }
    base = (uint8_t *)VirtualAlloc(NULL, (pages + 2) * pg, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!base) { printf("galloc: VirtualAlloc failed\n"); exit(2); }
    VirtualProtect(base, pg, PAGE_NOACCESS, &old);
    VirtualProtect(base + (pages + 1) * pg, pg, PAGE_NOACCESS, &old);
    gb[ngb++].base = base;
    return front ? base + pg : base + (pages + 1) * pg - size;
}
static void gfree_all(void) {
    int k;
    for (k = 0; k < ngb; k++) VirtualFree(gb[k].base, 0, MEM_RELEASE);
    ngb = 0;
}

/* ---- a misaligned call ------------------------------------------------------------------------------------------- */
struct dargs { const rop *op; const rview *v; const rprep *p; int y0, y1; };
static void draw_mis(const rop *op, const rview *v, const rprep *p, int y0, int y1, int mis) {
    struct dargs a, *ap = &a;
    a.op = op; a.v = v; a.p = p; a.y0 = y0; a.y1 = y1;
    /* (at raster_draw's entry esp == 8 - mis mod 16; the 16-byte convention wants 12: mis 12 aligned, 0 / 4 / 8 not) */
    __asm__ __volatile__(
        "movl %%esp, %%esi\n\t"
        "andl $-16, %%esp\n\t"
        "subl %%ecx, %%esp\n\t"
        "pushl 16(%%eax)\n\t"
        "pushl 12(%%eax)\n\t"
        "pushl 8(%%eax)\n\t"
        "pushl 4(%%eax)\n\t"
        "pushl (%%eax)\n\t"
        "call _raster_draw\n\t"
        "movl %%esi, %%esp\n\t"
        : "+a"(ap), "+c"(mis)
        :
        : "edx", "esi", "memory", "cc", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7");
}

/* ---- the world of one case --------------------------------------------------------------------------------------- */
static uint32_t *pal;
static uint8_t *lut;
static uint8_t nearest(uint32_t c) {
    uint32_t h = (c & 0xffffffu) * 2654435761u;
    return (c & 0x0f0f0fu) == 0 ? 0 : (uint8_t)(h >> 24);
}

typedef struct { uint8_t *data; int w, h, stride; } src_t;
#define MAXSRC 4
static src_t srcs[MAXSRC];
static int nsrc;

static void fill_src(src_t *s, int w, int h) {
    const int stride = chance(60) ? w : w + rr(1, 40), kind = ri(8);
    const size_t size = (size_t)stride * (h - 1) + (size_t)w;
    const uint8_t a = (uint8_t)r32(), b = (uint8_t)r32(), c = chance(50) ? 0 : (uint8_t)r32();
    int x, y;
    s->w = w; s->h = h; s->stride = stride;
    s->data = (uint8_t *)galloc(size, chance(50));
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            uint8_t v;
            switch (kind) {
            case 0: v = (uint8_t)r32(); break;                                 /* (anything) */
            case 1: v = a; break;                                              /* (one colour) */
            case 2: v = chance(70) ? 0 : (uint8_t)r32(); break;                /* (mostly 0) */
            case 3: v = chance(50) ? a : b; break;                             /* (two colours) */
            case 4: v = ((x / 3 + y / 5) & 1) ? a : c; break;                  /* (blocks) */
            case 5: v = (uint8_t)(40 + x / 4 + y / 3 + ri(3)); break;          /* (WA background-like) */
            case 6: v = ((x * 7 + y * 3) % 11) < 6 ? (uint8_t)(16 + (x + y) % 160) : 0; break;   /* (land-like) */
            default: v = chance(90) ? c : (uint8_t)r32(); break;               /* (mostly one, some others) */
            }
            s->data[(size_t)y * stride + x] = v;
        }
}

/* the land and its copies */
static rland L;
static void fill_mip(uint32_t *m, int mw, int mh, int kind) {
    int i, n = mw * mh;
    const uint32_t one = r32() | 0xff000000u, oneb = r32();
    for (i = 0; i < n; i++) {
        uint32_t t = r32();
        int k = kind == 6 ? ri(6) : kind;
        switch (k) {
        case 0: break;                                                         /* (rubbish) */
        case 1: {                                                              /* (premultiplied: channels <= A) */
            const uint32_t A = chance(30) ? 255u : chance(20) ? 0u : (uint32_t)ri(256);
            const uint32_t R = A ? (uint32_t)ri((int)A + 1) : 0, G = A ? (uint32_t)ri((int)A + 1) : 0;
            const uint32_t B = A ? (uint32_t)ri((int)A + 1) : 0;
            t = A << 24 | R << 16 | G << 8 | B;
            break;
        }
        case 2: t &= 0x00ffffffu; break;                                        /* (alpha 0 with colour) */
        case 3: t |= 0xff000000u; break;                                        /* (opaque) */
        case 4: t = ((i / 3) & 1) ? one : 0u; break;                            /* (uniform blocks and holes) */
        default: t = chance(80) ? oneb : t; break;                              /* (mostly one texel value) */
        }
        m[i] = t;
    }
}
static void make_land(int need_full) {
    int lev;
    memset(&L, 0, sizeof(L));
    L.levels = rr(1, 3);
    L.w = rr(40, 1200); L.h = rr(40, 400);
    L.tw = 8; L.th = 8; L.cols = 1; L.rows = 1; L.keyed = 1; L.key = 0;
    for (lev = 1; lev <= L.levels; lev++) {
        int mw = (L.w + (1 << lev) - 1) >> lev, mh = (L.h + (1 << lev) - 1) >> lev;
        if (chance(20)) { mw += rr(-3, 3); mh += rr(-3, 3); }
        if (mw < 1) mw = 1;
        if (mh < 1) mh = 1;
        L.mw[lev] = mw; L.mh[lev] = mh;
        L.mip[lev] = (uint32_t *)galloc(sizeof(uint32_t) * (size_t)mw * mh, chance(50));
        fill_mip(L.mip[lev], mw, mh, ri(7));
    }
    if (chance(15)) L.mip[L.levels] = NULL;                                      /* (a level not there) */
    if (need_full) {
        int i;
        uint8_t *snap = (uint8_t *)galloc((size_t)L.w * L.h, 0);
        uint32_t *full = (uint32_t *)galloc(sizeof(uint32_t) * (size_t)L.w * L.h, 0);
        for (i = 0; i < L.w * L.h; i++) { snap[i] = (uint8_t)ri(4); full[i] = chance(50) ? 0 : r32(); }
        L.snap = snap; L.full = full;
    }
}

/* ---- output and comparison --------------------------------------------------------------------------------------- */
static uint32_t *outbuf, *bg, *res0;
static int OSTRIDE, OROWS;
static long cases, diffs, diffs_rgb, crashes_avoided, fast_px, mis_calls;

typedef struct { rop op; rprep p; int prepared; } job;
#define MAXJOB 400
static job jobs[MAXJOB];
static int njob;

static int cuts[8], ncut;
static void make_cuts(int H) {
    int k, n = rr(1, 5);
    cuts[0] = 0;
    for (k = 1; k < n; k++) cuts[k] = ri(H + 1);
    cuts[n] = H;
    for (k = 1; k < n; k++) {                                                   /* (sorted) */
        int j;
        for (j = k + 1; j < n; j++) if (cuts[j] < cuts[k]) { int t = cuts[j]; cuts[j] = cuts[k]; cuts[k] = t; }
    }
    ncut = n;
}

static void run_jobs(const rview *v, const char *what) {
    int pass, i, b;
    const size_t n = (size_t)OSTRIDE * OROWS;
    size_t X;
    for (X = 0; X < n; X++) bg[X] = r32();
    for (pass = 0; pass < 2; pass++) {
        memcpy(outbuf, bg, n * sizeof(uint32_t));
        raster_fast = pass;
        for (i = 0; i < njob; i++) {
            if (!jobs[i].prepared) continue;
            for (b = 0; b < ncut; b++) {
                if (pass && chance(50)) { draw_mis(&jobs[i].op, v, &jobs[i].p, cuts[b], cuts[b + 1], 4 * ri(4)); mis_calls++; }
                else raster_draw(&jobs[i].op, v, &jobs[i].p, cuts[b], cuts[b + 1]);
            }
        }
        if (!pass) memcpy(res0, outbuf, n * sizeof(uint32_t));
    }
    raster_fast = 1;
    cases++;
    for (X = 0; X < n; X++)
        if (((res0[X] ^ outbuf[X]) & 0xffffffu) != 0) { diffs_rgb++; break; }
    for (X = 0; X < n; X++)
        if (res0[X] != outbuf[X]) {
            if (diffs < 20) {
                printf("DIFF %s case %ld: z=%.6f bx=%.6f by=%.6f ox=%d oy=%d view %d..%d x %d..%d point %d at %d,%d "
                       "fast %08x plain %08x bg %08x\n", what, cases, v->z, v->bx, v->by, v->ox, v->oy, v->x1, v->x2, v->y1,
                       v->y2, v->point, (int)(X % OSTRIDE), (int)(X / OSTRIDE), outbuf[X], res0[X], bg[X]);
                for (i = 0; i < njob; i++)
                    if (jobs[i].prepared)
                        printf("   job %d: kind %d keyed %d key %d mode %d lut %d dir %d l..r %d..%d t..b %d..%d src %dx%d/%d "
                               "col0 %d row0 %d land %d,%d level %d p %d..%d x %d..%d\n", i, jobs[i].op.kind, jobs[i].op.keyed,
                               jobs[i].op.key, jobs[i].op.mode, jobs[i].op.lut != NULL, jobs[i].op.dir, jobs[i].op.l,
                               jobs[i].op.r, jobs[i].op.t, jobs[i].op.b, jobs[i].op.src.w, jobs[i].op.src.h,
                               jobs[i].op.src.stride, jobs[i].op.col0, jobs[i].op.row0, jobs[i].op.land_x, jobs[i].op.land_y,
                               jobs[i].p.level, jobs[i].p.x1, jobs[i].p.x2, jobs[i].p.y1, jobs[i].p.y2);
            }
            diffs++;
            break;
        }
    for (X = 0; X < n; X++) fast_px += res0[X] != bg[X];
}

/* ---- the view ------------------------------------------------------------------------------------------------------ */
static rtap *gxa[RASTER_LEVELS + 1], *gya[RASTER_LEVELS + 1];
static double pick_zoom(void) {
    static const double exact[] = {1.0, 1.0, 1.0, 0.5, 0.25, 0.125, 2.0, 4.0, 8.0, 1.0 / 3, 2.0 / 3, 0.75, 1.25, 1.5,
                                   0.35, 0.73, 1.11, 1.6, 2.5, 0.0625, 0.1, 0.9999999, 1.0000001, 0.5000001};
    if (chance(35)) return exact[ri((int)(sizeof(exact) / sizeof(exact[0])))];
    return exp(log(0.03) + (log(12.0) - log(0.03)) * ru());
}
static double pick_cam(void) {
    const double base = (double)rr(-60, 500);
    switch (ri(6)) {
    case 0: return base;
    case 1: return base + 1e-4 * (chance(50) ? 1 : -1);
    case 2: return base + 1.0 / 512 + (ru() - 0.5) * 1e-5;
    case 3: return base + 0.5 + (ru() - 0.5) * 1e-6;
    default: return base + ru();
    }
}
/* a view W x H (the output buffer W + extra wide, H rows) */
static void make_view(rview *v, int W, int H) {
    memset(v, 0, sizeof(*v));
    v->z = pick_zoom(); v->bx = pick_cam(); v->by = pick_cam();
    v->x1 = 0; v->x2 = W; v->y1 = 0; v->y2 = H;
    if (chance(25)) { v->x1 = ri(W / 3 + 1); v->x2 = W - ri(W / 3 + 1); if (v->x2 <= v->x1) v->x2 = v->x1 + 1; }
    if (chance(25)) { v->y1 = ri(H / 3 + 1); v->y2 = H - ri(H / 3 + 1); if (v->y2 <= v->y1) v->y2 = v->y1 + 1; }
    v->ox = chance(50) ? W / 2 : rr(-W / 2, W + W / 2);
    v->oy = chance(50) ? H / 2 : rr(-H / 2, H + H / 2);
    v->point = chance(12);
    OSTRIDE = W + (chance(50) ? 0 : rr(1, 9)); OROWS = H;
    outbuf = (uint32_t *)galloc(sizeof(uint32_t) * (size_t)OSTRIDE * OROWS, chance(50));
    bg = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)OSTRIDE * OROWS);
    res0 = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)OSTRIDE * OROWS);
    v->out = outbuf; v->stride = OSTRIDE; v->pal = pal; v->nearest = nearest; v->gen = 1;
    gxa[0] = (rtap *)malloc(sizeof(rtap) * (size_t)(v->x2 - v->x1));
    gya[0] = (rtap *)malloc(sizeof(rtap) * (size_t)(v->y2 - v->y1));
    raster_axis(gxa[0], v->x1, v->x2, v->ox, v->bx, v->z, 0.0, 0, v->point);
    raster_axis(gya[0], v->y1, v->y2, v->oy, v->by, v->z, 0.0, 0, v->point);
    v->gx[0] = gxa[0]; v->gy[0] = gya[0];
}
static void land_taps(rview *v, int level, double offx, double offy) {
    gxa[level] = (rtap *)malloc(sizeof(rtap) * (size_t)(v->x2 - v->x1));
    gya[level] = (rtap *)malloc(sizeof(rtap) * (size_t)(v->y2 - v->y1));
    raster_axis(gxa[level], v->x1, v->x2, v->ox, v->bx, v->z, offx, level, 0);
    raster_axis(gya[level], v->y1, v->y2, v->oy, v->by, v->z, offy, level, 0);
    v->gx[level] = gxa[level]; v->gy[level] = gya[level]; v->gox[level] = offx; v->goy[level] = offy;
}
static void free_view(void) {
    int k;
    for (k = 0; k <= RASTER_LEVELS; k++) { free(gxa[k]); free(gya[k]); gxa[k] = gya[k] = NULL; }
    free(bg); free(res0);
    bg = res0 = NULL;
}

static void new_palette(void) {
    int k;
    pal = (uint32_t *)galloc(256 * sizeof(uint32_t), chance(50));
    for (k = 0; k < 256; k++) pal[k] = chance(50) ? r32() : (r32() & 0xffffffu);   /* (4th bytes: rubbish or 0) */
    if (chance(30)) pal[0] = 0;
    if (chance(10)) for (k = 0; k < 256; k++) pal[k] |= 0xffffffu;                  /* (all white: clamps) */
    lut = (uint8_t *)galloc(65536, 0);
    for (k = 0; k < 65536; k++) lut[k] = (uint8_t)r32();
}

/* a random op over the view's region */
static void random_op(rop *op, const rview *v, int allow_land) {
    const double s0 = v->bx + (v->x1 - v->ox) / v->z, s1 = v->bx + (v->x2 - v->ox) / v->z;
    const double t0 = v->by + (v->y1 - v->oy) / v->z, t1 = v->by + (v->y2 - v->oy) / v->z;
    const src_t *s = &srcs[ri(nsrc)];
    int w, h, cx, cy;
    memset(op, 0, sizeof(*op));
    op->land_x = -1;
    op->src.data = s->data; op->src.w = s->w; op->src.h = s->h; op->src.stride = s->stride;
    switch (ri(5)) {
    case 0: w = s->w; h = s->h; break;
    case 1: w = rr(1, 2); h = rr(1, 2 * s->h + 1); break;
    case 2: w = rr(1, 2 * s->w + 1); h = rr(1, 2); break;
    case 3: w = rr(1, 3); h = rr(1, 3); break;
    default: w = rr(1, (int)(1.8 * (s1 - s0)) + 3); h = rr(1, (int)(1.8 * (t1 - t0)) + 3); break;
    }
    if (w > 40000) w = 40000;
    if (h > 40000) h = 40000;
    cx = (int)floor(s0 + (s1 - s0) * (ru() * 1.6 - 0.3)); cy = (int)floor(t0 + (t1 - t0) * (ru() * 1.6 - 0.3));
    op->l = cx - w / 2; op->r = op->l + w; op->t = cy - h / 2; op->b = op->t + h;
    op->dir = chance(30) ? -1 : 1;
    op->col0 = (op->dir > 0 ? -op->l : op->r - 1) + (chance(30) ? rr(-6, 6) : chance(5) ? rr(-s->w * 2, s->w * 2) : 0);
    op->row0 = -op->t + (chance(30) ? rr(-6, 6) : chance(5) ? rr(-s->h * 2, s->h * 2) : 0);
    switch (ri(20)) {
    case 0: op->kind = ROP_FILL; op->index = (uint8_t)r32(); break;
    case 1: op->kind = ROP_GRID; op->mode = 1; op->lut = lut; break;
    case 2: op->kind = ROP_GRID; op->mode = 2 + ri(2); break;
    case 3: case 4: case 5: case 6: op->kind = ROP_GRID; op->mode = 0; break;
    case 7: case 8: case 9: case 10: op->kind = ROP_GRID; op->mode = 1; break;
    default:
        op->kind = ROP_BITMAP; op->keyed = chance(65);
        op->key = chance(40) ? 0 : chance(50) ? s->data[0] : (uint8_t)r32();
        break;
    }
    if (allow_land && op->kind == ROP_BITMAP && chance(60)) {
        /* a land tile: its source column c at land column land_x + c; tile origin at the op's left / top */
        op->dir = 1;
        op->col0 = -op->l; op->row0 = -op->t;
        op->land_x = rr(0, L.w - 1); op->land_y = rr(0, L.h - 1);
        if (chance(15)) { op->land_x = rr(-20, L.w + 20); if (op->land_x < 0) op->land_x = 0; }
    }
}

/* ---- part A ------------------------------------------------------------------------------------------------------ */
static void case_a(void) {
    rview v;
    int W, H, i, use_land = chance(35), nops = rr(1, 4);
    if (chance(30)) W = rr(1, 40); else if (chance(55)) W = rr(40, 400); else W = rr(400, 2600);
    H = rr(1, 60000 / W + 1);
    if (H > 300) H = 300;
    new_palette();
    nsrc = rr(1, MAXSRC);
    for (i = 0; i < nsrc; i++) {
        int sw, sh;
        if (chance(15)) { sw = 1; sh = rr(1, 3); }
        else if (chance(30)) { sw = rr(1, 64); sh = rr(1, 64); }
        else if (chance(50)) { sw = rr(64, 1500); sh = rr(1, 200); }
        else { sw = rr(1, 100); sh = rr(64, 1100); }
        fill_src(&srcs[i], sw, sh);
    }
    make_view(&v, W, H);
    if (use_land) make_land(chance(20));
    njob = 0;
    for (i = 0; i < nops; i++) {
        job *j = &jobs[njob++];
        random_op(&j->op, &v, use_land);
        j->prepared = 0;
    }
    if (use_land) {                         /* (the frame's copy taps: for one of the land ops' offsets) */
        int level;
        v.land = &L;
        level = v.point ? 0 : raster_level(&L, v.z);
        if (level) {
            double offx = 0, offy = 0;
            for (i = 0; i < njob; i++)
                if (jobs[i].op.land_x >= 0) { offx = jobs[i].op.col0 + jobs[i].op.land_x; offy = jobs[i].op.row0 + jobs[i].op.land_y; break; }
            land_taps(&v, level, offx, offy);
            for (i = 0; i < njob; i++)      /* (most land ops the frame's offset, as WA's tiles of one land) */
                if (jobs[i].op.land_x >= 0 && chance(80)) {
                    jobs[i].op.land_x = (int)offx - jobs[i].op.col0;
                    jobs[i].op.land_y = (int)offy - jobs[i].op.row0;
                    if (jobs[i].op.land_x < 0 || jobs[i].op.land_y < 0) jobs[i].op.land_x = jobs[i].op.land_y = -1;
                }
        }
    }
    for (i = 0; i < njob; i++) jobs[i].prepared = raster_prepare(&jobs[i].op, &v, &jobs[i].p);
    make_cuts(H);
    run_jobs(&v, use_land ? "A land" : "A");
    free_view();
    gfree_all();
}

/* ---- part B: arbitrary taps -------------------------------------------------------------------------------------- */
static long corrupted;
static int zero_sum_ok;
static void rand_tap(rtap *t, int kmin, int kmax) {
    int n, k, target, prev = 0, u[RASTER_MAX_TAPS], us = 0;
    double cum = 0;
    for (k = 0; k < RASTER_MAX_TAPS; k++) t->w[k] = (int16_t)r32();                /* (rubbish past n) */
    n = chance(6) ? 0 : chance(60) ? rr(1, 3) : rr(4, RASTER_MAX_TAPS);
    target = chance(65) ? 256 : chance(30) ? rr(200, 256) : ri(257);
    for (k = 0; k < n; k++) { u[k] = chance(12) ? 0 : rr(1, 1000); us += u[k]; }
    if (!us && n) { u[0] = 1; us = 1; }
    for (k = 0; k < n; k++) {
        int w;
        cum += (double)u[k] * target / us;
        w = (int)floor(cum + 0.5) - prev;
        prev += w;
        t->w[k] = (int16_t)w;
    }
    if (!zero_sum_ok && n && t->w[0] < 1) { /* (as tap_one / tap_whole make them: n > 0 -> the first weight >= 1) */
        int j;
        t->w[0] = 1; prev++;
        if (prev > target && target >= 1)
            for (j = 1; j < n; j++) if (t->w[j] > 0) { t->w[j]--; prev--; break; }
    }
    t->n = (int16_t)n;
    t->sum = (int16_t)prev;
    t->k0 = chance(3) ? rr(-100000, 100000) : rr(kmin, kmax);
    if (chance(2)) {                                                             /* (a tap the fast rows must refuse) */
        corrupted++;
        switch (ri(5)) {
        case 0: t->sum = (int16_t)(t->sum + (chance(50) ? 1 : -1)); break;
        case 1: if (n) t->w[ri(n)] = (int16_t)-rr(1, 50); break;
        case 2: if (n) { t->w[ri(n)] = (int16_t)rr(257, 3000); t->sum = 256; } break;
        case 3: t->n = (int16_t)(RASTER_MAX_TAPS + 1); break;
        default: if (n) { t->w[0] = (int16_t)(t->w[0] + 10); t->sum = (int16_t)(t->sum + 10); } break;  /* (> 256?) */
        }
        if (t->n > RASTER_MAX_TAPS) t->n = RASTER_MAX_TAPS;                       /* (scalar reads w[n - 1] too) */
    }
}

static void case_b(void) {
    rview v;
    int W, H, i, X, level = 0, nops = rr(1, 3);
    const int mipcase = chance(40);
    if (chance(30)) W = rr(1, 40); else if (chance(55)) W = rr(40, 400); else W = rr(400, 2600);
    H = rr(1, 30000 / W + 1);
    if (H > 200) H = 200;
    new_palette();
    nsrc = rr(1, MAXSRC);
    for (i = 0; i < nsrc; i++) fill_src(&srcs[i], chance(20) ? rr(1, 4) : rr(1, 1200), chance(20) ? rr(1, 4) : rr(1, 150));
    make_view(&v, W, H);
    if (mipcase) {
        make_land(chance(30));
        v.land = &L;
        level = rr(1, L.levels);
        if (!L.mip[level]) level = 0;
    }
    /* the frame's taps replaced by arbitrary ones (level 0, and the copy's level) */
    for (i = 0; i <= level; i = i ? level + 1 : (level ? level : 1)) {
        const int xn = i ? L.mw[i] : 1500, yn = i ? L.mh[i] : 200;
        if (!gxa[i]) { gxa[i] = (rtap *)malloc(sizeof(rtap) * (size_t)(v.x2 - v.x1)); gya[i] = (rtap *)malloc(sizeof(rtap) * (size_t)(v.y2 - v.y1)); }
        {
            int k0 = rr(-20, xn), step = chance(50) ? 1 : rr(0, 9);
            for (X = 0; X < v.x2 - v.x1; X++) {
                rand_tap(&gxa[i][X], -15, xn + 15);
                if (chance(70)) { gxa[i][X].k0 = k0; k0 += chance(90) ? step : rr(-5, 5); }   /* (mostly in order) */
            }
            k0 = rr(-20, yn); step = chance(50) ? 1 : rr(0, 9);
            for (X = 0; X < v.y2 - v.y1; X++) {
                rand_tap(&gya[i][X], -15, yn + 15);
                if (chance(70)) { gya[i][X].k0 = k0; k0 += chance(90) ? step : rr(-5, 5); }
            }
        }
        v.gx[i] = gxa[i]; v.gy[i] = gya[i];
    }
    njob = 0;
    for (i = 0; i < nops; i++) {
        job *j = &jobs[njob++];
        rprep *p = &j->p;
        const src_t *s = &srcs[ri(nsrc)];
        memset(&j->op, 0, sizeof(j->op));
        j->op.land_x = -1;
        j->op.src.data = s->data; j->op.src.w = s->w; j->op.src.h = s->h; j->op.src.stride = s->stride;
        j->op.dir = 1;
        if (chance(70)) { j->op.kind = ROP_BITMAP; j->op.keyed = chance(60); j->op.key = chance(50) ? 0 : (uint8_t)r32(); }
        else { j->op.kind = ROP_GRID; j->op.mode = chance(10) ? 2 + ri(2) : ri(2); if (j->op.mode == 1 && chance(10)) j->op.lut = lut; }
        p->x1 = rr(v.x1, v.x2 - 1); p->x2 = chance(30) ? v.x2 : rr(p->x1 + 1, v.x2);
        p->y1 = rr(v.y1, v.y2 - 1); p->y2 = chance(30) ? v.y2 : rr(p->y1 + 1, v.y2);
        p->level = level && chance(80) ? level : 0;
        if (p->level) {
            j->op.kind = ROP_BITMAP; j->op.land_x = rr(0, 50); j->op.land_y = rr(0, 50);
            p->xbase = 0; p->xdir = 1; p->xn = L.mw[p->level]; p->ybase = 0; p->yn = L.mh[p->level];
            rand_tap(&p->ex[0], -10, p->xn + 10); rand_tap(&p->ex[1], -10, p->xn + 10);
            rand_tap(&p->ey[0], -10, p->yn + 10); rand_tap(&p->ey[1], -10, p->yn + 10);
        } else {
            if (mipcase && j->op.kind == ROP_BITMAP && chance(30)) { j->op.land_x = rr(0, 50); j->op.land_y = rr(0, 50); }
            p->xdir = chance(4) ? (chance(50) ? 0 : 2) : chance(35) ? -1 : 1;
            p->xbase = chance(60) ? (p->xdir < 0 ? s->w - 1 : 0) : rr(-30, s->w + 30);
            p->xn = chance(85) ? s->w : rr(0, s->w);
            p->ybase = chance(60) ? 0 : rr(-30, s->h + 30);
            p->yn = chance(85) ? s->h : rr(0, s->h);
            j->op.dir = p->xdir;
            rand_tap(&p->ex[0], -10, 1500); rand_tap(&p->ex[1], -10, 1500);
            rand_tap(&p->ey[0], -10, 200); rand_tap(&p->ey[1], -10, 200);
            if (chance(60)) { p->ex[0].k0 = gxa[0][p->x1 - v.x1].k0; p->ex[1].k0 = gxa[0][p->x2 - 1 - v.x1].k0; }
        }
        p->gx = v.gx[p->level]; p->gy = v.gy[p->level];
        j->prepared = 1;
    }
    make_cuts(H);
    run_jobs(&v, level ? "B copies" : "B");
    free_view();
    gfree_all();
}

/* ---- part C: WA-like frames -------------------------------------------------------------------------------------- */
static void case_c(void) {
    rview v;
    int W = rr(200, 2600), H, i, px, py, tx, ty, level;
    const int TW = 296, TH = 496, LC = 6, LR = 2, LX0 = rr(-50, 50), LY0 = rr(-50, 50);
    H = rr(4, 90000 / W + 4);
    if (H > 200) H = 200;
    new_palette();
    nsrc = 3;
    fill_src(&srcs[0], 64, 1024);                       /* (sky strip) */
    fill_src(&srcs[1], 64, 65);                         /* (background piece) */
    fill_src(&srcs[2], TW * LC, TH * LR);               /* (the land, its tiles windows of it) */
    for (i = 0; i < TW * LC * TH * LR; i++) {
        uint8_t *d = &srcs[2].data[(i / (TW * LC)) * srcs[2].stride + i % (TW * LC)];
        if (chance(35)) *d = 0;
    }
    make_view(&v, W, H);
    v.bx = LX0 + (TW * LC) * ru(); v.by = LY0 + (TH * LR) * ru();
    if (chance(30)) { v.bx = floor(v.bx); v.by = floor(v.by); }
    raster_axis(gxa[0], v.x1, v.x2, v.ox, v.bx, v.z, 0.0, 0, v.point);
    raster_axis(gya[0], v.y1, v.y2, v.oy, v.by, v.z, 0.0, 0, v.point);
    /* the land's copies made from its pixels, as raster_land_sync makes them */
    memset(&L, 0, sizeof(L));
    L.w = TW * LC; L.h = TH * LR; L.tw = TW; L.th = TH; L.cols = LC; L.rows = LR; L.levels = 3; L.keyed = 1; L.key = 0;
    L.now = (uint8_t *)malloc((size_t)L.w * L.h);
    L.stamp = (uint32_t *)calloc((size_t)LC * LR, sizeof(uint32_t));
    for (i = 1; i <= 3; i++) {
        L.mw[i] = (L.w + (1 << i) - 1) >> i; L.mh[i] = (L.h + (1 << i) - 1) >> i;
        L.mip[i] = (uint32_t *)galloc(sizeof(uint32_t) * (size_t)L.mw[i] * L.mh[i], chance(50));
    }
    for (ty = 0; ty < LR; ty++)
        for (tx = 0; tx < LC; tx++) {
            rsrc t;
            int ph;
            t.data = srcs[2].data + (size_t)ty * TH * srcs[2].stride + (size_t)tx * TW; t.w = TW; t.h = TH; t.stride = srcs[2].stride;
            for (ph = 0; ph < 4; ph++) raster_land_sync(&L, &t, tx * TW, ty * TH, pal, 1, ph, NULL);
        }
    v.land = &L;
    level = v.point ? 0 : raster_level(&L, v.z);
    if (level) land_taps(&v, level, -LX0, -LY0);
    njob = 0;
    {   /* sky strips 64 x 1024 across, background pieces 64 x 65, then the land tiles */
        const double s0 = v.bx + (v.x1 - v.ox) / v.z, s1 = v.bx + (v.x2 - v.ox) / v.z;
        const double t0 = v.by + (v.y1 - v.oy) / v.z, t1 = v.by + (v.y2 - v.oy) / v.z;
        const int sx0 = (int)floor(s0 / 64) * 64 - 64, sy0 = (int)floor(t0 / 65) * 65 - 65;
        for (px = sx0; px < s1 + 64 && njob < MAXJOB - 40; px += 64) {
            job *j = &jobs[njob++];
            memset(&j->op, 0, sizeof(j->op));
            j->op.kind = ROP_BITMAP; j->op.land_x = -1; j->op.dir = 1;
            j->op.src.data = srcs[0].data; j->op.src.w = 64; j->op.src.h = 1024; j->op.src.stride = srcs[0].stride;
            j->op.l = px; j->op.r = px + 64; j->op.t = (int)floor(t0) - 300; j->op.b = j->op.t + 1024;
            j->op.col0 = -j->op.l; j->op.row0 = -j->op.t;
        }
        for (py = sy0; py < t1 + 65 && njob < MAXJOB - 20; py += 65)
            for (px = sx0; px < s1 + 64 && njob < MAXJOB - 20; px += 64) {
                job *j = &jobs[njob++];
                memset(&j->op, 0, sizeof(j->op));
                j->op.kind = ROP_BITMAP; j->op.land_x = -1; j->op.dir = 1; j->op.keyed = chance(10);
                j->op.src.data = srcs[1].data; j->op.src.w = 64; j->op.src.h = 65; j->op.src.stride = srcs[1].stride;
                j->op.l = px; j->op.r = px + 64; j->op.t = py; j->op.b = py + 65;
                j->op.col0 = -j->op.l; j->op.row0 = -j->op.t;
            }
        for (ty = 0; ty < LR; ty++)
            for (tx = 0; tx < LC && njob < MAXJOB; tx++) {
                job *j = &jobs[njob++];
                memset(&j->op, 0, sizeof(j->op));
                j->op.kind = ROP_BITMAP; j->op.keyed = 1; j->op.key = 0; j->op.dir = 1;
                j->op.src.data = srcs[2].data + (size_t)ty * TH * srcs[2].stride + (size_t)tx * TW;
                j->op.src.w = TW; j->op.src.h = TH; j->op.src.stride = srcs[2].stride;
                j->op.l = LX0 + tx * TW; j->op.t = LY0 + ty * TH; j->op.r = j->op.l + TW; j->op.b = j->op.t + TH;
                j->op.col0 = -j->op.l; j->op.row0 = -j->op.t; j->op.land_x = tx * TW; j->op.land_y = ty * TH;
            }
    }
    for (i = 0; i < njob; i++) jobs[i].prepared = raster_prepare(&jobs[i].op, &v, &jobs[i].p);
    make_cuts(H);
    run_jobs(&v, level ? "C frame (copies)" : "C frame");
    free(L.now); free(L.stamp);
    free_view();
    gfree_all();
}

int main(int argc, char **argv) {
    const long na = argc > 1 ? atol(argv[1]) : 6000, nb = argc > 2 ? atol(argv[2]) : 6000, nc = argc > 3 ? atol(argv[3]) : 300;
    long k;
    if (argc > 4) rs ^= (uint64_t)strtoull(argv[4], NULL, 10) * 0x2545F4914F6CDD1Dull;
    for (k = 0; k < na; k++) case_a();
    printf("raster_fuzz: part A %ld cases, %ld differ so far\n", cases, diffs);
    for (k = 0; k < nb; k++) case_b();
    printf("raster_fuzz: part B done, %ld cases, %ld differ so far (%ld corrupted taps)\n", cases, diffs, corrupted);
    if (argc > 5) {                         /* (taps of n > 0 whose weights add up to 0: tap_one never makes them) */
        const long d0 = diffs, r0 = diffs_rgb, c0 = cases, nb0 = atol(argv[5]);
        zero_sum_ok = 1;
        for (k = 0; k < nb0; k++) case_b();
        zero_sum_ok = 0;
        printf("raster_fuzz: part B0 (zero-sum taps allowed): %ld cases, %ld differ (%ld in RGB, the rest the 4th byte "
               "only) -- not counted below\n", cases - c0, diffs - d0, diffs_rgb - r0);
        diffs = d0; diffs_rgb = r0;
    }
    for (k = 0; k < nc; k++) case_c();
    printf("raster_fuzz: %ld cases, %ld differ (%ld in RGB), %ld pixels changed in all, %ld misaligned calls\n", cases,
           diffs, diffs_rgb, fast_px, mis_calls);
    (void)crashes_avoided;
    return diffs ? 1 : 0;
}
