/* wkMapZoom's drawn camera (camera_glide.h): WA's camera in steps -> an even glide; a camera that moves every frame
 * and one at rest -> left alone. */
#include <stdio.h>
#include <stdlib.h>
#include "../src/camera_glide.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
#define PX(v) ((int32_t)((v) * 65536.0))

int main(void) {
    cam_glide g;
    int32_t ox, oy;
    int i, moved, biggest, changed;
    double t;

    /* 1. Looking around: WA's camera steps 64 times a second by 160 px (10240 px/s); 650 frames a second. Drawn, it
     *    moves in nearly every frame by about 16 px, never by more than 20, and trails WA's by little more than one
     *    step (the steps fall on frames, so they are 10 or 11 frames apart). */
    cam_glide_reset(&g);
    moved = biggest = 0;
    {
        int32_t prev = 0, wa = 0, trail = 0;
        int have_prev = 0, frames = 0;
        for (i = 0; i < 1300; i++) {
            const int step = (int)(i / 650.0 * 64.0);            /* WA's step number at this frame */
            t = i / 650.0;
            wa = PX(160.0 * step);
            cam_glide_step(&g, t, wa, PX(100), 0.020, &ox, &oy);
            CHECK(oy == PX(100));
            CHECK(ox <= wa);                                       /* never ahead */
            if (wa - ox > trail) trail = wa - ox;
            if (have_prev && i > 30) {
                const int d = abs(ox - prev) >> 16;
                frames++;
                if (d) moved++;
                if (d > biggest) biggest = d;
            }
            prev = ox; have_prev = 1;
        }
        CHECK(moved > frames * 9 / 10);
        CHECK(biggest <= 20);
        CHECK(trail <= PX(200));
        printf("  64 steps a second of 160 px at 650 fps: drawn camera moved in %d of %d frames, largest step %d px, at most %d px behind\n",
               moved, frames, biggest, trail >> 16);
    }

    /* 2. The same at 120 frames a second (WA's own camera: 0 or 160 px a frame): no frame's move is more than 1.7 times
     *    an even share (85 px) */
    cam_glide_reset(&g);
    biggest = 0;
    {
        int32_t prev = 0;
        for (i = 0; i < 240; i++) {
            const int step = (int)(i / 120.0 * 64.0);
            cam_glide_step(&g, i / 120.0, PX(160.0 * step), 0, 0.020, &ox, &oy);
            if (i > 10) { const int d = abs(ox - prev) >> 16; if (d > biggest) biggest = d; }
            prev = ox;
        }
        CHECK(biggest <= 144);
        printf("  the same at 120 fps: largest step %d px (an even share is 85)\n", biggest);
    }

    /* 3. A camera that moves in every frame (following a worm): WA's own from the third frame on */
    cam_glide_reset(&g);
    changed = 0;
    for (i = 0; i < 600; i++) {
        const int32_t wa = PX(3.0 * i);
        const int r = cam_glide_step(&g, i / 600.0, wa, PX(2.0 * i), 0.020, &ox, &oy);
        if (i >= 3) { CHECK(!r && ox == wa); changed += r; }
    }
    CHECK(changed == 0);

    /* 4. At rest: WA's own; after a single step it arrives within 20 ms and stays */
    cam_glide_reset(&g);
    for (i = 0; i < 100; i++) { CHECK(!cam_glide_step(&g, i / 500.0, PX(700), PX(300), 0.020, &ox, &oy)); CHECK(ox == PX(700) && oy == PX(300)); }
    t = 100 / 500.0;
    cam_glide_step(&g, t, PX(900), PX(300), 0.020, &ox, &oy);
    CHECK(ox == PX(700));                                          /* the step's own frame: still where it was */
    cam_glide_step(&g, t + 0.010, PX(900), PX(300), 0.020, &ox, &oy);
    CHECK(ox > PX(790) && ox < PX(810));                           /* half way after half the time */
    cam_glide_step(&g, t + 0.021, PX(900), PX(300), 0.020, &ox, &oy);
    CHECK(ox == PX(900));
    CHECK(!cam_glide_step(&g, t + 0.5, PX(900), PX(300), 0.020, &ox, &oy) && ox == PX(900));

    /* 5. A clock that goes back starts anew */
    CHECK(!cam_glide_step(&g, 0.0, PX(50), PX(60), 0.020, &ox, &oy) && ox == PX(50) && oy == PX(60));

    if (failures) { printf("camera_glide_test: %d FAILED\n", failures); return 1; }
    printf("camera_glide_test: all passed\n");
    return 0;
}
