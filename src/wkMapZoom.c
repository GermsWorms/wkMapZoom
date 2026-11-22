/* wkMapZoom: hold Ctrl and turn the mouse wheel to zoom the map in and out in Worms Armageddon 3.8.1 (a WormKit module).
 * The HUD, the chat, the Esc menu and the weapon panel stay their normal size; a small "150%" shows while you zoom.
 *
 * How it works. Each frame WA's GameRender (00533DC0) has its tasks queue what they draw (the drawing queue, world
 * +0x524) and then draws the queue back to front (RenderDrawingQueue 00542350). Entries of kinds 1, 4 and 7 are placed
 * on the screen (the HUD); every other kind goes through the camera (the world: sky, land, water, worms, weapons,
 * names). While zoomed, this module has WA draw the queue one entry at a time (the split wkVista makes, MIT,
 * github.com/ropahektic/wkVista): for the world entries the display's frame grid, size, origin and clip point at a
 * bigger buffer of the module's own, so WA draws -- at its own scale, with its own code -- the whole area the zoomed
 * view needs (the renderer's frame pointer and size too, which WA's fills and land tiles draw into); at the first HUD
 * entry the module scales that area onto WA's frame (Filter=); WA's big rectangle fills (sky, water) and the buffer's
 * clearing are done by helper threads meanwhile. Everything on the map zooms with it, worm names and the turn arrow
 * included. Everything else is WA's own, at the game resolution: the HUD entries (team bars, timer, wind), the chat, the
 * Esc menu and the weapon panel (drawn after the queue). WA's camera clamp (00542F10) is given the size the zoomed view sees, so the camera reaches the level's edges
 * zoomed in. The readout is drawn into WA's frame at the end of GameRender, in WA's own palette. Only what is shown
 * changes -- no game state, nothing in WA's display or renderer setup -- so online games and replays stay in step, any
 * renderer works and a resolution change in game is WA's own (the module follows the new size the next frame).
 *
 * Hooks (WA 3.8.1 only: exact SHA-256 of WA.exe and the original bytes checked at each hook point):
 *   00533DC0 GameRender (ECX the runtime; ret): the frame's zoom, then the readout
 *   00542350 RenderDrawingQueue (EAX the queue; display, camera; ret 8): world entries into the module's buffer
 *   00542F10 ClampCameraToBounds (EAX view width, ECX bottom; context, view height, left, top, right; ret 0x14)
 *   a WH_GETMESSAGE hook on WA's own thread: Ctrl + wheel (taken out of WA's queue), Ctrl + middle click: back to 100%
 *
 * Settings (all optional) in wkMapZoom.ini beside WA.exe:
 *   [wkMapZoom]
 *   MaxOut=4        how far out: 4 (25%) shows four times the width and height of the map (the module's buffer is 4x
 *                   the game resolution each way; WA draws up to 16x the map pixels zoomed all the way out)
 *   MaxIn=8         how far in (800%)
 *   Step=1.15       zoom per wheel notch
 *   Smooth=1        ease to the new zoom (0: jump at once)
 *   Filter=smooth   how the zoomed map is scaled: smooth (each screen pixel the average of the map pixels it covers,
 *                   in WA's palette: clear zoomed out, even pixels zoomed in) or nearest (plain pixel picking)
 *   Indicator=zooming  the readout ("150%"): shown while zooming, then fades ("always": stays while not at 100%;
 *                   "off": never)
 *   IndicatorPlace=top-right   or top-left, top, bottom-right, bottom-left, bottom
 * Log: %LOCALAPPDATA%\wkMapZoom\wkMapZoom.log.
 *
 * Test mode (environment WKMAPZOOM_TEST=1, for an isolated copy of WA only): the in-memory options become windowed
 * 960x540 on Direct3D 9 -- the registry is not written -- and a scripted zoom runs (100% -> farthest out -> closest in
 * -> 100%) with WA's frame saved as BMP files (WKMAPZOOM_DUMP, else %LOCALAPPDATA%\wkMapZoom\test); WKMAPZOOM_TEST_RES=WxH
 * sets the window size. MinHook (third_party/minhook) is BSD licensed. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "MinHook.h"
#include "image_guard.h"
#include "zoom_math.h"
#include "zoom_badge.h"

#ifndef WM_MOUSEWHEEL
#define WM_MOUSEWHEEL 0x020a
#endif
#ifndef WHEEL_DELTA
#define WHEEL_DELTA 120
#endif
#ifndef GET_KEYSTATE_WPARAM
#define GET_KEYSTATE_WPARAM(w) (LOWORD(w))
#endif
#ifndef GET_WHEEL_DELTA_WPARAM
#define GET_WHEEL_DELTA_WPARAM(w) ((short)HIWORD(w))
#endif

#define WA_BASE 0x400000u
#define GAME_RENDER_VA 0x533dc0u
#define QUEUE_VA 0x542350u
#define FILL_VA 0x56b810u                /* the display rectangle fill (ECX display; x1, y1, x2, y2 from its origin,
                                           colour; ret 0x14) */
#define CLAMP_VA 0x542f10u
#define QSORT_VA 0x5d7890u              /* WA's qsort (cdecl) */
#define QUEUE_ORDER_VA 0x542330u        /* the queue's order (by layer: +4) */
#define DISPLAY_VTABLE_VA 0x66a218u     /* the game display (DDDisplay) */
#define FRAME_PTR_VA 0x7a0eecu          /* the renderer's 8-bit frame: its pixels, width (= row stride), height. The display
                                           takes its grid from these at each lock (0056A370), and its fills (0056B810 ->
                                           005A25C0) and land tiles draw into them directly */
#define FRAME_W_VA 0x7a0ef0u
#define FRAME_H_VA 0x7a0ef4u
#define OPT_DISPLAY_X_VA 0x88e3b8u      /* the options as read at start-up (004C8440, before the modules load) */
#define OPT_DISPLAY_Y_VA 0x88e3bcu
#define OPT_WINDOW_X_VA 0x88e3c0u
#define OPT_WINDOW_Y_VA 0x88e3c4u
#define OPT_RENDERER_VA 0x88e414u
#define OPT_WINDOWED_VA 0x88e42cu

