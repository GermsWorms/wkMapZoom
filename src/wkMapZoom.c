/* wkMapZoom 6: hold Ctrl and turn the mouse wheel (or numpad + / -) to zoom the map in and out in Worms Armageddon 3.8.1
 * (a WormKit module). An auto zoom (Ctrl + A: out where it opens up, in where it is tight) and a speed zoom (out the
 * faster the worm goes) change the zoom on their own. The HUD, the chat, the Esc menu and the weapon panel stay their
 * normal size; a small "150%" shows while the zoom changes.
 *
 * This file decides the zoom. The picture, zoomed or not, is drawn on the graphics card by gpu.c (wkMapZoom 5 needed a
 * second module for that, wkDirect3D: it is part of this one now). With WA on another renderer than Direct3D 9
 * (shader), or with GraphicsCard=0, WA draws by itself and the map stays at 100%. (Up to 4.3 the module drew the
 * zoomed map itself, on the CPU; it also had a speedometer and a movement line, which are wkSpeedometer now:
 * github.com/GermsWorms/wkSpeedometer.)
 *
 * How it works. Each frame WA's GameRender (00533DC0) has its tasks queue what they draw (the drawing queue, world
 * +0x524) and then draws the queue back to front (RenderDrawingQueue 00542350). Entries of kinds 1, 4 and 7 are placed
 * on the screen (the HUD); every other kind goes through the camera (the map: sky, land, water, worms, weapons, names).
 * While zoomed, the module has WA draw the queue one entry at a time (the split wkVista makes, MIT, Copyright (c) 2026
 * ropahektic, github.com/ropahektic/wkVista). For the map entries the display's size, origin and clip describe the
 * whole area the zoomed view needs (the "region": at 25% four times the screen each way), and the frame WA draws into
 * is one wkDirect3D hands out for the map (wkDirect3D_MapBegin), so WA's own code works out and draws everything that
 * is to be seen there, at WA's own scale. Then wkDirect3D puts that map on the screen at the zoom, each screen pixel
 * the average of the map pixels it covers (wkDirect3D_MapEnd), the display is given back its own size and frame, and WA
 * draws the HUD entries over it as ever. WA's camera clamp (00542F10) is given the size the zoomed view sees, so the
 * camera reaches the level's edges zoomed in, and WA's camera follow (00542F70) the zoomed-in view's size, so the camera
 * keeps up with the worm (at 100% and below both are left as WA has them). Only what is shown changes -- no game state
 * -- so online games and replays stay in step, and other players don't need the module.
 *
 * Hooks (WA 3.8.1 only: exact SHA-256 of WA.exe and the original bytes checked at each hook point):
 *   00533DC0 GameRender (ECX the runtime; ret): the frame's zoom, then the zoom number
 *   00542350 RenderDrawingQueue (EAX the queue; display, camera; ret 8): the map entries, then the HUD entries
 *   00542F10 ClampCameraToBounds (EAX view width, ECX bottom; context, view height, left, top, right; ret 0x14)
 *   00542F70 the camera follow (EAX half the view's width, EDI the camera; half its height, the box it follows), at
 *            WA's two calls of it
 *   00557310 a team task's message handler, 004FCDF0 an object's physics step: whose turn it is and where its worm
 *            is (the auto zoom's and the speed zoom's; they only look)
 *   a WH_GETMESSAGE hook on WA's own thread: Ctrl + wheel, numpad + / - / *, Ctrl + middle click, Ctrl + A (taken out
 *   of WA's queue)
 * Exported: wkMapZoom_View (the zoom and the camera of the frame being drawn, for modules that draw on the map), and
 * wkModSettings / wkModSettingsChanged (what the settings are, and that one was changed: the Mods menu of wkModMenu).
 *
 * Settings (all optional) in wkMapZoom.ini beside WA.exe, section [wkMapZoom]: MaxOut, MaxIn, Step, Smooth, Filter,
 * Indicator, IndicatorPlace, DefaultZoom, AutoZoom, AutoZoomMin, AutoZoomMax, AutoZoomFallSpeed, SpeedZoom,
 * SpeedZoomFrom, SpeedZoomStrength, ZoomModifier, ZoomInKey, ZoomOutKey, ZoomResetKey, AutoZoomKey (see the README; the
 * auto zoom's key writes AutoZoom back).
 * Log: %LOCALAPPDATA%\wkMapZoom\wkMapZoom.log.
 *
 * Test mode (environment WKMAPZOOM_TEST=1, for an isolated copy of WA only): the in-memory options become windowed
 * 960x540 on Direct3D 9 (shader) -- the registry is not written, no setting is saved -- and a scripted zoom runs (100% ->
 * farthest out -> closest in -> 100%); WKMAPZOOM_TEST_RES=WxH sets the window size, WKMAPZOOM_TEST_ZOOM=z one zoom all
 * along (no zoom script), WKMAPZOOM_TEST_AUTOZOOM=1 the auto zoom on, WKMAPZOOM_TEST_SPEEDZOOM=1 the speed zoom on,
 * WKMAPZOOM_TEST_TRACE=1 with them the worm's path (trace.csv) and the land (land-N.pgm) in WKMAPZOOM_DUMP (else
 * %LOCALAPPDATA%\wkMapZoom\test), WKMAPZOOM_TEST_RENDERER=3 another of WA's renderers.
 * MinHook (third_party/minhook) is BSD licensed. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <limits.h>
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

#define VERSION "6.1 beta"

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
#define CLAMP_VA 0x542f10u
#define FILL_VA 0x56b810u               /* the display's rectangle fill (ECX display; x1, y1, x2, y2 from its origin, colour; ret 0x14) */
#define FOLLOW_VA 0x542f70u
#define TEAM_HANDLER_VA 0x557310u       /* a team task's message handler (thiscall, ECX the task) */
#define PHYSICS_VA 0x4fcdf0u            /* an object's physics step (its manager, the object; every object, every step) */
#define WORM_VTABLE_VA 0x6644c8u        /* a worm: +0x2C the game, +0x84 / +0x88 its place (1/65536 px), +0xFC its team,
                                           +0x100 its number in the team */
#define QSORT_VA 0x5d7890u              /* WA's qsort (cdecl) */
#define QUEUE_ORDER_VA 0x542330u        /* the queue's order (by layer: +4) */
#define DISPLAY_VTABLE_VA 0x66a218u     /* the game display (DDDisplay) */
#define FRAME_PTR_VA 0x7a0eecu          /* the renderer's 8-bit frame: its pixels, width (= row stride), height. The display
                                           takes its grid from these at each lock (0056A370), and its fills and bitmap
                                           blits draw into them directly */
#define FRAME_W_VA 0x7a0ef0u
#define FRAME_H_VA 0x7a0ef4u
#define PAL_SHOWN_VA 0x8ac8c8u          /* the palette as shown: 256 x R, G, B, flags (dark while WA fades) */
#define OPT_DISPLAY_X_VA 0x88e3b8u      /* the options as read at start-up (004C8440, before the modules load) */
#define OPT_DISPLAY_Y_VA 0x88e3bcu
#define OPT_WINDOW_X_VA 0x88e3c0u
#define OPT_WINDOW_Y_VA 0x88e3c4u
#define OPT_RENDERER_VA 0x88e414u
#define OPT_WINDOWED_VA 0x88e42cu

#define RT_WORLD 0x488                  /* GameRender's runtime: +0x488 the world */
#define WO_DISPLAY 0x4                  /* the world: +4 the display */
#define D_W 0x3548                      /* the display: its size, clip (x1, y1, x2, y2), origin, frame grid */
#define D_H 0x354c
#define D_CLIP 0x3550
#define D_ORG 0x3560
#define D_GRID 0x3d9c
#define D_TERRAIN 0x4dd8                /* the land: an object whose +4 is its tile bitmaps row by row; the land's size, a */
#define D_LAND_W 0x4ddc                 /* tile's size (wkVista, land.rs) */
#define D_LAND_H 0x4de0
#define D_TILE_W 0x4de4
#define D_TILE_H 0x4de8
#define GR_DATA 0x08                    /* a BitGrid: its pixels, bits a pixel, row stride, size, clip */
#define GR_BPP 0x0c
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

/* the game resolution S (the display's own size) and the largest map region G (the zoomed-out view at its farthest:
 * the frame wkDirect3D is asked for is always this big) */
static int sw, sh, gw, gh, fit_ok;
static int filter_smooth = 1, numpad_keys = 1;
static double zoom_now = 1.0, zoom_goal = 1.0, zoom_lo = 1.0, zoom_hi = 1.0;
static double zoom_default = 1.0;       /* DefaultZoom=: each game starts at it; Ctrl + middle click and numpad * go back
                                           to it (within the zoom range) */
static double frame_zoom = 1.0;         /* the zoom this frame is drawn at (fixed at GameRender's start) */
static double eff_zoom(void);           /* the zoom wanted: the wheel's times the auto zoom's share */
static void *frame_display, *last_world;
static LARGE_INTEGER clock_freq, clock_last;
static DWORD live_tick;                 /* GetTickCount of the last GameRender */
static unsigned frames;                 /* GameRenders this game */
static HHOOK msg_hook;
static unsigned logged_split;

/* the graphics-card drawing: gpu.c's functions (or, with an old wkDirect3D.dll still loaded, that module's; all NULL
 * when neither runs) */
typedef struct { int size; double zoom; int bx, by, ox, oy, x1, y1, x2, y2, smooth; } wkd3d_view;
static struct {
    int (__cdecl *api)(void);
    int (__cdecl *mapmax)(int *, int *);
    void *(__cdecl *begin)(int, int, int, int);
    int (__cdecl *end)(const wkd3d_view *);
    int (__cdecl *overlay)(const uint32_t *, int, int, int, int, int);
} d3d;
static int d3d_looked;                  /* (looked for again at the first game frame: it may load after this module) */
static int gpu_inside;                  /* gpu.c runs (started at load) */
int gpu_start(HINSTANCE module, const char *ini);
void gpu_settings_changed(void);
int __cdecl wkDirect3D_Api(void);
int __cdecl wkDirect3D_MapMax(int *max_w, int *max_h);
void *__cdecl wkDirect3D_MapBegin(int stride, int rows, int rw, int rh);
int __cdecl wkDirect3D_MapEnd(const wkd3d_view *view);
int __cdecl wkDirect3D_Overlay(const uint32_t *argb, int w, int h, int pitch_bytes, int x, int y);
static int zoom_ready;                  /* this frame wkDirect3D can draw a zoomed map */
static unsigned zoomed_frames, refused_frames;

/* what wkMapZoom_View hands out: the frame's view as drawn */
typedef struct { int size; double zoom; int org_x, org_y, cam_x, cam_y; } wkmz_view;
static wkmz_view view_now;
static int view_ok;

/* the readout */
enum { IND_OFF, IND_ZOOMING, IND_ALWAYS };
static int ind_mode = IND_ZOOMING;
enum { PLACE_TOP_RIGHT, PLACE_TOP_LEFT, PLACE_TOP, PLACE_BOTTOM_RIGHT, PLACE_BOTTOM_LEFT, PLACE_BOTTOM };
static int ind_place = PLACE_TOP_RIGHT;
static DWORD ind_changed;               /* GetTickCount of the last zoom change */
static uint32_t *ind_img, *ind_pre;     /* the readout's picture (ARGB, ind_tw x ind_th) for ind_text; the same faded and
                                           premultiplied (wkDirect3D's) */
static int ind_tw, ind_th, ind_bw, ind_bh, ind_pct = -1, ind_lines;
static char ind_text[48], ind_msg[48];  /* the picture's text; a message shown instead of the number for a while */
static DWORD ind_msg_tick;
static uint8_t pal_seen[1024], dark_lut[256];
static int white_index = -1;

/* whose turn it is, and where its worm is (the auto zoom's and the speed zoom's) */
#define TRAIL_TEAMS 16
#define TRAIL_WORMS 64
static uintptr_t team_tasks[TRAIL_TEAMS], worm_objs[TRAIL_WORMS];
static int team_flags[TRAIL_TEAMS], n_team_tasks, n_worm_objs;
static int worms_ok;                    /* the team task and physics step hooks are on */
static int turn_owner = -1, turn_no, turn_multi;   /* the team whose turn it is (or was last); turns so far; several at once */
static int turn_worm_ok;                /* where the turn's worm is, this frame */
static double turn_wx, turn_wy;
static double turn_gv, turn_gvy;        /* its speed as the game has it, and its downward part (px/s; team_worm) */
static double spd_now;                  /* its speed (the speed zoom's) */
static int turn_key = -1, track_key = -1, track_turn = -1;   /* the turn's worm (team x 64 + its number), and whose
                                                                and which turn's the track is */

/* the land (the auto zoom's rays read WA's own tiles) */
static struct { int w, h, tw, th, cols, rows, key; } land;
static uintptr_t *tile_grid;            /* the land's tile bitmaps by place (row by row), as WA had them when set up */
static int land_ok;
static void *land_terrain;

static void (__fastcall *original_render)(void *);
void *orig_team, *orig_physics, *orig_queue, *orig_clamp, *orig_follow;   /* (read by the thunks below) */

static void say(const char *fmt, ...) {      /* (one write a line: the background builder says things too) */
    va_list ap;
    SYSTEMTIME t;
    char line[1024];
    int n, m;
    if (!log_file) return;
    GetLocalTime(&t);
    n = snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_start(ap, fmt);
    m = vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, ap);
    va_end(ap);
    if (m < 0) m = 0;
    if (n + m > (int)sizeof(line) - 2) m = (int)sizeof(line) - 2 - n;
    line[n + m] = '\n';
    fwrite(line, 1, (size_t)(n + m + 1), log_file);
    fflush(log_file);
}

