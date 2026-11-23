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
#include "present.h"

#define VA(a) ((uintptr_t)(base + ((uintptr_t)(a) - 0x400000u)))
#define U32(a) (*(uint32_t *)(a))
#define HEADER_ID 0x57u                 /* (WA's own ids are 0 to 4) */
#define DISPLAY_ID 0x58u
#define MODE_ID 0x60u
#define MAX_MODES 10
#define OPTS_ID 0x59u                   /* the module's options row, its "More options" row, and its options */
#define MORE_ID 0x5au
#define OPT_ID 0x70u
#define MAX_OPTS 12
#define MSG_SWITCH (WM_APP + 0x4d5a)
#define WND_VA 0x6b390cu                /* WA's window, and its CWnd object */
#define CWND_VA 0x6b3908u
#define WINDOWED_VA 0x7a084cu
#define RC_VA 0x79d6d4u                 /* the RenderContext */
#define OK_VA 0x8ac8b4u                 /* WA's success result */
#define SESSION_VA 0x7a0884u            /* the input session (mouse centre +0x54 / +0x58, +0x80 / +0x84) */
#define OPTIONS_VA 0x88e388u

static uintptr_t base;
static void say_none(const char *fmt, ...) { (void)fmt; }
static void (*say)(const char *, ...) = say_none;   /* (until installed: nothing logged, nothing called through NULL) */
static int keep_setting = 1, opened, nlisted, pending_w, pending_h, pending_toggle, pending_recover, busy;
static int listed_w[MAX_MODES], listed_h[MAX_MODES];
static char labels[MAX_MODES + 2][48];  /* (WA keeps pointers to them) */
static const escmenu_options *opts;
static int opts_open, opts_page, opt_first, opt_end;   /* (the page shown, and its options: [first, end)) */
static char opt_labels[MAX_OPTS + 1][64], more_label[48];
static uintptr_t runtime_now;
static DWORD runtime_tick;             /* (when a game frame last gave runtime_now: after the game it is freed) */
static int test_any_size;               /* (test mode: a fullscreen size that is no mode, to see the rollback) */
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
    runtime_tick = GetTickCount();
    ratio_name(cw > 0 ? cw : 1, ch > 0 ? ch : 1, rn, sizeof(rn));
    snprintf(labels[0], sizeof(labels[0]), "Resolution: %dx%d (%s) %s", cw, ch, rn, opened ? "-" : "+");
    last = r[2];
    if (opts && opts_open) goto options;    /* (the options open: Resolution and Display step aside, for room) */
    y = row + last + 1;
    if (!esc_call_append(panel, x, HEADER_ID, labels[0], y)) return;
    last = y;
    if (opened) {
        mode m[MAX_MODES];
        uint32_t bitmap = U32((uintptr_t)panel), bw = bitmap ? U32((uintptr_t)bitmap + 0x14) : 0, bh = bitmap ? U32((uintptr_t)bitmap + 0x18) : 0;
        const uint32_t after = (opts ? 2u : 1u);  /* (the rows of the module's still to come: Display, Map zoom options) */
        uint32_t step = row + 1, room = bh > y + after * step + 5 * step + 8 ? (bh - (y + after * step + 5 * step + 8)) / (step ? step : 1) : 0;
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
options:
    opt_first = opt_end = 0;
    if (opts && opts->count() > 0) {        /* the module's options: a row, and opened, one row each under it */
        snprintf(opt_labels[0], sizeof(opt_labels[0]), "Map zoom options %s", opts_open ? "-" : "+");
        y = last + row + 1;
        if (esc_call_append(panel, x, OPTS_ID, opt_labels[0], y)) {
            last = y;
            if (opts_open) {
                /* (room: the rows left above WA's own still to come -- at most 4 and the Volume slider: 5 steps) */
                uint32_t bitmap = U32((uintptr_t)panel), bh = bitmap ? U32((uintptr_t)bitmap + 0x18) : 0, step = row + 1;
                uint32_t room = bh > last + 5 * step + 8 ? (bh - (last + 5 * step + 8)) / (step ? step : 1) : 0;
                int n = opts->count(), starts[MAX_OPTS + 1], pages = 0, s;
                static int said_pages = -1, said_room = -1;
                if (n > MAX_OPTS) n = MAX_OPTS;
                if ((uint32_t)n <= room) { starts[0] = 0; pages = 1; }
                else if (room >= 2) {               /* pages of room - 1 options (and the "More" row): whole groups where
                                                       they fit, a group bigger than a page split */
                    const int per = (int)room - 1;
                    for (s = 0; s < n; ) {
                        int end = s;
                        while (end < n) {
                            int g = end + 1;
                            while (g < n && opts->group && opts->group(g) == opts->group(end)) g++;
                            if (g - s > per) break;
                            end = g;
                        }
                        if (end == s) end = s + per < n ? s + per : n;
                        starts[pages++] = s;
                        s = end;
                    }
                } else { starts[0] = 0; pages = 1; n = (int)room; }   /* (no room for a page and its "More" row) */
                starts[pages] = n;
                if (pages != said_pages || (int)room != said_room) {
                    said_pages = pages; said_room = (int)room;
                    if (pages > 1) say("Esc menu: room for %u rows (menu bitmap height %u): the options in %d pages", room, bh, pages);
                    else if (n < opts->count()) say("Esc menu: room for %u of %d options (menu bitmap height %u)", room, opts->count(), bh);
                }
                if (opts_page >= pages) opts_page = 0;
                for (k = starts[opts_page]; k < starts[opts_page + 1]; k++) {
                    opts->label(k, opt_labels[1 + k], sizeof(opt_labels[0]));
                    y = last + step;
                    if (!esc_call_append(panel, x, OPT_ID + (uint32_t)k, opt_labels[1 + k], y)) { say("Esc menu: the panel is full at option %d", k); break; }
                    last = y;
                    if (k == starts[opts_page]) opt_first = k;
                    opt_end = k + 1;
                }
                if (pages > 1) {
                    snprintf(more_label, sizeof(more_label), "More options (%d/%d) +", opts_page + 1, pages);
                    y = last + step;
                    if (esc_call_append(panel, x, MORE_ID, more_label, y)) last = y;
                }
            }
        }
    }
    r[2] = last;                            /* (EBP: WA's own items follow this row) */
}

