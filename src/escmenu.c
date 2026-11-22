/* escmenu.c: a Resolution dropdown and a Fullscreen / Windowed switch in WA's own Esc menu, applied live through WA's
 * own code (wkMapZoom). Ported from wkVista (MIT, Copyright (c) 2026 ropahektic, github.com/ropahektic/wkVista:
 * src/escmenu.rs, src/resize.rs, src/display.rs), which found every piece in WA.
 *
 * The menu. GameRuntime::OpenEscMenu 00535200 (stdcall, the runtime; ret 4) lays out its items one row under another
 * with MenuPanel::AppendItem 005408F0 (ESI the panel, runtime +0x30; EAX the x; stack: the item's id, its label (kept),
 * the row's y, centred, a slider's value and maximum; ret 0x18), then sizes the panel from the last row. Right after its
 * first item (the call returns to 005356D3: EBP that row's y, [esp+0x20] a row's height, [esp+0x2C] the buttons' x;
 * WA's next row is height + EBP + 1) the module appends its rows and moves EBP to its last one, so WA's own items
 * follow (Exit last): "Resolution: 1920x1080 (16:9)" (clicked, it opens or closes the list of this monitor's modes, in
 * two columns, the current one in brackets) and "Display: Fullscreen" / "Display: Windowed". The menu is drawn into a
 * bitmap of a fixed size (the panel's +0: its width +0x14, height +0x18), so the list takes only the rows left after
 * room for WA's items still to come and the frame (the smallest modes are left out). MenuPanel::ActivateAtCursor
 * 00540810 (stdcall: the panel, where the id goes; ret 8) gives a clicked item's id with 1; on the module's ids it acts
 * and gives 0 (WA only plays its click sound), and the menu is laid out again so it shows the change.
 *
 * A resolution (as WA switches between the frontend's 640x480 and the game's at every game start): the RenderContext's
 * resize (vtable 00662EC8 slot 9; ECX the RenderContext 0079D6D4, EDX the result, width, height; ret 8: a new frame,
 * then the backend's own reset), the display's sizes as DisplayGfx::Init sets them, the mouse and the window as WA's
 * InitHardware does, and the choice kept where WA keeps it (GameInfo 0088C7EC / 0088C7F0, WA's options in memory
 * 0088E388 +0x30 / +0x34, and HKCU\Software\Team17SoftwareLTD\WormsArmageddon\Options DisplayXSize / DisplayYSize).
 * Fullscreen <-> windowed: WA's flag 007A084C (and its option +0xA4), WA's own windowed game window setup
 * 004EC0D0, or at a switch to fullscreen no caption (CWnd::ModifyStyle 005C6350), the display set up again at the same
 * size, the window over the screen and topmost. Both run when WA's message pump delivers the module's message: between
 * frames, where WA handles activation and display messages itself. Every step is logged. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "MinHook.h"
#include "escmenu.h"

#define VA(a) ((uintptr_t)(base + ((uintptr_t)(a) - 0x400000u)))
#define U32(a) (*(uint32_t *)(a))
#define HEADER_ID 0x57u                 /* (WA's own ids are 0 to 4) */
#define DISPLAY_ID 0x58u
#define MODE_ID 0x60u
#define MAX_MODES 10
#define MSG_SWITCH (WM_APP + 0x4d5a)
#define WND_VA 0x6b390cu                /* WA's window, and its CWnd object */
#define CWND_VA 0x6b3908u
#define WINDOWED_VA 0x7a084cu
#define RC_VA 0x79d6d4u                 /* the RenderContext */
#define OK_VA 0x8ac8b4u                 /* WA's success result */
#define SESSION_VA 0x7a0884u            /* the input session (mouse centre +0x54 / +0x58, +0x80 / +0x84) */
#define OPTIONS_VA 0x88e388u

