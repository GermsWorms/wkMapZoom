/* present.c: true-colour output (wkMapZoom). The zoomed map is averaged in RGB; this shows it as it is instead of as
 * the nearest of WA's 256 colours.
 *
 * WA ends a frame with its renderer's "show frame" (RenderContext::FlushRender 005A26C0 -> the backend's vtable slot
 * 12; ECX the backend, EDX the result, the 8-bit frame on the stack; ret 4). In WA's Direct3D 9 backends (vtables
 * 006767B0 software palette, 006767F8 shader palette; the device at +0x40) that step (0059CB30 / 0059CDE0) checks the
 * device, uploads the 8-bit frame and the palette and draws them as one quad through the palette. When the module has
 * a frame of its own, the step is the module's: its frame goes into WA's back buffer and the result is WA's success
 * code (008AC8B4). Otherwise -- the frontend, a lost device, a back buffer of another size -- WA's own runs.
 * The way in is wkVista's (MIT, Copyright (c) 2026 ropahektic, github.com/ropahektic/wkVista, src/present.rs). Here the
 * frame is copied from a system-memory surface with UpdateSurface: such a surface survives Direct3D resets, so no
 * Direct3D function is hooked (the Steam overlay patches those) -- only WA's own code.
 *
 * The same hook is a safety net. WA's step tests its device only once it is marked lost (0059C960: +0x34), never for
 * NULL, and uses its textures (+0x7C frame, +0x80 vertex buffer, shader: +0x84 pixel shader, +0x88 palette) unchecked:
 * a NULL device with live textures crashed the laptop's WA at 0059CF42 (SetTexture on NULL, 2026-11-22). With no
 * device the frame is skipped, as WA's own step skips one on a lost device; with a device but its textures gone, WA's
 * own lost-device path or a remake of the textures puts them back first.
 *
 * An overlay (present_overlay: the speedometer on frames WA draws itself, 2026-11-23): WA's step runs as ever -- its
 * quad is drawn and its scene ended (0059C7A0) -- and then the small picture, blended on the CPU with the colours WA's
 * frame has there (the 8-bit frame through the palette WA's backends use, 008AC8C8), is copied onto the back buffer with
 * UpdateSurface, from a system-memory surface of its size. No Direct3D state is changed, and nothing of the module's has
 * to go before a Reset. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "MinHook.h"
#include "present.h"

#define VA(a) ((uintptr_t)(base + ((uintptr_t)(a) - 0x400000u)))
#define U32(a) (*(uint32_t *)(a))
#define SLOT(obj, k) ((*(void ***)(obj))[k])
#define SW_VT 0x6767b0u
#define SH_VT 0x6767f8u
#define D3DERR_DEVICELOST_ ((long)0x88760868)
#define D3DERR_DEVICENOTRESET_ ((long)0x88760869)

static uintptr_t base;
static present_compose_fn compose;
static void say_none(const char *fmt, ...) { (void)fmt; }
static void (*say)(const char *, ...) = say_none;   /* (until installed: nothing logged, nothing called through NULL) */
static void (*on_gone)(int stuck), (*on_back)(void);
static DWORD gone_posted, notreset_since;
static volatile LONG ready;
static int failed, logged_size, logged_on, logged_gone, logged_remade, test_gone;
static int fmt_bad;                     /* the back buffer's format can't be used (until the device is made anew) */
static int size_bad;                    /* a frame of another size refused (until one is shown, or the device anew) */
static DWORD hook_tick;                 /* WA's Direct3D 9 show-frame last ran */
static unsigned shown, gone_frames, remake_wait;
static void *surf, *surf_dev;
static UINT surf_w, surf_h, surf_fmt;
static struct { const uint32_t *img; int x, y, w, h; DWORD tick; } ov;   /* the overlay of the next frame WA shows itself */
static volatile LONG ov_ready;
static void *ov_surf, *ov_dev;
static UINT ov_w, ov_h, ov_fmt;
static int ov_failed, ov_logged, ov_every;
static unsigned ov_shown, ov_checked, ov_saved;
static void (*ov_save)(const char *name, int w, int h, const uint32_t *px);
/* WA's result for a lost device: 0059C9B1 builds {0x830, no text, 0} (its frontend, 00403BA8, tests for 0x830). Kept
 * here, as WA keeps its own in a slot of its 16-record error ring (008ACE98), which later errors write over */