/* ---- the thunks: WA's own conventions <-> C ---------------------------------------------------------------------- */
int __cdecl queue_hook(uint8_t *queue, void *display, void *camera);
void __cdecl clamp_hook(uint32_t *regs);
void __cdecl follow_hook(uint32_t *regs);
void thunk_queue(void);
void thunk_clamp(void);
void thunk_follow(void);
void thunk_team(void);
void thunk_physics(void);
void __cdecl team_seen(uint32_t task);
void __cdecl worm_seen(uint32_t obj);
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
    /* ClampCameraToBounds and the camera follow: the registers and arguments handed to C (which may change them),
     * then WA's own with them */
    ".globl _thunk_clamp\n"
    "_thunk_clamp:\n"
    "    pushal\n"
    "    pushl %esp\n"
    "    call _clamp_hook\n"
    "    addl $4, %esp\n"
    "    popal\n"
    "    jmp *_orig_clamp\n"
    ".globl _thunk_follow\n"
    "_thunk_follow:\n"
    "    pushal\n"
    "    pushl %esp\n"
    "    call _follow_hook\n"
    "    addl $4, %esp\n"
    "    popal\n"
    "    jmp *_orig_follow\n"
    /* a team task's message handler: the task noted (observe only: flags, registers and the x87 / SSE state kept, as
     * the WAI module's own hook here) */
    ".globl _thunk_team\n"
    "_thunk_team:\n"
    "    pushfl\n"
    "    pushal\n"
    "    movl %esp, %ebp\n"
    "    subl $544, %esp\n"
    "    andl $-16, %esp\n"
    "    fxsave 16(%esp)\n"
    "    pushl %ecx\n"
    "    call _team_seen\n"
    "    addl $4, %esp\n"
    "    fxrstor 16(%esp)\n"
    "    movl %ebp, %esp\n"
    "    popal\n"
    "    popfl\n"
    "    jmp *_orig_team\n"
    /* an object's physics step: worms noted (observe only, as above; the object is the second stack argument) */
    ".globl _thunk_physics\n"
    "_thunk_physics:\n"
    "    pushfl\n"
    "    pushal\n"
    "    movl %esp, %ebp\n"
    "    subl $544, %esp\n"
    "    andl $-16, %esp\n"
    "    fxsave 16(%esp)\n"
    "    pushl 44(%ebp)\n"
    "    call _worm_seen\n"
    "    addl $4, %esp\n"
    "    fxrstor 16(%esp)\n"
    "    movl %ebp, %esp\n"
    "    popal\n"
    "    popfl\n"
    "    jmp *_orig_physics\n"
);

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

/* the zoom range for the game's resolution and the largest map: none without wkDirect3D */
static void fit_limits(void) {
    zoom_limits(sw, sh, gw, gh, &cfg, &zoom_lo, &zoom_hi);
    if (!d3d.begin) zoom_lo = zoom_hi = 1.0;
    zoom_goal = zoom_clamp(zoom_goal, zoom_lo, zoom_hi);
    zoom_now = zoom_clamp(zoom_now, zoom_lo, zoom_hi);
    ind_pct = -1;
    ind_text[0] = 0;
}

/* the game resolution seen (a new game, or a resolution change): the largest map and the zoom range for it */
static int fit_size(int w, int h) {
    if (w < 64 || h < 64 || w > 16384 || h > 16384) return 0;
    if (w == sw && h == sh && fit_ok) return 1;
    sw = w; sh = h;
    zoom_frame(w, h, cfg.max_out, &gw, &gh);
    fit_ok = 1;
    fit_limits();
    say("game %dx%d: map up to %dx%d, zoom %.0f%% .. %.0f%%%s", sw, sh, gw, gh, zoom_lo * 100.0, zoom_hi * 100.0,
        d3d.begin ? "" : " (the graphics card drawing does not run: no zoom)");
    return 1;
}

/* wkDirect3D did not take the map this frame. Because of its size (the graphics card's largest texture, or its memory)?
 * Then the largest map is made to fit, and the zoom goes less far out. */
static void map_refused(void) {
    int mw = 0, mh = 0;
    double k, kh;
    refused_frames++;
    if (!d3d.mapmax || !d3d.mapmax(&mw, &mh) || (mw >= gw && mh >= gh)) return;
    k = (double)mw / sw; kh = (double)mh / sh;
    if (kh < k) k = kh;
    if (k < 1.0) k = 1.0;
    zoom_frame(sw, sh, k, &gw, &gh);
    while (gw > mw && gw - 2 >= sw) gw -= 2;
    while (gh > mh && gh - 2 >= sh) gh -= 2;
    fit_limits();
    say("the graphics card takes a map of at most %dx%d: the map is %dx%d now, zoom out to %.0f%%", mw, mh, gw, gh, zoom_lo * 100.0);
}

/* ---- the display's view ------------------------------------------------------------------------------------------ */
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

/* ---- the land ---------------------------------------------------------------------------------------------------- */
static void land_free(void) {
    free(tile_grid);
    tile_grid = NULL;
    memset(&land, 0, sizeof(land));
    land_ok = 0; land_terrain = NULL;
}

/* WA's land now against the module's copy, each frame: 0 the same; 1 the same sizes but other tile objects (WA made the
 * land again: a new game, or the same game's land made again); 2 other sizes (another level). WA can start a new game
 * with its world, display and terrain at the very addresses of the last one (the owner's crash, 2026-11-22: round 5 of
 * a match taken for round 4, whose tiles WA had freed, and the auto zoom's rays read one) */
static int land_check(void *display) {
    char *terrain = *(char **)((char *)display + D_TERRAIN);
    uint32_t *arr;
    if (!land_ok) return 0;
    if (!terrain || !(arr = *(uint32_t **)(terrain + 4))) return 2;
    if (FIELD(display, D_LAND_W) != land.w || FIELD(display, D_LAND_H) != land.h || FIELD(display, D_TILE_W) != land.tw ||
        FIELD(display, D_TILE_H) != land.th) return 2;
    if (terrain == land_terrain && !memcmp(arr, tile_grid, sizeof(uint32_t) * (size_t)land.cols * land.rows)) return 0;
    return 1;
}

/* the game's land, once its tiles exist (wkVista, land.rs): which tile lies where, and its see-through index */
static void land_setup(void *display) {
    char *terrain = *(char **)((char *)display + D_TERRAIN);
    uint32_t *arr;
    int w, h, tw, th, cols, rows, k, key = -1, n = 0;
    if (!terrain || terrain == land_terrain) return;
    arr = *(uint32_t **)(terrain + 4);
    w = FIELD(display, D_LAND_W); h = FIELD(display, D_LAND_H); tw = FIELD(display, D_TILE_W); th = FIELD(display, D_TILE_H);
    if (!arr || w <= 0 || h <= 0 || tw <= 0 || th <= 0 || w > 0x8000 || h > 0x8000) return;
    land_free();
    land_terrain = terrain;
    cols = (w + tw - 1) / tw; rows = (h + th - 1) / th;
    tile_grid = (uintptr_t *)calloc((size_t)cols * rows, sizeof(uintptr_t));
    if (!tile_grid) { say("land: no memory"); return; }
    for (k = 0; k < cols * rows; k++) {
        char *bm = (char *)(uintptr_t)arr[k], *s;
        tile_grid[k] = (uintptr_t)bm;
        if (!bm) continue;
        n++;
        s = *(char **)(bm + 4);
        if (s && key < 0) key = *(uint8_t *)(s + 0x10);
    }
    land.w = w; land.h = h; land.tw = tw; land.th = th; land.cols = cols; land.rows = rows;
    land.key = key < 0 ? 0 : key;
    land_ok = 1;
    say("land %dx%d in %d tiles of %dx%d (see-through %d)", w, h, n, tw, th, land.key);
}

/* ---- fills as wide as the level: across the whole zoomed view --------------------------------------------------- */
/* WA's deep water, the strips of water at its surface and the sky above its gradient are fills between the world's left
 * and right bounds (game +0x779C / +0x77A0: some way past the level's sides). At 100% the camera never shows what is
 * past them. Zoomed out it does: the water and the sky ended there, and beyond was nothing (the owner's screenshots,
 * 2026-11-24; a friend's, 2026-11-22). While the map entries of a zoomed frame are drawn, a fill that covers at least
 * the level's own columns is drawn from the view's left edge to its right. (Only fills: the land and everything else
 * stay as WA puts them. The waves and the far sea are rows WA tiles across the display's clip: they reach by themselves.) */
static void (__fastcall *original_fill)(void *, int, int, int, int, int, int);
static int wide_on, wide_l = 1, wide_r, wide_rw;   /* the level's columns in map coordinates (none: wide_l > wide_r), the map's width */
static unsigned widened;
static void __fastcall hook_fill(void *d, int edx, int x1, int y1, int x2, int y2, int colour) {
    if (wide_on && d == frame_display && wide_l < wide_r && y2 - y1 >= 2) {
        const int ox = FIELD(d, D_ORG), l = x1 + ox, r = x2 + ox;
        if (l <= wide_l && r >= wide_r && (l > 0 || r < wide_rw)) {
            if (test_mode && widened < 4) say("test: fill %d..%d x %d..%d (the level %d..%d) drawn across the map (0..%d)", l, r, y1, y2, wide_l, wide_r, wide_rw);
            widened++;
            x1 = -ox; x2 = wide_rw - ox;
        }
    }
    original_fill(d, edx, x1, y1, x2, y2, colour);
}

/* ---- the drawing queue: the map through wkDirect3D at the zoom, then the HUD ------------------------------------- */
/* Zoomed: every map entry (back to front) drawn by WA into the map frame wkDirect3D hands out, that put on the screen
 * at the zoom, then every HUD entry (back to front) as WA draws it. (WA's own order puts the HUD in front of the map; an
 * entry of each kind on one layer -- the turn's arrow shares layer 0 with the HUD -- keeps its place among its own kind.) */
static uint32_t queue_list[MAX_ENTRIES];
static int draw_entry(uint8_t *queue, void *display, void *camera, uint32_t e) {
    *(uint32_t *)(queue + Q_COUNT) = 1;
    ((uint32_t *)(queue + Q_ENTRIES))[0] = e;
    return call_queue(queue, display, camera);
}
static int is_screen(uint32_t e) {
    int32_t kind = *(int32_t *)(uintptr_t)e;
    return kind == 1 || kind == 4 || kind == 7;
}

/* For the log: how the camera moved in each second it moved much (someone looking around). Frames it did not move in,
 * or big single steps, would show as a choppy picture however many frames a second there are. */
static void camera_note(int cx, int cy) {
    static LARGE_INTEGER t_last, t_win;
    static int px, py, have, n, moved, big, dist, lines;
    static double longest;
    LARGE_INTEGER t;
    double dt;
    int dx, dy, d;
    if (!clock_freq.QuadPart) return;
    QueryPerformanceCounter(&t);
    if (!have) { have = 1; px = cx; py = cy; t_last = t_win = t; return; }
    dt = (double)(t.QuadPart - t_last.QuadPart) / (double)clock_freq.QuadPart;
    t_last = t;
    if (dt > longest) longest = dt;
    n++;
    dx = cx > px ? cx - px : px - cx; dy = cy > py ? cy - py : py - cy; d = dx > dy ? dx : dy;
    if (d) { moved++; dist += d; if (d > big) big = d; }
    px = cx; py = cy;
    if (t.QuadPart - t_win.QuadPart >= clock_freq.QuadPart) {
        if (dist >= 300 && lines < 80) {
            lines++;
            say("camera: in the last second %d frames (the longest %.1f ms); the camera moved in %d of them, %d px in all, largest step %d px; zoom %.0f%%",
                n, longest * 1000.0, moved, dist, big, frame_zoom * 100.0);
        }
        t_win = t; n = moved = big = dist = 0; longest = 0.0;
    }
}

