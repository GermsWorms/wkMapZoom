/* gpu.c: wkMapZoom's drawing on the graphics card (until wkMapZoom 6 this was a module of its own, wkDirect3D).
 * The GPU draws Worms Armageddon 3.8.1's in-game picture, zoomed or not. The land, the sky and the back
 * layers (everything WA draws through its "surface blit", 005A2790), WA's rectangle fills and most sprites are drawn by
 * the GPU inside WA's own Direct3D 9 device; everything else (lines, pixels, a few kinds of sprite) is still drawn by
 * WA on the CPU and laid over the GPU's picture in the order WA drew it. The picture is WA's, pixel for pixel.
 *
 * How. WA draws a frame through its display object's methods (vtable 0066A218, slots 11..22, one per kind of thing);
 * those end in the software RenderContext (vtable 00662EC8): slot 23 the surface blit, slot 19 the rectangle fill, the
 * sprite blit 004F6910, and for everything else plain writes into the 8-bit frame ([007A0EEC]). The module
 *   - takes slot 23, slot 19 and the display's four calls of the sprite blit: instead of copying bytes on the CPU it
 *     notes a textured (or plain) rectangle. Sprites it cannot draw exactly (turned, flipped top to bottom, or with
 *     colours that depend on what is under them) are left to WA; for the last kind the picture under the sprite is
 *     fetched back from the GPU first;
 *   - gives WA a frame of its own to draw the rest into: memory with a write watch, kept full of one "nothing here"
 *     index. Whenever the GPU is about to draw over something WA drew meanwhile, and at the frame's end, the pages WA
 *     wrote are uploaded and drawn as a layer (pixels with the "nothing" index left out), so the order stays WA's;
 *   - draws into a render target that holds palette indices, and at the frame's end puts that through WA's palette
 *     onto the back buffer, as WA's own Direct3D 9 shader renderer does with its frame. WA then presents as ever.
 * Nothing is changed when WA loads the module. When the first game is on the screen, entries of WA's function tables
 * are swapped and the target of the four sprite calls is changed (the only code bytes written), each still calling
 * what was there before. So modules that check WA's code or tables at load see WA's own.
 * Frame by frame the module stands down (WA's own drawing and show) when it is not given the map to draw: another
 * module drew it, Export Video reads the frames, another renderer, no device.
 *
 * Nothing of the game's state is read for writing or changed: only what is shown. Its log lines go into wkMapZoom's log
 * ("gpu: ..."), or into the file WKD3D_LOG names. Off switch: wkMapZoom.ini, GraphicsCard=0 (then there is no zoom).
 * wkMapZoom.c starts it (gpu_start) and calls the map functions below directly.
 *
 * The functions near the end of this file (exported by wkMapZoom.dll under the names they had in wkDirect3D.dll, for
 * wkSpeedometer and wkModMenu): the zoom has WA draw the map
 * bigger or smaller than the screen into a frame handed out here (wkDirect3D_MapBegin); that map is drawn as an ordinary
 * frame is, into a target of its own, and put on the screen at the zoom, each screen pixel the average of the map
 * pixels it covers (wkDirect3D_MapEnd). True-colour pictures and triangles can be laid over the finished frame
 * (wkDirect3D_Overlay, wkDirect3D_Triangles: wkSpeedometer's dial and line).
 *
 * Test switches (environment; for isolated test copies of WA): WKD3D_LOG=file, WKD3D_OFF=1 (hooks in, never drawing),
 * WKD3D_STATS=1 (a line a second), WKD3D_TRACE=n (n frames' drawing calls logged, from frame 400), WKD3D_SENT=n (the
 * "nothing here" index), WKD3D_TEST_ZOOM=0.35 (land, sky, back layers and fills drawn zoomed out about the screen's
 * centre, each screen pixel averaged in true colour; sprites stay 1:1: a speed test, not a finished picture),
 * WKD3D_HIST=1 (which palette indices WA's own frames use), WKD3D_OV=1 (layers through one small dynamic texture
 * instead of rectangle updates), WKD3D_RESET=n (the write watch reset every n frames), WKD3D_TEST_NOSPRITES=1 (the
 * map's sprites that WA would draw on the CPU are not drawn at all; 2: a small placeholder rectangle is drawn by the GPU
 * for each: what the leftover layer costs, and roughly what sprites on the GPU would), WKD3D_CHECK=n (each of the
 * display's drawing calls is made a second time with the module out of the way, into a second frame: WA's own picture;
 * every n-th frame the GPU's index picture is read back and compared with it, pixel by pixel; differences are logged.
 * WKD3D_STRICT=0: the older, weaker check: only blits, fills and sprites drawn by WA's own code into the second frame,
 * the leftover layers laid over it as the module lays them),
 * WKD3D_TEST_TABLE=n / WKD3D_TEST_KIND=2 or 3 (about every n-th sprite is drawn with WA's adding blend table / as that
 * kind, in WA's own picture too: exercises the path for sprites that depend on what is under them),
 * WKD3D_SABOTAGE=1..6 (the drawing broken on purpose, to see the check notice: 1 every blit one pixel to the right,
 * 2 see-through blits drawn whole, 3 every 97th rectangle left out, 4 the texture coordinates half a pixel off,
 * 5 sprites one texel off, 6 sprites that depend on what is under them drawn without fetching it), WKD3D_TEST_NOTICE=1 (the on-screen notice shown without another module to tell of). */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <tlhelp32.h>
#include <d3d9.h>
#include <emmintrin.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "image_guard.h"
#include "shaders.h"

#define VERSION "0.6 (in wkMapZoom)"
#define WA_BASE 0x400000u
#define VA(a) ((uintptr_t)(base + ((uintptr_t)(a) - WA_BASE)))
#define P32(a) (*(uint32_t *)(uintptr_t)(a))
#define PI32(a) (*(int32_t *)(uintptr_t)(a))

/* WA 3.8.1 (addresses for image base 00400000) */
#define SESSION_VA 0x7a0884u            /* the game session (0 outside a game): +0xAC the display */
#define RC_PTR_VA 0x79d6d4u             /* the RenderContext: +0x18 the renderer's backend */
#define RC_VT_VA 0x662ec8u              /* the software RenderContext's vtable */
#define DISP_VT_VA 0x66a218u            /* the game display's vtable */
#define BE_VT_VA 0x6767f8u              /* the Direct3D 9 shader-palette backend's vtable */
#define SURF_VT_VA 0x677570u            /* a RenderContext surface: +4 width, +8 height, +0xC pixels, +0x10 colour key */
#define FRAME_PTR_VA 0x7a0eecu          /* the 8-bit frame: pixels, width (= row stride), height */
#define FRAME_W_VA 0x7a0ef0u
#define FRAME_H_VA 0x7a0ef4u
#define RC_OK_VA 0x8accd4u              /* a RenderContext call's "fine" result */
#define BE_OK_VA 0x8ac8b4u              /* a backend call's "fine" result */
#define PAL_VA 0x8ac8c8u                /* the palette as shown: 256 x R, G, B, flags */
#define PAL_GEN_VA 0x8accd0u            /* counts its changes */
#define D_W 0x3548                      /* the display: its size, clip (x1, y1, x2, y2), origin, frame grid */
#define D_LOCKED 0x3d98
#define D_GRID 0x3d9c
#define D_SPRITES 0x1008                /* its sprites by id; +0x2008 too; +0x3DD4 those kept as surfaces */
#define D_SPRITES2 0x2008
#define D_SURF_SPRITES 0x3dd4
#define D_SINKS 0x24dfc                 /* its frame sinks (Export Video): a list's first and end */
#define GR_DATA 0x08                    /* a BitGrid: pixels, bits a pixel, row stride, size, clip (x1, y1, x2, y2) */
#define GR_BPP 0x0c
#define GR_STRIDE 0x10
#define GR_W 0x14
#define GR_H 0x18
#define GR_CLIP 0x1c
#define SPRITE_BLIT_VA 0x4f6910u        /* BitGrid sprite blit (ESI target; x, y, w, h, source, sx, sy, blend table, mode; ret 0x24) */
#define BE_W 0x1c                       /* the backend: its size, "lost" flag, device */
#define BE_H 0x20
#define BE_LOST 0x34
#define BE_DEV 0x40
#define QUEUE_FIRST_VA 0x5423acu        /* RenderDrawingQueue's code for the kinds (the callers of the display's methods) */
#define QUEUE_LAST_VA 0x542ac6u
#define Q_SPRITE4_RET 0x542493u         /* ... the return address of kind 4's sprite call (placed on the screen) */

/* a vertex: its place on the screen, what its rectangle is (colour: see the shaders, tools\mkshaders.c), its place in the
 * texture, and the part of the texture its picture lies in */
#define FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX2 | D3DFVF_TEXCOORDSIZE2(0) | D3DFVF_TEXCOORDSIZE4(1))
typedef struct { float x, y, z, rhw; DWORD col; float u, v, l0, l1, l2, l3; } vert;

static uintptr_t base;
static HINSTANCE self;
static HANDLE log_file = INVALID_HANDLE_VALUE;
static LARGE_INTEGER qpf;

