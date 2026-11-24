/* wkMapZoom: hold Ctrl and turn the mouse wheel (or numpad + / -) to zoom the map in and out in Worms Armageddon 3.8.1
 * (a WormKit module). The HUD, the chat, the Esc menu and the weapon panel stay their normal size; a small "150%" shows
 * while you zoom. Also: true colour on WA's Direct3D 9 renderers, full-colour PNG / JPEG maps (colourmap.c), a
 * Resolution dropdown, Fullscreen / Windowed switch and the module's options in WA's Esc menu (escmenu.c), the movement
 * line (where the worm whose turn starts went before, in red, until it moves or all turn), and an auto zoom (Ctrl + A:
 * out where it opens up, in where it is tight).
 *
 * How it works. Each frame WA's GameRender (00533DC0) has its tasks queue what they draw (the drawing queue, world
 * +0x524) and then draws the queue back to front (RenderDrawingQueue 00542350). Entries of kinds 1, 4 and 7 are placed
 * on the screen (the HUD); every other kind goes through the camera (the map: sky, land, water, worms, weapons, names).
 * While zoomed, the module has WA draw the queue one entry at a time (the split wkVista makes, MIT, Copyright (c) 2026
 * ropahektic, github.com/ropahektic/wkVista). For the map entries the display's size, origin and clip describe the
 * whole area the zoomed view needs (the "region": at 25% four times the screen each way), so WA's own code works out
 * everything that is to be drawn there -- and each drawing call it makes (WA's bitmap blit, rectangle fills, sprite
 * blits, pixels, lines: the seven functions wkVista draws through) is drawn by the module straight onto the screen at
 * the zoom, each screen pixel the average of the map pixels it covers (raster.c). WA's land tiles come from shrunk
 * copies kept up to date with the land, so zoomed out the land costs about what it costs at 100%. The map ends up in
 * an RGB picture: it goes into WA's 8-bit frame as the nearest palette colours, then WA draws the HUD entries on top as
 * ever, and on a Direct3D 9 renderer the frame is shown in true colour (present.c; the map's pixels as averaged, the
 * HUD's as WA drew them). WA's camera clamp (00542F10) is given the size the zoomed view sees, so the camera reaches the
 * level's edges zoomed in, and WA's camera follow (00542F70) the zoomed-in view's size, so the camera keeps up with the
 * worm (at 100% and below it follows as ever). Only what is shown changes -- no game state -- so online games and
 * replays stay in step, and other players don't need the module.
 *
 * Hooks (WA 3.8.1 only: exact SHA-256 of WA.exe and the original bytes checked at each hook point):
 *   00533DC0 GameRender (ECX the runtime; ret): the frame's zoom, then the readout
 *   00542350 RenderDrawingQueue (EAX the queue; display, camera; ret 8): the map entries, then the HUD entries
 *   0056A700 DisplayGfx::BlitBitmapClipped (EAX display, EDX width; x, y, height, bitmap, flags; ret 0x14)
 *   0056B810 the display's rectangle fill (ECX display; x1, y1, x2, y2 from its origin, colour; ret 0x14)
 *   004F6910 BitGrid::BlitSpriteRect (ESI target; x, y, w, h, source, sx, sy, blend table, mode; ret 0x24)
 *   004F9180 / 004F5E40 BitGrid pixel, clipped / not (ECX grid; x, y, colour; ret 0xC)
 *   004F9110 BitGrid vertical line (ECX grid; x, y1, y2, colour; ret 0x10)
 *   004F5D50 BitGrid rectangle fill (ECX grid; x1, y1, x2, y2, colour; ret 0x14)
 *   00542F10 ClampCameraToBounds (EAX view width, ECX bottom; context, view height, left, top, right; ret 0x14)
 *   00542F70 the camera follow (EAX half the view's width, EDI the camera; half its height, the box it follows)
 *   00557310 a team task's message handler, 004FCDF0 an object's physics step: the movement line (they only look)
 *   a WH_GETMESSAGE hook on WA's own thread: Ctrl + wheel, numpad + / - / *, Ctrl + middle click (taken out of WA's
 *   queue); and present.c, colourmap.c, escmenu.c's own
 *
 * Settings (all optional) in wkMapZoom.ini beside WA.exe, section [wkMapZoom]: MaxOut, MaxIn, Step, Smooth, Filter,
 * Indicator, IndicatorPlace, NumpadKeys, DefaultZoom, TrueColour, ColourMaps, MapTheme, EscMenu, TurnLine, TurnLineKeep,
 * TurnLineColour, AutoZoom, AutoZoomMin, AutoZoomMax, AutoZoomFallSpeed, SpeedZoom, SpeedZoomFrom, SpeedZoomStrength, Speedometer,
 * SpeedometerPlace (see the README; the Esc menu's options write theirs back).
 * Log: %LOCALAPPDATA%\wkMapZoom\wkMapZoom.log.
 *
 * Test mode (environment WKMAPZOOM_TEST=1, for an isolated copy of WA only): the in-memory options become windowed
 * 960x540 on Direct3D 9 -- the registry is not written -- and a scripted zoom runs (100% -> farthest out -> closest in
 * -> 100%) with WA's frame saved as BMP files (WKMAPZOOM_DUMP, else %LOCALAPPDATA%\wkMapZoom\test); WKMAPZOOM_TEST_RES=WxH
 * sets the window size, WKMAPZOOM_TEST_SWITCH=WxH a resolution switched to through the Esc menu's code (not saved),
 * WKMAPZOOM_TEST_ESC=1 the Esc menu and its list opened (=options: its options, two of them clicked, not saved), WKMAPZOOM_TEST_FULLCOLOUR=1 made-up full colours for the land,
 * WKMAPZOOM_TEST_ZOOM=z one zoom all along (no zoom script), WKMAPZOOM_TEST_AUTOZOOM=1 the auto zoom on (its frames saved
 * every 10 s), WKMAPZOOM_TEST_TRACE=1 with it the worm's path (trace.csv) and the land (land-N.pgm) for working the auto
 * zoom out away from WA, WKMAPZOOM_TEST_OPLOG=1 two frames' drawing calls logged (their kinds and sizes on the screen);
 * WKMAPZOOM_TEST_TOGGLE=1 the Esc menu's display switch at frame 940 and back at 1000, WKMAPZOOM_TEST_GONE=1 30 frames
 * shown as with no Direct3D device (the safety net's skip) at 1040, WKMAPZOOM_TEST_RECOVER=1 the device made anew as the
 * recovery does at 1100, WKMAPZOOM_TEST_FULLSCREEN=1 (or WxH) a fullscreen start, WKMAPZOOM_TEST_BADSIZE=WxH a size that
 * is no monitor mode asked for at 1160 (fullscreen: its rollback), WKMAPZOOM_TEST_CHAT=1 chat lines before and after
 * the switch with the chat's layout checked (=off: the chat's re-layout off, to see the old way);
 * a movement line's first two showings are saved too, and the speedometer at the first three moments over 700 px/s;
 * WKMAPZOOM_TEST_DUMPAT=700,1200 saves those frames (at-<frame>) with where the turn's worm is on the screen;
 * WKMAPZOOM_TEST_RENDERER=3 another of WA's renderers (3 and 4: Direct3D 9). Every 150th frame with the speedometer over
 * WA's own frame is read back from the back buffer and compared (present.c).
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
#include <emmintrin.h>
#include "MinHook.h"
#include "image_guard.h"
#include "zoom_math.h"
#include "zoom_badge.h"
#include "speedo.h"
#include "raster.h"
#include "present.h"
#include "colourmap.h"
#include "escmenu.h"

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
#define FOLLOW_VA 0x542f70u
#define BITMAP_VA 0x56a700u
#define FILL_VA 0x56b810u
#define GRID_VA 0x4f6910u
#define PIXEL_CLIPPED_VA 0x4f9180u
#define PIXEL_VA 0x4f5e40u
#define VLINE_VA 0x4f9110u
#define GRID_FILL_VA 0x4f5d50u
#define TEAM_HANDLER_VA 0x557310u       /* a team task's message handler (thiscall, ECX the task) */
#define PHYSICS_VA 0x4fcdf0u            /* an object's physics step (its manager, the object; every object, every step) */
#define WORM_VTABLE_VA 0x6644c8u        /* a worm: +0x2C the game, +0x84 / +0x88 its place (1/65536 px), +0xFC its team,
                                           +0x100 its number in the team */
#define QSORT_VA 0x5d7890u              /* WA's qsort (cdecl) */
#define QUEUE_ORDER_VA 0x542330u        /* the queue's order (by layer: +4) */
#define DISPLAY_VTABLE_VA 0x66a218u     /* the game display (DDDisplay) */
#define FRAME_PTR_VA 0x7a0eecu          /* the renderer's 8-bit frame: its pixels, width (= row stride), height. The display
                                           takes its grid from these at each lock (0056A370), and its fills (0056B810 ->
                                           005A25C0) and bitmap blits draw into them directly */
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
#define SENT 0xff                       /* the region buffer's "nothing drawn" (for WA's own drawing of what the module
                                           doesn't draw itself) */

#define VA(a) ((uintptr_t)(base + ((uintptr_t)(a) - WA_BASE)))
#define FIELD(p, off) (*(int32_t *)((char *)(p) + (off)))
#define VTBL(obj) (*(void ***)(obj))

static uintptr_t base;
static FILE *log_file;
static int installed, test_mode, test_fullcolour, test_esc;
static int test_checks;                 /* (test mode, unless WKMAPZOOM_TEST_NOCHECK: the costly self-checks, off for timing) */
static char dump_dir[MAX_PATH];
static HINSTANCE self;
static zoom_cfg cfg = ZOOM_CFG_DEFAULT;

/* the game resolution S (the display's own size); the region buffer G (what WA's own drawing reaches while the map is
 * drawn: normally nothing); the map as drawn this frame (RGB, S) */
static int sw, sh, gw, gh;
static uint8_t *wbuf;
static uint32_t *rgb;
static uint8_t *qcopy;                  /* the palette indices the map went into WA's frame as (the true-colour frame's) */
#define MAX_HELPERS 15
static rtap *frame_tx[RASTER_LEVELS + 1], *frame_ty[RASTER_LEVELS + 1];   /* the frame's taps per level (raster.h) */
static struct { int ok, p0, p1, o, point; double b, z, off; } taps_x[RASTER_LEVELS + 1], taps_y[RASTER_LEVELS + 1];
                                        /* (what they were made for: kept while the view doesn't change) */
static uint32_t *lut;                   /* (palette generation << 8) | index, by 18-bit colour */
static uint32_t lut_gen = 1, pal_gen = 1, pal32[256];
static uint8_t lut_pal[1024];           /* WA's palette as the tables were made for it */
static uint32_t ex_key[1024];           /* the palette's own colours (1 << 24 | colour) -> their index: exact */
static uint8_t ex_idx[1024];
static int filter_smooth = 1, numpad_keys = 1, true_colour = 1, colour_maps = 1, esc_menu = 1, map_theme = 11;
static int tc_ok;                       /* the true-colour hooks are on */
static int present_ok;                  /* the show-frame hooks are on (true colour, and the display safety net) */
static double zoom_now = 1.0, zoom_goal = 1.0, zoom_lo = 1.0, zoom_hi = 1.0;
static double zoom_default = 1.0;       /* DefaultZoom=: each game starts at it; Ctrl + middle click and numpad * go back
                                           to it (within the zoom range) */
static double frame_zoom = 1.0;         /* the zoom this frame is drawn at (fixed at GameRender's start) */
static double eff_zoom(void);           /* the zoom shown: the wheel's times the auto zoom's share */
static void *frame_display, *last_world, *frame_grid;
static LARGE_INTEGER clock_freq, clock_last;
static DWORD live_tick;                 /* GetTickCount of the last GameRender */
static unsigned frames;                 /* GameRenders this game */
static HHOOK msg_hook;
static unsigned logged_split;

/* the map drawing this frame */
static rview rv;
static int world_mode, rw_now, rh_now;
static struct { int on, x1, y1, x2, y2; } tc;   /* this frame's map pixels [x1, x2) x [y1, y2) are rgb's */
static struct { int on, l, t, r, b; } pend;    /* WA's own drawing of an operation into the region buffer, to copy */
static unsigned n_ops, n_land, n_refused, n_refused_logged;
static LONGLONG t_sync, t_land, t_ops;      /* this frame's time in the land copies' upkeep, the land, the other calls */
static LONGLONG t_line;                     /* ... and the movement line */
static LONGLONG t_compose;                  /* the true-colour frames' composing (in WA's show frame), summed */
static unsigned n_compose;
static unsigned trail_drops;                /* (bumped when a line's oldest points are dropped, or all lines cleared: a
                                               kept line mask is out of date) */
static LONGLONG qpc(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

/* the land */
typedef struct { uintptr_t bm; int x, y; } tile_ref;
static rland land;
static tile_ref *tiles;
static uintptr_t *tile_grid;            /* the land's tile bitmaps by place (row by row) */
static int ntiles, land_ok;
static void *land_terrain;
static uint8_t *land_snap;
static uint32_t *land_full;
static rop *batch;                      /* land tiles waiting to be drawn together */
static int nbatch, cap_batch;

/* the readout */
enum { IND_OFF, IND_ZOOMING, IND_ALWAYS };
static int ind_mode = IND_ZOOMING;
enum { PLACE_TOP_RIGHT, PLACE_TOP_LEFT, PLACE_TOP, PLACE_BOTTOM_RIGHT, PLACE_BOTTOM_LEFT, PLACE_BOTTOM };
static int ind_place = PLACE_TOP_RIGHT;
static DWORD ind_changed;               /* GetTickCount of the last zoom change */
static uint32_t *ind_img;               /* the readout's picture (ARGB, ind_tw x ind_th) for ind_text */
static int ind_tw, ind_th, ind_bw, ind_bh, ind_pct = -1, ind_lines;
static char ind_text[48], ind_msg[48];  /* the picture's text; a message shown instead of the number for a while */
static DWORD ind_msg_tick;


/* the movement line: where each worm went this game (TurnLine=all) or in its last turn (TurnLine=last), shown when its
 * turn starts */
#define TRAIL_TEAMS 16
#define TRAIL_PER_TEAM 16
#define TRAIL_WORMS 64
#define TRAIL_MAX 32768
typedef struct { float x, y; int32_t turn; int8_t brk, own; uint16_t spd; } tpoint;   /* turn: the game's turn it was
                                                               in; brk: not joined to the point before (a jump); own: in
                                                               the worm's own turn (or right after it, before the next
                                                               team's); spd: the worm's speed there (px/s, the game's) */
static int trail_colour = 1;            /* TurnLineColour=: 1 speed (the speedometer's colours: blue slow, red from the
                                           speed zoom's start), 0 red */
static double speed_from;               /* (SpeedZoomFrom: defined with the speed zoom) */
typedef struct { tpoint *pt; int n, cap; } team_trail;
static team_trail *trails;              /* by worm: its team (a team task's +0x38) * TRAIL_PER_TEAM + its number in it */
static uintptr_t team_tasks[TRAIL_TEAMS], worm_objs[TRAIL_WORMS];
static int worms_ok;                    /* the team task and physics step hooks are on (the line, the auto zoom) */
/* the Esc menu's options (Map zoom options), in their order: general (with the movement line), the auto zoom's, speed
 * (the speedometer and the speed zoom) -- a page each when WA's menu has no room for all (escmenu.c) */
enum { OPT_DEFAULT, OPT_NUMBER, OPT_LINE, OPT_LINE_COLOUR, OPT_AUTO, OPT_AUTO_OUT, OPT_AUTO_IN, OPT_SPEEDO, OPT_SPEEDO_PLACE,
       OPT_SPEED, OPT_SPEED_FROM, OPT_SPEED_STRENGTH, OPT_COUNT };   /* (12: escmenu.c's MAX_OPTS) */
static int spd_cam, spd_cx, spd_cy, spd_ox, spd_oy;   /* WA's camera transform this frame (the speedometer's: see */
static double spd_z;                                  /* queue_hook); spd_cam: seen this frame */
static int turn_worm_ok;                /* where the turn's worm is, this frame */
static double turn_wx, turn_wy;
static double turn_gv, turn_gvy;        /* its speed as the game has it, and its downward part (px/s; team_worm) */
/* The speedometer (the owner, 2026-11-22; speedo.h): the turn's worm's speed, as a digital dial or the number alone,
 * in the screen's bottom right corner (above WA's wind bar) or just above the worm's name. Speedometer= dial / number /
 * off, SpeedometerPlace= corner / worm, and the Esc menu */
static int speedo = 1, speedo_place;    /* 0 off, 1 dial, 2 number; 0 the corner, 1 above the worm */
static double spd_now, spd_disp, spd_best, spd_dist, spd_time;  /* the speed; shown (eased); the turn's best, and how
                                                                  far and how long it moved (the average) */
static double spd_lx, spd_ly, spd_moved;   /* where the worm was last frame; when it last moved (s) */
static int spd_turn = -1, spd_will;     /* spd_will: the speedometer is drawn this frame (set before WA draws it:
                                           queue_hook takes the module's path for it) */
static int turn_key = -1, track_key = -1, track_turn = -1;   /* the turn's worm (team x 64 + its number), and whose
                                                                and which turn's the track is */
/* a picture over the true-colour frame (compose blends it in: premultiplied A8R8G8B8, ov_w x ov_h at ov_x0, ov_y0) */
static const uint32_t *ov_img;
static int ov_on, ov_x0, ov_y0, ov_w, ov_h, ov_native;   /* (ov_native: over WA's own frame this time) */
static uint8_t lut12[4096];             /* the palette's nearest colour for each 4-bit R, G, B (the speedometer's) */
static int lut12_ok;
static int team_flags[TRAIL_TEAMS], n_team_tasks, n_worm_objs, trail_on = 1, trail_all = 1, trail_show = -1, trail_hl;
static int trail_keep;                  /* TurnLineKeep=1: the line stays all turn, also while the worm moves */
static int trail_kept_at = -1;          /* (test: the frame the worm moved with its line kept) */
static int trail_have0, trail_multi, trail_owner = -1, trail_visible, trail_turn;
static double trail_x0, trail_y0;
static uint8_t pal_seen[1024], dark_lut[256];
static int white_index = -1;

static void (__fastcall *original_render)(void *);
static void (__fastcall *original_fill)(void *, int, int, int, int, int, int);
void *orig_team, *orig_physics, *orig_queue, *orig_clamp, *orig_follow, *orig_bitmap, *orig_grid, *orig_pixel_clipped, *orig_pixel, *orig_vline,
     *orig_grid_fill;                   /* (read by the thunks below) */

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
int __cdecl h_bitmap(uint32_t *r);
int __cdecl h_grid(uint32_t *r);
int __cdecl h_pixel_clipped(uint32_t *r);
int __cdecl h_pixel(uint32_t *r);
int __cdecl h_vline(uint32_t *r);
int __cdecl h_grid_fill(uint32_t *r);
void thunk_queue(void);
void thunk_clamp(void);
void thunk_follow(void);
void thunk_team(void);
void thunk_physics(void);
void __cdecl team_seen(uint32_t task);
void __cdecl worm_seen(uint32_t obj);
static void trail_draw(const int32_t *camera);
void thunk_bitmap(void);
void thunk_grid(void);
void thunk_pixel_clipped(void);
void thunk_pixel(void);
void thunk_vline(void);
void thunk_grid_fill(void);
int __cdecl call_queue(void *queue, void *display, void *camera);
/* A drawing hook: the registers saved (pushal: edi esi ebp esp ebx edx ecx eax, then the return address and the stack
 * arguments) and handed to the handler; 1: handled (the registers as the handler left them, then ret N), 0: WA's own
 * function runs, untouched. */
#define DRAW_THUNK(name, ret)                                                                                          \
    ".globl _thunk_" name "\n"                                                                                          \
    "_thunk_" name ":\n"                                                                                                \
    "    pushal\n"                                                                                                      \
    "    pushl %esp\n"                                                                                                  \
    "    call _h_" name "\n"                                                                                            \
    "    addl $4, %esp\n"                                                                                               \
    "    testl %eax, %eax\n"                                                                                            \
    "    popal\n"                                                                                                       \
    "    jnz 1f\n"                                                                                                      \
    "    jmp *_orig_" name "\n"                                                                                         \
    "1:  ret $" ret "\n"
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
    DRAW_THUNK("bitmap", "0x14")
    DRAW_THUNK("grid", "0x24")
    DRAW_THUNK("pixel_clipped", "0xc")
    DRAW_THUNK("pixel", "0xc")
    DRAW_THUNK("vline", "0x10")
    DRAW_THUNK("grid_fill", "0x14"));

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