int __cdecl queue_hook(uint8_t *queue, void *display, void *camera) {
    typedef void (__cdecl *qsort_fn)(void *, size_t, size_t, void *);
    view_state wa, world;
    wkd3d_view v;
    uint32_t n = *(uint32_t *)(queue + Q_COUNT);
    uint32_t *entries = (uint32_t *)(queue + Q_ENTRIES);
    const double z = frame_zoom;
    uint8_t *map;
    int i, last = 0, rw, rh, bx, by, cx1, cy1, cx2, cy2, nworld = 0, nscreen = 0, ok;
    if (display == frame_display && camera) {   /* (for wkMapZoom_View: screen = origin + (world - camera) x zoom) */
        view_now.zoom = 1.0;
        view_now.org_x = FIELD(display, D_ORG); view_now.org_y = FIELD(display, D_ORG + 4);
        view_now.cam_x = ((const int32_t *)camera)[0] >> 16; view_now.cam_y = ((const int32_t *)camera)[1] >> 16;
        view_ok = 1;
        camera_note(view_now.cam_x, view_now.cam_y);
    }
    if (z == 1.0 || display != frame_display || !d3d.begin || !d3d.end || !fit_ok || !n || n > MAX_ENTRIES)
        return call_queue(queue, display, camera);
    {   /* the frame grid brought up to date if it describes another frame than the renderer's (as WA's lock would: a
         * resolution switch leaves it out of date until WA's next display drawing call) */
        char *g = *(char **)((char *)display + D_GRID);
        const int32_t fb = *(int32_t *)VA(FRAME_PTR_VA), fbw = *(int32_t *)VA(FRAME_W_VA), fbh = *(int32_t *)VA(FRAME_H_VA);
        if (g && fb && fbw > 0 && fbh > 0 && (FIELD(g, GR_DATA) != fb || FIELD(g, GR_STRIDE) != fbw || FIELD(g, GR_W) != fbw || FIELD(g, GR_H) != fbh)) {
            static unsigned told;
            if (told++ < 4) say("the display's frame grid was %dx%d (stride %d), the frame %dx%d: brought up to date", FIELD(g, GR_W),
                                FIELD(g, GR_H), FIELD(g, GR_STRIDE), fbw, fbh);
            FIELD(g, GR_DATA) = fb; FIELD(g, GR_STRIDE) = fbw; FIELD(g, GR_W) = fbw; FIELD(g, GR_H) = fbh;
            FIELD(g, GR_CLIP) = 0; FIELD(g, GR_CLIP + 4) = 0; FIELD(g, GR_CLIP + 8) = fbw; FIELD(g, GR_CLIP + 12) = fbh;
        }
    }
    view_save(display, &wa);
    cx1 = wa.clip[0]; cy1 = wa.clip[1]; cx2 = wa.clip[2]; cy2 = wa.clip[3];
    if (cx1 < 0) cx1 = 0;
    if (cy1 < 0) cy1 = 0;
    if (cx2 > wa.fbw) cx2 = wa.fbw;
    if (cy2 > wa.fbh) cy2 = wa.fbh;
    if (cx2 > sw) cx2 = sw;
    if (cy2 > sh) cy2 = sh;
    if (cx2 <= cx1 || cy2 <= cy1 || !wa.fb || wa.fbw != sw || wa.fbh != sh) return call_queue(queue, display, camera);
    zoom_region(cx1, cy1, cx2, cy2, wa.org[0], wa.org[1], z, gw, gh, &rw, &rh, &bx, &by);
    map = (uint8_t *)d3d.begin(gw, gh, rw, rh);   /* the frame for the map: gw bytes a row, gh rows */
    if (!map) {                             /* not this frame (wkDirect3D is not drawing it): as WA draws it, at 100% */
        map_refused();
        return call_queue(queue, display, camera);
    }
    ((qsort_fn)VA(QSORT_VA))(entries, n, 4, (void *)VA(QUEUE_ORDER_VA));   /* WA's own order, as it sorts them */
    memcpy(queue_list, entries, n * 4);
    world = wa;
    world.w = rw; world.h = rh;
    world.clip[0] = world.clip[1] = 0; world.clip[2] = rw; world.clip[3] = rh;
    world.org[0] = bx; world.org[1] = by;
    world.data = (int32_t)(uintptr_t)map; world.stride = gw; world.gw = rw; world.gh = rh;
    memcpy(world.gclip, world.clip, sizeof(world.clip));
    world.fb = (int32_t)(uintptr_t)map; world.fbw = gw; world.fbh = gh;   /* (rows gw apart) */
    view_load(display, &world);
    if (original_fill && camera && land_ok && land.w > 0) {   /* (WA's camera transform: map = world - camera + bx, whole pixels) */
        wide_l = bx - (((const int32_t *)camera)[0] >> 16); wide_r = wide_l + land.w; wide_rw = rw; wide_on = 1;
    }
    for (i = (int)n - 1; i >= 0; i--)       /* the map, back to front */
        if (!is_screen(queue_list[i])) { last = draw_entry(queue, display, camera, queue_list[i]); nworld++; }
    wide_on = 0;
    view_load(display, &wa);
    memset(&v, 0, sizeof(v));
    v.size = (int)sizeof(v); v.zoom = z; v.bx = bx; v.by = by; v.ox = wa.org[0]; v.oy = wa.org[1];
    v.x1 = cx1; v.y1 = cy1; v.x2 = cx2; v.y2 = cy2; v.smooth = filter_smooth;
    ok = d3d.end(&v);                       /* the map onto the screen at the zoom */
    for (i = (int)n - 1; i >= 0; i--)       /* the HUD, back to front */
        if (is_screen(queue_list[i])) { last = draw_entry(queue, display, camera, queue_list[i]); nscreen++; }
    *(uint32_t *)(queue + Q_COUNT) = n;
    memcpy(entries, queue_list, n * 4);
    if (ok) { view_now.zoom = z; zoomed_frames++; }
    if (!logged_split) {
        logged_split = 1;
        say("first zoomed frame: %u entries: %d map (region %dx%d of %dx%d), %d HUD; view %d,%d..%d,%d, centre %d,%d%s", n, nworld, rw, rh,
            gw, gh, nscreen, cx1, cy1, cx2, cy2, wa.org[0], wa.org[1], ok ? "" : "; wkDirect3D could not put it on the screen");
    }
    return last;
}

/* zoomed in, the camera's clamp told the size the zoomed view sees, so it reaches the level's edges; zoomed out it is
 * left as WA has it at 100% (the owner, 2026-11-22: told the bigger view, it pushed the camera toward the level's middle
 * near an edge as the map zoomed out -- now the camera stays where it is at 100%, and past the edge shows) */
void __cdecl clamp_hook(uint32_t *regs) {
    /* pushal: edi esi ebp esp ebx edx ecx eax, then the return address and the arguments */
    double z = frame_zoom;
    int32_t width, height, left, top, right, bottom, span_w, span_h;
    if (z <= 1.0 || !frame_display) return;
    width = (int32_t)regs[7]; height = (int32_t)regs[10];
    left = (int32_t)regs[11]; top = (int32_t)regs[12]; right = (int32_t)regs[13]; bottom = (int32_t)regs[6];
    span_w = (right - left) >> 16; span_h = (bottom - top) >> 16;
    width = (int32_t)ceil(width / z); height = (int32_t)ceil(height / z);
    if (span_w > 0 && width > span_w) width = span_w;
    if (span_h > 0 && height > span_h) height = span_h;
    regs[7] = (uint32_t)width;
    regs[10] = (uint32_t)height;
}

/* The camera follow (EAX half the view's width, EDI the camera; half its height, then the box it follows: x1, y1, x2,
 * y2 in 1/65536 pixels). Zoomed in it is told half the zoomed-in view's size, so it keeps the worm inside what is seen;
 * at 100% and below it follows as ever. */
void __cdecl follow_hook(uint32_t *regs) {
    const double z = eff_zoom();
    int32_t hw = (int32_t)regs[7], hh = (int32_t)regs[9];
    static unsigned logged;
    if (z <= 1.0 || !fit_ok || !zoom_ready || GetTickCount() - live_tick > 1000) return;
    hw = (int32_t)ceil(hw / z); hh = (int32_t)ceil(hh / z);
    if (hw < 1) hw = 1;
    if (hh < 1) hh = 1;
    if (test_mode && logged < 3) { logged++; say("test: camera follow at %.0f%%: half view %dx%d -> %dx%d", z * 100.0, (int)regs[7], (int)regs[9], hw, hh); }
    regs[7] = (uint32_t)hw;
    regs[9] = (uint32_t)hh;
}

/* ---- whose turn it is, and where its worm is --------------------------------------------------------------------- */
/* Each frame the worm of the team whose turn it is (a team task with its turn flag, +0x3C, up: one team at a time; its
 * current worm +0x48) is looked up: the auto zoom and the speed zoom follow it. Team tasks are found by their message
 * handler (00557310), worms by their physics step (004FCDF0). A team task: +0x2C the game, +0x38 its team. */
static int readable(uintptr_t p, size_t n) {
    MEMORY_BASIC_INFORMATION m;
    return p && VirtualQuery((void *)p, &m, sizeof(m)) && m.State == MEM_COMMIT && !(m.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
           p + n >= p && p + n <= (uintptr_t)m.BaseAddress + m.RegionSize;
}

void __cdecl team_seen(uint32_t task) {
    int i;
    if (!worms_ok || !last_world || n_team_tasks >= TRAIL_TEAMS || !readable(task, 0x50) || *(uint32_t *)(task + 0x2c) != (uint32_t)(uintptr_t)last_world)
        return;
    for (i = 0; i < n_team_tasks; i++) if (team_tasks[i] == task) return;
    team_tasks[n_team_tasks] = task;
    team_flags[n_team_tasks] = 0;
    n_team_tasks++;
}

void __cdecl worm_seen(uint32_t obj) {
    int i;
    if (!worms_ok || !obj || *(uint32_t *)obj != VA(WORM_VTABLE_VA) || n_worm_objs >= TRAIL_WORMS) return;
    for (i = 0; i < n_worm_objs; i++) if (worm_objs[i] == obj) return;
    worm_objs[n_worm_objs++] = obj;
}

static void turn_reset(void) {
    n_team_tasks = 0;
    n_worm_objs = 0;
    turn_owner = -1;
    turn_no = 0;
    turn_multi = 0;
}

/* a team's current worm and where it is (px); 0 when it isn't known. v (may be NULL): its speed as the game has it
 * (+0x90 / +0x94: its move each step, 1/65536 px; WA steps 50 times a second), in px/s */
static int team_worm(uintptr_t task, double *x, double *y, double *v) {
    const int32_t team = *(int32_t *)(task + 0x38), index = *(int32_t *)(task + 0x48);
    int i;
    for (i = 0; i < n_worm_objs; i++) {
        const uintptr_t o = worm_objs[i];
        if (*(int32_t *)(o + 0xfc) == team && *(int32_t *)(o + 0x100) == index) {
            *x = *(int32_t *)(o + 0x84) / 65536.0;
            *y = *(int32_t *)(o + 0x88) / 65536.0;
            if (v) {                        /* (the turn's worm: its downward speed kept too, for the auto zoom) */
                *v = hypot(*(int32_t *)(o + 0x90) / 65536.0, *(int32_t *)(o + 0x94) / 65536.0) * 50.0;
                turn_gvy = *(int32_t *)(o + 0x94) / 65536.0 * 50.0;
            }
            return 1;
        }
    }
    return 0;
}

/* each frame, before WA's own drawing: the worms and team tasks still there, turns starting, the turn's worm */
static void turn_frame(void *world) {
    int i, flagged = 0;
    uintptr_t owner_task = 0;
    turn_worm_ok = 0;
    if (!worms_ok) return;
    for (i = 0; i < n_worm_objs;) {
        const uintptr_t o = worm_objs[i];
        if (!readable(o, 0x104) || *(uint32_t *)o != VA(WORM_VTABLE_VA) || *(uint32_t *)(o + 0x2c) != (uint32_t)(uintptr_t)world)
            worm_objs[i] = worm_objs[--n_worm_objs];
        else
            i++;
    }
    for (i = 0; i < n_team_tasks;) {
        const uintptr_t t = team_tasks[i];
        int team, flag;
        if (!readable(t, 0x50) || *(uint32_t *)(t + 0x2c) != (uint32_t)(uintptr_t)world) {
            team_tasks[i] = team_tasks[--n_team_tasks];
            team_flags[i] = team_flags[n_team_tasks];
            continue;
        }
        team = *(int32_t *)(t + 0x38);
        flag = *(int32_t *)(t + 0x3c) != 0;
        if (team >= 0 && team < TRAIL_TEAMS) {
            if (flag && !team_flags[i]) { turn_no++; turn_owner = team; }   /* its turn starts */
            if (flag) flagged++;
            if (team == turn_owner) owner_task = t;
        }
        team_flags[i] = flag;
        i++;
    }
    if (flagged > 1 && !turn_multi) { turn_multi = 1; say("several teams have the turn at once (real-time play?): no auto zoom, no speed zoom"); }
    if (turn_multi) return;
    if (turn_owner >= 0 && owner_task && team_worm(owner_task, &turn_wx, &turn_wy, &turn_gv)) {
        turn_worm_ok = 1;
        turn_key = turn_owner * 64 + (*(int32_t *)(owner_task + 0x48) & 63);
    }
}

/* ---- the auto zoom ------------------------------------------------------------------------------------------- */
/* The owner's idea (2026-11-22): where it is open the map zooms out further -- already as the worm heads into the open,
 * not once it is there -- and where it is tight (a time trial map) it zooms in. Several times a second, from a point,
 * 16 rays go out through the land as it is now (1 px steps, so 1-2 px walls stop them; blown-up holes are open; the
 * level's edges are walls); each ray's end wants the zoom that puts it 80% of the way to the screen's edge in its
 * direction (its x against the half width, its y against the half height, so a tall open space counts for more than an
 * equally wide one). How open the point is: the average (in log) of the 8 most open directions' zooms -- below 1 it
 * is open and that is the zoom; 1 to 3 is ordinary (100%); above 3 it is tight and zooms in, (q / 3)^0.46 (about 150%
 * on a 1920x696 time trial map at 2560x1440). The ray straight down comes first while the worm falls fast (the owner,
 * 2026-11-22: AutoZoomFallSpeed, 1000 px/s down): the zoom never goes so far in that the land below the worm would be
 * off the screen's bottom edge (a quarter of the drop, at most 200 px, shown past it), so falling down a shaft zooms out
 * even where the sides are close (a narrow shaft read as tight before and zoomed in). The points: the worm, and where it is heading (its speed x 0.5, 1, 1.5
 * and 2 s along its way, stopped short of land); the most zoomed out of them, smoothed (0.3 s), is the goal. The goal scales
 * the zoom chosen with the wheel (the wheel is the base), between AutoZoomMin and AutoZoomMax, and never further out
 * than where the whole level fits the screen. It only changes once the goal is 10% away, and eases there (1 s out, 0.9 s
 * in; at most 60% a second out, 40% in): only the zoom changes, never where the camera is (the smart camera made the
 * owner sick). Off until Ctrl + A or AutoZoom=1. Worked out away from WA on the owner's Round 5 replay (a
 * maze with one tall open funnel: corridors read about 2.3, the funnel 0.7-1) and a time trial replay (about 7). */
static int auto_zoom;                   /* AutoZoom= (Ctrl + A switches it) */
static double auto_min = 0.35;          /* AutoZoomMin=: the furthest it zooms out */
static double auto_max = 1.5;           /* AutoZoomMax=: the furthest it zooms in */
static double auto_factor = 1.0, auto_goal = 1.0;   /* the zoom's auto share; its goal */
static double auto_lg, auto_want = 1.0, auto_here = 1.0, auto_below = 20.0, auto_speed;   /* the smoothed log of the
                                                     wanted zoom (once auto_lg_ok); the zoom wanted last; how open it is
                                                     at the worm alone, and what the ray under it wants; its speed */
static int auto_lg_ok, auto_falling;    /* (auto_falling: the ray down counts this look) */
static LARGE_INTEGER auto_clock, auto_evalt;
#define AUTO_M 0.8                      /* a ray's end at 80% of the way to the screen's edge */
#define AUTO_TIGHT 3.0                  /* how open: up to here ordinary (100%), above it tight (zoomed in) */
#define AUTO_IN_POW 0.464               /* tight: (q / AUTO_TIGHT)^this */
#define AUTO_DOWN 4                     /* the ray straight down (of 16, from 0 = right; y grows downwards) */
static double auto_fall = 1000.0;       /* AutoZoomFallSpeed=: the ray down counts only while the worm falls this fast
                                           (px/s down; the owner, 2026-11-22) */
#define AUTO_DOWN_ROOM 200.0            /* map px shown past the floor below the worm, at most (else a quarter of the drop:
                                           the owner, 2026-11-22: a 2000 px drop showed 500 px past the floor) */