/* ---- the log ---- */
void mz_gpu_log(const char *line);        /* (wkMapZoom.c: a line into its log) */
static void open_log(void) {
    char p[MAX_PATH + 32];
    DWORD high = 0, size, n = GetEnvironmentVariableA("WKD3D_LOG", p, MAX_PATH);
    if (!n || n >= MAX_PATH) return;        /* (no file of its own: the lines go into wkMapZoom's log) */
    log_file = CreateFileA(p, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (log_file == INVALID_HANDLE_VALUE) return;
    size = GetFileSize(log_file, &high);
    if (high || size > 2u * 1024u * 1024u) SetEndOfFile(log_file);
    else SetFilePointer(log_file, 0, NULL, FILE_END);
}

static void say(const char *fmt, ...) {
    char b[1024];
    SYSTEMTIME t;
    va_list v;
    DWORD n;
    int k, m;
    if (log_file == INVALID_HANDLE_VALUE) {  /* wkMapZoom's log */
        va_start(v, fmt);
        m = _vsnprintf(b, sizeof(b) - 1, fmt, v);
        va_end(v);
        b[m < 0 ? (int)sizeof(b) - 1 : m] = 0;
        mz_gpu_log(b);
        return;
    }
    GetLocalTime(&t);
    k = sprintf(b, "%04u-%02u-%02u %02u:%02u:%02u.%03u [%lu] ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                t.wMilliseconds, (unsigned long)GetCurrentProcessId());
    va_start(v, fmt);
    m = _vsnprintf(b + k, sizeof(b) - k - 3, fmt, v);
    va_end(v);
    k = m < 0 ? (int)sizeof(b) - 3 : k + m;
    b[k++] = '\r';
    b[k++] = '\n';
    WriteFile(log_file, b, (DWORD)k, &n, NULL);
}

static LONGLONG now(void) { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }

/* ---- options (stage 1: the environment) ---- */
static int opt_off, opt_stats, opt_trace, opt_hist, opt_ov, opt_reset = 16, opt_nosprites, opt_check, opt_strict = 1, opt_sabotage, test_table, test_kind, sent = 0xfd;
static double test_zoom = 1.0;
static int env_int(const char *name, int dflt) {
    char b[32];
    DWORD n = GetEnvironmentVariableA(name, b, sizeof(b));
    return n && n < sizeof(b) ? atoi(b) : dflt;
}

/* ---- state ---- */
enum { PASSIVE, ACTIVE };
static int installed, mode, failed;     /* failed: something of Direct3D's could not be had: passive for good */
static int user_off;                    /* switched off in the game (wkMapZoom.ini, GraphicsCard=0, read again): passive until switched on */
static unsigned frames, gpu_frames;     /* shown frames; those the GPU drew */
static int frame_ops;                   /* GPU operations noted this frame */
static int frame_abort;                 /* this frame cannot be finished on the GPU */
static int cpu_maybe;                   /* WA may have drawn into the frame since the last layer was taken */
static unsigned ops_serial;             /* counts GPU operations */
static unsigned cpu_touch;              /* counts drawing the module saw and left to WA (to see whether a display call did any) */
static int passive_blits;               /* blits that would have been the GPU's, this passive frame */
static int cooldown, cooldown_next = 2; /* passive frames before the next try */
static unsigned frame_layer_px;         /* pixels of leftover layers in this frame */
static int heavy, light, heavy_wait = 64, heavy_told;   /* frames in a row that came almost whole as leftover drawing / that did not */
static uintptr_t last_wa_frame;         /* WA's own frame as last shown passively */
static int zoom_clear = 1;                /* zoom test: the back buffer is cleared before the frame's first rectangle */
static int frame_started;               /* a display method has been called since the last frame was shown */
static unsigned read_seen;              /* the last frame something wanted to read (0: never): passive for a while after */
static int tracing;
static unsigned trace_left;

/* statistics (a second's worth) */
static struct { unsigned frames, gpu, ops, draws, flushes, pages, runs, ov_bytes, tex_bytes, tex_n, foreign; LONGLONG t_flush, t_show, t_ww, t_scan, t_copy, t_up, t_draw; } st;
static LONGLONG st_since;

/* ---- the frame WA draws the rest into ---- */
static uint8_t *my_frame;
static int my_w, my_h, my_dirty;        /* my_dirty: it may hold more than the "nothing" index */
static size_t my_size;
static uintptr_t wa_frame;              /* WA's own, while mine is in its place */
static void **ww_addr;
static ULONG_PTR ww_cap;
static int ww_age;                      /* frames since the write watch was last reset */
static uint8_t *ref;                    /* WKD3D_CHECK: the frame as WA's own code draws it */
static int in_ref;                      /* ... and WA is drawing into it now (the module keeps out of the way) */
static struct { unsigned frames, bad_frames; unsigned long long pixels, bad; } chk;
static unsigned chk_maps;              /* ... of them zoomed frames' maps */
static struct { uintptr_t surf; int x, y, w, h, sx, sy, flags, sw, sh, fresh; } chk_blit[4096];   /* this frame's blits and fills (surf 0) */
static int chk_n;

/* ---- Direct3D objects (WA's device; all released before WA resets or closes it) ---- */
static IDirect3DDevice9 *obj_dev;
static IDirect3DVertexBuffer9 *vb;
static IDirect3DTexture9 *pal_tex, *ov_sys, *ov_tex, *ov_dyn, *rt_tex, *stage_tex;
static IDirect3DSurface9 *ov_sys_lvl, *ov_lvl, *rt_lvl, *stage_lvl, *chk_surf, *back_rt, *back_sys;
#define BACK 512                        /* the size of the pieces the index target is fetched back in */
static IDirect3DPixelShader9 *sh_idx, *sh_final, *sh_rgb, *sh_avg[9], *sh_colour;
static IDirect3DStateBlock9 *wa_state;  /* the device's state as WA left it */
static IDirect3DTexture9 *ghost_tex;    /* a 1 x 1 frame texture and a vertex buffer of one point: WA's own show-frame */
static IDirect3DVertexBuffer9 *ghost_vb; /* step run for the sake of other modules' hooks on it, drawing nothing */
static UINT ghost_stride;
static int show_chain;                  /* another module's work on WA's show-frame is kept running (wkMapZoom's) */
static IDirect3DTexture9 *note_tex;     /* a notice for the player: a line or two of text over the picture for some seconds */
static IDirect3DPixelShader9 *sh_plain;
static char note_text[240];
static DWORD note_until;
static int note_drawn;                  /* the texture holds note_text */
#define NOTE_W 640
#define NOTE_H 44
static int stage_w, stage_h, obj_w, obj_h, ov_dyn_h, states_mine;

/* ---- the zoomed map (a zoom module asks for it through the exported functions near the end of this file) ----
 * While WA draws the map part of a zoomed frame, everything above that describes "the frame" (the write-watched memory,
 * the index target, the layer textures, the check's second frame) is the map's: as big as the largest zoomed-out view,
 * and drawn 1:1 as ever. target_swap() exchanges the two sets. Then the map is put on the screen at the zoom, every
 * screen pixel the average of the map pixels it covers, into a true-colour target of the screen's size; what WA draws
 * after it (the HUD) goes straight into that target too, and it is what is shown. */
typedef struct {
    uint8_t *frame, *ref;
    int w, h, dirty, ww_age;
    size_t size;
    void **ww_addr;
    ULONG_PTR ww_cap;
    IDirect3DTexture9 *ov_sys, *ov_tex, *rt_tex;
    IDirect3DSurface9 *ov_sys_lvl, *ov_lvl, *rt_lvl, *chk_surf;
} target;
static target zmap;                     /* the map's set (while zm_on: the screen's, and the map's is in use) */
static int zm_on;                       /* WA is drawing a zoomed frame's map part */
static int zframe;                      /* this frame is a zoomed one: its screen part goes into zs in true colour */
static int rt_used;                     /* something was drawn into the screen's index target this frame */
static int zm_rw, zm_rh;                /* the part of the map target in use this frame */
static int zm_deny_w, zm_deny_h;        /* a map target of this size could not be made */
static double zm_zoom;                  /* (for the log) */
static int zm_taps;
static IDirect3DTexture9 *zs_tex;       /* a zoomed frame's picture: true colour, the screen's size */
static IDirect3DSurface9 *zs_lvl, *zs_sys;
static int zs_shown;                    /* the last frame shown was a zoomed one (a screenshot is taken from zs) */
/* true-colour pictures and triangles other modules ask to have laid over the frame (the exported functions) */
#define OVL_W 2048
#define OVL_H 2048
#define OVL_MAX 32
#define TRI_MAX 190000
typedef struct { float x, y, z, rhw; DWORD col; } tvert;
static uint32_t *ovl_px;                /* this frame's pictures, side by side */
static struct { int x, y, w, h, ax, ay; } ovl[OVL_MAX];
static int n_ovl, ovl_x, ovl_y, ovl_shelf;
static IDirect3DTexture9 *ovl_tex;
static tvert *tri;
static int n_tri, tri_cap;

#define SWAP(T, a, b) do { T t_ = (a); (a) = (b); (b) = t_; } while (0)
static void target_swap(void) {
    SWAP(uint8_t *, my_frame, zmap.frame); SWAP(uint8_t *, ref, zmap.ref);
    SWAP(int, my_w, zmap.w); SWAP(int, my_h, zmap.h); SWAP(int, my_dirty, zmap.dirty); SWAP(int, ww_age, zmap.ww_age);
    SWAP(size_t, my_size, zmap.size); SWAP(void **, ww_addr, zmap.ww_addr); SWAP(ULONG_PTR, ww_cap, zmap.ww_cap);
    SWAP(IDirect3DTexture9 *, ov_sys, zmap.ov_sys); SWAP(IDirect3DTexture9 *, ov_tex, zmap.ov_tex);
    SWAP(IDirect3DTexture9 *, rt_tex, zmap.rt_tex);
    SWAP(IDirect3DSurface9 *, ov_sys_lvl, zmap.ov_sys_lvl); SWAP(IDirect3DSurface9 *, ov_lvl, zmap.ov_lvl);
    SWAP(IDirect3DSurface9 *, rt_lvl, zmap.rt_lvl); SWAP(IDirect3DSurface9 *, chk_surf, zmap.chk_surf);
    zm_on = !zm_on;
}
static uint32_t pal_gen_shown = 0xffffffffu;
#define VB_QUADS 4096
/* a noted rectangle: where on the screen, what (the vertex colour), from where in which texture (NULL: none needed) */
typedef struct { IDirect3DTexture9 *tex; DWORD col; uint8_t zoom; float x0, y0, x1, y1, u0, v0, u1, v1, l0, l1, l2, l3; } gop;
static gop ops[VB_QUADS];
static int n_ops;
#define COL_KEYED(key) (0xff000000u | (DWORD)(key) << 16)       /* a picture, index `key` see-through */
#define COL_OPAQUE 0u                                            /* a picture, whole */
#define COL_FILL(colour) (0x000000ffu | (DWORD)(colour) << 8)    /* one index all over */

/* The surfaces' pictures live side by side in a few big textures ("pages"), so that a frame's rectangles are drawn in a
 * few calls; a surface too big for that has a texture of its own */
#define MAX_PAGES 48
typedef struct { IDirect3DTexture9 *tex; IDirect3DSurface9 *lvl; } apage;
typedef struct { int page, y, h, x; } ashelf;
static apage pages[MAX_PAGES];
static ashelf shelves[1024];
static int n_pages, n_shelves, page_size = 4096;
typedef struct { int page, x, y, w, h; } arect;
static arect free_rects[2048];
static int n_free;

/* The sprites WA draws this frame: their pixels copied side by side into a system-memory picture ("strip") and from
 * there into a texture just before the rectangles that use them are drawn; then the strip is filled from its start
 * again. Nothing is kept from frame to frame: WA's sprite cache gives no lasting names to its pictures. */
#define SP_W 2048
#define SP_H 2048
static IDirect3DTexture9 *sp_sys, *sp_tex;
static IDirect3DSurface9 *sp_sys_lvl, *sp_lvl;
static uint8_t *sp_bits;                /* the strip's pixels while it is locked */
static int sp_pitch, sp_x, sp_y, sp_shelf, sp_d0 = SP_H, sp_d1;   /* where the next goes; rows written since the last upload */
static struct { unsigned gpu, left, pixels; unsigned combo[4][16][2]; } sp_stat;   /* (left to WA: by kind, orientation, with a table) */

#define TC_SIZE 8192
#define TC_GONE ((uintptr_t)1)
typedef struct { uintptr_t surf, pix; int w, h, dirty, page, ax, ay; IDirect3DTexture9 *tex; IDirect3DSurface9 *lvl; unsigned written, uploaded; } stex;
static stex tc[TC_SIZE];
static unsigned tc_used;
static size_t tex_mem;

static unsigned tc_hash(uintptr_t s) { return (unsigned)((s >> 2) * 2654435761u) >> 19; }   /* 13 bits */
static stex *tc_find(uintptr_t s) {
    unsigned i = tc_hash(s), n;
    for (n = 0; n < TC_SIZE; n++, i = (i + 1) & (TC_SIZE - 1)) {
        if (tc[i].surf == s) return &tc[i];
        if (!tc[i].surf) return NULL;
    }
    return NULL;
}
static void tc_free(stex *e) {            /* its place in a page can be had again; a texture of its own is let go */
    if (e->page >= 0 && e->tex && n_free < 2048) { arect r = {e->page, e->ax, e->ay, e->w, e->h}; free_rects[n_free++] = r; }
    if (e->page < 0) {
        if (e->lvl) IDirect3DSurface9_Release(e->lvl);
        if (e->tex) { IDirect3DTexture9_Release(e->tex); tex_mem -= (size_t)e->w * e->h; }
    }
    e->lvl = NULL; e->tex = NULL; e->page = -1;
}
static stex *tc_add(uintptr_t s) {
    unsigned i = tc_hash(s), n;
    stex *gone = NULL;
    for (n = 0; n < TC_SIZE; n++, i = (i + 1) & (TC_SIZE - 1)) {
        if (tc[i].surf == TC_GONE) { if (!gone) gone = &tc[i]; continue; }
        if (!tc[i].surf) break;
    }
    if (!gone) { if (n == TC_SIZE || tc_used > TC_SIZE * 3 / 4) return NULL; gone = &tc[i]; tc_used++; }
    memset(gone, 0, sizeof(*gone));
    gone->surf = s;
    gone->page = -1;
    return gone;
}
static void tc_release_all(void) {
    unsigned i;
    for (i = 0; i < TC_SIZE; i++) if (tc[i].surf > TC_GONE) tc_free(&tc[i]);
    memset(tc, 0, sizeof(tc));
    for (i = 0; i < (unsigned)n_pages; i++) {
        if (pages[i].lvl) IDirect3DSurface9_Release(pages[i].lvl);
        if (pages[i].tex) IDirect3DTexture9_Release(pages[i].tex);
    }
    memset(pages, 0, sizeof(pages));
    n_pages = n_shelves = n_free = 0;
    tc_used = 0;
    tex_mem = 0;
}

static void go_passive(const char *why);

/* ---- WA's objects ---- */
static uintptr_t wa_display(void) {
    const uintptr_t s = P32(VA(SESSION_VA));
    const uintptr_t d = s ? P32(s + 0xac) : 0;
    return d && P32(d) == VA(DISP_VT_VA) ? d : 0;
}
static uintptr_t wa_backend(void) {
    const uintptr_t rc = P32(VA(RC_PTR_VA));
    const uintptr_t be = rc && P32(rc) == VA(RC_VT_VA) ? P32(rc + 0x18) : 0;
    return be && P32(be) == VA(BE_VT_VA) ? be : 0;
}

/* something reads WA's frame after it is drawn (Export Video): such frames are WA's own */
static int frame_is_read(uintptr_t d) {   /* (as WA asks them itself, 0056CDB0: each sink's method 1, "do you want this frame?") */
    typedef int (__attribute__((thiscall)) *want_fn)(void *);
    uintptr_t p, end;
    if (!d) return 0;
    end = P32(d + D_SINKS + 4);
    for (p = P32(d + D_SINKS); p && p < end && end - p < 4096; p += 4) {
        void *sink = (void *)(uintptr_t)P32(p);
        if (sink && ((want_fn)(*(void ***)sink)[1])(sink)) return 1;
    }
    return 0;
}

/* ---- Direct3D ---- */
#define REL(o) do { if (o) { IUnknown_Release((IUnknown *)(o)); (o) = NULL; } } while (0)

static void restore_states(void) {
    if (states_mine && wa_state && obj_dev) IDirect3DStateBlock9_Apply(wa_state);
    states_mine = 0;
}

/* the map's Direct3D objects (its memory is kept) */
static void map_release(void) {
    if (zm_on) target_swap();
    REL(zmap.ov_sys_lvl); REL(zmap.ov_lvl); REL(zmap.rt_lvl); REL(zmap.chk_surf);
    REL(zmap.ov_sys); REL(zmap.ov_tex); REL(zmap.rt_tex);
    REL(zs_lvl); REL(zs_sys); REL(zs_tex);
    zframe = 0; zs_shown = 0;
}

static void release_objects(const char *why) {
    if (!obj_dev) return;
    restore_states();
    map_release();
    REL(ovl_tex); REL(sh_colour);
    n_ovl = n_tri = 0;
    tc_release_all();
    REL(wa_state); REL(vb); REL(pal_tex); REL(ghost_tex); REL(ghost_vb); REL(note_tex); REL(sh_plain);
    note_drawn = 0;
    ghost_stride = 0;
    REL(ov_sys_lvl); REL(ov_lvl); REL(rt_lvl); REL(stage_lvl); REL(chk_surf); REL(back_rt); REL(back_sys);
    REL(ov_sys); REL(ov_tex); REL(ov_dyn); REL(rt_tex); REL(stage_tex);
    if (sp_bits && sp_sys) IDirect3DTexture9_UnlockRect(sp_sys, 0);
    sp_bits = NULL; sp_x = sp_y = sp_shelf = 0; sp_d0 = SP_H; sp_d1 = 0;
    REL(sp_sys_lvl); REL(sp_lvl); REL(sp_sys); REL(sp_tex);
    REL(sh_idx); REL(sh_final); REL(sh_rgb);
    REL(sh_avg[2]); REL(sh_avg[3]); REL(sh_avg[4]); REL(sh_avg[6]); REL(sh_avg[8]);
    stage_w = stage_h = 0;
    obj_dev = NULL;
    pal_gen_shown = 0xffffffffu;
    say("Direct3D objects released (%s)", why);
}

static int make_stage(IDirect3DDevice9 *dev, int w, int h) {
    if (stage_tex && stage_w >= w && stage_h >= h) return 1;
    if (w < stage_w) w = stage_w;
    if (h < stage_h) h = stage_h;
    REL(stage_lvl); REL(stage_tex);
    stage_w = stage_h = 0;
    if (FAILED(IDirect3DDevice9_CreateTexture(dev, w, h, 1, 0, D3DFMT_L8, D3DPOOL_SYSTEMMEM, &stage_tex, NULL))) return 0;
    IDirect3DTexture9_GetSurfaceLevel(stage_tex, 0, &stage_lvl);
    stage_w = w; stage_h = h;
    return 1;
}

static int make_objects(IDirect3DDevice9 *dev, int w, int h) {
    D3DCAPS9 caps;
    HRESULT hr = 0;
    const char *what = "";
    if (obj_dev == dev && obj_w == w && obj_h == h) return 1;
    release_objects("device or size changed");
    memset(&caps, 0, sizeof(caps));
    IDirect3DDevice9_GetDeviceCaps(dev, &caps);
    if ((caps.PixelShaderVersion & 0xffff) < 0x0200 || ((caps.TextureCaps & D3DPTEXTURECAPS_POW2) && !(caps.TextureCaps & D3DPTEXTURECAPS_NONPOW2CONDITIONAL))) {
        say("this graphics device can't do what is needed (pixel shader %08lx, texture caps %08lx)", caps.PixelShaderVersion, caps.TextureCaps);
        return 0;
    }
    obj_dev = dev; obj_w = w; obj_h = h;
    ov_dyn_h = h < 512 ? h : 512;
    page_size = caps.MaxTextureWidth >= 4096 && caps.MaxTextureHeight >= 4096 ? 4096 : 2048;
#define TRY(x, name) if (!FAILED(hr)) { hr = (x); if (FAILED(hr)) what = name; }
    TRY(IDirect3DDevice9_CreateStateBlock(dev, D3DSBT_ALL, &wa_state), "state block");
    TRY(IDirect3DDevice9_CreateVertexBuffer(dev, VB_QUADS * 6 * sizeof(vert), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, FVF, D3DPOOL_DEFAULT, &vb, NULL), "vertex buffer");
    TRY(IDirect3DDevice9_CreateTexture(dev, 256, 1, 1, D3DUSAGE_DYNAMIC, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &pal_tex, NULL), "palette texture");
    TRY(IDirect3DDevice9_CreateTexture(dev, w, h, 1, 0, D3DFMT_L8, D3DPOOL_SYSTEMMEM, &ov_sys, NULL), "layer staging");
    TRY(IDirect3DDevice9_CreateTexture(dev, w, h, 1, 0, D3DFMT_L8, D3DPOOL_DEFAULT, &ov_tex, NULL), "layer texture");
    TRY(IDirect3DDevice9_CreateTexture(dev, w, ov_dyn_h, 1, D3DUSAGE_DYNAMIC, D3DFMT_L8, D3DPOOL_DEFAULT, &ov_dyn, NULL), "layer strip");
    TRY(IDirect3DDevice9_CreateTexture(dev, w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &rt_tex, NULL), "index target");
    TRY(IDirect3DDevice9_CreateTexture(dev, SP_W, SP_H, 1, 0, D3DFMT_L8, D3DPOOL_SYSTEMMEM, &sp_sys, NULL), "sprite staging");
    TRY(IDirect3DDevice9_CreateTexture(dev, SP_W, SP_H, 1, 0, D3DFMT_L8, D3DPOOL_DEFAULT, &sp_tex, NULL), "sprite texture");
    TRY(IDirect3DDevice9_CreatePixelShader(dev, ps_idx_any, &sh_idx), "shader");
    TRY(IDirect3DDevice9_CreatePixelShader(dev, ps_final, &sh_final), "shader");
    TRY(IDirect3DDevice9_CreatePixelShader(dev, ps_rgb_any, &sh_rgb), "shader");
    TRY(IDirect3DDevice9_CreatePixelShader(dev, ps_plain, &sh_plain), "shader");
    TRY(IDirect3DDevice9_CreatePixelShader(dev, ps_colour, &sh_colour), "shader");
#undef TRY
    if (FAILED(hr)) {
        say("Direct3D: no %s (%08lx): WA draws by itself", what, (unsigned long)hr);
        release_objects("could not be made");
        return 0;
    }
    if ((caps.PixelShaderVersion & 0xffff) >= 0x0300) {
        IDirect3DDevice9_CreatePixelShader(dev, ps_avg2, &sh_avg[2]);
        IDirect3DDevice9_CreatePixelShader(dev, ps_avg3, &sh_avg[3]);
        IDirect3DDevice9_CreatePixelShader(dev, ps_avg4, &sh_avg[4]);
        if (FAILED(IDirect3DDevice9_CreatePixelShader(dev, ps_avg6, &sh_avg[6]))) sh_avg[6] = NULL;
        if (FAILED(IDirect3DDevice9_CreatePixelShader(dev, ps_avg8, &sh_avg[8]))) sh_avg[8] = NULL;
    }
    IDirect3DTexture9_GetSurfaceLevel(ov_sys, 0, &ov_sys_lvl);
    IDirect3DTexture9_GetSurfaceLevel(ov_tex, 0, &ov_lvl);
    IDirect3DTexture9_GetSurfaceLevel(rt_tex, 0, &rt_lvl);
    IDirect3DTexture9_GetSurfaceLevel(sp_sys, 0, &sp_sys_lvl);
    IDirect3DTexture9_GetSurfaceLevel(sp_tex, 0, &sp_lvl);
    IDirect3DDevice9_ColorFill(dev, rt_lvl, NULL, 0);
    IDirect3DDevice9_CreateRenderTarget(dev, BACK, BACK, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &back_rt, NULL);
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, BACK, BACK, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &back_sys, NULL);
    if (opt_check) IDirect3DDevice9_CreateOffscreenPlainSurface(dev, w, h, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &chk_surf, NULL);
    say("Direct3D objects made for %dx%d (pixel shader %lu.%lu%s, texture pages of %d)", w, h, (caps.PixelShaderVersion >> 8) & 0xff,
        caps.PixelShaderVersion & 0xff, sh_avg[4] ? ", zoom shaders" : "", page_size);
    {   /* (what the screen runs at while WA has it: a fullscreen game need not use the desktop's rate) */
        D3DDISPLAYMODE dm;
        memset(&dm, 0, sizeof(dm));
        IDirect3DSwapChain9 *sc = NULL;
        D3DPRESENT_PARAMETERS pp;
        memset(&pp, 0, sizeof(pp));
        if (!FAILED(IDirect3DDevice9_GetSwapChain(dev, 0, &sc)) && sc) {
            IDirect3DSwapChain9_GetPresentParameters(sc, &pp);
            IDirect3DSwapChain9_Release(sc);
        }
        if (!FAILED(IDirect3DDevice9_GetDisplayMode(dev, 0, &dm)))
            say("the display: %ux%u at %u Hz, %s, vsync %s (present interval %08lx, asked refresh rate %u, %u back buffer(s), swap effect %d)",
                dm.Width, dm.Height, dm.RefreshRate, pp.Windowed ? "in a window" : "fullscreen",
                pp.PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE ? "off" : "on", (unsigned long)pp.PresentationInterval,
                pp.FullScreen_RefreshRateInHz, pp.BackBufferCount, (int)pp.SwapEffect);
    }
    return 1;
}

/* room for a w x h picture in a page: one let go before, else on a shelf of about its height, else a new shelf or page */
static int page_room(IDirect3DDevice9 *dev, int w, int h, stex *e) {
    int i;
    for (i = 0; i < n_free; i++) if (free_rects[i].w == w && free_rects[i].h == h) {
        e->page = free_rects[i].page; e->ax = free_rects[i].x; e->ay = free_rects[i].y;
        free_rects[i] = free_rects[--n_free];
        return 1;
    }
    for (i = 0; i < n_shelves; i++) if (shelves[i].h >= h && shelves[i].h * 3 <= h * 4 && shelves[i].x + w <= page_size) break;
    if (i == n_shelves) {
        int pg, y = 0, k;
        if (n_shelves == 1024) return 0;
        for (pg = 0; pg < n_pages; pg++) {  /* (the shelves of a page lie one under the other) */
            for (y = 0, k = 0; k < n_shelves; k++) if (shelves[k].page == pg && shelves[k].y + shelves[k].h > y) y = shelves[k].y + shelves[k].h;
            if (y + h <= page_size) break;
        }
        if (pg == n_pages) {
            if (n_pages == MAX_PAGES || tex_mem + (size_t)page_size * page_size > (700u << 20)) return 0;
            if (FAILED(IDirect3DDevice9_CreateTexture(dev, page_size, page_size, 1, 0, D3DFMT_L8, D3DPOOL_DEFAULT, &pages[pg].tex, NULL))) { pages[pg].tex = NULL; return 0; }
            IDirect3DTexture9_GetSurfaceLevel(pages[pg].tex, 0, &pages[pg].lvl);
            tex_mem += (size_t)page_size * page_size;
            n_pages++;
            y = 0;
        }
        shelves[i].page = pg; shelves[i].y = y; shelves[i].h = h; shelves[i].x = 0;
        n_shelves++;
    }
    e->page = shelves[i].page; e->ax = shelves[i].x; e->ay = shelves[i].y;
    shelves[i].x += w;
    return 1;
}

/* a surface's picture in a texture, up to date */
static stex *surface_texture(IDirect3DDevice9 *dev, uintptr_t s) {
    stex *e = tc_find(s);
    const int w = PI32(s + 4), h = PI32(s + 8);
    const uintptr_t pix = P32(s + 0xc);
    D3DLOCKED_RECT lr;
    RECT r;
    POINT pt;
    int y;
    if (e && e->tex && !e->dirty && e->w == w && e->h == h && e->pix == pix) return e;
    if (e && (e->w != w || e->h != h || e->pix != pix)) { tc_free(e); e->dirty = 1; }
    if (!e) { e = tc_add(s); if (!e) return NULL; e->dirty = 1; }
    if (!e->tex) {
        if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return NULL;
        e->w = w; e->h = h; e->pix = pix;
        if (w <= page_size / 2 && h <= page_size / 2) {
            if (!page_room(dev, w, h, e)) { e->page = -1; return NULL; }
            e->tex = pages[e->page].tex; e->lvl = pages[e->page].lvl;
        } else {
            if (tex_mem + (size_t)w * h > (700u << 20)) return NULL;
            if (FAILED(IDirect3DDevice9_CreateTexture(dev, w, h, 1, 0, D3DFMT_L8, D3DPOOL_DEFAULT, &e->tex, NULL))) { e->tex = NULL; return NULL; }
            IDirect3DTexture9_GetSurfaceLevel(e->tex, 0, &e->lvl);
            tex_mem += (size_t)w * h;
            e->page = -1; e->ax = e->ay = 0;
        }
        e->dirty = 1;
    }
    if (e->dirty) {
        if (!make_stage(dev, w, h)) return NULL;
        r.left = 0; r.top = 0; r.right = w; r.bottom = h;
        if (FAILED(IDirect3DTexture9_LockRect(stage_tex, 0, &lr, &r, 0))) return NULL;
        for (y = 0; y < h; y++) memcpy((uint8_t *)lr.pBits + (size_t)y * lr.Pitch, (const uint8_t *)pix + (size_t)y * w, w);
        IDirect3DTexture9_UnlockRect(stage_tex, 0);
        pt.x = e->ax; pt.y = e->ay;
        if (FAILED(IDirect3DDevice9_UpdateSurface(dev, stage_lvl, &r, e->lvl, &pt))) return NULL;
        e->dirty = 0;
        e->uploaded = frames;
        st.tex_bytes += (unsigned)(w * h);
        st.tex_n++;
    }
    return e;
}

static void set_states(IDirect3DDevice9 *dev) {
    int i;
    if (states_mine) return;
    if (wa_state) IDirect3DStateBlock9_Capture(wa_state);
    IDirect3DDevice9_SetVertexShader(dev, NULL);
    IDirect3DDevice9_SetFVF(dev, FVF);
    IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(vert));
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, 0);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, 0);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, 0);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHATESTENABLE, 0);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGENABLE, 0);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SPECULARENABLE, 0);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SHADEMODE, D3DSHADE_FLAT);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_STENCILENABLE, 0);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SCISSORTESTENABLE, 0);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0xf);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SRGBWRITEENABLE, 0);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_FILLMODE, D3DFILL_SOLID);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_ONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    for (i = 0; i < 2; i++) {
        IDirect3DDevice9_SetSamplerState(dev, i, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(dev, i, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(dev, i, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        IDirect3DDevice9_SetSamplerState(dev, i, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        IDirect3DDevice9_SetSamplerState(dev, i, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        IDirect3DDevice9_SetSamplerState(dev, i, D3DSAMP_SRGBTEXTURE, 0);
    }
    states_mine = 1;
}

static void update_palette(void) {
    const uint32_t gen = P32(VA(PAL_GEN_VA));
    const uint8_t *p = (const uint8_t *)VA(PAL_VA);
    D3DLOCKED_RECT lr;
    int i;
    if (gen == pal_gen_shown) return;
    if (FAILED(IDirect3DTexture9_LockRect(pal_tex, 0, &lr, NULL, D3DLOCK_DISCARD))) return;
    for (i = 0; i < 256; i++) ((uint32_t *)lr.pBits)[i] = 0xff000000u | (uint32_t)p[4 * i] << 16 | (uint32_t)p[4 * i + 1] << 8 | p[4 * i + 2];
    IDirect3DTexture9_UnlockRect(pal_tex, 0);
    pal_gen_shown = gen;
}

static void put_quad(vert *v, const gop *o) {
    const float x0 = o->x0 - 0.5f, y0 = o->y0 - 0.5f, x1 = o->x1 - 0.5f, y1 = o->y1 - 0.5f;
    const vert q[6] = {{x0, y0, 0, 1, o->col, o->u0, o->v0, o->l0, o->l1, o->l2, o->l3}, {x1, y0, 0, 1, o->col, o->u1, o->v0, o->l0, o->l1, o->l2, o->l3},
                       {x0, y1, 0, 1, o->col, o->u0, o->v1, o->l0, o->l1, o->l2, o->l3}, {x1, y0, 0, 1, o->col, o->u1, o->v0, o->l0, o->l1, o->l2, o->l3},
                       {x1, y1, 0, 1, o->col, o->u1, o->v1, o->l0, o->l1, o->l2, o->l3}, {x0, y1, 0, 1, o->col, o->u0, o->v1, o->l0, o->l1, o->l2, o->l3}};
    memcpy(v, q, sizeof(q));
}

/* the sprites noted since the last time go into their texture; the strip is free again */
static void sprite_upload(IDirect3DDevice9 *dev) {
    if (sp_bits) { IDirect3DTexture9_UnlockRect(sp_sys, 0); sp_bits = NULL; }
    if (sp_d1 > sp_d0) {
        RECT r;
        POINT pt;
        r.left = 0; r.top = sp_d0; r.right = SP_W; r.bottom = sp_d1;
        pt.x = 0; pt.y = sp_d0;
        if (FAILED(IDirect3DDevice9_UpdateSurface(dev, sp_sys_lvl, &r, sp_lvl, &pt))) frame_abort = 1;
    }
    sp_x = sp_y = sp_shelf = 0; sp_d0 = SP_H; sp_d1 = 0;
}

/* the noted rectangles, drawn in their order (into the index target, or zoomed frames: onto the back buffer); those
 * that follow each other from one texture in one call */
static void draw_ops(IDirect3DDevice9 *dev) {
    const int zoomed = test_zoom != 1.0;      /* (the zoom speed test: straight onto the back buffer) */
    const int rgb = zframe && !zm_on && !zoomed;   /* a zoomed frame's screen part: in true colour into its own target */
    const LONGLONG t0 = now();
    IDirect3DSurface9 *bb = NULL;
    IDirect3DTexture9 *cur = NULL;
    vert *v;
    int i, first;
    if (!n_ops) return;
    set_states(dev);
    sprite_upload(dev);
    if (zoomed || rgb) update_palette();
    if (zoomed && zoom_clear) { zoom_clear = 0; IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0); }
    if (FAILED(IDirect3DVertexBuffer9_Lock(vb, 0, (UINT)(n_ops * 6 * sizeof(vert)), (void **)&v, D3DLOCK_DISCARD))) { frame_abort = 1; n_ops = 0; return; }
    for (i = 0; i < n_ops; i++) put_quad(v + 6 * i, &ops[i]);
    IDirect3DVertexBuffer9_Unlock(vb);
    if (!zoomed) {
        IDirect3DDevice9_GetRenderTarget(dev, 0, &bb);
        IDirect3DDevice9_SetRenderTarget(dev, 0, rgb ? zs_lvl : rt_lvl);
        if (!rgb && !zm_on) rt_used = 1;
    }
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetTexture(dev, 1, (IDirect3DBaseTexture9 *)pal_tex);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, zoomed);
    for (first = 0; first < n_ops; first = i) {
        const gop *o = &ops[first];
        IDirect3DTexture9 *tex = o->tex;
        for (i = first + 1; i < n_ops; i++) {
            if (ops[i].zoom != o->zoom) break;
            if (!ops[i].tex) continue;       /* (a fill: any texture will do) */
            if (!tex) tex = ops[i].tex;
            else if (ops[i].tex != tex) break;
        }
        if (tex && tex != cur) { IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)tex); cur = tex; }
        if (rgb) IDirect3DDevice9_SetPixelShader(dev, sh_rgb);
        else if (!zoomed) IDirect3DDevice9_SetPixelShader(dev, sh_idx);
        else if (!o->zoom) IDirect3DDevice9_SetPixelShader(dev, sh_rgb);
        else {
            const int n = test_zoom <= 0.3 ? 4 : test_zoom <= 0.55 ? 3 : 2;
            D3DSURFACE_DESC d;
            float c[4] = {0, 0, 0, 0};
            if (tex && !FAILED(IDirect3DTexture9_GetLevelDesc(tex, 0, &d))) { c[0] = (float)(1.0 / test_zoom / d.Width); c[1] = (float)(1.0 / test_zoom / d.Height); }
            IDirect3DDevice9_SetPixelShaderConstantF(dev, 1, c, 1);
            IDirect3DDevice9_SetPixelShader(dev, sh_avg[n] ? sh_avg[n] : sh_rgb);
        }
        IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, first * 6, (i - first) * 2);
        st.draws++;
    }
    IDirect3DDevice9_EndScene(dev);
    if (bb) { IDirect3DDevice9_SetRenderTarget(dev, 0, bb); IDirect3DSurface9_Release(bb); }
    st.ops += (unsigned)n_ops;
    n_ops = 0;
    st.t_draw += now() - t0;
}