static void free_buffers(void) {
    int k;
    free(wbuf); free(rgb); free(qcopy);
    wbuf = NULL; rgb = NULL; qcopy = NULL;
    for (k = 0; k <= RASTER_LEVELS; k++) {
        free(frame_tx[k]); free(frame_ty[k]); frame_tx[k] = frame_ty[k] = NULL;
        taps_x[k].ok = taps_y[k].ok = 0;
    }
}

/* the game resolution seen (a new game, or a resolution change): the buffers and the zoom range for it */
static int fit_size(int w, int h) {
    int nw, nh, k, ok;
    if (w < 64 || h < 64 || w > 16384 || h > 16384) return 0;
    if (w == sw && h == sh && wbuf) return 1;
    zoom_frame(w, h, cfg.max_out, &nw, &nh);
    free_buffers();
    wbuf = (uint8_t *)malloc((size_t)nw * nh);
    rgb = (uint32_t *)malloc((size_t)w * h * 4);
    qcopy = (uint8_t *)malloc((size_t)w * h);
    if (!lut) lut = (uint32_t *)calloc(1u << 18, sizeof(uint32_t));
    ok = wbuf && rgb && qcopy && lut;
    for (k = 0; ok && k <= RASTER_LEVELS; k++) {
        frame_tx[k] = (rtap *)malloc(sizeof(rtap) * (size_t)w);
        frame_ty[k] = (rtap *)malloc(sizeof(rtap) * (size_t)h);
        ok = frame_tx[k] && frame_ty[k];
    }
    if (!ok) {
        free_buffers();
        sw = sh = 0;
        say("no memory for a %dx%d region buffer: no zoom", nw, nh);
        return 0;
    }
    sw = w; sh = h; gw = nw; gh = nh;
    zoom_limits(sw, sh, gw, gh, &cfg, &zoom_lo, &zoom_hi);
    zoom_goal = zoom_clamp(zoom_goal, zoom_lo, zoom_hi);
    zoom_now = zoom_clamp(zoom_now, zoom_lo, zoom_hi);
    ind_pct = -1;
    ind_text[0] = 0;
    say("game %dx%d: region buffer %dx%d, zoom %.0f%% .. %.0f%%", sw, sh, gw, gh, zoom_lo * 100.0, zoom_hi * 100.0);
    return 1;
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

/* ---- helper threads ---------------------------------------------------------------------------------------------- */
static int helpers = -1;
static volatile LONG lut_fills;         /* (test mode: the colour table's misses -- a 256-colour search each) */
static HANDLE helper_go[MAX_HELPERS], helper_done[MAX_HELPERS];
static int helper_y0[MAX_HELPERS], helper_y1[MAX_HELPERS];
static void (*helper_work)(int, int, int);

static DWORD WINAPI helper(LPVOID arg) {
    int k = (int)(intptr_t)arg;
    for (;;) {
        WaitForSingleObject(helper_go[k], INFINITE);
        helper_work(helper_y0[k], helper_y1[k], k + 1);
        SetEvent(helper_done[k]);
    }
    return 0;
}

static void start_helpers(void) {
    SYSTEM_INFO si;
    DWORD_PTR mine = 0, all = 0;
    int k, want, cpus = 0;
    GetSystemInfo(&si);
    /* the CPUs WA may run on (its affinity mask), not all the machine's: more helpers than that only take turns on them */
    if (GetProcessAffinityMask(GetCurrentProcess(), &mine, &all) && mine)
        for (; mine; mine &= mine - 1) cpus++;
    else
        cpus = (int)si.dwNumberOfProcessors;
    want = cpus - 1;
    {   /* (test mode: WKMAPZOOM_THREADS=n, at most n threads in all -- a smaller PC's) */
        char v[16];
        if (test_mode && GetEnvironmentVariableA("WKMAPZOOM_THREADS", v, sizeof(v)) > 0 && atoi(v) >= 1 && atoi(v) - 1 < want)
            want = atoi(v) - 1;
    }
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
    say("%d helper threads for the drawing (%d CPUs usable)", helpers, cpus);
}

/* rows y0..y1 shared out between this thread (worker 0) and the helpers (1..); small jobs stay on this thread */
static int rows_threads(int y0, int y1, long cost) {   /* (how many threads parallel_rows shares these rows between) */
    if (helpers < 0) start_helpers();
    return helpers <= 0 || cost < 32768 || y1 - y0 < 2 * (helpers + 1) ? 1 : helpers + 1;
}
static void parallel_rows(void (*work)(int, int, int), int y0, int y1, long cost) {
    int k, band, y;
    if (rows_threads(y0, y1, cost) == 1) { work(y0, y1, 0); return; }
    helper_work = work;
    band = (y1 - y0 + helpers) / (helpers + 1);
    y = y0 + band;                          /* the first band is this thread's own */
    for (k = 0; k < helpers; k++) {
        helper_y0[k] = y < y1 ? y : y1;
        helper_y1[k] = y + band < y1 ? y + band : y1;
        y += band;
        SetEvent(helper_go[k]);
    }
    work(y0, y0 + band < y1 ? y0 + band : y1, 0);
    WaitForMultipleObjects((DWORD)helpers, helper_done, TRUE, INFINITE);
}

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

static inline uint32_t ex_hash(uint32_t c) { return (c * 2654435761u) >> 22; }

/* the palette as RGB, and the colour tables started afresh when WA's palette changed */
static void palette_rgb(void *display) {
    const uint8_t *pal = (const uint8_t *)display + D_PALETTE;
    int k;
    if (!memcmp(lut_pal, pal, sizeof(lut_pal))) return;
    memcpy(lut_pal, pal, sizeof(lut_pal));
    memset(ex_key, 0, sizeof(ex_key));
    for (k = 0; k < 256; k++) {
        uint32_t c = (uint32_t)pal[4 * k] << 16 | (uint32_t)pal[4 * k + 1] << 8 | pal[4 * k + 2], h = ex_hash(c);
        pal32[k] = c;
        while (ex_key[h] && ex_key[h] != (c | 0x1000000u)) h = (h + 1) & 1023;
        if (!ex_key[h]) { ex_key[h] = c | 0x1000000u; ex_idx[h] = (uint8_t)k; }
    }
    lut_gen = (lut_gen + 1) & 0xffffff;
    if (!lut_gen) { if (lut) memset(lut, 0, sizeof(uint32_t) << 18); lut_gen = 1; }
    if (++pal_gen == 0 || pal_gen == RASTER_BUSY) pal_gen = 1;
}

/* the palette colour nearest a colour: its own index when it is one of the palette's, else from the table (filled as
 * colours come up; one aligned word each, so helper threads can share it). L and gen: the table and its generation
 * (the callers' loops keep them in registers) */
static inline uint8_t nearest_in(uint32_t c, uint32_t *L, uint32_t gen) {
    uint32_t h, e, key;
    int idx;
    c &= 0xffffffu;
    for (h = ex_hash(c); (e = ex_key[h]) != 0; h = (h + 1) & 1023)
        if (e == (c | 0x1000000u)) return ex_idx[h];
    key = ((c >> 18) & 63u) << 12 | ((c >> 10) & 63u) << 6 | ((c >> 2) & 63u);
    e = L[key];
    if ((e >> 8) == gen) return (uint8_t)e;
    if (test_mode) InterlockedIncrement(&lut_fills);   /* (test: how many the table misses a frame) */
    idx = pal_nearest(lut_pal, (int)((c >> 16) & 255u), (int)((c >> 8) & 255u), (int)(c & 255u));
    L[key] = (gen << 8) | (uint32_t)idx;
    return (uint8_t)idx;
}
static uint8_t nearest(uint32_t c) { return nearest_in(c, lut, lut_gen); }

/* ---- the land ---------------------------------------------------------------------------------------------------- */
/* The background builder: the land's shrunk copies made for every tile ahead of time, once a game's land is set up (and
 * again after a palette change), on a thread of its own at below-normal priority. They used to be made only when a tile
 * was first drawn zoomed out past 50% -- zooming out brought dozens of new tiles into view at once, and a 4-CPU PC
 * stalled 18-25 ms a frame making them (a friend's report, 2026-11-22: "stuck when changing the zoom"). A tile is
 * claimed before it is made (raster.c), so the drawing threads never wait for the builder: a tile it is at is drawn
 * from the tile itself that frame. land_free stops it (between two tiles) before the land goes. A palette change stops
 * it too (the tiles are made again with the new palette, from the next start). */
static HANDLE bg_thread, bg_wake, bg_idle;
static volatile LONG bg_busy, bg_cancel;
static uint32_t bg_pal[256], bg_gen, bg_started_gen;
static DWORD WINAPI bg_builder(LPVOID arg) {
    (void)arg;
    for (;;) {
        LARGE_INTEGER a, b;
        int ti, made = 0, n, done;
        WaitForSingleObject(bg_wake, INFINITE);
        QueryPerformanceCounter(&a);
        n = land.cols * land.rows;
        for (ti = 0; ti < n && !bg_cancel && *(volatile uint32_t *)&pal_gen == bg_gen; ti++)
            made += raster_land_build(&land, bg_pal, bg_gen, ti, &pal_gen);
        done = ti == n;
        QueryPerformanceCounter(&b);
        if (done) say("land copies made ahead for %d of %d tiles in %.0f ms (in the background)", made, n,
                      (double)(b.QuadPart - a.QuadPart) * 1000.0 / clock_freq.QuadPart);
        /* (idle signalled before busy is cleared: no start can come between them, so a stop never finds a signal left
         * over from an earlier run -- the land is never freed under a run) */
        SetEvent(bg_idle);
        InterlockedExchange(&bg_busy, 0);
    }
    return 0;
}
static void bg_stop(void) {                 /* (the builder off the land: before it is freed) */
    if (!bg_thread || !bg_busy) return;
    InterlockedExchange(&bg_cancel, 1);
    SetThreadPriority(bg_thread, GetThreadPriority(GetCurrentThread()));   /* (not kept waiting behind busy threads) */
    WaitForSingleObject(bg_idle, INFINITE);
}
static void bg_start(void) {                /* (once per land and palette generation, from WA's thread between frames) */
    if (!land_ok || !land.levels || !land.now || bg_busy || bg_started_gen == pal_gen) return;
    if (!bg_thread) {
        bg_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
        bg_idle = CreateEventA(NULL, FALSE, FALSE, NULL);
        if (!bg_wake || !bg_idle || !(bg_thread = CreateThread(NULL, 64 * 1024, bg_builder, NULL, 0, NULL))) {
            bg_thread = NULL;
            bg_started_gen = pal_gen;       /* (no builder: copies made when drawn, as before) */
            return;
        }
    }
    SetThreadPriority(bg_thread, THREAD_PRIORITY_BELOW_NORMAL);
    memcpy(bg_pal, pal32, sizeof(bg_pal));
    bg_gen = pal_gen;
    bg_started_gen = pal_gen;
    InterlockedExchange(&bg_cancel, 0);
    ResetEvent(bg_idle);
    InterlockedExchange(&bg_busy, 1);
    SetEvent(bg_wake);
}

static void land_free(void) {
    bg_stop();
    bg_started_gen = 0;
    int k;
    free(tiles); free(tile_grid); tile_grid = NULL; free(land.now); free(land.stamp); free(land_snap); free(land_full);
    for (k = 1; k <= RASTER_LEVELS; k++) free(land.mip[k]);
    tiles = NULL; ntiles = 0; land_snap = NULL; land_full = NULL;
    memset(&land, 0, sizeof(land));
    land_ok = 0; land_terrain = NULL;
}

static int tile_cmp(const void *a, const void *b) {
    uintptr_t x = ((const tile_ref *)a)->bm, y = ((const tile_ref *)b)->bm;
    return x < y ? -1 : x > y;
}

static int tile_find(uintptr_t bm) {
    int lo = 0, hi = ntiles - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (tiles[m].bm == bm) return m;
        if (tiles[m].bm < bm) lo = m + 1; else hi = m - 1;
    }
    return -1;
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

/* the game's land, once its tiles exist (wkVista, land.rs): where each tile lies, the land at the start, the shrunk
 * copies' room, and its full-colour original when one matches */
static void land_setup(void *display) {
    char *terrain = *(char **)((char *)display + D_TERRAIN);
    uint32_t *arr;
    int w, h, tw, th, cols, rows, k, need, lev, key = -1;
    if (!terrain || terrain == land_terrain) return;
    arr = *(uint32_t **)(terrain + 4);
    w = FIELD(display, D_LAND_W); h = FIELD(display, D_LAND_H); tw = FIELD(display, D_TILE_W); th = FIELD(display, D_TILE_H);
    if (!arr || w <= 0 || h <= 0 || tw <= 0 || th <= 0 || w > 0x8000 || h > 0x8000) return;
    land_free();
    land_terrain = terrain;
    cols = (w + tw - 1) / tw; rows = (h + th - 1) / th;
    tiles = (tile_ref *)malloc(sizeof(tile_ref) * (size_t)cols * rows);
    tile_grid = (uintptr_t *)calloc((size_t)cols * rows, sizeof(uintptr_t));
    land_snap = (uint8_t *)calloc((size_t)w * h, 1);
    land.now = (uint8_t *)malloc((size_t)w * h);
    land.stamp = (uint32_t *)calloc((size_t)cols * rows, sizeof(uint32_t));
    if (!tiles || !tile_grid || !land_snap || !land.now || !land.stamp) { land_free(); land_terrain = terrain; say("land: no memory"); return; }
    for (k = 0; k < cols * rows; k++) {
        char *bm = (char *)(uintptr_t)arr[k], *s;
        int x0 = (k % cols) * tw, y0 = (k / cols) * th, y, sw_, sh_;
        const uint8_t *data;
        if (!bm) continue;
        tiles[ntiles].bm = (uintptr_t)bm; tiles[ntiles].x = x0; tiles[ntiles].y = y0; ntiles++;
        tile_grid[k] = (uintptr_t)bm;
        s = *(char **)(bm + 4);
        if (!s) continue;
        sw_ = FIELD(s, 4); sh_ = FIELD(s, 8); data = *(const uint8_t **)(s + 0xc);
        if (key < 0) key = *(uint8_t *)(s + 0x10);
        if (!data || sw_ <= 0 || sh_ <= 0) continue;
        for (y = 0; y < sh_ && y0 + y < h; y++)
            memcpy(land_snap + (size_t)(y0 + y) * w + x0, data + (size_t)y * sw_, (size_t)(sw_ < w - x0 ? sw_ : w - x0));
    }
    qsort(tiles, (size_t)ntiles, sizeof(tile_ref), tile_cmp);
    need = cfg.max_out >= 8.0 ? 3 : cfg.max_out >= 4.0 ? 2 : cfg.max_out >= 2.0 ? 1 : 0;
    for (lev = 0; lev < need && tw % (2 << lev) == 0 && th % (2 << lev) == 0; lev++) {}
    land.w = w; land.h = h; land.tw = tw; land.th = th; land.cols = cols; land.rows = rows;
    land.keyed = 1; land.key = (uint8_t)(key < 0 ? 0 : key);
    for (k = 1; k <= lev; k++) {
        land.mw[k] = (w + (1 << k) - 1) >> k; land.mh[k] = (h + (1 << k) - 1) >> k;
        land.mip[k] = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)land.mw[k] * land.mh[k]);
        if (!land.mip[k]) break;
    }
    land.levels = k - 1 < lev ? k - 1 : lev;
    memcpy(land.now, land_snap, (size_t)w * h);
    land.snap = land_snap;
    if (colour_maps) land_full = colourmap_match(w, h, land_snap);
    if (!land_full && test_fullcolour && (land_full = (uint32_t *)calloc((size_t)w * h, sizeof(uint32_t))) != NULL) {
        int x, y;                           /* (test mode: made-up full colours, a rainbow across the land) */
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                const int i = y * w + x, hue = ((x + y) / 4) % 768, a = hue % 256;
                uint32_t r = hue < 256 ? 255 - a : hue < 512 ? 0 : a, g = hue < 256 ? a : hue < 512 ? 255 - a : 0;
                uint32_t b = hue < 256 ? 0 : hue < 512 ? a : 255 - a;
                if (land_snap[i] != land.key) land_full[i] = 0x1000000u | r << 16 | g << 8 | b;
            }
        say("test: made-up full colours for the land");
    }
    land.full = land_full;
    land_ok = 1;
    say("land %dx%d in %d tiles of %dx%d (see-through %d), %d shrunk copies%s", w, h, ntiles, tw, th, land.key,
        land.levels, land_full ? ", full colour" : "");
}