void escmenu_set_options(const escmenu_options *o) { opts = o; }

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
    if (id == HEADER_ID) { opened = !opened; if (opened) opts_open = 0; refresh(runtime_now); return 0; }
    if (id == OPTS_ID) { opts_open = !opts_open; if (opts_open) { opened = 0; opts_page = 0; } refresh(runtime_now); return 0; }
    if (id == MORE_ID) { opts_page++; refresh(runtime_now); return 0; }   /* (past the last page: the first) */
    if (id >= OPT_ID && id < OPT_ID + MAX_OPTS) {
        const int i = (int)(id - OPT_ID);
        if (opts && i >= opt_first && i < opt_end) opts->click(i);
        refresh(runtime_now);
        return 0;
    }
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

/* ---- the chat laid out again after a size change -----------------------------------------------------------------
 * WA lays the chat out once, as the game starts (GameRuntime's constructor 00526500, from the size DisplayGfx's GetSize
 * 0056A460 gives): the chat's width runtime +0x384 = screen width - the team column +0x388 (00526F9D), the history
 * BitmapImage +0x18 (+0x384 - 7 wide, 00526FD1), the panel +0x1C (the screen's width, 00527034), the input TextBox +0x24
 * (its bitmap and +0x440: +0x384 - 3, 005270FD) and the screen's size kept in the world +0x77B8 / +0x77BC (0052670E).
 * Every frame GameRender centres the panel on the new width / 2 (005342C5, slot 20 0056B660), so an old, wider panel
 * hangs (old - new) / 2 off both sides: the start of every line off the screen (the owner's, 2026-11-22). 00526500
 * builds the whole runtime and cannot run again, so the sizes are set here as it sets them: each bitmap made anew in
 * place by WA's own BitmapImage constructor 004F6370 (WA keeps pointers to the objects; the runtime's destructor at the
 * game's end, 00528F2A, frees their pixels with 005C0AE8, WA's heap), the history's rows copied across (WA keeps it
 * only as pixels), new columns in WA's chat colour (world +0x7328, as 00527154 fills it). WA's blitter 004F6910 keeps
 * within its target only, so +0x384 is set last and never wider than the bitmaps it is read from (005339AF, 00533816).
 * The line height and fonts (from the game's starting height, 720 and 600 the steps) stay the game's. */
#define BMP_VT 0x664144u                /* the chat's BitmapImages (slot 0 0x4F9090 the fill, slot 3 0x4F5DE0 delete) */
#define WA_DELETE_VA 0x5c0ae8u          /* WA's operator delete (cdecl), as the bitmaps' destructor frees their pixels */

uint32_t __cdecl esc_bitmap_make(uintptr_t bmp, uint32_t w, uint32_t h, uint32_t bpp);
void __cdecl esc_textbox_fit(uintptr_t tb);
void *esc_bitmap_ctor_fn, *esc_textbox_fit_fn;
__asm__(
    ".text\n"
    /* BitmapImage's constructor 004F6370 (ESI the object, ECX bits a pixel, EDI the height; the width; ret 4) from C */
    ".globl _esc_bitmap_make\n"
    "_esc_bitmap_make:\n"
    "    pushl %ebx\n"
    "    pushl %esi\n"
    "    pushl %edi\n"
    "    pushl %ebp\n"
    "    movl 20(%esp), %esi\n"
    "    movl 28(%esp), %edi\n"
    "    movl 32(%esp), %ecx\n"
    "    pushl 24(%esp)\n"                  /* the width */
    "    call *_esc_bitmap_ctor_fn\n"
    "    popl %ebp\n"
    "    popl %edi\n"
    "    popl %esi\n"
    "    popl %ebx\n"
    "    ret\n"
    /* the TextBox's scroll fitted to its bitmap 005641E0 (ESI the TextBox; ret), as WA does after a key, from C */
    ".globl _esc_textbox_fit\n"
    "_esc_textbox_fit:\n"
    "    pushl %ebx\n"
    "    pushl %esi\n"
    "    pushl %edi\n"
    "    pushl %ebp\n"
    "    movl 20(%esp), %esi\n"
    "    call *_esc_textbox_fit_fn\n"
    "    popl %ebp\n"
    "    popl %edi\n"
    "    popl %esi\n"
    "    popl %ebx\n"
    "    ret\n");