static uint32_t lost_result[3] = {0x830u, 0, 0};
void *present_orig_sw, *present_orig_sh, *present_orig_close;

typedef long (__stdcall *f_tcl)(void *);
typedef unsigned long (__stdcall *f_release)(void *);
typedef long (__stdcall *f_getbb)(void *, UINT, UINT, UINT, void **);
typedef long (__stdcall *f_getdesc)(void *, uint32_t *);
typedef long (__stdcall *f_mksurf)(void *, UINT, UINT, UINT, UINT, void **, void *);
typedef long (__stdcall *f_lock)(void *, void *, const void *, DWORD);
typedef long (__stdcall *f_unlock)(void *);
typedef long (__stdcall *f_update)(void *, void *, const void *, void *, const void *);
typedef void (__attribute__((thiscall)) *f_free_res)(void *);               /* backend slot 16 (0059C6F0 / 0059CD70) */
typedef uint32_t *(__attribute__((thiscall)) *f_make_res)(void *, uint32_t *); /* backend slot 15 (0059C3D0 / 0059CC90) */

static void release(void *o) { if (o) ((f_release)SLOT(o, 2))(o); }

int __cdecl present_show(uint32_t *r);
void present_thunk_sw(void);
void present_thunk_sh(void);
__asm__(
    ".text\n"
    /* pushal: edi esi ebp esp ebx edx ecx eax; then the return address and the 8-bit frame */
    ".globl _present_thunk_sw\n"
    "_present_thunk_sw:\n"
    "    pushal\n"
    "    pushl %esp\n"
    "    call _present_show\n"
    "    addl $4, %esp\n"
    "    testl %eax, %eax\n"
    "    popal\n"
    "    jnz 1f\n"
    "    jmp *_present_orig_sw\n"
    "1:  ret $4\n"
    ".globl _present_thunk_sh\n"
    "_present_thunk_sh:\n"
    "    pushal\n"
    "    pushl %esp\n"
    "    call _present_show\n"
    "    addl $4, %esp\n"
    "    testl %eax, %eax\n"
    "    popal\n"
    "    jnz 2f\n"
    "    jmp *_present_orig_sh\n"
    "2:  ret $4\n");

void present_thunk_close(void);
__asm__(
    ".text\n"
    /* the backends' close (0059C730, both vtables' slot 7: ECX the backend, EDX the result, keep; ret 4): the module's
     * surface let go first, so it never keeps a device WA releases alive (a Direct3D 9 resource holds its device) */
    ".globl _present_thunk_close\n"
    "_present_thunk_close:\n"
    "    pushal\n"
    "    call _present_forget\n"
    "    popal\n"
    "    jmp *_present_orig_close\n");

/* this frame not shown: the result WA's own step gives for a lost device, so FlushRender (005A26C0: 'test eax,eax' on
 * the result at 005A26E0) skips the backend's Present (slot 13, 0059C8A0), which uses the device unchecked too; WA's
 * game (0056A580) drops the result */
static int skip_frame(uint32_t *r, uint32_t *result) {
    *result = (uint32_t)(uintptr_t)lost_result;
    r[7] = (uint32_t)(uintptr_t)result;                      /* (EAX: the result, as WA's own returns it) */
    return 1;
}

static int textures_there(uintptr_t be, int sh) {
    return U32(be + 0x7c) && U32(be + 0x80) && (!sh || (U32(be + 0x84) && U32(be + 0x88)));
}