#define AUTO_TRACK 48
static double auto_tx[AUTO_TRACK], auto_ty[AUTO_TRACK], auto_tt[AUTO_TRACK];   /* the worm's last places, and when */
static int auto_tn, auto_ti;
static int auto_phase, auto_np;         /* the look's frame (0-5); its points ahead */
/* The speed zoom (the owner, 2026-11-22): past a speed the zoom is multiplied down, more the faster the worm goes --
 * the strength once for every SPEED_STEP px/s over (from 800 px/s at x0.7: x0.7 at 1300 px/s, x0.49 at 1800). The speed
 * is the auto zoom's (the turn's worm, over the last quarter second), held: it falls off over SPEED_HOLD, so a rope
 * swing's slow top doesn't pump the zoom. On its own or with the auto zoom (then it multiplies the auto zoom's); never
 * further out than AutoZoomMin or where the whole level fits. Its speeds, measured on the owner's replays: moving worms
 * go about 350-400 px/s (median), above 600 px/s 6% of the time, falls and fast swings up to about 1700 px/s. */
static int speed_zoom;                  /* SpeedZoom= */
static double speed_from = 800.0;       /* SpeedZoomFrom=: from this speed (land pixels a second; 800: on the owner's
                                           newest game 500 fired 4 times a minute on ordinary rope flings, 800 only
                                           on the 5 real falls) */
static double speed_mult = 0.7;         /* SpeedZoomStrength=: the zoom times this for every SPEED_STEP px/s over */
static double speed_held, speed_m = 1.0;   /* the speed, held; the multiplier now */
static double auto_vx, auto_vy;         /* the worm's velocity (over the last quarter second; auto_speed its size) */

#define SPEED_STEP 500.0
#define SPEED_HOLD 1.5                  /* s: the held speed's fall-off (its time constant) */
static double auto_px[4], auto_py[4], auto_acc;   /* those points; the most zoomed-out zoom wanted so far this look
                                                     (0: none) */
static double eff_zoom(void) { return zoom_clamp(zoom_now * auto_factor, zoom_lo, zoom_hi); }
static double ind_alpha(void);

static int land_solid(int x, int y) {
    uintptr_t bm;
    char *sf;
    int sw_, sh_, lx, ly;
    if (x < 0 || y < 0 || x >= land.w || y >= land.h) return 1;          /* (the level's edges count as walls: there
                                                                            is nothing to see past them) */
    bm = tile_grid[(y / land.th) * land.cols + x / land.tw];
    if (!bm || !(sf = *(char **)(bm + 4)) || !*(uint8_t **)(sf + 0xc)) return 0;
    sw_ = FIELD(sf, 4); sh_ = FIELD(sf, 8);
    lx = x % land.tw; ly = y % land.th;
    if (lx >= sw_ || ly >= sh_) return 0;
    return (*(uint8_t **)(sf + 0xc))[(size_t)ly * sw_ + lx] != land.key;
}

static int dcmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

/* how far the land is from (px, py) along (dx, dy), up to maxd: one pixel at a time along the longer axis (so a 1 px
 * wall across the way can't be stepped over), read in WA's own tiles as they are now (blown up: open; a girder put
 * down: a wall). The tile a step lies in is looked up only when the ray leaves the last one. */
static double auto_ray(double px, double py, double dx, double dy, double maxd) {
    const double adx = fabs(dx), ady = fabs(dy), major = adx > ady ? adx : ady;
    const double sx = dx / major, sy = dy / major, per = 1.0 / major;
    const int n = (int)(maxd * major), w = land.w, h = land.h, tw = land.tw, th = land.th;
    const uint8_t *data = NULL;
    int tx0 = 0, ty0 = 0, tx1 = 0, ty1 = 0, tsw = 0, tsh = 0;   /* the tile the last step lay in: its place, its bitmap */
    double x = px, y = py;
    int i;
    for (i = 1; i <= n; i++) {
        int ix, iy;
        x += sx; y += sy;
        if (x < 0.0 || y < 0.0) return i * per;                 /* (the level's edge: a wall) */
        ix = (int)x; iy = (int)y;
        if (ix >= w || iy >= h) return i * per;
        if (ix < tx0 || ix >= tx1 || iy < ty0 || iy >= ty1) {
            const uintptr_t bm = tile_grid[(iy / th) * land.cols + ix / tw];
            char *sf = bm ? *(char **)(bm + 4) : NULL;
            tx0 = ix / tw * tw; ty0 = iy / th * th; tx1 = tx0 + tw; ty1 = ty0 + th;
            data = sf ? *(const uint8_t **)(sf + 0xc) : NULL;
            tsw = sf ? FIELD(sf, 4) : 0; tsh = sf ? FIELD(sf, 8) : 0;
        }
        if (data && ix - tx0 < tsw && iy - ty0 < tsh && data[(size_t)(iy - ty0) * tsw + (ix - tx0)] != land.key) return i * per;
    }
    return maxd;
}

/* how open it is around (px, py) (see above): the zoom that the 8 most open of 16 directions want, on average (in
 * log); no further out than lo is looked for. *below: the zoom the ray straight down wants on its own */
static double auto_fit(double px, double py, double lo, double lo_down, double *below) {
    double z[16], s = 0.0, down = 1.0;
    int k;
    for (k = 0; k < 16; k++) {
        const double ang = k * (6.283185307179586 / 16.0), dx = cos(ang), dy = sin(ang);
        const double ex = fabs(dx) > 1e-9 ? (sw / 2.0) / fabs(dx) : 1e9, ey = fabs(dy) > 1e-9 ? (sh / 2.0) / fabs(dy) : 1e9;
        const double edge = AUTO_M * (ex < ey ? ex : ey);   /* 80% of the way to the screen's edge this way, at 100% */
        const double d = auto_ray(px, py, dx, dy, edge / (k == AUTO_DOWN ? lo_down : lo) + 2.0);   /* (the ray goes only as
                                                     far as matters; down: as far as the real zoom's floor) */
        z[k] = edge / d;
        if (k == AUTO_DOWN) down = d;
    }
    /* the ray down on its own: the screen's bottom edge a little past the floor (a quarter of the drop, at most
     * AUTO_DOWN_ROOM px) */
    *below = (sh / 2.0) / (down + (down * 0.25 < AUTO_DOWN_ROOM ? down * 0.25 : AUTO_DOWN_ROOM));
    qsort(z, 16, sizeof(double), dcmp);
    for (k = 0; k < 8; k++) s += log(z[k] < lo ? lo : z[k] > 20.0 ? 20.0 : z[k]);
    return exp(s / 8.0);
}

/* how open it is -> the zoom: open (below 1) as it is, ordinary (1 to AUTO_TIGHT) 100%, tight zoomed in */
static double auto_map(double q) { return q < 1.0 ? q : q <= AUTO_TIGHT ? 1.0 : pow(q / AUTO_TIGHT, AUTO_IN_POW); }

/* the zoom (the auto share) a point wants: as open as it is (auto_map); at the worm itself (under) also never so far in
 * that the land straight below it would be off the screen's bottom edge (a quarter of the drop, at most AUTO_DOWN_ROOM
 * px, shown past it) -- the ray under the worm
 * comes first (the owner, 2026-11-22: falling down a shaft, or high up with nothing below, zooms out even where the
 * sides are close; zoomed in, it eases off as the ground drops away). That ray's zoom is a real zoom (the screen at
 * 100%), so it is taken as a share of the wheel's zoom. Only at the worm: the points ahead lie on a straight line
 * along its speed, high above where a jump goes, and their rays down pumped the zoom in and out on the owner's replays.
 * *q, *below (may be NULL): how open it is, and the zoom the ray down wants */
static double auto_point(double px, double py, double lo, int under, double *q, double *below) {
    double b, o = auto_fit(px, py, lo, under && zoom_now < 1.0 ? lo * zoom_now : lo, &b), w = auto_map(o);
    if (under && b / zoom_now < w) w = b / zoom_now;
    if (q) *q = o;
    if (below) *below = b;
    return w;
}

