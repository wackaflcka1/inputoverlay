/* apm.h - live APM rate: sliding window + light EMA smoothing. Portable C, so
 * it is unit-tested on Linux with exact timestamps. Stores only timestamps. */
#ifndef MAO_APM_H
#define MAO_APM_H
#define APM_CAP 65536
typedef struct {
    double t[APM_CAP]; unsigned n;   /* ring of action times */
    double session_start;            /* app start or last reset */
    double burst_start;              /* first action after a gap >= window */
    double last_t;                   /* last action time */
    unsigned long long total;        /* actions since session_start */
    double ema, ema_t;               /* smoothed APM and when it was updated */
} ApmState;
void   apm_init(ApmState *a, double now);           /* also used for reset */
void   apm_add(ApmState *a, double t, double win);  /* one action at time t */
double apm_raw(const ApmState *a, double now, double win);
double apm_tick(ApmState *a, double now, double win); /* update + return smoothed */
double apm_avg(const ApmState *a, double now);       /* whole-session average */
#endif