/* ---- the map's drawing calls, drawn onto the screen -------------------------------------------------------------- */
static const rop *job_op;
static rprep job_prep;
static void op_rows(int y0, int y1, int k) { (void)k; raster_draw(job_op, &rv, &job_prep, y0, y1); }

/* the frame's taps of one level (columns, then rows), shared out between the threads */
static struct { rtap *t; int p0, o, level; double b, off; } ax;
static void axis_rows(int i0, int i1, int k) {
    (void)k;
    raster_axis(ax.t + (i0 - ax.p0), i0, i1, ax.o, ax.b, rv.z, ax.off, ax.level, ax.level ? 0 : rv.point);
}
/* (made again only when what they depend on changed: the zoom, the camera, the view, the offset, the filter) */
static void frame_taps(int level, double offx, double offy) {
    const int point = level ? 0 : rv.point;
    if (!(taps_x[level].ok && taps_x[level].p0 == rv.x1 && taps_x[level].p1 == rv.x2 && taps_x[level].o == rv.ox &&
          taps_x[level].b == rv.bx && taps_x[level].z == rv.z && taps_x[level].off == offx && taps_x[level].point == point)) {
        ax.t = frame_tx[level]; ax.p0 = rv.x1; ax.o = rv.ox; ax.b = rv.bx; ax.off = offx; ax.level = level;
        parallel_rows(axis_rows, rv.x1, rv.x2, (long)(rv.x2 - rv.x1) * 64);
        taps_x[level].ok = 1; taps_x[level].p0 = rv.x1; taps_x[level].p1 = rv.x2; taps_x[level].o = rv.ox;
        taps_x[level].b = rv.bx; taps_x[level].z = rv.z; taps_x[level].off = offx; taps_x[level].point = point;
    }
    if (!(taps_y[level].ok && taps_y[level].p0 == rv.y1 && taps_y[level].p1 == rv.y2 && taps_y[level].o == rv.oy &&
          taps_y[level].b == rv.by && taps_y[level].z == rv.z && taps_y[level].off == offy && taps_y[level].point == point)) {
        ax.t = frame_ty[level]; ax.p0 = rv.y1; ax.o = rv.oy; ax.b = rv.by; ax.off = offy; ax.level = level;
        parallel_rows(axis_rows, rv.y1, rv.y2, (long)(rv.y2 - rv.y1) * 64);
        taps_y[level].ok = 1; taps_y[level].p0 = rv.y1; taps_y[level].p1 = rv.y2; taps_y[level].o = rv.oy;
        taps_y[level].b = rv.by; taps_y[level].z = rv.z; taps_y[level].off = offy; taps_y[level].point = point;
    }
    rv.gx[level] = frame_tx[level]; rv.gy[level] = frame_ty[level];
    rv.gox[level] = offx; rv.goy[level] = offy;
}

static void draw_op(const rop *op) {
    rprep p;
    long cost;
    LONGLONG t0 = qpc();
    if (!raster_prepare(op, &rv, &p)) { t_ops += qpc() - t0; return; }
    n_ops++;
    if (test_mode && GetEnvironmentVariableA("WKMAPZOOM_TEST_OPLOG", NULL, 0) > 0 && (frames == 120 || frames == 300)) {
        static int logged;                  /* (test: what one frame draws, for seams between operations) */
        if (logged++ < 600)
            say("test: op frame %u kind %d keyed %d land %d: %.2f,%.2f - %.2f,%.2f (%.2f x %.2f screen px at %.3f)", frames,
                op->kind, op->kind == ROP_BITMAP ? op->keyed : -1, op->land_x >= 0, (double)op->l, (double)op->t,
                (double)op->r, (double)op->b, (op->r - op->l) * rv.z, (op->b - op->t) * rv.z, rv.z);
    }
    cost = (long)(p.x2 - p.x1) * (p.y2 - p.y1);
    if (op->kind != ROP_FILL && !p.level && rv.z < 1.0) cost = (long)(cost / (rv.z * rv.z));
    if (cost >= 32768) { job_op = op; job_prep = p; parallel_rows(op_rows, p.y1, p.y2, cost); }
    else raster_draw(op, &rv, &p, p.y1, p.y2);
    t_ops += qpc() - t0;
}

/* an operation's output rows */
static int op_out_rows(const rop *op, int *a, int *b) {
    *a = (int)floor((op->t - rv.by) * rv.z + rv.oy);
    *b = (int)ceil((op->b - rv.by) * rv.z + rv.oy);
    if (*a < rv.y1) *a = rv.y1;
    if (*b > rv.y2) *b = rv.y2;
    return *a < *b;
}

/* (a tile whose copies aren't ready -- the background builder is at it, or this frame's budget of tiles made from scratch,
 * one per drawing thread, is spent -- is drawn from itself this frame) */
static long sync_budget;
static void sync_rows(int i0, int i1, int k) {
    int i;
    (void)k;
    for (i = i0; i < i1; i++)
        batch[i].no_mip = !raster_land_sync(&land, &batch[i].src, batch[i].land_x, batch[i].land_y, pal32, pal_gen, (int)(frames & 3),
                                            &sync_budget);
}

static void land_rows(int y0, int y1, int k) {
    int i;
    (void)k;
    for (i = 0; i < nbatch; i++) {
        rprep p;
        int a, b;
        if (!op_out_rows(&batch[i], &a, &b) || b <= y0 || a >= y1) continue;
        if (raster_prepare(&batch[i], &rv, &p)) raster_draw(&batch[i], &rv, &p, y0, y1);
    }
}

/* the land tiles queued (they never overlap: drawn together, shared out by rows; their shrunk copies brought up to date
 * first) */
static void batch_flush(void) {
    int i, ya = INT_MAX, yb = INT_MIN;
    long cost = 0;
    LONGLONG t0, t1;
    int level;
    if (!nbatch) return;
    t0 = qpc();
    level = rv.point ? 0 : raster_level(&land, rv.z);
    if (level > 0) {
        const double offx = (double)batch[0].col0 + batch[0].land_x, offy = (double)batch[0].row0 + batch[0].land_y;
        sync_budget = rows_threads(0, nbatch, (long)nbatch * 8192);   /* (one tile a thread) */
        parallel_rows(sync_rows, 0, nbatch, (long)nbatch * 8192);
        if (!rv.gx[level] || rv.gox[level] != offx || rv.goy[level] != offy) frame_taps(level, offx, offy);
    }
    t1 = qpc();
    t_sync += t1 - t0;
    for (i = 0; i < nbatch; i++) {
        int a, b;
        if (!op_out_rows(&batch[i], &a, &b)) continue;
        if (a < ya) ya = a;
        if (b > yb) yb = b;
        cost += (long)(b - a) * (long)((batch[i].r - batch[i].l) * rv.z + 1);
    }
    n_land += (unsigned)nbatch;
    if (ya < yb) parallel_rows(land_rows, ya, yb, cost);
    nbatch = 0;
    t_land += qpc() - t1;
}

static void batch_add(const rop *op) {
    if (nbatch == cap_batch) {
        int cap = cap_batch ? cap_batch * 2 : 256;
        rop *nb = (rop *)realloc(batch, sizeof(rop) * (size_t)cap);
        if (!nb) { batch_flush(); draw_op(op); return; }
        batch = nb; cap_batch = cap;
    }
    batch[nbatch++] = *op;
}

/* the region buffer's part WA drew itself (an operation the module doesn't draw: its pixels other than SENT) */
static void pend_flush(void) {
    rop op;
    if (!pend.on) return;
    pend.on = 0;
    memset(&op, 0, sizeof(op));
    op.kind = ROP_BITMAP; op.l = pend.l; op.t = pend.t; op.r = pend.r; op.b = pend.b;
    op.src.data = wbuf; op.src.w = rw_now; op.src.h = rh_now; op.src.stride = gw;
    op.col0 = 0; op.dir = 1; op.row0 = 0; op.keyed = 1; op.key = SENT; op.land_x = -1;
    draw_op(&op);
}

/* clip an operation's area to a clip rectangle (NULL: none) and the region */
static int clip_area(rop *op, int l, int t, int r, int b, const int32_t *c) {
    if (c) {
        if (l < c[0]) l = c[0];
        if (t < c[1]) t = c[1];
        if (r > c[2]) r = c[2];
        if (b > c[3]) b = c[3];
    }
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r > rw_now) r = rw_now;
    if (b > rh_now) b = rh_now;
    op->l = l; op->t = t; op->r = r; op->b = b;
    return l < r && t < b;
}

/* an operation the module doesn't draw: WA draws it into the region buffer (its area marked first), and the next
 * operation (or the map's end) takes what it drew from there */
static void refuse(int l, int t, int r, int b, const int32_t *c, const char *why) {
    rop a;
    int y;
    batch_flush();
    n_refused++;
    if (n_refused_logged < 8) { n_refused_logged++; say("drawn by WA into the region buffer: %s", why); }
    if (!clip_area(&a, l, t, r, b, c)) return;
    for (y = a.t; y < a.b; y++) memset(wbuf + (size_t)y * gw + a.l, SENT, (size_t)(a.r - a.l));
    pend.on = 1; pend.l = a.l; pend.t = a.t; pend.r = a.r; pend.b = a.b;
}

static int32_t clip_of(const void *obj, int off, int32_t *c) {
    int k;
    for (k = 0; k < 4; k++) c[k] = FIELD(obj, off + 4 * k);
    return 1;
}

/* DisplayGfx::BlitBitmapClipped: x, y its top-left on the display, EDX its width; flags & 2: see-through colour */
int __cdecl h_bitmap(uint32_t *r) {
    void *d = (void *)(uintptr_t)r[7];
    char *bm = (char *)(uintptr_t)r[12], *s;
    int32_t c[4];
    int x = (int32_t)r[9], y = (int32_t)r[10], h = (int32_t)r[11], w = (int32_t)r[5], k;
    rop op;
    if (!world_mode || d != frame_display) return 0;
    pend_flush();
    s = bm ? *(char **)(bm + 4) : NULL;
    if (!s || !*(uint8_t **)(s + 0xc)) return 0;    /* (no surface yet: WA makes one and draws nothing) */
    memset(&op, 0, sizeof(op));
    op.kind = ROP_BITMAP;
    op.src.data = *(const uint8_t **)(s + 0xc); op.src.w = FIELD(s, 4); op.src.h = FIELD(s, 8); op.src.stride = op.src.w;
    op.keyed = (r[13] & 2) != 0; op.key = *(uint8_t *)(s + 0x10);
    op.col0 = -x; op.dir = 1; op.row0 = -y; op.land_x = -1;
    clip_of(d, D_CLIP, c);
    if (!clip_area(&op, x, y, x + w, y + h, c)) return 1;
    if (land_ok && (k = tile_find((uintptr_t)bm)) >= 0 && op.keyed && op.key == land.key) {
        op.land_x = tiles[k].x; op.land_y = tiles[k].y;
        batch_add(&op);
        return 1;
    }
    batch_flush();
    draw_op(&op);
    return 1;
}

/* Zoomed far out, WA's deep water (and the sky above its gradient) are fills that reach some way past the level's sides
 * but not to the view's: the corners below the water line stayed black (a friend's screenshot, 2026-11-22; 66 px short
 * of each side at 25% on a 1920 px level here). A fill as wide as the level or wider is drawn across the whole view.
 * (Only fills: the land and everything else stay as WA puts them.) */
static int wide_l = 1, wide_r = 0;      /* the level's columns in region coordinates this frame (none: wide_l > wide_r) */
static void widen_fill(int *l, int *r, int t, int b, int32_t *c) {
    if (wide_l >= wide_r || *l > wide_l || *r < wide_r || b - t < 2 || (*l <= 0 && *r >= rw_now)) return;
    if (test_mode) {
        static int told;
        if (told++ < 4) say("test: fill %d..%d x %d..%d (the level %d..%d) drawn across the view (0..%d)", *l, *r, t, b, wide_l,
                            wide_r, rw_now);
    }
    *l = 0; *r = rw_now;
    if (c[0] > 0) c[0] = 0;
    if (c[2] < rw_now) c[2] = rw_now;
}

/* the display's rectangle fill (from its origin, inside its clip) */
static void __fastcall hook_fill(void *d, int edx, int x1, int y1, int x2, int y2, int colour) {
    int32_t c[4];
    int ox, oy;
    rop op;
    if (!world_mode || d != frame_display) { original_fill(d, edx, x1, y1, x2, y2, colour); return; }
    pend_flush();
    batch_flush();
    ox = FIELD(d, D_ORG); oy = FIELD(d, D_ORG + 4);
    memset(&op, 0, sizeof(op));
    op.kind = ROP_FILL; op.index = (uint8_t)colour; op.land_x = -1;
    clip_of(d, D_CLIP, c);
    x1 += ox; x2 += ox; y1 += oy; y2 += oy;
    widen_fill(&x1, &x2, y1, y2, c);
    if (clip_area(&op, x1, y1, x2, y2, c)) draw_op(&op);
}

/* BitGrid::BlitSpriteRect into the frame: the variants wkVista draws (raster.rs `check`); the rest WA's own */
int __cdecl h_grid(uint32_t *r) {
    char *g = (char *)(uintptr_t)r[1], *sg = (char *)(uintptr_t)r[13];
    int x = (int32_t)r[9], y = (int32_t)r[10], w = (int32_t)r[11], h = (int32_t)r[12];
    int sx = (int32_t)r[14], sy = (int32_t)r[15], mode = (int32_t)r[17], drawn, kind, orient, mirrored, ok;
    const uint8_t *lut_ = (const uint8_t *)(uintptr_t)r[16];
    int32_t c[4];
    rop op;
    if (!world_mode || !frame_grid || g != (char *)frame_grid) return 0;
    pend_flush();
    clip_of(g, GR_CLIP, c);
    drawn = w && h && x < c[2] && x + w > c[0] && y < c[3] && y + h > c[1] &&
            (x > c[0] ? x : c[0]) != (x + w < c[2] ? x + w : c[2]) && (y > c[1] ? y : c[1]) != (y + h < c[3] ? y + h : c[3]);
    if (drawn) {
        kind = mode & 0xffff; orient = (int)((uint32_t)mode >> 16);
        mirrored = orient == 1 || orient == 0xa;
        ok = (orient == 0 || orient == 0xb || mirrored) &&
             ((kind == 1 && (!mirrored || !lut_)) || ((kind == 0 || kind == 2 || kind == 3) && !mirrored && !lut_));
        if (!sg || sg == g || FIELD(sg, GR_BPP) != 8 || !ok) {
            char why[96];
            snprintf(why, sizeof(why), "a sprite blit, mode %d orientation %x, %s", kind, orient,
                     !sg ? "no source" : sg == g ? "from the frame itself" : FIELD(sg, GR_BPP) != 8 ? "not 8-bit" : "variant");
            refuse(x, y, x + w, y + h, c, why);
            return 0;
        }
        memset(&op, 0, sizeof(op));
        op.kind = ROP_GRID; op.mode = kind; op.lut = lut_; op.land_x = -1;
        op.src.data = *(const uint8_t **)(sg + GR_DATA); op.src.w = FIELD(sg, GR_W); op.src.h = FIELD(sg, GR_H);
        op.src.stride = FIELD(sg, GR_STRIDE);
        if (mirrored) { op.col0 = sx + x + w - 1; op.dir = -1; } else { op.col0 = sx - x; op.dir = 1; }
        op.row0 = sy - y;
        batch_flush();
        if (op.src.data && clip_area(&op, x, y, x + w, y + h, c)) draw_op(&op);
    }
    r[7] = (uint32_t)drawn;                 /* (WA's result: 1 when something is drawn) */
    return 1;
}

/* the frame grid's pixels, lines and fills: rectangles of one colour */
static int grid_rect(uint32_t *r, int l, int t, int rr, int b, int colour, int clipped) {
    char *g = (char *)(uintptr_t)r[6];
    int32_t c[4];
    rop op;
    if (!world_mode || !frame_grid || g != (char *)frame_grid) return 0;
    pend_flush();
    batch_flush();
    memset(&op, 0, sizeof(op));
    op.kind = ROP_FILL; op.index = (uint8_t)colour; op.land_x = -1;
    if (clipped) clip_of(g, GR_CLIP, c);
    if (clipped) widen_fill(&l, &rr, t, b, c);
    if (clip_area(&op, l, t, rr, b, clipped ? c : NULL)) draw_op(&op);
    return 1;
}
int __cdecl h_pixel_clipped(uint32_t *r) { return grid_rect(r, (int32_t)r[9], (int32_t)r[10], (int32_t)r[9] + 1, (int32_t)r[10] + 1, (int32_t)r[11], 1); }
int __cdecl h_pixel(uint32_t *r) { return grid_rect(r, (int32_t)r[9], (int32_t)r[10], (int32_t)r[9] + 1, (int32_t)r[10] + 1, (int32_t)r[11], 0); }
int __cdecl h_vline(uint32_t *r) { return grid_rect(r, (int32_t)r[9], (int32_t)r[10], (int32_t)r[9] + 1, (int32_t)r[11], (int32_t)r[12], 0); }
int __cdecl h_grid_fill(uint32_t *r) { return grid_rect(r, (int32_t)r[9], (int32_t)r[10], (int32_t)r[11], (int32_t)r[12], (int32_t)r[13], 1); }