static uintptr_t base;
static void (*say)(const char *, ...);
static int keep_setting = 1, opened, nlisted, pending_w, pending_h, pending_toggle;
static int listed_w[MAX_MODES], listed_h[MAX_MODES];
static char labels[MAX_MODES + 2][48];  /* (WA keeps pointers to them) */
static uintptr_t runtime_now;
void *esc_orig_after_first, *esc_append_fn;
static int (__stdcall *orig_activate)(void *, uint32_t *);

/* ---- the display modes ------------------------------------------------------------------------------------------- */
static void ratio_name(int w, int h, char *out, size_t n) {
    static const struct { const char *name; double v; } r[] = {
        {"16:9", 16.0 / 9.0}, {"16:10", 1.6}, {"4:3", 4.0 / 3.0}, {"5:4", 1.25}, {"3:2", 1.5}, {"21:9", 64.0 / 27.0}};
    double v = (double)w / h;
    unsigned k;
    int a = w, b = h;
    for (k = 0; k < sizeof(r) / sizeof(r[0]); k++)
        if ((v > r[k].v ? v - r[k].v : r[k].v - v) / r[k].v < 0.01) { snprintf(out, n, "%s", r[k].name); return; }
    while (b) { int t = a % b; a = b; b = t; }
    snprintf(out, n, "%d:%d", w / (a ? a : 1), h / (a ? a : 1));
}

typedef struct { int w, h; } mode;
static int mode_cmp(const void *a, const void *b) {
    const mode *x = (const mode *)a, *y = (const mode *)b;
    long ax = (long)x->w * x->h, ay = (long)y->w * y->h;
    return ax != ay ? (ax < ay ? -1 : 1) : x->w - y->w;
}

/* this monitor's modes of its own ratio (within 1%), and the current one; at most MAX_MODES, the largest kept */
static int offered(mode *out, int cw, int ch) {
    mode all[256];
    DEVMODEA dm;
    DWORD k;
    int n = 0, i, j, m = 0;
    double want = (double)GetSystemMetrics(SM_CXSCREEN) / GetSystemMetrics(SM_CYSCREEN);
    for (k = 0; n < 255; k++) {
        memset(&dm, 0, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsA(NULL, k, &dm)) break;
        if (dm.dmPelsWidth < 640 || dm.dmPelsHeight < 480) continue;
        {
            double r = (double)dm.dmPelsWidth / dm.dmPelsHeight;
            if (((r > want ? r - want : want - r) / want >= 0.01) && ((int)dm.dmPelsWidth != cw || (int)dm.dmPelsHeight != ch)) continue;
        }
        for (i = 0; i < n && (all[i].w != (int)dm.dmPelsWidth || all[i].h != (int)dm.dmPelsHeight); i++) {}
        if (i == n) { all[n].w = (int)dm.dmPelsWidth; all[n].h = (int)dm.dmPelsHeight; n++; }
    }
    for (i = 0; i < n && (all[i].w != cw || all[i].h != ch); i++) {}
    if (i == n && cw > 0) { all[n].w = cw; all[n].h = ch; n++; }
    qsort(all, (size_t)n, sizeof(mode), mode_cmp);
    for (j = n > MAX_MODES ? n - MAX_MODES : 0; j < n; j++) out[m++] = all[j];
    return m;
}

/* ---- the menu ---------------------------------------------------------------------------------------------------- */
uint32_t __cdecl esc_call_append(uint32_t panel, uint32_t x, uint32_t id, const char *label, uint32_t y);
void __cdecl esc_after_first(uint32_t *r);
void esc_thunk_after_first(void);
__asm__(
    ".text\n"
    /* MenuPanel::AppendItem(ESI panel, EAX x; id, label, y, centred 1, 0, 0) from C */
    ".globl _esc_call_append\n"
    "_esc_call_append:\n"
    "    pushl %ebx\n"
    "    pushl %esi\n"
    "    pushl %edi\n"
    "    pushl %ebp\n"
    "    movl 20(%esp), %esi\n"
    "    movl 24(%esp), %eax\n"
    "    pushl $0\n"
    "    pushl $0\n"
    "    pushl $1\n"
    "    pushl 48(%esp)\n"                  /* y */
    "    pushl 48(%esp)\n"                  /* label */
    "    pushl 48(%esp)\n"                  /* id */
    "    call *_esc_append_fn\n"
    "    popl %ebp\n"
    "    popl %edi\n"
    "    popl %esi\n"
    "    popl %ebx\n"
    "    ret\n"
    /* 005356D3, WA's first item appended: the registers to C (EBP may change), then WA's own code on */
    ".globl _esc_thunk_after_first\n"
    "_esc_thunk_after_first:\n"
    "    pushal\n"
    "    pushl %esp\n"
    "    call _esc_after_first\n"
    "    addl $4, %esp\n"
    "    popal\n"
    "    jmp *_esc_orig_after_first\n");