static int chat_ok, chat_fix_off;       /* (WA's chat code as in 3.8.1; test: the fix off, to see the old way) */

/* a chat BitmapImage made new_w wide in place, its height kept; keep: its rows copied across; new pixels in colour.
 * 1: new_w wide */
static int chat_bitmap(uintptr_t bmp, int new_w, int keep, uint8_t colour, const char *what) {
    uint8_t saved[0x2c];
    uintptr_t old_px;
    uint32_t old_stride, stride;
    int old_w, h, y, n;
    if (!bmp || new_w <= 0) return 0;
    old_w = (int)U32(bmp + 0x14);
    if (old_w == new_w) return 1;
    if (U32(bmp) != VA(BMP_VT) || U32(bmp + 4) || U32(bmp + 0xc) != 8 || !U32(bmp + 8)) {
        say("chat: the %s bitmap is not as WA made it (vtable %08x, not its own %u, %u bits): left %d wide", what,
            (unsigned)(U32(bmp) - base + 0x400000u), U32(bmp + 4), U32(bmp + 0xc), old_w);
        return 0;
    }
    {   /* (WA's constructor 004F6370 memsets its new block unchecked: room for it made sure of first) */
        void *room = malloc((size_t)((new_w + 3) & ~3) * (size_t)U32(bmp + 0x18) + 64);
        if (!room) { say("chat: no memory for the %s bitmap %d wide: left %d wide", what, new_w, old_w); return 0; }
        free(room);
    }
    memcpy(saved, (const void *)bmp, sizeof(saved));
    old_px = U32(bmp + 8); old_stride = U32(bmp + 0x10); h = (int)U32(bmp + 0x18);
    esc_bitmap_make(bmp, (uint32_t)new_w, (uint32_t)h, 8);  /* (WA's: the stride, pixels on WA's heap, size, clip, +4 0) */
    U32(bmp) = VA(BMP_VT);                  /* (the constructor leaves 006640EC; WA's callers store 00664144) */
    if (!U32(bmp + 8)) { memcpy((void *)bmp, saved, sizeof(saved)); say("chat: no memory for the %s bitmap %d wide", what, new_w); return 0; }
    stride = U32(bmp + 0x10);
    n = keep ? (old_w < new_w ? old_w : new_w) : 0;
    for (y = 0; y < h; y++) {
        uint8_t *row = (uint8_t *)(uintptr_t)(U32(bmp + 8) + (uint32_t)y * stride);
        if (n) memcpy(row, (const uint8_t *)(old_px + (uintptr_t)y * old_stride), (size_t)n);
        memset(row + n, colour, (size_t)(new_w - n));
    }
    ((void (__cdecl *)(void *))VA(WA_DELETE_VA))((void *)old_px);
    return 1;
}

/* the chat (and the world's copy of the screen's size) laid out for w x h, between frames; at the same width nothing */
static void relayout_hud(uintptr_t runtime, uintptr_t display, int w, int h) {
    uintptr_t world, hist, panel, tb, input;
    int col, chat, fit, was, line;
    uint8_t bg;
    if (!chat_ok || chat_fix_off || !runtime || w <= 0 || h <= 0 || !(world = U32(runtime + 0x488))) return;
    if (display && ((int)U32(display + 0x3548) != w || (int)U32(display + 0x354c) != h))
        say("chat: the display is %ux%u, the frame %dx%d: laid out for the frame", U32(display + 0x3548), U32(display + 0x354c), w, h);
    U32(world + 0x77b8) = (uint32_t)w; U32(world + 0x77bc) = (uint32_t)h;
    hist = U32(runtime + 0x18); panel = U32(runtime + 0x1c); tb = U32(runtime + 0x24); input = tb ? U32(tb) : 0;
    col = (int)U32(runtime + 0x388); was = (int)U32(runtime + 0x384); chat = w - col;
    if (!hist || !panel || !input || col <= 0 || chat < 16) {
        say("chat: not laid out for %dx%d (history %08x, panel %08x, input %08x, team column %d)", w, h,
            (unsigned)hist, (unsigned)panel, (unsigned)input, col);
        return;
    }
    if (was == chat && (int)U32(panel + 0x14) == w && (int)U32(hist + 0x14) == chat - 7 && (int)U32(input + 0x14) == chat - 3) return;
    bg = (uint8_t)U32(world + 0x7328);
    chat_bitmap(panel, w, 0, bg, "panel");              /* (drawn again each frame; the history copied in below) */
    chat_bitmap(hist, chat - 7, 1, bg, "history");      /* (its lines kept: wrapped as they were) */
    chat_bitmap(input, chat - 3, 0, (uint8_t)U32(tb + 0x43c), "input");
    fit = chat;                                         /* (never wider than what 005336E0 copies from) */
    if ((int)U32(hist + 0x14) + 7 < fit) fit = (int)U32(hist + 0x14) + 7;
    if ((int)U32(input + 0x14) + 3 < fit) fit = (int)U32(input + 0x14) + 3;
    U32(runtime + 0x384) = (uint32_t)fit;
    U32(tb + 0x440) = U32(input + 0x14);                /* (its fill and centred messages, 00564480) */
    U32(tb + 0x14) = 0;                                 /* (the typed text's start: 005641E0 only moves it on) */
    if ((int)U32(tb + 0x20) > 0) esc_textbox_fit(tb);
    U32(tb + 0x18) = 1;                                 /* (the input drawn again) */
    U32(runtime + 0x404) = 1;                           /* (the history copied into the new panel, 0053397F) */
    line = (int)U32(runtime + 0x2b0);
    say("chat: laid out for %dx%d: panel %u wide, chat %d -> %d (history %u, input %u), team column %d, lines %d px%s",
        w, h, U32(panel + 0x14), was, fit, U32(hist + 0x14), U32(input + 0x14), col, line,
        line != (h < 720 ? 12 : 16) ? " (the game's: WA picks its fonts once, at the start)" : "");
}