/* ---- the map onto WA's frame, and the true-colour frame ---------------------------------------------------------- */
static uint32_t clear_colour;
static uint8_t *q_frame;
static int q_stride;
static void clear_rows(int y0, int y1, int k) {
    int y, x;
    (void)k;
    for (y = y0; y < y1; y++) {
        uint32_t *row = rgb + (size_t)y * sw;
        for (x = tc.x1; x < tc.x2; x++) row[x] = clear_colour;
    }
}
static void sent_rows(int y0, int y1, int k) {
    int y;
    (void)k;
    for (y = y0; y < y1; y++) memset(wbuf + (size_t)y * gw, SENT, (size_t)rw_now);
}
/* the map into WA's frame as its nearest palette colours -- and a copy of them (qcopy) for the true-colour frame to
 * compare against (it looked each pixel's colour up again: a second pass as long as this one, 2026-11-22). A pixel of
 * the same colour as the one before takes its index. */
static void quant_rows(int y0, int y1, int k) {
    uint32_t *const L = lut;
    const uint32_t gen = lut_gen;
    int y, x;
    (void)k;
    for (y = y0; y < y1; y++) {
        const uint32_t *s = rgb + (size_t)y * sw;
        uint8_t *d = q_frame + (size_t)y * q_stride, *q = qcopy + (size_t)y * sw;
        uint32_t pc = 0xffffffffu;
        uint8_t pi = 0;
        for (x = tc.x1; x < tc.x2; x++) {
            const uint32_t c = s[x] & 0xffffffu;
            if (c != pc) { pc = c; pi = nearest_in(c, L, gen); }
            d[x] = pi;
            q[x] = pi;
        }
    }
}

/* the frame to show: each pixel the map's own colour where WA's frame still holds what the map put there (qcopy), else
 * (the HUD, the readout) the palette's; 16 pixels at a time where all 16 are the map's */
static struct { uint8_t *bits; int pitch; const uint8_t *frame; int fstride, w; } cj;
static void compose_rows(int y0, int y1, int k) {
    int y, x;
    (void)k;
    for (y = y0; y < y1; y++) {
        uint32_t *d = (uint32_t *)(cj.bits + (size_t)y * cj.pitch);
        const uint8_t *f = cj.frame + (size_t)y * cj.fstride;
        const uint32_t *s = rgb + (size_t)y * sw;
        const uint8_t *q = qcopy + (size_t)y * sw;
        if (y >= tc.y1 && y < tc.y2) {
            for (x = 0; x < tc.x1; x++) d[x] = pal32[f[x]];
            x = tc.x1;
            while (x < tc.x2) {
                if (x + 16 <= tc.x2 &&
                    _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)(f + x)), _mm_loadu_si128((const __m128i *)(q + x)))) == 0xffff) {
                    memcpy(d + x, s + x, 16 * sizeof(uint32_t));
                    x += 16;
                    continue;
                }
                d[x] = f[x] == q[x] ? s[x] : pal32[f[x]];
                x++;
            }
            for (x = tc.x2; x < cj.w; x++) d[x] = pal32[f[x]];
        } else {
            for (x = 0; x < cj.w; x++) d[x] = pal32[f[x]];
        }
        if (ov_on && y >= ov_y0 && y < ov_y0 + ov_h) {   /* the overlay (the speedometer), premultiplied */
            const uint32_t *o = ov_img + (size_t)(y - ov_y0) * ov_w;
            const int xa = ov_x0 < 0 ? 0 : ov_x0, xb = ov_x0 + ov_w > cj.w ? cj.w : ov_x0 + ov_w;
            for (x = xa; x < xb; x++) {
                const uint32_t c = o[x - ov_x0], a = c >> 24, na = 255u - a, b = d[x];
                if (!a) continue;
                d[x] = (((c >> 16) & 0xff) + ((b >> 16) & 0xff) * na / 255u) << 16 |
                       (((c >> 8) & 0xff) + ((b >> 8) & 0xff) * na / 255u) << 8 | ((c & 0xff) + (b & 0xff) * na / 255u);
            }
        }
    }
}
static int compose(uint8_t *bits, int pitch, int w, int h) {
    const uint8_t *frame = *(const uint8_t **)VA(FRAME_PTR_VA);
    int fw = *(int32_t *)VA(FRAME_W_VA), fh = *(int32_t *)VA(FRAME_H_VA);
    LONGLONG t0;
    if (!frame || !rgb || !qcopy || !tc.on || w != sw || h != sh || fw < w || fh < h) return 0;
    t0 = qpc();
    cj.bits = bits; cj.pitch = pitch; cj.frame = frame; cj.fstride = fw; cj.w = w;
    parallel_rows(compose_rows, 0, h, (long)w * h);
    t_compose += qpc() - t0;
    n_compose++;
    return 1;
}

/* Zoomed (or a full-colour land): every map entry (back to front) drawn onto the screen through the module, that into
 * WA's frame, then every HUD entry (back to front) as WA draws it. (WA's own order puts the HUD in front of the map; an
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

int __cdecl queue_hook(uint8_t *queue, void *display, void *camera) {
    typedef void (__cdecl *qsort_fn)(void *, size_t, size_t, void *);
    view_state wa, world;
    uint32_t n = *(uint32_t *)(queue + Q_COUNT);
    uint32_t *entries = (uint32_t *)(queue + Q_ENTRIES);
    double z = frame_zoom;
    int i, last = 0, rw, rh, bx, by, cx1, cy1, cx2, cy2, nworld = 0, nscreen = 0, colour_land = land_ok && land.full;
    LARGE_INTEGER t0, t1, t1b, t2;
    static unsigned checks;
    int check = 0;
    if (display == frame_display && camera) {   /* (the speedometer's: WA's camera transform this frame, screen = origin
                                                   + (world - camera) x zoom, as the movement line's) */
        spd_cx = ((const int32_t *)camera)[0] >> 16; spd_cy = ((const int32_t *)camera)[1] >> 16;
        spd_ox = FIELD(display, D_ORG); spd_oy = FIELD(display, D_ORG + 4); spd_z = z;
        spd_cam = 1;
    }
    if ((z == 1.0 && !colour_land && !(trail_visible && trail_show >= 0 && trail_hl)) || display != frame_display || !wbuf || !rgb || !n || n > MAX_ENTRIES)
        return call_queue(queue, display, camera);
    QueryPerformanceCounter(&t0);
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
    /* The frame the map goes into is the renderer's own (007A0EEC: pixels, width = row stride, height), as WA's blits
     * and fills use it. (Not the display's frame grid: WA brings that up to date lazily, at its first drawing call of a
     * frame (its lock, 0056A370) -- and while zoomed the map's calls are the module's, so after a resolution switch the
     * grid still describes the old frame when the map goes in: rows past the new frame's end, a crash.) */
    cx1 = wa.clip[0]; cy1 = wa.clip[1]; cx2 = wa.clip[2]; cy2 = wa.clip[3];
    if (cx1 < 0) cx1 = 0;
    if (cy1 < 0) cy1 = 0;
    if (cx2 > wa.fbw) cx2 = wa.fbw;
    if (cy2 > wa.fbh) cy2 = wa.fbh;
    if (cx2 > sw) cx2 = sw;
    if (cy2 > sh) cy2 = sh;
    if (cx2 <= cx1 || cy2 <= cy1 || !wa.fb || wa.fbw != sw || wa.fbh != sh) return call_queue(queue, display, camera);
    ((qsort_fn)VA(QSORT_VA))(entries, n, 4, (void *)VA(QUEUE_ORDER_VA));   /* WA's own order, as it sorts them */
    memcpy(queue_list, entries, n * 4);
    zoom_region(cx1, cy1, cx2, cy2, wa.org[0], wa.org[1], z, gw, gh, &rw, &rh, &bx, &by);
    rw_now = rw; rh_now = rh;
    world = wa;
    world.w = rw; world.h = rh;
    world.clip[0] = world.clip[1] = 0; world.clip[2] = rw; world.clip[3] = rh;
    world.org[0] = bx; world.org[1] = by;
    world.data = (int32_t)(uintptr_t)wbuf; world.stride = gw; world.gw = rw; world.gh = rh;
    memcpy(world.gclip, world.clip, sizeof(world.clip));
    world.fb = (int32_t)(uintptr_t)wbuf; world.fbw = gw; world.fbh = gh;   /* (rows gw apart) */
    memset(&rv, 0, sizeof(rv));
    rv.z = z; rv.bx = bx; rv.by = by; rv.ox = wa.org[0]; rv.oy = wa.org[1];
    rv.x1 = cx1; rv.y1 = cy1; rv.x2 = cx2; rv.y2 = cy2;
    rv.out = rgb; rv.stride = sw; rv.pal = pal32; rv.nearest = nearest; rv.land = land_ok ? &land : NULL; rv.gen = pal_gen;
    rv.point = !filter_smooth;
    frame_taps(0, 0.0, 0.0);
    tc.on = 0; tc.x1 = cx1; tc.y1 = cy1; tc.x2 = cx2; tc.y2 = cy2;
    clear_colour = pal32[0];
    if (camera && land_ok && land.w > 0) { wide_l = bx - (((const int32_t *)camera)[0] >> 16); wide_r = wide_l + land.w; }
    else { wide_l = 1; wide_r = 0; }        /* (WA's camera transform: region = world - camera + bx, whole pixels) */
    parallel_rows(clear_rows, cy1, cy2, (long)(cx2 - cx1) * (cy2 - cy1));
    check = test_mode && checks < 3 && frames > 150;   /* (test mode: the region buffer watched for WA's own drawing) */
    if (check) parallel_rows(sent_rows, 0, rh, (long)rw * rh);
    n_ops = n_land = n_refused = 0;
    t_sync = t_land = t_ops = t_line = 0;
    pend.on = 0; nbatch = 0;
    view_load(display, &world);
    world_mode = 1;
    for (i = (int)n - 1; i >= 0; i--)       /* the map, back to front */
        if (!is_screen(queue_list[i])) { last = draw_entry(queue, display, camera, queue_list[i]); nworld++; }
    pend_flush();
    batch_flush();
    world_mode = 0;
    view_load(display, &wa);
    if (camera) trail_draw((const int32_t *)camera);
    if (check) {                            /* anything WA drew into the region buffer behind the module's back */
        long stray = 0;
        int y, x;
        checks++;
        for (y = 0; y < rh; y++) for (x = 0; x < rw; x++) stray += wbuf[(size_t)y * gw + x] != SENT;
        say("test: %ld region pixels drawn by WA itself (%u operations it drew, %u drawn by the module, %u land tiles)",
            stray, n_refused, n_ops, n_land);
    }
    QueryPerformanceCounter(&t1);
    q_frame = (uint8_t *)(uintptr_t)wa.fb; q_stride = wa.fbw;
    parallel_rows(quant_rows, cy1, cy2, (long)(cx2 - cx1) * (cy2 - cy1));
    tc.on = 1;
    QueryPerformanceCounter(&t1b);
    if (test_checks && frames % 300 == 200) {   /* (test: the indices kept for the true-colour frame against a fresh look) */
        long diff = 0;
        int y, x;
        for (y = cy1; y < cy2; y++)
            for (x = cx1; x < cx2; x++) diff += qcopy[(size_t)y * sw + x] != nearest(rgb[(size_t)y * sw + x]);
        say("test: the kept palette indices against a fresh look: %ld pixels differ (a few can: two threads filling one "
            "table entry at once, each with its own colour)", diff);
    }
    for (i = (int)n - 1; i >= 0; i--)       /* the HUD, back to front */
        if (is_screen(queue_list[i])) { last = draw_entry(queue, display, camera, queue_list[i]); nscreen++; }
    *(uint32_t *)(queue + Q_COUNT) = n;
    memcpy(entries, queue_list, n * 4);
    QueryPerformanceCounter(&t2);
    if (test_mode) {                        /* (test: frames with many colour table misses, or slow -- zooming) */
        const double f = 1000.0 / clock_freq.QuadPart, map = (double)(t1.QuadPart - t0.QuadPart) * f, q = (double)(t1b.QuadPart - t1.QuadPart) * f;
        const LONG fills = InterlockedExchange(&lut_fills, 0);
        static unsigned told;
        if ((fills > 2000 || map + q > 12.0) && told < 400) {
            told++;
            say("test: frame %u at %.1f%%: %ld colour table fills; map %.2f ms (upkeep %.2f, land %.2f, other %.2f, line %.2f), "
                "into WA's frame %.2f ms", frames, z * 100.0, (long)fills, map, t_sync * f, t_land * f, t_ops * f, t_line * f, q);
        }
    }
    {   /* the cost, for the log: the map, its colours into WA's frame, the HUD */
        static double map_ms, scale_ms, rest_ms, zsum, sync_ms, land_ms, ops_ms, line_ms;
        static unsigned count, ops, lands;
        const double f = 1000.0 / clock_freq.QuadPart;
        sync_ms += t_sync * f; land_ms += t_land * f; ops_ms += t_ops * f; line_ms += t_line * f;
        map_ms += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / clock_freq.QuadPart;
        scale_ms += (double)(t1b.QuadPart - t1.QuadPart) * 1000.0 / clock_freq.QuadPart;
        rest_ms += (double)(t2.QuadPart - t1b.QuadPart) * 1000.0 / clock_freq.QuadPart;
        zsum += z; ops += n_ops; lands += n_land;
        if (++count == 120) {
            const double comp_ms = n_compose ? (double)t_compose * f / n_compose : 0.0;
            say("120 drawn frames (zoom %.0f%% on average): map %.2f ms (land copies' upkeep %.2f, land %.2f for %u tiles, "
                "%u other drawing calls %.2f, movement line %.2f, WA's own %.2f), into WA's frame %.2f ms, HUD %.2f ms, "
                "true-colour frame %.2f ms a frame; %u frames shown in true colour so far", zsum / count * 100.0,
                map_ms / count, sync_ms / count, land_ms / count, lands / count, ops / count, ops_ms / count,
                line_ms / count, (map_ms - sync_ms - land_ms - ops_ms - line_ms) / count, scale_ms / count, rest_ms / count,
                comp_ms, present_shown());
            map_ms = scale_ms = rest_ms = zsum = sync_ms = land_ms = ops_ms = line_ms = 0; count = ops = lands = 0;
            t_compose = 0; n_compose = 0;
        }
    }
    if (!logged_split) {
        logged_split = 1;
        say("first drawn frame: %u entries: %d map (region %dx%d), %d HUD; view %d,%d..%d,%d, centre %d,%d", n, nworld, rw, rh,
            nscreen, cx1, cy1, cx2, cy2, wa.org[0], wa.org[1]);
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
    if (z <= 1.0 || !wbuf || GetTickCount() - live_tick > 1000) return;
    hw = (int32_t)ceil(hw / z); hh = (int32_t)ceil(hh / z);
    if (hw < 1) hw = 1;
    if (hh < 1) hh = 1;
    if (test_mode && logged < 3) { logged++; say("test: camera follow at %.0f%%: half view %dx%d -> %dx%d", z * 100.0, (int)regs[7], (int)regs[9], hw, hh); }
    regs[7] = (uint32_t)hw;
    regs[9] = (uint32_t)hh;
}

/* ---- the movement line ---------------------------------------------------------------------------------------- */
/* Each frame the worm of the team whose turn it is (a team task with its turn flag, +0x3C, up: one team at a time; its
 * current worm +0x48) is noted every 2 pixels it moves -- the worm itself, never the camera or the targeting cursor, so
 * shots and a teleport's aiming are not in it -- and after its turn too, until another team's turn starts (it may fall
 * or slide on). A jump (a teleport, another of the team's worms) starts a new piece of line. When a team's turn starts,
 * its line is drawn on the map in red -- its last turn bright, fainter where that turn began and solid where it ended
 * (the way it was going), its earlier turns dimmer -- until its worm moves. Team tasks are found by their message handler
 * (00557310), worms by their physics step (004FCDF0). A team task: +0x2C the game, +0x38 its team. */
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

static void trail_reset(void) {
    int k;
    n_team_tasks = 0;
    n_worm_objs = 0;
    trail_show = -1;
    trail_owner = -1;
    trail_visible = 0;
    trail_turn = 0;
    trail_multi = 0;
    if (trails) for (k = 0; k < TRAIL_TEAMS * TRAIL_PER_TEAM; k++) trails[k].n = 0;
    trail_drops++;                          /* (a kept line mask is out of date) */
}

/* a team's current worm and where it is (px); 0 when it isn't known. v (may be NULL): its speed as the game has it
 * (+0x90 / +0x94: its move each step, 1/65536 px; WA steps 50 times a second), in px/s */
static int team_worm(uintptr_t task, void *world, double *x, double *y, double *v) {
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
            (void)world;
            return 1;
        }
    }
    return 0;
}

static void trail_add(team_trail *t, double x, double y, int own, double spd) {
    tpoint *e, *p;
    e = t->n ? &t->pt[t->n - 1] : NULL;
    if (e && fabs(x - e->x) < 2.0 && fabs(y - e->y) < 2.0 && e->own == own) return;
    if (t->n == t->cap && t->cap < TRAIL_MAX) {
        int cap = t->cap ? t->cap * 2 : 256;
        tpoint *np = (tpoint *)realloc(t->pt, sizeof(tpoint) * (size_t)(cap > TRAIL_MAX ? TRAIL_MAX : cap));
        if (!np) return;
        t->pt = np;
        t->cap = cap > TRAIL_MAX ? TRAIL_MAX : cap;
        e = t->n ? &t->pt[t->n - 1] : NULL;
    }
    if (!t->cap) return;
    if (t->n == TRAIL_MAX) {                /* (a long game: its oldest quarter dropped) */
        memmove(t->pt, t->pt + TRAIL_MAX / 4, sizeof(tpoint) * (TRAIL_MAX - TRAIL_MAX / 4));
        t->n -= TRAIL_MAX / 4;
        t->pt[0].brk = 1;
        trail_drops++;                      /* (a kept line mask is out of date) */
        e = &t->pt[t->n - 1];
    }
    p = &t->pt[t->n++];
    p->x = (float)x; p->y = (float)y;
    p->turn = trail_turn;
    p->brk = (int8_t)(!e || fabs(x - e->x) > 80.0 || fabs(y - e->y) > 80.0);
    p->own = (int8_t)own;
    p->spd = (uint16_t)(spd < 0.0 ? 0.0 : spd > 65535.0 ? 65535.0 : spd);
}