static gop *new_op(IDirect3DDevice9 *dev) {
    if (n_ops == VB_QUADS) draw_ops(dev);
    ops_serial++;
    frame_ops++;
    return &ops[n_ops++];
}

/* room for a w x h sprite in the strip: where, and the memory to copy it to (NULL: none to be had) */
static uint8_t *sprite_room(IDirect3DDevice9 *dev, int w, int h, int *ax, int *ay) {
    D3DLOCKED_RECT lr;
    if (w > SP_W || h > SP_H) return NULL;
    if (sp_x + w > SP_W) { sp_y += sp_shelf; sp_x = 0; sp_shelf = 0; }
    if (sp_y + h > SP_H) draw_ops(dev);     /* (full: what is noted is drawn, the strip starts anew) */
    if (frame_abort) return NULL;
    if (!sp_bits) {
        if (FAILED(IDirect3DTexture9_LockRect(sp_sys, 0, &lr, NULL, D3DLOCK_NO_DIRTY_UPDATE))) return NULL;
        sp_bits = (uint8_t *)lr.pBits; sp_pitch = lr.Pitch;
    }
    *ax = sp_x; *ay = sp_y;
    sp_x += w;
    if (h > sp_shelf) sp_shelf = h;
    if (sp_y < sp_d0) sp_d0 = sp_y;
    if (sp_y + h > sp_d1) sp_d1 = sp_y + h;
    return sp_bits + (size_t)*ay * sp_pitch + *ax;
}

/* ---- the frame WA draws the rest into ---- */
static int frame_alloc(int w, int h) {
    SYSTEM_INFO si;
    size_t size;
    if (my_frame && my_w == w && my_h == h) return 1;
    if (my_frame) { VirtualFree(my_frame, 0, MEM_RELEASE); my_frame = NULL; }
    free(ww_addr); ww_addr = NULL;
    GetSystemInfo(&si);
    size = ((size_t)w * h + si.dwPageSize - 1) / si.dwPageSize * si.dwPageSize;
    my_frame = (uint8_t *)VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    ww_cap = size / si.dwPageSize;
    ww_addr = (void **)malloc(ww_cap * sizeof(void *));
    if (!my_frame || !ww_addr || si.dwPageSize != 4096) {
        say("no write-watched memory for a %dx%d frame (error %lu)", w, h, GetLastError());
        if (my_frame) VirtualFree(my_frame, 0, MEM_RELEASE);
        my_frame = NULL;
        return 0;
    }
    my_w = w; my_h = h; my_size = size;
    free(ref);
    ref = opt_check ? (uint8_t *)calloc(size, 1) : NULL;
    memset(my_frame, sent, size);
    ResetWriteWatch(my_frame, size);
    my_dirty = 0;
    ww_age = 0;
    return 1;
}

/* everything WA (or anyone) wrote back to "nothing", without looking at it */
static void frame_clean(void) {
    ULONG_PTR n = ww_cap, i;
    DWORD gran = 0;
    if (!my_frame || !my_dirty) return;
    if (GetWriteWatch(0, my_frame, my_size, ww_addr, &n, &gran)) { memset(my_frame, sent, my_size); }
    else for (i = 0; i < n; i++) memset(ww_addr[i], sent, 4096);
    ResetWriteWatch(my_frame, my_size);
    my_dirty = 0;
    ww_age = 0;
}

static void grid_fix(uintptr_t from, uintptr_t to) {   /* the display's frame grid, if it is locked onto the frame now */
    const uintptr_t d = wa_display();
    const uintptr_t g = d ? P32(d + D_GRID) : 0;
    if (d && g && P32(d + D_LOCKED) && P32(g + 8) == from) P32(g + 8) = (uint32_t)to;
}
static void swap_in(void) {
    wa_frame = P32(VA(FRAME_PTR_VA));
    P32(VA(FRAME_PTR_VA)) = (uint32_t)(uintptr_t)my_frame;
    grid_fix(wa_frame, (uintptr_t)my_frame);
}
static void swap_out(void) {
    if (P32(VA(FRAME_PTR_VA)) == (uint32_t)(uintptr_t)my_frame) {
        P32(VA(FRAME_PTR_VA)) = (uint32_t)wa_frame;
        grid_fix((uintptr_t)my_frame, wa_frame);
    } else say("WA's frame pointer is %08x, not this module's (%p): left as it is", P32(VA(FRAME_PTR_VA)), (void *)my_frame);
}
static void go_passive(const char *why) {
    if (mode != ACTIVE) return;
    if (zm_on) { target_swap(); zmap.dirty = 1; }
    zframe = 0; rt_used = 0;
    swap_out();
    mode = PASSIVE;
    my_dirty = 1;
    n_ops = 0; frame_ops = 0; frame_abort = 0; frame_layer_px = 0;
    if (why) say("passive: %s (frame %u)", why, frames);
}

/* the first / last byte of [p, p + n) that is not `sent`: its place, or -1 */
static int first_set(const uint8_t *p, int n) {
    const __m128i s = _mm_set1_epi8((char)sent);
    int i = 0;
    for (; i + 16 <= n; i += 16) {
        const int m = _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)(p + i)), s)) ^ 0xffff;
        if (m) return i + __builtin_ctz(m);
    }
    for (; i < n; i++) if (p[i] != sent) return i;
    return -1;
}
static int last_set(const uint8_t *p, int n) {
    const __m128i s = _mm_set1_epi8((char)sent);
    int i = n;
    for (; i - 16 >= 0; i -= 16) {
        const int m = _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)(p + i - 16)), s)) ^ 0xffff;
        if (m) return i - 16 + (31 - __builtin_clz(m));
    }
    for (i--; i >= 0; i--) if (p[i] != sent) return i;
    return -1;
}

static void ref_over(const uint8_t *row, int y, int x, int w) {
    uint8_t *d = ref + (size_t)y * my_w + x;
    int i;
    for (i = 0; i < w; i++) if (row[i] != sent) d[i] = row[i];
}

static gop *layer_op(IDirect3DDevice9 *dev, IDirect3DTexture9 *tex, const RECT *on_screen, int tx, int ty, int tw, int th) {
    gop *o = new_op(dev);
    frame_ops--;                            /* (not one of the GPU's own) */
    ops_serial--;
    o->tex = tex; o->col = COL_KEYED(sent); o->zoom = 0;
    o->x0 = (float)on_screen->left; o->y0 = (float)on_screen->top; o->x1 = (float)on_screen->right; o->y1 = (float)on_screen->bottom;
    o->u0 = (float)tx / tw; o->v0 = (float)ty / th;
    o->u1 = (float)(tx + on_screen->right - on_screen->left) / tw; o->v1 = (float)(ty + on_screen->bottom - on_screen->top) / th;
    o->l0 = o->l1 = 0; o->l2 = o->l3 = 1;
    return o;
}

/* Pieces of the frame with something in them: uploaded and noted as a layer over what the GPU has drawn so far, and the
 * frame made "nothing" there again */
static void put_pieces(IDirect3DDevice9 *dev, RECT *rects, int nr) {
    D3DLOCKED_RECT lr;
    LONGLONG t1, t2;
    int k, y;
    if (!nr || frame_abort) return;
    draw_ops(dev);                          /* (what the GPU drew before WA drew this lies under it) */
    t1 = now();
    if (!opt_ov || zm_on) {                 /* a frame-sized texture, each piece updated in its own place */
        if (FAILED(IDirect3DTexture9_LockRect(ov_sys, 0, &lr, NULL, D3DLOCK_NO_DIRTY_UPDATE))) { frame_abort = 1; return; }
        for (k = 0; k < nr; k++) {
            const int w = rects[k].right - rects[k].left;
            for (y = rects[k].top; y < rects[k].bottom; y++) {
                uint8_t *row = my_frame + (size_t)y * my_w + rects[k].left;
                memcpy((uint8_t *)lr.pBits + (size_t)y * lr.Pitch + rects[k].left, row, w);
                if (ref && !opt_strict) ref_over(row, y, rects[k].left, w);
                memset(row, sent, w);
            }
            st.ov_bytes += (unsigned)(w * (rects[k].bottom - rects[k].top));
            /* (only the screen's own layers say "this picture came whole from elsewhere": a zoomed map's are in map
             * pixels, and a long rope's rectangle there is no sign of another module) */
            if (!zm_on) frame_layer_px += (unsigned)(w * (rects[k].bottom - rects[k].top));
        }
        IDirect3DTexture9_UnlockRect(ov_sys, 0);
        t2 = now();
        st.t_copy += t2 - t1;
        for (k = 0; k < nr; k++) {
            POINT pt;
            pt.x = rects[k].left; pt.y = rects[k].top;
            if (FAILED(IDirect3DDevice9_UpdateSurface(dev, ov_sys_lvl, &rects[k], ov_lvl, &pt))) { frame_abort = 1; break; }
            layer_op(dev, ov_tex, &rects[k], rects[k].left, rects[k].top, my_w, my_h);
        }
        st.t_up += now() - t2;
        draw_ops(dev);                      /* (drawn now: the next layer uses the same texture) */
    } else {                                /* a strip refilled from its top: the pieces one under the other */
        k = 0;
        while (k < nr && !frame_abort) {
            int ay = 0;
            if (FAILED(IDirect3DTexture9_LockRect(ov_dyn, 0, &lr, NULL, D3DLOCK_DISCARD))) { frame_abort = 1; break; }
            while (k < nr) {
                RECT r = rects[k];
                const int w = r.right - r.left;
                if (r.bottom - r.top > ov_dyn_h - ay) {
                    if (ay && r.bottom - r.top <= ov_dyn_h) break;   /* (fits a fresh strip whole) */
                    if (ay == ov_dyn_h) break;
                    r.bottom = r.top + ov_dyn_h - ay;                /* (too tall for any: in parts) */
                    rects[k].top = r.bottom;
                } else k++;
                for (y = r.top; y < r.bottom; y++) {
                    uint8_t *row = my_frame + (size_t)y * my_w + r.left;
                    memcpy((uint8_t *)lr.pBits + (size_t)(ay + y - r.top) * lr.Pitch + r.left, row, w);
                    if (ref && !opt_strict) ref_over(row, y, r.left, w);
                    memset(row, sent, w);
                }
                st.ov_bytes += (unsigned)(w * (r.bottom - r.top));
                frame_layer_px += (unsigned)(w * (r.bottom - r.top));
                layer_op(dev, ov_dyn, &r, r.left, ay, my_w, ov_dyn_h);
                ay += r.bottom - r.top;
            }
            IDirect3DTexture9_UnlockRect(ov_dyn, 0);
            draw_ops(dev);                  /* (drawn now: the strip is filled anew next) */
        }
        st.t_copy += now() - t1;
    }
    st.runs += (unsigned)nr;
}