static int windowed(void) { return U32(VA(WINDOWED_VA)) != 0; }

void __cdecl esc_after_first(uint32_t *r) {
    /* pushal: edi esi ebp esp ebx edx ecx eax */
    uintptr_t runtime = r[4], esp = r[3];
    uint32_t panel = U32(runtime + 0x30), row = U32(esp + 0x20), x = U32(esp + 0x2c), y, last;
    int cw = (int)U32(VA(0x7a0ef0)), ch = (int)U32(VA(0x7a0ef4)), k;
    char rn[16];
    runtime_now = runtime;
    ratio_name(cw > 0 ? cw : 1, ch > 0 ? ch : 1, rn, sizeof(rn));
    snprintf(labels[0], sizeof(labels[0]), "Resolution: %dx%d (%s) %s", cw, ch, rn, opened ? "-" : "+");
    y = row + r[2] + 1;
    if (!esc_call_append(panel, x, HEADER_ID, labels[0], y)) return;
    last = y;
    if (opened) {
        mode m[MAX_MODES];
        uint32_t bitmap = U32((uintptr_t)panel), bw = bitmap ? U32((uintptr_t)bitmap + 0x14) : 0, bh = bitmap ? U32((uintptr_t)bitmap + 0x18) : 0;
        uint32_t step = row + 1, room = bh > y + step + 5 * step + 8 ? (bh - (y + step + 5 * step + 8)) / (step ? step : 1) : 0;
        int n = offered(m, cw, ch), fit = (int)room * 2, first = 0;
        if (fit > MAX_MODES) fit = MAX_MODES;
        if (n > fit) { say("Esc menu: room for %d of %d modes (menu bitmap %ux%u): the smallest left out", fit, n, bw, bh); first = n - fit; }
        nlisted = 0;
        for (k = first; k < n; k++) {
            int i = k - first;
            uint32_t col = n - first == 1 ? x : (i % 2 == 0 ? x - bw / 4 : x + bw / 4);
            snprintf(labels[1 + i], sizeof(labels[0]), m[k].w == cw && m[k].h == ch ? "[%dx%d]" : "%dx%d", m[k].w, m[k].h);
            if (i % 2 == 0) y += step;
            if (!esc_call_append(panel, col, MODE_ID + (uint32_t)i, labels[1 + i], y)) { say("Esc menu: the panel is full at %dx%d", m[k].w, m[k].h); break; }
            listed_w[i] = m[k].w; listed_h[i] = m[k].h; nlisted = i + 1;
            last = y;
        }
    }
    snprintf(labels[MAX_MODES + 1], sizeof(labels[0]), windowed() ? "Display: Windowed" : "Display: Fullscreen");
    y = last + row + 1;
    if (esc_call_append(panel, x, DISPLAY_ID, labels[MAX_MODES + 1], y)) last = y;
    r[2] = last;                            /* (EBP: WA's own items follow this row) */
}

/* the Esc menu laid out again, when it is open (it shows the list's state and the current resolution) */
static void refresh(uintptr_t runtime) {
    if (!runtime || U32(runtime + 0x434) != 1) return;
    ((void (__stdcall *)(uintptr_t))VA(0x535200))(runtime);
}