#define RT_WORLD 0x488                  /* GameRender's runtime: +0x488 the world */
#define WO_DISPLAY 0x4                  /* the world: +4 the display */
#define D_W 0x3548                      /* the display: its size, clip (x1, y1, x2, y2), origin, palette, frame grid */
#define D_H 0x354c
#define D_CLIP 0x3550
#define D_ORG 0x3560
#define D_PALETTE 0x358d                /* 256 x R, G, B, flags */
#define D_GRID 0x3d9c
#define GR_DATA 0x08                    /* the frame grid (BitGrid): its pixels, row stride, size, clip */
#define GR_STRIDE 0x10
#define GR_W 0x14
#define GR_H 0x18
#define GR_CLIP 0x1c
#define Q_COUNT 0x10004                 /* the drawing queue: its count and its entries (each: kind, layer, ...) */
#define Q_ENTRIES 0x10008
#define MAX_ENTRIES 16384

#define VA(a) ((uintptr_t)(base + ((uintptr_t)(a) - WA_BASE)))
#define FIELD(p, off) (*(int32_t *)((char *)(p) + (off)))
#define VTBL(obj) (*(void ***)(obj))

static uintptr_t base;
static FILE *log_file;
static int installed, test_mode;
static char dump_dir[MAX_PATH];
static HINSTANCE self;
static zoom_cfg cfg = ZOOM_CFG_DEFAULT;

/* the game resolution S (the display's own size), the module's world buffer G */
static int sw, sh, gw, gh;
static uint8_t *wbuf;
static int *colmap;                     /* screen column -> buffer column, this frame */
#define MAX_TAPS 10                     /* buffer pixels one screen pixel covers, each way (1 / zoom + 1, and 2) */
typedef struct { int n, i[MAX_TAPS]; uint16_t w[MAX_TAPS]; } tap;   /* buffer pixels and their weights (sum 256) */
static tap *coltaps, *rowtaps;
static uint32_t *lut;                   /* (palette generation << 8) | index, by 15-bit colour */
static uint32_t lut_gen = 1;
static uint8_t pal_rgb[256][3], lut_pal[1024];   /* (lut_pal: WA's palette as the table was made for it) */
static int filter_smooth = 1;          /* Filter=smooth (else nearest) */
static double zoom_now = 1.0, zoom_goal = 1.0, zoom_lo = 1.0, zoom_hi = 1.0;
static double frame_zoom = 1.0;         /* the zoom this frame is drawn at (fixed at GameRender's start) */
static void *frame_display, *last_world;
static LARGE_INTEGER clock_freq, clock_last;
static DWORD live_tick;                 /* GetTickCount of the last GameRender */
static unsigned frames;                 /* GameRenders this game */
static HHOOK msg_hook;
static unsigned logged_split;

/* the readout */
enum { IND_OFF, IND_ZOOMING, IND_ALWAYS };
static int ind_mode = IND_ZOOMING;
enum { PLACE_TOP_RIGHT, PLACE_TOP_LEFT, PLACE_TOP, PLACE_BOTTOM_RIGHT, PLACE_BOTTOM_LEFT, PLACE_BOTTOM };
static int ind_place = PLACE_TOP_RIGHT;
static DWORD ind_changed;               /* GetTickCount of the last zoom change */
static uint32_t *ind_img;               /* the readout's picture (ARGB, ind_tw x ind_th) for ind_pct */
static int ind_tw, ind_th, ind_bw, ind_bh, ind_pct = -1, ind_lines;
static uint8_t pal_seen[1024], dark_lut[256];
static int white_index = -1;

static void (__fastcall *original_render)(void *);
void *orig_queue, *orig_clamp;          /* (read by the thunks below) */