/* 1: the frame is skipped (no device, or its textures can't be had); 0: WA's step (or the module's) may run */
static int guard(uint32_t *r, uintptr_t be, uint32_t *result) {
    const int sh = U32(be) == VA(SH_VT);
    void *dev;
    long hr;
    if (!sh && U32(be) != VA(SW_VT)) return 0;              /* (another renderer: not WA's Direct3D 9) */
    dev = *(void **)(be + 0x40);
    if (test_gone > 0) { test_gone--; dev = NULL; }          /* (test mode: as with no device, without the recovery) */
    else if (!dev || !*(void **)dev) {
        if (!logged_gone) {
            logged_gone = 1;
            say("display: WA's Direct3D device is gone (textures %08x %08x, lost %u, kept %u): frames skipped until it is "
                "set up again", U32(be + 0x7c), sh ? U32(be + 0x88) : 0u, *(uint8_t *)(be + 0x34), *(uint8_t *)(be + 0x35));
        }
        /* (a recovery asked for about once a second -- by the clock: skipped frames don't wait for the screen) */
        if (on_gone && (!gone_frames || GetTickCount() - gone_posted >= 1000)) { gone_posted = GetTickCount(); on_gone(0); }
        gone_frames++;
        dev = NULL;
    }
    if (!dev) return skip_frame(r, result);
    if (gone_frames) {                                       /* (back: the next loss is a new incident, logged again) */
        gone_frames = 0;
        logged_gone = 0;
        if (on_back) on_back();
    }
    if (textures_there(be, sh)) { notreset_since = 0; return 0; }
    hr = ((f_tcl)SLOT(dev, 3))(dev);
    if (hr == D3DERR_DEVICELOST_ || hr == D3DERR_DEVICENOTRESET_) {
        /* marked lost, WA's own check runs: lost, it skips the frame; not reset, its Reset (0059CA50) makes the
         * textures again (and on a failure it returns its error before any texture is used). Lost is waited out (WA
         * minimised, alt-tabbed out of fullscreen); 'not reset' for 3 s means WA's Reset keeps failing: the device made
         * anew (the recovery, asked for once every 3 s) */
        *(uint8_t *)(be + 0x34) = 1;
        if (hr == D3DERR_DEVICENOTRESET_) {
            const DWORD now = GetTickCount();
            if (!notreset_since) notreset_since = now;
            else if (now - notreset_since >= 3000 && on_gone) {
                say("display: WA's Direct3D Reset has failed for %lu ms: the device made anew", (unsigned long)(now - notreset_since));
                notreset_since = now;
                on_gone(1);
            }
        } else notreset_since = 0;
        return 0;
    }
    notreset_since = 0;
    if (hr >= 0 && remake_wait++ % 30 == 0) {               /* the device works, its textures were not made: made again,
                                                              * released first, as WA's Reset path does (0059CA58, 0059CAF9) */
        uint32_t res = 0;
        ((f_free_res)SLOT(be, 16))((void *)be);
        ((f_make_res)SLOT(be, 15))((void *)be, &res);
        if (!logged_remade) { logged_remade = 1; say("display: WA's Direct3D textures were missing: made again (%s)", res ? "failed" : "done"); }
        if (textures_there(be, sh)) { remake_wait = 0; return 0; }
    }
    return skip_frame(r, result);
}

/* WA's own frame at (x, y) as the back buffer holds it: the 8-bit frame through the palette WA's backends use (R, G, B,
 * flags a colour) */
static uint32_t shown_colour(const uint8_t *frame, int stride, int x, int y) {
    const uint8_t *p = (const uint8_t *)VA(0x8ac8c8) + 4 * frame[(size_t)y * stride + x];
    return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
}
static uint32_t over(uint32_t c, uint32_t b) {               /* (premultiplied c over b, as compose_rows) */
    const uint32_t na = 255u - (c >> 24);
    return (((c >> 16) & 0xff) + ((b >> 16) & 0xff) * na / 255u) << 16 |
           (((c >> 8) & 0xff) + ((b >> 8) & 0xff) * na / 255u) << 8 | ((c & 0xff) + (b & 0xff) * na / 255u);
}

/* test mode: the back buffer read back, against WA's frame through its palette with the overlay over it */
static void overlay_check(void *dev, void *bb, const uint32_t *desc, const uint8_t *frame, int fw, int fh, int x0, int y0, int x1, int y1) {
    typedef long (__stdcall *f_getrt)(void *, void *, void *);
    void *s = NULL;
    struct { INT pitch; void *bits; } lr;
    long in = 0, out = 0;
    int x, y;
    if (((f_mksurf)SLOT(dev, 36))(dev, desc[6], desc[7], desc[0], 2, &s, NULL) < 0 || !s) return;
    if (((f_getrt)SLOT(dev, 32))(dev, bb, s) >= 0 && ((f_lock)SLOT(s, 13))(s, &lr, NULL, 0x10 /* READONLY */) >= 0) {
        for (y = 0; y < fh; y++)
            for (x = 0; x < fw; x++) {
                const int inside = x >= x0 && x < x1 && y >= y0 && y < y1;
                uint32_t want = shown_colour(frame, fw, x, y);
                if (inside) want = over(ov.img[(size_t)(y - ov.y) * ov.w + (x - ov.x)], want);
                if ((((const uint32_t *)((const uint8_t *)lr.bits + (size_t)y * lr.pitch))[x] & 0xffffffu) != want) { if (inside) in++; else out++; }
            }
        say("test: overlay frame %u: the back buffer differs from what it should hold in %ld pixels of the picture's %d and %ld of the rest",
            ov_shown, in, (x1 - x0) * (y1 - y0), out);
        if (ov_save && ov_saved < 4) {
            uint32_t *px = (uint32_t *)malloc((size_t)fw * fh * 4);
            char name[32] = "overlay-0";
            if (px) {
                for (y = 0; y < fh; y++) memcpy(px + (size_t)y * fw, (const uint8_t *)lr.bits + (size_t)y * lr.pitch, (size_t)fw * 4);
                name[8] = (char)('1' + ov_saved++);
                ov_save(name, fw, fh, px);
                free(px);
            }
        }
        ((f_unlock)SLOT(s, 14))(s);
    }
    release(s);
}

