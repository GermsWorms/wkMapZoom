/* camera_glide.h: the camera the picture is drawn with (pure arithmetic: tests/camera_glide_test.c).
 *
 * While you look around with the mouse, WA moves its camera in steps: about 64 a second (the ticks of the system
 * clock its mouse camera is eased by), however many frames it draws. At 600 frames a second the picture then stands
 * still for nine frames and jumps in the tenth -- zoomed out, where one looks around fast, by tens of screen pixels.
 * Here each step is spread over the time WA's steps are apart (measured as they come; 20 ms, `max_glide`, at most):
 * the drawn camera goes from where it is to WA's new place at an even speed, and arrives about as the next step is
 * due. So it trails WA's camera by one step (16 ms) while that moves, and is the same when it rests.
 *
 * When WA's camera moves in every frame anyway (it follows a worm), nothing is done: from the third frame in a row
 * with a move the drawn camera is WA's own. Only the picture is concerned: WA's own camera is not changed. */
#ifndef CAMERA_GLIDE_H
#define CAMERA_GLIDE_H
#include <math.h>
#include <stdint.h>

typedef struct {
    int have, run;              /* a camera seen; frames in a row in which WA's camera moved */
    int32_t wx, wy;             /* WA's camera at the last frame (16.16 pixels) */
    double sx, sy, tx, ty;      /* the glide: from s (at time ts) to t (at time te), 16.16 pixels */
    double ts, te, t_step;      /* seconds; t_step: when WA's camera last moved */
    double period;              /* how far apart WA's steps are, as they come (seconds) */
} cam_glide;

static void cam_glide_reset(cam_glide *g) { g->have = 0; g->run = 0; g->period = 0.0; }

/* WA's camera wx, wy (16.16) at time `now` (seconds, any start) -> the camera to draw this frame with (16.16).
 * Returns 1 when that differs from WA's. */
static int cam_glide_step(cam_glide *g, double now, int32_t wx, int32_t wy, double max_glide, int32_t *ox, int32_t *oy) {
    double k, cx, cy;
    if (!g->have || now < g->ts) {          /* the first frame (or the clock went back): WA's own */
        g->have = 1; g->run = 0; g->period = 0.0;
        g->wx = wx; g->wy = wy;
        g->sx = g->tx = (double)wx; g->sy = g->ty = (double)wy;
        g->ts = g->te = g->t_step = now;
        *ox = wx; *oy = wy;
        return 0;
    }
    k = g->te > g->ts ? (now - g->ts) / (g->te - g->ts) : 1.0;   /* where the drawn camera is by now */
    if (k > 1.0) k = 1.0;
    cx = g->sx + (g->tx - g->sx) * k; cy = g->sy + (g->ty - g->sy) * k;
    if (wx != g->wx || wy != g->wy) {       /* WA moved its camera */
        const double gap = now - g->t_step;
        double d;
        if (gap > 0.1 || g->period <= 0.0) g->period = max_glide;   /* after a rest: not known yet, the longest */
        else g->period += (gap - g->period) * 0.25;
        if (g->period > max_glide) g->period = max_glide;
        d = g->period;
        g->run++;
        if (g->run >= 3 || gap <= 0.0) {    /* it moves every frame (or no time passed): WA's own */
            cx = (double)wx; cy = (double)wy; d = 0.0;
        }
        g->sx = cx; g->sy = cy; g->tx = (double)wx; g->ty = (double)wy;
        g->ts = now; g->te = now + d; g->t_step = now;
        g->wx = wx; g->wy = wy;
    } else g->run = 0;
    *ox = (int32_t)floor(cx + 0.5); *oy = (int32_t)floor(cy + 0.5);
    return *ox != wx || *oy != wy;
}
#endif