/* ---- WA's display setup ------------------------------------------------------------------------------------------- */
/* WA's Direct3D 9 backends (the RenderContext's +0x18): vtables 006767B0 (software palette, 0x84 bytes) and 006767F8
 * (shader palette, 0x90 bytes); +0x18 the window, +0x1C / +0x20 the back buffer's size, +0x34 lost, +0x35 device kept
 * by a close, +0x3C IDirect3D9, +0x40 the device, +0x44 D3DPRESENT_PARAMETERS, +0x7C the frame texture, +0x80 the
 * vertex buffer, shader only: +0x84 the pixel shader, +0x88 the palette texture */
#define SW_VT 0x6767b0u
#define SH_VT 0x6767f8u
typedef uint32_t *(__fastcall *close_fn)(uintptr_t, uint32_t *, uint32_t);          /* ECX, EDX the result; keep; ret 4 */
typedef uint32_t *(__fastcall *init_fn)(uintptr_t, uint32_t *, uint32_t, uint32_t); /* ECX, EDX the result; w, h; ret 8 */

static uintptr_t backend(void) { uintptr_t rc = U32(VA(RC_VA)); return rc ? U32(rc + 0x18) : 0; }
static int d3d9(uintptr_t be) { return be && (U32(be) == VA(SW_VT) || U32(be) == VA(SH_VT)); }
/* a Direct3D 9 backend with its device and textures, or with its device marked lost (WA's own check, 0059C960, then
 * resets it and makes the textures before any frame uses them) */
static int d3d9_whole(uintptr_t be) {
    if (!U32(be + 0x40)) return 0;
    if (*(uint8_t *)(be + 0x34)) return 1;
    if (!U32(be + 0x7c) || !U32(be + 0x80)) return 0;
    return U32(be) != VA(SH_VT) || (U32(be + 0x84) && U32(be + 0x88));
}

static void backend_log(const char *when) {
    uintptr_t be = backend();
    if (!d3d9(be)) {
        say("display %s: renderer vtable %08x (not Direct3D 9), frame %ux%u", when,
            be ? (unsigned)(U32(be) - base + 0x400000u) : 0u, U32(VA(0x7a0ef0)), U32(VA(0x7a0ef4)));
        return;
    }
    say("display %s: Direct3D 9 (%s palette) device %08x, lost %u, kept %u, textures %08x %08x, back buffer %ux%u %s, "
        "frame %08x %ux%u", when, U32(be) == VA(SH_VT) ? "shader" : "software", U32(be + 0x40), *(uint8_t *)(be + 0x34),
        *(uint8_t *)(be + 0x35), U32(be + 0x7c), U32(be) == VA(SH_VT) ? U32(be + 0x88) : 0u, U32(be + 0x1c), U32(be + 0x20),
        windowed() ? "windowed" : "fullscreen", U32(VA(0x7a0eec)), U32(VA(0x7a0ef0)), U32(VA(0x7a0ef4)));
}

/* WA's result: 0 its success (008AC8B4, 008ACCD4: never written, so 0 -- and FlushRender tests it with 'test eax,eax'),
 * else a record {0 (0x830: lost), what, HRESULT} of its error ring 008ACE98 */
static void say_failed(const char *step, uint32_t r) {
    const char *what = r ? (const char *)(uintptr_t)U32(r + 4) : NULL;
    say("display setup: %s failed: %s %08x", step, what ? what : (r && U32(r) == 0x830u ? "device lost" : "?"), r ? U32(r + 8) : 0u);
}

/* WA's own display setup, as at every game's start: RunGame (004EC540) closes the RenderContext keeping the device
 * (slot 7, 005A24A0, keep 1, at 004EC5D5), then DisplayGfx::Init sets it up at the game's size (slot 6, 005A2430, at
 * 00569DBB). The close: Direct3D 9's (0059C730) marks the device kept (+0x35 = 1) and releases nothing; the RenderContext
 * frees the 8-bit frame 007A0EEC and zeroes its size 007A0EF0 / 007A0EF4. The set-up: a new zeroed frame of w x h, then
 * the backend's init (0059CC50 -> 0059C2F0): the size into +0x1C / +0x20, and as the device is kept, TestCooperativeLevel
 * (lost: marked lost, done; WA's own check resets it later) and the Reset (0059CA50): textures released (slot 16), the
 * present parameters filled again from +0x1C / +0x20, the window and WA's windowed flag 007A084C, Reset on the same
 * device, textures made again (slot 15); then +0x35 = 0. keep 0: the device released too and made anew (CreateDevice).
 * Never slot 9 alone (005A2530): its backend step (0059CB00) runs the init with no close first, so Direct3D 9 calls
 * CreateDevice over its live device, and on a failure is left with no device and its old textures (the laptop's crash,
 * 2026-11-22: 0059CF42, SetTexture on a NULL device); and it reports success whatever happened. 1: done */
