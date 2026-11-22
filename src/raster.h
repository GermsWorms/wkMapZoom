/* raster.h: WA's map drawing operations drawn straight onto the zoomed screen (wkMapZoom).
 *
 * The operations, and what each does to a pixel, are wkVista's (MIT, Copyright (c) 2026 ropahektic,
 * github.com/ropahektic/wkVista, src/raster.rs, checked there pixel for pixel against WA's own functions). wkVista
 * shows each screen pixel as the one map pixel it maps back to; here each screen pixel gets the AVERAGE of the map
 * pixels it covers, in RGB, so zoomed out nothing is skipped (a 1-pixel wall stays visible as a fainter line) and zoomed
 * in every map pixel shows the same size. Operations are laid one over another as WA draws them (each one's coverage
 * of a screen pixel blends it over what is there). The land is drawn from pre-shrunk copies (1/2, 1/4, 1/8 size) kept
 * up to date with the land's changes, so zoomed out the land costs about what it costs at 100%.
 *
 * Coordinates: WA draws into a "region" (the area the zoomed view needs, in WA's own pixels); output pixel (X, Y)
 * covers region [bx + (X - ox) / z, bx + (X + 1 - ox) / z) x [by + (Y - oy) / z, by + (Y + 1 - oy) / z). Which region
 * pixels (or land copy texels) each output column and row covers -- its "taps" -- is worked out once a frame for the
 * whole output (raster_axis); an operation only works out its own edge columns and rows.
 * Nothing here knows about Windows or WA's memory: tests/raster_test.c checks it against a plain reference. */
#ifndef WKMAPZOOM_RASTER_H
#define WKMAPZOOM_RASTER_H
#include <stdint.h>

#define RASTER_MAX_TAPS 10      /* source pixels one output pixel covers, each way (8 + 1 at 1/8 zoom, and 1 spare) */
#define RASTER_LEVELS 3         /* the land's shrunk copies: 1/2, 1/4, 1/8 */

typedef struct { const uint8_t *data; int w, h, stride; } rsrc;   /* 8-bit pixels: h rows, stride apart, w used */

/* the land: WA keeps it as tiles (bitmaps) of tw x th; `now` is the land as the shrunk copies were made from it */
typedef struct {
    int w, h, tw, th, cols, rows, levels;   /* levels: copies usable (tiles a multiple of 2^levels) */
    uint8_t keyed, key;                     /* the tiles' see-through colour */
    uint8_t *now;                           /* w x h */
    const uint8_t *snap;                    /* the land at the game's start (w x h), with ... */
    const uint32_t *full;                   /* ... its full colour per pixel (0x01RRGGBB; 0: none), or NULL */
    uint32_t *mip[RASTER_LEVELS + 1];       /* level L: mw[L] x mh[L], premultiplied 0xAARRGGBB */
    int mw[RASTER_LEVELS + 1], mh[RASTER_LEVELS + 1];
    uint32_t *stamp;                        /* per tile: the palette generation its copies were made with (0: none) */
} rland;

enum { ROP_FILL, ROP_BITMAP, ROP_GRID };
typedef struct {
    int kind;
    int l, t, r, b;             /* the region pixels drawn: [l, r) x [t, b) (inside the clip) */
    uint8_t index;              /* ROP_FILL: the colour */
    rsrc src;                   /* ROP_BITMAP / ROP_GRID: region pixel (X, Y) shows source column col0 + dir * X, */
    int col0, dir, row0;        /* row row0 + Y; outside the source it reads 0 */
    int keyed;                  /* ROP_BITMAP: source == key is see-through */
    uint8_t key;
    int mode;                   /* ROP_GRID: 0 copy, 1 skip 0 (with lut: lut[dst << 8 | src]), 2 only over non-0, 3 only */
    const uint8_t *lut;         /*   over 0 (dst: the palette colour nearest what is under it) */
    int land_x, land_y;         /* ROP_BITMAP from a land tile: the land pixel of its source (0, 0); land_x < 0: not */
} rop;

/* an output pixel's taps along one axis: texels k0 .. k0 + n - 1 (region pixels, or a land copy's texels), weights in
 * 1/256 of an output pixel (sum: 256 when it is fully covered) */
typedef struct { int16_t n, sum; int32_t k0; int16_t w[RASTER_MAX_TAPS]; } rtap;

typedef struct {
    double z, bx, by;
    int ox, oy;
    int x1, y1, x2, y2;         /* the output: [x1, x2) x [y1, y2) */
    uint32_t *out;              /* 0x00RRGGBB, rows `stride` pixels apart */
    int stride;
    const uint32_t *pal;        /* WA's palette as 0x00RRGGBB */
    uint8_t (*nearest)(uint32_t rgb);   /* the palette colour nearest a colour (blend tables, modes 2 and 3) */
    rland *land;
    uint32_t gen;               /* the palette's generation (the land's copies are made again when it changes) */
    int point;                  /* 1: no averaging, each output pixel the region pixel under its centre (Filter=nearest) */
    const rtap *gx[RASTER_LEVELS + 1], *gy[RASTER_LEVELS + 1];   /* the frame's taps per level, from x1 / y1 (raster_axis) */
    double gox[RASTER_LEVELS + 1], goy[RASTER_LEVELS + 1];       /* the land offsets levels 1.. were made for */
} rview;

typedef struct {
    int x1, x2, y1, y2, level;  /* the output pixels an operation touches; the land copy used */
    int xbase, xdir, xn, ybase, yn;     /* tap texel k -> source column xbase + xdir * k (valid below xn); rows alike */
    rtap ex[2], ey[2];          /* its edge columns (x1, x2 - 1) and rows (y1, y2 - 1) */
    const rtap *gx, *gy;        /* the rest: the frame's */
} rprep;

/* the taps of output pixels [p0, p1) along one axis of a view (o, b, z) over the whole region, at land copy `level`
 * (region coordinate s at texel (s + off) / 2^level; level 0: off 0, region pixels) */
void raster_axis(rtap *t, int p0, int p1, int o, double b, double z, double off, int level, int point);
/* the output pixels `op` touches (0: none); the frame's taps of its level must be in `v` */
int raster_prepare(const rop *op, const rview *v, rprep *p);
/* rows [y0, y1) of it (a band: the rows can be shared out between threads) */
void raster_draw(const rop *op, const rview *v, const rprep *p, int y0, int y1);
/* the land tile `tile` (its source (0, 0) at land (lx, ly)) compared with the land as last seen -- the rows that are
 * `phase` (0..3) mod 4, all of them when the palette changed -- and the changed strips' shrunk copies made again; a
 * change shows within 4 frames of phases 0, 1, 2, 3. Tiles can be done on different threads. */
void raster_land_sync(rland *L, const rsrc *tile, int lx, int ly, const uint32_t *pal, uint32_t gen, int phase);
/* the land copy an output at zoom z is drawn from (0: the tiles themselves) */
int raster_level(const rland *L, double z);

#endif