static void say(const char *fmt, ...) {
    va_list ap;
    SYSTEMTIME t;
    if (!log_file) return;
    GetLocalTime(&t);
    fprintf(log_file, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_start(ap, fmt);
    vfprintf(log_file, fmt, ap);
    va_end(ap);
    fputc('\n', log_file);
    fflush(log_file);
}

/* ---- the thunks: WA's own conventions <-> C ---------------------------------------------------------------------- */
int __cdecl queue_hook(uint8_t *queue, void *display, void *camera);
void __cdecl clamp_hook(uint32_t *regs);
void thunk_queue(void);
void thunk_clamp(void);
int __cdecl call_queue(void *queue, void *display, void *camera);
__asm__(
    ".text\n"
    /* RenderDrawingQueue's entry: EAX the queue, [esp+4] the display, [esp+8] the camera; ret 8 */
    ".globl _thunk_queue\n"
    "_thunk_queue:\n"
    "    pushl 8(%esp)\n"
    "    pushl 8(%esp)\n"
    "    pushl %eax\n"
    "    call _queue_hook\n"
    "    addl $12, %esp\n"
    "    ret $8\n"
    /* WA's own RenderDrawingQueue called from C: call_queue(queue, display, camera) */
    ".globl _call_queue\n"
    "_call_queue:\n"
    "    pushl %ebx\n"
    "    pushl %esi\n"
    "    pushl %edi\n"
    "    pushl %ebp\n"
    "    movl 20(%esp), %eax\n"
    "    pushl 28(%esp)\n"
    "    pushl 28(%esp)\n"
    "    call *_orig_queue\n"
    "    popl %ebp\n"
    "    popl %edi\n"
    "    popl %esi\n"
    "    popl %ebx\n"
    "    ret\n"
    /* ClampCameraToBounds: the registers and arguments handed to clamp_hook (which may change EAX and the view
     * height), then WA's own with them */
    ".globl _thunk_clamp\n"
    "_thunk_clamp:\n"
    "    pushal\n"
    "    pushl %esp\n"
    "    call _clamp_hook\n"
    "    addl $4, %esp\n"
    "    popal\n"
    "    jmp *_orig_clamp\n");

/* ---- the zoom ---------------------------------------------------------------------------------------------------- */
static void set_goal(double z, const char *why) {
    double before = zoom_goal;
    zoom_goal = zoom_clamp(z, zoom_lo, zoom_hi);
    if (zoom_goal != before) say("zoom %.0f%% (%s)", zoom_goal * 100.0, why);
    ind_changed = GetTickCount();
    if (!cfg.smooth) zoom_now = zoom_goal;
}

static void advance(void) {
    LARGE_INTEGER now;
    double dt;
    QueryPerformanceCounter(&now);
    dt = clock_last.QuadPart && clock_freq.QuadPart ? (double)(now.QuadPart - clock_last.QuadPart) / clock_freq.QuadPart : 0.0;
    clock_last = now;
    zoom_now = cfg.smooth ? zoom_ease(zoom_now, zoom_goal, dt) : zoom_goal;
}

/* the game resolution seen (a new game, or a resolution change): the world buffer and the zoom range for it */
static int fit_size(int w, int h) {
    int nw, nh;
    if (w < 64 || h < 64 || w > 16384 || h > 16384) return 0;
    if (w == sw && h == sh && wbuf) return 1;
    zoom_frame(w, h, cfg.max_out, &nw, &nh);
    free(wbuf); free(colmap); free(coltaps); free(rowtaps);
    wbuf = (uint8_t *)malloc((size_t)nw * nh);
    colmap = (int *)malloc(sizeof(int) * (size_t)w);
    coltaps = (tap *)malloc(sizeof(tap) * (size_t)w);
    rowtaps = (tap *)malloc(sizeof(tap) * (size_t)h);
    if (!lut) lut = (uint32_t *)calloc(32768, sizeof(uint32_t));
    if (!wbuf || !colmap || !coltaps || !rowtaps) {
        free(wbuf); free(colmap); free(coltaps); free(rowtaps);
        wbuf = NULL; colmap = NULL; coltaps = rowtaps = NULL; sw = sh = 0;
        say("no memory for a %dx%d world buffer: no zoom", nw, nh);
        return 0;
    }
    sw = w; sh = h; gw = nw; gh = nh;
    zoom_limits(sw, sh, gw, gh, &cfg, &zoom_lo, &zoom_hi);
    zoom_goal = zoom_clamp(zoom_goal, zoom_lo, zoom_hi);
    zoom_now = zoom_clamp(zoom_now, zoom_lo, zoom_hi);
    ind_pct = -1;
    say("game %dx%d: world buffer %dx%d, zoom %.0f%% .. %.0f%%", sw, sh, gw, gh, zoom_lo * 100.0, zoom_hi * 100.0);
    return 1;
}

/* ---- the drawing queue: the world through the module's buffer ---------------------------------------------------- */
typedef struct {
    int32_t w, h, clip[4], org[2];          /* the display's */
    int32_t data, stride, gw, gh, gclip[4]; /* its frame grid's */
    int32_t fb, fbw, fbh;                   /* the renderer's frame */
} view_state;

static void view_save(void *d, view_state *s) {
    char *g = *(char **)((char *)d + D_GRID);
    int k;
    s->w = FIELD(d, D_W); s->h = FIELD(d, D_H);
    for (k = 0; k < 4; k++) s->clip[k] = FIELD(d, D_CLIP + 4 * k);
    s->org[0] = FIELD(d, D_ORG); s->org[1] = FIELD(d, D_ORG + 4);
    s->data = FIELD(g, GR_DATA); s->stride = FIELD(g, GR_STRIDE); s->gw = FIELD(g, GR_W); s->gh = FIELD(g, GR_H);
    for (k = 0; k < 4; k++) s->gclip[k] = FIELD(g, GR_CLIP + 4 * k);
    s->fb = *(int32_t *)VA(FRAME_PTR_VA); s->fbw = *(int32_t *)VA(FRAME_W_VA); s->fbh = *(int32_t *)VA(FRAME_H_VA);
}

static void view_load(void *d, const view_state *s) {
    char *g = *(char **)((char *)d + D_GRID);
    int k;
    FIELD(d, D_W) = s->w; FIELD(d, D_H) = s->h;
    for (k = 0; k < 4; k++) FIELD(d, D_CLIP + 4 * k) = s->clip[k];
    FIELD(d, D_ORG) = s->org[0]; FIELD(d, D_ORG + 4) = s->org[1];
    FIELD(g, GR_DATA) = s->data; FIELD(g, GR_STRIDE) = s->stride; FIELD(g, GR_W) = s->gw; FIELD(g, GR_H) = s->gh;
    for (k = 0; k < 4; k++) FIELD(g, GR_CLIP + 4 * k) = s->gclip[k];
    *(int32_t *)VA(FRAME_PTR_VA) = s->fb; *(int32_t *)VA(FRAME_W_VA) = s->fbw; *(int32_t *)VA(FRAME_H_VA) = s->fbh;
}

/* ---- the world onto WA's frame, scaled --------------------------------------------------------------------------- */
/* Filter=smooth (the default): each screen pixel is the average colour of the world pixels it covers (exact coverage:
 * up to 3 x 3 of them zoomed out, 2 x 2 zoomed in), as the nearest colour of WA's palette -- zoomed out nothing is
 * skipped (thin lines and the rope stay), zoomed in every world pixel shows the same size. A pixel whose world pixels
 * are all one colour keeps that exact colour. Filter=nearest: the world pixel under each screen pixel's centre. Rows
 * are shared out between a few threads (WA's own thread waits for them). */
static struct { const uint8_t *src; uint8_t *dst; int stride, cx1, cx2, cy1; double z; int rw, rh, bx, by, ox, oy; } job;

static void make_taps(tap *t, int p0, int p1, int o, int b, double z, int r) {
    double inv = 1.0 / z;
    int p, k;
    for (p = p0; p < p1; p++, t++) {
        double s0 = b + (p - o) * inv, s1 = s0 + inv, wsum = 0, wk[MAX_TAPS];
        int i0 = (int)floor(s0), n = (int)ceil(s1) - i0, left = 256;
        if (n < 1) n = 1;
        if (n > MAX_TAPS) n = MAX_TAPS;
        for (k = 0; k < n; k++) {
            double a = i0 + k > s0 ? i0 + k : s0, e = i0 + k + 1 < s1 ? i0 + k + 1 : s1;
            wk[k] = e > a ? e - a : 0.0;
            wsum += wk[k];
        }
        if (wsum <= 0) { wk[0] = wsum = 1.0; }
        t->n = 0;
        for (k = 0; k < n; k++) {           /* (pixels it barely touches dropped: weight 0 after rounding) */
            int q = i0 + k, w = k < n - 1 ? (int)floor(wk[k] / wsum * 256.0 + 0.5) : left;
            if (w > left) w = left;
            left -= w;
            if (w <= 0) continue;
            t->i[t->n] = q < 0 ? 0 : q >= r ? r - 1 : q;
            t->w[t->n] = (uint16_t)w;
            t->n++;
        }
        if (!t->n) { t->i[0] = i0 < 0 ? 0 : i0 >= r ? r - 1 : i0; t->w[0] = 256; t->n = 1; }
    }
}

static int pal_nearest(const uint8_t *pal, int r, int g, int b);

static uint8_t quantize(int r, int g, int b) {
    uint32_t key = (uint32_t)((r >> 3) << 10 | (g >> 3) << 5 | (b >> 3)), e = lut[key];
    int idx;
    if ((e >> 8) == lut_gen) return (uint8_t)e;
    idx = pal_nearest(lut_pal, r, g, b);
    lut[key] = (lut_gen << 8) | (uint32_t)idx;   /* (one aligned word: safe to fill from several threads) */
    return (uint8_t)idx;
}

static void rows_smooth(int y0, int y1) {
    int y, x, ky, kx;
    for (y = y0; y < y1; y++) {
        const tap *rt = &rowtaps[y - job.cy1];
        const uint8_t *row[MAX_TAPS];
        const int ny = rt->n;
        uint8_t *dst = job.dst + (size_t)y * job.stride;
        for (ky = 0; ky < ny; ky++) row[ky] = job.src + (size_t)rt->i[ky] * gw;
        for (x = job.cx1; x < job.cx2; x++) {
            const tap *ct = &coltaps[x - job.cx1];
            const int nx = ct->n, c0 = ct->i[0];
            const uint8_t first = row[0][c0];
            uint32_t r = 0, g = 0, b = 0;
            /* most screen pixels cover one colour only (sky, water, land): a byte compare each, no colour sums */
            for (ky = 0; ky < ny; ky++) {
                const uint8_t *rr = row[ky];
                for (kx = 0; kx < nx; kx++) if (rr[ct->i[kx]] != first) goto mixed;
            }
            dst[x] = first;
            continue;
        mixed:
            for (ky = 0; ky < ny; ky++) {
                const uint8_t *rr = row[ky];
                const uint32_t wy = rt->w[ky];
                for (kx = 0; kx < nx; kx++) {
                    const uint8_t *c = pal_rgb[rr[ct->i[kx]]];
                    const uint32_t w = wy * ct->w[kx];
                    r += c[0] * w; g += c[1] * w; b += c[2] * w;
                }
            }
            dst[x] = quantize((int)(r >> 16), (int)(g >> 16), (int)(b >> 16));
        }
    }
}

static void rows_nearest(int y0, int y1) {
    int y, x;
    for (y = y0; y < y1; y++) {
        const uint8_t *src = job.src + (size_t)zoom_src(y, job.oy, job.by, job.z, job.rh) * gw;
        uint8_t *dst = job.dst + (size_t)y * job.stride;
        for (x = job.cx1; x < job.cx2; x++) dst[x] = src[colmap[x]];
    }
}

static void rows(int y0, int y1) { if (filter_smooth) rows_smooth(y0, y1); else rows_nearest(y0, y1); }

/* a rectangle of one colour in the world buffer (WA's fills while the map is drawn, and the buffer's clearing) */
static struct { int x1, x2; uint8_t colour; } fill;
static void fill_rows(int y0, int y1) {
    int y;
    for (y = y0; y < y1; y++) memset(wbuf + (size_t)y * gw + fill.x1, fill.colour, (size_t)(fill.x2 - fill.x1));
}

/* the helpers: started at the first zoomed frame, each waits for its band of rows */
#define MAX_HELPERS 15
static int helpers = -1;
static HANDLE helper_go[MAX_HELPERS], helper_done[MAX_HELPERS];
static int helper_y0[MAX_HELPERS], helper_y1[MAX_HELPERS];
static void (*helper_work)(int, int);

static DWORD WINAPI helper(LPVOID arg) {
    int k = (int)(intptr_t)arg;
    for (;;) {
        WaitForSingleObject(helper_go[k], INFINITE);
        helper_work(helper_y0[k], helper_y1[k]);
        SetEvent(helper_done[k]);
    }
    return 0;
}

static void start_helpers(void);

/* rows y0..y1 shared out between this thread and the helpers; small jobs stay on this thread */
static void parallel_rows(void (*work)(int, int), int y0, int y1, long pixels) {
    int k, band, y;
    if (helpers < 0) start_helpers();
    if (helpers <= 0 || pixels < 65536 || y1 - y0 < 2 * (helpers + 1)) { work(y0, y1); return; }
    helper_work = work;
    band = (y1 - y0 + helpers) / (helpers + 1);
    y = y0 + band;                          /* the first band is this thread's own */
    for (k = 0; k < helpers; k++) {
        helper_y0[k] = y < y1 ? y : y1;
        helper_y1[k] = y + band < y1 ? y + band : y1;
        y += band;
        SetEvent(helper_go[k]);
    }
    work(y0, y0 + band < y1 ? y0 + band : y1);
    WaitForMultipleObjects((DWORD)helpers, helper_done, TRUE, INFINITE);
}

static void start_helpers(void) {
    SYSTEM_INFO si;
    int k, want;
    GetSystemInfo(&si);
    want = (int)si.dwNumberOfProcessors - 1;
    if (want > MAX_HELPERS) want = MAX_HELPERS;
    helpers = 0;
    for (k = 0; k < want; k++) {
        HANDLE t;
        helper_go[k] = CreateEventA(NULL, FALSE, FALSE, NULL);
        helper_done[k] = CreateEventA(NULL, FALSE, FALSE, NULL);
        if (!helper_go[k] || !helper_done[k]) break;
        t = CreateThread(NULL, 64 * 1024, helper, (LPVOID)(intptr_t)k, 0, NULL);
        if (!t) break;
        CloseHandle(t);
        helpers++;
    }
    say("%d helper threads for the scaling and fills", helpers);
}

/* the palette as RGB, and the colour table started afresh when WA's palette changed */
static void palette_rgb(void *display) {
    const uint8_t *pal = (const uint8_t *)display + D_PALETTE;
    int k;
    if (!memcmp(lut_pal, pal, sizeof(lut_pal))) return;
    memcpy(lut_pal, pal, sizeof(lut_pal));
    for (k = 0; k < 256; k++) { pal_rgb[k][0] = pal[4 * k]; pal_rgb[k][1] = pal[4 * k + 1]; pal_rgb[k][2] = pal[4 * k + 2]; }
    lut_gen = (lut_gen + 1) & 0xffffff;
    if (!lut_gen) { memset(lut, 0, sizeof(uint32_t) * 32768); lut_gen = 1; }
}

/* the world drawn so far (the buffer's rw x rh with the camera's centre at bx, by) onto WA's frame, scaled */
static void composite(void *display, const view_state *wa, int cx1, int cy1, int cx2, int cy2, double z, int rw, int rh,
                      int bx, int by) {
    int x;
    job.src = wbuf; job.dst = (uint8_t *)(uintptr_t)wa->data; job.stride = wa->stride;
    job.cx1 = cx1; job.cx2 = cx2; job.cy1 = cy1; job.z = z; job.rw = rw; job.rh = rh; job.bx = bx; job.by = by;
    job.ox = wa->org[0]; job.oy = wa->org[1];
    if (filter_smooth && lut && coltaps && rowtaps) {
        palette_rgb(display);
        make_taps(coltaps, cx1, cx2, wa->org[0], bx, z, rw);
        make_taps(rowtaps, cy1, cy2, wa->org[1], by, z, rh);
    } else {
        filter_smooth = 0;
        for (x = cx1; x < cx2; x++) colmap[x] = zoom_src(x, wa->org[0], bx, z, rw);
    }
    parallel_rows(rows, cy1, cy2, (long)(cx2 - cx1) * (cy2 - cy1));
}

/* WA's rectangle fill (sky, water and other fills) while the map is drawn into the world buffer: the same rectangle
 * (from the display's origin, inside its clip), filled by the helpers; otherwise WA's own */
static int world_mode;
static void (__fastcall *original_fill)(void *, int, int, int, int, int, int);
static void __fastcall hook_fill(void *d, int edx, int x1, int y1, int x2, int y2, int colour) {
    int ox, oy, cl0, cl1, cl2, cl3;
    if (!world_mode || d != frame_display || !wbuf) { original_fill(d, edx, x1, y1, x2, y2, colour); return; }
    ox = FIELD(d, D_ORG); oy = FIELD(d, D_ORG + 4);
    cl0 = FIELD(d, D_CLIP); cl1 = FIELD(d, D_CLIP + 4); cl2 = FIELD(d, D_CLIP + 8); cl3 = FIELD(d, D_CLIP + 12);
    if (cl0 < 0) cl0 = 0;
    if (cl1 < 0) cl1 = 0;
    if (cl2 > gw) cl2 = gw;
    if (cl3 > gh) cl3 = gh;
    x1 += ox; x2 += ox; y1 += oy; y2 += oy;
    if (x2 <= cl0 || y2 <= cl1 || x1 >= cl2 || y1 >= cl3) return;
    if (x1 < cl0) x1 = cl0;
    if (y1 < cl1) y1 = cl1;
    if (x2 > cl2) x2 = cl2;
    if (y2 > cl3) y2 = cl3;
    fill.x1 = x1; fill.x2 = x2; fill.colour = (uint8_t)colour;
    parallel_rows(fill_rows, y1, y2, (long)(x2 - x1) * (y2 - y1));
}

static uint32_t queue_list[MAX_ENTRIES];

/* one entry drawn by WA's own RenderDrawingQueue (the queue shown as holding it alone) */
static int draw_entry(uint8_t *queue, void *display, void *camera, uint32_t e) {
    *(uint32_t *)(queue + Q_COUNT) = 1;
    ((uint32_t *)(queue + Q_ENTRIES))[0] = e;
    return call_queue(queue, display, camera);
}

static int is_screen(uint32_t e) {
    int32_t kind = *(int32_t *)(uintptr_t)e;
    return kind == 1 || kind == 4 || kind == 7;
}

/* Zoomed: every map entry (back to front) into the world buffer, that scaled onto WA's frame, then every HUD entry (back
 * to front) as WA draws it. (WA's own order puts the HUD in front of the map; an entry of each kind on one layer -- the
 * turn's arrow shares layer 0 with the HUD -- keeps its place among its own kind.) */
int __cdecl queue_hook(uint8_t *queue, void *display, void *camera) {
    typedef void (__cdecl *qsort_fn)(void *, size_t, size_t, void *);
    view_state wa, world;
    uint32_t n = *(uint32_t *)(queue + Q_COUNT);
    uint32_t *entries = (uint32_t *)(queue + Q_ENTRIES);
    double z = frame_zoom;
    int i, last = 0, rw, rh, bx, by, cx1, cy1, cx2, cy2, nworld = 0, nscreen = 0;
    LARGE_INTEGER t0, t1, t1b, t2;
    if (z == 1.0 || display != frame_display || !wbuf || !n || n > MAX_ENTRIES)
        return call_queue(queue, display, camera);
    QueryPerformanceCounter(&t0);
    view_save(display, &wa);
    cx1 = wa.clip[0]; cy1 = wa.clip[1]; cx2 = wa.clip[2]; cy2 = wa.clip[3];
    if (cx1 < 0) cx1 = 0;
    if (cy1 < 0) cy1 = 0;
    if (cx2 > wa.gw) cx2 = wa.gw;
    if (cy2 > wa.gh) cy2 = wa.gh;
    if (cx2 > sw) cx2 = sw;
    if (cy2 > sh) cy2 = sh;
    if (cx2 <= cx1 || cy2 <= cy1 || !wa.data) return call_queue(queue, display, camera);
    ((qsort_fn)VA(QSORT_VA))(entries, n, 4, (void *)VA(QUEUE_ORDER_VA));   /* WA's own order, as it sorts them */
    memcpy(queue_list, entries, n * 4);
    zoom_region(cx1, cy1, cx2, cy2, wa.org[0], wa.org[1], z, gw, gh, &rw, &rh, &bx, &by);
    world = wa;
    world.w = rw; world.h = rh;
    world.clip[0] = world.clip[1] = 0; world.clip[2] = rw; world.clip[3] = rh;
    world.org[0] = bx; world.org[1] = by;
    world.data = (int32_t)(uintptr_t)wbuf; world.stride = gw; world.gw = rw; world.gh = rh;
    memcpy(world.gclip, world.clip, sizeof(world.clip));
    world.fb = (int32_t)(uintptr_t)wbuf; world.fbw = gw; world.fbh = gh;   /* (WA's fills and land tiles: rows gw apart) */
    fill.x1 = 0; fill.x2 = rw; fill.colour = 0;                           /* the buffer cleared (helpers) */
    parallel_rows(fill_rows, 0, rh, (long)rw * rh);
    view_load(display, &world);
    world_mode = 1;
    for (i = (int)n - 1; i >= 0; i--)       /* the map, back to front */
        if (!is_screen(queue_list[i])) { last = draw_entry(queue, display, camera, queue_list[i]); nworld++; }
    world_mode = 0;
    view_load(display, &wa);
    QueryPerformanceCounter(&t1);
    composite(display, &wa, cx1, cy1, cx2, cy2, z, rw, rh, bx, by);
    QueryPerformanceCounter(&t1b);
    for (i = (int)n - 1; i >= 0; i--)       /* the HUD, back to front */
        if (is_screen(queue_list[i])) { last = draw_entry(queue, display, camera, queue_list[i]); nscreen++; }
    *(uint32_t *)(queue + Q_COUNT) = n;
    memcpy(entries, queue_list, n * 4);
    QueryPerformanceCounter(&t2);
    {   /* the cost, for the log: the map drawn big, the scaling, the HUD */
        static double map_ms, scale_ms, rest_ms, zsum;
        static unsigned count;
        map_ms += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / clock_freq.QuadPart;
        scale_ms += (double)(t1b.QuadPart - t1.QuadPart) * 1000.0 / clock_freq.QuadPart;
        rest_ms += (double)(t2.QuadPart - t1b.QuadPart) * 1000.0 / clock_freq.QuadPart;
        zsum += z;
        if (++count == 120) {
            say("120 zoomed frames (zoom %.0f%% on average): map %.1f ms, scaling %.1f ms, HUD %.1f ms a frame",
                zsum / count * 100.0, map_ms / count, scale_ms / count, rest_ms / count);
            map_ms = scale_ms = rest_ms = zsum = 0; count = 0;
        }
    }
    if (!logged_split) {
        logged_split = 1;
        say("first zoomed frame: %u entries: %d map (into %dx%d), %d HUD; view %d,%d..%d,%d, centre %d,%d", n, nworld, rw, rh,
            nscreen, cx1, cy1, cx2, cy2, wa.org[0], wa.org[1]);
    }
    return last;
}

/* the camera's clamp told the size the zoomed view sees (never more than the level), so it reaches the level's edges */
void __cdecl clamp_hook(uint32_t *regs) {
    /* pushal: edi esi ebp esp ebx edx ecx eax, then the return address and the arguments */
    double z = frame_zoom;
    int32_t width, height, left, top, right, bottom, span_w, span_h;
    if (z == 1.0 || !frame_display) return;
    width = (int32_t)regs[7]; height = (int32_t)regs[10];
    left = (int32_t)regs[11]; top = (int32_t)regs[12]; right = (int32_t)regs[13]; bottom = (int32_t)regs[6];
    span_w = (right - left) >> 16; span_h = (bottom - top) >> 16;
    width = (int32_t)ceil(width / z); height = (int32_t)ceil(height / z);
    if (span_w > 0 && width > span_w) width = span_w;
    if (span_h > 0 && height > span_h) height = span_h;
    regs[7] = (uint32_t)width;
    regs[10] = (uint32_t)height;
}

/* ---- the readout: "150%" in WA's palette ------------------------------------------------------------------------- */
static int pal_nearest(const uint8_t *pal, int r, int g, int b) {
    int k, best = 0;
    long bd = 0x7fffffff;
    for (k = 0; k < 256; k++) {
        long dr = pal[4 * k] - r, dg = pal[4 * k + 1] - g, db = pal[4 * k + 2] - b, d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
        if (d < bd) { bd = d; best = k; }
    }
    return best;
}

/* the palette's whitest colour, and each colour's darker one (the readout's see-through box) */
static void palette_tables(void *display) {
    const uint8_t *pal = (const uint8_t *)display + D_PALETTE;
    int k;
    if (white_index >= 0 && !memcmp(pal, pal_seen, sizeof(pal_seen))) return;
    memcpy(pal_seen, pal, sizeof(pal_seen));
    white_index = pal_nearest(pal, 255, 255, 255);
    for (k = 0; k < 256; k++)
        dark_lut[k] = (uint8_t)pal_nearest(pal, pal[4 * k] * 30 / 100, pal[4 * k + 1] * 30 / 100, pal[4 * k + 2] * 30 / 100);
}

static double ind_alpha(void) {
    DWORD age = GetTickCount() - ind_changed;
    double a;
    if (ind_mode == IND_OFF) return 0.0;
    if (zoom_now != zoom_goal) return 1.0;
    a = age < 1500 ? 1.0 : age < 2000 ? 1.0 - (age - 1500) / 500.0 : 0.0;
    if (ind_mode == IND_ALWAYS && zoom_goal != 1.0 && a < 0.6) a = 0.6;
    return a;
}

static int pow2(int v) { int p = 16; while (p < v) p <<= 1; return p; }

static void draw_readout(void *display) {
    static const uint8_t bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    char *g = *(char **)((char *)display + D_GRID);
    uint8_t *frame = g ? *(uint8_t **)(g + GR_DATA) : NULL;
    int stride = g ? FIELD(g, GR_STRIDE) : 0, fw = g ? FIELD(g, GR_W) : 0, fh = g ? FIELD(g, GR_H) : 0;
    int pct = (int)floor(zoom_now * 100.0 + 0.5), m, x0, y0, x, y, level;
    double a = ind_alpha();
    if (a <= 0.02 || !frame || fw < 64 || fh < 64) return;
    if (pct != ind_pct || fh != ind_lines || !ind_img) {   /* the picture, made again when the number changes */
        int f;
        badge_size(fh, &f, &ind_bw, &ind_bh);
        if (!ind_img || pow2(ind_bw) != ind_tw || pow2(ind_bh) != ind_th) {
            free(ind_img);
            ind_tw = pow2(ind_bw); ind_th = pow2(ind_bh);
            ind_img = (uint32_t *)malloc((size_t)ind_tw * ind_th * 4);
            if (!ind_img) return;
        }
        if (!badge_paint(pct, f, ind_bw, ind_bh, ind_tw, ind_th, ind_img, ind_tw * 4)) return;
        ind_pct = pct; ind_lines = fh;
    }
    palette_tables(display);
    m = fh * 16 / 1080;
    x0 = ind_place == PLACE_TOP_LEFT || ind_place == PLACE_BOTTOM_LEFT ? m
       : ind_place == PLACE_TOP || ind_place == PLACE_BOTTOM ? (fw - ind_bw) / 2 : fw - ind_bw - m;
    y0 = ind_place >= PLACE_BOTTOM_RIGHT ? fh - ind_bh - fh * 120 / 1080 : (fh * 110 / 1080 > 80 ? fh * 110 / 1080 : 80);
    level = (int)(a * 16.0 + 0.5);          /* fading: an ordered dither (WA's frame has no see-through colours) */
    for (y = 0; y < ind_bh; y++) {
        uint8_t *row;
        if (y0 + y < 0 || y0 + y >= fh) continue;
        row = frame + (size_t)(y0 + y) * stride;
        for (x = 0; x < ind_bw; x++) {
            uint32_t c = ind_img[y * ind_tw + x], r, gg, b, mx;
            uint8_t *px;
            if (!(c >> 24) || x0 + x < 0 || x0 + x >= fw || bayer[(x & 3) + (y & 3) * 4] >= level) continue;
            r = (c >> 16) & 0xff; gg = (c >> 8) & 0xff; b = c & 0xff;
            mx = r > gg ? (r > b ? r : b) : (gg > b ? gg : b);
            px = row + x0 + x;
            *px = mx > 128 ? (uint8_t)white_index : dark_lut[*px];
        }
    }
}

/* ---- test mode: WA's frame as a BMP ------------------------------------------------------------------------------ */
static void dump_frame(void *display, const char *name) {
    char *g = *(char **)((char *)display + D_GRID), path[MAX_PATH + 64];
    const uint8_t *frame = g ? *(uint8_t **)(g + GR_DATA) : NULL, *pal = (const uint8_t *)display + D_PALETTE;
    int stride = g ? FIELD(g, GR_STRIDE) : 0, w = g ? FIELD(g, GR_W) : 0, h = g ? FIELD(g, GR_H) : 0, x, y;
    uint8_t hdr[54] = {'B', 'M'}, *row;
    uint32_t rowb, img, fsz, off = 54, isz = 40;
    int32_t hgt;
    uint16_t planes = 1, bpp = 24;
    FILE *f;
    if (!frame || w <= 0 || h <= 0) return;
    rowb = ((uint32_t)w * 3 + 3) & ~3u; img = rowb * h; fsz = 54 + img; hgt = -h;
    snprintf(path, sizeof(path), "%s\\%s.bmp", dump_dir, name);
    if (!(f = fopen(path, "wb"))) return;
    memcpy(hdr + 2, &fsz, 4); memcpy(hdr + 10, &off, 4); memcpy(hdr + 14, &isz, 4); memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &hgt, 4); memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bpp, 2); memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, 54, f);
    row = (uint8_t *)calloc(rowb, 1);
    for (y = 0; row && y < h; y++) {
        for (x = 0; x < w; x++) {
            const uint8_t *p = pal + 4 * frame[(size_t)y * stride + x];
            row[3 * x] = p[2]; row[3 * x + 1] = p[1]; row[3 * x + 2] = p[0];
        }
        fwrite(row, 1, rowb, f);
    }
    free(row);
    fclose(f);
    say("test: saved %s (%dx%d, zoom %.3f)", path, w, h, zoom_now);
}