/* What WA drew into the frame since the last time: uploaded and noted as a layer over what the GPU has drawn so far,
 * and the frame made "nothing" again. Pages the write watch names (in rising order) -> the rows each has a part of ->
 * the smallest rectangles with anything in them: a piece ends after 9 empty rows, or at 64 rows (a long slanted line
 * is then a chain of small rectangles, not one big one). Rows in no written page are empty: they count as such, so a
 * piece goes on across them -- in a frame wider than a page (a zoomed map's) the pages of one tall, thin thing do not
 * touch each other. (The write watch is reset only now and then: a page written once is looked at each time until
 * then, which costs less than the page fault a freshly watched page takes at its first write.) */
static void take_layer(IDirect3DDevice9 *dev, int last) {
    ULONG_PTR n = ww_cap, i;
    DWORD gran = 0;
    RECT rects[256];
    int nr = 0, y, done = 0, x0 = my_w, x1 = -1, ya = -1, yb = -1;
    const LONGLONG t0 = now();
    LONGLONG t1;
    if (GetWriteWatch(0, my_frame, my_size, ww_addr, &n, &gran)) { frame_abort = 1; return; }
    t1 = now();
    st.t_ww += t1 - t0;
    st.pages += (unsigned)n;
#define PIECE_END() do { \
        if (nr == 256) { put_pieces(dev, rects, nr); nr = 0; }   /* (a layer holds any number: in lots of 256) */ \
        rects[nr].left = x0; rects[nr].right = x1 + 1; rects[nr].top = ya; rects[nr].bottom = yb + 1; nr++; \
        x0 = my_w; x1 = -1; ya = yb = -1; \
    } while (0)
    for (i = 0; i < n && !frame_abort; i++) {
        const size_t b0 = (size_t)((uint8_t *)ww_addr[i] - my_frame);
        int y0 = (int)(b0 / (size_t)my_w), y1 = (int)((b0 + 4096 + (size_t)my_w - 1) / (size_t)my_w);
        if (y1 > my_h) y1 = my_h;
        if (y0 < done) y0 = done;           /* (rows an earlier page had a part of are looked at already) */
        for (y = y0; y < y1; y++) {
            const uint8_t *row = my_frame + (size_t)y * my_w;
            const int a = first_set(row, my_w);
            int b;
            if (a < 0) continue;
            if (ya >= 0 && (y - yb > 9 || yb - ya >= 63)) PIECE_END();
            b = last_set(row, my_w);
            if (a < x0) x0 = a;
            if (b > x1) x1 = b;
            if (ya < 0) ya = y;
            yb = y;
        }
        if (y1 > done) done = y1;
    }
    if (ya >= 0 && !frame_abort) PIECE_END();
#undef PIECE_END
    st.t_scan += now() - t1;
    put_pieces(dev, rects, nr);
    if (last && ++ww_age >= opt_reset) { ResetWriteWatch(my_frame, my_size); ww_age = 0; }
    st.flushes++;
    st.t_flush += now() - t0;
}

/* The picture so far, within a rectangle, fetched from the index target into the frame (for something WA is about to
 * draw there that depends on what is under it). Everything noted is drawn first; the fetch waits for the GPU. The
 * leftover layer then takes the rectangle as WA leaves it. 0: it could not be done */
static unsigned n_fetch;
static int fetch_under(IDirect3DDevice9 *dev, int x0, int y0, int x1, int y1) {
    D3DLOCKED_RECT lr;
    RECT from, to;
    int x, y, i, j;
    if (!back_rt || !back_sys || test_zoom != 1.0) return 0;
    draw_ops(dev);
    if (frame_abort) return 0;
    for (y = y0; y < y1; y += BACK) for (x = x0; x < x1; x += BACK) {
        const int w = x1 - x < BACK ? x1 - x : BACK, h = y1 - y < BACK ? y1 - y : BACK;
        from.left = x; from.top = y; from.right = x + w; from.bottom = y + h;
        to.left = 0; to.top = 0; to.right = w; to.bottom = h;
        if (FAILED(IDirect3DDevice9_StretchRect(dev, rt_lvl, &from, back_rt, &to, D3DTEXF_NONE)) ||
            FAILED(IDirect3DDevice9_GetRenderTargetData(dev, back_rt, back_sys)) ||
            FAILED(IDirect3DSurface9_LockRect(back_sys, &lr, &to, D3DLOCK_READONLY))) return 0;
        for (j = 0; j < h; j++) {
            const uint32_t *g = (const uint32_t *)((const uint8_t *)lr.pBits + (size_t)j * lr.Pitch);
            uint8_t *d = my_frame + (size_t)(y + j) * my_w + x;
            for (i = 0; i < w; i++) d[i] = (uint8_t)g[i];
        }
        IDirect3DSurface9_UnlockRect(back_sys);
    }
    n_fetch++;
    return 1;
}

/* the device, fit to draw with now; else the frame is given up */
static IDirect3DDevice9 *device_now(void) {
    const uintptr_t be = wa_backend();
    IDirect3DDevice9 *dev = be ? (IDirect3DDevice9 *)(uintptr_t)P32(be + BE_DEV) : NULL;
    if (!dev || *(uint8_t *)(be + BE_LOST) || dev != obj_dev) { frame_abort = 1; return NULL; }
    return dev;
}

/* ---- the zoom test's region: WA's own code made to work out everything a zoomed-out view shows ---- */
static struct { int on, rw, rh; double z; } region;
static void to_screen(double rx, double ry, float *x, float *y) {
    *x = (float)((rx - region.rw * 0.5) * region.z + my_w * 0.5);
    *y = (float)((ry - region.rh * 0.5) * region.z + my_h * 0.5);
}

/* ---- the hooks ---- */
typedef uint32_t *(__attribute__((fastcall)) *blit_fn)(void *, uint32_t *, uintptr_t, int, int, int, int, int, int, int);
typedef uint32_t *(__attribute__((fastcall)) *fill_fn)(void *, uint32_t *, int, int, int, int, int);
typedef uint32_t *(__attribute__((fastcall)) *show_fn)(void *, uint32_t *, void *);
static blit_fn orig_blit;
static fill_fn orig_fill;
static show_fn orig_show;
static int ghost_direct;                /* the kept show-frame work hangs on WA's own function: call that, not the table's old entry */
void *orig_be_free, *orig_be_close, *orig_rc_alloc, *orig_rc_free, *orig_rc_resize, *orig_rc_read, *orig_rc_read_end;
void *orig_surf_dtor, *orig_surf_unlock, *orig_surf_alloc, *orig_surf_free;
void *disp_orig[12];

static int mine_now(void) {
    return mode == ACTIVE && P32(VA(FRAME_PTR_VA)) == (uint32_t)(uintptr_t)my_frame && PI32(VA(FRAME_W_VA)) == my_w && PI32(VA(FRAME_H_VA)) == my_h;
}

/* RenderContext slot 23, the surface blit (ECX the context, EDX the result; surface, x, y, source x, y, width, height,
 * flags: 2 = the surface's colour key is see-through; ret 0x20) */
static uint32_t *__attribute__((fastcall)) hook_blit(void *ctx, uint32_t *result, uintptr_t surf, int dx, int dy, int sx, int sy, int w, int h, int flags) {
    IDirect3DDevice9 *dev;
    stex *t;
    gop *o;
    float tw, th;
    int odx, ody, osx, osy, ow, oh;
    if (tracing) say("  blit surf %08x %dx%d key %u -> %d,%d from %d,%d size %dx%d flags %x frame %08x", (unsigned)surf, surf ? PI32(surf + 4) : 0,
                     surf ? PI32(surf + 8) : 0, surf ? *(uint8_t *)(surf + 0x10) : 0, dx, dy, sx, sy, w, h, flags, P32(VA(FRAME_PTR_VA)));
    if (in_ref) return orig_blit(ctx, result, surf, dx, dy, sx, sy, w, h, flags);
    if (!mine_now()) {
        if (mode == PASSIVE && P32(VA(FRAME_PTR_VA)) == last_wa_frame) passive_blits++;
        return orig_blit(ctx, result, surf, dx, dy, sx, sy, w, h, flags);
    }
    if (frame_abort || !surf || !P32(surf + 0xc)) { cpu_touch++; return orig_blit(ctx, result, surf, dx, dy, sx, sy, w, h, flags); }
    odx = dx; ody = dy; osx = sx; osy = sy; ow = w; oh = h;
    if (!region.on) {                       /* WA's own clipping to the frame (005A27F2..005A28BC) */
        if (dx >= my_w || dy >= my_h) goto done;
        if (dx < 0) { sx -= dx; w += dx; dx = 0; }
        if (dy < 0) { sy -= dy; h += dy; dy = 0; }
        if (dx + w > my_w) w = my_w - dx;
        if (dy + h > my_h) h = my_h - dy;
    }
    if (w <= 0 || h <= 0) goto done;
    dev = device_now();
    if (!dev) { cpu_touch++; return orig_blit(ctx, result, surf, dx, dy, sx, sy, w, h, flags); }
    if (cpu_maybe) { take_layer(dev, 0); cpu_maybe = 0; }
    if (ref && !opt_strict && !region.on) {
        uint32_t res = 0;
        if (chk_n < 4096) {
            const stex *e = tc_find(surf);
            chk_blit[chk_n].surf = surf; chk_blit[chk_n].x = odx; chk_blit[chk_n].y = ody; chk_blit[chk_n].w = ow; chk_blit[chk_n].h = oh;
            chk_blit[chk_n].sx = osx; chk_blit[chk_n].sy = osy; chk_blit[chk_n].flags = flags; chk_blit[chk_n].sw = PI32(surf + 4); chk_blit[chk_n].sh = PI32(surf + 8);
            chk_blit[chk_n].fresh = !e || !e->tex ? 2 : e->dirty;
            chk_n++;
        }
        P32(VA(FRAME_PTR_VA)) = (uint32_t)(uintptr_t)ref;
        orig_blit(ctx, &res, surf, odx, ody, osx, osy, ow, oh, flags);
        P32(VA(FRAME_PTR_VA)) = (uint32_t)(uintptr_t)my_frame;
    }
    t = frame_abort ? NULL : surface_texture(dev, surf);
    if (!t) { frame_abort = 1; cpu_touch++; return orig_blit(ctx, result, surf, dx, dy, sx, sy, w, h, flags); }
    o = new_op(dev);
    o->tex = t->tex;
    o->col = (flags & 2) ? COL_KEYED(*(uint8_t *)(surf + 0x10)) : COL_OPAQUE;
    if (t->page >= 0) tw = th = (float)page_size; else { tw = (float)t->w; th = (float)t->h; }
    o->u0 = (float)(t->ax + sx) / tw; o->v0 = (float)(t->ay + sy) / th;
    o->u1 = (float)(t->ax + sx + w) / tw; o->v1 = (float)(t->ay + sy + h) / th;
    o->l0 = (t->ax + 0.5f) / tw; o->l1 = (t->ay + 0.5f) / th; o->l2 = (t->ax + t->w - 0.5f) / tw; o->l3 = (t->ay + t->h - 0.5f) / th;
    if (region.on) {
        to_screen(dx, dy, &o->x0, &o->y0);
        to_screen(dx + w, dy + h, &o->x1, &o->y1);
        o->zoom = 1;
    } else {
        o->x0 = (float)dx; o->y0 = (float)dy; o->x1 = (float)(dx + w); o->y1 = (float)(dy + h);
        o->zoom = 0;
    }
    if (opt_sabotage == 1) { o->x0 += 1; o->x1 += 1; }
    else if (opt_sabotage == 2) o->col = COL_OPAQUE;
    else if (opt_sabotage == 3 && ops_serial % 97 == 0) n_ops--;
    else if (opt_sabotage == 4) { o->u0 += 0.5f / tw; o->u1 += 0.5f / tw; o->v0 += 0.5f / th; o->v1 += 0.5f / th; }
done:
    *result = P32(VA(RC_OK_VA));
    return result;
}

/* RenderContext slot 19, the rectangle fill (ECX the context, EDX the result; x, y, width, height, colour; ret 0x14) */
static uint32_t *__attribute__((fastcall)) hook_fill(void *ctx, uint32_t *result, int x, int y, int w, int h, int colour) {
    IDirect3DDevice9 *dev;
    gop *o;
    if (tracing) say("  fill %d,%d size %dx%d colour %d frame %08x", x, y, w, h, colour, P32(VA(FRAME_PTR_VA)));
    if (in_ref) return orig_fill(ctx, result, x, y, w, h, colour);
    if (!mine_now() || frame_abort || w <= 0 || h <= 0) { cpu_touch++; return orig_fill(ctx, result, x, y, w, h, colour); }
    dev = device_now();
    if (!dev) { cpu_touch++; return orig_fill(ctx, result, x, y, w, h, colour); }
    if (cpu_maybe) { take_layer(dev, 0); cpu_maybe = 0; }
    if (frame_abort) { cpu_touch++; return orig_fill(ctx, result, x, y, w, h, colour); }
    if (ref && !opt_strict && !region.on) {
        uint32_t res = 0;
        if (chk_n < 4096) { chk_blit[chk_n].surf = 0; chk_blit[chk_n].x = x; chk_blit[chk_n].y = y; chk_blit[chk_n].w = w; chk_blit[chk_n].h = h; chk_blit[chk_n].flags = colour; chk_n++; }
        P32(VA(FRAME_PTR_VA)) = (uint32_t)(uintptr_t)ref;
        orig_fill(ctx, &res, x, y, w, h, colour);
        P32(VA(FRAME_PTR_VA)) = (uint32_t)(uintptr_t)my_frame;
    }
    o = new_op(dev);
    o->tex = NULL; o->col = COL_FILL(colour & 255); o->zoom = 0;
    o->u0 = o->v0 = o->u1 = o->v1 = 0; o->l0 = o->l1 = 0; o->l2 = o->l3 = 1;
    if (region.on) { to_screen(x, y, &o->x0, &o->y0); to_screen(x + w, y + h, &o->x1, &o->y1); }
    else { o->x0 = (float)x; o->y0 = (float)y; o->x1 = (float)(x + w); o->y1 = (float)(y + h); }
    *result = P32(VA(RC_OK_VA));
    return result;
}

/* WA's sprite blit (004F6910), at its calls in the display's code (0056AEDE with a blend table, 0056AFED, 0056B062
 * tiled, 0056B709 bitmaps). ESI the target grid; x, y, width, height, source grid, source x, y, blend table, mode (low
 * word: 0 whole, 1 index 0 see-through, 2 / 3 depending on what is there already; high word: mirrored / turned); ret
 * 0x24. Onto the module's frame, 8-bit, whole or see-through, as it is or mirrored left to right, without a table: a
 * rectangle for the GPU, its pixels copied into the strip. Anything else stays WA's (and is counted). */
void *orig_sprite;
static int sprite_hooked;              /* WA's sprite calls come to the module */
uint32_t __cdecl call_sprite(uintptr_t grid, const uint32_t *args);
__asm__(".text\n.globl _call_sprite\n_call_sprite:\n"
        "    pushl %esi\n    pushl %ebx\n    movl 12(%esp), %esi\n    movl 16(%esp), %ebx\n"
        "    pushl 32(%ebx)\n    pushl 28(%ebx)\n    pushl 24(%ebx)\n    pushl 20(%ebx)\n    pushl 16(%ebx)\n"
        "    pushl 12(%ebx)\n    pushl 8(%ebx)\n    pushl 4(%ebx)\n    pushl 0(%ebx)\n"
        "    call *_orig_sprite\n    popl %ebx\n    popl %esi\n    ret\n");
int __cdecl sprite_hook(uint32_t *f) {
    const uintptr_t dst = f[1], src = f[13];
    const int x = (int)f[9], y = (int)f[10], w = (int)f[11], h = (int)f[12], sx = (int)f[14], sy = (int)f[15];
    uint32_t table, mode;
    unsigned kind, orient;
    int mirrored;
    IDirect3DDevice9 *dev;
    const int32_t *clip;
    const uint8_t *from;
    uint8_t *to;
    gop *o;
    int cx0, cy0, cx1, cy1, cw, ch, ssx, ssy, sstride, ax, ay, r;
    if ((test_table || test_kind) && dst && src && (f[17] & 0xffff) == 1 && !f[16] && wa_display() && dst == P32(wa_display() + D_GRID) &&
        (unsigned)(x * 7 + y * 13 + w) % (unsigned)(test_table ? test_table : 5) == 0) {   /* (the same sprites in both of the check's passes) */
        if (test_table) f[16] = (uint32_t)(wa_display() + 0x4df4);
        else f[17] = (f[17] & 0xffff0000u) | (uint32_t)test_kind;
    }
    table = f[16]; mode = f[17];
    kind = mode & 0xffff; orient = (mode >> 16) & 15;
    mirrored = orient == 1 || orient == 10;
    if (in_ref || !mine_now() || frame_abort || !dst || !src || !w || !h) return 0;
    if (P32(dst + GR_DATA) != (uint32_t)(uintptr_t)my_frame || PI32(dst + GR_BPP) != 8 || PI32(dst + GR_STRIDE) != my_w || PI32(src + GR_BPP) != 8) return 0;
    clip = (const int32_t *)(dst + GR_CLIP);   /* WA's own clipping (004F6939..) */
    cx0 = clip[0] > x ? clip[0] : x; cy0 = clip[1] > y ? clip[1] : y;
    cx1 = clip[2] < x + w ? clip[2] : x + w; cy1 = clip[3] < y + h ? clip[3] : y + h;
    if (cx0 >= cx1 || cy0 >= cy1) return 0;
    if (cx0 < 0 || cy0 < 0 || cx1 > my_w || cy1 > my_h) { cpu_touch++; return 0; }
    cw = cx1 - cx0; ch = cy1 - cy0;
    ssx = mirrored ? sx + (x + w - cx1) : sx + (cx0 - x);
    ssy = sy + (cy0 - y);
    if (kind > 1 || table || !(orient == 0 || orient == 11 || (mirrored && kind == 1)) ||
        ssx < 0 || ssy < 0 || ssx + cw > PI32(src + GR_W) || ssy + ch > PI32(src + GR_H) || !P32(src + GR_DATA)) {
        sp_stat.left++;
        sp_stat.combo[kind & 3][orient][table != 0]++;
        cpu_touch++;
        if (kind > 1 || table) {            /* its result depends on what is under it: that is fetched for WA */
            dev = device_now();
            if (dev && cpu_maybe) { take_layer(dev, 0); cpu_maybe = 0; }
            if (opt_sabotage == 6) ;
            else if (!dev || frame_abort || !fetch_under(dev, cx0, cy0, cx1, cy1)) {
                static int told;
                if (!told) { told = 1; say("a sprite that depends on what is under it could not be given the picture under it: its colours may be off"); }
            }
        }
        return 0;
    }
    dev = device_now();
    if (!dev) { cpu_touch++; return 0; }
    if (cpu_maybe) { take_layer(dev, 0); cpu_maybe = 0; }
    if (frame_abort) { cpu_touch++; return 0; }
    if (ref && !opt_strict && !region.on) { /* (the weaker check: WA's own blit into the second frame) */
        P32(dst + GR_DATA) = (uint32_t)(uintptr_t)ref;
        call_sprite(dst, f + 9);
        P32(dst + GR_DATA) = (uint32_t)(uintptr_t)my_frame;
    }
    to = sprite_room(dev, cw, ch, &ax, &ay);
    if (!to) { cpu_touch++; return 0; }
    sstride = PI32(src + GR_STRIDE);
    from = (const uint8_t *)(uintptr_t)P32(src + GR_DATA) + (size_t)ssy * sstride + ssx;
    for (r = 0; r < ch; r++) memcpy(to + (size_t)r * sp_pitch, from + (size_t)r * sstride, cw);
    o = new_op(dev);
    o->tex = sp_tex; o->col = kind ? COL_KEYED(0) : COL_OPAQUE; o->zoom = 0;
    o->x0 = (float)cx0; o->y0 = (float)cy0; o->x1 = (float)cx1; o->y1 = (float)cy1;
    o->u0 = (float)(mirrored ? ax + cw : ax) / SP_W; o->u1 = (float)(mirrored ? ax : ax + cw) / SP_W;
    o->v0 = (float)ay / SP_H; o->v1 = (float)(ay + ch) / SP_H;
    o->l0 = (ax + 0.5f) / SP_W; o->l1 = (ay + 0.5f) / SP_H; o->l2 = (ax + cw - 0.5f) / SP_W; o->l3 = (ay + ch - 0.5f) / SP_H;
    if (opt_sabotage == 5) { o->u0 += 1.0f / SP_W; o->u1 += 1.0f / SP_W; }
    sp_stat.gpu++;
    sp_stat.pixels += (unsigned)(cw * ch);
    return 1;
}
__asm__(".text\n.globl _sprite_thunk\n_sprite_thunk:\n"
        "    pushal\n    pushl %esp\n    call _sprite_hook\n    addl $4, %esp\n    testl %eax, %eax\n    popal\n"
        "    jnz 1f\n    jmp *_orig_sprite\n1:  movl $1, %eax\n    ret $0x24\n");
