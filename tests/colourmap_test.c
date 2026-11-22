/* colourmap_test.c: full-colour maps (src/colourmap.c) end to end on this PC, without WA: a full-colour PNG and a JPEG
 * in a SavedLevels folder are opened through this program's own (patched) CreateFileA, as WA opens maps; the PNG that
 * comes back is taken apart (signature, IHDR, palette, the waLV settings, the image data) and checked against the
 * original; a land made from it is matched back to the original's colours. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/colourmap.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("  log: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

/* ---- a tiny RGBA PNG writer (stored deflate) for the test image ---- */
static uint32_t crcs[256];
static uint32_t crc(uint32_t c, const uint8_t *b, size_t n) {
    c = ~c;
    while (n--) c = crcs[(c ^ *b++) & 255] ^ (c >> 8);
    return ~c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void chunk(FILE *f, const char *t, const uint8_t *d, size_t n) {
    uint8_t b[4];
    uint32_t c;
    be32(b, (uint32_t)n); fwrite(b, 1, 4, f);
    fwrite(t, 1, 4, f);
    if (n) fwrite(d, 1, n, f);
    c = crc(crc(0, (const uint8_t *)t, 4), d, n);
    be32(b, c); fwrite(b, 1, 4, f);
}
static void write_png(const char *path, int w, int h, const uint32_t *px) {
    FILE *f = fopen(path, "wb");
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    uint8_t ihdr[13], *raw, *z;
    size_t rawn = (size_t)(w * 4 + 1) * h, zn = 0, at = 0, k;
    uint32_t a = 1, b = 0;
    int x, y;
    raw = (uint8_t *)malloc(rawn);
    z = (uint8_t *)malloc(rawn + rawn / 65535 * 5 + 64);
    for (y = 0; y < h; y++) {
        uint8_t *r = raw + (size_t)y * (w * 4 + 1);
        r[0] = 0;
        for (x = 0; x < w; x++) {
            uint32_t p = px[y * w + x];
            r[1 + 4 * x] = (uint8_t)(p >> 16); r[2 + 4 * x] = (uint8_t)(p >> 8); r[3 + 4 * x] = (uint8_t)p; r[4 + 4 * x] = (uint8_t)(p >> 24);
        }
    }
    z[zn++] = 0x78; z[zn++] = 0x01;
    while (at < rawn) {
        size_t len = rawn - at > 65535 ? 65535 : rawn - at;
        z[zn++] = at + len >= rawn; z[zn++] = (uint8_t)len; z[zn++] = (uint8_t)(len >> 8); z[zn++] = (uint8_t)~len; z[zn++] = (uint8_t)(~len >> 8);
        memcpy(z + zn, raw + at, len); zn += len; at += len;
    }
    for (k = 0; k < rawn; k++) { a = (a + raw[k]) % 65521; b = (b + a) % 65521; }
    be32(z + zn, b << 16 | a); zn += 4;
    fwrite(sig, 1, 8, f);
    be32(ihdr, (uint32_t)w); be32(ihdr + 4, (uint32_t)h); ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, zn);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw); free(z);
}