/* 100% for a moment, out to the farthest, in to the closest, back to 100%; WA's frame saved in each */
static void test_script(void *display) {
    switch (frames) {
    case 190: dump_frame(display, "1-zoom100"); set_goal(zoom_lo, "test script"); break;
    case 260: dump_frame(display, "2-zoomout-readout"); break;
    case 390: dump_frame(display, "3-zoomout"); set_goal(zoom_hi, "test script"); break;
    case 460: dump_frame(display, "4-zoomin-readout"); break;
    case 590: dump_frame(display, "5-zoomin"); set_goal(1.0, "test script"); break;
    case 660: dump_frame(display, "6-zoom100-readout"); break;
    case 900: dump_frame(display, "7-zoom100"); break;
    }
}

/* ---- the frame --------------------------------------------------------------------------------------------------- */
static void __fastcall hook_render(void *runtime) {
    void *world = runtime ? *(void **)((char *)runtime + RT_WORLD) : NULL;
    void *display = world ? *(void **)((char *)world + WO_DISPLAY) : NULL;
    frame_display = NULL;
    frame_zoom = 1.0;
    if (display && (uintptr_t)VTBL(display) == VA(DISPLAY_VTABLE_VA) && fit_size(FIELD(display, D_W), FIELD(display, D_H))) {
        if (world != last_world) {          /* a new game: 100% */
            last_world = world;
            zoom_now = zoom_goal = 1.0;
            frames = 0;
            logged_split = 0;
            clock_last.QuadPart = 0;
            ind_changed = GetTickCount() - 60000;
            say("a new game (world %p)", world);
        }
        frame_display = display;
        frame_zoom = zoom_now;
        live_tick = GetTickCount();
    }
    original_render(runtime);
    if (frame_display) {
        frames++;
        draw_readout(display);
        if (test_mode) test_script(display);   /* (the frame as shown: with the readout) */
        advance();
    }
    frame_display = NULL;
    frame_zoom = 1.0;
}