extern void sprite_thunk(void);

/* the display's drawing methods (vtable slots 11..22): looked at on the way in and on the way out, whatever their
 * arguments (the return address is swapped for the module's while WA's method runs) */
typedef struct { uint32_t ret; unsigned touch; uintptr_t disp; int region, cpu, again; int32_t saved[8]; uint32_t args[16]; uintptr_t argp; } dcall;
void __cdecl call_disp(void *method, uintptr_t disp, const uint32_t *args, uint32_t bytes);
__asm__(".text\n.globl _call_disp\n_call_disp:\n"
        "    pushl %ebp\n    movl %esp, %ebp\n    pushl %esi\n    pushl %edi\n    pushl %ebx\n"
        "    movl 20(%ebp), %ecx\n    movl 16(%ebp), %esi\n    subl %ecx, %esp\n    movl %esp, %edi\n    shrl $2, %ecx\n    cld\n    rep movsl\n"
        "    movl 12(%ebp), %ecx\n    call *8(%ebp)\n"
        "    leal -12(%ebp), %esp\n    popl %ebx\n    popl %edi\n    popl %esi\n    popl %ebp\n    ret\n");
static dcall dstack[32];
static int dsp;
void *disp_back[12];

void __cdecl disp_enter(uint32_t *f, int k) {
    dcall *c = &dstack[dsp < 31 ? dsp : 31];
    uint32_t *a = f + 9;
    const uint32_t ret = f[8];
    const uintptr_t d = f[6];
    dsp++;
    c->ret = ret; c->touch = cpu_touch; c->disp = d; c->region = 0; c->again = 0;
    if (in_ref) { f[8] = (uint32_t)(uintptr_t)disp_back[k]; c->cpu = 0; return; }   /* (inside the second call: WA's own, untouched) */
    if (ref && opt_strict && dsp == 1 && mine_now() && !frame_abort && test_zoom == 1.0 && (!zframe || zm_on)) {
        memcpy(c->args, a, sizeof(c->args));
        c->argp = (uintptr_t)a;
        c->again = 1;
    }
    /* The sky (slot 11), fills (18) and the land (22) draw through the blit and the fill only; sprites (19) and
     * bitmaps (20) through those and the sprite blit, except with the flags for WA's dotted see-through drawing
     * (0x8000000, 0x10000000: pixel by pixel into the frame). Every other method draws on the CPU. */
    c->cpu = !(k == 0 || k == 7 || k == 11 || (sprite_hooked && ((k == 8 && !(a[2] & 0x18000000)) || (k == 9 && !(a[7] & 0x18000000)))));
    f[8] = (uint32_t)(uintptr_t)disp_back[k];
    if (!frame_started) {                   /* the frame's first drawing call: nothing is drawn yet */
        frame_started = 1;
        if (P32(d) == VA(DISP_VT_VA) && frame_is_read(d)) {
            if (!read_seen || frames - read_seen > 120) say("WA's frames are being read (Export Video): WA draws them itself while that goes on");
            read_seen = frames ? frames : 1;
            if (mode == ACTIVE) go_passive(NULL);
        }
    }
    if (tracing) say(" slot %d from %08x: %08x %08x %08x %08x", k + 11, (unsigned)(ret - base + WA_BASE), a[0], a[1], a[2], a[3]);
    if (opt_nosprites && k == 8 && !region.on && mine_now() && !frame_abort && d &&
        ret >= VA(QUEUE_FIRST_VA) && ret < VA(QUEUE_LAST_VA) && ret != VA(Q_SPRITE4_RET)) {
        const unsigned id = a[2] & 0xffff;
        if (id && (P32(d + D_SPRITES + id * 4) || P32(d + D_SPRITES2 + id * 4))) {
            int32_t *p = (int32_t *)(d + D_W);
            memcpy(c->saved, p, sizeof(c->saved));
            if (opt_nosprites == 2 && n_pages) {   /* a placeholder where the sprite's middle is */
                IDirect3DDevice9 *dev = device_now();
                if (dev) {
                    const float x = (float)(p[6] + ((int32_t)a[0] >> 16)), y = (float)(p[7] + ((int32_t)a[1] >> 16));
                    gop *o = new_op(dev);
                    o->tex = pages[0].tex; o->col = COL_KEYED(0); o->zoom = 0;
                    o->x0 = x - 12; o->y0 = y - 12; o->x1 = x + 12; o->y1 = y + 12;
                    o->u0 = 0; o->v0 = 0; o->u1 = 24.0f / page_size; o->v1 = 24.0f / page_size; o->l0 = o->l1 = 0; o->l2 = o->l3 = 1;
                }
            }
            p[2] = p[3] = p[4] = p[5] = 0;  /* nothing is inside this clip */
            c->region = 2;
        }
    }
    if (test_zoom != 1.0 && !region.on && !c->region && mine_now() && !frame_abort && d) {
        const int from_queue = ret >= VA(QUEUE_FIRST_VA) && ret < VA(QUEUE_LAST_VA) && ret != VA(Q_SPRITE4_RET);
        int want = k == 0 || k == 11 || (k == 7 && from_queue);
        if (k == 8 && from_queue) {         /* a sprite of the map: kept as a surface (a back layer) -> the region; else moved only */
            const unsigned id = a[2] & 0xffff;
            if (id && !P32(d + D_SPRITES + id * 4) && !P32(d + D_SPRITES2 + id * 4) && P32(d + D_SURF_SPRITES + id * 4)) want = 1;
            else { a[0] = (uint32_t)(int32_t)((int32_t)a[0] * test_zoom); a[1] = (uint32_t)(int32_t)((int32_t)a[1] * test_zoom); }
        }
        if (want) {
            int32_t *p = (int32_t *)(d + D_W);
            const int rw = (int)ceil(my_w / test_zoom), rh = (int)ceil(my_h / test_zoom);
            memcpy(c->saved, p, sizeof(c->saved));   /* width, height, clip x1 y1 x2 y2, origin x y */
            p[0] = rw; p[1] = rh; p[2] = 0; p[3] = 0; p[4] = rw; p[5] = rh;
            p[6] += (rw - my_w) / 2; p[7] += (rh - my_h) / 2;
            region.on = 1; region.rw = rw; region.rh = rh; region.z = test_zoom;
            c->region = 1;
        }
    }
}
uint32_t __cdecl disp_leave(int k, uintptr_t sp) {
    dcall *c;
    if (dsp > 0) dsp--;
    c = &dstack[dsp < 31 ? dsp : 31];
    if (in_ref) return c->ret;
    if (c->region) { memcpy((void *)(c->disp + D_W), c->saved, sizeof(c->saved)); if (c->region == 1) region.on = 0; }
    if (c->cpu || c->touch != cpu_touch) cpu_maybe = 1;
    if (c->again && sp >= c->argp && sp - c->argp <= sizeof(c->args) && !((sp - c->argp) & 3) && mine_now()) {
        /* the same call once more, by WA alone, into the second frame (the method took sp - argp bytes of arguments) */
        const int maybe = cpu_maybe;
        const unsigned touch = cpu_touch;
        const uint32_t ret = c->ret;
        in_ref = 1;
        P32(VA(FRAME_PTR_VA)) = (uint32_t)(uintptr_t)ref;
        grid_fix((uintptr_t)my_frame, (uintptr_t)ref);
        call_disp(disp_orig[k], c->disp, c->args, (uint32_t)(sp - c->argp));
        grid_fix((uintptr_t)ref, (uintptr_t)my_frame);
        P32(VA(FRAME_PTR_VA)) = (uint32_t)(uintptr_t)my_frame;
        in_ref = 0;
        cpu_maybe = maybe; cpu_touch = touch;
        return ret;
    }
    return c->ret;
}
#define DISP_THUNK(K) \
    __asm__(".text\n.globl _disp_thunk_" #K "\n_disp_thunk_" #K ":\n" \
            "    pushal\n    pushl $" #K "\n    leal 4(%esp), %eax\n    pushl %eax\n    call _disp_enter\n    addl $8, %esp\n    popal\n" \
            "    jmp *_disp_orig+4*" #K "\n" \
            ".globl _disp_back_" #K "\n_disp_back_" #K ":\n" \
            "    leal -4(%esp), %esp\n    pushfl\n    pushal\n    leal 40(%esp), %eax\n    pushl %eax\n    pushl $" #K "\n    call _disp_leave\n    addl $8, %esp\n" \
            "    movl %eax, 36(%esp)\n    popal\n    popfl\n    ret\n"); \
    extern void disp_thunk_##K(void); extern void disp_back_##K(void);
DISP_THUNK(0) DISP_THUNK(1) DISP_THUNK(2) DISP_THUNK(3) DISP_THUNK(4) DISP_THUNK(5)
DISP_THUNK(6) DISP_THUNK(7) DISP_THUNK(8) DISP_THUNK(9) DISP_THUNK(10) DISP_THUNK(11)

/* told first, then WA's own runs: (ECX) a surface is destroyed, written, given or losing its pixels; the backend frees
 * its Direct3D objects or closes; the RenderContext frees or re-makes its frame */
void __cdecl note_surface_gone(uintptr_t s) { stex *e = tc_find(s); if (e) { tc_free(e); e->surf = TC_GONE; } }
void __cdecl note_surface_written(uintptr_t s) { stex *e = tc_find(s); if (e) { e->dirty = 1; e->written = frames; } }
void __cdecl note_backend_free(uintptr_t be) { (void)be; go_passive("WA frees its Direct3D objects"); release_objects("WA frees its own"); cooldown = 2; }
void __cdecl note_frame_change(uintptr_t rc) { (void)rc; go_passive("WA re-makes its frame"); cooldown = 2; }
/* RenderContext slots 15 / 16: the frame locked for reading and let go again (WA's screenshot, 0056C6F0; Export Video
 * too, but then the module is passive). The frame the module gave WA holds only what WA drew itself, so the whole
 * picture is fetched into it from the index target first; afterwards it is "nothing" again. */
static int read_filled;
static int fetch_zs(IDirect3DDevice9 *dev);
void __cdecl note_read(uintptr_t rc) {
    IDirect3DDevice9 *dev;
    (void)rc;
    if (mode != ACTIVE || in_ref || read_filled || P32(VA(FRAME_PTR_VA)) != (uint32_t)(uintptr_t)my_frame || n_ops || frame_ops) return;
    dev = device_now();
    frame_abort = 0;                        /* (asked between frames: no frame to give up) */
    if (dev && zs_shown) {                  /* a zoomed frame: its true-colour picture, as the palette's nearest colours */
        if (!fetch_zs(dev)) { say("a screenshot is being taken, but the zoomed picture could not be fetched from the GPU: it will be wrong"); return; }
        read_filled = 1;
        say("WA reads its frame (a screenshot): the zoomed picture was fetched from the GPU for it, in the palette's nearest colours");
        return;
    }
    if (!dev || !fetch_under(dev, 0, 0, my_w, my_h)) { say("a screenshot is being taken, but the picture could not be fetched from the GPU: it will be wrong"); return; }
    read_filled = 1;
    if (ref && opt_strict) {                /* (the check: against WA's own picture of the same frame) */
        size_t i, bad = 0;
        for (i = 0; i < (size_t)my_w * my_h; i++) bad += my_frame[i] != ref[i];
        say("check: the frame given to WA for reading (a screenshot): %u of %u pixels differ from WA's own picture", (unsigned)bad, (unsigned)(my_w * my_h));
    } else say("WA reads its frame (a screenshot): the picture was fetched from the GPU for it");
}
void __cdecl note_read_end(uintptr_t rc) {
    (void)rc;
    if (!read_filled) return;
    read_filled = 0;
    if (my_frame) memset(my_frame, sent, (size_t)my_w * my_h);
}
#define NOTE_THUNK(NAME, NOTE, ORIG) \
    __asm__(".text\n.globl _" #NAME "\n_" #NAME ":\n    pushal\n    pushl %ecx\n    call _" #NOTE "\n    addl $4, %esp\n    popal\n    jmp *_" #ORIG "\n"); \
    extern void NAME(void);
NOTE_THUNK(thunk_surf_dtor, note_surface_gone, orig_surf_dtor)
NOTE_THUNK(thunk_surf_alloc, note_surface_gone, orig_surf_alloc)
NOTE_THUNK(thunk_surf_free, note_surface_gone, orig_surf_free)
NOTE_THUNK(thunk_surf_unlock, note_surface_written, orig_surf_unlock)
NOTE_THUNK(thunk_be_free, note_backend_free, orig_be_free)
NOTE_THUNK(thunk_be_close, note_backend_free, orig_be_close)
NOTE_THUNK(thunk_rc_alloc, note_frame_change, orig_rc_alloc)
NOTE_THUNK(thunk_rc_free, note_frame_change, orig_rc_free)
NOTE_THUNK(thunk_rc_resize, note_frame_change, orig_rc_resize)
NOTE_THUNK(thunk_rc_read, note_read, orig_rc_read)
NOTE_THUNK(thunk_rc_read_end, note_read_end, orig_rc_read_end)

/* WKD3D_CHECK: the index target read back and compared with the frame WA's own code drew */
static void check_frame(IDirect3DDevice9 *dev) {
    D3DLOCKED_RECT lr;
    unsigned long long bad = 0;
    const int cw = zm_on ? zm_rw : my_w, ch = zm_on ? zm_rh : my_h;   /* (of the map: the part in use) */
    int x, y, k, fx = -1, fy = -1, fa = 0, fb = 0, bx0 = 1 << 30, by0 = 1 << 30, bx1 = -1, by1 = -1;
    if (FAILED(IDirect3DDevice9_GetRenderTargetData(dev, rt_lvl, chk_surf)) || FAILED(IDirect3DSurface9_LockRect(chk_surf, &lr, NULL, D3DLOCK_READONLY))) {
        say("check: the index target can't be read back");
        return;
    }
    for (y = 0; y < ch; y++) {
        const uint32_t *g = (const uint32_t *)((const uint8_t *)lr.pBits + (size_t)y * lr.Pitch);
        const uint8_t *r = ref + (size_t)y * my_w;
        for (x = 0; x < cw; x++) if ((uint8_t)g[x] != r[x]) {
            if (!bad) { fx = x; fy = y; fa = (uint8_t)g[x]; fb = r[x]; }
            if (x < bx0) bx0 = x;
            if (x > bx1) bx1 = x;
            if (y < by0) by0 = y;
            if (y > by1) by1 = y;
            bad++;
        }
    }
    if (bad && chk.bad_frames < 3) {        /* (where, row by row, and what: for finding the cause) */
        int rows = 0;
        for (y = by0; y <= by1 && rows < 24; y++) {
            const uint32_t *g = (const uint32_t *)((const uint8_t *)lr.pBits + (size_t)y * lr.Pitch);
            const uint8_t *r = ref + (size_t)y * my_w;
            char b[400];
            int n = 0, c = 0;
            for (x = bx0; x <= bx1 && n < 380; x++) if ((uint8_t)g[x] != r[x]) { n += sprintf(b + n, "%d:%d/%d ", x, (uint8_t)g[x], r[x]); c++; }
            if (c) { say("check:     row %d: %d differ: %s", y, c, b); rows++; }
        }
    }
    IDirect3DSurface9_UnlockRect(chk_surf);
    chk.frames++; chk.pixels += (unsigned long long)cw * ch; chk.bad += bad;
    if (zm_on) chk_maps++;
    if (bad) {
        chk.bad_frames++;
        if (chk.bad_frames <= 20) {
            say("check: frame %u: %llu pixels differ within %d,%d..%d,%d (first at %d,%d: the GPU has index %d, WA's own drawing %d)", frames, bad, bx0, by0, bx1, by1, fx, fy, fa, fb);
            for (k = 0; k < chk_n; k++) if (fx >= chk_blit[k].x && fx < chk_blit[k].x + chk_blit[k].w && fy >= chk_blit[k].y && fy < chk_blit[k].y + chk_blit[k].h)
            {
                const stex *e = chk_blit[k].surf ? tc_find(chk_blit[k].surf) : NULL;
                const int px = fx - chk_blit[k].x + chk_blit[k].sx, py = fy - chk_blit[k].y + chk_blit[k].sy;
                say("check:   over it: %s %d of %d: surface %08x %dx%d -> %d,%d from %d,%d size %dx%d flags %x, texture %s; its pixel there is now %d; "
                    "last written in frame %u, uploaded in frame %u, to upload %d", chk_blit[k].surf ? "blit" : "fill", k, chk_n,
                    (unsigned)chk_blit[k].surf, chk_blit[k].sw, chk_blit[k].sh, chk_blit[k].x, chk_blit[k].y, chk_blit[k].sx, chk_blit[k].sy, chk_blit[k].w, chk_blit[k].h,
                    chk_blit[k].flags, chk_blit[k].fresh == 2 ? "new" : chk_blit[k].fresh ? "uploaded again" : "as it was",
                    e && e->pix && px >= 0 && py >= 0 && px < e->w && py < e->h ? *(const uint8_t *)(e->pix + (size_t)py * e->w + px) : -1,
                    e ? e->written : 0, e ? e->uploaded : 0, e ? e->dirty : -1);
            }
        }
    }
    if (chk.frames % 200 == 0) say("check: %u frames compared (%u of them zoomed maps), %u with differences, %llu of %llu pixels differ", chk.frames, chk_maps, chk.bad_frames, chk.bad, chk.pixels);
}

/* the notice: said in the log, and shown over the picture for 15 seconds of the frames this module draws */
static void notify(const char *fmt, ...) {
    va_list v;
    va_start(v, fmt);
    _vsnprintf(note_text, sizeof(note_text) - 1, fmt, v);
    va_end(v);
    note_text[sizeof(note_text) - 1] = 0;
    note_drawn = 0;
    note_until = 0;                         /* (starts counting with the first frame it is shown in) */
    say("notice: %s", note_text);
}
static void draw_notice(IDirect3DDevice9 *dev) {
    const DWORD t = GetTickCount();
    D3DLOCKED_RECT lr;
    gop o;
    vert *v;
    if (!note_text[0] || !sh_plain) return;
    if (!note_until) note_until = t + 15000;
    if ((int)(note_until - t) <= 0) { note_text[0] = 0; return; }
    if (!note_tex && FAILED(IDirect3DDevice9_CreateTexture(dev, NOTE_W, NOTE_H, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &note_tex, NULL))) { note_tex = NULL; return; }
    if (!note_drawn) {                      /* the text, drawn by Windows into a small picture */
        BITMAPINFO bi;
        uint32_t *bits = NULL;
        HDC dc = CreateCompatibleDC(NULL);
        HBITMAP bm;
        HFONT font;
        RECT r = {8, 4, NOTE_W - 8, NOTE_H - 4};
        int i, y;
        memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = NOTE_W; bi.bmiHeader.biHeight = -NOTE_H;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        bm = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0) : NULL;
        if (!bm || !bits) { if (bm) DeleteObject(bm); if (dc) DeleteDC(dc); note_text[0] = 0; return; }
        font = CreateFontA(15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, NONANTIALIASED_QUALITY, 0, "Tahoma");
        SelectObject(dc, bm);
        if (font) SelectObject(dc, font);
        for (i = 0; i < NOTE_W * NOTE_H; i++) bits[i] = 0x00101820;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, 0x00ffffff);
        DrawTextA(dc, note_text, -1, &r, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
        GdiFlush();
        if (!FAILED(IDirect3DTexture9_LockRect(note_tex, 0, &lr, NULL, D3DLOCK_DISCARD))) {
            for (y = 0; y < NOTE_H; y++) {
                uint32_t *d = (uint32_t *)((uint8_t *)lr.pBits + (size_t)y * lr.Pitch);
                for (i = 0; i < NOTE_W; i++) d[i] = 0xd8000000u | (bits[y * NOTE_W + i] & 0xffffff);
            }
            IDirect3DTexture9_UnlockRect(note_tex, 0);
            note_drawn = 1;
        }
        if (font) DeleteObject(font);
        DeleteObject(bm);
        DeleteDC(dc);
        if (!note_drawn) return;
    }
    memset(&o, 0, sizeof(o));
    o.x0 = 8; o.y0 = 8; o.x1 = 8 + NOTE_W; o.y1 = 8 + NOTE_H; o.u1 = o.v1 = 1; o.l2 = o.l3 = 1;
    set_states(dev);
    if (FAILED(IDirect3DVertexBuffer9_Lock(vb, 0, 6 * sizeof(vert), (void **)&v, D3DLOCK_DISCARD))) return;
    put_quad(v, &o);
    IDirect3DVertexBuffer9_Unlock(vb);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, 1);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)note_tex);
    IDirect3DDevice9_SetPixelShader(dev, sh_plain);
    IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, 2);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_ONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, 0);
    IDirect3DDevice9_SetTexture(dev, 0, NULL);
    IDirect3DDevice9_EndScene(dev);
}

