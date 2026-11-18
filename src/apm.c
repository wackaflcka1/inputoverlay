#include "apm.h"
#include <math.h>
#include <string.h>

#define APM_TAU 0.35   /* EMA time constant, seconds */
#define APM_MIN_SPAN 10.0

void apm_init(ApmState *a, double now) {
    a->n = 0; a->total = 0; a->ema = 0; a->ema_t = now;
    a->session_start = now; a->burst_start = now; a->last_t = -1e18;
}
void apm_add(ApmState *a, double t, double win) {
    /* after an idle gap the window is not "full" yet: measure from burst start */
    if (t - a->last_t >= win) a->burst_start = t > a->session_start ? t : a->session_start;
    a->last_t = t;
    a->t[a->n % APM_CAP] = t; a->n++;
    a->total++;
}
double apm_raw(const ApmState *a, double now, double win) {
    unsigned cnt = 0;
    for (unsigned i = 0; i < a->n && i < APM_CAP; i++) {
        double t = a->t[(a->n - 1 - i) % APM_CAP];
        if (now - t > win || t < a->session_start) break;
        cnt++;
    }
    if (!cnt) return 0.0;
    /* divisor = time since start / reset / first action after a long idle,
       but never less than APM_MIN_SPAN (or the window if shorter) and never
       more than the window: a short burst cannot spike the number */
    double span = now - a->burst_start;
    double lo = win < APM_MIN_SPAN ? win : APM_MIN_SPAN;
    if (span > win) span = win;
    if (span < lo) span = lo;
    return cnt * 60.0 / span;
}
double apm_tick(ApmState *a, double now, double win) {
    double raw = apm_raw(a, now, win);
    double dt = now - a->ema_t; if (dt < 0) dt = 0; if (dt > 1) dt = 1;
    a->ema_t = now;
    a->ema += (raw - a->ema) * (1.0 - exp(-dt / APM_TAU));
    if (raw == 0.0 && a->ema < 0.5) a->ema = 0.0;
    return a->ema;
}
double apm_avg(const ApmState *a, double now) {
    double el = now - a->session_start;
    if (el < APM_MIN_SPAN) el = APM_MIN_SPAN;
    return a->total * 60.0 / el;
}