static LRESULT CALLBACK on_message(int code, WPARAM wp, LPARAM lp) {
    MSG *m = (MSG *)lp;
    if (code == HC_ACTION && wp == PM_REMOVE && m && wbuf && GetTickCount() - live_tick < 1000) {
        if (m->message == WM_MOUSEWHEEL && (GET_KEYSTATE_WPARAM(m->wParam) & MK_CONTROL)) {
            set_goal(zoom_step(zoom_goal, GET_WHEEL_DELTA_WPARAM(m->wParam) / (double)WHEEL_DELTA, cfg.step), "Ctrl + wheel");
            m->message = WM_NULL;
        } else if ((m->message == WM_MBUTTONDOWN || m->message == WM_MBUTTONUP || m->message == WM_MBUTTONDBLCLK) &&
                   (GET_KEYSTATE_WPARAM(m->wParam) & MK_CONTROL)) {
            if (m->message != WM_MBUTTONUP) set_goal(1.0, "Ctrl + middle click");
            m->message = WM_NULL;
        }
    }
    return CallNextHookEx(msg_hook, code, wp, lp);
}

/* ---- start-up ---------------------------------------------------------------------------------------------------- */
static void read_settings(const char *exe) {
    char ini[MAX_PATH], v[64], *slash;
    strcpy(ini, exe);
    slash = strrchr(ini, '\\');
    if (!slash) return;
    strcpy(slash + 1, "wkMapZoom.ini");
#define READ_D(key, field) if (GetPrivateProfileStringA("wkMapZoom", key, "", v, sizeof(v), ini) && v[0]) cfg.field = atof(v)
    READ_D("MaxOut", max_out);
    READ_D("MaxIn", max_in);
    READ_D("Step", step);
#undef READ_D
    if (GetPrivateProfileStringA("wkMapZoom", "Smooth", "", v, sizeof(v), ini) && v[0]) cfg.smooth = atoi(v) != 0;
    if (GetPrivateProfileStringA("wkMapZoom", "Filter", "", v, sizeof(v), ini) && v[0]) filter_smooth = lstrcmpiA(v, "nearest") != 0;
    if (GetPrivateProfileStringA("wkMapZoom", "Indicator", "", v, sizeof(v), ini) && v[0])
        ind_mode = !lstrcmpiA(v, "off") || !strcmp(v, "0") ? IND_OFF : !lstrcmpiA(v, "always") ? IND_ALWAYS : IND_ZOOMING;
    if (GetPrivateProfileStringA("wkMapZoom", "IndicatorPlace", "", v, sizeof(v), ini) && v[0]) {
        static const char *const places[] = {"top-right", "top-left", "top", "bottom-right", "bottom-left", "bottom"};
        unsigned i;
        for (i = 0; i < sizeof(places) / sizeof(places[0]); i++) if (!lstrcmpiA(v, places[i])) ind_place = (int)i;
    }
    zoom_cfg_sane(&cfg);
}