/* ---- the zoomed map: its target, and how it gets onto the screen ---- */
/* one rectangle over the whole of the target in use, from the whole of a texture, through a shader */
static void whole_quad(IDirect3DDevice9 *dev, IDirect3DTexture9 *tex, IDirect3DPixelShader9 *sh) {
    gop o;
    vert *v;
    memset(&o, 0, sizeof(o));
    o.x1 = (float)obj_w; o.y1 = (float)obj_h; o.u1 = o.v1 = 1; o.l2 = o.l3 = 1;
    update_palette();
    set_states(dev);
    if (FAILED(IDirect3DVertexBuffer9_Lock(vb, 0, 6 * sizeof(vert), (void **)&v, D3DLOCK_DISCARD))) return;
    put_quad(v, &o);
    IDirect3DVertexBuffer9_Unlock(vb);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, 0);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)tex);
    IDirect3DDevice9_SetTexture(dev, 1, (IDirect3DBaseTexture9 *)pal_tex);
    IDirect3DDevice9_SetPixelShader(dev, sh);
    IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, 2);
    IDirect3DDevice9_SetTexture(dev, 0, NULL);
    IDirect3DDevice9_EndScene(dev);
}

/* the map's frame and Direct3D objects for a map of w x h (the screen's set in place); 0: not to be had */
static int map_make(IDirect3DDevice9 *dev, int w, int h) {
    D3DCAPS9 caps;
    HRESULT hr = 0;
    const char *what = "";
    if (zmap.rt_tex && zs_tex && zmap.frame && zmap.w == w && zmap.h == h) return 1;
    if (zm_deny_w && w >= zm_deny_w && h >= zm_deny_h) return 0;
    map_release();
    memset(&caps, 0, sizeof(caps));
    IDirect3DDevice9_GetDeviceCaps(dev, &caps);
    if (!sh_avg[2] || !sh_avg[3] || !sh_avg[4] || (DWORD)w > caps.MaxTextureWidth || (DWORD)h > caps.MaxTextureHeight) {
        if (!zm_deny_w) say("zoom: no map target of %dx%d on this graphics device (textures up to %lux%lu%s)", w, h, caps.MaxTextureWidth,
                            caps.MaxTextureHeight, sh_avg[4] ? "" : "; no pixel shader 3.0");
        zm_deny_w = w; zm_deny_h = h;
        return 0;
    }
    if (!zmap.frame || zmap.w != w || zmap.h != h) {
        const size_t size = ((size_t)w * h + 4095) / 4096 * 4096;
        if (zmap.frame) VirtualFree(zmap.frame, 0, MEM_RELEASE);
        free(zmap.ww_addr); free(zmap.ref);
        memset(&zmap, 0, sizeof(zmap));
        zmap.frame = (uint8_t *)VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
        zmap.ww_cap = size / 4096;
        zmap.ww_addr = (void **)malloc(zmap.ww_cap * sizeof(void *));
        zmap.ref = opt_check && zmap.frame ? (uint8_t *)calloc(size, 1) : NULL;
        if (!zmap.frame || !zmap.ww_addr || (opt_check && !zmap.ref)) {
            say("zoom: no memory for a %dx%d map frame (error %lu)", w, h, GetLastError());
            if (zmap.frame) VirtualFree(zmap.frame, 0, MEM_RELEASE);
            free(zmap.ww_addr); free(zmap.ref);
            memset(&zmap, 0, sizeof(zmap));
            zm_deny_w = w; zm_deny_h = h;
            return 0;
        }
        zmap.w = w; zmap.h = h; zmap.size = size;
        memset(zmap.frame, sent, size);
        ResetWriteWatch(zmap.frame, size);
    }
#define TRY(x, name) if (!FAILED(hr)) { hr = (x); if (FAILED(hr)) what = name; }
    TRY(IDirect3DDevice9_CreateTexture(dev, w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &zmap.rt_tex, NULL), "map target");
    TRY(IDirect3DDevice9_CreateTexture(dev, w, h, 1, 0, D3DFMT_L8, D3DPOOL_SYSTEMMEM, &zmap.ov_sys, NULL), "map layer staging");
    TRY(IDirect3DDevice9_CreateTexture(dev, w, h, 1, 0, D3DFMT_L8, D3DPOOL_DEFAULT, &zmap.ov_tex, NULL), "map layer texture");
    TRY(IDirect3DDevice9_CreateTexture(dev, obj_w, obj_h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &zs_tex, NULL), "true-colour target");
    if (opt_check) TRY(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, w, h, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &zmap.chk_surf, NULL), "check surface");
#undef TRY
    if (FAILED(hr)) {
        say("zoom: no %s for a %dx%d map (%08lx)", what, w, h, (unsigned long)hr);
        map_release();
        zm_deny_w = w; zm_deny_h = h;
        return 0;
    }
    IDirect3DTexture9_GetSurfaceLevel(zmap.rt_tex, 0, &zmap.rt_lvl);
    IDirect3DTexture9_GetSurfaceLevel(zmap.ov_sys, 0, &zmap.ov_sys_lvl);
    IDirect3DTexture9_GetSurfaceLevel(zmap.ov_tex, 0, &zmap.ov_lvl);
    IDirect3DTexture9_GetSurfaceLevel(zs_tex, 0, &zs_lvl);
    IDirect3DDevice9_ColorFill(dev, zmap.rt_lvl, NULL, 0);
    IDirect3DDevice9_ColorFill(dev, zs_lvl, NULL, 0);
    zmap.dirty = 1;
    say("zoom: map target made, %dx%d (%u MB on the graphics card, %u MB of memory)", w, h, (unsigned)(((size_t)w * h * 5 + (size_t)obj_w * obj_h * 4) >> 20),
        (unsigned)(((size_t)w * h * (opt_check ? 7 : 2)) >> 20));
    return 1;
}

/* the screen's picture so far into the true-colour target (the zoomed frame starts there) */
static void screen_to_zs(IDirect3DDevice9 *dev) {
    IDirect3DSurface9 *bb = NULL;
    set_states(dev);
    IDirect3DDevice9_GetRenderTarget(dev, 0, &bb);
    IDirect3DDevice9_SetRenderTarget(dev, 0, zs_lvl);
    if (rt_used) whole_quad(dev, rt_tex, sh_final);
    else IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
    if (bb) { IDirect3DDevice9_SetRenderTarget(dev, 0, bb); IDirect3DSurface9_Release(bb); }
}

/* how the map lies on the screen (the zoom module's words: zoom_math.h's): map point bx, by is at screen point ox, oy,
 * `zoom` screen pixels a map pixel, shown within x1, y1 .. x2, y2 */
typedef struct { int size; double zoom; int bx, by, ox, oy, x1, y1, x2, y2, smooth; } wkd3d_view;

static int map_to_screen(IDirect3DDevice9 *dev, const wkd3d_view *v) {
    const double z = v->zoom;
    const float w = (float)zmap.w, h = (float)zmap.h;   /* (the map's set is back in zmap) */
    int x1 = v->x1 < 0 ? 0 : v->x1, y1 = v->y1 < 0 ? 0 : v->y1, x2 = v->x2 > obj_w ? obj_w : v->x2, y2 = v->y2 > obj_h ? obj_h : v->y2;
    int n = z >= 1.0 ? 2 : (int)ceil(1.0 / z - 0.02);
    IDirect3DPixelShader9 *sh;
    IDirect3DSurface9 *bb = NULL;
    float c[4] = {0, 0, 0, 0};
    gop o;
    vert *q;
    if (x2 <= x1 || y2 <= y1 || !zmap.rt_tex || !zs_lvl) return 0;
    n = n <= 2 ? 2 : n == 3 ? 3 : n == 4 ? 4 : n <= 6 ? 6 : 8;   /* samples each way: one for every map pixel a screen pixel covers */
    while (n > 2 && !sh_avg[n]) n = n == 8 ? 6 : n == 6 ? 4 : n - 1;
    sh = v->smooth ? sh_avg[n] : sh_rgb;
    zm_zoom = z; zm_taps = v->smooth ? n : 1;
    memset(&o, 0, sizeof(o));
    o.col = COL_OPAQUE;
    o.x0 = (float)x1; o.y0 = (float)y1; o.x1 = (float)x2; o.y1 = (float)y2;
    o.u0 = (float)((v->bx + (x1 - v->ox) / z) / w); o.u1 = (float)((v->bx + (x2 - v->ox) / z) / w);
    o.v0 = (float)((v->by + (y1 - v->oy) / z) / h); o.v1 = (float)((v->by + (y2 - v->oy) / z) / h);
    o.l0 = 0.5f / w; o.l1 = 0.5f / h; o.l2 = (zm_rw - 0.5f) / w; o.l3 = (zm_rh - 0.5f) / h;
    c[0] = (float)(1.0 / z / w); c[1] = (float)(1.0 / z / h);
    update_palette();
    set_states(dev);
    if (FAILED(IDirect3DVertexBuffer9_Lock(vb, 0, 6 * sizeof(vert), (void **)&q, D3DLOCK_DISCARD))) return 0;
    put_quad(q, &o);
    IDirect3DVertexBuffer9_Unlock(vb);
    IDirect3DDevice9_GetRenderTarget(dev, 0, &bb);
    IDirect3DDevice9_SetRenderTarget(dev, 0, zs_lvl);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, 0);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)zmap.rt_tex);
    IDirect3DDevice9_SetTexture(dev, 1, (IDirect3DBaseTexture9 *)pal_tex);
    IDirect3DDevice9_SetPixelShaderConstantF(dev, 1, c, 1);
    IDirect3DDevice9_SetPixelShader(dev, sh);
    IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, 2);
    IDirect3DDevice9_SetTexture(dev, 0, NULL);
    IDirect3DDevice9_EndScene(dev);
    if (bb) { IDirect3DDevice9_SetRenderTarget(dev, 0, bb); IDirect3DSurface9_Release(bb); }
    st.draws++;
    return 1;
}

/* a zoomed frame's true-colour picture into the frame WA reads (a screenshot), as the palette's nearest colours */
static int fetch_zs(IDirect3DDevice9 *dev) {
    static uint8_t pal_was[1024];
    static uint16_t *lut;                   /* by 5 bits of red, green and blue: the nearest index (0xffff: not worked out yet) */
    const uint8_t *pal = (const uint8_t *)VA(PAL_VA);
    D3DLOCKED_RECT lr;
    int x, y, i;
    if (!zs_lvl || !my_frame) return 0;
    if (!zs_sys && FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, obj_w, obj_h, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &zs_sys, NULL))) { zs_sys = NULL; return 0; }
    if (!lut || memcmp(pal_was, pal, sizeof(pal_was))) {
        if (!lut) lut = (uint16_t *)malloc(32768 * sizeof(uint16_t));
        if (!lut) return 0;
        memset(lut, 0xff, 32768 * sizeof(uint16_t));
        memcpy(pal_was, pal, sizeof(pal_was));
    }
    if (FAILED(IDirect3DDevice9_GetRenderTargetData(dev, zs_lvl, zs_sys)) || FAILED(IDirect3DSurface9_LockRect(zs_sys, &lr, NULL, D3DLOCK_READONLY))) return 0;
    for (y = 0; y < my_h && y < obj_h; y++) {
        const uint32_t *row = (const uint32_t *)((const uint8_t *)lr.pBits + (size_t)y * lr.Pitch);
        uint8_t *to = my_frame + (size_t)y * my_w;
        for (x = 0; x < my_w && x < obj_w; x++) {
            const uint32_t px = row[x];
            const int r = (px >> 16) & 255, g = (px >> 8) & 255, b = px & 255, k = (r >> 3) << 10 | (g >> 3) << 5 | b >> 3;
            if (lut[k] == 0xffff) {
                const int cr = (r & 0xf8) | 4, cg = (g & 0xf8) | 4, cb = (b & 0xf8) | 4;
                long best = 1L << 30;
                for (i = 0; i < 256; i++) {
                    const long dr = pal[4 * i] - cr, dg = pal[4 * i + 1] - cg, db = pal[4 * i + 2] - cb, d = dr * dr + dg * dg + db * db;
                    if (d < best) { best = d; lut[k] = (uint16_t)i; }
                }
            }
            to[x] = (uint8_t)lut[k];
        }
    }
    IDirect3DSurface9_UnlockRect(zs_sys);
    return 1;
}

/* the pictures and triangles other modules asked for this frame, over the finished picture (the back buffer) */
static void draw_overlays(IDirect3DDevice9 *dev) {
    if (n_tri && tri && sh_colour) {
        set_states(dev);
        IDirect3DDevice9_BeginScene(dev);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, 1);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        IDirect3DDevice9_SetTexture(dev, 0, NULL);
        IDirect3DDevice9_SetPixelShader(dev, sh_colour);
        IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, (UINT)(n_tri / 3), tri, sizeof(tvert));
        IDirect3DDevice9_SetFVF(dev, FVF);  /* (drawing from memory leaves no vertex buffer set) */
        IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(vert));
        IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_ONE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, 0);
        IDirect3DDevice9_EndScene(dev);
    }
    if (n_ovl && ovl_px && sh_plain) {
        D3DLOCKED_RECT lr;
        const int rows = ovl_y + ovl_shelf;
        vert *v;
        int i, y;
        if (!ovl_tex && FAILED(IDirect3DDevice9_CreateTexture(dev, OVL_W, OVL_H, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &ovl_tex, NULL))) ovl_tex = NULL;
        if (ovl_tex && !FAILED(IDirect3DTexture9_LockRect(ovl_tex, 0, &lr, NULL, D3DLOCK_DISCARD))) {
            for (y = 0; y < rows; y++) memcpy((uint8_t *)lr.pBits + (size_t)y * lr.Pitch, ovl_px + (size_t)y * OVL_W, OVL_W * 4);
            IDirect3DTexture9_UnlockRect(ovl_tex, 0);
            set_states(dev);
            if (!FAILED(IDirect3DVertexBuffer9_Lock(vb, 0, (UINT)(n_ovl * 6 * sizeof(vert)), (void **)&v, D3DLOCK_DISCARD))) {
                for (i = 0; i < n_ovl; i++) {
                    gop o;
                    memset(&o, 0, sizeof(o));
                    o.x0 = (float)ovl[i].x; o.y0 = (float)ovl[i].y; o.x1 = (float)(ovl[i].x + ovl[i].w); o.y1 = (float)(ovl[i].y + ovl[i].h);
                    o.u0 = (float)ovl[i].ax / OVL_W; o.v0 = (float)ovl[i].ay / OVL_H;
                    o.u1 = (float)(ovl[i].ax + ovl[i].w) / OVL_W; o.v1 = (float)(ovl[i].ay + ovl[i].h) / OVL_H;
                    o.l2 = o.l3 = 1;
                    put_quad(v + 6 * i, &o);
                }
                IDirect3DVertexBuffer9_Unlock(vb);
                IDirect3DDevice9_BeginScene(dev);
                IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, 1);   /* (premultiplied: one, 1 - alpha) */
                IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)ovl_tex);
                IDirect3DDevice9_SetPixelShader(dev, sh_plain);
                IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, (UINT)(n_ovl * 2));
                IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, 0);
                IDirect3DDevice9_SetTexture(dev, 0, NULL);
                IDirect3DDevice9_EndScene(dev);
            }
        }
    }
    n_ovl = n_tri = 0; ovl_x = ovl_y = ovl_shelf = 0;
}