/* the overlay onto the back buffer, after WA's own step drew its frame there */
static void overlay_draw(void *dev) {
    const uint8_t *frame = *(const uint8_t **)VA(0x7a0eec);
    const int fw = *(const int32_t *)VA(0x7a0ef0), fh = *(const int32_t *)VA(0x7a0ef4);
    const int x0 = ov.x < 0 ? 0 : ov.x, y0 = ov.y < 0 ? 0 : ov.y, x1 = ov.x + ov.w > fw ? fw : ov.x + ov.w, y1 = ov.y + ov.h > fh ? fh : ov.y + ov.h;
    uint32_t desc[8];
    struct { INT pitch; void *bits; } lr;
    RECT from;
    POINT to;
    void *bb = NULL;
    long hr;
    int x, y;
    if (!frame || x1 <= x0 || y1 <= y0) return;
    if (((f_tcl)SLOT(dev, 3))(dev) != 0) return;             /* (lost meanwhile: WA's Present finds out) */
    if (((f_getbb)SLOT(dev, 18))(dev, 0, 0, 0, &bb) < 0 || !bb) return;
    memset(desc, 0, sizeof(desc));
    ((f_getdesc)SLOT(bb, 12))(bb, desc);
    if (desc[0] != 21 && desc[0] != 22) { fmt_bad = 1; release(bb); return; }          /* (as for true colour) */
    if ((int)desc[6] != fw || (int)desc[7] != fh) { size_bad = 1; release(bb); return; }
    if (!ov_surf || ov_dev != dev || ov_w != (UINT)ov.w || ov_h != (UINT)ov.h || ov_fmt != desc[0]) {
        release(ov_surf);
        ov_surf = NULL;
        hr = ((f_mksurf)SLOT(dev, 36))(dev, (UINT)ov.w, (UINT)ov.h, desc[0], 2 /* D3DPOOL_SYSTEMMEM */, &ov_surf, NULL);
        if (hr < 0 || !ov_surf) {
            ov_surf = NULL;
            ov_failed = 1;
            say("overlay off: no %dx%d system-memory surface (%08lx)", ov.w, ov.h, (unsigned long)hr);
            release(bb);
            return;
        }
        ov_dev = dev; ov_w = (UINT)ov.w; ov_h = (UINT)ov.h; ov_fmt = desc[0];
    }
    if (((f_lock)SLOT(ov_surf, 13))(ov_surf, &lr, NULL, 0) < 0) { release(bb); return; }
    for (y = y0; y < y1; y++) {
        uint32_t *d = (uint32_t *)((uint8_t *)lr.bits + (size_t)(y - ov.y) * lr.pitch);
        const uint32_t *o = ov.img + (size_t)(y - ov.y) * ov.w;
        for (x = x0; x < x1; x++) d[x - ov.x] = over(o[x - ov.x], shown_colour(frame, fw, x, y));
    }
    ((f_unlock)SLOT(ov_surf, 14))(ov_surf);
    from.left = x0 - ov.x; from.top = y0 - ov.y; from.right = x1 - ov.x; from.bottom = y1 - ov.y;
    to.x = x0; to.y = y0;
    hr = ((f_update)SLOT(dev, 30))(dev, ov_surf, &from, bb, &to);
    if (hr < 0) {
        ov_failed = 1;
        say("overlay off: UpdateSurface into the back buffer failed (%08lx)", (unsigned long)hr);
    } else {
        ov_shown++;
        if (!ov_logged) { ov_logged = 1; say("overlay: on (WA's own frames shown as they are, the speedometer put over them)"); }
        if (ov_every && ov_shown % ov_every == 0 && ov_checked++ < 40) overlay_check(dev, bb, desc, frame, fw, fh, x0, y0, x1, y1);
    }
    release(bb);
}