static void post_switch(void) {
    HWND wnd = (HWND)(uintptr_t)U32(VA(WND_VA));
    if (wnd) PostMessageA(wnd, MSG_SWITCH, 0, 0);
}

void escmenu_ask(int w, int h) {
    if (w <= 0) { pending_toggle = 1; say("display: %s asked", windowed() ? "fullscreen" : "windowed"); }
    else { pending_w = w; pending_h = h; say("resolution: %dx%d -> %dx%d asked", (int)U32(VA(0x7a0ef0)), (int)U32(VA(0x7a0ef4)), w, h); }
    post_switch();
}

static int __stdcall hook_activate(void *panel, uint32_t *out) {
    int r = orig_activate(panel, out);
    uint32_t id;
    if (r != 1 || !out) return r;
    id = *out;
    if (id == DISPLAY_ID) { escmenu_ask(0, 0); return 0; }
    if (id == HEADER_ID) { opened = !opened; refresh(runtime_now); return 0; }
    if (id >= MODE_ID && id < MODE_ID + MAX_MODES) {
        int i = (int)(id - MODE_ID);
        if (i < nlisted) escmenu_ask(listed_w[i], listed_h[i]);
        opened = 0;
        refresh(runtime_now);
        return 0;
    }
    return r;
}

/* ---- the switches ------------------------------------------------------------------------------------------------ */
static void rects(HWND wnd, char *out, size_t n) {
    RECT wr, cr;
    GetWindowRect(wnd, &wr);
    GetClientRect(wnd, &cr);
    snprintf(out, n, "%s, window (%ld,%ld)-(%ld,%ld) client %ldx%ld, frame %ux%u", windowed() ? "windowed" : "fullscreen",
             wr.left, wr.top, wr.right, wr.bottom, cr.right - cr.left, cr.bottom - cr.top, U32(VA(0x7a0ef0)), U32(VA(0x7a0ef4)));
}

/* the RenderContext's resize: a new frame and the backend's reset; 1: WA's success */
static int render_resize(int w, int h) {
    typedef uint32_t *(__fastcall *resize_fn)(uintptr_t, uint32_t *, uint32_t, uint32_t);
    uintptr_t rc = U32(VA(RC_VA));
    uint32_t result = 0;
    if (!rc) return 0;
    ((resize_fn)(uintptr_t)U32(U32(rc) + 9 * 4))(rc, &result, (uint32_t)w, (uint32_t)h);
    if (result != U32(VA(OK_VA))) {
        const char *what = result ? (const char *)(uintptr_t)U32(result + 4) : "";
        say("WA's display setup failed: %s %08x", what ? what : "", result ? U32(result + 8) : 0);
        return 0;
    }
    return 1;
}

/* DisplayGfx's sizes as DisplayGfx::Init sets them, and its frame grid (+0x3D9C: +8 pixels, +0x10 row stride, +0x14 /
 * +0x18 size, +0x1C.. clip) as its lock (0056A370) sets it, from the new frame: WA brings the grid up to date only at
 * its next display drawing call, and draws through it before that (text, sprites) -- with the old frame's stride into
 * the new, smaller frame, a crash (the owner's, 2026-11-22) */
static void display_sizes(uintptr_t display, int w, int h) {
    static const int offs[] = {0x3548, 0x354c, 0x3550, 0x3554, 0x3558, 0x355c, 0x3560, 0x3564};
    int v[8], k;
    uintptr_t g = U32(display + 0x3d9c);
    v[0] = w; v[1] = h; v[2] = 0; v[3] = 0; v[4] = w; v[5] = h; v[6] = w / 2; v[7] = h / 2;
    for (k = 0; k < 8; k++) U32(display + offs[k]) = (uint32_t)v[k];
    if (g && U32(VA(0x7a0eec)) && (int)U32(VA(0x7a0ef0)) == w && (int)U32(VA(0x7a0ef4)) == h) {
        U32(g + 0x08) = U32(VA(0x7a0eec));
        U32(g + 0x10) = (uint32_t)w; U32(g + 0x14) = (uint32_t)w; U32(g + 0x18) = (uint32_t)h;
        U32(g + 0x1c) = 0; U32(g + 0x20) = 0; U32(g + 0x24) = (uint32_t)w; U32(g + 0x28) = (uint32_t)h;
    }
}