/* ---- what other modules can ask for (exported; all on WA's own thread, while WA draws a frame) ---- */
#define WKD3D_API __declspec(dllexport)
static int drawing_now(void) { return mode == ACTIVE && !frame_abort && !in_ref && obj_dev && my_frame && (zm_on || mine_now()); }

WKD3D_API int __cdecl wkDirect3D_Api(void) { return 1; }

/* The largest map (see MapBegin) that can be drawn now. 0: none (this module is not drawing, or the graphics device
 * cannot do it) */
WKD3D_API int __cdecl wkDirect3D_MapMax(int *max_w, int *max_h) {
    D3DCAPS9 caps;
    int w, h;
    if (mode != ACTIVE || !obj_dev || !sh_avg[2] || !sh_avg[3] || !sh_avg[4] || test_zoom != 1.0) return 0;
    memset(&caps, 0, sizeof(caps));
    IDirect3DDevice9_GetDeviceCaps(obj_dev, &caps);
    w = (int)caps.MaxTextureWidth; h = (int)caps.MaxTextureHeight;
    if (zm_deny_w && w >= zm_deny_w) w = zm_deny_w - 2;
    if (zm_deny_h && h >= zm_deny_h) h = zm_deny_h - 2;
    if (w < obj_w || h < obj_h) return 0;
    if (max_w) *max_w = w;
    if (max_h) *max_h = h;
    return 1;
}

/* The map's picture starts empty each frame (index 0, as WA's own frame is black where nothing is drawn). Past the
 * world's edges WA draws no sky and no water, so without this the picture of earlier frames stayed there: waves and sea
 * of other zooms and camera places, heaped up (the owner's "cursed" edges, 2026-11-24). The check's own picture too. */
static void map_clear(IDirect3DDevice9 *dev, int rw, int rh) {
    IDirect3DSurface9 *bb = NULL;
    D3DRECT r;
    int y;
    r.x1 = 0; r.y1 = 0; r.x2 = rw; r.y2 = rh;
    set_states(dev);
    IDirect3DDevice9_GetRenderTarget(dev, 0, &bb);
    IDirect3DDevice9_SetRenderTarget(dev, 0, rt_lvl);
    IDirect3DDevice9_Clear(dev, 1, &r, D3DCLEAR_TARGET, 0, 1.0f, 0);
    if (bb) { IDirect3DDevice9_SetRenderTarget(dev, 0, bb); IDirect3DSurface9_Release(bb); }
    if (ref) for (y = 0; y < rh && y < my_h; y++) memset(ref + (size_t)y * my_w, 0, (size_t)(rw < my_w ? rw : my_w));
}

/* A zoomed frame's map part begins. The caller then makes WA's display and frame describe a map of rw x rh pixels in a
 * frame of `stride` bytes a row and `rows` rows (the largest map it will ever ask for: the same every frame), at the
 * address this returns, and has WA draw the map into it at WA's own scale. NULL: not this frame (draw it unzoomed). */
WKD3D_API void *__cdecl wkDirect3D_MapBegin(int stride, int rows, int rw, int rh) {
    IDirect3DDevice9 *dev;
    if (zm_on || in_ref || read_filled || test_zoom != 1.0 || frame_abort || !mine_now()) return NULL;
    if (stride < 64 || rows < 64 || rw < 1 || rh < 1 || rw > stride || rh > rows) return NULL;
    dev = device_now();
    if (!dev || !map_make(dev, stride, rows)) return NULL;
    if (cpu_maybe) { take_layer(dev, 0); cpu_maybe = 0; }   /* what is drawn so far lies under the map */
    draw_ops(dev);
    if (frame_abort) return NULL;
    if (!zframe) { screen_to_zs(dev); zframe = 1; }
    target_swap();
    frame_clean();
    zm_rw = rw; zm_rh = rh;
    map_clear(dev, rw, rh);
    cpu_maybe = 0;
    return my_frame;
}

/* The map part is drawn: it is put on the screen as `view` says. What WA draws after this (the caller has given the
 * display and the frame back first or does so now) lies over it at the screen's own scale. 0: it could not be done
 * (this frame is lost: WA shows what it has) */
WKD3D_API int __cdecl wkDirect3D_MapEnd(const wkd3d_view *view) {
    IDirect3DDevice9 *dev;
    int ok = 0;
    if (!zm_on) return 0;
    dev = frame_abort ? NULL : device_now();
    if (dev) { take_layer(dev, 1); draw_ops(dev); }
    if (dev && !frame_abort && ref && chk_surf && opt_check && frames % (unsigned)opt_check == 0) check_frame(dev);
    target_swap();
    cpu_maybe = 0;
    if (dev && !frame_abort && view && view->size >= (int)sizeof(*view) && view->zoom > 0.01) ok = map_to_screen(dev, view);
    if (!ok) { frame_abort = 1; zmap.dirty = 1; }
    return ok;
}

/* A true-colour picture laid over the finished picture of the frame being drawn: premultiplied A8R8G8B8, h rows of
 * pitch_bytes bytes, its top left at screen point x, y. The pixels are copied. 1: it will be drawn; 0: this module is
 * not drawing this frame (or there is no room left this frame): draw it yourself. */
WKD3D_API int __cdecl wkDirect3D_Overlay(const uint32_t *argb, int w, int h, int pitch_bytes, int x, int y) {
    int r;
    if (!drawing_now() || !argb || w < 1 || h < 1 || w > OVL_W || pitch_bytes < w * 4 || n_ovl == OVL_MAX) return 0;
    if (!ovl_px) ovl_px = (uint32_t *)malloc((size_t)OVL_W * OVL_H * 4);
    if (!ovl_px) return 0;
    if (ovl_x + w > OVL_W) { ovl_y += ovl_shelf; ovl_x = 0; ovl_shelf = 0; }
    if (ovl_y + h > OVL_H) return 0;
    for (r = 0; r < h; r++) memcpy(ovl_px + (size_t)(ovl_y + r) * OVL_W + ovl_x, (const uint8_t *)argb + (size_t)r * pitch_bytes, (size_t)w * 4);
    ovl[n_ovl].x = x; ovl[n_ovl].y = y; ovl[n_ovl].w = w; ovl[n_ovl].h = h; ovl[n_ovl].ax = ovl_x; ovl[n_ovl].ay = ovl_y;
    n_ovl++;
    ovl_x += w;
    if (h > ovl_shelf) ovl_shelf = h;
    return 1;
}

/* Triangles laid over the finished picture the same way: `count` vertices (three a triangle) in screen pixels, each
 * with a colour of its own (A8R8G8B8, alpha not premultiplied), blended in. 1 / 0 as above; at most 190000 vertices a
 * frame (what is over is left out). */
typedef struct { float x, y; uint32_t argb; } wkd3d_vtx;
WKD3D_API int __cdecl wkDirect3D_Triangles(const wkd3d_vtx *v, int count) {
    int i;
    if (!drawing_now() || !v || count < 3 || !sh_colour) return 0;
    count -= count % 3;
    if (n_tri + count > TRI_MAX) count = (TRI_MAX - n_tri) / 3 * 3;
    if (count <= 0) return 1;
    if (n_tri + count > tri_cap) {
        const int cap = n_tri + count < 3072 ? 3072 : (n_tri + count) * 2 > TRI_MAX ? TRI_MAX : (n_tri + count) * 2;
        tvert *t = (tvert *)realloc(tri, (size_t)cap * sizeof(tvert));
        if (!t) return 0;
        tri = t; tri_cap = cap;
    }
    for (i = 0; i < count; i++) {
        tri[n_tri + i].x = v[i].x - 0.5f; tri[n_tri + i].y = v[i].y - 0.5f; tri[n_tri + i].z = 0; tri[n_tri + i].rhw = 1;
        tri[n_tri + i].col = v[i].argb;
    }
    n_tri += count;
    return 1;
}

/* wkMapZoom.ini was changed in the game (wkModMenu): GraphicsCard read again */
static char ini_file[MAX_PATH + 16];
void gpu_settings_changed(void) {
    const int was = user_off;
    if (!ini_file[0]) return;
    user_off = !GetPrivateProfileIntA("wkMapZoom", "GraphicsCard", 1, ini_file);
    if (user_off != was) say("GraphicsCard read again: %s", user_off ? "off: WA draws by itself from the next frame on" : "on");
}

/* WA's show-frame and whatever other modules hung on it (wkMapZoom: its safety net before, its dial after), for a frame
 * the GPU has drawn already: WA's step is given a 1 x 1 frame to upload and a vertex buffer whose points are all one
 * point, so it costs next to nothing and draws nothing; the others' work happens as ever. */
static uint32_t *ghost_show(IDirect3DDevice9 *dev, void *be, uint32_t *result, void *frame) {
    const uintptr_t b = (uintptr_t)be;
    const uint32_t tex = P32(b + 0x7c), w = P32(b + BE_W), h = P32(b + BE_H);
    IDirect3DVertexBuffer9 *vb0 = NULL;
    UINT off0 = 0, stride0 = 0;
    uint32_t *r;
    restore_states();
    IDirect3DDevice9_GetStreamSource(dev, 0, &vb0, &off0, &stride0);
    if (stride0 && (!ghost_vb || ghost_stride != stride0)) {
        uint8_t *v;
        UINT i;
        REL(ghost_vb);
        ghost_stride = 0;
        if (!FAILED(IDirect3DDevice9_CreateVertexBuffer(dev, 8 * stride0, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &ghost_vb, NULL)) &&
            !FAILED(IDirect3DVertexBuffer9_Lock(ghost_vb, 0, 0, (void **)&v, 0))) {
            memset(v, 0, 8 * stride0);
            if (stride0 >= 16) for (i = 0; i < 8; i++) { const float one = 1.0f; memcpy(v + i * stride0 + 12, &one, 4); }
            IDirect3DVertexBuffer9_Unlock(ghost_vb);
            ghost_stride = stride0;
        } else REL(ghost_vb);
    }
    if (!ghost_tex && FAILED(IDirect3DDevice9_CreateTexture(dev, 1, 1, 1, D3DUSAGE_DYNAMIC, D3DFMT_L8, D3DPOOL_DEFAULT, &ghost_tex, NULL))) ghost_tex = NULL;
    if (!tex || !ghost_tex || !ghost_vb || !vb0) {   /* (not to be had: this frame's other-module work is left out) */
        if (vb0) IDirect3DVertexBuffer9_Release(vb0);
        *result = P32(VA(BE_OK_VA));
        return result;
    }
    P32(b + 0x7c) = (uint32_t)(uintptr_t)ghost_tex; P32(b + BE_W) = 1; P32(b + BE_H) = 1;
    IDirect3DDevice9_SetStreamSource(dev, 0, ghost_vb, 0, stride0);
    r = (ghost_direct ? (show_fn)VA(0x59cde0) : orig_show)(be, result, frame);
    P32(b + 0x7c) = tex; P32(b + BE_W) = w; P32(b + BE_H) = h;
    IDirect3DDevice9_SetStreamSource(dev, 0, vb0, off0, stride0);
    IDirect3DVertexBuffer9_Release(vb0);
    return r;
}

static unsigned long long hist[256];
static unsigned hist_frames;
static void stats_line(void) {
    const LONGLONG t = now();
    const double dt = (double)(t - st_since) / (double)qpf.QuadPart, f = st.frames ? st.frames : 1, ms = 1000.0 / qpf.QuadPart / f;
    if (dt < 1.0) return;
    if (opt_stats && st.frames)
        say("stats: %.0f fps, %u%% by the GPU | a frame: %.1f rectangles in %.1f draws, %.1f layers of %.1f pieces from %.1f written pages, "
            "%.1f KB layer upload, %.1f KB textures | ms a frame: layers %.3f (watch %.3f scan %.3f copy %.3f update %.3f) draws %.3f show %.3f | "
            "%u frames drawn elsewhere, textures %u MB in %d pages",
            st.frames / dt, st.gpu * 100 / st.frames, st.ops / f, st.draws / f, st.flushes / f, st.runs / f, st.pages / f, st.ov_bytes / 1024.0 / f,
            st.tex_bytes / 1024.0 / f, st.t_flush * ms, st.t_ww * ms, st.t_scan * ms, st.t_copy * ms, st.t_up * ms, st.t_draw * ms, st.t_show * ms,
            st.foreign, (unsigned)(tex_mem >> 20), n_pages);
    if (opt_stats && st.frames && (sp_stat.gpu || sp_stat.left)) {
        char b[400];
        int n = 0, k, o, tb;
        b[0] = 0;
        for (k = 0; k < 4; k++) for (o = 0; o < 16; o++) for (tb = 0; tb < 2; tb++)
            if (sp_stat.combo[k][o][tb] && n < 360) n += sprintf(b + n, " kind %d turned %d%s: %.1f;", k, o, tb ? " with a table" : "", sp_stat.combo[k][o][tb] / f);
        say("stats: sprites a frame: %.1f by the GPU (%.1f KB copied), %.1f left to WA%s%s (%.1f fetches of the picture under one)", sp_stat.gpu / f,
            sp_stat.pixels / 1024.0 / f, sp_stat.left / f, n ? ":" : "", b, n_fetch / f);
        n_fetch = 0;
    }
    memset(&sp_stat, 0, sizeof(sp_stat));
    memset(&st, 0, sizeof(st));
    st_since = t;
}

/* the backend's "show frame" (vtable slot 12; ECX the backend, EDX the result, the 8-bit frame; ret 4): a frame the GPU
 * drew is finished here (the last layer, then the index target through the palette onto the back buffer) and WA's own
 * upload and quad are left out; any other frame is WA's as ever */
static uint32_t *__attribute__((fastcall)) hook_show(void *be, uint32_t *result, void *frame) {
    const LONGLONG t0 = now();
    uint32_t *r;
    frames++;
    st.frames++;
    frame_started = 0;
    if (zm_on) { target_swap(); zmap.dirty = 1; frame_abort = 1; }   /* (a map part that was never ended) */
    if (opt_trace && frames == 400) { tracing = 1; trace_left = (unsigned)opt_trace; say("trace: %u frames from here (mode %d)", trace_left, mode); }
    if (tracing) { say("show: frame %u mode %d, %d GPU operations, frame %p", frames, mode, frame_ops, frame); if (!trace_left--) tracing = 0; }
    if (mode == ACTIVE) {
        IDirect3DDevice9 *dev = !frame_abort && frame == (void *)my_frame ? device_now() : NULL;
        if (dev && !frame_ops) {            /* nothing for the GPU: did anyone draw at all? (a frame WA does not redraw is
                                             * shown again as it is; one drawn elsewhere is whole in the frame) */
            ULONG_PTR n = ww_cap, i;
            DWORD gran = 0;
            if (GetWriteWatch(0, my_frame, my_size, ww_addr, &n, &gran)) dev = NULL;
            else for (i = 0; i < n && dev; i++) if (first_set((const uint8_t *)ww_addr[i], 4096) >= 0) dev = NULL;
        }
        if (dev) {
            take_layer(dev, 1);
            draw_ops(dev);
        }
        if (dev && !frame_abort && ref && chk_surf && test_zoom == 1.0 && !zframe && frames % (unsigned)opt_check == 0) check_frame(dev);
        if (dev && !frame_abort) {
            if (zframe) whole_quad(dev, zs_tex, sh_plain);   /* a zoomed frame: its true-colour picture as it is */
            else if (test_zoom == 1.0) {
                gop o;
                vert *v;
                memset(&o, 0, sizeof(o));
                o.x1 = (float)my_w; o.y1 = (float)my_h; o.u1 = o.v1 = 1; o.l2 = o.l3 = 1;
                update_palette();
                set_states(dev);
                if (!FAILED(IDirect3DVertexBuffer9_Lock(vb, 0, 6 * sizeof(vert), (void **)&v, D3DLOCK_DISCARD))) {
                    put_quad(v, &o);
                    IDirect3DVertexBuffer9_Unlock(vb);
                    IDirect3DDevice9_BeginScene(dev);
                    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, 0);
                    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)rt_tex);
                    IDirect3DDevice9_SetTexture(dev, 1, (IDirect3DBaseTexture9 *)pal_tex);
                    IDirect3DDevice9_SetPixelShader(dev, sh_final);
                    IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, 2);
                    IDirect3DDevice9_SetTexture(dev, 0, NULL);
                    IDirect3DDevice9_EndScene(dev);
                }
            } else update_palette();         /* (zoomed test frames: drawn through the palette as it was) */
            draw_overlays(dev);
            draw_notice(dev);
            gpu_frames++;
            st.gpu++;
            chk_n = 0;
            frame_ops = 0; cpu_maybe = 0; zoom_clear = 1;
            zs_shown = zframe; zframe = 0; rt_used = 0;
            if (opt_stats && zs_shown) {
                static DWORD at;
                static unsigned n;
                n++;
                if (GetTickCount() - at >= 1000) {
                    say("stats: zoomed: %u frames; map part %dx%d of %dx%d at %.1f%%, %d samples a pixel", n, zm_rw, zm_rh, zmap.w, zmap.h, zm_zoom * 100.0, zm_taps * zm_taps);
                    at = GetTickCount(); n = 0;
                }
            }
            cooldown_next = 2;
            if (show_chain) r = ghost_show(dev, be, result, frame);
            else { *result = P32(VA(BE_OK_VA)); r = result; }
            /* A frame that came almost whole as leftover drawing was drawn by someone else (a zoom module that draws
             * the picture itself and has only the fixed parts drawn by WA): WA's own upload shows such frames cheaper
             * than a layer does. After some in a row the module stands aside for a while, longer each time. */
            if ((size_t)frame_layer_px * 2 > (size_t)my_w * my_h) { heavy++; light = 0; }
            else { heavy = 0; if (++light == 2000) heavy_wait = 64; }
            frame_layer_px = 0;
            if (user_off) go_passive("switched off in the game (wkMapZoom.ini, GraphicsCard=0): WA draws by itself until it is switched on again");
            if (heavy >= 8) {
                go_passive(heavy_told ? NULL : "the picture comes almost whole from elsewhere (another module draws the map): WA shows such frames itself; tried again now and then");
                heavy_told = 1;
                heavy = 0;
                cooldown = heavy_wait;
                if (heavy_wait < 512) heavy_wait *= 2;
            }
            st.t_show += now() - t0;
            stats_line();
            return r;
        }
        /* not the GPU's frame: nothing reached the module (another module drew the map: the frame is whole), or it
         * could not be finished (WA shows what there is; the next frame is WA's own again) */
        if (frame_ops) say("frame %u could not be finished on the GPU (%d operations): shown as far as WA drew it", frames, frame_ops);
        zs_shown = 0; n_ovl = n_tri = 0;
        st.foreign++;
        restore_states();
        r = orig_show(be, result, frame);
        go_passive(NULL);
        cooldown = cooldown_next;
        if (cooldown_next < 64) cooldown_next *= 2;
        passive_blits = 0;
        stats_line();
        return r;
    }
    if (opt_hist && (frames & 7) == 0 && frame && PI32(VA(FRAME_W_VA)) > 0) {
        const uint8_t *p = (const uint8_t *)frame;
        const size_t n = (size_t)PI32(VA(FRAME_W_VA)) * PI32(VA(FRAME_H_VA));
        size_t i;
        for (i = 0; i < n; i++) hist[p[i]]++;
        if (++hist_frames % 200 == 0) {
            char b[900];
            int k = 0, j;
            for (j = 0; j < 256 && k < 880; j++) if (!hist[j]) k += sprintf(b + k, "%d ", j);
            say("palette indices never seen in %u of WA's frames: %s", hist_frames, k ? b : "none");
        }
    }
    restore_states();
    r = orig_show(be, result, frame);
    last_wa_frame = (uintptr_t)frame;
    if (cooldown) cooldown--;
    else if (!opt_off && !user_off && !failed && passive_blits && frame && *result == P32(VA(BE_OK_VA)) && (!read_seen || frames - read_seen > 120)) {
        const int w = PI32(VA(FRAME_W_VA)), h = PI32(VA(FRAME_H_VA));
        IDirect3DDevice9 *dev = (IDirect3DDevice9 *)(uintptr_t)P32((uintptr_t)be + BE_DEV);
        if (dev && !*(uint8_t *)((uintptr_t)be + BE_LOST) && w == PI32((uintptr_t)be + BE_W) && h == PI32((uintptr_t)be + BE_H) &&
            P32(VA(FRAME_PTR_VA)) == (uint32_t)(uintptr_t)frame && w >= 320 && h >= 200) {
            if (!frame_alloc(w, h) || !make_objects(dev, w, h)) { failed = 1; say("WA draws by itself from here on"); }
            else {
                frame_clean();
                swap_in();
                mode = ACTIVE;
                frame_ops = 0; frame_abort = 0; cpu_maybe = 0; n_ops = 0;
                if (env_int("WKD3D_PALDUMP", 0) && !gpu_frames) {
                    const uintptr_t d = wa_display();
                    char b[900];
                    int i, n, row;
                    for (row = 0; row < 8; row++) {
                        for (i = row * 32, n = 0; i < row * 32 + 32; i++) n += sprintf(b + n, "%02x%02x%02x.%x/%x ", *(uint8_t *)(VA(PAL_VA) + 4 * i), *(uint8_t *)(VA(PAL_VA) + 4 * i + 1),
                                                                             *(uint8_t *)(VA(PAL_VA) + 4 * i + 2), *(uint8_t *)(VA(PAL_VA) + 4 * i + 3), d ? *(uint8_t *)(d + 0x358d + 4 * i + 3) : 0);
                        say("palette %3d: %s", row * 32, b);
                    }
                }
                if (!gpu_frames) say("active: the GPU draws the map from frame %u on (%dx%d, \"nothing\" index %d%s)", frames + 1, w, h, sent,
                                     test_zoom != 1.0 ? ", zoom test" : "");
            }
        }
    }
    passive_blits = 0;
    st.t_show += now() - t0;
    stats_line();
    return r;
}

