/* wkMapZoom's readout picture (zoom_badge.h): clear outside the box, see-through black box, a white number in the middle.
 * With a folder argument it also writes each picture over a grey background as a BMP to look at. */
#include <stdio.h>
#include <stdlib.h>
#include "../src/zoom_badge.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void save(const char *dir, int pct, const uint32_t *img, int tw, int th) {
    char path[MAX_PATH];
    FILE *f;
    int x, y;
    uint8_t hdr[54] = {'B', 'M'};
    uint32_t size = 54 + tw * th * 4, off = 54, isz = 40, w = (uint32_t)tw;
    int32_t h = -th;
    uint16_t planes = 1, bpp = 32;
    snprintf(path, sizeof(path), "%s\\badge-%d.bmp", dir, pct);
    if (!(f = fopen(path, "wb"))) return;
    memcpy(hdr + 2, &size, 4); memcpy(hdr + 10, &off, 4); memcpy(hdr + 14, &isz, 4); memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4); memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bpp, 2);
    fwrite(hdr, 1, 54, f);
    for (y = 0; y < th; y++)
        for (x = 0; x < tw; x++) {   /* over grey 140, as the screen would show it */
            uint32_t c = img[y * tw + x], a = c >> 24, out = 0, k;
            for (k = 0; k < 24; k += 8) out |= ((((c >> k) & 0xff) * a + 140 * (255 - a)) / 255) << k;
            fwrite(&out, 4, 1, f);
        }
    fclose(f);
}

int main(int argc, char **argv) {
    static const int pcts[] = {50, 100, 225};
    int f, bw, bh, i;
    badge_size(1080, &f, &bw, &bh);
    CHECK(f == 28 && bw == 100 && bh == 44);
    badge_size(540, &f, &bw, &bh);
    CHECK(f == 14);
    badge_size(1080, &f, &bw, &bh);
    for (i = 0; i < 3; i++) {
        int tw = 128, th = 64, x, y, white = 0, box = 0, outside_clear = 1, lit_left = bw, lit_right = 0;
        uint32_t *img = calloc((size_t)tw * th, 4);
        CHECK(badge_paint(pcts[i], f, bw, bh, tw, th, img, tw * 4));
        for (y = 0; y < th; y++)
            for (x = 0; x < tw; x++) {
                uint32_t c = img[y * tw + x], a = c >> 24;
                if (x >= bw + 1 || y >= bh + 1) { if (a) outside_clear = 0; continue; }
                if (a >= 150) box++;
                if ((c & 0xffffff) == 0xffffff && a == 255) {
                    white++;
                    if (x < lit_left) lit_left = x;
                    if (x > lit_right) lit_right = x;
                }
            }
        CHECK(outside_clear);
        CHECK(img[0] >> 24 == 0);                       /* the rounded corner */
        CHECK(box > bw * bh * 8 / 10);
        CHECK(white > 40);                              /* the number is there */
        CHECK(lit_left > 2 && lit_right < bw - 3);      /* and fits inside the box */
        CHECK(abs((lit_left + lit_right) / 2 - bw / 2) <= 3);   /* centred */
        if (argc > 1) save(argv[1], pcts[i], img, tw, th);
        free(img);
    }
    printf(failures ? "zoom_badge_test: %d failed\n" : "zoom_badge_test: all passed\n", failures);
    return failures != 0;
}