/* each frame, before WA's own drawing: the worms still there, turns starting and ending, the worm's place noted, the
 * shown line hidden once the worm moves */
static void trail_frame(void *world) {
    int i, flagged = 0;
    uintptr_t owner_task = 0;
    double x, y;
    turn_worm_ok = 0;
    if (!worms_ok || !trails) return;
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
            if (flag && !team_flags[i]) {           /* its turn starts: its worm's line shown (the worm found below) */
                trail_turn++;
                trail_owner = team;
                trail_visible = 1;
                trail_show = -1;
            } else if (!flag && team_flags[i] && team == trail_owner) {
                trail_visible = 0;
                trail_show = -1;
                if (test_mode) say("test: team %d's turn ended (turn %d)", team, trail_turn);
            }
            if (flag) flagged++;
            if (team == trail_owner) owner_task = t;
        }
        team_flags[i] = flag;
        i++;
    }
    if (test_mode) {                        /* (test: the turn's worm fields, as they change) */
        static int32_t prev[TRAIL_TEAMS + 2];
        int k, changed = 0;
        int32_t now[TRAIL_TEAMS + 2];
        now[0] = *(int32_t *)((char *)world + 0x45e4); now[1] = *(int32_t *)((char *)world + 0x4620);
        for (k = 0; k < n_team_tasks && k < TRAIL_TEAMS; k++) now[2 + k] = *(int32_t *)(team_tasks[k] + 0x38) * 100 + *(int32_t *)(team_tasks[k] + 0x48) * 10 + (*(int32_t *)(team_tasks[k] + 0x3c) != 0);
        for (k = 0; k < 2 + n_team_tasks && k < TRAIL_TEAMS + 2; k++) if (now[k] != prev[k]) changed = 1;
        if (changed) {
            char line[256];
            int at = snprintf(line, sizeof(line), "test: frame %u: game+45E4 %d, +4620 %d; tasks (team*100+worm*10+turn):", frames, now[0], now[1]);
            for (k = 0; k < n_team_tasks && k < TRAIL_TEAMS && at < 230; k++) at += snprintf(line + at, sizeof(line) - at, " %d", now[2 + k]);
            say("%s", line);
            memcpy(prev, now, sizeof(prev));
        }
    }
    if (test_mode && frames % 60 == 30 && frames < 1500) {
        char line[512];
        int k, at = 0;
        at += snprintf(line + at, sizeof(line) - at, "test: frame %u: %d worms", frames, n_worm_objs);
        for (k = 0; k < n_worm_objs && at < 400; k++) {
            const uintptr_t o = worm_objs[k];
            at += snprintf(line + at, sizeof(line) - at, " [t%d n%d %.0f,%.0f]", *(int32_t *)(o + 0xfc), *(int32_t *)(o + 0x100),
                           *(int32_t *)(o + 0x84) / 65536.0, *(int32_t *)(o + 0x88) / 65536.0);
        }
        for (k = 0; k < n_team_tasks && at < 480; k++)
            at += snprintf(line + at, sizeof(line) - at, " task team %d turn %d worm %d;", *(int32_t *)(team_tasks[k] + 0x38),
                           *(int32_t *)(team_tasks[k] + 0x3c), *(int32_t *)(team_tasks[k] + 0x48));
        say("%s", line);
    }
    if (flagged > 1 && !trail_multi) { trail_multi = 1; say("several teams have the turn at once (real-time play?): no movement line"); }
    if (trail_multi) return;
    if (trail_owner >= 0 && owner_task && team_worm(owner_task, world, &turn_wx, &turn_wy, &turn_gv)) {
        turn_worm_ok = 1;
        turn_key = trail_owner * 64 + (*(int32_t *)(owner_task + 0x48) & 63);
    }   /* (the auto zoom's) */
    if (!trail_on) return;
    {   /* every worm, every frame (knocked about in other turns too: its line ends where it is); "own" while it is the
         * current worm of the team that has (or last had) the turn */
        const int owner_index = owner_task ? *(int32_t *)(owner_task + 0x48) : -1;
        for (i = 0; i < n_worm_objs; i++) {
            const uintptr_t o = worm_objs[i];
            const int team = *(int32_t *)(o + 0xfc), index = *(int32_t *)(o + 0x100);
            if (team < 0 || team >= TRAIL_TEAMS || index < 0 || index >= TRAIL_PER_TEAM) continue;
            trail_add(&trails[team * TRAIL_PER_TEAM + index], *(int32_t *)(o + 0x84) / 65536.0, *(int32_t *)(o + 0x88) / 65536.0,
                      team == trail_owner && index == owner_index,
                      hypot(*(int32_t *)(o + 0x90) / 65536.0, *(int32_t *)(o + 0x94) / 65536.0) * 50.0);
        }
    }
    if (trail_owner < 0 || !owner_task || !team_worm(owner_task, world, &x, &y, NULL)) return;
    {
        const int index = *(int32_t *)(owner_task + 0x48), slot = trail_owner * TRAIL_PER_TEAM + index;
        team_trail *tr;
        if (index < 0 || index >= TRAIL_PER_TEAM) return;
        tr = &trails[slot];
        if (!trail_visible) return;
        if (slot != trail_show) {               /* the turn's worm: its last own turn bright */
            int k;
            trail_show = slot;
            trail_have0 = 0;
            for (trail_hl = 0, k = tr->n - 1; k >= 0 && !trail_hl; k--) if (tr->pt[k].own && tr->pt[k].turn < trail_turn) trail_hl = tr->pt[k].turn;
            if (trail_hl) {
                int n_hl = 0;
                for (k = 0; k < tr->n; k++) n_hl += tr->pt[k].own && tr->pt[k].turn == trail_hl;
                say("turn %d: team %d's worm %d: its line shown (%d points; its last own turn, turn %d, bright: %d of them)",
                    trail_turn, trail_owner, index, tr->n, trail_hl, n_hl);
            }
        }
        if (!trail_have0) { trail_x0 = x; trail_y0 = y; trail_have0 = 1; }     /* hidden once the worm moves (unless
                                                                                     TurnLineKeep: at the turn's end) */
        else if (trail_keep && trail_hl && trail_have0 == 1 && (fabs(x - trail_x0) > 6.0 || fabs(y - trail_y0) > 6.0)) {
            trail_have0 = 2;                    /* (once a showing) */
            say("team %d's worm %d moved: its line stays (TurnLineKeep)", trail_owner, index);
            if (test_mode) trail_kept_at = (int)frames;
        } else if (!trail_keep && (fabs(x - trail_x0) > 6.0 || fabs(y - trail_y0) > 6.0)) {
            if (trail_hl) say("team %d's worm %d moved: its line hidden", trail_owner, index);
            trail_visible = 0;
            trail_show = -1;
        }
    }
}

/* The line is drawn into a coverage mask first (each pixel the most any segment covers it, times that segment's
 * opacity), then blended onto the map: where short segments overlap, the line keeps its look.
 *
 * The mask is kept from frame to frame and drawn again only when something it depends on changed (the camera, the zoom,
 * the view, which worm's line and which turn is bright, the points of earlier turns -- see trail_key), so a still
 * camera blends a ready mask (it was drawn afresh every frame: ~3 ms a frame at 100% and 17 ms with a long line on the
 * owner's PC, 2026-11-22). A redraw and the blend are shared out by rows between the helper threads; each row keeps
 * the span its segments' boxes cover, so the blend and the next redraw's clear touch only those. Pixel for pixel the
 * same as drawing it afresh (the per-pixel formula is the same, and the max over segments doesn't depend on their order). */
typedef struct { double x0, y0, x1, y1, a; int c; } tseg;   /* c: its speed as a colour index (trail_lut) */
static uint8_t *trail_mask, *trail_cmask;   /* (each pixel's coverage, and the colour index of the segment that gave it) */
static uint32_t trail_lut[256];             /* colour index -> RGB (made again when the colours' settings change) */
static int trail_lut_mode = -1;
static double trail_lut_from = -1.0;
static int16_t *trail_lo, *trail_hi;        /* per row: the mask's span [lo, hi) (lo >= hi: none) */
static int trail_mask_n, trail_rows;
static tseg *tsegs;
static int n_tsegs, cap_tsegs;
static double trail_half;
static uint8_t *trail_target;               /* (the mask the segments go into: the kept one, or a test check's) */
static struct {
    int valid, show, hl, turn, all, nhist, next_brk, x1, y1, x2, y2, w, h;
    int32_t cx, cy;
    double z, ox, oy;
    unsigned drops;
} trail_key;

/* one segment's rows [y0, y1) into the target mask (and the rows' spans): anti-aliased, `half` pixels each side,
 * opacity a */
static void trail_segment_rows(const tseg *s, double half, int y0, int y1) {
    const double x0 = s->x0, x1 = s->x1, dx = s->x1 - s->x0, dy = s->y1 - s->y0, len2 = dx * dx + dy * dy;
    const double reach = half + 0.5, reach2 = reach * reach, a = s->a;
    int X, Y, xa = (int)floor((x0 < x1 ? x0 : x1) - half - 1), xb = (int)ceil((x0 > x1 ? x0 : x1) + half + 1);
    int ya = (int)floor((s->y0 < s->y1 ? s->y0 : s->y1) - half - 1), yb = (int)ceil((s->y0 > s->y1 ? s->y0 : s->y1) + half + 1);
    if (xa < rv.x1) xa = rv.x1;
    if (ya < rv.y1) ya = rv.y1;
    if (xb > rv.x2) xb = rv.x2;
    if (yb > rv.y2) yb = rv.y2;
    if (ya < y0) ya = y0;
    if (yb > y1) yb = y1;
    if (xa >= xb || ya >= yb) return;
    for (Y = ya; Y < yb; Y++) {
        uint8_t *row = trail_target + (size_t)Y * sw, *crow = trail_target == trail_mask ? trail_cmask + (size_t)Y * sw : NULL;
        if (trail_target == trail_mask) {
            if (xa < trail_lo[Y]) trail_lo[Y] = (int16_t)xa;
            if (xb > trail_hi[Y]) trail_hi[Y] = (int16_t)xb;
        }
        for (X = xa; X < xb; X++) {
            const double px = X + 0.5 - x0, py = Y + 0.5 - s->y0;
            double t = len2 > 0.0 ? (px * dx + py * dy) / len2 : 0.0, ex, ey, d2, cov;
            uint8_t v;
            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;
            ex = px - t * dx; ey = py - t * dy;
            d2 = ex * ex + ey * ey;
            if (d2 >= reach2) continue;         /* (most of the box: no square root) */
            cov = reach - sqrt(d2);
            if (cov > 1.0) cov = 1.0;
            v = (uint8_t)(cov * a * 255.0);
            if (v > row[X]) { row[X] = v; if (crow) crow[X] = (uint8_t)s->c; }
        }
    }
}

/* (workers) the kept mask's rows cleared within their spans; every segment's part in rows [y0, y1); the mask onto the
 * map */
static void trail_clear_rows(int y0, int y1, int k) {
    int Y;
    (void)k;
    for (Y = y0; Y < y1; Y++) {
        if (trail_lo[Y] < trail_hi[Y]) {
            memset(trail_mask + (size_t)Y * sw + trail_lo[Y], 0, (size_t)(trail_hi[Y] - trail_lo[Y]));
            memset(trail_cmask + (size_t)Y * sw + trail_lo[Y], 0, (size_t)(trail_hi[Y] - trail_lo[Y]));
        }
        trail_lo[Y] = INT16_MAX; trail_hi[Y] = 0;
    }
}
static void trail_draw_rows(int y0, int y1, int k) {
    int i;
    (void)k;
    for (i = 0; i < n_tsegs; i++) trail_segment_rows(&tsegs[i], trail_half, y0, y1);
}
static void trail_blend_rows(int y0, int y1, int k) {
    int X, Y;
    (void)k;
    for (Y = y0; Y < y1; Y++) {
        const uint8_t *m = trail_mask + (size_t)Y * sw, *cm = trail_cmask + (size_t)Y * sw;
        uint32_t *row = rgb + (size_t)Y * sw;
        for (X = trail_lo[Y]; X < trail_hi[Y]; X++) {
            const uint32_t w = m[X], o = row[X], c = trail_lut[cm[X]];
            if (!w) continue;
            row[X] = ((((o >> 16) & 255u) * (255u - w) + ((c >> 16) & 255u) * w) / 255u) << 16 |
                     ((((o >> 8) & 255u) * (255u - w) + ((c >> 8) & 255u) * w) / 255u) << 8 |
                     (((o & 255u) * (255u - w) + (c & 255u) * w) / 255u);
        }
    }
}

/* the segments of the shown line, in screen pixels (WA's camera transform: screen = world - camera, whole pixels);
 * points closer than 3 screen pixels to the last one drawn are passed over. 0: not all of them (no memory) */
static int trail_segments(const team_trail *tr, double cx, double cy) {
    double lx = 0.0, ly = 0.0, ls = 0.0;
    int i, first = -1, n_hl = 0, k = 0, have = 0;
    n_tsegs = 0;
    for (i = 0; i < tr->n; i++) if (tr->pt[i].own && tr->pt[i].turn == trail_hl) { if (first < 0) first = i; n_hl++; }
    for (i = 0; i < tr->n; i++) {
        const tpoint *b = &tr->pt[i];
        const double bx = rv.ox + (b->x - cx) * rv.z, by = rv.oy + (b->y - cy) * rv.z;
        double alpha;
        if (b->turn >= trail_turn) { have = 0; continue; }   /* (not this turn's own start) */
        if (b->own && b->turn == trail_hl) { k = i - first; alpha = 0.5 + 0.5 * k / (n_hl > 1 ? n_hl - 1 : 1); }
        else if (!trail_all) { have = 0; continue; }
        else alpha = 0.35;
        if (!have || b->brk) { lx = bx; ly = by; ls = b->spd; have = 1; continue; }
        if (fabs(bx - lx) < 3.0 && fabs(by - ly) < 3.0 && i + 1 < tr->n && !tr->pt[i + 1].brk) continue;
        if (n_tsegs == cap_tsegs) {
            const int cap = cap_tsegs ? cap_tsegs * 2 : 1024;
            tseg *ns = (tseg *)realloc(tsegs, sizeof(tseg) * (size_t)cap);
            if (!ns) return 0;
            tsegs = ns; cap_tsegs = cap;
        }
        tsegs[n_tsegs].x0 = lx; tsegs[n_tsegs].y0 = ly; tsegs[n_tsegs].x1 = bx; tsegs[n_tsegs].y1 = by; tsegs[n_tsegs].a = alpha;
        {   /* (its colour: the speed between its two ends, 0-255 over 0 to the dial's end) */
            const double v = (ls + b->spd) / 2.0;
            tsegs[n_tsegs].c = v >= SPEEDO_VMAX ? 255 : (int)(v * 255.0 / SPEEDO_VMAX + 0.5);
        }
        n_tsegs++;
        lx = bx; ly = by; ls = b->spd;
    }
    return 1;
}

