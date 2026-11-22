/* colourmap.c: full-colour maps (wkMapZoom). Ported from wkVista (MIT, Copyright (c) 2026 ropahektic,
 * github.com/ropahektic/wkVista: src/files.rs, src/pngmap.rs, src/land.rs); the idea of handing WA a converted stand-in
 * when it opens a full-colour map was first wkRGBA's (rUNaW4y, github.com/rUNaW4y/wkRGBA).
 *
 * WA opens map files with CreateFileA / CreateFileW (its import table is patched here). When it opens a full-colour
 * image under User\SavedLevels (a PNG with 8-bit RGB or RGBA, or a JPEG -- WA can't load either), it gets a handle to
 * the map's "proxy" instead: the same map as a palette PNG WA accepts, in a temporary file Windows deletes when WA
 * closes it (nothing is kept on disk; the original is never written). WA's map loader takes only an 8-bit palette PNG,
 * at least 640 x 32, sides multiples of 8, palette entry 0 black (air), at most 0x71 colours and a 40-byte "waLV"
 * settings chunk. The proxy: land = alpha >= 128 (black included), air the rest; the land's colours reduced to 64 by
 * median cut (as WA's own image import: WA fills craters with darker copies of the map's colours, which need palette
 * slots of their own); padded with air to multiples of 8 and at least 640 x 32 on the right and on top (a map with no
 * air at all gets 128 rows on top, so worms can be placed); settings: an open map (no borders), its seed from the image,
 * a theme (the soil behind craters and the backdrop come from it). Other players get the proxy from WA as any map.
 *
 * In game (colourmap_match): the game's land (its tiles at the first frame) is matched to the full-colour images whose
 * proxy has the land's size -- the ones WA opened this session first -- by its shape at a few placings; where it
 * matches, each land pixel WA has not changed since shows the original's colour (raster.c). Images are decoded with
 * Windows' own GDI+ (PNG, JPEG). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include "colourmap.h"

static void (*say)(const char *, ...);
static uintptr_t base;
static int theme = 11;
static wchar_t levels_dir[MAX_PATH];
static CRITICAL_SECTION lock;
static HANDLE (WINAPI *orig_cfa)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static HANDLE (WINAPI *orig_cfw)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static LONG temp_n;

/* WA's themes by number (the settings' theme word): WA's own table 00649BA0 (the first five: the drawn-map themes) */
static const char *const themes[29] = {
    "-Beach", "-Desert", "-Farm", "-Forest", "-Hell", "Art", "Cheese", "Construction", "Desert", "Dungeon", "Easter",
    "Forest", "Fruit", "Gulf", "Hell", "Hospital", "Jungle", "Manhattan", "Medieval", "Music", "Pirate", "Snow", "Space",
    "Sports", "Tentacle", "Time", "Tools", "Tribal", "Urban"};

int colourmap_theme(const char *name) {
    int k;
    for (k = 0; k < 29; k++) if (!lstrcmpiA(name, themes[k])) return k;
    return -1;
}

/* ---- images: GDI+ (Windows' own) --------------------------------------------------------------------------------- */
typedef struct { uint32_t w, h; uint32_t *px; } image;   /* 0xAARRGGBB */
typedef struct { UINT32 version; void *debug; BOOL no_thread, no_codecs; } gp_input;
typedef struct { UINT w, h; INT stride; INT format; void *scan0; UINT_PTR reserved; } gp_data;
typedef struct { INT x, y, w, h; } gp_rect;
static int (WINAPI *gp_startup)(ULONG_PTR *, const gp_input *, void *);
static int (WINAPI *gp_from_file)(const WCHAR *, void **);
static int (WINAPI *gp_width)(void *, UINT *);
static int (WINAPI *gp_height)(void *, UINT *);
static int (WINAPI *gp_lock)(void *, const gp_rect *, UINT, INT, gp_data *);
static int (WINAPI *gp_unlock)(void *, gp_data *);
static int (WINAPI *gp_dispose)(void *);
static int gp_state;                    /* 0 not tried, 1 ready, -1 unavailable */