static int render_setup(int w, int h, int keep, int accept_lost) {
    uintptr_t rc = U32(VA(RC_VA)), be;
    uint32_t r = 0;
    int lost0;
    if (!rc || !(be = U32(rc + 0x18))) { say("display setup: no RenderContext"); return 0; }
    lost0 = d3d9(be) && *(uint8_t *)(be + 0x34);
    present_forget();
    ((close_fn)(uintptr_t)U32(U32(rc) + 7 * 4))(rc, &r, keep ? 1u : 0u);
    if (r != U32(VA(OK_VA))) say_failed("close", r);   /* (then slot 6 closes it again, without keep, as WA's code does) */
    r = 0;
    ((init_fn)(uintptr_t)U32(U32(rc) + 6 * 4))(rc, &r, (uint32_t)w, (uint32_t)h);
    if (r != U32(VA(OK_VA))) { say_failed(keep ? "the set-up on the same device" : "the set-up with a new device", r); return 0; }
    if (d3d9(be) && !d3d9_whole(be)) { say("display setup: done, but WA's Direct3D device or textures are missing"); return 0; }
    if (d3d9(be) && !lost0 && !accept_lost && *(uint8_t *)(be + 0x34)) {
        /* (TestCooperativeLevel said lost: 0059C31C marked it and returned success with no Reset done -- WA resets later,
         * and if that Reset is refused at the new size nothing would take it back: not counted as made) */
        say("display setup: the device is lost just now: not counted as set up");
        return 0;
    }
    return 1;
}

static void set_windowed(int on) { U32(VA(WINDOWED_VA)) = (uint32_t)on; U32(VA(OPTIONS_VA) + 0xa4) = (uint32_t)on; }

/* the display at w x h, windowed or fullscreen, through WA's own setup; failing that, back as it was (on the same
 * device, then with a new one); the display's sizes and frame grid as the frame now is. 1: as asked */
static int display_set(uintptr_t display, int w, int h, int win) {
    const int w0 = (int)U32(VA(0x7a0ef0)), h0 = (int)U32(VA(0x7a0ef4)), win0 = windowed();
    uintptr_t be = backend();
    int ok;
    backend_log("before");
    set_windowed(win);
    ok = render_setup(w, h, 1, 0);
    if (!ok) {
        say("display %dx%d %s: not set up; back to %dx%d %s", w, h, win ? "windowed" : "fullscreen", w0, h0,
            win0 ? "windowed" : "fullscreen");
        set_windowed(win0);
        if (!render_setup(w0, h0, 1, 1)) {
            /* (no new device: on the owner's PC a fullscreen CreateDevice after the device's release failed, lost, again
             * and again -- something kept the old one; the device kept, marked lost, WA's own check (0059C960) resets
             * it each frame until it works, and the frames are skipped meanwhile) */
            say("display: no working display; WA's device marked lost: it resets it until it works (frames skipped meanwhile)");
            if (d3d9(be) && U32(be + 0x40)) *(uint8_t *)(be + 0x34) = 1;
        }
    }
    display_sizes(display, (int)U32(VA(0x7a0ef0)), (int)U32(VA(0x7a0ef4)));
    relayout_hud(runtime_now, display, (int)U32(VA(0x7a0ef0)), (int)U32(VA(0x7a0ef4)));   /* (the chat for the frame now) */
    backend_log("after");
    return ok;
}

/* a fullscreen size Direct3D takes: one of this monitor's 32-bit modes (Reset fails, D3DERR_INVALIDCALL, on others) */
static int is_mode(int w, int h) {
    DEVMODEA dm;
    DWORD k;
    for (k = 0;; k++) {
        memset(&dm, 0, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsA(NULL, k, &dm)) return 0;
        if ((int)dm.dmPelsWidth == w && (int)dm.dmPelsHeight == h && dm.dmBitsPerPel == 32) return 1;
    }
}

static void desktop_mode(int *w, int *h) {
    DEVMODEA dm;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm)) { *w = (int)dm.dmPelsWidth; *h = (int)dm.dmPelsHeight; }
    else { *w = GetSystemMetrics(SM_CXSCREEN); *h = GetSystemMetrics(SM_CYSCREEN); }
}

/* the size kept where WA keeps it (GameInfo, WA's options in memory, and its setting unless in test mode) */
static void keep_size(int w, int h) {
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
}