static void mouse_centre(HWND wnd, int w, int h) {
    uintptr_t s = U32(VA(SESSION_VA));
    int cx = windowed() ? GetSystemMetrics(SM_CXSCREEN) / 2 : w / 2, cy = windowed() ? GetSystemMetrics(SM_CYSCREEN) / 2 : h / 2;
    if (s) { U32(s + 0x54) = (uint32_t)cx; U32(s + 0x58) = (uint32_t)cy; U32(s + 0x80) = (uint32_t)cx; U32(s + 0x84) = (uint32_t)cy; }
    SetCursorPos(cx, cy);
    if (windowed() && wnd) {
        RECT cr;
        GetClientRect(wnd, &cr);
        MapWindowPoints(wnd, NULL, (POINT *)&cr, 2);
        ClipCursor(&cr);
    } else {
        ClipCursor(NULL);
    }
}

static void apply_resolution(uintptr_t runtime, uintptr_t display, int w, int h) {
    HWND wnd = (HWND)(uintptr_t)U32(VA(WND_VA));
    char before[160], after[160];
    if (wnd) rects(wnd, before, sizeof(before)); else strcpy(before, "no window");
    if (!render_resize(w, h)) { say("resolution %dx%d: not applied (before: %s)", w, h, before); return; }
    display_sizes(display, w, h);
    if (!windowed() && wnd) MoveWindow(wnd, 0, 0, w, h, FALSE);
    mouse_centre(wnd, w, h);
    U32(VA(0x88c7ec)) = (uint32_t)w; U32(VA(0x88c7f0)) = (uint32_t)h;
    U32(VA(OPTIONS_VA) + 0x30) = (uint32_t)w; U32(VA(OPTIONS_VA) + 0x34) = (uint32_t)h;
    if (keep_setting) {
        HKEY key;
        DWORD vw = (DWORD)w, vh = (DWORD)h;
        if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Team17SoftwareLTD\\WormsArmageddon\\Options", 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
            RegSetValueExA(key, "DisplayXSize", 0, REG_DWORD, (const BYTE *)&vw, 4);
            RegSetValueExA(key, "DisplayYSize", 0, REG_DWORD, (const BYTE *)&vh, 4);
            RegCloseKey(key);
        } else {
            say("resolution: WA's setting not saved");
        }
    }
    if (wnd) rects(wnd, after, sizeof(after)); else strcpy(after, "no window");
    say("resolution %dx%d: before: %s; after: %s%s", w, h, before, after, keep_setting ? "" : " (test mode: not saved)");
    refresh(runtime);
}