static void auto_zoom_frame(void) {
    LARGE_INTEGER now;
    double dt, step, fl = auto_min, ce = auto_max < 1.0 ? 1.0 : auto_max;   /* the auto share's least and most */
    int have, inside, track, skip = 0;
    QueryPerformanceCounter(&now);
    dt = auto_clock.QuadPart ? (double)(now.QuadPart - auto_clock.QuadPart) / clock_freq.QuadPart : 0.0;
    auto_clock = now;
    if (dt > 0.25) dt = 0.25;
    if (land_ok && land.w > 0 && land.h > 0 && zoom_now > 0.0) {   /* no further out than where the whole level fits the
                                                       screen (a 1920x696 time trial map at 2560x1440: 133%) */
        const double fx = (double)sw / land.w, fy = (double)sh / land.h, whole = (fx < fy ? fx : fy) / zoom_now;
        if (whole > fl) fl = whole;
    }
    if (fl > 1.0) fl = 1.0;
    have = (auto_zoom || speed_zoom) && land_ok && tile_grid && turn_worm_ok && zoom_now > 0.0 &&
           (fl < 0.999 || (auto_zoom && ce > 1.001));   /* (the speed zoom alone only zooms out) */
    /* (the worm off the level -- above an island map's top, or before it is placed, when WA keeps it far off the map --
     * is not inside) */
    inside = turn_worm_ok && land_ok && turn_wx >= 0.0 && turn_wy >= 0.0 && turn_wx < land.w && turn_wy < land.h;
    /* the track in a looser box: high above the level is fine; not WA's far-off place for a worm
     * not placed yet (y -10000) */
    track = turn_worm_ok && land_ok && turn_wx > -4000.0 && turn_wx < land.w + 4000.0 && turn_wy > -8000.0 &&
            turn_wy < land.h + 4000.0;
    if (have && track) {                    /* the worm's track, and its speed */
        const double ts = (double)now.QuadPart / clock_freq.QuadPart;
        const int last = (auto_ti + AUTO_TRACK - 1) % AUTO_TRACK;
        int k, j = -1;
        if (turn_key != track_key || turn_no != track_turn) {   /* another worm or turn: a new track, from rest */
            track_key = turn_key; track_turn = turn_no;
            auto_tn = 0;
            speed_held = 0.0;
        }
        if (auto_tn && hypot(turn_wx - auto_tx[last], turn_wy - auto_ty[last]) > 300.0) auto_tn = 0;   /* (a teleport) */
        auto_tx[auto_ti] = turn_wx; auto_ty[auto_ti] = turn_wy; auto_tt[auto_ti] = ts;
        auto_ti = (auto_ti + 1) % AUTO_TRACK;
        if (auto_tn < AUTO_TRACK) auto_tn++;
        for (k = 1; k < auto_tn; k++) {             /* the speed: over the last quarter second */
            const int s = (auto_ti + AUTO_TRACK - 1 - k) % AUTO_TRACK;
            if (ts - auto_tt[s] > 0.25) break;
            j = s;
        }
        auto_vx = auto_vy = 0.0;
        if (j >= 0 && ts - auto_tt[j] > 0.05) {
            auto_vx = (turn_wx - auto_tx[j]) / (ts - auto_tt[j]);
            auto_vy = (turn_wy - auto_ty[j]) / (ts - auto_tt[j]);
        }
        auto_speed = hypot(auto_vx, auto_vy);
        spd_now = turn_gv > 1.0 ? turn_gv : auto_speed;
    } else {
        spd_now = 0.0;
        auto_tn = 0;
        auto_speed = auto_vx = auto_vy = 0.0;
    }
    {   /* a look's step at most every 3 ms (at very high frame rates not every frame: the rays cost the same as ever) */
        static LARGE_INTEGER last_look;
        if (last_look.QuadPart && (double)(now.QuadPart - last_look.QuadPart) / clock_freq.QuadPart < 0.003) skip = 1;
        else last_look = now;
    }
    if (have && !inside) {                          /* the zoom held */
        auto_phase = 0;
        auto_acc = 0.0;
    } else if (have && skip) {
        /* (not this frame) */
    } else if (have) {
        /* one look every 6 frames, shared out so no frame casts more than one point's 16 rays: frame 0 the worm's own
         * point, frames 1-4 the points ahead, frame 5 the goal */
        if (auto_phase == 0) {
            const double vx = auto_vx, vy = auto_vy;
            int k;
            auto_np = 0;
            if (!auto_zoom) {                       /* (the speed zoom alone: no rays) */
                auto_acc = 1.0;
                auto_here = 1.0;
                auto_below = 20.0;
            } else {                        /* (the ray down: only while the worm falls fast -- the game's speed, else
                                               its path's) */
                const double down_v = turn_gv > 1.0 ? turn_gvy : auto_vy;
                auto_falling = down_v >= auto_fall;
                auto_acc = auto_point(turn_wx, turn_wy, fl, auto_falling, &auto_here, &auto_below);
            }
            if (auto_zoom && auto_speed > 80.0) {   /* where it is heading */
                static const double look[4] = { 0.5, 1.0, 1.5, 2.0 };
                const double ux = vx / auto_speed, uy = vy / auto_speed;
                const double free_ = auto_ray(turn_wx, turn_wy, ux, uy, auto_speed * 2.0 + 4.0);
                for (k = 0; k < 4; k++) {
                    double dist = auto_speed * look[k];
                    const int stopped = dist > free_ - 48.0;
                    if (stopped) dist = free_ - 48.0;
                    if (dist < 32.0) break;
                    auto_px[auto_np] = turn_wx + ux * dist;
                    auto_py[auto_np++] = turn_wy + uy * dist;
                    if (stopped) break;
                }
            }
        } else if (auto_phase <= 4) {
            if (auto_phase - 1 < auto_np) {
                const double w = auto_point(auto_px[auto_phase - 1], auto_py[auto_phase - 1], fl, 0, NULL, NULL);
                if (w < auto_acc) auto_acc = w;
            }
        } else if (auto_acc > 0.0) {
            double want = auto_acc, g, edt;
            edt = auto_evalt.QuadPart ? (double)(now.QuadPart - auto_evalt.QuadPart) / clock_freq.QuadPart : 0.1;
            auto_evalt = now;
            if (edt > 0.5) edt = 0.5;
            /* the speed zoom: the speed held (it falls off over SPEED_HOLD), and past speed_from the strength once for
             * every SPEED_STEP px/s over */
            speed_held = spd_now > speed_held ? spd_now : speed_held * exp(-edt / SPEED_HOLD);
            speed_m = speed_zoom && speed_held > speed_from ? pow(speed_mult, (speed_held - speed_from) / SPEED_STEP) : 1.0;
            want *= speed_m;
            if (want > ce) want = ce;
            if (want < fl) want = fl;
            auto_want = want;
            auto_lg = auto_lg_ok ? auto_lg + (log(want) - auto_lg) * (1.0 - exp(-edt / 0.3)) : log(want);
            auto_lg_ok = 1;
            g = exp(auto_lg);                       /* (the auto share of the zoom: it scales the wheel's) */
            g = g > ce ? ce : g < fl ? fl : g;
            if (fabs(log(g)) < 0.02) g = 1.0;       /* (ordinary: just the wheel's zoom) */
            if (fabs(log(g / auto_goal)) > 0.10 || (g == 1.0 && auto_goal != 1.0)) auto_goal = g;
        }
        if (test_mode) {                            /* (test: what the rays cost a frame) */
            LARGE_INTEGER done;
            double ms;
            static double worst, sum;
            static unsigned n;
            QueryPerformanceCounter(&done);
            ms = (double)(done.QuadPart - now.QuadPart) * 1000.0 / clock_freq.QuadPart;
            sum += ms; n++;
            if (ms > worst) worst = ms;
            if (n % 600 == 0) say("test: auto zoom: %.3f ms a frame on average, %.3f ms at most (%u frames)", sum / n, worst, n);
        }
        auto_phase = (auto_phase + 1) % 6;
    } else {
        auto_goal = 1.0;
        auto_lg_ok = 0;
        auto_phase = 0;
        auto_acc = 0.0;
        speed_held = 0.0;
        speed_m = 1.0;
    }
    if (auto_goal > ce) auto_goal = ce;
    if (auto_goal < fl) auto_goal = fl;
    /* eased in log space: 1 s out (at most 60% a second), 0.9 s back in (at most 40% a second) */
    step = (log(auto_goal) - log(auto_factor)) * (1.0 - exp(-dt / (auto_goal < auto_factor ? 1.0 : 0.9)));
    if (step > 0.4 * dt) step = 0.4 * dt;
    if (step < -0.6 * dt) step = -0.6 * dt;
    auto_factor = exp(log(auto_factor) + step);
    if (fabs(log(auto_factor / auto_goal)) < 0.003) auto_factor = auto_goal;
    {   /* the zoom number shown while the auto zoom changes the zoom (as while the wheel does), then fading */
        static int last_pct = -1;
        const int pct = (int)floor(eff_zoom() * 100.0 + 0.5);
        if (step != 0.0 && last_pct >= 0 && pct != last_pct) ind_changed = GetTickCount();
        last_pct = pct;
    }
    if (test_mode && frames % 50 == 0 && (auto_zoom || speed_zoom))
        say("test: auto zoom: wants %.0f%% (how open at the worm %.2f, the ray down %.0f%%%s, speed %.0f, held %.0f: x%.2f), "
            "goal %.0f%%, now %.0f%%, zoom number %.2f", auto_want * 100.0, auto_here, auto_below * 100.0,
            auto_falling ? " (falling: counts)" : "", auto_speed, speed_held, speed_m, zoom_now * auto_goal * 100.0,
            eff_zoom() * 100.0, ind_alpha());
    if (test_mode && (auto_zoom || speed_zoom) && GetEnvironmentVariableA("WKMAPZOOM_TEST_TRACE", NULL, 0) > 0) {   /* (test: the worm's
        path each frame and the land, for working the auto zoom out away from WA) */
        static FILE *tf;
        static int land_dumps;
        char path[MAX_PATH + 32];
        if (!tf) { snprintf(path, sizeof(path), "%s\\trace.csv", dump_dir); tf = fopen(path, "w"); if (tf) fprintf(tf, "frame,ms,ok,wx,wy,zoom\n"); }
        if (tf) {
            fprintf(tf, "%u,%.1f,%d,%.2f,%.2f,%.4f\n", frames, (double)now.QuadPart * 1000.0 / clock_freq.QuadPart, turn_worm_ok,
                    turn_wx, turn_wy, eff_zoom());
            if (frames % 50 == 0) fflush(tf);
        }
        if (land_ok && tile_grid && land_dumps < 2 && (frames == 120 || frames == 2400)) {
            FILE *pf;
            snprintf(path, sizeof(path), "%s\\land-%d.pgm", dump_dir, land_dumps++);
            if ((pf = fopen(path, "wb")) != NULL) {
                int x, y;
                unsigned char *row = (unsigned char *)malloc((size_t)land.w);
                fprintf(pf, "P5\n%d %d\n255\n", land.w, land.h);
                for (y = 0; row && y < land.h; y++) {
                    for (x = 0; x < land.w; x++) row[x] = land_solid(x, y) ? 255 : 0;
                    fwrite(row, 1, (size_t)land.w, pf);
                }
                free(row);
                fclose(pf);
                say("test: land %dx%d written to %s", land.w, land.h, path);
            }
        }
    }
}

/* ---- WA's palette ------------------------------------------------------------------------------------------------ */
/* ---- WA's palette ------------------------------------------------------------------------------------------------ */
static int pal_nearest(const uint8_t *pal, int r, int g, int b) {
    int k, best = 0;
    long bd = 0x7fffffff;
    for (k = 0; k < 256; k++) {
        long dr = pal[4 * k] - r, dg = pal[4 * k + 1] - g, db = pal[4 * k + 2] - b, d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
        if (d < bd) { bd = d; best = k; }
    }
    return best;
}

/* ---- the readout: "150%" in WA's palette ------------------------------------------------------------------------- */
/* the palette's whitest colour, and each colour's darker one (the readout's see-through box) */
static void palette_tables(void *display) {
    const uint8_t *pal = (const uint8_t *)VA(PAL_SHOWN_VA);   /* (as shown: dark while WA fades) */
    int k;
    if (white_index >= 0 && !memcmp(pal, pal_seen, sizeof(pal_seen))) return;
    memcpy(pal_seen, pal, sizeof(pal_seen));
    (void)display;
    white_index = pal_nearest(pal, 255, 255, 255);
    for (k = 0; k < 256; k++)
        dark_lut[k] = (uint8_t)pal_nearest(pal, pal[4 * k] * 30 / 100, pal[4 * k + 1] * 30 / 100, pal[4 * k + 2] * 30 / 100);
}

/* a short message in the readout's place for 2 seconds (a setting switched by a key) */
static void show_message(const char *text) {
    snprintf(ind_msg, sizeof(ind_msg), "%s", text);
    ind_msg_tick = ind_changed = GetTickCount();
}

static int message_on(void) { return ind_msg[0] && GetTickCount() - ind_msg_tick < 2000; }

static double ind_alpha(void) {
    DWORD age = GetTickCount() - ind_changed;
    double a;
    if (message_on()) return age < 1500 ? 1.0 : 1.0 - (age - 1500) / 500.0;
    if (ind_mode == IND_OFF) return 0.0;
    if (zoom_now != zoom_goal) return 1.0;
    a = age < 1500 ? 1.0 : age < 2000 ? 1.0 - (age - 1500) / 500.0 : 0.0;
    if (ind_mode == IND_ALWAYS && fabs(zoom_goal * auto_goal - zoom_clamp(zoom_default, zoom_lo, zoom_hi)) > 0.005 && a < 0.6)
        a = 0.6;                            /* (while not at the default zoom; the auto zoom's share counts too) */
    return a;
}

static int pow2(int v) { int p = 16; while (p < v) p <<= 1; return p; }

/* a badge picture (badge_paint_text's: its box a see-through dark, its text white) into WA's frame at (x0, y0), faded
 * to level / 16 by an ordered dither (WA's frame has no see-through colours); palette_tables first */
static void blit_badge(const uint32_t *img, int tw, int bw, int bh, int x0, int y0, int level) {
    static const uint8_t bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    uint8_t *frame = *(uint8_t **)VA(FRAME_PTR_VA);   /* (the renderer's frame: see queue_hook) */
    const int stride = *(int32_t *)VA(FRAME_W_VA), fw = stride, fh = *(int32_t *)VA(FRAME_H_VA);
    int x, y;
    if (!frame || !img) return;
    for (y = 0; y < bh; y++) {
        uint8_t *row;
        if (y0 + y < 0 || y0 + y >= fh) continue;
        row = frame + (size_t)(y0 + y) * stride;
        for (x = 0; x < bw; x++) {
            uint32_t c = img[y * tw + x], r, gg, b, mx;
            uint8_t *px;
            if (!(c >> 24) || x0 + x < 0 || x0 + x >= fw || bayer[(x & 3) + (y & 3) * 4] >= level) continue;
            r = (c >> 16) & 0xff; gg = (c >> 8) & 0xff; b = c & 0xff;
            mx = r > gg ? (r > b ? r : b) : (gg > b ? gg : b);
            px = row + x0 + x;
            *px = mx > 128 ? (uint8_t)white_index : dark_lut[*px];
        }
    }
}

static void draw_readout(void *display) {
    uint8_t *frame = *(uint8_t **)VA(FRAME_PTR_VA);   /* (the renderer's frame: see queue_hook) */
    int stride = *(int32_t *)VA(FRAME_W_VA), fw = stride, fh = *(int32_t *)VA(FRAME_H_VA);
    int pct = (int)floor(eff_zoom() * 100.0 + 0.5), m, x0, y0, level;
    double a = ind_alpha();
    char text[48];
    if (a <= 0.02 || !frame || fw < 64 || fh < 64) return;
    if (!zoom_ready && !message_on()) return;   /* (no zoom is drawn: no number, only a message) */
    if (message_on()) snprintf(text, sizeof(text), "%s", ind_msg); else snprintf(text, sizeof(text), "%d%%", pct);
    if (strcmp(text, ind_text) || fh != ind_lines || !ind_img) {   /* the picture, made again when its text changes */
        int f;
        badge_size(fh, &f, &ind_bw, &ind_bh);
        if (message_on()) ind_bw = badge_text_width(f, text);
        if (ind_bw > fw - 8) ind_bw = fw - 8;
        if (!ind_img || pow2(ind_bw) != ind_tw || pow2(ind_bh) != ind_th) {
            free(ind_img); free(ind_pre);
            ind_tw = pow2(ind_bw); ind_th = pow2(ind_bh);
            ind_img = (uint32_t *)malloc((size_t)ind_tw * ind_th * 4);
            ind_pre = (uint32_t *)malloc((size_t)ind_tw * ind_th * 4);
            if (!ind_img || !ind_pre) { free(ind_img); free(ind_pre); ind_img = ind_pre = NULL; return; }
        }
        if (!badge_paint_text(text, f, ind_bw, ind_bh, ind_tw, ind_th, ind_img, ind_tw * 4)) { ind_text[0] = 0; return; }
        snprintf(ind_text, sizeof(ind_text), "%s", text);
        ind_pct = pct; ind_lines = fh;
    }
    m = fh * 16 / 1080;
    x0 = ind_place == PLACE_TOP_LEFT || ind_place == PLACE_BOTTOM_LEFT ? m
       : ind_place == PLACE_TOP || ind_place == PLACE_BOTTOM ? (fw - ind_bw) / 2 : fw - ind_bw - m;
    y0 = ind_place >= PLACE_BOTTOM_RIGHT ? fh - ind_bh - fh * 120 / 1080 : (fh * 110 / 1080 > 80 ? fh * 110 / 1080 : 80);
    level = (int)(a * 16.0 + 0.5);          /* (fading) */
    if (d3d.overlay) {                      /* through wkDirect3D: see-through and smooth, over the finished picture */
        const uint32_t fade = (uint32_t)(a * 256.0 + 0.5) > 256 ? 256 : (uint32_t)(a * 256.0 + 0.5);
        int x, y;
        for (y = 0; y < ind_bh; y++)
            for (x = 0; x < ind_bw; x++) {
                const uint32_t c = ind_img[y * ind_tw + x], al = ((c >> 24) * fade) >> 8;
                ind_pre[y * ind_tw + x] = al << 24 | (((c >> 16) & 0xff) * al / 255) << 16 | (((c >> 8) & 0xff) * al / 255) << 8 | ((c & 0xff) * al / 255);
            }
        if (d3d.overlay(ind_pre, ind_bw, ind_bh, ind_tw * 4, x0, y0)) return;
    }
    palette_tables(display);                /* else into WA's frame, in its palette */
    blit_badge(ind_img, ind_tw, ind_bw, ind_bh, x0, y0, level);
}