static int gp_ready(void) {
    HMODULE m;
    ULONG_PTR token;
    gp_input in = {1, NULL, FALSE, FALSE};
    if (gp_state) return gp_state > 0;
    gp_state = -1;
    if (!(m = LoadLibraryA("gdiplus.dll"))) { say("full-colour maps: no gdiplus.dll"); return 0; }
    gp_startup = (int (WINAPI *)(ULONG_PTR *, const gp_input *, void *))GetProcAddress(m, "GdiplusStartup");
    gp_from_file = (int (WINAPI *)(const WCHAR *, void **))GetProcAddress(m, "GdipCreateBitmapFromFile");
    gp_width = (int (WINAPI *)(void *, UINT *))GetProcAddress(m, "GdipGetImageWidth");
    gp_height = (int (WINAPI *)(void *, UINT *))GetProcAddress(m, "GdipGetImageHeight");
    gp_lock = (int (WINAPI *)(void *, const gp_rect *, UINT, INT, gp_data *))GetProcAddress(m, "GdipBitmapLockBits");
    gp_unlock = (int (WINAPI *)(void *, gp_data *))GetProcAddress(m, "GdipBitmapUnlockBits");
    gp_dispose = (int (WINAPI *)(void *))GetProcAddress(m, "GdipDisposeImage");
    if (!gp_startup || !gp_from_file || !gp_width || !gp_height || !gp_lock || !gp_unlock || !gp_dispose || gp_startup(&token, &in, NULL)) {
        say("full-colour maps: GDI+ unavailable");
        return 0;
    }
    gp_state = 1;
    return 1;
}

static int decode(const wchar_t *path, image *img) {
    void *bmp = NULL;
    UINT w = 0, h = 0, y;
    gp_rect r;
    gp_data d;
    memset(img, 0, sizeof(*img));
    if (!gp_ready() || gp_from_file(path, &bmp) || !bmp) return 0;
    if (gp_width(bmp, &w) || gp_height(bmp, &h) || !w || !h || w > 0x8000 || h > 0x8000 ||
        !(img->px = (uint32_t *)malloc((size_t)w * h * 4))) { gp_dispose(bmp); return 0; }
    r.x = 0; r.y = 0; r.w = (INT)w; r.h = (INT)h;
    if (gp_lock(bmp, &r, 1 /* read */, 0x26200a /* 32bppARGB */, &d)) { free(img->px); img->px = NULL; gp_dispose(bmp); return 0; }
    for (y = 0; y < h; y++) memcpy(img->px + (size_t)y * w, (const uint8_t *)d.scan0 + (size_t)y * d.stride, (size_t)w * 4);
    gp_unlock(bmp, &d);
    gp_dispose(bmp);
    img->w = w; img->h = h;
    return 1;
}

/* a full-colour image WA can't load itself, by its first bytes: a PNG with 8-bit RGB or RGBA, or a JPEG */
static int full_colour(const uint8_t *b, DWORD n) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    if (n >= 3 && b[0] == 0xff && b[1] == 0xd8 && b[2] == 0xff) return 1;
    return n >= 26 && !memcmp(b, sig, 8) && !memcmp(b + 12, "IHDR", 4) && b[24] == 8 && (b[25] == 6 || b[25] == 2);
}