static void apply_resolution(uintptr_t runtime, uintptr_t display, int w, int h) {
    HWND wnd = (HWND)(uintptr_t)U32(VA(WND_VA));
    char before[160], after[160];
    if (wnd) rects(wnd, before, sizeof(before)); else strcpy(before, "no window");
    if (!windowed() && !test_any_size && !is_mode(w, h)) { say("resolution %dx%d: not a mode of this monitor, not applied", w, h); return; }
    if (!display_set(display, w, h, windowed())) {
        mouse_centre(wnd, (int)U32(VA(0x7a0ef0)), (int)U32(VA(0x7a0ef4)));
        say("resolution %dx%d: not applied (before: %s)", w, h, before);
        refresh(runtime);
        return;
    }
    if (!windowed() && wnd) MoveWindow(wnd, 0, 0, w, h, FALSE);
    mouse_centre(wnd, w, h);
    keep_size(w, h);
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
        /* to windowed: not topmost, the caption marked removed (it is) so WA's windowed window setup (004EC0D0 with 0:
         * the window only) puts it back; the display set up again on the same device, windowed (where 4.1 called
         * 004EC0D0 with 1, whose RenderContext slot 6 closes without keep and makes a new device); the window setup
         * again (leaving fullscreen, Direct3D puts back the style and place it saved when it went fullscreen) */
        SetWindowPos(wnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        *(uint8_t *)VA(0x6b32aa) = 1;
        ((setup_fn)VA(0x4ec0d0))(0);
        if (display_set(display, w, h, 1)) {
            *(uint8_t *)VA(0x6b32aa) = 1;
            ((setup_fn)VA(0x4ec0d0))(0);
        } else {                            /* (still fullscreen: no caption, over the screen, topmost) */
            ((modify_fn)VA(0x5c6350))(cwnd, 0x00c00000u, 0, 0);
            *(uint8_t *)VA(0x6b32aa) = 0;
            SetWindowPos(wnd, HWND_TOPMOST, 0, 0, w, h, SWP_SHOWWINDOW | SWP_FRAMECHANGED);
        }
    } else {
        /* to fullscreen: at the frame's size if it is one of the monitor's modes, else at the desktop's; no caption,
         * the display set up again on the same device, the window over the screen and topmost */
        int fw = w, fh = h;
        if (!is_mode(w, h)) { desktop_mode(&fw, &fh); say("display switch: %dx%d is not a mode of this monitor: fullscreen at %dx%d", w, h, fw, fh); }
        ((modify_fn)VA(0x5c6350))(cwnd, 0x00c00000u, 0, 0);
        *(uint8_t *)VA(0x6b32aa) = 0;
        if (display_set(display, fw, fh, 0)) {
            SetWindowPos(wnd, HWND_TOPMOST, 0, 0, fw, fh, SWP_SHOWWINDOW | SWP_FRAMECHANGED);
            if (fw != w || fh != h) keep_size(fw, fh);
        } else {                            /* (still windowed: the caption back, the window placed) */
            *(uint8_t *)VA(0x6b32aa) = 1;
            ((setup_fn)VA(0x4ec0d0))(0);
        }
    }
    mouse_centre(wnd, (int)U32(VA(0x7a0ef0)), (int)U32(VA(0x7a0ef4)));
    rects(wnd, after, sizeof(after));
    say("display switch: before: %s; after: %s", before, after);
    refresh(runtime);
}

/* the safety net's recovery (present.c: WA's show-frame found no device): the backend's own close without keep
 * (0059C730: its textures released, +0x35 cleared) and init at its size (CreateDevice anew) -- the backend's, not the
 * RenderContext's, so WA's 8-bit frame and the display's grid stay as they are (this also runs in the frontend) */
/* mode 0: WA has no device (the safety net); 1: its Reset keeps failing (the safety net, 'stuck'); 2: test (always).
 * Five tries an incident (a device seen again, or made, starts a new one) */