/* WA's own step, then the overlay (the step is __fastcall: ECX the backend, EDX the result, the 8-bit frame on the stack) */
typedef uint32_t *(__attribute__((fastcall)) *show_fn)(void *, uint32_t *, void *);
static int overlay_show(uint32_t *r, void *backend, uint32_t *result) {
    const uint32_t vt = U32(backend);
    const show_fn orig = (show_fn)(vt == VA(SH_VT) ? present_orig_sh : vt == VA(SW_VT) ? present_orig_sw : NULL);
    void *dev;
    if (!orig) return 0;
    r[7] = (uint32_t)(uintptr_t)orig(backend, result, (void *)(uintptr_t)r[9]);   /* (EAX: the result, as it returns it) */
    if (*result != *(uint32_t *)VA(0x8ac8b4)) return 1;      /* (not drawn: lost, an error) */
    dev = *(void **)((char *)backend + 0x40);
    if (dev && *(void **)dev) overlay_draw(dev);
    return 1;
}

int __cdecl present_show(uint32_t *r) {
    void *backend = (void *)(uintptr_t)r[6], *dev, *bb = NULL;
    uint32_t *result = (uint32_t *)(uintptr_t)r[5], desc[8];
    struct { INT pitch; void *bits; } lr;
    long hr;
    int ok;
    if (!backend || !result) return 0;
    hook_tick = GetTickCount();             /* (WA is on a Direct3D 9 renderer: present_live) */
    ok = InterlockedExchange(&ov_ready, 0) && GetTickCount() - ov.tick < 5000;   /* (left over from a frame that was never shown; WA may save a
                                                                                           * screenshot in between) */
    if (guard(r, (uintptr_t)backend, result)) { InterlockedExchange(&ready, 0); return 1; }
    if (!InterlockedExchange(&ready, 0) || failed || !compose) return ok && !ov_failed && !failed && compose ? overlay_show(r, backend, result) : 0;
    dev = *(void **)((char *)backend + 0x40);
    if (!dev || !*(void **)dev) return 0;
    if (((f_tcl)SLOT(dev, 3))(dev) != 0) return 0;          /* lost: WA's own step resets the device */
    hr = ((f_getbb)SLOT(dev, 18))(dev, 0, 0, 0, &bb);
    if (hr < 0 || !bb) return 0;
    memset(desc, 0, sizeof(desc));
    ((f_getdesc)SLOT(bb, 12))(bb, desc);                     /* format, type, usage, pool, ms, quality, width, height */
    if (desc[0] != 21 && desc[0] != 22) {                    /* (A8R8G8B8 / X8R8G8B8 only) */
        if (!logged_size) { logged_size = 1; say("true colour: back buffer format %u, not used", desc[0]); }
        fmt_bad = 1;
        release(bb);
        return 0;
    }
    if (!surf || surf_dev != dev || surf_w != desc[6] || surf_h != desc[7] || surf_fmt != desc[0]) {
        release(surf);
        surf = NULL;
        hr = ((f_mksurf)SLOT(dev, 36))(dev, desc[6], desc[7], desc[0], 2 /* D3DPOOL_SYSTEMMEM */, &surf, NULL);
        if (hr < 0 || !surf) {
            surf = NULL;
            release(bb);
            failed = 1;
            say("true colour off: no %ux%u system-memory surface (%08lx)", desc[6], desc[7], (unsigned long)hr);
            return 0;
        }
        surf_dev = dev; surf_w = desc[6]; surf_h = desc[7]; surf_fmt = desc[0];
    }
    if (((f_lock)SLOT(surf, 13))(surf, &lr, NULL, 0) < 0) { release(bb); return 0; }
    ok = compose((uint8_t *)lr.bits, lr.pitch, (int)desc[6], (int)desc[7]);
    ((f_unlock)SLOT(surf, 14))(surf);
    if (!ok) {
        if (!logged_size) { logged_size = 1; say("true colour: back buffer %ux%u is not the frame's size, not used", desc[6], desc[7]); }
        size_bad = 1;
        release(bb);
        return 0;
    }
    hr = ((f_update)SLOT(dev, 30))(dev, surf, NULL, bb, NULL);
    release(bb);
    if (hr < 0) {
        failed = 1;
        say("true colour off: UpdateSurface into the back buffer failed (%08lx); WA's own colours from now on", (unsigned long)hr);
        return 0;
    }
    size_bad = 0;
    if (!logged_on) { logged_on = 1; say("true colour: on (back buffer %ux%u, format %u)", desc[6], desc[7], desc[0]); }
    shown++;
    *result = *(uint32_t *)VA(0x8ac8b4);
    r[7] = (uint32_t)(uintptr_t)result;                      /* (EAX: the result, as WA's own returns it) */
    return 1;
}