static int read_head(const wchar_t *path, uint8_t *buf, DWORD want, DWORD *got) {
    HANDLE f = orig_cfw(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    *got = 0;
    ReadFile(f, buf, want, got, NULL);
    CloseHandle(f);
    return 1;
}

/* a full-colour image's size without decoding it (PNG: IHDR; JPEG: its frame header) */
static int image_size(const wchar_t *path, uint32_t *w, uint32_t *h) {
    static uint8_t b[65536];
    DWORD n = 0, i;
    if (!read_head(path, b, sizeof(b), &n) || !full_colour(b, n)) return 0;
    if (b[0] == 0x89) { *w = (uint32_t)b[16] << 24 | b[17] << 16 | b[18] << 8 | b[19]; *h = (uint32_t)b[20] << 24 | b[21] << 16 | b[22] << 8 | b[23]; return 1; }
    for (i = 2; i + 9 < n;) {
        uint32_t len;
        if (b[i] != 0xff) return 0;
        if (b[i + 1] >= 0xc0 && b[i + 1] <= 0xcf && b[i + 1] != 0xc4 && b[i + 1] != 0xc8 && b[i + 1] != 0xcc) {
            *h = (uint32_t)b[i + 5] << 8 | b[i + 6];
            *w = (uint32_t)b[i + 7] << 8 | b[i + 8];
            return 1;
        }
        len = (uint32_t)b[i + 2] << 8 | b[i + 3];
        i += 2 + len;
    }
    return 0;
}

/* the proxy's size for an image of w x h (with or without any air), and the original's top-left in it */
static void layout(uint32_t w, uint32_t h, int any_air, uint32_t *pw, uint32_t *ph, uint32_t *ax, uint32_t *ay) {
    uint32_t top = any_air ? 0 : 128, ww = w < 640 ? 640 : w, hh = h + top < 32 ? 32 : h + top;
    *pw = (ww + 7) / 8 * 8;
    *ph = (hh + 7) / 8 * 8;
    *ax = 0;
    *ay = *ph - h;
}

static int is_land(uint32_t p) { return (p >> 24) >= 128; }

/* ---- median cut: at most 64 colours for the land --------------------------------------------------------------- */
typedef struct { uint32_t c, n, bucket; } ccount;   /* a colour (its bucket's average), its count, its bucket */
static int cut_channel;
static int cc_cmp(const void *a, const void *b) {
    uint32_t x = (((const ccount *)a)->c >> (16 - 8 * cut_channel)) & 255, y = (((const ccount *)b)->c >> (16 - 8 * cut_channel)) & 255;
    return (int)x - (int)y;
}

/* the land's colours (cols: n buckets, counted) into at most k boxes: each bucket's box (box_of, by bucket) and each
 * box's average */
static int median_cut(ccount *cols, int n, int k, uint32_t *pal, uint8_t *box_of) {
    int starts[64], ends[64], nb = 1, i, b;
    starts[0] = 0; ends[0] = n;
    while (nb < k) {
        int best = -1, best_ch = 0, best_range = -1;
        for (b = 0; b < nb; b++) {
            int ch;
            if (ends[b] - starts[b] < 2) continue;
            for (ch = 0; ch < 3; ch++) {
                int lo = 255, hi = 0;
                for (i = starts[b]; i < ends[b]; i++) {
                    int v = (int)((cols[i].c >> (16 - 8 * ch)) & 255);
                    if (v < lo) lo = v;
                    if (v > hi) hi = v;
                }
                if (hi - lo > best_range) { best_range = hi - lo; best = b; best_ch = ch; }
            }
        }
        if (best < 0) break;
        {
            uint64_t total = 0, acc = 0;
            int cut = starts[best] + 1;
            cut_channel = best_ch;
            qsort(cols + starts[best], (size_t)(ends[best] - starts[best]), sizeof(ccount), cc_cmp);
            for (i = starts[best]; i < ends[best]; i++) total += cols[i].n;
            for (i = starts[best]; i < ends[best]; i++) {
                acc += cols[i].n;
                if (acc * 2 >= total) { cut = i + 1; break; }
            }
            if (cut <= starts[best]) cut = starts[best] + 1;
            if (cut >= ends[best]) cut = ends[best] - 1;
            starts[nb] = cut; ends[nb] = ends[best]; ends[best] = cut; nb++;
        }
    }
    for (b = 0; b < nb; b++) {
        uint64_t t = 0, r = 0, g = 0, bl = 0;
        for (i = starts[b]; i < ends[b]; i++) {
            t += cols[i].n;
            r += ((cols[i].c >> 16) & 255) * (uint64_t)cols[i].n;
            g += ((cols[i].c >> 8) & 255) * (uint64_t)cols[i].n;
            bl += (cols[i].c & 255) * (uint64_t)cols[i].n;
            box_of[cols[i].bucket] = (uint8_t)b;
        }
        if (!t) t = 1;
        pal[b] = (uint32_t)(r / t) << 16 | (uint32_t)(g / t) << 8 | (uint32_t)(bl / t);
    }
    return nb;
}

/* ---- PNG writing (stored deflate: WA's loader inflates any zlib stream) ------------------------------------------ */
static uint32_t crc_table[256];
static void crc_init(void) {
    uint32_t c, n, k;
    for (n = 0; n < 256; n++) { for (c = n, k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1; crc_table[n] = c; }
}
static uint32_t crc(uint32_t c, const uint8_t *b, size_t n) {
    c = ~c;
    while (n--) c = crc_table[(c ^ *b++) & 255] ^ (c >> 8);
    return ~c;
}
typedef struct { uint8_t *b; size_t n, cap; int bad; } buf;
static void put(buf *o, const void *p, size_t n) {
    if (o->bad) return;
    if (o->n + n > o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 65536;
        uint8_t *nb;
        while (cap < o->n + n) cap *= 2;
        if (!(nb = (uint8_t *)realloc(o->b, cap))) { o->bad = 1; return; }
        o->b = nb; o->cap = cap;
    }
    memcpy(o->b + o->n, p, n);
    o->n += n;
}
static void put32(buf *o, uint32_t v) { uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v}; put(o, b, 4); }
static void chunk(buf *o, const char *type, const uint8_t *data, size_t n) {
    uint32_t c;
    put32(o, (uint32_t)n);
    put(o, type, 4);
    put(o, data, n);
    if (o->bad) return;
    c = crc(0, (const uint8_t *)type, 4);
    c = crc(c, data, n);
    put32(o, c);
}

/* the proxy PNG of `img` */
static int make_proxy(const image *img, buf *out, uint32_t *pwo, uint32_t *pho) {
    uint32_t pw, ph, ax, ay, x, y, pal[64], n_land = 0;
    uint64_t seed = 0xcbf29ce484222325ull;
    int any_air = 0, n = 0, nb, i;
    ccount *cols = NULL;
    uint8_t *box = NULL, *raw = NULL, hdr[13], plte[65 * 3], walv[40];
    size_t rowb, k;
    buf z = {0};
    for (k = 0; k < (size_t)img->w * img->h; k++) {
        if (!is_land(img->px[k])) any_air = 1; else n_land++;
        seed = (seed ^ img->px[k]) * 0x100000001b3ull;
    }
    layout(img->w, img->h, any_air, &pw, &ph, &ax, &ay);
    if (pw >= 0x6f00 || ph >= 0x7760 || !n_land) return 0;
    /* the land's colours, counted in 18-bit buckets (each bucket's own average kept) */
    {
        uint32_t *cnt = (uint32_t *)calloc(1u << 18, sizeof(uint32_t));
        uint64_t *sum = (uint64_t *)calloc((size_t)3 << 18, sizeof(uint64_t));
        if (!cnt || !sum || !(box = (uint8_t *)calloc(1u << 18, 1)) || !(cols = (ccount *)malloc(sizeof(ccount) << 18))) {
            free(cnt); free(sum); free(box); free(cols);
            return 0;
        }
        for (k = 0; k < (size_t)img->w * img->h; k++) {
            uint32_t p = img->px[k], q;
            if (!is_land(p)) continue;
            q = ((p >> 18) & 63u) << 12 | ((p >> 10) & 63u) << 6 | ((p >> 2) & 63u);
            cnt[q]++;
            sum[3 * q] += (p >> 16) & 255; sum[3 * q + 1] += (p >> 8) & 255; sum[3 * q + 2] += p & 255;
        }
        for (k = 0; k < (1u << 18); k++)
            if (cnt[k]) {
                cols[n].c = (uint32_t)(sum[3 * k] / cnt[k]) << 16 | (uint32_t)(sum[3 * k + 1] / cnt[k]) << 8 | (uint32_t)(sum[3 * k + 2] / cnt[k]);
                cols[n].n = cnt[k]; cols[n].bucket = (uint32_t)k; n++;
            }
        free(cnt); free(sum);
    }
    nb = median_cut(cols, n, 64, pal, box);
    free(cols);
    /* the image rows: filter byte 0 + indices (0 air, 1 + box) */
    rowb = (size_t)pw + 1;
    if (!(raw = (uint8_t *)calloc(rowb * ph, 1))) { free(box); return 0; }
    for (y = 0; y < img->h; y++)
        for (x = 0; x < img->w; x++) {
            uint32_t p = img->px[(size_t)y * img->w + x];
            if (!is_land(p)) continue;
            raw[(size_t)(ay + y) * rowb + 1 + ax + x] = (uint8_t)(1 + box[((p >> 18) & 63u) << 12 | ((p >> 10) & 63u) << 6 | ((p >> 2) & 63u)]);
        }
    free(box);
    /* zlib, stored blocks */
    {
        static const uint8_t zh[2] = {0x78, 0x01};
        size_t total = rowb * ph, at = 0;
        uint32_t a = 1, b2 = 0;
        put(&z, zh, 2);
        while (at < total || !at) {
            size_t len = total - at > 65535 ? 65535 : total - at;
            uint8_t bh[5];
            bh[0] = at + len >= total ? 1 : 0;
            bh[1] = (uint8_t)len; bh[2] = (uint8_t)(len >> 8); bh[3] = (uint8_t)~len; bh[4] = (uint8_t)(~len >> 8);
            put(&z, bh, 5);
            put(&z, raw + at, len);
            at += len;
            if (!len) break;
        }
        for (k = 0; k < total; k++) { a = (a + raw[k]) % 65521u; b2 = (b2 + a) % 65521u; }
        put32(&z, b2 << 16 | a);
    }
    free(raw);
    if (z.bad) { free(z.b); return 0; }
    /* the PNG */
    if (!crc_table[1]) crc_init();
    {
        static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
        put(out, sig, 8);
    }
    hdr[0] = (uint8_t)(pw >> 24); hdr[1] = (uint8_t)(pw >> 16); hdr[2] = (uint8_t)(pw >> 8); hdr[3] = (uint8_t)pw;
    hdr[4] = (uint8_t)(ph >> 24); hdr[5] = (uint8_t)(ph >> 16); hdr[6] = (uint8_t)(ph >> 8); hdr[7] = (uint8_t)ph;
    hdr[8] = 8; hdr[9] = 3; hdr[10] = 0; hdr[11] = 0; hdr[12] = 0;
    chunk(out, "IHDR", hdr, 13);
    memset(plte, 0, sizeof(plte));
    for (i = 0; i < nb; i++) {
        uint32_t c = pal[i] ? pal[i] : 0x080808u;   /* (a land colour of pure black is land, not air) */
        plte[3 + 3 * i] = (uint8_t)(c >> 16); plte[4 + 3 * i] = (uint8_t)(c >> 8); plte[5 + 3 * i] = (uint8_t)c;
    }
    chunk(out, "PLTE", plte, (size_t)(nb + 1) * 3);
    memset(walv, 0, sizeof(walv));
    memcpy(walv, &seed, 8);
    walv[0x10] = 1;
    walv[0x20] = (uint8_t)theme; walv[0x21] = (uint8_t)(theme >> 8); walv[0x22] = 1; walv[0x23] = 0;
    chunk(out, "waLV", walv, sizeof(walv));
    chunk(out, "IDAT", z.b, z.n);
    chunk(out, "IEND", NULL, 0);
    free(z.b);
    *pwo = pw; *pho = ph;
    return !out->bad;
}

/* ---- the session's conversions and the originals WA opened -------------------------------------------------------- */
typedef struct { wchar_t path[MAX_PATH]; FILETIME t; uint8_t *png; size_t n; } conv;
#define MAX_CONV 64
static conv convs[MAX_CONV];
static int nconv;
static wchar_t recent[16][MAX_PATH];    /* latest last */
static int nrecent;

static void remember(const wchar_t *path) {
    int i, j;
    for (i = 0; i < nrecent; i++) if (!lstrcmpiW(recent[i], path)) break;
    if (i == nrecent) { if (nrecent == 16) i = 0; else nrecent++; }
    for (j = i; j < nrecent - 1; j++) wcscpy(recent[j], recent[j + 1]);
    wcsncpy(recent[nrecent - 1], path, MAX_PATH - 1);
    recent[nrecent - 1][MAX_PATH - 1] = 0;
}

/* the proxy for a full-colour image under SavedLevels (NULL for any other file); under the lock */
static conv *proxy_for(const wchar_t *path) {
    wchar_t lower[MAX_PATH];
    uint8_t head[26];
    DWORD got = 0;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    image img;
    buf out = {0};
    uint32_t pw = 0, ph = 0;
    DWORD t0;
    int i;
    size_t k;
    for (k = 0; path[k] && k < MAX_PATH - 1; k++) lower[k] = path[k] == L'/' ? L'\\' : (wchar_t)towlower(path[k]);
    lower[k] = 0;
    if (!wcsstr(lower, L"\\savedlevels\\")) return NULL;
    if (!read_head(path, head, sizeof(head), &got) || !full_colour(head, got)) return NULL;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) return NULL;
    remember(path);
    for (i = 0; i < nconv; i++)
        if (!lstrcmpiW(convs[i].path, path)) {
            if (!CompareFileTime(&convs[i].t, &fa.ftLastWriteTime)) return &convs[i];
            free(convs[i].png); convs[i].png = NULL;
            break;
        }
    t0 = GetTickCount();
    if (!decode(path, &img)) { say("map %ls: could not be read", path); return NULL; }
    if (!make_proxy(&img, &out, &pw, &ph)) { free(img.px); free(out.b); say("map %ls (%ux%u): not converted", path, img.w, img.h); return NULL; }
    if (i == nconv) { if (nconv == MAX_CONV) { free(convs[0].png); memmove(convs, convs + 1, sizeof(conv) * (MAX_CONV - 1)); nconv--; } i = nconv++; }
    wcsncpy(convs[i].path, path, MAX_PATH - 1);
    convs[i].path[MAX_PATH - 1] = 0;
    convs[i].t = fa.ftLastWriteTime; convs[i].png = out.b; convs[i].n = out.n;
    say("map %ls: %ux%u full colour -> WA map %ux%u (theme %s), %lu ms", path, img.w, img.h, pw, ph, themes[theme % 29],
        (unsigned long)(GetTickCount() - t0));
    free(img.px);
    return &convs[i];
}

