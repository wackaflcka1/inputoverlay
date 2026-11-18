#include "demo.h"
#include <math.h>
static void demo_pos(double t, float w, float h, float *x, float *y) {
    *x = w * (float)(0.5 + 0.30 * sin(t * 1.30) + 0.12 * sin(t * 3.10 + 1.0));
    *y = h * (float)(0.5 + 0.27 * sin(t * 1.70 + 0.5) + 0.10 * cos(t * 2.30));
}
int demo_trail(double now, float w, float h, float len_s, TrailPt *out, int cap) {
    const double dt = 1.0 / 240.0; /* like a 240 Hz poll */
    int n = (int)(len_s / dt) + 2;
    if (n > cap) n = cap;
    double t0 = floor(now / dt) * dt - (n - 2) * dt;
    int k = 0;
    for (int i = 0; i < n - 1; i++) {
        double t = t0 + i * dt;
        out[k].t = t; demo_pos(t, w, h, &out[k].x, &out[k].y); k++;
    }
    out[k].t = now; demo_pos(now, w, h, &out[k].x, &out[k].y); k++;
    return k;
}
int demo_clicks(double now, float w, float h, float fade_s, ClickEv *out, int cap) {
    const double period = 0.42;
    int k = 0;
    long last = (long)floor(now / period);
    for (long i = last; i >= 0 && k < cap; i--) {
        double t = i * period;
        if (now - t >= fade_s) break;
        out[k].t = t; out[k].btn = (i % 3 == 2) ? 0 : 1;
        demo_pos(t, w, h, &out[k].x, &out[k].y); k++;
    }
    return k;
}