void present_ready(void) { InterlockedExchange(&ready, 1); }
void present_overlay(const uint32_t *argb, int x, int y, int w, int h) {
    if (!argb || w <= 0 || h <= 0) { InterlockedExchange(&ov_ready, 0); return; }
    ov.img = argb; ov.x = x; ov.y = y; ov.w = w; ov.h = h; ov.tick = GetTickCount();
    InterlockedExchange(&ov_ready, 1);
}
void present_test_overlay(int every, void (*save)(const char *, int, int, const uint32_t *)) { ov_every = every; ov_save = save; }
unsigned present_shown(void) { return shown; }
void present_on_gone(void (*gone)(int stuck), void (*back)(void)) { on_gone = gone; on_back = back; }
/* the module's surfaces let go (before WA's display is set up again: they hold no device of WA's after) */
void present_forget(void) {
    release(surf); surf = NULL; surf_dev = NULL;
    release(ov_surf); ov_surf = NULL; ov_dev = NULL; ov_failed = 0;
    fmt_bad = 0; size_bad = 0;
}
int present_live(void) {
    const DWORD now = GetTickCount();
    return compose && !failed && !fmt_bad && !size_bad && hook_tick && now - hook_tick < 1000;
}
int present_overlay_live(void) {
    static int rgba = -1;                   /* (wkRGBA colours the land on the back buffer inside WA's step: what is under
                                             * the picture isn't WA's 8-bit frame there; asked once, at the first frame) */
    if (rgba < 0) rgba = GetModuleHandleA("wkRGBA.dll") != NULL;
    return present_live() && !ov_failed && !rgba;
}
void present_test_gone(int frames) { test_gone = frames; say("test: %d frames as with no Direct3D device", frames); }

int present_install(uintptr_t b, present_compose_fn c, void (*log)(const char *, ...)) {
    static const unsigned char sw_bytes[] = {0x81, 0xec, 0x0c, 0x04, 0x00, 0x00, 0x53, 0x57};
    static const unsigned char sh_bytes[] = {0x83, 0xec, 0x0c, 0x53, 0x57, 0x8d, 0x44, 0x24, 0x08};
    static const unsigned char close_bytes[] = {0x56, 0x8b, 0xf1, 0x83, 0x7e, 0x14, 0x00, 0x57, 0x8b, 0xfa};
    uint8_t *fsw, *fsh;
    base = b; compose = c; say = log;
    fsw = (uint8_t *)VA(0x59cb30); fsh = (uint8_t *)VA(0x59cde0);
    if (*(uintptr_t *)(VA(0x6767b0) + 12 * 4) != (uintptr_t)fsw || *(uintptr_t *)(VA(0x6767f8) + 12 * 4) != (uintptr_t)fsh ||
        memcmp(fsw, sw_bytes, sizeof(sw_bytes)) || memcmp(fsh, sh_bytes, sizeof(sh_bytes)) ||
        *(uintptr_t *)(VA(0x6767b0) + 7 * 4) != VA(0x59c730) || *(uintptr_t *)(VA(0x6767f8) + 7 * 4) != VA(0x59c730) ||
        memcmp((void *)VA(0x59c730), close_bytes, sizeof(close_bytes))) {
        say("true colour and the display safety net unavailable: WA's Direct3D 9 backends are not as in WA 3.8.1");
        return 0;
    }
    if (MH_CreateHook(fsw, (void *)present_thunk_sw, &present_orig_sw) != MH_OK ||
        MH_CreateHook(fsh, (void *)present_thunk_sh, &present_orig_sh) != MH_OK ||
        MH_CreateHook((void *)VA(0x59c730), (void *)present_thunk_close, &present_orig_close) != MH_OK) {
        say("true colour and the display safety net unavailable: could not hook WA's Direct3D 9 backends");
        return 0;
    }
    return 1;
}