/* ---- test mode: a scripted zoom ------------------------------------------------------------------------------------ */
static double test_zoom;                /* WKMAPZOOM_TEST_ZOOM: one zoom all along, no zoom script */
static void test_script(void) {
    if (frames % 600 == 0)
        say("test: frame %u: zoom %.1f%% wanted, %.1f%% drawn; %u zoomed frames so far, %u not taken by wkDirect3D", frames, eff_zoom() * 100.0,
            view_now.zoom * 100.0, zoomed_frames, refused_frames);
    if (test_zoom > 0.0) return;
    switch (frames) {
    case 190: set_goal(zoom_lo, "test script"); break;
    case 390: set_goal(zoom_hi, "test script"); break;
    case 590: set_goal(1.0, "test script"); break;
    case 905: case 910: {                   /* numpad + then numpad -, posted to WA's own window (through the message hook) */
        HWND wnd = (HWND)(uintptr_t)*(uint32_t *)VA(0x6b390cu);
        if (wnd) PostMessageA(wnd, WM_KEYDOWN, frames == 905 ? VK_ADD : VK_SUBTRACT, 1);
        break;
    }
    }
}

/* ---- wkDirect3D ---------------------------------------------------------------------------------------------------- */
void mz_gpu_log(const char *line) { say("gpu: %s", line); }   /* (gpu.c's log lines) */

static void d3d_find(void) {
    const HMODULE m = GetModuleHandleA("wkDirect3D.dll");
    if (d3d.begin) return;
    if (gpu_inside) {
        d3d.api = wkDirect3D_Api; d3d.mapmax = wkDirect3D_MapMax; d3d.begin = wkDirect3D_MapBegin; d3d.end = wkDirect3D_MapEnd;
        d3d.overlay = wkDirect3D_Overlay;
        if (fit_ok) fit_limits();
        return;
    }
    if (!m) return;
    d3d.api = (int (__cdecl *)(void))(void *)GetProcAddress(m, "wkDirect3D_Api");
    if (!d3d.api || d3d.api() < 1) { say("wkDirect3D.dll is loaded, but it is an older one without the zoom: delete it (its drawing is part of wkMapZoom now)"); d3d.api = NULL; return; }
    d3d.mapmax = (int (__cdecl *)(int *, int *))(void *)GetProcAddress(m, "wkDirect3D_MapMax");
    d3d.end = (int (__cdecl *)(const wkd3d_view *))(void *)GetProcAddress(m, "wkDirect3D_MapEnd");
    d3d.overlay = (int (__cdecl *)(const uint32_t *, int, int, int, int, int))(void *)GetProcAddress(m, "wkDirect3D_Overlay");
    d3d.begin = (void *(__cdecl *)(int, int, int, int))(void *)GetProcAddress(m, "wkDirect3D_MapBegin");
    if (!d3d.mapmax || !d3d.end || !d3d.begin) { d3d.begin = NULL; say("wkDirect3D.dll is loaded, but its zoom functions are missing"); return; }
    say("wkDirect3D.dll (the older, separate module) is loaded: it draws the zoomed map. It can be deleted: its drawing is part of wkMapZoom now");
    if (fit_ok) fit_limits();
}

/* the view the frame being drawn has, for modules that draw on the map (wkSpeedometer): a map point is at screen point
 * org + (map - cam) x zoom. Set v->size to the structure's size first. 1: filled; 0: no game frame is being drawn */
__declspec(dllexport) int __cdecl wkMapZoom_View(wkmz_view *v) {
    if (!v || v->size < (int)sizeof(*v) || !view_ok) return 0;
    *v = view_now;
    v->size = (int)sizeof(*v);
    return 1;
}

/* ---- the frame --------------------------------------------------------------------------------------------------- */
static void __fastcall hook_render(void *runtime) {
    void *world = runtime ? *(void **)((char *)runtime + RT_WORLD) : NULL;
    void *display = world ? *(void **)((char *)world + WO_DISPLAY) : NULL;
    frame_display = NULL;
    frame_zoom = 1.0;
    view_ok = 0;
    if (!d3d_looked) { d3d_looked = 1; d3d_find(); if (!d3d.begin) say("the graphics card drawing does not run (see the lines above): the map stays at 100%%"); }
    if (display && (uintptr_t)VTBL(display) == VA(DISPLAY_VTABLE_VA) && fit_size(FIELD(display, D_W), FIELD(display, D_H))) {
        const int land_changed = land_check(display);
        const DWORD gap = GetTickCount() - live_tick;   /* (since the last game frame: the lobby between games) */
        if (world != last_world || land_changed == 2 || (land_changed && gap > 1500)) {   /* a new game: the default zoom
                                                           (also when WA put it where the last one was: other land after a
                                                           pause) */
            if (world == last_world) say("a new game in the last one's place (the land %s after %lu ms without game frames)",
                                         land_changed == 2 ? "has other sizes" : "is made of other tiles", (unsigned long)gap);
            last_world = world;
            zoom_now = zoom_goal = zoom_clamp(zoom_default, zoom_lo, zoom_hi);
            frames = 0;
            logged_split = 0;
            clock_last.QuadPart = 0;
            ind_changed = GetTickCount() - 60000;
            land_free();
            turn_reset();
            if (test_mode && test_zoom > 0.0) zoom_now = zoom_goal = zoom_clamp(test_zoom, zoom_lo, zoom_hi);
            auto_factor = auto_goal = 1.0;
            auto_lg_ok = 0;
            track_key = track_turn = -1;
            auto_tn = 0;
            auto_phase = 0;
            auto_acc = 0.0;
            say("a new game (world %p), zoom %.0f%%", world, zoom_goal * 100.0);
        } else if (land_changed) {          /* the same game, its land made of other tile objects: looked up again */
            say("land: WA's tiles are other objects now (mid-game): looked up again");
            land_free();
        }
        if (!land_ok) land_setup(display);
        turn_frame(world);
        auto_zoom_frame();
        frame_display = display;
        zoom_ready = d3d.begin && d3d.mapmax && d3d.mapmax(NULL, NULL);
        frame_zoom = zoom_ready ? eff_zoom() : 1.0;   /* (wkDirect3D not drawing this frame: WA's own picture, at 100%) */
        live_tick = GetTickCount();
    }
    original_render(runtime);
    if (frame_display) {
        frames++;
        draw_readout(display);
        if (test_mode) test_script();
        advance();
    }
    frame_display = NULL;
    frame_zoom = 1.0;
}

/* ---- settings and keys ------------------------------------------------------------------------------------------- */
static char ini_path[MAX_PATH];

/* ---- a key as text: "A", "Ctrl+A", "Alt+F6", "Shift+PageUp", "Numpad +", "Key 220" (a virtual-key number), "none" ---- */
typedef struct { int vk, mods; } keybind;   /* mods: 1 Ctrl, 2 Alt, 4 Shift; vk 0: none */
static const struct { int vk; const char *name; } key_names[] = {
    {VK_SPACE, "Space"}, {VK_TAB, "Tab"}, {VK_RETURN, "Enter"}, {VK_BACK, "Backspace"}, {VK_INSERT, "Insert"}, {VK_DELETE, "Delete"},
    {VK_HOME, "Home"}, {VK_END, "End"}, {VK_PRIOR, "PageUp"}, {VK_NEXT, "PageDown"}, {VK_LEFT, "Left"}, {VK_RIGHT, "Right"},
    {VK_UP, "Up"}, {VK_DOWN, "Down"}, {VK_ADD, "Numpad +"}, {VK_SUBTRACT, "Numpad -"}, {VK_MULTIPLY, "Numpad *"},
    {VK_DIVIDE, "Numpad /"}, {VK_DECIMAL, "Numpad ."}, {VK_PAUSE, "Pause"}, {VK_SCROLL, "ScrollLock"}, {VK_CAPITAL, "CapsLock"}};
static int key_pre(const char *t, const char *p) {   /* t starts with p, whatever the letters' case */
    for (; *p; t++, p++) if ((*t | 0x20) != (*p | 0x20)) return 0;
    return 1;
}
static keybind key_parse(const char *text) {
    keybind k = {0, 0};
    unsigned i;
    int n;
    while (*text == ' ') text++;
    for (;;) {
        if (key_pre(text, "Ctrl+")) { k.mods |= 1; text += 5; }
        else if (key_pre(text, "Alt+")) { k.mods |= 2; text += 4; }
        else if (key_pre(text, "Shift+")) { k.mods |= 4; text += 6; }
        else break;
    }
    if (!text[0] || !lstrcmpiA(text, "none")) { k.mods = 0; return k; }
    if (!text[1] && ((text[0] >= '0' && text[0] <= '9') || ((text[0] | 0x20) >= 'a' && (text[0] | 0x20) <= 'z'))) k.vk = text[0] >= 'a' ? text[0] - 32 : text[0];
    else if ((text[0] | 0x20) == 'f' && sscanf(text + 1, "%d", &n) == 1 && n >= 1 && n <= 24 && strlen(text) <= 3) k.vk = VK_F1 + n - 1;
    else if (key_pre(text, "Numpad ") && text[7] >= '0' && text[7] <= '9' && !text[8]) k.vk = VK_NUMPAD0 + text[7] - '0';
    else if (key_pre(text, "Key ") && sscanf(text + 4, "%d", &n) == 1 && n > 0 && n < 256) k.vk = n;
    else for (i = 0; i < sizeof(key_names) / sizeof(key_names[0]); i++) if (!lstrcmpiA(text, key_names[i].name)) k.vk = key_names[i].vk;
    if (!k.vk) k.mods = 0;
    return k;
}
static int key_mods_now(void) { return (GetKeyState(VK_CONTROL) < 0 ? 1 : 0) | (GetKeyState(VK_MENU) < 0 ? 2 : 0) | (GetKeyState(VK_SHIFT) < 0 ? 4 : 0); }
/* the keys (wkMapZoom.ini; changed in the game through wkModMenu): zoom in, out, back to the default zoom, the auto
 * zoom on / off, and the key held with the wheel and the middle click (1 Ctrl, 2 Alt, 4 Shift) */
static keybind key_in = {VK_ADD, 0}, key_out = {VK_SUBTRACT, 0}, key_reset = {VK_MULTIPLY, 0}, key_auto = {'A', 1};
static int wheel_mod = 1;
static int wheel_held(WPARAM keys) { return wheel_mod == 2 ? GetKeyState(VK_MENU) < 0 : wheel_mod == 4 ? (keys & MK_SHIFT) != 0 : (keys & MK_CONTROL) != 0; }
static int key_is(const MSG *m, keybind k) { return k.vk && (int)m->wParam == k.vk && key_mods_now() == k.mods; }

static void save_setting(const char *key, const char *value) {
    if (test_mode || !ini_path[0]) { say("%s=%s (test mode: not saved)", key, value); return; }
    if (WritePrivateProfileStringA("wkMapZoom", key, value, ini_path)) say("%s=%s saved", key, value);
    else say("%s=%s: not saved (%s, error %lu)", key, value, ini_path, GetLastError());
}

static void set_auto_zoom(int on, const char *why) {
    auto_zoom = on;
    say("auto zoom %s (%s)", on ? "on" : "off", why);
    show_message(on ? "Auto zoom: on" : "Auto zoom: off");
    save_setting("AutoZoom", on ? "1" : "0");
}

static double default_zoom_now(void) { return zoom_clamp(zoom_default, zoom_lo, zoom_hi); }   /* (the one games start at) */

/* the zoom asked for by a key or the wheel: said when it cannot be had */
static void ask_zoom(double z, const char *why) {
    if (!d3d.begin) { show_message("Zoom is off (GraphicsCard=0)"); return; }
    if (!zoom_ready) { show_message("Zoom needs Direct3D 9 (shader)"); return; }   /* (WA's Graphics API; or the frames are WA's own just now) */
    set_goal(z, why);
}

static LRESULT CALLBACK on_message(int code, WPARAM wp, LPARAM lp) {
    MSG *m = (MSG *)lp;
    if (code == HC_ACTION && wp == PM_REMOVE && m && fit_ok && GetTickCount() - live_tick < 1000) {
        if (m->message == WM_MOUSEWHEEL && wheel_held(GET_KEYSTATE_WPARAM(m->wParam))) {
            ask_zoom(zoom_step2(zoom_goal, GET_WHEEL_DELTA_WPARAM(m->wParam) / (double)WHEEL_DELTA, cfg.step, default_zoom_now()),
                     "the wheel");          /* (100% and the default zoom: stops on the way through) */
            m->message = WM_NULL;
        } else if ((m->message == WM_MBUTTONDOWN || m->message == WM_MBUTTONUP || m->message == WM_MBUTTONDBLCLK) &&
                   wheel_held(GET_KEYSTATE_WPARAM(m->wParam))) {
            if (m->message != WM_MBUTTONUP) ask_zoom(zoom_default, "the middle click");
            m->message = WM_NULL;
        } else if ((m->message == WM_KEYDOWN || m->message == WM_SYSKEYDOWN) && key_is(m, key_auto)) {
            if (!(m->lParam & (1 << 30))) {  /* (not the key's repeats) */
                if (!worms_ok) show_message("Auto zoom: unavailable");
                else if (!d3d.begin) show_message("Zoom is off (GraphicsCard=0)");
                else set_auto_zoom(!auto_zoom, "its key");
            }
            m->message = WM_NULL;
        } else if ((m->message == WM_KEYDOWN || m->message == WM_SYSKEYDOWN) && (key_is(m, key_in) || key_is(m, key_out) || key_is(m, key_reset))) {
            if (key_is(m, key_reset)) ask_zoom(zoom_default, "the default zoom's key");
            else ask_zoom(zoom_step2(zoom_goal, key_is(m, key_in) ? 1.0 : -1.0, cfg.step, default_zoom_now()), "a zoom key");
            m->message = WM_NULL;
        }
    }
    return CallNextHookEx(msg_hook, code, wp, lp);
}