static void apply_toggle(uintptr_t runtime, uintptr_t display) {
    typedef void (__stdcall *setup_fn)(uint32_t);
    typedef int (__attribute__((thiscall)) *modify_fn)(uintptr_t, uint32_t, uint32_t, uint32_t);
    HWND wnd = (HWND)(uintptr_t)U32(VA(WND_VA));
    uintptr_t cwnd = U32(VA(CWND_VA)), rc = U32(VA(RC_VA));
    int w = (int)U32(VA(0x7a0ef0)), h = (int)U32(VA(0x7a0ef4));
    char before[160], after[160];
    if (!wnd || !cwnd || !rc) { say("display switch: no window or RenderContext"); return; }
    rects(wnd, before, sizeof(before));
    if (!windowed()) {
        /* to windowed: WA's flag and option, not topmost, the caption marked removed (it is) so WA's windowed setup puts
         * it back; WA's setup with the display, then again without it (leaving fullscreen, Direct3D's reset puts back
         * the style and place it saved when it went fullscreen) */
        U32(VA(WINDOWED_VA)) = 1;
        U32(VA(OPTIONS_VA) + 0xa4) = 1;
        SetWindowPos(wnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        *(uint8_t *)VA(0x6b32aa) = 1;
        ((setup_fn)VA(0x4ec0d0))(1);
        *(uint8_t *)VA(0x6b32aa) = 1;
        ((setup_fn)VA(0x4ec0d0))(0);
    } else {
        /* to fullscreen: WA's flag and option, no caption, the display set up again at the same size, the window over
         * the screen and topmost */
        U32(VA(WINDOWED_VA)) = 0;
        U32(VA(OPTIONS_VA) + 0xa4) = 0;
        ((modify_fn)VA(0x5c6350))(cwnd, 0x00c00000u, 0, 0);
        *(uint8_t *)VA(0x6b32aa) = 0;
        render_resize(w, h);
        SetWindowPos(wnd, HWND_TOPMOST, 0, 0, w, h, SWP_SHOWWINDOW | SWP_FRAMECHANGED);
    }
    mouse_centre(wnd, w, h);
    display_sizes(display, w, h);
    rects(wnd, after, sizeof(after));
    say("display switch: before: %s; after: %s", before, after);
    refresh(runtime);
}

int escmenu_message(const MSG *m) {
    uintptr_t world, display;
    if (m->message != MSG_SWITCH) return 0;
    if (!runtime_now || !(world = U32(runtime_now + 0x488)) || !(display = U32(world + 4))) return 1;
    if (pending_w) { int w = pending_w, h = pending_h; pending_w = pending_h = 0; apply_resolution(runtime_now, display, w, h); }
    if (pending_toggle) { pending_toggle = 0; apply_toggle(runtime_now, display); }
    return 1;
}

void escmenu_frame(void *runtime) { if (runtime) runtime_now = (uintptr_t)runtime; }

void escmenu_test_open(void) {
    if (!runtime_now) return;
    ((void (__stdcall *)(uintptr_t))VA(0x535200))(runtime_now);
    say("test: WA's Esc menu opened (%s)", U32(runtime_now + 0x434) == 1 ? "open" : "not open");
}

void escmenu_test_list(void) {
    opened = 1;
    say("test: the resolution list opened (Esc menu %s)", runtime_now && U32(runtime_now + 0x434) == 1 ? "open" : "not open");
    refresh(runtime_now);
}

int escmenu_install(uintptr_t b, void (*log)(const char *, ...), int keep) {
    static const unsigned char first_bytes[] = {0x6a, 0x03, 0x89, 0x44, 0x24, 0x44, 0xe8, 0x1d, 0xb2, 0x00, 0x00, 0x8b, 0x83, 0x88, 0x04, 0x00, 0x00};
    static const unsigned char activate_bytes[] = {0x53, 0x8b, 0x5c, 0x24, 0x08, 0x56, 0x8b, 0x73, 0x14};
    static const unsigned char open_bytes[] = {0x81, 0xec, 0x08, 0x04, 0x00, 0x00, 0x53};
    static const unsigned char append_bytes[] = {0x83, 0xbe, 0xb0, 0x03, 0x00, 0x00, 0x10, 0x53};
    base = b; say = log; keep_setting = keep;
    if (memcmp((void *)VA(0x5356c8), first_bytes, sizeof(first_bytes)) || memcmp((void *)VA(0x540810), activate_bytes, sizeof(activate_bytes)) ||
        memcmp((void *)VA(0x535200), open_bytes, sizeof(open_bytes)) || memcmp((void *)VA(0x5408f0), append_bytes, sizeof(append_bytes))) {
        say("Esc menu items unavailable: WA's Esc menu is not as in WA 3.8.1");
        return 0;
    }
    esc_append_fn = (void *)VA(0x5408f0);
    if (MH_CreateHook((void *)VA(0x5356d3), (void *)esc_thunk_after_first, &esc_orig_after_first) != MH_OK ||
        MH_CreateHook((void *)VA(0x540810), (void *)hook_activate, (void **)&orig_activate) != MH_OK) {
        say("Esc menu items unavailable: could not hook WA's Esc menu");
        return 0;
    }
    return 1;
}