static int tries;
static void recover(int mode) {
    uintptr_t be = backend();
    uint32_t r = 0;
    HWND wnd = (HWND)(uintptr_t)U32(VA(WND_VA));
    const int in_game = runtime_now && GetTickCount() - runtime_tick < 2000;   /* (before any slow device call) */
    if (!d3d9(be)) return;
    if (mode == 0 && U32(be + 0x40)) { tries = 0; return; }    /* (set up again meanwhile) */
    if (mode != 2 && tries >= 5) return;
    if (mode != 2 && ++tries == 5) say("display: the last try to make WA's Direct3D device again (restart WA if it fails)");
    backend_log("recovery, before");
    present_forget();
    ((close_fn)(uintptr_t)U32(U32(be) + 7 * 4))(be, &r, 0);
    r = 0;
    ((init_fn)(uintptr_t)U32(U32(be) + 6 * 4))(be, &r, U32(be + 0x1c), U32(be + 0x20));
    if (r != U32(VA(OK_VA))) {
        say_failed("the recovery", r);
        if (!windowed() && wnd && in_game) {
            /* fullscreen: a new device can be refused (lost) while something still holds the old one -- the owner's PC,
             * 2026-11-22 test -- so the game goes on windowed (its window as the display switch makes it); whatever the
             * failed init left (a device made, its textures not) released first, never an init over a live device */
            typedef void (__stdcall *setup_fn)(uint32_t);
            say("display: the recovery goes windowed");
            r = 0;
            ((close_fn)(uintptr_t)U32(U32(be) + 7 * 4))(be, &r, 0);
            SetWindowPos(wnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            set_windowed(1);
            *(uint8_t *)VA(0x6b32aa) = 1;
            ((setup_fn)VA(0x4ec0d0))(0);
            r = 0;
            ((init_fn)(uintptr_t)U32(U32(be) + 6 * 4))(be, &r, U32(be + 0x1c), U32(be + 0x20));
            if (r != U32(VA(OK_VA))) say_failed("the recovery, windowed", r);
            *(uint8_t *)VA(0x6b32aa) = 1;
            ((setup_fn)VA(0x4ec0d0))(0);
            mouse_centre(wnd, (int)U32(VA(0x7a0ef0)), (int)U32(VA(0x7a0ef4)));
            refresh(runtime_now);
        }
    }
    if (U32(be + 0x40) && d3d9_whole(be)) tries = 0;
    backend_log("recovery, after");
}

/* from the safety net (inside WA's show-frame: only a message posted; the recovery runs between frames) */
void escmenu_recover(int stuck) {
    const int want = stuck ? 2 : 1;
    if (pending_recover < want) { pending_recover = want; post_switch(); }
}
void escmenu_device_back(void) { tries = 0; }
void escmenu_test_recover(void) { say("test: WA's Direct3D device made anew, as the recovery does"); recover(2); }

int escmenu_message(const MSG *m) {
    uintptr_t world, display;
    if (m->message != MSG_SWITCH) return 0;
    if (busy) { post_switch(); return 1; } /* (a switch running: this one after it) */
    busy = 1;
    if (pending_recover) { const int mode = pending_recover == 2 ? 1 : 0; pending_recover = 0; recover(mode); }
    if (runtime_now && GetTickCount() - runtime_tick > 2000) {
        /* (no game frame for 2 s: the runtime is not used -- WA frees it at the game's end, see esc_runtime_gone) */
        if (pending_w || pending_toggle) say("display: the switch asked for is dropped (no game frame for 2 s)");
        pending_w = pending_h = pending_toggle = 0;
        runtime_now = 0;
    }
    if ((pending_w || pending_toggle) && runtime_now && (world = U32(runtime_now + 0x488)) && (display = U32(world + 4))) {
        if (pending_w) { int w = pending_w, h = pending_h; pending_w = pending_h = 0; apply_resolution(runtime_now, display, w, h); }
        if (pending_toggle) { pending_toggle = 0; apply_toggle(runtime_now, display); }
    }
    busy = 0;
    return 1;
}

/* GameRuntime's destructor (00528CD0, EAX the runtime; from DestroyGlobalContext 0056DFB0 at the game's end): the
 * module's pointer to it dropped as WA frees it, and any switch still asked for with it (a click in the last frame) */
void *esc_orig_runtime_gone;
void __cdecl esc_runtime_gone(uintptr_t rt);
void esc_thunk_runtime_gone(void);
__asm__(
    ".text\n"
    ".globl _esc_thunk_runtime_gone\n"
    "_esc_thunk_runtime_gone:\n"
    "    pushal\n"
    "    pushl %eax\n"
    "    call _esc_runtime_gone\n"
    "    addl $4, %esp\n"
    "    popal\n"
    "    jmp *_esc_orig_runtime_gone\n");
void __cdecl esc_runtime_gone(uintptr_t rt) {
    if (!rt || rt != runtime_now) return;
    if (pending_w || pending_toggle) say("display: the switch asked for is dropped (the game is over)");
    pending_w = pending_h = pending_toggle = 0;
    runtime_now = 0;
    opened = opts_open = 0;
}

/* what the display's safety net and the recovery need, with or without the Esc menu's items (installed first) */
int escmenu_base(uintptr_t b, void (*log)(const char *, ...)) {
    static const unsigned char gone_bytes[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x51, 0x53, 0x56, 0x8b, 0xf0, 0x83, 0xbe,
                                               0x38, 0x04, 0x00, 0x00, 0x00};
    base = b; say = log;
    if (memcmp((void *)VA(0x528cd0), gone_bytes, sizeof(gone_bytes)) ||
        MH_CreateHook((void *)VA(0x528cd0), (void *)esc_thunk_runtime_gone, &esc_orig_runtime_gone) != MH_OK) {
        say("the game's end could not be hooked: the Esc menu's switches unavailable");
        return 0;
    }
    return 1;
}

void escmenu_frame(void *runtime) { if (runtime) { runtime_now = (uintptr_t)runtime; runtime_tick = GetTickCount(); } }
void escmenu_test_any_size(int w, int h) { test_any_size = 1; escmenu_ask(w, h); }

/* test: a line in WA's chat (ShowChatMessage 0052ACB0, stdcall: the runtime, colour (+0x298, as WA's own), text, 0;
 * ret 0x10); pin: the chat pinned open (runtime +0x414, ChatPinned: its pinned lines shown with the chat closed) */
void escmenu_test_chat(const char *text, int pin) {
    typedef void (__stdcall *chat_fn)(uintptr_t, uint32_t, const char *, uint32_t);
    if (!runtime_now) return;
    if (pin) *(uint8_t *)(runtime_now + 0x414) = 1;
    ((chat_fn)VA(0x52acb0))(runtime_now, U32(runtime_now + 0x298), text, 0);
}

/* test: the chat's sizes against the frame's */
void escmenu_test_chat_check(const char *when) {
    uintptr_t r = runtime_now, tb, world;
    int w, pw, chat, col, hw, iw, tw, ok;
    if (!r || !U32(r + 0x18) || !U32(r + 0x1c) || !(tb = U32(r + 0x24)) || !U32(tb) || !(world = U32(r + 0x488))) return;
    w = (int)U32(VA(0x7a0ef0)); pw = (int)U32(U32(r + 0x1c) + 0x14); chat = (int)U32(r + 0x384); col = (int)U32(r + 0x388);
    hw = (int)U32(U32(r + 0x18) + 0x14); iw = (int)U32(U32(tb) + 0x14); tw = (int)U32(tb + 0x440);
    ok = pw == w && chat + col == w && hw == chat - 7 && iw == chat - 3 && tw == iw && (int)U32(world + 0x77b8) == w;
    say("test: chat %s: %s (frame %d wide, panel %d, chat %d + team column %d, history %d, input %d / %d, world %u)", when,
        ok ? "laid out for the frame" : "NOT laid out for the frame", w, pw, chat, col, hw, iw, tw, U32(world + 0x77b8));
}

void escmenu_test_chatfix(int on) { chat_fix_off = !on; }

void escmenu_test_open(void) {
    if (!runtime_now) return;
    ((void (__stdcall *)(uintptr_t))VA(0x535200))(runtime_now);
    say("test: WA's Esc menu opened (%s)", U32(runtime_now + 0x434) == 1 ? "open" : "not open");
}

void escmenu_test_more(void) {
    opts_page++;
    say("test: the options' next page");
    refresh(runtime_now);
}

void escmenu_test_options(void) {
    opts_open = 1;
    opts_page = 0;
    opened = 0;
    say("test: the options opened (Esc menu %s)", runtime_now && U32(runtime_now + 0x434) == 1 ? "open" : "not open");
    refresh(runtime_now);
}

void escmenu_test_click(int i) {
    if (opts && i >= 0 && i < opts->count()) opts->click(i);
    refresh(runtime_now);
}

void escmenu_test_list(void) {
    opened = 1;
    opts_open = 0;
    say("test: the resolution list opened (Esc menu %s)", runtime_now && U32(runtime_now + 0x434) == 1 ? "open" : "not open");
    refresh(runtime_now);
}

int escmenu_install(uintptr_t b, void (*log)(const char *, ...), int keep) {
    static const unsigned char first_bytes[] = {0x6a, 0x03, 0x89, 0x44, 0x24, 0x44, 0xe8, 0x1d, 0xb2, 0x00, 0x00, 0x8b, 0x83, 0x88, 0x04, 0x00, 0x00};
    static const unsigned char activate_bytes[] = {0x53, 0x8b, 0x5c, 0x24, 0x08, 0x56, 0x8b, 0x73, 0x14};
    static const unsigned char open_bytes[] = {0x81, 0xec, 0x08, 0x04, 0x00, 0x00, 0x53};
    static const unsigned char append_bytes[] = {0x83, 0xbe, 0xb0, 0x03, 0x00, 0x00, 0x10, 0x53};
    base = b; say = log; keep_setting = keep;
    {
        static const unsigned char ctor_bytes[] = {0x53, 0x55, 0x8b, 0x6c, 0x24, 0x0c, 0x8b, 0xc5, 0x0f, 0xaf, 0xc1};
        static const unsigned char fit_bytes[] = {0x8b, 0x06, 0x8b, 0x40, 0x14, 0x83, 0xec, 0x08, 0x55, 0x57};
        static const unsigned char delete_bytes[] = {0xe9, 0x3e, 0x02, 0x01, 0x00};
        static const unsigned char layout_bytes[] = {0x8b, 0x4c, 0x24, 0x44, 0x2b, 0xc8, 0x6a, 0x4c, 0x89, 0x8d, 0x84, 0x03, 0x00, 0x00};
        static const unsigned char tbw_bytes[] = {0x89, 0x83, 0x40, 0x04, 0x00, 0x00};
        static const unsigned char copied_bytes[] = {0xc7, 0x87, 0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        chat_ok = !memcmp((void *)VA(0x4f6370), ctor_bytes, sizeof(ctor_bytes)) && !memcmp((void *)VA(0x5641e0), fit_bytes, sizeof(fit_bytes)) &&
                  !memcmp((void *)VA(0x5c0ae8), delete_bytes, sizeof(delete_bytes)) && !memcmp((void *)VA(0x526f9d), layout_bytes, sizeof(layout_bytes)) &&
                  !memcmp((void *)VA(0x56406b), tbw_bytes, sizeof(tbw_bytes)) && !memcmp((void *)VA(0x5339bc), copied_bytes, sizeof(copied_bytes)) &&
                  U32(VA(BMP_VT)) == VA(0x4f9090) && U32(VA(BMP_VT) + 12) == VA(0x4f5de0);
        esc_bitmap_ctor_fn = (void *)VA(0x4f6370);
        esc_textbox_fit_fn = (void *)VA(0x5641e0);
        if (!chat_ok) say("chat: WA's chat code is not as in WA 3.8.1: after a size change the chat keeps the game's starting size");
    }
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