/* ---- start-up ---------------------------------------------------------------------------------------------------- */
static int ini_flag(const char *ini, const char *key, int dflt) {
    char v[16];
    if (!GetPrivateProfileStringA("wkMapZoom", key, "", v, sizeof(v), ini) || !v[0]) return dflt;
    return atoi(v) != 0 || !lstrcmpiA(v, "on") || !lstrcmpiA(v, "yes") || !lstrcmpiA(v, "true");
}

static int told_old;
static void read_settings(const char *exe) {
    static const char *const gone[] = {"Speedometer", "SpeedometerPlace", "TurnLine", "TurnLineKeep", "TurnLineColour"};
    static const char *const dropped[] = {"TrueColour", "ColourMaps", "MapTheme", "EscMenu"};
    char ini[MAX_PATH], v[64], *slash;
    unsigned i;
    strcpy(ini, exe);
    slash = strrchr(ini, '\\');
    if (!slash) return;
    strcpy(slash + 1, "wkMapZoom.ini");
    strcpy(ini_path, ini);
#define READ_D(key, field) if (GetPrivateProfileStringA("wkMapZoom", key, "", v, sizeof(v), ini) && v[0]) cfg.field = atof(v)
    READ_D("MaxOut", max_out);
    READ_D("MaxIn", max_in);
    READ_D("Step", step);
#undef READ_D
    cfg.smooth = ini_flag(ini, "Smooth", cfg.smooth);
    if (GetPrivateProfileStringA("wkMapZoom", "Filter", "", v, sizeof(v), ini) && v[0]) filter_smooth = lstrcmpiA(v, "nearest") != 0;
    if (GetPrivateProfileStringA("wkMapZoom", "Indicator", "", v, sizeof(v), ini) && v[0])
        ind_mode = !lstrcmpiA(v, "off") || !strcmp(v, "0") ? IND_OFF : !lstrcmpiA(v, "always") ? IND_ALWAYS : IND_ZOOMING;
    if (GetPrivateProfileStringA("wkMapZoom", "IndicatorPlace", "", v, sizeof(v), ini) && v[0]) {
        static const char *const places[] = {"top-right", "top-left", "top", "bottom-right", "bottom-left", "bottom"};
        for (i = 0; i < sizeof(places) / sizeof(places[0]); i++) if (!lstrcmpiA(v, places[i])) ind_place = (int)i;
    }
    numpad_keys = ini_flag(ini, "NumpadKeys", 1);
    {   /* the keys (NumpadKeys=0, an older file's switch: the three zoom keys are none unless named) */
        static const struct { const char *name, *dflt; keybind *to; int numpad; } keys[] = {
            {"ZoomInKey", "Numpad +", &key_in, 1}, {"ZoomOutKey", "Numpad -", &key_out, 1}, {"ZoomResetKey", "Numpad *", &key_reset, 1},
            {"AutoZoomKey", "Ctrl+A", &key_auto, 0}};
        for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            GetPrivateProfileStringA("wkMapZoom", keys[i].name, keys[i].numpad && !numpad_keys ? "none" : keys[i].dflt, v, sizeof(v), ini);
            *keys[i].to = key_parse(v);
        }
        if (GetPrivateProfileStringA("wkMapZoom", "ZoomModifier", "", v, sizeof(v), ini) && v[0])
            wheel_mod = !lstrcmpiA(v, "alt") ? 2 : !lstrcmpiA(v, "shift") ? 4 : 1;
    }
    if (GetPrivateProfileStringA("wkMapZoom", "DefaultZoom", "", v, sizeof(v), ini) && v[0]) {
        double d = atof(v);
        if (strchr(v, '%') || d > 16.0) d /= 100.0;   /* (0.7, 70% or 70) */
        if (d > 0.0) zoom_default = d < 0.05 ? 0.05 : d > 16.0 ? 16.0 : d;   /* (the zoom range's own limits apply too) */
    }
    auto_zoom = ini_flag(ini, "AutoZoom", 0);
    speed_zoom = ini_flag(ini, "SpeedZoom", 0);
    if (GetPrivateProfileStringA("wkMapZoom", "AutoZoomFallSpeed", "", v, sizeof(v), ini) && v[0]) {
        const double d = atof(v);
        if (d >= 0.0 && d <= 5000.0) auto_fall = d;
    }
    if (GetPrivateProfileStringA("wkMapZoom", "SpeedZoomFrom", "", v, sizeof(v), ini) && v[0]) {
        const double d = atof(v);
        if (d >= 50.0 && d <= 5000.0) speed_from = d;
    }
    if (GetPrivateProfileStringA("wkMapZoom", "SpeedZoomStrength", "", v, sizeof(v), ini) && v[0]) {
        double d = atof(v);
        if (strchr(v, '%') || d > 1.0) d /= 100.0;   /* (0.7, 70% or 70) */
        if (d >= 0.1 && d <= 1.0) speed_mult = d;
    }
    if (GetPrivateProfileStringA("wkMapZoom", "AutoZoomMin", "", v, sizeof(v), ini) && v[0]) {
        auto_min = atof(v);
        if (!(auto_min > 0.0)) auto_min = 0.35;
        if (auto_min > 1.0) auto_min = 1.0;
    }
    if (GetPrivateProfileStringA("wkMapZoom", "AutoZoomMax", "", v, sizeof(v), ini) && v[0]) {
        auto_max = atof(v);
        if (!(auto_max >= 1.0)) auto_max = 1.0;
        if (auto_max > 8.0) auto_max = 8.0;
    }
    for (i = 0; !told_old && i < sizeof(gone) / sizeof(gone[0]); i++)
        if (GetPrivateProfileStringA("wkMapZoom", gone[i], "", v, sizeof(v), ini) && v[0]) {
            say("wkMapZoom.ini has %s=%s: the speedometer and the movement line are the module wkSpeedometer now (its own wkSpeedometer.ini)", gone[i], v);
            break;
        }
    for (i = 0; !told_old && i < sizeof(dropped) / sizeof(dropped[0]); i++)
        if (GetPrivateProfileStringA("wkMapZoom", dropped[i], "", v, sizeof(v), ini) && v[0]) {
            say("wkMapZoom.ini has %s=%s: true colour, full-colour maps and the Esc menu's items are not in wkMapZoom 5 (4.3 had them)", dropped[i], v);
            break;
        }
    told_old = 1;                           /* (said once, not at every re-read) */
    zoom_cfg_sane(&cfg);
}

/* ---- for wkModMenu (github.com/GermsWorms/wkModMenu): what the settings are, and that one was changed in the game ------ */
static char exe_path[MAX_PATH];
__declspec(dllexport) const char *__cdecl wkModSettings(void) {
    return "wkModSettings 1\n"
           "title=wkMapZoom\n"
           "file=wkMapZoom.ini\n"
           "section=wkMapZoom\n"
           "live=1\n"
           "GraphicsCard|The graphics card draws|bool||1|On: the graphics card draws the game's picture, which the zoom needs (WA's Graphics API must be Direct3D 9 (shader)). Off: WA draws by itself, as without the module, and the map stays at 100%. Started with it off, WA has to be started again to switch it on.\n"
           "AutoZoom|Auto zoom|bool||0|The map zooms on its own to fit the space around your worm: out where it opens up, in where it is tight. Ctrl + A switches it too.\n"
           "AutoZoomMin|Auto zoom: out to|pct|0.25:1:0.05|0.35|The furthest the auto zoom and the speed zoom go out, as a share of your own zoom. 100% = never out.\n"
           "AutoZoomMax|Auto zoom: in to|pct|1:3:0.25|1.5|The furthest the auto zoom goes in. 100% = never in.\n"
           "AutoZoomFallSpeed|Auto zoom: falling from|int|0:2000:100|1000|Falling at least this fast (pixels a second), the auto zoom keeps the ground below on screen. 0 = always, above 1600 = never.\n"
           "SpeedZoom|Speed zoom|bool||0|Past a speed the map zooms out, more the faster your worm goes.\n"
           "SpeedZoomFrom|Speed zoom: from|int|400:1600:100|800|The speed the speed zoom starts at, in pixels a second.\n"
           "SpeedZoomStrength|Speed zoom: strength|num|0.5:0.95:0.05|0.7|The zoom is multiplied by this for every 500 px/s over the start speed. Smaller is stronger.\n"
           "DefaultZoom|Default zoom|pct|0.25:4:0.05|1|The zoom each game starts at. Ctrl + middle click and numpad * go back to it. Changed here, it is taken at once.\n"
           "Indicator|Zoom number|choice|zooming,always,off|zooming|The zoom shown as a number: while you zoom, always (while not at the default zoom), or never.\n"
           "IndicatorPlace|Zoom number: place|choice|top-right,top-left,top,bottom-right,bottom-left,bottom|top-right|Where the zoom number is shown.\n"
           "Filter|Picture|choice|smooth,nearest|smooth|smooth: each screen pixel is the average of the map pixels it covers. nearest: plain pixel picking.\n"
           "Smooth|Glide to a new zoom|bool||1|On: the zoom glides to its new value. Off: it jumps.\n"
           "Step|Zoom per wheel notch|num|1.05:2:0.05|1.15|How much one notch of the wheel (or one numpad press) zooms.\n"
           "ZoomModifier|Key held with the wheel|choice|ctrl,alt,shift|ctrl|The key you hold while turning the mouse wheel to zoom (and while clicking the middle button to go back to the default zoom).\n"
           "ZoomInKey|Key: zoom in|key||Numpad +|A key that zooms in one step. Click, then press the key (with Ctrl, Alt or Shift if you like). Delete: none.\n"
           "ZoomOutKey|Key: zoom out|key||Numpad -|A key that zooms out one step.\n"
           "ZoomResetKey|Key: default zoom|key||Numpad *|A key that goes back to the default zoom.\n"
           "AutoZoomKey|Key: auto zoom on / off|key||Ctrl+A|The key that switches the auto zoom.\n"
           "MaxOut|Furthest out|int|1:8:1|4|How far out you can zoom: 4 = 25%, 8 = 12.5% (if the graphics card can make a map that big).\n"
           "MaxIn|Furthest in|int|1:16:1|8|How far in you can zoom: 8 = 800%.\n";
}
__declspec(dllexport) void __cdecl wkModSettingsChanged(void) {
    const double was_default = zoom_default, was_out = cfg.max_out, was_in = cfg.max_in;
    if (!exe_path[0]) return;
    read_settings(exe_path);
    gpu_settings_changed();
    if (cfg.max_out != was_out || cfg.max_in != was_in) fit_ok = 0;   /* (the zoom range: worked out again at the next frame) */
    if (zoom_default != was_default) set_goal(zoom_default, "its default changed in the Mods menu");
    ind_changed = GetTickCount();
    say("settings read again (changed in the Mods menu): auto zoom %s (%.0f%% .. %.0f%%), speed zoom %s, default zoom %.0f%%, zoom number %s",
        auto_zoom ? "on" : "off", auto_min * 100.0, auto_max * 100.0, speed_zoom ? "on" : "off", zoom_default * 100.0,
        ind_mode == IND_OFF ? "off" : ind_mode == IND_ALWAYS ? "always" : "while zooming");
}

static int runs_without_screen(void) {
    const char *line = GetCommandLineA();
    static const char *const switches[] = {"/getvideo", "/getlog", "/getmap", "/getscheme", "/sanitise", "/sanitize", "/repair"};
    unsigned i;
    for (i = 0; i < sizeof(switches) / sizeof(switches[0]); i++)
        if (line && strstr(line, switches[i])) return 1;
    return 0;
}

typedef struct { uint32_t va; const unsigned char *bytes; unsigned n; void *detour; void **orig; const char *name; } hook_def;

/* Pins the module that holds addr (another WormKit module whose hook ours chains onto: if it were unloaded, our call
 * into it would land in freed memory); WA itself, Windows' own DLLs and this module are left alone. */
typedef BOOL (WINAPI *handle_ex_fn)(DWORD, LPCSTR, HMODULE *);   /* (GetModuleHandleExA: behind _WIN32_WINNT >= XP in
                                                                    this MinGW's headers; flags PIN 1, UNCHANGED 2,
                                                                    FROM_ADDRESS 4) */