static void trail_draw(const int32_t *camera) {
    const team_trail *tr;
    const int32_t cx = camera[0] >> 16, cy = camera[1] >> 16;
    LONGLONG t0;
    int nhist, next_brk, hit;
    if (!trail_visible || trail_show < 0 || !trail_hl || !trails) return;
    t0 = qpc();
    if (trail_mask_n != sw * sh || trail_rows != sh) {
        int Y;
        free(trail_mask); free(trail_cmask); free(trail_lo); free(trail_hi);
        trail_mask = (uint8_t *)calloc((size_t)sw * sh, 1);
        trail_cmask = (uint8_t *)calloc((size_t)sw * sh, 1);
        trail_lo = (int16_t *)malloc(sizeof(int16_t) * (size_t)sh);
        trail_hi = (int16_t *)malloc(sizeof(int16_t) * (size_t)sh);
        trail_key.valid = 0;
        if (!trail_mask || !trail_cmask || !trail_lo || !trail_hi) {
            free(trail_mask); free(trail_cmask); free(trail_lo); free(trail_hi);
            trail_mask = trail_cmask = NULL; trail_lo = trail_hi = NULL; trail_mask_n = trail_rows = 0;
            return;
        }
        trail_mask_n = sw * sh; trail_rows = sh;
        for (Y = 0; Y < sh; Y++) { trail_lo[Y] = INT16_MAX; trail_hi[Y] = 0; }
    }
    tr = &trails[trail_show];
    trail_half = sh / 540.0;
    if (trail_half < 1.25) trail_half = 1.25;
    /* what the drawn line depends on: its earlier turns' points (they come first: points go in in time order), and the
     * first of this turn's (the 3-pixel test looks one point ahead) */
    for (nhist = tr->n; nhist > 0 && tr->pt[nhist - 1].turn >= trail_turn; nhist--) {}
    next_brk = nhist < tr->n ? 1 + tr->pt[nhist].brk : 0;
    hit = trail_key.valid && trail_key.show == trail_show && trail_key.hl == trail_hl && trail_key.turn == trail_turn &&
          trail_key.all == trail_all && trail_key.nhist == nhist && trail_key.next_brk == next_brk && trail_key.cx == cx &&
          trail_key.cy == cy && trail_key.z == rv.z && trail_key.ox == rv.ox && trail_key.oy == rv.oy && trail_key.x1 == rv.x1 &&
          trail_key.y1 == rv.y1 && trail_key.x2 == rv.x2 && trail_key.y2 == rv.y2 && trail_key.w == sw && trail_key.h == sh &&
          trail_key.drops == trail_drops;
    if (!hit) {                             /* drawn afresh: the old mask cleared, the segments gathered, then drawn */
        long cost;
        int whole;
        trail_key.valid = 0;
        parallel_rows(trail_clear_rows, 0, sh, (long)sw * sh / 8);
        whole = trail_segments(tr, (double)cx, (double)cy);
        trail_target = trail_mask;
        cost = (long)n_tsegs * (long)(trail_half * 2 + 4) * 64;
        parallel_rows(trail_draw_rows, rv.y1, rv.y2, cost);
        trail_key.valid = whole;            /* (part of the line only, out of memory: drawn again next frame) */
        trail_key.show = trail_show; trail_key.hl = trail_hl; trail_key.turn = trail_turn;
        trail_key.all = trail_all; trail_key.nhist = nhist; trail_key.next_brk = next_brk; trail_key.cx = cx; trail_key.cy = cy;
        trail_key.z = rv.z; trail_key.ox = rv.ox; trail_key.oy = rv.oy; trail_key.x1 = rv.x1; trail_key.y1 = rv.y1;
        trail_key.x2 = rv.x2; trail_key.y2 = rv.y2; trail_key.w = sw; trail_key.h = sh; trail_key.drops = trail_drops;
    } else if (test_checks && frames % 30 == 0) {   /* (test: a kept mask against one drawn afresh) */
        uint8_t *fresh = (uint8_t *)calloc((size_t)sw * sh, 1);
        if (fresh) {
            long diff = 0, outside = 0;
            int Y, X;
            trail_segments(tr, (double)cx, (double)cy);
            trail_target = fresh;
            trail_draw_rows(rv.y1, rv.y2, 0);
            trail_target = trail_mask;
            for (Y = 0; Y < sh; Y++)
                for (X = 0; X < sw; X++) {
                    const uint8_t m = trail_mask[(size_t)Y * sw + X];
                    diff += fresh[(size_t)Y * sw + X] != m;
                    outside += m && (X < trail_lo[Y] || X >= trail_hi[Y]);   /* (pixels the blend would miss) */
                }
            say("test: the kept line mask against a fresh one: %ld pixels differ, %ld outside their rows' spans (%d segments)",
                diff, outside, n_tsegs);
            free(fresh);
        }
    }
    if (trail_colour != trail_lut_mode || speed_from != trail_lut_from) {   /* the colours: the speedometer's, or red */
        int k;
        for (k = 0; k < 256; k++) {
            if (trail_colour) {
                const speedo_rgb c = speedo_colour(k * SPEEDO_VMAX / 255.0, speed_from);
                trail_lut[k] = (uint32_t)(c.r * 255.0f + 0.5f) << 16 | (uint32_t)(c.g * 255.0f + 0.5f) << 8 | (uint32_t)(c.b * 255.0f + 0.5f);
            } else trail_lut[k] = 235u << 16 | 30u << 8 | 30u;
        }
        trail_lut_mode = trail_colour; trail_lut_from = speed_from;
    }
    parallel_rows(trail_blend_rows, rv.y1, rv.y2, (long)(rv.x2 - rv.x1) * (rv.y2 - rv.y1) / 4);
    t_line += qpc() - t0;
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
 * owner sick). Off until Ctrl + A, AutoZoom=1 or the Esc menu. Worked out away from WA on the owner's Round 5 replay (a
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
 * wall across the way can't be stepped over), through the land copy kept for the drawing (land.now), each land pixel it
 * meets checked in WA's own tiles (blown up since: open) */
