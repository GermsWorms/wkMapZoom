/* wkMapZoom's zoom readout as a picture: "150%" in white on a rounded, see-through black box. GDI only, no Direct3D or
 * game calls (native test: zoom_badge_test.c). */
#ifndef WKMAPZOOM_BADGE_H
#define WKMAPZOOM_BADGE_H
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

#define BADGE_KEY 0x00ff00ffu           /* GDI's "nothing here" colour (outside the rounded box) */

/* the font and box sizes for a screen of screen_h lines (1080 lines: a 28 px number on a 100 x 44 box) */
static void badge_size(int screen_h, int *font, int *bw, int *bh) {
    int f = screen_h * 28 / 1080;
    if (f < 14) f = 14;
    *font = f;
    *bw = f * 18 / 5;
    *bh = f * 8 / 5;
}

/* paints the readout for `pct` into `out` (tw x th pixels, A8R8G8B8 rows `pitch` bytes apart): the bw x bh box at the
 * top left, everything else clear. Outside the box alpha 0, the box's black 60% opaque, the number opaque white. */
static int badge_paint(int pct, int f, int bw, int bh, int tw, int th, uint32_t *out, int pitch) {
    BITMAPINFO bi;
    uint32_t *px = NULL;
    HDC dc;
    HBITMAP bmp, old_bmp;
    HFONT font, old_font;
    HBRUSH b;
    HGDIOBJ old_pen, old_brush;
    RECT rc;
    char text[16];
    int x, y;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = tw; bi.bmiHeader.biHeight = -th;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    dc = CreateCompatibleDC(NULL);
    bmp = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0) : NULL;
    if (!bmp || !px) { if (dc) DeleteDC(dc); return 0; }
    old_bmp = (HBITMAP)SelectObject(dc, bmp);
    for (x = 0; x < tw * th; x++) px[x] = BADGE_KEY;
    b = CreateSolidBrush(RGB(0, 0, 0));
    old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
    old_brush = SelectObject(dc, b);
    RoundRect(dc, 0, 0, bw + 1, bh + 1, f, f);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(b);
    font = CreateFontA(-f, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    old_font = (HFONT)SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    snprintf(text, sizeof(text), "%d%%", pct);
    rc.left = 0; rc.top = 0; rc.right = bw; rc.bottom = bh;
    DrawTextA(dc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old_font);
    DeleteObject(font);
    GdiFlush();
    for (y = 0; y < th; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)out + (size_t)y * pitch);
        for (x = 0; x < tw; x++) {
            uint32_t c = px[y * tw + x] & 0xffffffu, r = c >> 16, g = (c >> 8) & 0xff, bl = c & 0xff;
            uint32_t m = r > g ? (r > bl ? r : bl) : (g > bl ? g : bl);
            row[x] = c == BADGE_KEY ? 0u : ((150u + 105u * m / 255u) << 24) | c;
        }
    }
    SelectObject(dc, old_bmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    return 1;
}
#endif