/* the proxy as a temporary file Windows deletes when WA closes it, at its start */
static HANDLE temp_handle(const conv *c) {
    wchar_t dir[MAX_PATH], path[MAX_PATH + 64];
    HANDLE f;
    DWORD put_n = 0;
    if (!GetTempPathW(MAX_PATH, dir)) return INVALID_HANDLE_VALUE;
    _snwprintf(path, MAX_PATH + 63, L"%lswkMapZoom-%lu-%ld.png", dir, (unsigned long)GetCurrentProcessId(), (long)InterlockedIncrement(&temp_n));
    path[MAX_PATH + 63] = 0;
    f = orig_cfw(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS,
                 FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (f == INVALID_HANDLE_VALUE) return f;
    if (!WriteFile(f, c->png, (DWORD)c->n, &put_n, NULL) || put_n != c->n) { CloseHandle(f); return INVALID_HANDLE_VALUE; }
    SetFilePointer(f, 0, NULL, FILE_BEGIN);
    return f;
}

static HANDLE swap(const wchar_t *path, DWORD access) {
    HANDLE h = INVALID_HANDLE_VALUE;
    conv *c;
    EnterCriticalSection(&lock);
    c = proxy_for(path);
    if (c) {
        h = temp_handle(c);
        if (access & GENERIC_WRITE) say("map %ls: WA writes it: not kept (the original is never written)", path);
    }
    LeaveCriticalSection(&lock);
    return h;
}

static HANDLE WINAPI hook_cfw(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (name && (disp == OPEN_EXISTING || disp == OPEN_ALWAYS)) {
        HANDLE h = swap(name, access);
        if (h != INVALID_HANDLE_VALUE) return h;
    }
    return orig_cfw(name, access, share, sa, disp, flags, tmpl);
}

static HANDLE WINAPI hook_cfa(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (name && (disp == OPEN_EXISTING || disp == OPEN_ALWAYS)) {
        wchar_t w[MAX_PATH];
        if (MultiByteToWideChar(CP_ACP, 0, name, -1, w, MAX_PATH)) {
            HANDLE h = swap(w, access);
            if (h != INVALID_HANDLE_VALUE) return h;
        }
    }
    return orig_cfa(name, access, share, sa, disp, flags, tmpl);
}

/* ---- in game: the land matched to its full-colour original ------------------------------------------------------ */
static int try_match(const wchar_t *path, int w, int h, const uint8_t *snap, uint32_t **full) {
    image img;
    uint32_t pw, ph, ax, ay;
    int any_air = 0, c;
    size_t k;
    long cands[4][2];
    if (!decode(path, &img)) return 0;
    for (k = 0; k < (size_t)img.w * img.h && !any_air; k++) any_air = !is_land(img.px[k]);
    layout(img.w, img.h, any_air, &pw, &ph, &ax, &ay);
    cands[0][0] = 0; cands[0][1] = 0;
    cands[1][0] = 0; cands[1][1] = (long)h - (long)ph;
    cands[2][0] = ((long)w - (long)pw) / 2; cands[2][1] = (long)h - (long)ph;
    cands[3][0] = ((long)w - (long)pw) / 2; cands[3][1] = ((long)h - (long)ph) / 2;
    for (c = 0; c < 4; c++) {
        long ox = cands[c][0] + (long)ax, oy = cands[c][1] + (long)ay, x, y;
        unsigned long same = 0, total = 0;
        for (y = 0; y < (long)img.h; y += 3)
            for (x = 0; x < (long)img.w; x += 3) {
                long lx = ox + x, ly = oy + y;
                if (lx < 0 || ly < 0 || lx >= w || ly >= h) continue;
                total++;
                same += (snap[(size_t)ly * w + lx] != 0) == is_land(img.px[(size_t)y * img.w + x]);
            }
        if (total > 1000 && (double)same / total > 0.995) {
            uint32_t *f = (uint32_t *)calloc((size_t)w * h, sizeof(uint32_t));
            if (!f) break;
            for (y = 0; y < (long)img.h; y++)
                for (x = 0; x < (long)img.w; x++) {
                    long lx = ox + x, ly = oy + y;
                    uint32_t p = img.px[(size_t)y * img.w + x];
                    if (lx >= 0 && ly >= 0 && lx < w && ly < h && is_land(p)) f[(size_t)ly * w + lx] = (p & 0xffffffu) | 0x1000000u;
                }
            say("land is %ls at %ld,%ld (%.2f%% of its shape matches): full colour on", path, ox, oy, 100.0 * same / total);
            *full = f;
            free(img.px);
            return 1;
        }
    }
    free(img.px);
    return 0;
}

uint32_t *colourmap_match(int w, int h, const uint8_t *snap) {
    wchar_t tried[64][MAX_PATH], pattern[MAX_PATH + 4];
    int ntried = 0, i, any = 0;
    uint32_t *full = NULL;
    WIN32_FIND_DATAW fd;
    HANDLE fh;
    if (!orig_cfw || w <= 0 || h <= 0) return NULL;
    EnterCriticalSection(&lock);
    for (i = nrecent - 1; i >= 0 && !full && ntried < 64; i--) {        /* the ones WA opened this session, latest first */
        uint32_t iw, ih, pw, ph, ax, ay;
        if (!image_size(recent[i], &iw, &ih)) continue;
        wcscpy(tried[ntried++], recent[i]);
        layout(iw, ih, 1, &pw, &ph, &ax, &ay);
        if ((pw != (uint32_t)w || ph != (uint32_t)h) && (layout(iw, ih, 0, &pw, &ph, &ax, &ay), pw != (uint32_t)w || ph != (uint32_t)h)) continue;
        any = 1;
        try_match(recent[i], w, h, snap, &full);
    }
    _snwprintf(pattern, MAX_PATH + 3, L"%ls\\*", levels_dir);
    pattern[MAX_PATH + 3] = 0;
    if (!full && (fh = FindFirstFileW(pattern, &fd)) != INVALID_HANDLE_VALUE) {          /* then the rest */
        do {
            wchar_t p[MAX_PATH];
            uint32_t iw, ih, pw, ph, ax, ay;
            int j, seen = 0;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            _snwprintf(p, MAX_PATH - 1, L"%ls\\%ls", levels_dir, fd.cFileName);
            p[MAX_PATH - 1] = 0;
            for (j = 0; j < ntried; j++) if (!lstrcmpiW(tried[j], p)) seen = 1;
            if (seen || !image_size(p, &iw, &ih)) continue;
            layout(iw, ih, 1, &pw, &ph, &ax, &ay);
            if ((pw != (uint32_t)w || ph != (uint32_t)h) && (layout(iw, ih, 0, &pw, &ph, &ax, &ay), pw != (uint32_t)w || ph != (uint32_t)h)) continue;
            any = 1;
            try_match(p, w, h, snap, &full);
        } while (!full && FindNextFileW(fh, &fd));
        FindClose(fh);
    }
    LeaveCriticalSection(&lock);
    if (!full && any) say("land %dx%d: no full-colour map matches its shape", w, h);
    return full;
}

/* ---- WA's imports ------------------------------------------------------------------------------------------------ */
int colourmap_install(uintptr_t b, const char *wa_dir, int t, void (*log)(const char *, ...)) {
    uint8_t *mod;
    IMAGE_NT_HEADERS *nt;
    IMAGE_IMPORT_DESCRIPTOR *d;
    DWORD rva;
    int patched = 0;
    base = b; say = log; theme = t;
    InitializeCriticalSection(&lock);
    if (!MultiByteToWideChar(CP_ACP, 0, wa_dir, -1, levels_dir, MAX_PATH - 32)) return 0;
    wcscat(levels_dir, L"\\User\\SavedLevels");
    orig_cfa = CreateFileA;
    orig_cfw = CreateFileW;
    mod = (uint8_t *)base;
    nt = (IMAGE_NT_HEADERS *)(mod + ((IMAGE_DOS_HEADER *)mod)->e_lfanew);
    rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return 0;
    for (d = (IMAGE_IMPORT_DESCRIPTOR *)(mod + rva); d->Name; d++) {
        IMAGE_THUNK_DATA *names, *thunks;
        if (lstrcmpiA((const char *)(mod + d->Name), "kernel32.dll")) continue;
        names = (IMAGE_THUNK_DATA *)(mod + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        thunks = (IMAGE_THUNK_DATA *)(mod + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, thunks++) {
            const char *fn;
            void *nw = NULL;
            DWORD old;
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            fn = (const char *)((IMAGE_IMPORT_BY_NAME *)(mod + names->u1.AddressOfData))->Name;
            if (!strcmp(fn, "CreateFileA")) { orig_cfa = (HANDLE (WINAPI *)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE))(uintptr_t)thunks->u1.Function; nw = (void *)hook_cfa; }
            else if (!strcmp(fn, "CreateFileW")) { orig_cfw = (HANDLE (WINAPI *)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE))(uintptr_t)thunks->u1.Function; nw = (void *)hook_cfw; }
            if (!nw) continue;
            if (VirtualProtect(&thunks->u1.Function, sizeof(thunks->u1.Function), PAGE_READWRITE, &old)) {
                thunks->u1.Function = (uintptr_t)nw;
                VirtualProtect(&thunks->u1.Function, sizeof(thunks->u1.Function), old, &old);
                patched++;
            }
        }
    }
    if (!patched) { say("full-colour maps unavailable: WA imports no CreateFile"); return 0; }
    say("full-colour maps: PNG / JPEG maps in %ls are shown in their own colours (theme %s)", levels_dir, themes[theme % 29]);
    return 1;
}
