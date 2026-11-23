/* wkMapZoom's arithmetic (zoom_math.h): buffer size, limits, wheel steps, easing, the zoomed view's mapping. */
#include <stdio.h>
#include "../src/zoom_math.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
#define NEAR(a, b) (fabs((a) - (b)) < 1e-6)

int main(void) {
    zoom_cfg c = ZOOM_CFG_DEFAULT;
    int gw, gh, rw, rh, bx, by, i;
    double lo, hi, z;

    /* the world buffer: 2x a 1920x1080 game, capped at 8192 a side, never smaller than the game */
    zoom_frame(1920, 1080, 2.0, &gw, &gh); CHECK(gw == 3840 && gh == 2160);
    zoom_frame(5120, 2880, 4.0, &gw, &gh); CHECK(gw == 16384 && gh <= 16384 && gh >= 9200);
    zoom_frame(1920, 1080, 4.0, &gw, &gh); CHECK(gw == 7680 && gh == 4320);
    zoom_frame(1920, 1080, 1.0, &gw, &gh); CHECK(gw == 1920 && gh == 1080);
    zoom_frame(1366, 768, 2.0, &gw, &gh); CHECK(gw == 2732 && gh == 1536);

    /* limits (defaults): out to 25%, in to 800%; out only as far as the buffer holds */
    zoom_frame(1920, 1080, c.max_out, &gw, &gh);
    zoom_limits(1920, 1080, gw, gh, &c, &lo, &hi); CHECK(NEAR(lo, 0.25) && NEAR(hi, 8.0));
    zoom_frame(2560, 1440, c.max_out, &gw, &gh);
    zoom_limits(2560, 1440, gw, gh, &c, &lo, &hi); CHECK(NEAR(lo, 0.25) && NEAR(hi, 8.0));
    zoom_limits(5120, 2880, 16384, 9216, &c, &lo, &hi); CHECK(NEAR(lo, 0.3125) && NEAR(hi, 8.0));
    zoom_frame(1920, 1080, 2.0, &gw, &gh);

    /* wheel: 100% is a stop both ways; a part notch moves a part step */
    CHECK(NEAR(zoom_step(1.0, 1, 1.15), 1.15));
    CHECK(NEAR(zoom_step(1.0, -1, 1.15), 1 / 1.15));
    CHECK(zoom_step(0.9, 1, 1.15) == 1.0);
    CHECK(zoom_step(1.1, -1, 1.15) == 1.0);
    CHECK(NEAR(zoom_step(1.15, 0.5, 1.15), 1.15 * sqrt(1.15)));
    z = 1.0; for (i = 0; i < 40; i++) z = zoom_clamp(zoom_step(z, -1, 1.15), 0.5, 4.0); CHECK(NEAR(z, 0.5));
    z = 1.0; for (i = 0; i < 40; i++) z = zoom_clamp(zoom_step(z, 1, 1.15), 0.5, 4.0); CHECK(NEAR(z, 4.0));
    /* a second stop (the default zoom): reached both ways, and 100% still a stop */
    CHECK(NEAR(zoom_step2(1.0, -1, 1.15, 0.9), 0.9));                     /* (1 -> 0.87 passes 0.9) */
    CHECK(NEAR(zoom_step2(0.9, -1, 1.15, 0.9), 0.9 / 1.15));          /* (from the stop: on) */
    CHECK(NEAR(zoom_step2(0.8, 1, 1.15, 0.9), 0.9));                      /* (0.8 -> 0.92 passes 0.9) */
    CHECK(NEAR(zoom_step2(0.9, 1, 1.15, 0.9), 1.0));                      /* (0.9 -> 1.035: near 100%) */
    CHECK(NEAR(zoom_step2(0.85, 2, 1.15, 0.9), 0.9));                     /* (both passed: the nearer) */
    CHECK(NEAR(zoom_step2(1.2, -2, 1.15, 0.9), 1.0));
    CHECK(NEAR(zoom_step2(0.9, 1, 1.15, 1.0), 1.0) && NEAR(zoom_step2(0.9, 1, 1.15, 0.0), 1.0));   /* (no second stop) */
    z = 1.0; for (i = 0; i < 6; i++) z = zoom_step2(z, -1, 1.15, 0.7); CHECK(NEAR(z, 0.7 / pow(1.15, 3)));   /* (0.87, 0.756, 0.7, ...) */
    z = 0.4; for (i = 0; i < 4; i++) z = zoom_step2(z, 1, 1.15, 0.7); CHECK(NEAR(z, 0.7));
    z = 0.8; for (i = 0; i < 3; i++) z = zoom_step2(z, 10.0 / 120.0, 1.15, 0.8); CHECK(z > 0.8 + 1e-3);   /* (a touchpad's
                                                                    small turns leave the stop) */
    z = 1.0; for (i = 0; i < 3; i++) z = zoom_step2(z, -10.0 / 120.0, 1.15, 0.8); CHECK(z < 1.0 - 1e-3);
    {   /* steps of 1 to 8 / 120 notch (a precision touchpad) leave 100% and the default stop both ways, and never jump
         * back the other way */
        int dlt, dir, st;
        for (dlt = 1; dlt <= 8; dlt++) for (dir = -1; dir <= 1; dir += 2) for (st = 0; st < 2; st++) {
            const double stop = st ? 0.8 : 1.0, def = 0.8;
            double zz = stop, prev;
            int moved_back = 0;
            for (i = 0; i < 60; i++) {
                prev = zz;
                zz = zoom_step2(zz, dir * dlt / 120.0, 1.15, def);
                if ((zz - prev) * dir < -1e-12) moved_back = 1;
            }
            CHECK(!moved_back && fabs(log(zz / stop)) > 0.05);
        }
        z = 0.99; for (i = 0; i < 20; i++) z = zoom_step2(z, -4.0 / 120.0, 1.15, 0.99); CHECK(z < 0.98);   /* (two stops
                                                                    1% apart: down from 0.99 goes down) */
    }   /* (0.46, 0.529, 0.608, 0.7) */

    /* easing: no move with no time, most of the way in ~3 time constants, lands exactly, never overshoots */
    CHECK(zoom_ease(1.0, 2.0, 0.0) == 1.0);
    z = zoom_ease(1.0, 2.0, 0.18); CHECK(z > 1.9 && z < 2.0);
    z = 1.0; for (i = 0; i < 60; i++) { double n = zoom_ease(z, 0.5, 1.0 / 60); CHECK(n <= z && n >= 0.5); z = n; }
    CHECK(z == 0.5);

    /* the view: a full 1920x1080 screen, camera centre in the middle */
    zoom_region(0, 0, 1920, 1080, 960, 540, 1.0, 3840, 2160, &rw, &rh, &bx, &by);
    CHECK(rw == 1920 && rh == 1080 && bx == 960 && by == 540);
    for (i = 0; i < 1920; i += 37) CHECK(zoom_src(i, 960, bx, 1.0, rw) == i);       /* 100%: pixel for pixel */
    zoom_region(0, 0, 1920, 1080, 960, 540, 0.5, 3840, 2160, &rw, &rh, &bx, &by);
    CHECK(rw == 3840 && rh == 2160 && bx == 1920 && by == 1080);                     /* 50%: twice the world */
    CHECK(zoom_src(0, 960, bx, 0.5, rw) == 1 && zoom_src(1919, 960, bx, 0.5, rw) == 3839);
    CHECK(zoom_src(960, 960, bx, 0.5, rw) == 1921);
    zoom_region(0, 0, 1920, 1080, 960, 540, 2.0, 3840, 2160, &rw, &rh, &bx, &by);
    CHECK(rw == 960 && rh == 540 && bx == 480 && by == 270);                         /* 200%: half the world */
    CHECK(zoom_src(0, 960, bx, 2.0, rw) == 0 && zoom_src(1, 960, bx, 2.0, rw) == 0 && zoom_src(2, 960, bx, 2.0, rw) == 1);
    CHECK(zoom_src(1919, 960, bx, 2.0, rw) == 959);
    /* the camera's centre stays put: the screen's centre pixel shows the world's centre at any zoom */
    zoom_region(0, 0, 1920, 1080, 960, 540, 3.0, 3840, 2160, &rw, &rh, &bx, &by);
    CHECK(zoom_src(960, 960, bx, 3.0, rw) == bx);
    /* a view under a chat panel (screen rows 80..1080, centre at 580) */
    zoom_region(0, 80, 1920, 1080, 960, 580, 2.0, 3840, 2160, &rw, &rh, &bx, &by);
    CHECK(rh == 500 && by == 250 && zoom_src(80, 580, by, 2.0, rh) == 0 && zoom_src(1079, 580, by, 2.0, rh) == 499);
    /* never past the buffer */
    zoom_region(0, 0, 1920, 1080, 960, 540, 0.25, 3840, 2160, &rw, &rh, &bx, &by);
    CHECK(rw == 3840 && rh == 2160);
    for (i = 0; i < 1920; i += 13) { int q = zoom_src(i, 960, bx, 0.25, rw); CHECK(q >= 0 && q < rw); }

    /* settings out of range come back sane */
    c.max_out = 9; c.max_in = 0.2; c.step = 5;
    zoom_cfg_sane(&c);
    CHECK(c.max_out == 8.0 && c.max_in == 1.0 && c.step == 2.0);

    printf(failures ? "zoom_math_test: %d failed\n" : "zoom_math_test: all passed\n", failures);
    return failures != 0;
}