static int runs_without_screen(void) {
    const char *line = GetCommandLineA();
    static const char *const switches[] = {"/getvideo", "/getlog", "/getmap", "/getscheme", "/sanitise", "/sanitize", "/repair"};
    unsigned i;
    for (i = 0; i < sizeof(switches) / sizeof(switches[0]); i++)
        if (line && strstr(line, switches[i])) return 1;
    return 0;
}

static void install(void) {
    static const unsigned char render_bytes[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x83, 0xec, 0x20};
    static const unsigned char queue_bytes[] = {0x83, 0xec, 0x1c, 0x53, 0x55, 0x8b, 0x6c, 0x24, 0x28};
    static const unsigned char clamp_bytes[] = {0xc1, 0xe0, 0x10, 0x99, 0x2b, 0xc2, 0x53};
    static const unsigned char order_bytes[] = {0x8b, 0x44, 0x24, 0x04, 0x8b, 0x08, 0x8b, 0x54, 0x24, 0x08};
    char exe[MAX_PATH], path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof(exe));
    MH_STATUS s;
    uint8_t *render, *queue, *clamp;
    if (GetEnvironmentVariableA("LOCALAPPDATA", path, sizeof(path))) {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        strcat(path, "\\wkMapZoom");
        CreateDirectoryA(path, NULL);
        strcat(path, "\\wkMapZoom.log");
        if (GetFileAttributesExA(path, GetFileExInfoStandard, &fa) && fa.nFileSizeLow > (2u << 20)) DeleteFileA(path);
        log_file = fopen(path, "a");
    }
    if (!n || n >= sizeof(exe) || !wa_supported_image(exe)) { say("not the WA 3.8.1 this module knows: not installed"); return; }
    if (runs_without_screen()) { say("a /get... or repair run: not installed"); return; }
    base = (uintptr_t)GetModuleHandleA(NULL);
    render = (uint8_t *)VA(GAME_RENDER_VA); queue = (uint8_t *)VA(QUEUE_VA); clamp = (uint8_t *)VA(CLAMP_VA);
    if (memcmp(render, render_bytes, sizeof(render_bytes)) && *render != 0xe9) { say("unexpected code at 00533DC0: not installed"); return; }
    if (memcmp(queue, queue_bytes, sizeof(queue_bytes)) && *queue != 0xe9) { say("unexpected code at 00542350: not installed"); return; }
    if (memcmp(clamp, clamp_bytes, sizeof(clamp_bytes)) && *clamp != 0xe9) { say("unexpected code at 00542F10: not installed"); return; }
    if (memcmp((void *)VA(QUEUE_ORDER_VA), order_bytes, sizeof(order_bytes))) { say("unexpected code at 00542330: not installed"); return; }
    if (*render == 0xe9 || *queue == 0xe9 || *clamp == 0xe9)
        say("another module hooks WA's drawing too (wkVista or WormForge's zoom?): use one zoom module at a time");
    read_settings(exe);
    QueryPerformanceFrequency(&clock_freq);
    test_mode = GetEnvironmentVariableA("WKMAPZOOM_TEST", path, sizeof(path)) > 0;
    if (test_mode) {
        DWORD k = GetEnvironmentVariableA("WKMAPZOOM_DUMP", dump_dir, sizeof(dump_dir));
        if (!k || k >= sizeof(dump_dir)) {
            GetEnvironmentVariableA("LOCALAPPDATA", dump_dir, sizeof(dump_dir) - 32);
            strcat(dump_dir, "\\wkMapZoom\\test");
        }
        CreateDirectoryA(dump_dir, NULL);
        {
            int tw = 960, th = 540;
            char res[32];
            if (GetEnvironmentVariableA("WKMAPZOOM_TEST_RES", res, sizeof(res)) && sscanf(res, "%dx%d", &tw, &th) == 2 &&
                (tw < 640 || th < 480 || tw > 7680 || th > 4320)) { tw = 960; th = 540; }
            *(int32_t *)VA(OPT_WINDOWED_VA) = 1;
            *(int32_t *)VA(OPT_DISPLAY_X_VA) = tw; *(int32_t *)VA(OPT_DISPLAY_Y_VA) = th;
            *(int32_t *)VA(OPT_WINDOW_X_VA) = tw; *(int32_t *)VA(OPT_WINDOW_Y_VA) = th;
            *(int32_t *)VA(OPT_RENDERER_VA) = 4;
            say("test mode: windowed %dx%d, scripted zoom, frames to %s", tw, th, dump_dir);
        }
    }
    s = MH_Initialize();
    if (s == MH_OK) s = MH_CreateHook(render, (void *)hook_render, (void **)&original_render);
    if (s == MH_OK) s = MH_CreateHook(queue, (void *)thunk_queue, &orig_queue);
    if (s == MH_OK) s = MH_CreateHook(clamp, (void *)thunk_clamp, &orig_clamp);
    if (s == MH_OK) {   /* the fills: only on WA 3.8.1's own code (else WA fills, on one thread, as ever) */
        static const unsigned char fill_bytes[] = {0x8b, 0x81, 0x60, 0x35, 0x00, 0x00, 0x01, 0x44, 0x24, 0x04};
        uint8_t *fillp = (uint8_t *)VA(FILL_VA);
        if (memcmp(fillp, fill_bytes, sizeof(fill_bytes)) ||
            MH_CreateHook(fillp, (void *)hook_fill, (void **)&original_fill) != MH_OK) say("the fills stay WA's own");
    }
    if (s == MH_OK) s = MH_EnableHook(MH_ALL_HOOKS);
    installed = s == MH_OK;
    if (installed) {
        msg_hook = SetWindowsHookExA(WH_GETMESSAGE, on_message, self, GetCurrentThreadId());
        if (!msg_hook) say("could not watch the mouse wheel (error %lu)", GetLastError());
    }
    say("wkMapZoom %s (base %08x): Ctrl + wheel zooms the map (up to %.0f%% out, %.0f%% in), Ctrl + middle click 100%%",
        installed ? "ready" : "failed", (unsigned)base, 100.0 / cfg.max_out, cfg.max_in * 100.0);
}

BOOL WINAPI DllMain(HINSTANCE dll, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) { self = dll; DisableThreadLibraryCalls(dll); install(); }
    else if (reason == DLL_PROCESS_DETACH) {
        if (msg_hook) UnhookWindowsHookEx(msg_hook);
        if (log_file) fclose(log_file);
    }
    return TRUE;
}