static handle_ex_fn handle_ex(void) {
    return (handle_ex_fn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetModuleHandleExA");
}
static void pin_owner(const void *addr, const char *what) {
    const handle_ex_fn ex = handle_ex();
    HMODULE m = NULL, pinned;
    char name[MAX_PATH] = "", win[MAX_PATH] = "";
    UINT wn;
    if (!ex || !addr || !ex(0x2 | 0x4, (LPCSTR)addr, &m) || !m) return;
    if ((uintptr_t)m == base || m == self || !GetModuleFileNameA(m, name, sizeof(name))) return;
    wn = GetSystemWindowsDirectoryA(win, sizeof(win));
    if (wn && wn < sizeof(win)) {           /* (Windows' own DLLs stay loaded anyway) */
        UINT k;
        for (k = 0; k < wn && name[k] && (name[k] | 0x20) == (win[k] | 0x20); k++) {}
        if (k == wn) return;
    }
    if (ex(0x1 | 0x4, (LPCSTR)addr, &pinned)) say("%s chains on to %s: pinned too, so it stays loaded", what, name);
}

/* The camera follow (0054 2F70), hooked at WA's two calls of it (00554636 in the per-frame camera update, 0055A4B8), not
 * at its start: another camera module may hook its start too (a friend's set had wkTrackMeBetter381) -- it then sees
 * WA's own bytes there, not a jump of ours, and wkMapZoom no longer stands down because of it; our thunk goes on into
 * whatever is at the start. A call another module has already redirected is left alone (logged). */
static int follow_sites_install(void) {
    static const uint32_t sites[2] = {0x554636u, 0x55a4b8u};
    const uintptr_t target = VA(FOLLOW_VA);
    int k, done = 0;
    orig_follow = (void *)target;
    for (k = 0; k < 2; k++) {
        uint8_t *at = (uint8_t *)VA(sites[k]);
        DWORD old;
        if (at[0] != 0xe8 || (uintptr_t)(at + 5) + (uintptr_t)*(int32_t *)(at + 1) != target) {
            say("WA's camera follow call at %08X goes elsewhere (another module's?): left alone", sites[k]);
            continue;
        }
        if (!VirtualProtect(at + 1, 4, PAGE_EXECUTE_READWRITE, &old)) continue;
        *(int32_t *)(at + 1) = (int32_t)((uintptr_t)(void *)thunk_follow - (uintptr_t)(at + 5));
        VirtualProtect(at + 1, 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), at, 5);
        done++;
    }
    return done;
}

static void install(void) {
    static const unsigned char render_bytes[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x83, 0xec, 0x20};
    static const unsigned char queue_bytes[] = {0x83, 0xec, 0x1c, 0x53, 0x55, 0x8b, 0x6c, 0x24, 0x28};
    static const unsigned char clamp_bytes[] = {0xc1, 0xe0, 0x10, 0x99, 0x2b, 0xc2, 0x53};
    static const unsigned char order_bytes[] = {0x8b, 0x44, 0x24, 0x04, 0x8b, 0x08, 0x8b, 0x54, 0x24, 0x08};
    const hook_def hooks[] = {
        {GAME_RENDER_VA, render_bytes, sizeof(render_bytes), (void *)hook_render, (void **)&original_render, "GameRender"},
        {QUEUE_VA, queue_bytes, sizeof(queue_bytes), (void *)thunk_queue, &orig_queue, "RenderDrawingQueue"},
        {CLAMP_VA, clamp_bytes, sizeof(clamp_bytes), (void *)thunk_clamp, &orig_clamp, "the camera clamp"},
        /* (the camera follow: at its two calls, see follow_sites_install) */
    };
    char exe[MAX_PATH], path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof(exe));
    MH_STATUS s;
    unsigned i, taken = 0;
    if (GetEnvironmentVariableA("LOCALAPPDATA", path, sizeof(path))) {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        strcat(path, "\\wkMapZoom");
        CreateDirectoryA(path, NULL);
        strcat(path, "\\wkMapZoom.log");
        if (GetFileAttributesExA(path, GetFileExInfoStandard, &fa) && fa.nFileSizeLow > (2u << 20)) DeleteFileA(path);
        log_file = fopen(path, "a");
    }
    {   /* Steam's WA.exe 3.8.1, or another with the very same code and layout (GOG's): the hook points are checked too */
        char why[256] = "WA.exe's path unknown";
        const int kind = n && n < sizeof(exe) ? wa_supported_image(exe, why, sizeof(why)) : 0;
        if (!kind) { say("not the WA 3.8.1 this module knows (%s): not installed", why); return; }
        if (kind == 2) say("%s: not Steam's file, but the same game (GOG's edition?)", why);
    }
    if (runs_without_screen()) { say("a /get... or repair run: not installed"); return; }
    base = (uintptr_t)GetModuleHandleA(NULL);
    for (i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++) {
        const uint8_t *at = (const uint8_t *)VA(hooks[i].va);
        if (*at == 0xe9) taken++;
        else if (memcmp(at, hooks[i].bytes, hooks[i].n)) { say("unexpected code at %08X (%s): not installed", hooks[i].va, hooks[i].name); return; }
    }
    if (memcmp((void *)VA(QUEUE_ORDER_VA), order_bytes, sizeof(order_bytes))) { say("unexpected code at 00542330: not installed"); return; }
    if (taken) {
        unsigned stale = 0;                 /* (jumps into memory with no code in it: a module unloaded with its hooks
                                               still in WA) */
        for (i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++) {
            const uint8_t *at = (const uint8_t *)VA(hooks[i].va);
            MEMORY_BASIC_INFORMATION mi;
            if (*at == 0xe9 && VirtualQuery(at + 5 + *(const int32_t *)(at + 1), &mi, sizeof(mi)) &&
                (mi.State != MEM_COMMIT || !(mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))))
                stale++;
        }
        if (stale)
            say("%u of WA's functions still jump into a module that is no longer loaded (an earlier copy of wkMapZoom, "
                "unloaded?): WA will crash there -- restart WA, and keep one copy of wkMapZoom", stale);
        else {
            /* The zoom needs those functions to itself. The graphics-card drawing does not (it goes in through WA's
             * function tables when the first game is on the screen), so it still runs: the picture at 100%, drawn
             * faster. A second copy of this module finds the first one's drawing and stays out of that too. */
            const handle_ex_fn ex = handle_ex();
            HMODULE me;
            say("another module hooks WA's drawing queue or camera too (another copy of wkMapZoom, wkVista, wkJellyWorm or "
                "WormForge's zoom?): the zoom is not installed -- use one zoom module, and one copy of it, at a time");
            strcpy(exe_path, exe);
            read_settings(exe);
            if (ex && ex(0x1 | 0x4, (LPCSTR)(void *)&install, &me)) gpu_inside = gpu_start(self, ini_path);
            say("wkMapZoom " VERSION ": no zoom in this run of WA; the graphics card drawing: %s",
                gpu_inside ? "starts with the first game" : "does not run");
        }
        return;
    }
    strcpy(exe_path, exe);
    read_settings(exe);
    QueryPerformanceFrequency(&clock_freq);
    test_mode = GetEnvironmentVariableA("WKMAPZOOM_TEST", path, sizeof(path)) > 0;
    if (test_mode) {
        DWORD k = GetEnvironmentVariableA("WKMAPZOOM_DUMP", dump_dir, sizeof(dump_dir));
        char res[32];
        int tw = 960, th = 540;
        if (!k || k >= sizeof(dump_dir)) {
            GetEnvironmentVariableA("LOCALAPPDATA", dump_dir, sizeof(dump_dir) - 32);
            strcat(dump_dir, "\\wkMapZoom\\test");
        }
        CreateDirectoryA(dump_dir, NULL);
        if (GetEnvironmentVariableA("WKMAPZOOM_TEST_RES", res, sizeof(res)) && sscanf(res, "%dx%d", &tw, &th) == 2 &&
            (tw < 640 || th < 480 || tw > 7680 || th > 4320)) { tw = 960; th = 540; }
        if (GetEnvironmentVariableA("WKMAPZOOM_TEST_ZOOM", res, sizeof(res)) > 0) test_zoom = atof(res);
        if (GetEnvironmentVariableA("WKMAPZOOM_TEST_AUTOZOOM", res, sizeof(res)) > 0) auto_zoom = 1;
        if (GetEnvironmentVariableA("WKMAPZOOM_TEST_SPEEDZOOM", res, sizeof(res)) > 0) speed_zoom = 1;
        *(int32_t *)VA(OPT_WINDOWED_VA) = 1;
        *(int32_t *)VA(OPT_DISPLAY_X_VA) = tw; *(int32_t *)VA(OPT_DISPLAY_Y_VA) = th;
        *(int32_t *)VA(OPT_WINDOW_X_VA) = tw; *(int32_t *)VA(OPT_WINDOW_Y_VA) = th;
        *(int32_t *)VA(OPT_RENDERER_VA) = GetEnvironmentVariableA("WKMAPZOOM_TEST_RENDERER", res, sizeof(res)) > 0 ? atoi(res) : 4;
        say("test mode: windowed %dx%d, %s", tw, th, test_zoom > 0.0 ? "one zoom all along" : "scripted zoom");
    }
    say("WA starts at %dx%d, %s, renderer %d (its options as read)", *(int32_t *)VA(OPT_DISPLAY_X_VA), *(int32_t *)VA(OPT_DISPLAY_Y_VA),
        *(int32_t *)VA(OPT_WINDOWED_VA) ? "windowed" : "fullscreen", *(int32_t *)VA(OPT_RENDERER_VA));
    {   /* Pinned before any of WA's code points into this DLL (its hooks, its message hook), so it is never unloaded
         * while WA runs: an unload is a no-op and a reload gets this very copy back, hooks and all. Not pinned, it
         * doesn't install. */
        const handle_ex_fn ex = handle_ex();
        HMODULE me;
        if (!ex || !ex(0x1 | 0x4, (LPCSTR)(void *)&install, &me)) {
            say("could not pin the module in memory (error %lu): not installed", GetLastError());
            return;
        }
    }
    gpu_inside = gpu_start(self, ini_path);  /* the graphics-card drawing: started with the first game */
    d3d_find();                             /* (its functions; or an old wkDirect3D.dll's, which loads before this module) */
    s = MH_Initialize();
    for (i = 0; s == MH_OK && i < sizeof(hooks) / sizeof(hooks[0]); i++)
        if ((s = MH_CreateHook((void *)VA(hooks[i].va), hooks[i].detour, hooks[i].orig)) != MH_OK)
            say("could not hook %s (%d)", hooks[i].name, (int)s);
    if (s == MH_OK) {
        {                                   /* (the team tasks: only on WA 3.8.1's own code, or another module's hook) */
            static const unsigned char team_bytes[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x64, 0xa1, 0x00, 0x00, 0x00, 0x00};
            static const unsigned char physics_bytes[] = {0x83, 0xec, 0x08, 0x53, 0x8b, 0x5c, 0x24, 0x14};
            const uint8_t *tp = (const uint8_t *)VA(TEAM_HANDLER_VA), *pp = (const uint8_t *)VA(PHYSICS_VA);
            if ((memcmp(tp, team_bytes, sizeof(team_bytes)) && *tp != 0xe9) ||
                (memcmp(pp, physics_bytes, sizeof(physics_bytes)) && *pp != 0xe9) ||
                MH_CreateHook((void *)tp, (void *)thunk_team, &orig_team) != MH_OK) {
                say("the auto zoom and the speed zoom are unavailable");
            } else if (MH_CreateHook((void *)pp, (void *)thunk_physics, &orig_physics) != MH_OK) {
                MH_RemoveHook((void *)tp);
                say("the auto zoom and the speed zoom are unavailable");
            } else {
                worms_ok = 1;
                /* (another module's hook already there: ours chains onto it, so it is kept loaded) */
                if (*tp == 0xe9) pin_owner(tp + 5 + *(const int32_t *)(tp + 1), "the team tasks' hook");
                if (*pp == 0xe9) pin_owner(pp + 5 + *(const int32_t *)(pp + 1), "the physics step's hook");
            }
        }
        {                                   /* (the fills across the view: only on WA's own code; without it the zoom still works) */
            static const unsigned char fill_bytes[] = {0x8b, 0x81, 0x60, 0x35, 0x00, 0x00, 0x01, 0x44, 0x24, 0x04};
            if (memcmp((void *)VA(FILL_VA), fill_bytes, sizeof(fill_bytes)) ||
                MH_CreateHook((void *)VA(FILL_VA), (void *)hook_fill, (void **)&original_fill) != MH_OK) {
                original_fill = NULL;
                say("the display's fill (0056B810) is not WA's own code (another module's hook?): zoomed far out, the water and the sky end where WA ends them");
            }
        }
        s = MH_EnableHook(MH_ALL_HOOKS);
        if (s == MH_OK && follow_sites_install() < 2) say("the zoomed-in camera follow works at some of WA's calls only");
    }
    installed = s == MH_OK;
    if (installed) {
        msg_hook = SetWindowsHookExA(WH_GETMESSAGE, on_message, self, GetCurrentThreadId());
        if (!msg_hook) say("could not watch the mouse wheel (error %lu)", GetLastError());
    }
    say("wkMapZoom " VERSION " %s (base %08x): Ctrl + wheel zooms the map (up to %.0f%% out, %.0f%% in), default zoom %.0f%% (Ctrl + "
        "middle click%s); auto zoom %s, speed zoom %s; the graphics card drawing: %s",
        installed ? "ready" : "failed", (unsigned)base, 100.0 / cfg.max_out, cfg.max_in * 100.0, zoom_default * 100.0,
        numpad_keys ? ", numpad + / - / *" : "",
        worms_ok ? (auto_zoom ? "on (Ctrl + A switches it)" : "off (Ctrl + A switches it)") : "unavailable",
        worms_ok ? (speed_zoom ? "on" : "off") : "unavailable", gpu_inside ? "starts with the first game" : d3d.begin ? "an old wkDirect3D.dll's" : "does not run");
    {   /* (which file this is, and where it lies: two copies, or one unloaded, show in the log) */
        char me[MAX_PATH] = "";
        GetModuleFileNameA(self, me, sizeof(me));
        say("this module: %s at %08x, pinned (it stays loaded until WA exits)", me, (unsigned)(uintptr_t)self);
    }
}

BOOL WINAPI DllMain(HINSTANCE dll, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) { self = dll; DisableThreadLibraryCalls(dll); install(); }
    else if (reason == DLL_PROCESS_DETACH) {   /* (pinned: only at WA's exit, the hooks still live -- so no say() after) */
        if (msg_hook) UnhookWindowsHookEx(msg_hook);
        msg_hook = NULL;
        if (log_file) fclose(log_file);
        log_file = NULL;
    }
    return TRUE;
}