static double auto_ray(double px, double py, double dx, double dy, double maxd) {
    const double adx = fabs(dx), ady = fabs(dy), major = adx > ady ? adx : ady;
    const double sx = dx / major, sy = dy / major, per = 1.0 / major;
    const int n = (int)(maxd * major), w = land.w, h = land.h;
    const uint8_t *now = land.now, key = land.key;
    double x = px, y = py;
    int i;
    for (i = 1; i <= n; i++) {
        int ix, iy;
        x += sx; y += sy;
        if (x < 0.0 || y < 0.0) return i * per;                 /* (the level's edge: a wall) */
        ix = (int)x; iy = (int)y;
        if (ix >= w || iy >= h) return i * per;
        if ((!now || now[(size_t)iy * w + ix] != key) && land_solid(ix, iy)) return i * per;
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
    int have, inside, track;
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
    /* the track (and the speedometer) in a looser box: high above the level is fine; not WA's far-off place for a worm
     * not placed yet (y -10000) */
    track = turn_worm_ok && land_ok && turn_wx > -4000.0 && turn_wx < land.w + 4000.0 && turn_wy > -8000.0 &&
            turn_wy < land.h + 4000.0;
    if ((have || speedo) && track) {        /* the worm's track, and its speed (the speedometer's too) */
        const double ts = (double)now.QuadPart / clock_freq.QuadPart;
        const int last = (auto_ti + AUTO_TRACK - 1) % AUTO_TRACK;
        int k, j = -1;
        if (turn_key != track_key || trail_turn != track_turn) {   /* another worm or turn: a new track, from rest */
            track_key = turn_key; track_turn = trail_turn;
            auto_tn = 0;
            speed_held = 0.0;
            spd_disp = 0.0;
            spd_lx = turn_wx; spd_ly = turn_wy; spd_moved = 0.0;
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
        spd_disp = spd_disp + (spd_now - spd_disp) * (1.0 - exp(-dt / 0.06));
        if (trail_turn != spd_turn) { spd_turn = trail_turn; spd_best = spd_dist = spd_time = 0.0; }
        if (spd_now > spd_best) spd_best = spd_now;
        {   /* the average: the way it went over the time it was moving (a frozen game -- a paused replay, an online
             * game waiting -- adds nothing: nothing moves) */
            const double step_ = hypot(turn_wx - spd_lx, turn_wy - spd_ly);
            const int moved = step_ > 0.0 && step_ < 300.0;
            if (moved) spd_moved = ts;
            if (spd_moved > 0.0 && ts - spd_moved < 0.1 && spd_now > 30.0) {   /* (the way and the time on the same
                                                                                   frames) */
                spd_time += dt;
                if (moved) spd_dist += step_;
            }
            spd_lx = turn_wx; spd_ly = turn_wy;
            if (test_mode && frames % 120 == 0)   /* (test: the game's speed against the one from the worm's path) */
                say("test: speed: the game's %.0f, from the path %.0f, shown %.0f; the turn's best %.0f, average %.0f (%.0f px in %.1f s)",
                    turn_gv, auto_speed, spd_disp, spd_best, spd_time > 0.5 ? spd_dist / spd_time : 0.0, spd_dist, spd_time);
        }
    } else {
        spd_now = spd_disp = 0.0;
        auto_tn = 0;
        auto_speed = auto_vx = auto_vy = 0.0;
    }
    if (have && !inside) {                          /* the zoom held */
        auto_phase = 0;
        auto_acc = 0.0;
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
    } else {                                        /* (the track is the speedometer's too: kept above) */
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

/* ---- the readout: "150%" in WA's palette ------------------------------------------------------------------------- */
/* the palette's whitest colour, and each colour's darker one (the readout's see-through box) */
static void palette_tables(void *display) {
    const uint8_t *pal = (const uint8_t *)display + D_PALETTE;
    int k;
    if (white_index >= 0 && !memcmp(pal, pal_seen, sizeof(pal_seen))) return;
    memcpy(pal_seen, pal, sizeof(pal_seen));
    lut12_ok = 0;                           /* (the speedometer's table: made again when used) */
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

/* the speedometer (speedo.h): the dial or the number for the turn's worm, in the screen's bottom right corner (above
 * WA's wind bar, and above the zoom number when that is there too) or just above the worm's name (WA draws the name and
 * the energy above the worm, zoomed with the map: the picture's bottom SPEEDO_UP map pixels above the worm's middle, as
 * shown, and 2 screen pixels more); made again when what it shows changes; not over WA's Esc menu. In true colour it is
 * blended into the true-colour frame (compose: see-through, smooth), or over WA's own frame where WA drew it
 * (present_overlay); otherwise put into WA's frame in its palette. */
#define SPEEDO_UP 44.0
static speedo_dial spd_dial;
static speedo_number spd_numpic;
static uint32_t *spd_img;
static int spd_cap, spd_w, spd_h, spd_key[7] = {-1};
static double spd_digits, spd_avg;      /* the digits' speed: taken 10 times a second (as digital gauges do); the
                                           average shown, in tens, twice a second */
static DWORD spd_digits_tick, spd_avg_tick;
static int speedo_will(void *runtime) {   /* (before WA draws the frame) */
    return speedo && worms_ok && turn_worm_ok && auto_tn && !(runtime && *(uint32_t *)((char *)runtime + 0x434) == 1);
}
static void draw_speedo(void *display) {
    uint8_t *frame = *(uint8_t **)VA(FRAME_PTR_VA);
    const int stride = *(int32_t *)VA(FRAME_W_VA), fw = stride, fh = *(int32_t *)VA(FRAME_H_VA);
    const double avg = spd_time > 0.5 ? (spd_dist / spd_time < spd_best ? spd_dist / spd_time : spd_best) : 0.0;
    const int dial = speedo == 1, size = dial ? (fh * 120 / 1080 < 110 ? 110 : fh * 120 / 1080) : (fh * 34 / 1080 < 22 ? 22 : fh * 34 / 1080);
    int w, h, x0, y0, x, y, key[7];
    ov_on = ov_native = 0;
    present_overlay(NULL, 0, 0, 0, 0);
    if (!spd_will || !frame || fw < 64 || fh < 64) return;
    if (dial) { w = h = size; } else speedo_number_size(size, &w, &h);
    if (GetTickCount() - spd_digits_tick >= 100) { spd_digits = spd_disp; spd_digits_tick = GetTickCount(); }
    if (GetTickCount() - spd_avg_tick >= 500 || (avg > 0.0) != (spd_avg > 0.0)) {
        spd_avg = avg > 0.0 ? floor(avg / 10.0 + 0.5) * 10.0 : 0.0;
        spd_avg_tick = GetTickCount();
    }
    key[0] = (int)(spd_digits + 0.5);
    key[1] = dial ? (int)ceil(spd_disp / (SPEEDO_VMAX / SPEEDO_SEGS)) : 0;   /* (the ring: how many segments lit) */
    key[2] = dial ? (int)(spd_best / (SPEEDO_VMAX / SPEEDO_SEGS)) : 0;
    key[3] = dial ? (int)spd_avg : 0;
    key[4] = speedo; key[5] = size; key[6] = (int)speed_from;
    if (memcmp(key, spd_key, sizeof(key)) || !spd_img) {   /* the picture, made again when what it shows changes */
        const LONGLONG t0 = test_mode ? qpc() : 0;
        if (w * h > spd_cap) {
            free(spd_img);
            spd_img = (uint32_t *)malloc((size_t)w * h * 4);
            spd_cap = spd_img ? w * h : 0;
            if (!spd_img) return;
        }
        if (dial ? !speedo_dial_paint(&spd_dial, size, speed_from, spd_disp, key[0], spd_best, spd_avg, spd_img, w * 4)
                 : !speedo_number_paint(&spd_numpic, size, key[0], speed_from, spd_img, w * 4)) { spd_key[0] = -1; return; }
        memcpy(spd_key, key, sizeof(key));
        spd_w = w; spd_h = h;
        if (test_mode) {                    /* (test: what a repaint costs) */
            static double sum, worst;
            static unsigned n;
            const double ms = (double)(qpc() - t0) * 1000.0 / clock_freq.QuadPart;
            sum += ms; n++;
            if (ms > worst) worst = ms;
            if (n % 300 == 0) say("test: speedometer: %u repaints, %.3f ms each on average, %.3f ms at most (%d px)", n, sum / n, worst, size);
        }
    }
    if (speedo_place == 1) {                /* above the worm's name */
        if (!spd_cam) return;
        x0 = (int)floor(spd_ox + (turn_wx - spd_cx) * spd_z - w / 2.0 + 0.5);
        y0 = (int)floor(spd_oy + (turn_wy - SPEEDO_UP - spd_cy) * spd_z + 0.5) - 2 - h;
    } else {                                /* the bottom right corner */
        x0 = fw - w - fh * 14 / 1080;
        y0 = fh - h - 30 - fh * 8 / 1080;   /* (WA's wind bar: about 30 px at the bottom, at any size) */
        if (ind_place == PLACE_BOTTOM_RIGHT && (ind_mode != IND_OFF || message_on())) {   /* (above the zoom number) */
            int f, bw, bh;
            badge_size(fh, &f, &bw, &bh);
            y0 = fh - bh - fh * 120 / 1080 - 8 - h;
        }
    }
    if (x0 + w <= 0 || x0 >= fw || y0 + h <= 0 || y0 >= fh) return;
    if (tc.on && tc_ok && present_live()) { /* the true-colour frame: blended in there (compose) */
        ov_img = spd_img; ov_x0 = x0; ov_y0 = y0; ov_w = w; ov_h = h; ov_on = 1;
        return;
    }
    if (!tc.on && tc_ok && present_overlay_live()) {   /* a frame WA drew itself (100%): put over it as it is shown (the
                                                          map was redrawn by the module for this until 2026-11-23) */
        present_overlay(spd_img, x0, y0, w, h);
        ov_img = spd_img; ov_x0 = x0; ov_y0 = y0; ov_w = w; ov_h = h; ov_native = 1;   /* (for the test dumps) */
        return;
    }
    palette_tables(display);                /* WA's own frame: its palette's nearest colours; the shadow darkens */
    if (!lut12_ok) {
        const uint8_t *pal = (const uint8_t *)display + D_PALETTE;
        int k;
        for (k = 0; k < 4096; k++) lut12[k] = (uint8_t)pal_nearest(pal, (k >> 8) * 17, ((k >> 4) & 15) * 17, (k & 15) * 17);
        lut12_ok = 1;
    }
    for (y = 0; y < h; y++) {
        uint8_t *row;
        if (y0 + y < 0 || y0 + y >= fh) continue;
        row = frame + (size_t)(y0 + y) * stride;
        for (x = 0; x < w; x++) {
            const uint32_t c = spd_img[y * w + x], a = c >> 24;
            uint32_t r, g, b;
            if (a < 100 || x0 + x < 0 || x0 + x >= fw) continue;
            r = ((c >> 16) & 0xff) * 255 / a; g = ((c >> 8) & 0xff) * 255 / a; b = (c & 0xff) * 255 / a;
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            if (r < 40 && g < 40 && b < 40) row[x0 + x] = dark_lut[row[x0 + x]];
            else if (a >= 150) row[x0 + x] = lut12[(r >> 4) << 8 | (g >> 4) << 4 | b >> 4];
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
    if (message_on()) snprintf(text, sizeof(text), "%s", ind_msg); else snprintf(text, sizeof(text), "%d%%", pct);
    if (strcmp(text, ind_text) || fh != ind_lines || !ind_img) {   /* the picture, made again when its text changes */
        int f;
        badge_size(fh, &f, &ind_bw, &ind_bh);
        if (message_on()) ind_bw = badge_text_width(f, text);
        if (ind_bw > fw - 8) ind_bw = fw - 8;
        if (!ind_img || pow2(ind_bw) != ind_tw || pow2(ind_bh) != ind_th) {
            free(ind_img);
            ind_tw = pow2(ind_bw); ind_th = pow2(ind_bh);
            ind_img = (uint32_t *)malloc((size_t)ind_tw * ind_th * 4);
            if (!ind_img) return;
        }
        if (!badge_paint_text(text, f, ind_bw, ind_bh, ind_tw, ind_th, ind_img, ind_tw * 4)) { ind_text[0] = 0; return; }
        snprintf(ind_text, sizeof(ind_text), "%s", text);
        ind_pct = pct; ind_lines = fh;
    }
    palette_tables(display);
    m = fh * 16 / 1080;
    x0 = ind_place == PLACE_TOP_LEFT || ind_place == PLACE_BOTTOM_LEFT ? m
       : ind_place == PLACE_TOP || ind_place == PLACE_BOTTOM ? (fw - ind_bw) / 2 : fw - ind_bw - m;
    y0 = ind_place >= PLACE_BOTTOM_RIGHT ? fh - ind_bh - fh * 120 / 1080 : (fh * 110 / 1080 > 80 ? fh * 110 / 1080 : 80);
    level = (int)(a * 16.0 + 0.5);          /* (fading) */
    blit_badge(ind_img, ind_tw, ind_bw, ind_bh, x0, y0, level);
}

/* ---- test mode: WA's frame as a BMP ------------------------------------------------------------------------------ */
static void write_bmp(const char *name, int w, int h, const uint32_t *px) {
    char path[MAX_PATH + 64];
    uint8_t hdr[54] = {'B', 'M'}, *row;
    uint32_t rowb = ((uint32_t)w * 3 + 3) & ~3u, img = rowb * (uint32_t)h, fsz = 54 + img, off = 54, isz = 40;
    int32_t hgt = -h;
    uint16_t planes = 1, bpp = 24;
    int x, y;
    FILE *f;
    snprintf(path, sizeof(path), "%s\\%s.bmp", dump_dir, name);
    if (!(f = fopen(path, "wb"))) return;
    memcpy(hdr + 2, &fsz, 4); memcpy(hdr + 10, &off, 4); memcpy(hdr + 14, &isz, 4); memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &hgt, 4); memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bpp, 2); memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, 54, f);
    row = (uint8_t *)calloc(rowb, 1);
    for (y = 0; row && y < h; y++) {
        for (x = 0; x < w; x++) {
            uint32_t c = px[(size_t)y * w + x];
            row[3 * x] = (uint8_t)c; row[3 * x + 1] = (uint8_t)(c >> 8); row[3 * x + 2] = (uint8_t)(c >> 16);
        }
        fwrite(row, 1, rowb, f);
    }
    free(row);
    fclose(f);
    say("test: saved %s (%dx%d, zoom %.3f)", path, w, h, zoom_now);
}

/* WA's 8-bit frame through its palette, and (when this frame has one) the true-colour frame */
static void dump_frame(void *display, const char *name) {
    char tcname[96];
    const uint8_t *frame = *(const uint8_t **)VA(FRAME_PTR_VA);
    int stride = *(int32_t *)VA(FRAME_W_VA), w = stride, h = *(int32_t *)VA(FRAME_H_VA), x, y;
    uint32_t *px;
    (void)display;
    if (!frame || w <= 0 || h <= 0 || !(px = (uint32_t *)malloc((size_t)w * h * 4))) return;
    for (y = 0; y < h; y++) for (x = 0; x < w; x++) px[(size_t)y * w + x] = pal32[frame[(size_t)y * stride + x]];
    write_bmp(name, w, h, px);
    if (tc.on && compose((uint8_t *)px, w * 4, w, h)) {
        snprintf(tcname, sizeof(tcname), "%s-truecolour", name);
        write_bmp(tcname, w, h, px);
    } else if (!tc.on && ov_native) {       /* (WA's own frame with the speedometer over it, as present.c shows it) */
        for (y = ov_y0 < 0 ? 0 : ov_y0; y < ov_y0 + ov_h && y < h; y++)
            for (x = ov_x0 < 0 ? 0 : ov_x0; x < ov_x0 + ov_w && x < w; x++) {
                const uint32_t c = ov_img[(size_t)(y - ov_y0) * ov_w + (x - ov_x0)], na = 255u - (c >> 24), b = px[(size_t)y * w + x];
                px[(size_t)y * w + x] = (((c >> 16) & 0xff) + ((b >> 16) & 0xff) * na / 255u) << 16 |
                                        (((c >> 8) & 0xff) + ((b >> 8) & 0xff) * na / 255u) << 8 | ((c & 0xff) + (b & 0xff) * na / 255u);
            }
        snprintf(tcname, sizeof(tcname), "%s-overlay", name);
        write_bmp(tcname, w, h, px);
    }
    free(px);
}

/* 100% for a moment, out to the farthest, in to the closest, back to 100%; WA's frame saved in each */
static int test_switch_w, test_switch_h;
static int test_fullscreen, test_toggle, test_gone, test_recover, test_bad_w, test_bad_h;
static int test_chat;                   /* WKMAPZOOM_TEST_CHAT=1: chat lines before and after the switch, frames saved (=off: the fix off) */
static double test_zoom;                /* WKMAPZOOM_TEST_ZOOM: one zoom all along, no zoom script */
static void test_script(void *display) {
    static int shown_at = -1, trail_dumps;
    if (trail_visible && trail_show >= 0 && trail_hl && shown_at < 0) shown_at = (int)frames;
    if (!(trail_visible && trail_show >= 0 && trail_hl)) shown_at = -1;
    if (shown_at >= 0 && (int)frames == shown_at + 20 && trail_dumps < 2) {
        char name[32];
        snprintf(name, sizeof(name), "trail-%d", ++trail_dumps);
        dump_frame(display, name);
    }
    if (trail_kept_at >= 0 && (int)frames == trail_kept_at + 90) {   /* (1.5 s after it moved, the line kept) */
        static int kept_dumps;
        char name[32];
        if (kept_dumps < 2) { snprintf(name, sizeof(name), "kept-%d", ++kept_dumps); dump_frame(display, name); }
    }
    if (auto_zoom && frames % 600 == 300 && frames <= 3900) {
        char name[32];
        snprintf(name, sizeof(name), "auto-%02u", frames / 600);
        dump_frame(display, name);
    }
    {   /* frames asked for by number (WKMAPZOOM_TEST_DUMPAT=700,1200): saved as at-<frame>, with where the turn's worm is
         * on the screen */
        static char at[128];
        static int read_at;
        char *q, name[32];
        if (!read_at) { read_at = 1; if (!GetEnvironmentVariableA("WKMAPZOOM_TEST_DUMPAT", at, sizeof(at))) at[0] = 0; }
        for (q = at; *q; ) {
            const unsigned f = (unsigned)strtoul(q, &q, 10);
            if (f && f == frames) {
                snprintf(name, sizeof(name), "at-%u", f);
                say("test: frame %u: the turn's worm at %.0f,%.0f, on the screen at %.0f,%.0f (zoom %.2f)", f, turn_wx, turn_wy,
                    spd_ox + (turn_wx - spd_cx) * spd_z, spd_oy + (turn_wy - spd_cy) * spd_z, spd_z);
                dump_frame(display, name);
            }
            while (*q == ',' || *q == ' ') q++;
            if (!f) break;
        }
    }
    {   /* the speedometer at the first fast moments (over 700 px/s, a few seconds apart) */
        static int fast_dumps;
        static unsigned fast_at;
        if (speedo && spd_now > 700.0 && fast_dumps < 3 && (!fast_at || frames - fast_at > 300)) {
            char name[32];
            fast_at = frames;
            snprintf(name, sizeof(name), "speedo-fast-%d", ++fast_dumps);
            say("test: the speedometer at %.0f px/s (best %.0f, the turn's average %.0f)", spd_now, spd_best,
                spd_time > 0.5 ? spd_dist / spd_time : 0.0);
            dump_frame(display, name);
        }
    }
    if (test_zoom > 0.0) return;
    switch (frames) {
    case 190: dump_frame(display, "1-zoom100"); set_goal(zoom_lo, "test script"); break;
    case 260: dump_frame(display, "2-zoomout-readout"); break;
    case 390: dump_frame(display, "3-zoomout"); set_goal(zoom_hi, "test script"); break;
    case 460: dump_frame(display, "4-zoomin-readout"); break;
    case 590: dump_frame(display, "5-zoomin"); set_goal(1.0, "test script"); break;
    case 660: dump_frame(display, "6-zoom100-readout"); break;
    case 640:
        if (test_chat && esc_menu) {
            escmenu_test_chat("test: where does the chat start? 0123456789 abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789 the end", 1);
            escmenu_test_chat("test: a second line", 0);
        }
        break;
    case 670: if (test_switch_w) set_goal(0.5, "test script: zoomed for the switch"); break;   /* (the switch zoomed: the
                                                                                             owner's crash, 2026-11-22) */
    case 690: if (test_chat && esc_menu) { escmenu_test_chat_check("before the switch"); dump_frame(display, "13a-chat-before"); } break;
    case 700: if (test_switch_w && esc_menu) escmenu_ask(test_switch_w, test_switch_h); break;
    case 703: if (test_switch_w) dump_frame(display, "8a-just-switched"); break;
    case 712:
        if (test_chat && esc_menu) {
            escmenu_test_chat_check("after the switch");
            dump_frame(display, "13b-chat-after");
            escmenu_test_chat("test: a line after the switch, wrapped to the new width: 0123456789 abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789", 0);
        }
        break;
    case 730: if (test_chat) dump_frame(display, "13c-chat-new-line"); break;
    case 720: if (test_esc) escmenu_test_open(); break;
    case 760:
        if (test_switch_w) set_goal(0.5, "test script");
        if (test_esc) { dump_frame(display, "9-escmenu"); if (test_esc == 2) escmenu_test_options(); else escmenu_test_list(); }
        break;
    case 790: if (test_esc) dump_frame(display, test_esc == 2 ? "10-escmenu-options" : "10-escmenu-list"); break;
    case 800:
        if (test_esc == 2) {
            escmenu_test_click(OPT_DEFAULT); escmenu_test_click(OPT_AUTO_IN); escmenu_test_click(OPT_LINE);
            escmenu_test_click(OPT_SPEED_STRENGTH);
            if (test_switch_w) set_goal(0.5, "test script");   /* (the switch test's zoom kept) */
        }
        break;
    case 815: if (test_esc == 2) escmenu_test_more(); break;   /* (the options' pages: the second, then the third) */
    case 822: if (test_esc == 2) dump_frame(display, "11a-escmenu-options-page2"); break;
    case 825: if (test_esc == 2) escmenu_test_more(); break;
    case 830: if (test_esc == 2) dump_frame(display, "11-escmenu-options-clicked"); break;
    case 840: if (test_switch_w) dump_frame(display, "8-switched-zoom50"); break;
    case 900: dump_frame(display, "7-zoom100"); break;
    case 905: case 910: {                   /* numpad + then numpad -, posted to WA's own window (through the message hook) */
        HWND wnd = (HWND)(uintptr_t)*(uint32_t *)VA(0x6b390cu);
        if (wnd) PostMessageA(wnd, WM_KEYDOWN, frames == 905 ? VK_ADD : VK_SUBTRACT, 1);
        break;
    }
    case 940: if (test_toggle && esc_menu) escmenu_ask(0, 0); break;                    /* windowed <-> fullscreen */
    case 943: if (test_toggle) dump_frame(display, "12a-toggled"); break;
    case 1000: if (test_toggle && esc_menu) escmenu_ask(0, 0); break;                   /* and back */
    case 1003: if (test_toggle) dump_frame(display, "12b-toggled-back"); break;
    case 1040: if (test_gone && present_ok) present_test_gone(30); break;                 /* 30 frames as with no device: skipped */
    case 1080: if (test_gone) dump_frame(display, "13-after-no-device"); break;
    case 1100: if (test_recover && present_ok) escmenu_test_recover(); break;             /* a new device, as the recovery makes one */
    case 1103: if (test_recover) dump_frame(display, "14a-recovered"); break;
    case 1160: if (test_bad_w && esc_menu) escmenu_test_any_size(test_bad_w, test_bad_h); break;   /* (fullscreen: Reset refused, rollback) */
    case 1200: if (test_bad_w) dump_frame(display, "15-after-bad-size"); break;
    }
}

/* ---- the frame --------------------------------------------------------------------------------------------------- */
static void __fastcall hook_render(void *runtime) {
    void *world = runtime ? *(void **)((char *)runtime + RT_WORLD) : NULL;
    void *display = world ? *(void **)((char *)world + WO_DISPLAY) : NULL;
    frame_display = NULL;
    frame_grid = NULL;
    frame_zoom = 1.0;
    tc.on = 0;
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
            trail_reset();
            if (test_mode && test_zoom > 0.0) zoom_now = zoom_goal = zoom_clamp(test_zoom, zoom_lo, zoom_hi);
            auto_factor = auto_goal = 1.0;
            auto_lg_ok = 0;
            spd_turn = -1; spd_best = spd_dist = spd_time = 0.0; track_key = track_turn = -1;
            auto_tn = 0;
            auto_phase = 0;
            auto_acc = 0.0;
            say("a new game (world %p), zoom %.0f%%", world, zoom_goal * 100.0);
        } else if (land_changed) {          /* the same game, its land made of other tile objects: copied again */
            say("land: WA's tiles are other objects now (mid-game): the land copied again");
            land_free();
        }
        palette_rgb(display);
        if (!land_ok) land_setup(display);
        bg_start();                         /* (the land's copies made ahead, in the background: see bg_builder) */
        trail_frame(world);
        auto_zoom_frame();
        frame_display = display;
        frame_grid = *(void **)((char *)display + D_GRID);
        frame_zoom = eff_zoom();
        live_tick = GetTickCount();
    }
    if (esc_menu || present_ok) escmenu_frame(runtime);
    spd_cam = 0;
    spd_will = frame_display && speedo_will(runtime);
    original_render(runtime);
    if (frame_display) {
        frames++;
        draw_speedo(display);
        draw_readout(display);
        if (tc.on && tc_ok) present_ready();
        if (test_mode) test_script(display);   /* (the frame as shown: with the readout) */
        advance();
    }
    frame_display = NULL;
    frame_zoom = 1.0;
}

/* ---- the options in WA's Esc menu ("Map zoom options"), kept in wkMapZoom.ini --------------------------------------- */
static char ini_path[MAX_PATH];

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

static const double opt_defaults[] = { 0.5, 0.6, 0.7, 0.8, 0.9, 1.0, 1.25, 1.5, 2.0 };   /* Default zoom */
static const double opt_outs[] = { 0.25, 0.35, 0.5, 0.7, 1.0 };       /* Auto zoom out to */
static const double opt_ins[] = { 1.0, 1.25, 1.5, 2.0, 2.5, 3.0 };    /* Auto zoom in to */
static const double opt_froms[] = { 400.0, 500.0, 600.0, 700.0, 800.0, 1000.0, 1200.0 };   /* Speed zoom from */
static const double opt_strengths[] = { 0.9, 0.8, 0.7, 0.6, 0.5 };    /* Speed zoom strength (each click stronger) */

static int opt_count(void) { return worms_ok ? OPT_COUNT : 2; }

/* (without the worms' hooks: only the default zoom and the zoom number) */
static int opt_which(int i) { return worms_ok ? i : i == 0 ? OPT_DEFAULT : OPT_NUMBER; }

/* the pages' groups (escmenu.c pages them when WA's menu has no room for all): general, the auto zoom, speed */
static int opt_group(int i) {
    const int o = opt_which(i);
    return o >= OPT_SPEEDO ? 2 : o >= OPT_AUTO ? 1 : 0;
}

static double default_zoom_now(void) { return zoom_clamp(zoom_default, zoom_lo, zoom_hi); }   /* (the one games start at) */

static void opt_label(int i, char *out, size_t n) {
    switch (opt_which(i)) {
    case OPT_DEFAULT: snprintf(out, n, "Default zoom: %.0f%%", default_zoom_now() * 100.0); break;
    case OPT_SPEEDO: snprintf(out, n, "Speedometer: %s", speedo == 1 ? "Dial" : speedo == 2 ? "Number" : "Off"); break;
    case OPT_SPEEDO_PLACE: snprintf(out, n, "Speedometer place: %s", speedo_place ? "Above the worm" : "Bottom right"); break;
    case OPT_SPEED: snprintf(out, n, "Speed zoom: %s", speed_zoom ? "On" : "Off"); break;
    case OPT_SPEED_FROM: snprintf(out, n, "Speed zoom from: %.0f px/s", speed_from); break;
    case OPT_SPEED_STRENGTH: snprintf(out, n, "Speed zoom strength: x%.2g per %.0f px/s", speed_mult, SPEED_STEP); break;
    case OPT_AUTO: snprintf(out, n, "Auto zoom: %s", auto_zoom ? "On" : "Off"); break;
    case OPT_AUTO_OUT:
        if (auto_min >= 0.999) snprintf(out, n, "Auto zoom out to: never");
        else snprintf(out, n, "Auto zoom out to: %.0f%%", auto_min * 100.0);
        break;
    case OPT_AUTO_IN:
        if (auto_max <= 1.001) snprintf(out, n, "Auto zoom in to: never");
        else snprintf(out, n, "Auto zoom in to: %.0f%%", auto_max * 100.0);
        break;
    case OPT_LINE:
        snprintf(out, n, "Movement line: %s%s", !trail_on ? "Off" : trail_all ? "Whole game" : "Last turn", trail_on && trail_keep ? ", stays" : "");
        break;
    case OPT_LINE_COLOUR: snprintf(out, n, "Movement line colour: %s", trail_colour ? "Speed" : "Red"); break;
    default:
        snprintf(out, n, "Zoom number: %s", ind_mode == IND_OFF ? "Off" : ind_mode == IND_ALWAYS ? "Always" : "While zooming");
        break;
    }
}

/* the next value in a list after v (the first after the last), within [lo, hi] */
static double opt_next(const double *list, int n, double v, double lo, double hi) {
    int k, first = -1;
    for (k = 0; k < n; k++) {
        if (list[k] < lo - 1e-9 || list[k] > hi + 1e-9) continue;
        if (first < 0) first = k;
        if (list[k] > v + 1e-6) return list[k];
    }
    return first >= 0 ? list[first] : v;
}

static void opt_click(int i) {
    char v[32];
    switch (opt_which(i)) {
    case OPT_DEFAULT:                           /* (taken at once too, so it shows behind the menu) */
        zoom_default = opt_next(opt_defaults, (int)(sizeof(opt_defaults) / sizeof(opt_defaults[0])), default_zoom_now(), zoom_lo, zoom_hi);
        snprintf(v, sizeof(v), "%.2f", zoom_default);
        save_setting("DefaultZoom", v);
        set_goal(zoom_default, "Esc menu: the default zoom");
        break;
    case OPT_LINE_COLOUR:
        trail_colour = !trail_colour;
        save_setting("TurnLineColour", trail_colour ? "speed" : "red");
        break;
    case OPT_SPEEDO:                            /* dial -> number -> off -> dial */
        speedo = speedo == 1 ? 2 : speedo == 2 ? 0 : 1;
        save_setting("Speedometer", speedo == 1 ? "dial" : speedo == 2 ? "number" : "off");
        break;
    case OPT_SPEEDO_PLACE:
        speedo_place = !speedo_place;
        save_setting("SpeedometerPlace", speedo_place ? "worm" : "corner");
        break;
    case OPT_SPEED:
        speed_zoom = !speed_zoom;
        say("speed zoom %s (Esc menu)", speed_zoom ? "on" : "off");
        save_setting("SpeedZoom", speed_zoom ? "1" : "0");
        break;
    case OPT_SPEED_FROM:
        speed_from = opt_next(opt_froms, (int)(sizeof(opt_froms) / sizeof(opt_froms[0])), speed_from, 0.0, 1e9);
        snprintf(v, sizeof(v), "%.0f", speed_from);
        save_setting("SpeedZoomFrom", v);
        break;
    case OPT_SPEED_STRENGTH: {                  /* (the next smaller multiplier: stronger; after the strongest the weakest) */
        const int nn = (int)(sizeof(opt_strengths) / sizeof(opt_strengths[0]));
        int k;
        for (k = 0; k < nn && opt_strengths[k] >= speed_mult - 1e-6; k++) {}
        speed_mult = k < nn ? opt_strengths[k] : opt_strengths[0];
        snprintf(v, sizeof(v), "%.2f", speed_mult);
        save_setting("SpeedZoomStrength", v);
        break;
    }
    case OPT_AUTO: set_auto_zoom(!auto_zoom, "Esc menu"); break;
    case OPT_AUTO_OUT:
        auto_min = opt_next(opt_outs, (int)(sizeof(opt_outs) / sizeof(opt_outs[0])), auto_min, zoom_lo, 1.0);
        snprintf(v, sizeof(v), "%.2f", auto_min);
        save_setting("AutoZoomMin", v);
        break;
    case OPT_AUTO_IN:
        auto_max = opt_next(opt_ins, (int)(sizeof(opt_ins) / sizeof(opt_ins[0])), auto_max, 1.0, zoom_hi);
        snprintf(v, sizeof(v), "%.2f", auto_max);
        save_setting("AutoZoomMax", v);
        break;
    case OPT_LINE: {                            /* whole game -> whole game, stays -> last turn -> last turn, stays -> off */
        const int was_keep = trail_keep;
        if (!trail_on) { trail_on = 1; trail_all = 1; trail_keep = 0; }
        else if (!trail_keep) trail_keep = 1;
        else if (trail_all) { trail_all = 0; trail_keep = 0; }
        else { trail_on = 0; trail_keep = 0; }
        if (!trail_on) { trail_visible = 0; trail_show = -1; }
        save_setting("TurnLine", !trail_on ? "off" : trail_all ? "all" : "last");
        if (trail_keep != was_keep) save_setting("TurnLineKeep", trail_keep ? "1" : "0");
        break;
    }
    default:
        ind_mode = ind_mode == IND_ZOOMING ? IND_ALWAYS : ind_mode == IND_ALWAYS ? IND_OFF : IND_ZOOMING;
        save_setting("Indicator", ind_mode == IND_OFF ? "off" : ind_mode == IND_ALWAYS ? "always" : "zooming");
        break;
    }
}

static const escmenu_options menu_options = { opt_count, opt_label, opt_click, opt_group };

static LRESULT CALLBACK on_message(int code, WPARAM wp, LPARAM lp) {
    MSG *m = (MSG *)lp;
    if (code == HC_ACTION && wp == PM_REMOVE && m) {
        if ((esc_menu || present_ok) && escmenu_message(m)) m->message = WM_NULL;
        else if (wbuf && GetTickCount() - live_tick < 1000) {
            if (m->message == WM_MOUSEWHEEL && (GET_KEYSTATE_WPARAM(m->wParam) & MK_CONTROL)) {
                set_goal(zoom_step2(zoom_goal, GET_WHEEL_DELTA_WPARAM(m->wParam) / (double)WHEEL_DELTA, cfg.step, default_zoom_now()),
                         "Ctrl + wheel");   /* (100% and the default zoom: stops on the way through) */
                m->message = WM_NULL;
            } else if ((m->message == WM_MBUTTONDOWN || m->message == WM_MBUTTONUP || m->message == WM_MBUTTONDBLCLK) &&
                       (GET_KEYSTATE_WPARAM(m->wParam) & MK_CONTROL)) {
                if (m->message != WM_MBUTTONUP) set_goal(zoom_default, "Ctrl + middle click");
                m->message = WM_NULL;
            } else if (m->message == WM_KEYDOWN && m->wParam == 'A' && GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0) {
                if (!(m->lParam & (1 << 30))) set_auto_zoom(!auto_zoom, "Ctrl + A");   /* (not the key's repeats) */
                m->message = WM_NULL;
            } else if (numpad_keys && m->message == WM_KEYDOWN &&
                       (m->wParam == VK_ADD || m->wParam == VK_SUBTRACT || m->wParam == VK_MULTIPLY)) {
                if (m->wParam == VK_MULTIPLY) set_goal(zoom_default, "numpad *");
                else set_goal(zoom_step2(zoom_goal, m->wParam == VK_ADD ? 1.0 : -1.0, cfg.step, default_zoom_now()), "numpad");
                m->message = WM_NULL;
            }
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

static void read_settings(const char *exe) {
    char ini[MAX_PATH], v[64], *slash;
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
        unsigned i;
        for (i = 0; i < sizeof(places) / sizeof(places[0]); i++) if (!lstrcmpiA(v, places[i])) ind_place = (int)i;
    }
    numpad_keys = ini_flag(ini, "NumpadKeys", 1);
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
    if (GetPrivateProfileStringA("wkMapZoom", "Speedometer", "", v, sizeof(v), ini) && v[0])
        speedo = !lstrcmpiA(v, "off") || !strcmp(v, "0") ? 0 : !lstrcmpiA(v, "number") || !strcmp(v, "2") ? 2 : 1;
    if (GetPrivateProfileStringA("wkMapZoom", "SpeedometerPlace", "", v, sizeof(v), ini) && v[0])
        speedo_place = !lstrcmpiA(v, "worm") || !lstrcmpiA(v, "above") || !strcmp(v, "1");
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
    if (GetPrivateProfileStringA("wkMapZoom", "TurnLine", "", v, sizeof(v), ini) && v[0]) {
        trail_on = lstrcmpiA(v, "off") && strcmp(v, "0");
        trail_all = lstrcmpiA(v, "last") != 0;
    }
    trail_keep = ini_flag(ini, "TurnLineKeep", 0);
    if (GetPrivateProfileStringA("wkMapZoom", "TurnLineColour", "", v, sizeof(v), ini) && v[0])
        trail_colour = lstrcmpiA(v, "red") && strcmp(v, "0");
    true_colour = ini_flag(ini, "TrueColour", 1);
    colour_maps = ini_flag(ini, "ColourMaps", 1);
    esc_menu = ini_flag(ini, "EscMenu", 1);
    if (GetPrivateProfileStringA("wkMapZoom", "MapTheme", "", v, sizeof(v), ini) && v[0]) {
        int t = colourmap_theme(v);
        if (t >= 0) map_theme = t; else say("MapTheme=%s: not one of WA's themes (Forest is used)", v);
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
    static const unsigned char bitmap_bytes[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x83, 0xec, 0x24, 0x53, 0x56};
    static const unsigned char fill_bytes[] = {0x8b, 0x81, 0x60, 0x35, 0x00, 0x00, 0x01, 0x44, 0x24, 0x04};
    static const unsigned char grid_bytes[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x8b, 0x4d, 0x10, 0x8b, 0x45, 0x18};
    static const unsigned char pixc_bytes[] = {0x8b, 0x54, 0x24, 0x04, 0x3b, 0x51, 0x1c, 0x7c, 0x25};
    static const unsigned char pix_bytes[] = {0x8b, 0x41, 0x10, 0x0f, 0xaf, 0x44, 0x24, 0x08, 0x03, 0x41, 0x08};
    static const unsigned char vline_bytes[] = {0x8b, 0x41, 0x10, 0x8b, 0x54, 0x24, 0x0c, 0x56, 0x8b, 0x74, 0x24, 0x0c};
    static const unsigned char gfill_bytes[] = {0x8b, 0x44, 0x24, 0x14, 0x8b, 0x54, 0x24, 0x10, 0x50};
    static const unsigned char order_bytes[] = {0x8b, 0x44, 0x24, 0x04, 0x8b, 0x08, 0x8b, 0x54, 0x24, 0x08};
    const hook_def hooks[] = {
        {GAME_RENDER_VA, render_bytes, sizeof(render_bytes), (void *)hook_render, (void **)&original_render, "GameRender"},
        {QUEUE_VA, queue_bytes, sizeof(queue_bytes), (void *)thunk_queue, &orig_queue, "RenderDrawingQueue"},
        {CLAMP_VA, clamp_bytes, sizeof(clamp_bytes), (void *)thunk_clamp, &orig_clamp, "the camera clamp"},
        /* (the camera follow: at its two calls, see follow_sites_install) */
        {BITMAP_VA, bitmap_bytes, sizeof(bitmap_bytes), (void *)thunk_bitmap, &orig_bitmap, "the bitmap blit"},
        {FILL_VA, fill_bytes, sizeof(fill_bytes), (void *)hook_fill, (void **)&original_fill, "the display fill"},
        {GRID_VA, grid_bytes, sizeof(grid_bytes), (void *)thunk_grid, &orig_grid, "the sprite blit"},
        {PIXEL_CLIPPED_VA, pixc_bytes, sizeof(pixc_bytes), (void *)thunk_pixel_clipped, &orig_pixel_clipped, "the pixel"},
        {PIXEL_VA, pix_bytes, sizeof(pix_bytes), (void *)thunk_pixel, &orig_pixel, "the unclipped pixel"},
        {VLINE_VA, vline_bytes, sizeof(vline_bytes), (void *)thunk_vline, &orig_vline, "the line"},
        {GRID_FILL_VA, gfill_bytes, sizeof(gfill_bytes), (void *)thunk_grid_fill, &orig_grid_fill, "the grid fill"},
    };
    char exe[MAX_PATH], path[MAX_PATH], dir[MAX_PATH], *slash;
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
                                               still in WA -- the friend's crash, 2026-11-22) */
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
        else
            say("another module hooks WA's drawing too (another copy of wkMapZoom, wkVista or WormForge's zoom?): not installed "
                "-- use one zoom module, and one copy of it, at a time");
        return;
    }
    read_settings(exe);
    QueryPerformanceFrequency(&clock_freq);
    test_mode = GetEnvironmentVariableA("WKMAPZOOM_TEST", path, sizeof(path)) > 0;
    test_checks = test_mode && GetEnvironmentVariableA("WKMAPZOOM_TEST_NOCHECK", NULL, 0) == 0;
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
        if (GetEnvironmentVariableA("WKMAPZOOM_TEST_SWITCH", res, sizeof(res)) &&
            (sscanf(res, "%dx%d", &test_switch_w, &test_switch_h) != 2 || test_switch_w < 640 || test_switch_h < 480))
            test_switch_w = test_switch_h = 0;
        test_fullcolour = GetEnvironmentVariableA("WKMAPZOOM_TEST_FULLCOLOUR", res, sizeof(res)) > 0;
        test_esc = GetEnvironmentVariableA("WKMAPZOOM_TEST_ESC", res, sizeof(res)) > 0 ? (!lstrcmpiA(res, "options") ? 2 : 1) : 0;
        if (GetEnvironmentVariableA("WKMAPZOOM_TEST_ZOOM", res, sizeof(res)) > 0) test_zoom = atof(res);
        if (GetEnvironmentVariableA("WKMAPZOOM_TEST_AUTOZOOM", res, sizeof(res)) > 0) auto_zoom = 1;
        test_toggle = GetEnvironmentVariableA("WKMAPZOOM_TEST_TOGGLE", res, sizeof(res)) > 0;
        if (GetEnvironmentVariableA("WKMAPZOOM_TEST_CHAT", res, sizeof(res)) > 0) { test_chat = 1; if (!strcmp(res, "off")) escmenu_test_chatfix(0); }
        test_gone = GetEnvironmentVariableA("WKMAPZOOM_TEST_GONE", res, sizeof(res)) > 0;
        test_recover = GetEnvironmentVariableA("WKMAPZOOM_TEST_RECOVER", res, sizeof(res)) > 0;
        if (GetEnvironmentVariableA("WKMAPZOOM_TEST_BADSIZE", res, sizeof(res)) > 0 && sscanf(res, "%dx%d", &test_bad_w, &test_bad_h) != 2)
            test_bad_w = test_bad_h = 0;
        if (GetEnvironmentVariableA("WKMAPZOOM_TEST_FULLSCREEN", res, sizeof(res)) > 0) {   /* WxH (a monitor mode), or 1: the desktop's */
            DEVMODEA dm;
            memset(&dm, 0, sizeof(dm));
            dm.dmSize = sizeof(dm);
            test_fullscreen = 1;
            if (sscanf(res, "%dx%d", &tw, &th) != 2 && EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm)) {
                tw = (int)dm.dmPelsWidth; th = (int)dm.dmPelsHeight;
            }
        }
        *(int32_t *)VA(OPT_WINDOWED_VA) = test_fullscreen ? 0 : 1;
        *(int32_t *)VA(OPT_DISPLAY_X_VA) = tw; *(int32_t *)VA(OPT_DISPLAY_Y_VA) = th;
        *(int32_t *)VA(OPT_WINDOW_X_VA) = tw; *(int32_t *)VA(OPT_WINDOW_Y_VA) = th;
        *(int32_t *)VA(OPT_RENDERER_VA) = GetEnvironmentVariableA("WKMAPZOOM_TEST_RENDERER", res, sizeof(res)) > 0 ? atoi(res) : 4;
        say("test mode: %s %dx%d, scripted zoom, frames to %s", test_fullscreen ? "fullscreen" : "windowed", tw, th, dump_dir);
    }
    say("WA starts at %dx%d, %s, renderer %d (its options as read)", *(int32_t *)VA(OPT_DISPLAY_X_VA), *(int32_t *)VA(OPT_DISPLAY_Y_VA),
        *(int32_t *)VA(OPT_WINDOWED_VA) ? "windowed" : "fullscreen", *(int32_t *)VA(OPT_RENDERER_VA));
    {   /* Pinned before any of WA's code or imports point into this DLL (its hooks, its CreateFile imports, its message
         * hook), so it is never unloaded while WA runs. A friend's WA crashed (2026-11-22) with WA's camera follow
         * jumping into a copy of this module that was no longer loaded (unloaded and loaded again by something; the
         * copy loaded again took the old jumps for another zoom module's and stood down). Pinned, an unload is a no-op
         * and a reload gets this very copy back, hooks and all. Not pinned, it doesn't install. */
        const handle_ex_fn ex = handle_ex();
        HMODULE me;
        if (!ex || !ex(0x1 | 0x4, (LPCSTR)(void *)&install, &me)) {
            say("could not pin the module in memory (error %lu): not installed", GetLastError());
            return;
        }
    }
    s = MH_Initialize();
    for (i = 0; s == MH_OK && i < sizeof(hooks) / sizeof(hooks[0]); i++)
        if ((s = MH_CreateHook((void *)VA(hooks[i].va), hooks[i].detour, hooks[i].orig)) != MH_OK)
            say("could not hook %s (%d)", hooks[i].name, (int)s);
    if (s == MH_OK) {
        if (true_colour || esc_menu) present_ok = present_install(base, true_colour ? compose : NULL, say);
        tc_ok = true_colour && present_ok;
        if ((esc_menu || present_ok) && !escmenu_base(base, say)) esc_menu = 0;   /* (the game's end hooked) */
        strcpy(dir, exe);
        if ((slash = strrchr(dir, '\\')) != NULL) *slash = 0;
        if (colour_maps && !colourmap_install(base, dir, map_theme, say)) colour_maps = 0;
        if (colour_maps) {                  /* (another module's CreateFile hook we chain onto: kept loaded) */
            const void *a, *w;
            colourmap_originals(&a, &w);
            pin_owner(a, "the full-colour maps' CreateFileA");
            pin_owner(w, "the full-colour maps' CreateFileW");
        }
        {                                   /* (the team tasks: only on WA 3.8.1's own code, or another module's hook) */
            static const unsigned char team_bytes[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x64, 0xa1, 0x00, 0x00, 0x00, 0x00};
            static const unsigned char physics_bytes[] = {0x83, 0xec, 0x08, 0x53, 0x8b, 0x5c, 0x24, 0x14};
            const uint8_t *tp = (const uint8_t *)VA(TEAM_HANDLER_VA), *pp = (const uint8_t *)VA(PHYSICS_VA);
            trails = (team_trail *)calloc(TRAIL_TEAMS * TRAIL_PER_TEAM, sizeof(team_trail));
            if (!trails || (memcmp(tp, team_bytes, sizeof(team_bytes)) && *tp != 0xe9) ||
                (memcmp(pp, physics_bytes, sizeof(physics_bytes)) && *pp != 0xe9) ||
                MH_CreateHook((void *)tp, (void *)thunk_team, &orig_team) != MH_OK) {
                trail_on = 0;
                say("the movement line and the auto zoom are unavailable");
            } else if (MH_CreateHook((void *)pp, (void *)thunk_physics, &orig_physics) != MH_OK) {
                MH_RemoveHook((void *)tp);
                trail_on = 0;
                say("the movement line and the auto zoom are unavailable");
            } else {
                worms_ok = 1;
                /* (another module's hook already there: ours chains onto it, so it is kept loaded) */
                if (*tp == 0xe9) pin_owner(tp + 5 + *(const int32_t *)(tp + 1), "the team tasks' hook");
                if (*pp == 0xe9) pin_owner(pp + 5 + *(const int32_t *)(pp + 1), "the physics step's hook");
            }
        }
        if (esc_menu && !escmenu_install(base, say, !test_mode)) esc_menu = 0;
        if (esc_menu) escmenu_set_options(&menu_options);
        if (test_checks) present_test_overlay(150, write_bmp);
        if (present_ok) present_on_gone(escmenu_recover, escmenu_device_back);   /* (no device: one made between frames) */
        s = MH_EnableHook(MH_ALL_HOOKS);
        if (s == MH_OK && follow_sites_install() < 2) say("the zoomed-in camera follow works at some of WA's calls only");
    }
    installed = s == MH_OK;
    if (installed) {
        msg_hook = SetWindowsHookExA(WH_GETMESSAGE, on_message, self, GetCurrentThreadId());
        if (!msg_hook) say("could not watch the mouse wheel (error %lu)", GetLastError());
    }
    say("wkMapZoom 4.3 beta 2 %s (base %08x): Ctrl + wheel zooms the map (up to %.0f%% out, %.0f%% in), default zoom %.0f%% (Ctrl + "
        "middle click%s); true colour %s, full-colour maps %s, Esc menu resolution %s, movement line %s, auto zoom %s",
        installed ? "ready" : "failed", (unsigned)base, 100.0 / cfg.max_out, cfg.max_in * 100.0, zoom_default * 100.0,
        numpad_keys ? ", numpad + / - / *" : "", tc_ok ? "on (Direct3D 9)" : "off", colour_maps ? "on" : "off",
        esc_menu ? "on" : "off", trail_on ? (trail_all ? "on (whole game)" : "on (last turn)") : "off",
        worms_ok ? (auto_zoom ? "on (Ctrl + A switches it)" : "off (Ctrl + A switches it)") : "unavailable");
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