/* ---- putting the hooks in, once a game is on the screen ---- */
static int swap_slot(uintptr_t vt_va, int slot, void *mine, void **orig) {
    void **p = (void **)(VA(vt_va) + 4 * slot);
    DWORD old;
    if (!VirtualProtect(p, 4, PAGE_READWRITE, &old)) return 0;
    *orig = *p;
    *p = mine;
    VirtualProtect(p, 4, old, &old);
    return 1;
}

typedef BOOL(WINAPI *handle_ex_fn)(DWORD, LPCSTR, HMODULE *);
static handle_ex_fn handle_ex(void) { return (handle_ex_fn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetModuleHandleExA"); }
static const char *owner_of(const void *p, char *out, DWORD size) {
    const handle_ex_fn ex = handle_ex();
    HMODULE m;
    char path[MAX_PATH];
    const char *s;
    strcpy(out, "?");
    if (ex && ex(0x2 | 0x4, (LPCSTR)p, &m) && GetModuleFileNameA(m, path, sizeof(path))) {
        s = strrchr(path, '\\');
        strncpy(out, s ? s + 1 : path, size - 1);
        out[size - 1] = 0;
    }
    return out;
}

static void look_around(void) {
    static const struct { uint32_t va; const char *name; } code[] = {
        {0x533dc0, "GameRender"}, {0x542350, "the drawing queue"}, {0x56a700, "the bitmap blit"}, {0x5a2790, "the surface blit"},
        {0x59cde0, "show frame"}, {0x5a26c0, "flush"}, {0x56b810, "the display fill"}, {0x4f6910, "the sprite blit"}};
    char names[700], who[64];
    MODULEENTRY32 me;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    unsigned i;
    int k = 0;
    names[0] = 0;
    if (snap != INVALID_HANDLE_VALUE) {
        me.dwSize = sizeof(me);
        if (Module32First(snap, &me)) do {
            if ((!_strnicmp(me.szModule, "wk", 2) || !_stricmp(me.szModule, "gameoverlayrenderer.dll") || !_strnicmp(me.szModule, "action", 6)) && k < 640)
                k += sprintf(names + k, "%s%s", k ? ", " : "", me.szModule);
        } while (Module32Next(snap, &me));
        CloseHandle(snap);
    }
    say("modules loaded: %s", names);
    for (i = 0; i < sizeof(code) / sizeof(code[0]); i++) {
        const uint8_t *at = (const uint8_t *)VA(code[i].va);
        if (*at == 0xe9) say("%s (%08X) is hooked by %s", code[i].name, code[i].va, owner_of(at + 5 + *(const int32_t *)(at + 1), who, sizeof(who)));
    }
}

static void *const disp_thunks[12] = {disp_thunk_0, disp_thunk_1, disp_thunk_2, disp_thunk_3, disp_thunk_4, disp_thunk_5,
                                      disp_thunk_6, disp_thunk_7, disp_thunk_8, disp_thunk_9, disp_thunk_10, disp_thunk_11};
static void *const disp_backs[12] = {disp_back_0, disp_back_1, disp_back_2, disp_back_3, disp_back_4, disp_back_5,
                                     disp_back_6, disp_back_7, disp_back_8, disp_back_9, disp_back_10, disp_back_11};

static void install(void) {
    static const uint32_t disp_wa[12] = {0x56b8c0, 0x56bcc0, 0x56bdb0, 0x56bd50, 0x56be10, 0x56be80, 0x56bfd0, 0x56b810, 0x56b080, 0x56b660, 0x56b7c0, 0x56c5a0};
    char who[64];
    int k, ok = 1;
    installed = 1;
    look_around();
    for (k = 0; k < 12; k++) {
        const uint32_t cur = P32(VA(DISP_VT_VA) + 4 * (11 + k));
        if (cur != VA(disp_wa[k])) say("the display's method %d is %s's already: chained", 11 + k, owner_of((void *)(uintptr_t)cur, who, sizeof(who)));
    }
    if (P32(VA(RC_VT_VA) + 4 * 23) != VA(0x5a2790)) say("the surface blit's table entry is %s's already: chained", owner_of((void *)(uintptr_t)P32(VA(RC_VT_VA) + 4 * 23), who, sizeof(who)));
    {   /* Other modules on WA's show-frame (its table entry, or a jump at the start of its code): wkMapZoom's part (its
         * safety net, its dial) is kept running in the frames this module draws. Any other is overridden in those
         * frames: what it would put on the screen there is left out, and the player is told. Frames that module draws
         * itself are its own as ever. */
        const uintptr_t slot = P32(VA(BE_VT_VA) + 4 * 12);
        const uint8_t *code = (const uint8_t *)VA(0x59cde0);
        const void *theirs[2];
        char other[64] = "";
        int i, keep[2] = {0, 0}, left_out = 0;
        theirs[0] = slot != VA(0x59cde0) ? (const void *)slot : NULL;
        theirs[1] = *code == 0xe9 ? (const void *)(code + 5 + *(const int32_t *)(code + 1)) : NULL;
        for (i = 0; i < 2; i++) if (theirs[i]) {
            owner_of(theirs[i], who, sizeof(who));
            if (!_stricmp(who, "wkMapZoom.dll")) keep[i] = 1;
            else { strncpy(other, !_stricmp(who, "wkLua.dll") ? "WormForge" : who, sizeof(other) - 1); other[sizeof(other) - 1] = 0; }
        }
        if (keep[0]) show_chain = 1;                                        /* (the table's entry: its chain, down to WA's function) */
        else if (keep[1]) { show_chain = 1; ghost_direct = theirs[0] != NULL; }   /* (on WA's function: called past another's table entry) */
        if (show_chain) say("wkMapZoom works on WA's show-frame too: its part is still run each frame, with WA's own upload and quad made into nothing");
        left_out = other[0] && !(keep[0] && theirs[0] && theirs[1] && !keep[1]);   /* (the other's jump inside a kept chain still runs) */
        if (left_out) notify("wkMapZoom draws this game on the graphics card. %s also puts things on the finished picture: that part of it is "
                             "left out meanwhile. (wkMapZoom.ini, GraphicsCard=0, switches this off.)", other);
    }
    if (env_int("WKD3D_TEST_NOTICE", 0)) notify("wkMapZoom draws this game on the graphics card. WormForge also puts things on the finished picture: that part of it is "
                                            "left out meanwhile. (wkMapZoom.ini, GraphicsCard=0, switches this off.)");
    memcpy(disp_back, disp_backs, sizeof(disp_back));
    ok &= swap_slot(SURF_VT_VA, 0, (void *)thunk_surf_dtor, &orig_surf_dtor);
    ok &= swap_slot(SURF_VT_VA, 4, (void *)thunk_surf_unlock, &orig_surf_unlock);
    ok &= swap_slot(SURF_VT_VA, 5, (void *)thunk_surf_alloc, &orig_surf_alloc);
    ok &= swap_slot(SURF_VT_VA, 6, (void *)thunk_surf_free, &orig_surf_free);
    ok &= swap_slot(BE_VT_VA, 16, (void *)thunk_be_free, &orig_be_free);
    ok &= swap_slot(BE_VT_VA, 7, (void *)thunk_be_close, &orig_be_close);
    ok &= swap_slot(RC_VT_VA, 6, (void *)thunk_rc_alloc, &orig_rc_alloc);
    ok &= swap_slot(RC_VT_VA, 7, (void *)thunk_rc_free, &orig_rc_free);
    ok &= swap_slot(RC_VT_VA, 9, (void *)thunk_rc_resize, &orig_rc_resize);
    ok &= swap_slot(RC_VT_VA, 15, (void *)thunk_rc_read, &orig_rc_read);
    ok &= swap_slot(RC_VT_VA, 16, (void *)thunk_rc_read_end, &orig_rc_read_end);
    for (k = 0; k < 12 && ok; k++) ok &= swap_slot(DISP_VT_VA, 11 + k, disp_thunks[k], &disp_orig[k]);
    ok = ok && swap_slot(RC_VT_VA, 19, (void *)hook_fill, (void **)&orig_fill);
    ok = ok && swap_slot(RC_VT_VA, 23, (void *)hook_blit, (void **)&orig_blit);
    ok = ok && swap_slot(BE_VT_VA, 12, (void *)hook_show, (void **)&orig_show);
    if (ok && !env_int("WKD3D_NOSPRITEHOOK", 0)) {   /* the sprite blit's four calls in the display's code */
        static const uint32_t sites[4] = {0x56aede, 0x56afed, 0x56b062, 0x56b709};
        uintptr_t target = 0;
        int good = 1;
        for (k = 0; k < 4; k++) {
            const uint8_t *at = (const uint8_t *)VA(sites[k]);
            const uintptr_t t = (uintptr_t)(at + 5) + (uintptr_t)*(const int32_t *)(at + 1);
            if (at[0] != 0xe8 || (target && t != target)) good = 0;
            target = t;
        }
        if (!good) say("WA's sprite calls are not as in 3.8.1 (another module's?): sprites stay WA's own drawing");
        else {
            if (target != VA(SPRITE_BLIT_VA)) say("WA's sprite calls go to %s already: chained", owner_of((void *)target, who, sizeof(who)));
            orig_sprite = (void *)target;
            for (k = 0; k < 4; k++) {
                uint8_t *at = (uint8_t *)VA(sites[k]);
                DWORD old;
                if (!VirtualProtect(at + 1, 4, PAGE_EXECUTE_READWRITE, &old)) { say("WA's code can't be written (error %lu): some sprites stay WA's", GetLastError()); continue; }
                *(int32_t *)(at + 1) = (int32_t)((uintptr_t)(void *)sprite_thunk - (uintptr_t)(at + 5));
                VirtualProtect(at + 1, 4, old, &old);
                FlushInstructionCache(GetCurrentProcess(), at, 5);
            }
            sprite_hooked = 1;
        }
    }
    st_since = now();
    say(ok ? "ready: WA's function tables point here (the first game frame is on the screen)%s" : "WA's function tables can't be written: some hooks are missing%s",
        opt_off ? "; WKD3D_OFF: never drawing" : "");
}

/* a message for WA's thread (and a timer's, four times a second, until the hooks are in): is a game on the screen? */
static HHOOK msg_hook;
static UINT_PTR timer;
static LRESULT CALLBACK on_message(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && !installed && wa_display() && P32(VA(RC_PTR_VA)) && P32(P32(VA(RC_PTR_VA))) == VA(RC_VT_VA) && P32(VA(FRAME_PTR_VA))) {
        if (timer) { KillTimer(NULL, timer); timer = 0; }
        if (wa_backend()) install();
        else { installed = 1; say("WA is not on its Direct3D 9 (shader) renderer (Options, Advanced, Graphics API): the graphics card drawing does nothing in this run, so there is no zoom"); }
    }
    return CallNextHookEx(msg_hook, code, wp, lp);
}

/* Started by wkMapZoom.c when WA loads the module. 1: the drawing will start with the first game; 0: it does not run
 * (switched off, another copy of it is loaded -- an old wkDirect3D.dll -- or something is missing: the log says) */
int gpu_start(HINSTANCE module, const char *ini) {
    char exe[MAX_PATH], why[256] = "its path is unknown", b[32];
    const handle_ex_fn ex = handle_ex();
    HMODULE m;
    DWORD n;
    self = module;
    open_log();
    QueryPerformanceFrequency(&qpf);
    base = (uintptr_t)GetModuleHandleA(NULL);
    n = GetModuleFileNameA(NULL, exe, sizeof(exe));
    if (!n || n >= sizeof(exe) || !wa_supported_image(exe, why, sizeof(why))) { say("graphics card drawing: not Worms Armageddon 3.8.1 (%s): not started", why); return 0; }
    if (!IsProcessorFeaturePresent(10 /* SSE2 */)) { say("graphics card drawing: this CPU has no SSE2: not started"); return 0; }
    if (ini && strlen(ini) < sizeof(ini_file)) strcpy(ini_file, ini);
    if (ini_file[0] && !GetPrivateProfileIntA("wkMapZoom", "GraphicsCard", 1, ini_file)) {
        say("graphics card drawing: switched off in wkMapZoom.ini (GraphicsCard=0): not started (no zoom; switched on, it starts with WA's next start)");
        return 0;
    }
    {   /* one copy only: the name is the one wkDirect3D.dll (the module this was) takes, so that with an old wkDirect3D.dll
         * still in the folder (it loads first) only that one draws, and wkMapZoom asks it */
        char name[64];
        sprintf(name, "wkDirect3D-loaded-%lu", (unsigned long)GetCurrentProcessId());
        if (!CreateMutexA(NULL, FALSE, name) || GetLastError() == ERROR_ALREADY_EXISTS) {
            say("graphics card drawing: wkDirect3D.dll (the older, separate module) is loaded and draws: this one stays out. Delete wkDirect3D.dll: it is part of wkMapZoom now");
            return 0;
        }
    }
    opt_off = env_int("WKD3D_OFF", 0); opt_stats = env_int("WKD3D_STATS", 0); opt_trace = env_int("WKD3D_TRACE", 0);
    opt_hist = env_int("WKD3D_HIST", 0); sent = env_int("WKD3D_SENT", 0xfd) & 255; opt_ov = env_int("WKD3D_OV", 0);
    opt_reset = env_int("WKD3D_RESET", 16); opt_nosprites = env_int("WKD3D_TEST_NOSPRITES", 0);
    opt_check = env_int("WKD3D_CHECK", 0); opt_strict = env_int("WKD3D_STRICT", 1);
    test_table = env_int("WKD3D_TEST_TABLE", 0); test_kind = env_int("WKD3D_TEST_KIND", 0);
    if (test_table < 0) test_table = 0;
    if (test_kind != 2 && test_kind != 3) test_kind = 0;
    opt_sabotage = env_int("WKD3D_SABOTAGE", 0);
    if (opt_check < 0) opt_check = 0;
    if (opt_reset < 1) opt_reset = 1;
    if (GetEnvironmentVariableA("WKD3D_TEST_ZOOM", b, sizeof(b)) > 0) { test_zoom = atof(b); if (test_zoom < 0.2 || test_zoom > 1.0) test_zoom = 1.0; }
    if (!ex || !ex(0x1 | 0x4, (LPCSTR)(void *)&gpu_start, &m)) { say("graphics card drawing: could not pin the module in memory (error %lu): not started", GetLastError()); return 0; }
    msg_hook = SetWindowsHookExA(WH_GETMESSAGE, on_message, self, GetCurrentThreadId());
    if (!msg_hook) { say("graphics card drawing: could not watch for the first game (error %lu): not started", GetLastError()); return 0; }
    timer = SetTimer(NULL, 0, 250, NULL);
    say("graphics card drawing " VERSION " loaded (%s; WA at %08lx): waits for the first game", why, (unsigned long)base);
    return 1;
}