/* ---- the same picture as a JPEG, through GDI+ ---- */
typedef struct { UINT32 v; void *d; BOOL a, b; } gpin;
static int write_jpeg(const wchar_t *path, int w, int h, uint32_t *px) {
    static const GUID jpeg = {0x557cf401, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
    HMODULE m = LoadLibraryA("gdiplus.dll");
    int (WINAPI *start)(ULONG_PTR *, const gpin *, void *);
    int (WINAPI *mk)(INT, INT, INT, INT, BYTE *, void **);
    int (WINAPI *save)(void *, const WCHAR *, const GUID *, void *);
    ULONG_PTR tok;
    gpin in = {1, NULL, FALSE, FALSE};
    void *bmp = NULL;
    int ok;
    if (!m) return 0;
    start = (int (WINAPI *)(ULONG_PTR *, const gpin *, void *))GetProcAddress(m, "GdiplusStartup");
    mk = (int (WINAPI *)(INT, INT, INT, INT, BYTE *, void **))GetProcAddress(m, "GdipCreateBitmapFromScan0");
    save = (int (WINAPI *)(void *, const WCHAR *, const GUID *, void *))GetProcAddress(m, "GdipSaveImageToFile");
    if (!start || !mk || !save || start(&tok, &in, NULL)) return 0;
    if (mk(w, h, w * 4, 0x22009 /* 32bppRGB */, (BYTE *)px, &bmp) || !bmp) return 0;
    ok = !save(bmp, path, &jpeg, NULL);
    return ok;
}

/* ---- taking a PNG apart ---- */
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
typedef struct { uint32_t w, h; int type, npal; uint8_t pal[256][3]; int walv; uint8_t settings[40]; uint8_t *idx; } png;
static int parse(const uint8_t *b, size_t n, png *p) {
    size_t at = 8, zn = 0;
    uint8_t *z = (uint8_t *)malloc(n), *raw;
    uint32_t y;
    memset(p, 0, sizeof(*p));
    if (n < 8 || memcmp(b, "\x89PNG\r\n\x1a\n", 8)) { free(z); return 0; }
    while (at + 12 <= n) {
        uint32_t len = rd32(b + at);
        const uint8_t *t = b + at + 4, *d = b + at + 8;
        if (at + 12 + len > n) break;
        if (crc(crc(0, t, 4), d, len) != rd32(d + len)) { printf("bad CRC in %.4s\n", (const char *)t); free(z); return 0; }
        if (!memcmp(t, "IHDR", 4)) { p->w = rd32(d); p->h = rd32(d + 4); p->type = d[9]; if (d[8] != 8 || d[12]) { free(z); return 0; } }
        else if (!memcmp(t, "PLTE", 4)) { p->npal = (int)len / 3; memcpy(p->pal, d, len); }
        else if (!memcmp(t, "waLV", 4)) { p->walv = (int)len; if (len == 40) memcpy(p->settings, d, 40); }
        else if (!memcmp(t, "IDAT", 4)) { memcpy(z + zn, d, len); zn += len; }
        at += 12 + len;
    }
    /* zlib: stored blocks only (the module writes no other) */
    raw = (uint8_t *)malloc((size_t)(p->w + 1) * p->h + 16);
    {
        size_t zi = 2, ri = 0;
        int last = 0;
        if (zn < 2 || (z[0] << 8 | z[1]) % 31) { free(z); free(raw); return 0; }
        while (!last && zi + 5 <= zn) {
            uint32_t len = (uint32_t)z[zi + 1] | (uint32_t)z[zi + 2] << 8, nlen = (uint32_t)z[zi + 3] | (uint32_t)z[zi + 4] << 8;
            last = z[zi] & 1;
            if ((z[zi] & 6) || (len ^ 0xffff) != nlen || ri + len > (size_t)(p->w + 1) * p->h) { free(z); free(raw); return 0; }
            memcpy(raw + ri, z + zi + 5, len);
            ri += len; zi += 5 + len;
        }
        if (ri != (size_t)(p->w + 1) * p->h) { free(z); free(raw); return 0; }
    }
    p->idx = (uint8_t *)malloc((size_t)p->w * p->h);
    for (y = 0; y < p->h; y++) {
        if (raw[(size_t)y * (p->w + 1)]) { free(z); free(raw); return 0; }
        memcpy(p->idx + (size_t)y * p->w, raw + (size_t)y * (p->w + 1) + 1, p->w);
    }
    free(z); free(raw);
    return 1;
}

static uint8_t *open_proxy(const char *path, size_t *n) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD size, got = 0;
    uint8_t *b;
    if (f == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize(f, NULL);
    b = (uint8_t *)malloc(size ? size : 1);
    ReadFile(f, b, size, &got, NULL);
    CloseHandle(f);
    *n = got;
    return b;
}

int main(void) {
    char dir[MAX_PATH], levels[MAX_PATH], pngpath[MAX_PATH], jpgpath[MAX_PATH], other[MAX_PATH];
    wchar_t wjpg[MAX_PATH];
    const int W = 700, H = 300;
    uint32_t *px = (uint32_t *)malloc(sizeof(uint32_t) * W * H), *full;
    uint8_t *b;
    size_t n;
    png p;
    int x, y, k, bad = 0, worst = 0;
    uint32_t n_, c;
    for (n_ = 0; n_ < 256; n_++) { for (c = n_, k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1; crcs[n_] = c; }
    GetTempPathA(MAX_PATH, dir);
    strcat(dir, "wkmapzoom_colourmap_test");
    CreateDirectoryA(dir, NULL);
    snprintf(levels, sizeof(levels), "%s\\User", dir); CreateDirectoryA(levels, NULL);
    snprintf(levels, sizeof(levels), "%s\\User\\SavedLevels", dir); CreateDirectoryA(levels, NULL);
    snprintf(pngpath, sizeof(pngpath), "%s\\colours.png", levels);
    snprintf(jpgpath, sizeof(jpgpath), "%s\\photo.png", levels);       /* (a JPEG under a .png name, as WA lists them) */
    snprintf(other, sizeof(other), "%s\\notalevel.png", dir);
    /* a hill of smooth colours under open sky, a thin wall, and pure black land */
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            int land = y > 120 + (int)(60.0 * __builtin_sin(x / 70.0)) || (x == 350 && y > 40) || (x > 600 && x < 640 && y > 20 && y < 60);
            uint32_t col = x > 600 && x < 640 && y < 60 ? 0 : (uint32_t)((x * 255) / W) << 16 | (uint32_t)((y * 255) / H) << 8 | (uint32_t)(255 - (x * 255) / W);
            px[y * W + x] = land ? 0xff000000u | col : 0x00000000u;
        }
    write_png(pngpath, W, H, px);
    write_png(other, W, H, px);
    MultiByteToWideChar(CP_ACP, 0, jpgpath, -1, wjpg, MAX_PATH);
    CHECK(write_jpeg(wjpg, 640, 200, px), "could not write the JPEG");

    CHECK(colourmap_theme("forest") == 11 && colourmap_theme("-Beach") == 0 && colourmap_theme("nope") < 0, "theme names");
    CHECK(colourmap_install((uintptr_t)GetModuleHandleA(NULL), dir, 11, say), "install");

    /* a file outside SavedLevels: untouched (the same bytes as on disk) */
    b = open_proxy(other, &n);
    {
        FILE *f = fopen(other, "rb");
        uint8_t *disk = (uint8_t *)malloc(n + 1);
        size_t dn = f ? fread(disk, 1, n + 1, f) : 0;
        if (f) fclose(f);
        CHECK(b && dn == n && !memcmp(b, disk, n), "a PNG outside SavedLevels came back changed");
        free(disk);
    }
    free(b);

    /* the full-colour PNG */
    b = open_proxy(pngpath, &n);
    CHECK(b != NULL, "no handle for the full-colour map");
    if (b && parse(b, n, &p)) {
        uint32_t ay;
        CHECK(p.type == 3, "proxy colour type %d", p.type);
        CHECK(p.w == 704 && p.h == 304, "proxy size %ux%u (want 704x304)", p.w, p.h);
        CHECK(p.npal >= 2 && p.npal <= 65 && !p.pal[0][0] && !p.pal[0][1] && !p.pal[0][2], "palette: %d entries, entry 0 %d,%d,%d", p.npal, p.pal[0][0], p.pal[0][1], p.pal[0][2]);
        CHECK(p.walv == 40 && p.settings[0x10] == 1 && p.settings[0x20] == 11 && p.settings[0x22] == 1, "waLV settings");
        ay = p.h - H;
        for (y = 0; y < (int)p.h; y++)
            for (x = 0; x < (int)p.w; x++) {
                int ox = x, oy = y - (int)ay, i = p.idx[(size_t)y * p.w + x];
                uint32_t o = ox < W && oy >= 0 && oy < H ? px[oy * W + ox] : 0;
                if (!(o >> 24)) { if (i) bad++; continue; }
                if (!i || i >= p.npal) { bad++; continue; }
                {
                    int d0 = abs((int)p.pal[i][0] - (int)((o >> 16) & 255)), d1 = abs((int)p.pal[i][1] - (int)((o >> 8) & 255));
                    int d2 = abs((int)p.pal[i][2] - (int)(o & 255)), d = d0 > d1 ? (d0 > d2 ? d0 : d2) : (d1 > d2 ? d1 : d2);
                    if (d > worst) worst = d;
                }
            }
        CHECK(!bad, "%d pixels on the wrong side of land / air", bad);
        CHECK(worst <= 48, "a land colour %d off in the 64-colour proxy", worst);
        printf("  proxy %ux%u, %d palette entries, worst colour difference %d\n", p.w, p.h, p.npal, worst);
        /* the land WA would make from it, matched back to the original */
        full = colourmap_match((int)p.w, (int)p.h, p.idx);
        CHECK(full != NULL, "the land did not match its original");
        if (full) {
            int wrong = 0;
            for (y = 0; y < H; y++)
                for (x = 0; x < W; x++) {
                    uint32_t o = px[y * W + x], f = full[(size_t)(y + ay) * p.w + x];
                    if ((o >> 24) ? f != (0x1000000u | (o & 0xffffffu)) : f != 0) wrong++;
                }
            CHECK(!wrong, "%d land pixels with the wrong full colour", wrong);
            free(full);
        }
        /* a land with a few pixels changed still matches (it is matched at the game's first frame); a different land
         * doesn't */
        for (y = 180; y < 190; y++) for (x = 200; x < 210; x++) p.idx[(size_t)(y + ay) * p.w + x] = 0;
        full = colourmap_match((int)p.w, (int)p.h, p.idx);
        CHECK(full != NULL, "a slightly changed land should still match");
        free(full);
        memset(p.idx, 0, (size_t)p.w * p.h);
        for (k = 0; k < (int)(p.w * p.h); k += 3) p.idx[k] = 5;
        full = colourmap_match((int)p.w, (int)p.h, p.idx);
        CHECK(full == NULL, "a different land matched");
        free(full);
    } else {
        CHECK(0, "the proxy is not a PNG this test can read");
    }
    free(b);

    /* the JPEG: all land (no transparency), so 128 rows of air on top */
    b = open_proxy(jpgpath, &n);
    CHECK(b != NULL, "no handle for the JPEG map");
    if (b && parse(b, n, &p)) {
        int air_top = 1, land_below = 1;
        CHECK(p.w == 640 && p.h == 328, "JPEG proxy size %ux%u (want 640x328)", p.w, p.h);
        for (k = 0; k < (int)p.w * 128; k++) if (p.idx[k]) air_top = 0;
        for (k = (int)p.w * 128; k < (int)(p.w * p.h); k++) if (!p.idx[k]) land_below = 0;
        CHECK(air_top && land_below, "JPEG proxy: air on top %d, land below %d", air_top, land_below);
    } else {
        CHECK(0, "the JPEG proxy is not a PNG this test can read");
    }
    free(b);
    free(px);
    if (failures) { printf("colourmap_test: %d failures\n", failures); return 1; }
    printf("colourmap_test: ok\n");
    return 0;
}
