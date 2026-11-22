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
 * Direct3D function is hooked (the Steam overlay patches those) -- only WA's own code. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "MinHook.h"
#include "present.h"

#define VA(a) ((uintptr_t)(base + ((uintptr_t)(a) - 0x400000u)))
#define SLOT(obj, k) ((*(void ***)(obj))[k])

static uintptr_t base;
static present_compose_fn compose;
static void (*say)(const char *, ...);
static volatile LONG ready;
static int failed, logged_size, logged_on;
static unsigned shown;
static void *surf, *surf_dev;
static UINT surf_w, surf_h, surf_fmt;
void *present_orig_sw, *present_orig_sh;

typedef long (__stdcall *f_tcl)(void *);
typedef unsigned long (__stdcall *f_release)(void *);
typedef long (__stdcall *f_getbb)(void *, UINT, UINT, UINT, void **);
typedef long (__stdcall *f_getdesc)(void *, uint32_t *);
typedef long (__stdcall *f_mksurf)(void *, UINT, UINT, UINT, UINT, void **, void *);
typedef long (__stdcall *f_lock)(void *, void *, const void *, DWORD);
typedef long (__stdcall *f_unlock)(void *);
typedef long (__stdcall *f_update)(void *, void *, const void *, void *, const void *);

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

int __cdecl present_show(uint32_t *r) {
    void *backend = (void *)(uintptr_t)r[6], *dev, *bb = NULL;
    uint32_t *result = (uint32_t *)(uintptr_t)r[5], desc[8];
    struct { INT pitch; void *bits; } lr;
    long hr;
    int ok;
    if (!InterlockedExchange(&ready, 0) || failed || !backend || !result) return 0;
    dev = *(void **)((char *)backend + 0x40);
    if (!dev || !*(void **)dev) return 0;
    if (((f_tcl)SLOT(dev, 3))(dev) != 0) return 0;          /* lost: WA's own step resets the device */
    hr = ((f_getbb)SLOT(dev, 18))(dev, 0, 0, 0, &bb);
    if (hr < 0 || !bb) return 0;
    memset(desc, 0, sizeof(desc));
    ((f_getdesc)SLOT(bb, 12))(bb, desc);                     /* format, type, usage, pool, ms, quality, width, height */
    if (desc[0] != 21 && desc[0] != 22) {                    /* (A8R8G8B8 / X8R8G8B8 only) */
        if (!logged_size) { logged_size = 1; say("true colour: back buffer format %u, not used", desc[0]); }
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
    if (!logged_on) { logged_on = 1; say("true colour: on (back buffer %ux%u, format %u)", desc[6], desc[7], desc[0]); }
    shown++;
    *result = *(uint32_t *)VA(0x8ac8b4);
    r[7] = (uint32_t)(uintptr_t)result;                      /* (EAX: the result, as WA's own returns it) */
    return 1;
}

void present_ready(void) { InterlockedExchange(&ready, 1); }
unsigned present_shown(void) { return shown; }

int present_install(uintptr_t b, present_compose_fn c, void (*log)(const char *, ...)) {
    static const unsigned char sw_bytes[] = {0x81, 0xec, 0x0c, 0x04, 0x00, 0x00, 0x53, 0x57};
    static const unsigned char sh_bytes[] = {0x83, 0xec, 0x0c, 0x53, 0x57, 0x8d, 0x44, 0x24, 0x08};
    uint8_t *fsw, *fsh;
    base = b; compose = c; say = log;
    fsw = (uint8_t *)VA(0x59cb30); fsh = (uint8_t *)VA(0x59cde0);
    if (*(uintptr_t *)(VA(0x6767b0) + 12 * 4) != (uintptr_t)fsw || *(uintptr_t *)(VA(0x6767f8) + 12 * 4) != (uintptr_t)fsh ||
        memcmp(fsw, sw_bytes, sizeof(sw_bytes)) || memcmp(fsh, sh_bytes, sizeof(sh_bytes))) {
        say("true colour unavailable: WA's Direct3D 9 backends are not as in WA 3.8.1");
        return 0;
    }
    if (MH_CreateHook(fsw, (void *)present_thunk_sw, &present_orig_sw) != MH_OK ||
        MH_CreateHook(fsh, (void *)present_thunk_sh, &present_orig_sh) != MH_OK) {
        say("true colour unavailable: could not hook WA's Direct3D 9 backends");
        return 0;
    }
    return 1;
}
