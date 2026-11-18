/* render.c - anti-aliased software rendering of the trail, click markers and
 * APM panel. Pure C, no OS dependencies, so the exact same code renders the
 * preview image on Linux and the live overlay on Windows. */
#include "render.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline float minf(float a, float b) { return a < b ? a : b; }
static inline float maxf(float a, float b) { return a > b ? a : b; }

IRect ir_empty(void) { IRect r = { 0, 0, 0, 0 }; return r; }
int ir_is_empty(IRect r) { return r.x0 >= r.x1 || r.y0 >= r.y1; }
IRect ir_union(IRect a, IRect b) {
    if (ir_is_empty(a)) return b;
    if (ir_is_empty(b)) return a;
    IRect r = { a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0,
                a.x1 > b.x1 ? a.x1 : b.x1, a.y1 > b.y1 ? a.y1 : b.y1 };
    return r;
}
static IRect ir_clip(IRect r, int w, int h) {
    if (r.x0 < 0) r.x0 = 0;
    if (r.y0 < 0) r.y0 = 0;
    if (r.x1 > w) r.x1 = w;
    if (r.y1 > h) r.y1 = h;
    if (ir_is_empty(r)) return ir_empty();
    return r;
}
static IRect ir_from(float x0, float y0, float x1, float y1, int w, int h) {
    IRect r = { (int)floorf(x0) - 1, (int)floorf(y0) - 1, (int)ceilf(x1) + 2, (int)ceilf(y1) + 2 };
    return ir_clip(r, w, h);
}

/* premultiplied "over" blend of a straight colour with coverage a */
static inline void blend_px(uint32_t *d, RGBf c, float a) {
    if (a <= 0.0f) return;
    if (a > 1.0f) a = 1.0f;
    uint32_t v = *d;
    float ia = 1.0f - a;
    float db = (float)(v & 255), dg = (float)((v >> 8) & 255), dr = (float)((v >> 16) & 255), da = (float)(v >> 24);
    uint32_t b = (uint32_t)(c.b * 255.0f * a + db * ia + 0.5f);
    uint32_t g = (uint32_t)(c.g * 255.0f * a + dg * ia + 0.5f);
    uint32_t r = (uint32_t)(c.r * 255.0f * a + dr * ia + 0.5f);
    uint32_t al = (uint32_t)(255.0f * a + da * ia + 0.5f);
    if (b > 255) b = 255;
    if (g > 255) g = 255;
    if (r > 255) r = 255;
    if (al > 255) al = 255;
    *d = b | (g << 8) | (r << 16) | (al << 24);
}

int layer_init(Layer *L, int w, int h) {
    L->cov = (float *)calloc((size_t)w * (size_t)h, sizeof(float));
    L->par = (float *)calloc((size_t)w * (size_t)h, sizeof(float));
    L->w = w; L->h = h; L->bb = ir_empty();
    return L->cov != NULL && L->par != NULL;
}
void layer_free(Layer *L) { free(L->cov); free(L->par); L->cov = L->par = NULL; L->w = L->h = 0; }

void canvas_clear(Canvas *C, IRect r) {
    r = ir_clip(r, C->w, C->h);
    if (ir_is_empty(r)) return;
    for (int y = r.y0; y < r.y1; y++)
        memset(C->px + (size_t)y * C->w + r.x0, 0, (size_t)(r.x1 - r.x0) * 4);
}

/* Tapered capsule into the coverage layer; max() keeps overlapping joints
 * from doubling up, so the trail stays one clean ribbon. */
/* when g_par_on, capsules also record where along the colour ramp each pixel sits */
static int g_par_on; static float g_pu0, g_pu1;
static void layer_capsule(Layer *L, float ax, float ay, float bx, float by,
                          float ra, float rb, float aa, float ab) {
    float rm = maxf(ra, rb) + 1.0f;
    IRect r = ir_from(minf(ax, bx) - rm, minf(ay, by) - rm, maxf(ax, bx) + rm, maxf(ay, by) + rm, L->w, L->h);
    if (ir_is_empty(r)) return;
    L->bb = ir_union(L->bb, r);
    float dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
    float inv = len2 > 1e-6f ? 1.0f / len2 : 0.0f;
    for (int y = r.y0; y < r.y1; y++) {
        float py = (float)y + 0.5f;
        float *row = L->cov + (size_t)y * L->w;
        for (int x = r.x0; x < r.x1; x++) {
            float px = (float)x + 0.5f;
            float t = ((px - ax) * dx + (py - ay) * dy) * inv;
            t = clampf(t, 0.0f, 1.0f);
            float qx = ax + t * dx - px, qy = ay + t * dy - py;
            float d = sqrtf(qx * qx + qy * qy);
            float rad = ra + (rb - ra) * t;
            float c;
            if (rad < 0.5f) c = clampf((0.5f + rad) - d, 0.0f, 1.0f) * (rad * 2.0f); /* thin: scale by width */
            else c = clampf(rad - d + 0.5f, 0.0f, 1.0f);
            if (c <= 0.0f) continue;
            c *= aa + (ab - aa) * t;
            if (c > row[x]) { row[x] = c; if (g_par_on) L->par[(size_t)y * L->w + x] = g_pu0 + (g_pu1 - g_pu0) * t; }
        }
    }
}

static IRect layer_composite(Canvas *C, Layer *L, RGBf col, float alpha) {
    IRect r = ir_clip(L->bb, C->w, C->h);
    if (!ir_is_empty(r)) {
        for (int y = r.y0; y < r.y1; y++) {
            float *row = L->cov + (size_t)y * L->w;
            uint32_t *d = C->px + (size_t)y * C->w;
            for (int x = r.x0; x < r.x1; x++) {
                if (row[x] > 0.0f) { blend_px(d + x, col, row[x] * alpha); row[x] = 0.0f; }
            }
        }
    }
    /* zero anything outside the canvas clip too (layer and canvas share size) */
    L->bb = ir_empty();
    return r;
}


/* colour ramps are interpolated in OKLab so the midpoint looks even (white -> black passes through grey, not mud) */
static float srgb_to_lin(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static float lin_to_srgb(float c) { c = clampf(c, 0, 1); return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f; }
static void to_oklab(RGBf c, float o[3]) {
    float r = srgb_to_lin(c.r), g = srgb_to_lin(c.g), b = srgb_to_lin(c.b);
    float l = cbrtf(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    float m = cbrtf(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    float s = cbrtf(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
    o[0] = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    o[1] = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    o[2] = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}
static RGBf from_oklab(const float o[3]) {
    float l = o[0] + 0.3963377774f * o[1] + 0.2158037573f * o[2];
    float m = o[0] - 0.1055613458f * o[1] - 0.0638541728f * o[2];
    float s = o[0] - 0.0894841775f * o[1] - 1.2914855480f * o[2];
    l = l * l * l; m = m * m * m; s = s * s * s;
    RGBf c = { lin_to_srgb(4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s),
               lin_to_srgb(-1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s),
               lin_to_srgb(-0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s) };
    return c;
}
#define RAMP_N 64
typedef struct { RGBf c[RAMP_N + 1]; } Ramp;
static void ramp_make(Ramp *r, RGBf a, RGBf b) {
    float A[3], B[3]; to_oklab(a, A); to_oklab(b, B);
    for (int i = 0; i <= RAMP_N; i++) {
        float t = (float)i / RAMP_N, o[3] = { A[0] + (B[0] - A[0]) * t, A[1] + (B[1] - A[1]) * t, A[2] + (B[2] - A[2]) * t };
        r->c[i] = i == 0 ? a : (i == RAMP_N ? b : from_oklab(o));
    }
}
static inline RGBf ramp_at(const Ramp *r, float u) { int i = (int)(clampf(u, 0, 1) * RAMP_N + 0.5f); return r->c[i]; }
static IRect layer_composite_ramp(Canvas *C, Layer *L, const Ramp *rp, float alpha) {
    IRect r = ir_clip(L->bb, C->w, C->h);
    if (!ir_is_empty(r)) {
        for (int y = r.y0; y < r.y1; y++) {
            float *row = L->cov + (size_t)y * L->w, *pr = L->par + (size_t)y * L->w;
            uint32_t *d = C->px + (size_t)y * C->w;
            for (int x = r.x0; x < r.x1; x++)
                if (row[x] > 0.0f) { blend_px(d + x, ramp_at(rp, pr[x]), row[x] * alpha); row[x] = 0.0f; }
        }
    }
    L->bb = ir_empty();
    return r;
}

static float *g_gb; static size_t g_gbcap;
static int *g_gv; static size_t g_gvcap;
/* 1D squared EDT (Felzenszwalb). Optional payload (colour position) follows the nearest site. */
static void edt_1d(const float *f, float *d, const float *pin, float *pout, int n, int *v, float *z) {
    const float INF = 1e20f;
    int any = 0;
    for (int i = 0; i < n; i++) if (f[i] < 1e19f) { any = 1; break; }
    if (!any) {
        for (int i = 0; i < n; i++) { d[i] = INF; if (pout) pout[i] = pin ? pin[i] : 0; }
        return;
    }
    int k = 0;
    v[0] = 0; z[0] = -INF; z[1] = INF;
    for (int q = 1; q < n; q++) {
        float s;
        for (;;) {
            int p = v[k];
            float den = (float)(2 * (q - p));
            s = den != 0.0f ? ((f[q] + (float)q * (float)q) - (f[p] + (float)p * (float)p)) / den : (float)q;
            if (s > z[k]) break;
            if (--k < 0) { k = 0; s = -INF; break; }
        }
        k++; v[k] = q; z[k] = s; z[k + 1] = INF;
    }
    k = 0;
    for (int q = 0; q < n; q++) {
        while (z[k + 1] < (float)q) k++;
        int p = v[k];
        d[q] = (float)(q - p) * (float)(q - p) + f[p];
        if (pout) pout[q] = pin[p];
    }
}
/* Glow from one coverage mask: a distance field (min/max, never additive), then a smooth
 * falloff that reaches zero at `radius`, so the halo cannot clip into a rectangle. */
static IRect layer_glow_ex(Canvas *C, Layer *L, RGBf col, const Ramp *rp, float strength, float radius, int solid) {
    IRect src = ir_clip(L->bb, C->w, C->h);
    if (ir_is_empty(src)) { L->bb = ir_empty(); return ir_empty(); }
    float R = maxf(radius, 1.0f);
    int pad = (int)ceilf(R) + 2;
    IRect r = { src.x0 - pad, src.y0 - pad, src.x1 + pad, src.y1 + pad };
    r = ir_clip(r, C->w, C->h);
    int w = r.x1 - r.x0, h = r.y1 - r.y0;
    if (w <= 0 || h <= 0) {
        for (int y = src.y0; y < src.y1; y++) for (int x = src.x0; x < src.x1; x++) L->cov[(size_t)y * L->w + x] = 0.0f;
        L->bb = ir_empty();
        return ir_empty();
    }
    int nmax = w > h ? w : h;
    size_t plane = (size_t)w * (size_t)h;
    size_t need = plane * (rp ? 4 : 2) + (size_t)(nmax + 2) * 5;
    if (need > g_gbcap) { float *p = (float *)realloc(g_gb, need * sizeof(float)); if (!p) return ir_empty(); g_gb = p; g_gbcap = need; }
    if ((size_t)(nmax + 2) > g_gvcap) { int *p = (int *)realloc(g_gv, (size_t)(nmax + 2) * sizeof(int)); if (!p) return ir_empty(); g_gv = p; g_gvcap = (size_t)(nmax + 2); }
    float *A = g_gb, *B = A + plane;
    float *PA = rp ? B + plane : NULL, *PB = rp ? PA + plane : NULL;
    float *line_f = (rp ? PB : B) + plane, *line_d = line_f + nmax;
    float *line_pin = line_d + nmax, *line_pout = line_pin + nmax, *z = line_pout + nmax;
    int *v = g_gv;
    const float INF = 1e20f;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            size_t o = (size_t)y * w + x;
            int gx = x + r.x0, gy = y + r.y0;
            float c = 0, u = 0;
            if (gx >= src.x0 && gx < src.x1 && gy >= src.y0 && gy < src.y1) {
                c = L->cov[(size_t)gy * L->w + gx];
                if (rp) u = L->par[(size_t)gy * L->w + gx];
            }
            if (c > 0.01f) {
                float dd = 0.5f - c; if (dd < 0) dd = 0;
                A[o] = dd * dd;
                if (PA) PA[o] = u;
            } else {
                A[o] = INF;
                if (PA) PA[o] = 0;
            }
        }
    }
    for (int y = src.y0; y < src.y1; y++) {
        float *row = L->cov + (size_t)y * L->w;
        for (int x = src.x0; x < src.x1; x++) row[x] = 0.0f;
    }
    L->bb = ir_empty();
    for (int y = 0; y < h; y++)
        edt_1d(A + (size_t)y * w, B + (size_t)y * w, PA ? PA + (size_t)y * w : NULL, PB ? PB + (size_t)y * w : NULL, w, v, z);
    for (int x = 0; x < w; x++) {
        for (int y = 0; y < h; y++) { line_f[y] = B[(size_t)y * w + x]; if (PB) line_pin[y] = PB[(size_t)y * w + x]; }
        edt_1d(line_f, line_d, PB ? line_pin : NULL, PA ? line_pout : NULL, h, v, z);
        for (int y = 0; y < h; y++) { A[(size_t)y * w + x] = line_d[y]; if (PA) PA[(size_t)y * w + x] = line_pout[y]; }
    }
    float gain = clampf(strength, 0, 1);
    float R2 = R * R;
    for (int y = 0; y < h; y++) {
        uint32_t *d = C->px + (size_t)(y + r.y0) * C->w + r.x0;
        for (int x = 0; x < w; x++) {
            float d2 = A[(size_t)y * w + x];
            if (d2 >= R2 || d2 >= 1e19f) continue;
            float u = sqrtf(d2) / R;
            if (u >= 1.0f) continue;
            float s = 1.0f - u;
            s = s * s * (3.0f - 2.0f * s);
            float a = gain * s;
            if (a <= 0.002f) continue;
            RGBf gc = rp ? ramp_at(rp, PA[(size_t)y * w + x]) : col;
            if (solid) {
                /* stepped bands, every pixel fully opaque: colour mixed over a dark base so keying leaves no fringe */
                a = a < 0.4f ? 0.0f : (a < 0.62f ? 0.42f : (a < 0.82f ? 0.64f : 0.85f));
                if (a <= 0) continue;
                RGBf m = { 0.04f + (gc.r - 0.04f) * a, 0.05f + (gc.g - 0.05f) * a, 0.06f + (gc.b - 0.06f) * a };
                if ((d[x] >> 24) >= 128) blend_px(d + x, gc, a * 0.6f);
                else d[x] = 0xFF000000u | ((uint32_t)(m.r * 255 + 0.5f) << 16) | ((uint32_t)(m.g * 255 + 0.5f) << 8) | (uint32_t)(m.b * 255 + 0.5f);
                continue;
            }
            blend_px(d + x, gc, a);
        }
    }
    return r;
}
static IRect layer_glow(Canvas *C, Layer *L, RGBf col, float strength, float radius, int solid) {
    return layer_glow_ex(C, L, col, NULL, strength, radius, solid);
}

static TextRasterFn g_raster;
void render_set_text_raster(TextRasterFn f) { g_raster = f; }
static void stamp_one(Layer *L, const TextMask *m, int ix, int iy) {
    for (int y = 0; y < m->h; y++) {
        int yy = iy + y; if (yy < 0 || yy >= L->h) continue;
        float *row = L->cov + (size_t)yy * L->w;
        const unsigned char *src = m->a + (size_t)y * m->w;
        for (int x = 0; x < m->w; x++) {
            int xx = ix + x; if (xx < 0 || xx >= L->w || !src[x]) continue;
            float c = src[x] / 255.0f; if (c > row[xx]) row[xx] = c;
        }
    }
    IRect r = { ix, iy, ix + m->w, iy + m->h };
    r = ir_clip(r, L->w, L->h);
    if (!ir_is_empty(r)) L->bb = ir_union(L->bb, r);
}
/* text outline: the glyph grown by `extra` px with a soft, round edge (grey-level dilation) */
static void stamp_mask(Layer *L, const TextMask *m, float x, float y, float extra) {
    int ix = (int)lroundf(x), iy = (int)lroundf(y);
    if (extra <= 0.3f) { stamp_one(L, m, ix, iy); return; }
    if (extra > 7.0f) extra = 7.0f;
    int r = (int)ceilf(extra + 1.0f);
    IRect b = ir_clip((IRect){ ix - r, iy - r, ix + m->w + r, iy + m->h + r }, L->w, L->h);
    if (ir_is_empty(b)) return;
    float wt[17][17];
    for (int dy = -r; dy <= r; dy++) for (int dx = -r; dx <= r; dx++)
        wt[dy + r][dx + r] = extra - sqrtf((float)(dx * dx + dy * dy)); /* grown edge: coverage + (extra - distance) */
    for (int y = b.y0; y < b.y1; y++) {
        float *row = L->cov + (size_t)y * L->w;
        for (int x = b.x0; x < b.x1; x++) {
            float best = 0;
            for (int dy = -r; dy <= r && best < 1.0f; dy++) {
                int my = y - iy + dy; if (my < 0 || my >= m->h) continue;
                const unsigned char *src = m->a + (size_t)my * m->w;
                for (int dx = -r; dx <= r; dx++) {
                    int mx = x - ix + dx; if (mx < 0 || mx >= m->w || !src[mx]) continue;
                    float c = src[mx] / 255.0f + wt[dy + r][dx + r]; if (c > best) best = c;
                }
            }
            if (best > 1.0f) best = 1.0f;
            if (best > row[x]) row[x] = best;
        }
    }
    L->bb = ir_union(L->bb, b);
}

IRect draw_disc(Canvas *C, float cx, float cy, float rad, RGBf col, float a) {
    IRect r = ir_from(cx - rad - 1, cy - rad - 1, cx + rad + 1, cy + rad + 1, C->w, C->h);
    for (int y = r.y0; y < r.y1; y++) {
        uint32_t *row = C->px + (size_t)y * C->w;
        float dy = (float)y + 0.5f - cy;
        for (int x = r.x0; x < r.x1; x++) {
            float dx = (float)x + 0.5f - cx;
            float c = clampf(rad - sqrtf(dx * dx + dy * dy) + 0.5f, 0.0f, 1.0f);
            if (c > 0.0f) blend_px(row + x, col, c * a);
        }
    }
    return r;
}

IRect draw_ring(Canvas *C, float cx, float cy, float R, float w, RGBf col, float a) {
    float o = R + w * 0.5f + 1.0f;
    IRect r = ir_from(cx - o, cy - o, cx + o, cy + o, C->w, C->h);
    float hw = w * 0.5f;
    for (int y = r.y0; y < r.y1; y++) {
        uint32_t *row = C->px + (size_t)y * C->w;
        float dy = (float)y + 0.5f - cy;
        for (int x = r.x0; x < r.x1; x++) {
            float dx = (float)x + 0.5f - cx;
            float d = fabsf(sqrtf(dx * dx + dy * dy) - R);
            float c = clampf(hw - d + 0.5f, 0.0f, 1.0f);
            if (c > 0.0f) blend_px(row + x, col, c * a);
        }
    }
    return r;
}

IRect draw_round_rect(Canvas *C, float x, float y, float w, float h, float rad, RGBf col, float a) {
    IRect r = ir_from(x, y, x + w, y + h, C->w, C->h);
    float cx = x + w * 0.5f, cy = y + h * 0.5f, hx = w * 0.5f - rad, hy = h * 0.5f - rad;
    for (int yy = r.y0; yy < r.y1; yy++) {
        uint32_t *row = C->px + (size_t)yy * C->w;
        float qy = fabsf((float)yy + 0.5f - cy) - hy;
        for (int xx = r.x0; xx < r.x1; xx++) {
            float qx = fabsf((float)xx + 0.5f - cx) - hx;
            float mx = maxf(qx, 0.0f), my = maxf(qy, 0.0f);
            float d = sqrtf(mx * mx + my * my) + minf(maxf(qx, qy), 0.0f) - rad;
            float c = clampf(0.5f - d, 0.0f, 1.0f);
            if (c > 0.0f) blend_px(row + xx, col, c * a);
        }
    }
    return r;
}

IRect draw_round_rect_outline(Canvas *C, float x, float y, float w, float h, float rad, float lw, RGBf col, float a) {
    IRect r = ir_from(x - lw, y - lw, x + w + lw, y + h + lw, C->w, C->h);
    float cx = x + w * 0.5f, cy = y + h * 0.5f, hx = w * 0.5f - rad, hy = h * 0.5f - rad;
    for (int yy = r.y0; yy < r.y1; yy++) {
        uint32_t *row = C->px + (size_t)yy * C->w;
        float qy = fabsf((float)yy + 0.5f - cy) - hy;
        for (int xx = r.x0; xx < r.x1; xx++) {
            float qx = fabsf((float)xx + 0.5f - cx) - hx;
            float mx = maxf(qx, 0.0f), my = maxf(qy, 0.0f);
            float d = sqrtf(mx * mx + my * my) + minf(maxf(qx, qy), 0.0f) - rad;
            float c = clampf(lw * 0.5f - fabsf(d) + 0.5f, 0.0f, 1.0f);
            if (c > 0.0f) blend_px(row + xx, col, c * a);
        }
    }
    return r;
}

/* ---------- tiny stroke font: digits, A, P, M ---------- */
typedef struct { Layer *L; float ox, oy, h, rad; } Pen;
static void pseg(Pen *p, float x0, float y0, float x1, float y1) {
    layer_capsule(p->L, p->ox + x0 * p->h, p->oy + y0 * p->h, p->ox + x1 * p->h, p->oy + y1 * p->h,
                  p->rad, p->rad, 1.0f, 1.0f);
}
static void parc(Pen *p, float cx, float cy, float rx, float ry, float a0, float a1) {
    int n = (int)ceilf(fabsf(a1 - a0) / 9.0f);
    if (n < 2) n = 2;
    const float D = 3.14159265f / 180.0f;
    float px = cx + rx * cosf(a0 * D), py = cy + ry * sinf(a0 * D);
    for (int i = 1; i <= n; i++) {
        float a = a0 + (a1 - a0) * (float)i / (float)n;
        float x = cx + rx * cosf(a * D), y = cy + ry * sinf(a * D);
        pseg(p, px, py, x, y);
        px = x; py = y;
    }
}
/* glyph space: height 1, y down; angles in degrees, 270 = up */
static float glyph_adv(char c) {
    switch (c) { case 'M': return 0.66f; case 'P': return 0.50f; case ' ': return 0.30f; default: return 0.56f; }
}
static void glyph(Pen *p, char c) {
    switch (c) {
    case '0': parc(p, .28f, .28f, .28f, .28f, 180, 360); pseg(p, .56f, .28f, .56f, .72f);
              parc(p, .28f, .72f, .28f, .28f, 0, 180); pseg(p, 0, .72f, 0, .28f); break;
    case '1': pseg(p, .32f, 0, .32f, 1); pseg(p, .08f, .2f, .32f, 0); break;
    case '2': parc(p, .28f, .27f, .27f, .27f, 190, 385); pseg(p, .525f, .384f, 0, 1); pseg(p, 0, 1, .56f, 1); break;
    case '3': parc(p, .28f, .25f, .25f, .25f, 200, 450); parc(p, .28f, .75f, .26f, .25f, 270, 520); break;
    case '4': pseg(p, .44f, 1, .44f, 0); pseg(p, .44f, 0, 0, .68f); pseg(p, 0, .68f, .58f, .68f); break;
    case '5': pseg(p, .52f, 0, .06f, 0); pseg(p, .06f, 0, .03f, .47f);
              parc(p, .28f, .7f, .28f, .3f, 215, 505); break;
    case '6': parc(p, .28f, .72f, .28f, .28f, 0, 360); pseg(p, .46f, 0, .03f, .64f); break;
    case '7': pseg(p, 0, 0, .56f, 0); pseg(p, .56f, 0, .16f, 1); break;
    case '8': parc(p, .28f, .25f, .23f, .25f, 0, 360); parc(p, .28f, .74f, .28f, .26f, 0, 360); break;
    case '9': parc(p, .28f, .28f, .28f, .28f, 0, 360); pseg(p, .53f, .36f, .1f, 1); break;
    case 'A': pseg(p, 0, 1, .28f, 0); pseg(p, .28f, 0, .56f, 1); pseg(p, .1f, .66f, .46f, .66f); break;
    case 'P': pseg(p, 0, 1, 0, 0); pseg(p, 0, 0, .25f, 0); parc(p, .25f, .25f, .25f, .25f, 270, 450);
              pseg(p, .25f, .5f, 0, .5f); break;
    case 'V': pseg(p, 0, 0, .28f, 1); pseg(p, .28f, 1, .56f, 0); break;
    case 'G': parc(p, .28f, .5f, .28f, .5f, 315, 0); pseg(p, .56f, .5f, .3f, .5f); break;
    case 'M': pseg(p, 0, 1, 0, 0); pseg(p, 0, 0, .33f, .62f); pseg(p, .33f, .62f, .66f, 0); pseg(p, .66f, 0, .66f, 1); break;
    default: break;
    }
}
static const float TRACK = 0.24f; /* letter spacing, in units of height */
static float text_width(const char *s, int min_chars) {
    float w = 0; int n = 0;
    for (; *s; s++, n++) w += glyph_adv(*s) + TRACK;
    for (; n < min_chars; n++) w += 0.56f + TRACK;
    return w > 0 ? w - TRACK : 0;
}
static void text_layer_ex(Layer *L, const char *s, float x, float y, float h, float weight, float extra) {
    Pen p = { L, x, y, h, h * weight * 0.5f + extra };
    for (; *s; s++) { glyph(&p, *s); p.ox += (glyph_adv(*s) + TRACK) * h; }
}


static int builtin_ok(const char *s) { for (; *s; s++) if (!strchr("0123456789APMVG ", *s)) return 0; return 1; }
static int text_mask(const Style *s, const char *str, float h, TextMask *m) {
    if (!g_raster || !*str) return 0;
    if (s->font <= 0 && builtin_ok(str)) return 0;
    return g_raster(str, h, s->text_bold, s->font > 0 ? s->font : 1, m);
}
static float text_w(const Style *s, const char *str, int minc, float h, float weight) {
    TextMask m;
    if (text_mask(s, str, h, &m)) {
        float w = (float)m.w;
        if (minc) { TextMask z; if (text_mask(s, "000", h, &z) && (float)z.w > w) w = (float)z.w; }
        return w;
    }
    if (!*str && !minc) return 0;
    return text_width(str, minc) * h + h * weight;
}
static void text_cov(Layer *L, const Style *s, const char *str, float x, float y, float h, float weight, float extra) {
    TextMask m;
    if (text_mask(s, str, h, &m)) { stamp_mask(L, &m, x - h * weight * 0.5f, y + h - (float)m.base, extra); return; }
    text_layer_ex(L, str, x, y, h, weight, extra);
}

static float ease_out_cubic(float t) { t = 1.0f - clampf(t, 0, 1); return 1.0f - t * t * t; }

typedef struct {
    char num[16], avg[24];
    float u, numH, labH, wN, wL, px, py, pw, ph, tx, ty, lx, ly, ax, ay, cell, numW, tr, bar;
} ApmLayout;
static void fmt_int(char *out, int v) {
    if (v < 0) v = 0;
    if (v > 99999) v = 99999;
    char t[16]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (int i = 0; i < n; i++) out[i] = t[n - 1 - i];
    out[n] = 0;
}
/* the label / average line use their own boldness and edge settings */
static Style label_style(const Style *s) {
    Style t = *s; t.text_bold = s->lab_bold; t.text_outline = s->lab_outline; t.text_shadow = s->text_shadow; return t;
}
static float res_k(const Style *s) { return s->apm_res > 0 ? s->apm_res : 1.0f; }
/* glyph masks carry a few pixels of padding; this is how far one character advances */
static float mask_adv(const TextMask *m) { return (float)m->w - 6.0f; }
static float digit_cell(const Style *s, float h) {
    TextMask m; float c = 0;
    char d[2] = { 0, 0 };
    for (char ch = '0'; ch <= '9'; ch++) { d[0] = ch; if (!text_mask(s, d, h, &m)) return 0; if (mask_adv(&m) > c) c = mask_adv(&m); }
    return c;
}
/* letter-spaced width; falls back to the normal width for the built-in font */
static float text_w_tr(const Style *s, const char *str, float h, float weight, float tr) {
    TextMask m; char d[2] = { 0, 0 };
    if (!*str) return 0;
    d[0] = str[0];
    if (!text_mask(s, d, h, &m)) return text_w(s, str, 0, h, weight);
    float w = 0; int n = 0;
    for (const char *p = str; *p; p++, n++) { d[0] = *p; if (text_mask(s, d, h, &m)) w += mask_adv(&m); }
    return w + tr * (n - 1) + 6.0f;
}
static void apm_layout(const Style *s, const Scene *sc, ApmLayout *o) {
    float k = sc->scale * res_k(s), u = k * s->apm_scale;
    Style ls = label_style(s);
    o->u = u;
    fmt_int(o->num, sc->apm);
    strcpy(o->avg, "AVG "); fmt_int(o->avg + 4, sc->apm_avg);
    o->numH = 30.0f * u * clampf(s->num_scale > 0 ? s->num_scale : 1.0f, 0.3f, 3.0f);
    o->labH = o->numH * clampf(s->lab_scale > 0 ? s->lab_scale : 0.45f, 0.2f, 1.0f);
    o->wN = s->text_bold ? 0.19f : 0.13f; o->wL = ls.text_bold ? 0.23f : 0.17f;
    float pad = clampf(s->panel_pad, 0, 80) * u, gap = maxf(9.0f * u, o->labH * 0.35f);
    /* the number uses equal-width digit cells so it does not jump around as it changes */
    o->cell = digit_cell(s, o->numH);
    int nd = (int)strlen(o->num); if (nd < 3) nd = 3;
    float numW = o->cell > 0 ? nd * o->cell + 6.0f : text_w(s, o->num, 3, o->numH, o->wN);
    o->numW = numW; o->tr = roundf(o->labH * 0.07f);
    float labW = s->label[0] ? text_w_tr(&ls, s->label, o->labH, o->wL, o->tr) : 0;
    float avgW = 0;
    if (s->apm_show_avg) {
        avgW = text_w_tr(&ls, "AVG 000", o->labH, o->wL, o->tr);
        float aw = text_w_tr(&ls, o->avg, o->labH, o->wL, o->tr);
        if (aw > avgW) avgW = aw;
    }
    o->bar = (s->panel_accent && s->panel_enabled) ? maxf(3.0f, roundf(3.0f * u)) : 0;
    float numExt = o->numH + o->numH * o->wN, labExt = o->labH + o->labH * o->wL, lgap = o->labH * 0.3f;
    float cw, chh;
    /* positions relative to the content's top-left */
    float tx = o->numH * o->wN * 0.5f, ty = o->numH * o->wN * 0.5f, lx = 0, ly = 0, ax = 0, ay = 0;
    if (s->lab_below) {
        float lineW = labW + (labW > 0 && avgW > 0 ? gap : 0) + avgW;
        cw = maxf(numW, lineW);
        tx += (cw - numW) * 0.5f;
        float y2 = numExt + lgap + o->labH * o->wL * 0.5f;
        float x2 = (cw - lineW) * 0.5f + o->labH * o->wL * 0.5f;
        lx = x2; ly = y2; ax = x2 + (labW > 0 ? labW + gap : 0); ay = y2;
        chh = (labW > 0 || avgW > 0) ? numExt + lgap + labExt : numExt;
    } else {
        float colW = maxf(labW, avgW);
        cw = numW + (colW > 0 ? gap + colW : 0);
        float cx = numW + gap + o->labH * o->wL * 0.5f;
        int stack = labW > 0 && avgW > 0 && 2 * labExt + lgap > numExt;
        if (stack) { /* label over average, both centred next to the number */
            float colH = 2 * labExt + lgap; chh = maxf(numExt, colH);
            ty += (chh - numExt) * 0.5f;
            lx = cx; ly = (chh - colH) * 0.5f + o->labH * o->wL * 0.5f; ax = cx; ay = ly + labExt + lgap;
        } else {
            lx = cx; ly = ty + o->numH - o->labH; ax = cx; ay = ty; chh = maxf(numExt, labExt);
            if (labExt > numExt) { ty += labExt - numExt; ly = ty + o->numH - o->labH; }
        }
    }
    float lead = o->bar > 0 ? o->bar + roundf(10.0f * u) : 0;
    o->pw = pad * 2 + cw + lead; o->ph = pad * 2 + chh;
    float ox = s->apm_off_x * k, oy = s->apm_off_y * k, px, py;
    int right = s->apm_corner == APM_TR || s->apm_corner == APM_BR;
    int bottom = s->apm_corner == APM_BL || s->apm_corner == APM_BR;
    px = right ? sc->area_x + sc->area_w - ox - o->pw : sc->area_x + ox;
    py = bottom ? sc->area_y + sc->area_h - oy - o->ph : sc->area_y + oy;
    if (px > sc->area_x + sc->area_w - o->pw) px = sc->area_x + sc->area_w - o->pw;
    if (py > sc->area_y + sc->area_h - o->ph) py = sc->area_y + sc->area_h - o->ph;
    if (px < sc->area_x) px = sc->area_x;
    if (py < sc->area_y) py = sc->area_y;
    o->px = floorf(px); o->py = floorf(py);
    float bx = o->px + pad + lead, by = o->py + pad;
    o->tx = floorf(bx + tx); o->ty = floorf(by + ty);
    o->lx = floorf(bx + lx); o->ly = floorf(by + ly); o->ax = floorf(bx + ax); o->ay = floorf(by + ay);
}

void apm_panel_rect(const Style *s, const Scene *sc, float *x, float *y, float *w, float *h) {
    ApmLayout lay; apm_layout(s, sc, &lay);
    *x = lay.px; *y = lay.py; *w = lay.pw; *h = lay.ph;
}

/* text with optional solid drop shadow and outline (no blur, chroma-safe) */
static IRect text_fx(Canvas *C, Layer *L, const Style *s, float k, const char *str, float x, float y,
                     float h, float weight, RGBf col, float alpha) {
    IRect bb = ir_empty();
    float ot = (s->text_outline && s->outline_px > 0) ? s->outline_px * k : 0.0f;
    if (s->text_shadow) {
        float off = roundf(maxf(1.0f, h * 0.06f + ot * 0.5f));
        text_cov(L, s, str, x + off, y + off, h, weight, ot);
        bb = ir_union(bb, layer_composite(C, L, s->outline_col, alpha * 0.9f));
    }
    if (ot > 0) {
        text_cov(L, s, str, x, y, h, weight, ot);
        bb = ir_union(bb, layer_composite(C, L, s->outline_col, alpha));
    }
    text_cov(L, s, str, x, y, h, weight, 0);
    bb = ir_union(bb, layer_composite(C, L, col, alpha));
    return bb;
}

static IRect text_fx_tr(Canvas *C, Layer *L, const Style *s, float k, const char *str, float x, float y,
                        float h, float weight, float tr, RGBf col, float alpha) {
    TextMask m; char d[2] = { str[0], 0 };
    if (!str[0] || !text_mask(s, d, h, &m)) return text_fx(C, L, s, k, str, x, y, h, weight, col, alpha);
    IRect bb = ir_empty();
    for (const char *p = str; *p; p++) {
        d[0] = *p;
        if (!text_mask(s, d, h, &m)) continue;
        if (*p != ' ') bb = ir_union(bb, text_fx(C, L, s, k, d, roundf(x), y, h, weight, col, alpha));
        x += mask_adv(&m) + tr;
    }
    return bb;
}
typedef struct { float x, y; double t; } TP;
static TP *g_tp; static int g_tpcap;
static int tp_reserve(int n) {
    if (n <= g_tpcap) return 1;
    TP *p = (TP *)realloc(g_tp, (size_t)n * 2 * sizeof(TP));
    if (!p) return 0;
    g_tp = p; g_tpcap = n;
    return 1;
}
static float lpf_a(float cutoff, float dt) {
    float tau = 1.0f / (6.2831853f * maxf(cutoff, 0.01f));
    return 1.0f / (1.0f + tau / maxf(dt, 1e-4f));
}
/* One Euro: stronger on slow jitter, higher cutoff when the cursor moves fast so flicks stay tight. */
static void trail_filter(TP *p, int m, float mincut, float beta) {
    if (m < 2 || mincut <= 0) return;
    float x = p[0].x, y = p[0].y, dxh = 0, dyh = 0;
    for (int i = 1; i < m; i++) {
        float dt = (float)(p[i].t - p[i - 1].t);
        if (dt < 1e-4f) dt = 1e-4f;
        float ad = lpf_a(1.0f, dt);
        float vx = (p[i].x - x) / dt, vy = (p[i].y - y) / dt;
        dxh += ad * (vx - dxh); dyh += ad * (vy - dyh);
        float cut = mincut + beta * sqrtf(dxh * dxh + dyh * dyh);
        float a = lpf_a(cut, dt);
        x += a * (p[i].x - x); y += a * (p[i].y - y);
        p[i].x = x; p[i].y = y;
    }
}
static void trail_relax(TP *p, TP *tmp, int m, int passes, float amt) {
    if (m < 3 || passes <= 0 || amt <= 0) return;
    for (int k = 0; k < passes; k++) {
        tmp[0] = p[0]; tmp[m - 1] = p[m - 1];
        for (int i = 1; i + 1 < m; i++) {
            tmp[i].t = p[i].t;
            tmp[i].x = p[i].x + amt * (0.5f * (p[i - 1].x + p[i + 1].x) - p[i].x);
            tmp[i].y = p[i].y + amt * (0.5f * (p[i - 1].y + p[i + 1].y) - p[i].y);
        }
        memcpy(p, tmp, (size_t)m * sizeof(TP));
    }
}
static int trail_points(const Style *s, const Scene *sc, int first, TP **out) {
    float k = sc->scale;
    int n = sc->ntrail - first;
    int lvl = s->trail_smooth < 0 ? 0 : (s->trail_smooth > 4 ? 4 : s->trail_smooth);
    int want = n * (8 + lvl * 8) + 32; if (want < 256) want = 256; if (want > 8192) want = 8192;
    if (n < 2 || !tp_reserve(want)) return 0;
    TP *a = g_tp, *b = g_tp + g_tpcap;
    int m = 0;
    float skip = lvl > 0 ? 0.36f : 2.25f;
    for (int i = first; i < sc->ntrail; i++) {
        float x = sc->trail[i].x * k + sc->offx, y = sc->trail[i].y * k + sc->offy;
        if (m > 0 && i != sc->ntrail - 1) {
            float dx = x - a[m - 1].x, dy = y - a[m - 1].y;
            if (dx * dx + dy * dy < skip) continue;
        }
        a[m].x = x; a[m].y = y; a[m].t = sc->trail[i].t; m++;
    }
    if (lvl > 0 && m >= 2) {
        enum { PIN = 8 };
        TP head[PIN];
        int nh = m < PIN ? m : PIN;
        for (int i = 0; i < nh; i++) head[i] = a[m - nh + i];
        const float cut[5] = { 0, 9.0f, 5.5f, 3.2f, 1.8f };
        const float beta[5] = { 0, 0.050f, 0.040f, 0.035f, 0.030f };
        const int rpass[5] = { 0, 1, 2, 3, 4 };
        const float ramt[5] = { 0, 0.18f, 0.28f, 0.36f, 0.45f };
        trail_filter(a, m, cut[lvl], beta[lvl]);
        trail_relax(a, b, m, rpass[lvl], ramt[lvl]);
        for (int i = 0; i < nh; i++) {
            int j = m - nh + i;
            float w = (float)(i + 1) / (float)nh;
            w = w * w * (3.0f - 2.0f * w);
            a[j].x = a[j].x * (1.0f - w) + head[i].x * w;
            a[j].y = a[j].y * (1.0f - w) + head[i].y * w;
        }
        a[m - 1] = head[nh - 1];
    }
    /* centripetal Catmull-Rom through the (filtered) samples: passes through each point, no loops */
    if (lvl > 0 && m >= 3) {
        float spacing = 7.0f / (float)lvl;
        int max_steps = 8 + lvl * 6;
        int budget = g_tpcap - 2;
        if (max_steps * (m - 1) + 2 > budget) { max_steps = budget / (m - 1); if (max_steps < 1) max_steps = 1; }
        int q = 0, cap = g_tpcap;
        for (int i = 0; i + 1 < m && q < cap - 2; i++) {
            TP P0 = a[i > 0 ? i - 1 : 0], P1 = a[i], P2 = a[i + 1], P3 = a[i + 2 < m ? i + 2 : m - 1];
            float d01 = sqrtf(sqrtf((P1.x - P0.x) * (P1.x - P0.x) + (P1.y - P0.y) * (P1.y - P0.y))) + 1e-4f;
            float d12 = sqrtf(sqrtf((P2.x - P1.x) * (P2.x - P1.x) + (P2.y - P1.y) * (P2.y - P1.y))) + 1e-4f;
            float d23 = sqrtf(sqrtf((P3.x - P2.x) * (P3.x - P2.x) + (P3.y - P2.y) * (P3.y - P2.y))) + 1e-4f;
            float t0 = 0, t1 = d01, t2 = t1 + d12, t3 = t2 + d23;
            float chord = sqrtf((P2.x - P1.x) * (P2.x - P1.x) + (P2.y - P1.y) * (P2.y - P1.y));
            int steps = (int)ceilf(chord / spacing); if (steps < 1) steps = 1; if (steps > max_steps) steps = max_steps;
            b[q++] = P1;
            for (int j = 1; j < steps && q < cap - 2; j++) {
                float t = t1 + (t2 - t1) * j / steps;
                float A1x = (t1 - t) / (t1 - t0) * P0.x + (t - t0) / (t1 - t0) * P1.x, A1y = (t1 - t) / (t1 - t0) * P0.y + (t - t0) / (t1 - t0) * P1.y;
                float A2x = (t2 - t) / (t2 - t1) * P1.x + (t - t1) / (t2 - t1) * P2.x, A2y = (t2 - t) / (t2 - t1) * P1.y + (t - t1) / (t2 - t1) * P2.y;
                float A3x = (t3 - t) / (t3 - t2) * P2.x + (t - t2) / (t3 - t2) * P3.x, A3y = (t3 - t) / (t3 - t2) * P2.y + (t - t2) / (t3 - t2) * P3.y;
                float B1x = (t2 - t) / (t2 - t0) * A1x + (t - t0) / (t2 - t0) * A2x, B1y = (t2 - t) / (t2 - t0) * A1y + (t - t0) / (t2 - t0) * A2y;
                float B2x = (t3 - t) / (t3 - t1) * A2x + (t - t1) / (t3 - t1) * A3x, B2y = (t3 - t) / (t3 - t1) * A2y + (t - t1) / (t3 - t1) * A3y;
                float u = (float)j / steps;
                b[q].x = (t2 - t) / (t2 - t1) * B1x + (t - t1) / (t2 - t1) * B2x;
                b[q].y = (t2 - t) / (t2 - t1) * B1y + (t - t1) / (t2 - t1) * B2y;
                b[q].t = P1.t + (P2.t - P1.t) * u;
                if (!(b[q].x == b[q].x) || !(b[q].y == b[q].y)) { b[q].x = P1.x + (P2.x - P1.x) * u; b[q].y = P1.y + (P2.y - P1.y) * u; }
                q++;
            }
        }
        b[q++] = a[m - 1];
        a = b; m = q;
    }
    *out = a;
    return m;
}
static void trail_cov(Layer *L, const Style *s, const TP *p, int n, float R, float extra, double now) {
    float len = s->trail_len_s, tp = clampf(s->trail_taper, 0, 1);
    for (int i = 0; i + 1 < n; i++) {
        float uj = clampf((float)((now - p[i].t) / len), 0, 1), ui = clampf((float)((now - p[i + 1].t) / len), 0, 1);
        float fa, fb, ra, rb;
        if (s->solid) {
            fa = fb = 1.0f;
            ra = R * (1.0f - tp * uj) * minf(1.0f, (1.0f - uj) * 4.0f);
            rb = R * (1.0f - tp * ui) * minf(1.0f, (1.0f - ui) * 4.0f);
            if (ra < 0.6f && rb < 0.6f) continue;
            ra = maxf(ra, 0.6f); rb = maxf(rb, 0.6f);
        } else {
            fa = 1.0f - uj * uj; fb = 1.0f - ui * ui;
            ra = R * (1.0f - tp * uj); rb = R * (1.0f - tp * ui);
        }
        g_pu0 = uj; g_pu1 = ui;
        if (fa > 0.003f || fb > 0.003f) layer_capsule(L, p[i].x, p[i].y, p[i + 1].x, p[i + 1].y, ra + extra, rb + extra, fa, fb);
    }
}
static void ring_cov(Layer *L, float x, float y, float R, float rw, float a) {
    const float TAU = 6.2831853f;
    int n = (int)clampf(R * 0.8f, 16, 96);
    float px = x + R, py = y;
    for (int i = 1; i <= n; i++) {
        float qx = x + cosf(TAU * i / n) * R, qy = y + sinf(TAU * i / n) * R;
        layer_capsule(L, px, py, qx, qy, rw * 0.5f, rw * 0.5f, a, a);
        px = qx; py = qy;
    }
}

/* ---------------------------------------------------------------- on-screen keys + mouse */
#define KEY_SLANT 0.194f /* tan(11 degrees) */
typedef KeyItem KDef;
typedef struct { const char *name; int vk; float w; const char *label; } KName;
static const KName knames[] = {
    { "TAB", 0x09, 1.5f, "TAB" }, { "SHIFT", 0x10, 2.25f, "SHIFT" }, { "CTRL", 0x11, 1.5f, "CTRL" }, { "ALT", 0x12, 1.5f, "ALT" },
    { "SPACE", 0x20, 3.25f, "SPACE" }, { "ESC", 0x1B, 1, "ESC" }, { "ENTER", 0x0D, 1.75f, "ENTER" }, { "CAPS", 0x14, 1.75f, "CAPS" },
    { "BKSP", 0x08, 2, "BKSP" }, { "~", 0xC0, 1, "~" }, { "`", 0xC0, 1, "~" }, { "-", 0xBD, 1, "-" }, { "=", 0xBB, 1, "=" },
    { "[", 0xDB, 1, "[" }, { "]", 0xDD, 1, "]" }, { ";", 0xBA, 1, ";" }, { "'", 0xDE, 1, "'" }, { ",", 0xBC, 1, "," }, { ".", 0xBE, 1, "." },
    { "/", 0xBF, 1, "/" }, { "WIN", 0x5B, 1.25f, "WIN" },
    { "UP", 0x26, 1, "UP" }, { "DOWN", 0x28, 1, "DN" }, { "LEFT", 0x25, 1, "LT" }, { "RIGHT", 0x27, 1, "RT" },
    { "SLASH", 0xBF, 1, "/" }, { "COMMA", 0xBC, 1, "," }, { "BSLASH", 0xDC, 1.5f, "\\" }, { "\\", 0xDC, 1.5f, "\\" },
    { "INS", 0x2D, 1, "INS" }, { "DEL", 0x2E, 1, "DEL" }, { "HOME", 0x24, 1, "HOME" }, { "END", 0x23, 1, "END" },
    { "PGUP", 0x21, 1, "PGUP" }, { "PGDN", 0x22, 1, "PGDN" }, { "M4", 0x05, 1, "M4" }, { "M5", 0x06, 1, "M5" },
};
static int kname_find(const char *t, float *w, char *label) {
    char up[16]; int n = 0;
    for (; t[n] && n < 15; n++) up[n] = (char)((t[n] >= 'a' && t[n] <= 'z') ? t[n] - 32 : t[n]);
    up[n] = 0;
    if (n == 1 && ((up[0] >= 'A' && up[0] <= 'Z') || (up[0] >= '0' && up[0] <= '9'))) { *w = 1; strcpy(label, up); return up[0]; }
    if (up[0] == 'F' && n >= 2 && n <= 3) { int f = atoi(up + 1); if (f >= 1 && f <= 12) { *w = 1; strcpy(label, up); return 0x6F + f; } }
    for (size_t i = 0; i < sizeof(knames) / sizeof(knames[0]); i++)
        if (!strcmp(up, knames[i].name)) { *w = knames[i].w; strcpy(label, knames[i].label); return knames[i].vk; }
    return 0;
}
int keys_vk(const char *name, float *width_units) { char l[16]; return kname_find(name, width_units, l); }
int keys_name_info(const char *name, float *w, char *label) { char l[16]; int vk = kname_find(name, w, l); if (label) { l[7] = 0; strcpy(label, l); } return vk; }
static int kadd(KDef *k, int n, int cap, const char *name, float x, float y) {
    if (n >= cap) return n;
    float w; char lab[16]; int vk = kname_find(name, &w, lab);
    if (!vk) return n;
    lab[7] = 0; strcpy(k[n].label, lab);
    { int i = 0; for (; name[i] && i < 7; i++) k[n].name[i] = (char)((name[i] >= 'a' && name[i] <= 'z') ? name[i] - 32 : name[i]); k[n].name[i] = 0; }
    k[n].vk = vk; k[n].x = x; k[n].y = y; k[n].w = w;
    return n + 1;
}
static int kaddw(KDef *k, int n, int cap, const char *name, float x, float y, float w) {
    int m = kadd(k, n, cap, name, x, y); if (m > n) k[n].w = w; return m;
}
static int keys_build(const Style *s, KDef *k, int cap, float *W, float *H) {
    int n = 0;
    if (s->keys_layout == 1) { /* League: abilities, summoners, items, recall and the usual extras, packed tight */
        for (int i = 0; i < 6; i++) { char t[2] = { (char)('1' + i), 0 }; n = kadd(k, n, cap, t, (float)i, 0); }
        n = kadd(k, n, cap, "TAB", 0, 1); { const char *r[] = { "Q", "W", "E", "R" }; for (int i = 0; i < 4; i++) n = kadd(k, n, cap, r[i], 1.5f + i, 1); }
        n = kadd(k, n, cap, "SHIFT", 0, 2); n = kadd(k, n, cap, "D", 2.25f, 2); n = kadd(k, n, cap, "F", 3.25f, 2); n = kadd(k, n, cap, "B", 4.25f, 2);
        n = kadd(k, n, cap, "CTRL", 0, 3); n = kadd(k, n, cap, "ALT", 1.5f, 3); n = kaddw(k, n, cap, "SPACE", 3.0f, 3, 3.0f);
    } else if (s->keys_layout == 3) { /* WASD */
        const char *r0[] = { "~", "1", "2", "3", "4", "5" }, *r1[] = { "Q", "W", "E", "R" }, *r2[] = { "A", "S", "D", "F" }, *r3[] = { "Z", "X", "C", "V" };
        for (int i = 0; i < 6; i++) n = kadd(k, n, cap, r0[i], (float)i, 0);
        n = kadd(k, n, cap, "TAB", 0, 1); for (int i = 0; i < 4; i++) n = kadd(k, n, cap, r1[i], 1.5f + i, 1);
        n = kadd(k, n, cap, "CAPS", 0, 2); for (int i = 0; i < 4; i++) n = kadd(k, n, cap, r2[i], 1.75f + i, 2);
        n = kadd(k, n, cap, "SHIFT", 0, 3); for (int i = 0; i < 4; i++) n = kadd(k, n, cap, r3[i], 2.25f + i, 3);
        n = kadd(k, n, cap, "CTRL", 0, 4); n = kadd(k, n, cap, "ALT", 1.5f, 4); n = kaddw(k, n, cap, "SPACE", 3.0f, 4, 3.25f);
    } else if (s->keys_layout == 4) { /* full main block */
        const char *r0 = "~1234567890-=", *r1 = "QWERTYUIOP[]", *r2 = "ASDFGHJKL;'", *r3 = "ZXCVBNM,./";
        for (int i = 0; r0[i]; i++) { char t[2] = { r0[i], 0 }; n = kadd(k, n, cap, t, (float)i, 0); }
        n = kadd(k, n, cap, "BKSP", 13, 0);
        n = kadd(k, n, cap, "TAB", 0, 1); for (int i = 0; r1[i]; i++) { char t[2] = { r1[i], 0 }; n = kadd(k, n, cap, t, 1.5f + i, 1); }
        n = kadd(k, n, cap, "CAPS", 0, 2); for (int i = 0; r2[i]; i++) { char t[2] = { r2[i], 0 }; n = kadd(k, n, cap, t, 1.75f + i, 2); }
        n = kaddw(k, n, cap, "ENTER", 12.75f, 2, 2.25f);
        n = kadd(k, n, cap, "SHIFT", 0, 3); for (int i = 0; r3[i]; i++) { char t[2] = { r3[i], 0 }; n = kadd(k, n, cap, t, 2.25f + i, 3); }
        n = kaddw(k, n, cap, "SHIFT", 12.25f, 3, 2.75f);
        n = kadd(k, n, cap, "CTRL", 0, 4); n = kadd(k, n, cap, "WIN", 1.5f, 4); n = kaddw(k, n, cap, "ALT", 2.75f, 4, 1.25f);
        n = kaddw(k, n, cap, "SPACE", 4.0f, 4, 6.0f); n = kaddw(k, n, cap, "ALT", 10.0f, 4, 1.25f); n = kaddw(k, n, cap, "CTRL", 11.25f, 4, 1.5f);
    } else if (s->keys_layout == 0) {
        const char *r0[] = { "~", "1", "2", "3", "4", "5" }, *r1[] = { "Q", "W", "E", "R", "T" }, *r2[] = { "A", "S", "D", "F", "G" }, *r3[] = { "Z", "X", "C", "V", "B" };
        for (int i = 0; i < 6; i++) n = kadd(k, n, cap, r0[i], (float)i, 0);
        n = kadd(k, n, cap, "TAB", 0, 1); for (int i = 0; i < 5; i++) n = kadd(k, n, cap, r1[i], 1.5f + i, 1);
        for (int i = 0; i < 5; i++) n = kadd(k, n, cap, r2[i], 1.75f + i, 2);
        n = kadd(k, n, cap, "SHIFT", 0, 3); for (int i = 0; i < 5; i++) n = kadd(k, n, cap, r3[i], 2.25f + i, 3);
        n = kadd(k, n, cap, "CTRL", 0, 4); n = kadd(k, n, cap, "ALT", 1.5f, 4); n = kadd(k, n, cap, "SPACE", 3.0f, 4);
    } else {
        const char *p = s->keys_custom; float x = 0, y = 0; char tok[32];
        while (*p) {
            while (*p == ' ' || *p == ',') p++;
            if (!*p) break;
            if (*p == '/' || *p == '|') { p++; y += 1; x = 0; continue; }
            int t = 0; while (*p && *p != ' ' && *p != '/' && *p != '|' && t < 31) tok[t++] = *p++;
            tok[t] = 0;
            while (*p && *p != ' ' && *p != '/' && *p != '|') p++;
            /* placed key: NAME@x,y with an optional :width */
            float px = x, py = y, pw = -1; char *at = strchr(tok + 1, '@'), *co = strchr(tok + 1, ':');
            if (at) { px = strtof(at + 1, NULL); char *c = strchr(at, ','); if (c) py = strtof(c + 1, NULL); }
            if (co) pw = strtof(co + 1, NULL);
            if (at) *at = 0;
            if (co) *co = 0;
            if (!(px >= -50 && px <= 60)) px = 0;
            if (!(py >= -50 && py <= 20)) py = 0;
            int before = n; n = kadd(k, n, cap, tok, px, py);
            if (n > before) { if (pw >= 0.5f && pw <= 10) k[n - 1].w = pw; x = px + k[n - 1].w; y = py; }
        }
    }
    if (s->keys_layout == 2 && n > 0) { /* placed keys: start at 0,0 */
        float mx = k[0].x, my = k[0].y;
        for (int i = 1; i < n; i++) { if (k[i].x < mx) mx = k[i].x; if (k[i].y < my) my = k[i].y; }
        for (int i = 0; i < n; i++) { k[i].x -= mx; k[i].y -= my; }
    }
    float w = 0, h = 0;
    for (int i = 0; i < n; i++) { if (k[i].x + k[i].w > w) w = k[i].x + k[i].w; if (k[i].y + 1 > h) h = k[i].y + 1; }
    *W = w; *H = h;
    return n;
}
typedef struct { float px, py, pw, ph, U, lw, pad, kbx, kby, mx, my, mw, mh, mpx, mpy, mpw, mph; int nk, msep; KDef k[KEYS_MAX]; } KeysLay;
static void place_panel(const Scene *sc, int corner, float ox, float oy, float w, float h, float *x, float *y) {
    int right = corner == APM_TR || corner == APM_BR, bottom = corner == APM_BL || corner == APM_BR;
    float px = right ? sc->area_x + sc->area_w - ox - w : sc->area_x + ox;
    float py = bottom ? sc->area_y + sc->area_h - oy - h : sc->area_y + oy;
    if (px > sc->area_x + sc->area_w - w) px = sc->area_x + sc->area_w - w;
    if (py > sc->area_y + sc->area_h - h) py = sc->area_y + sc->area_h - h;
    if (px < sc->area_x) px = sc->area_x;
    if (py < sc->area_y) py = sc->area_y;
    *x = floorf(px); *y = floorf(py);
}
static void keys_layout(const Style *s, const Scene *sc, KeysLay *o) {
    float k = sc->scale * res_k(s), U = roundf(40.0f * k * clampf(s->keys_scale > 0 ? s->keys_scale : 1, 0.3f, 4.0f));
    if (U < 8) U = 8;
    if (U > 200) U = 200; /* about 3x the 4K default; keeps the panel on screen and fast */
    float kw = 0, kh = 0;
    o->nk = s->keys_kb ? keys_build(s, o->k, KEYS_MAX, &kw, &kh) : 0;
    if (o->nk == 0) kw = kh = 0;
    o->U = U;
    o->lw = maxf(1.0f, roundf(clampf(s->keys_line > 0 ? s->keys_line : 2.0f, 0.5f, 8.0f) * k * clampf(s->keys_scale > 0 ? s->keys_scale : 1, 0.5f, 2.0f)));
    o->mw = s->keys_mouse ? roundf(2.15f * U) : 0; o->mh = s->keys_mouse ? roundf(3.55f * U) : 0;
    o->msep = s->keys_mouse && s->mouse_sep && o->nk > 0;
    /* slanted keys and mouse lean past their boxes by half their height: leave room for that */
    float sx = s->keys_shape == 1 ? KEY_SLANT : 0;
    float ek = (sx > 0 && o->nk) ? ceilf(sx * U * 0.5f) + 1 : 0, em = (sx > 0 && s->keys_mouse) ? ceilf(sx * o->mh * 0.5f) + 1 : 0;
    float inw = o->msep ? 0 : (o->mw > 0 ? o->mw + 2 * em : 0), inh = o->msep ? 0 : o->mh;
    float gap = (o->nk && inw > 0) ? roundf((sx > 0 ? 0.30f : 0.42f) * U) : 0;
    o->pad = s->keys_plate ? roundf(0.30f * U) : 0;
    o->pw = roundf(kw * U) + 2 * ek + gap + inw + o->pad * 2; o->ph = maxf(roundf(kh * U), inh) + o->pad * 2;
    place_panel(sc, s->keys_corner, s->keys_off_x * k, s->keys_off_y * k, o->pw, o->ph, &o->px, &o->py);
    float ih = o->ph - o->pad * 2;
    o->kbx = o->px + o->pad + ek; o->kby = o->py + o->pad + floorf((ih - roundf(kh * U)) * 0.5f);
    if (o->msep) {
        o->mpw = o->mw + 2 * em + o->pad * 2; o->mph = o->mh + o->pad * 2;
        place_panel(sc, s->mouse_corner, s->mouse_off_x * k, s->mouse_off_y * k, o->mpw, o->mph, &o->mpx, &o->mpy);
        o->mx = o->mpx + o->pad + em; o->my = o->mpy + o->pad;
    } else {
        o->mx = o->px + o->pad + roundf(kw * U) + 2 * ek + gap + em; o->my = o->py + o->pad + floorf((ih - o->mh) * 0.5f);
        o->mpx = o->mx - em - o->pad; o->mpy = o->my - o->pad; o->mpw = o->mw + 2 * em + o->pad * 2; o->mph = o->mh + o->pad * 2;
    }
}
int keys_items(const Style *s, KeyItem *out, int cap) {
    float w, h; KDef tmp[KEYS_MAX]; int n = keys_build(s, tmp, KEYS_MAX, &w, &h);
    if (n > cap) n = cap;
    memcpy(out, tmp, sizeof(KDef) * (size_t)n); return n;
}
void keys_geom(const Style *s, const Scene *sc, float *x0, float *y0, float *unit) {
    KeysLay *L = (KeysLay *)malloc(sizeof(KeysLay)); if (!L) { *x0 = *y0 = 0; *unit = 40; return; }
    keys_layout(s, sc, L); *x0 = L->kbx; *y0 = L->kby; *unit = L->U; free(L);
}
void keys_panel_rect(const Style *s, const Scene *sc, float *x, float *y, float *w, float *h) {
    KeysLay L; keys_layout(s, sc, &L); *x = L.px; *y = L.py; *w = L.pw; *h = L.ph;
}
/* where the mouse is drawn; when it sits next to the keys this is its spot inside the keys panel */
void mouse_panel_rect(const Style *s, const Scene *sc, float *x, float *y, float *w, float *h) {
    KeysLay L; keys_layout(s, sc, &L); *x = L.mpx; *y = L.mpy; *w = s->keys_mouse ? L.mpw : 0; *h = s->keys_mouse ? L.mph : 0;
}
/* lean for slanted keys: x shifts by g_shx * (centre - y), so the top leans right. Each key leans
   around its own middle; the mouse leans around one shared middle (g_shc_on). */
static float g_shx, g_shc; static int g_shc_on;
static float rr_sd(float px, float py, float x, float y, float w, float h, float r) {
    if (g_shx != 0) px += g_shx * (py - (g_shc_on ? g_shc : y + h * 0.5f));
    float qx = fabsf(px - (x + w * 0.5f)) - (w * 0.5f - r), qy = fabsf(py - (y + h * 0.5f)) - (h * 0.5f - r);
    float ox = maxf(qx, 0), oy = maxf(qy, 0);
    return sqrtf(ox * ox + oy * oy) + minf(maxf(qx, qy), 0) - r;
}
/* rounded rect coverage grown by `grow` (negative = shrunk), written into the layer (max) */
static void layer_rr(Layer *L, float x, float y, float w, float h, float r, float grow) {
    r = clampf(r, 0, minf(w, h) * 0.5f);
    float ex = g_shx != 0 ? fabsf(g_shx) * (h * 0.5f + fabsf(grow) + 2 + (g_shc_on ? fabsf(y + h * 0.5f - g_shc) : 0)) : 0;
    IRect b = ir_from(x - grow - 1 - ex, y - grow - 1, x + w + grow + 1 + ex, y + h + grow + 1, L->w, L->h);
    if (ir_is_empty(b)) return;
    L->bb = ir_union(L->bb, b);
    for (int yy = b.y0; yy < b.y1; yy++) {
        float *row = L->cov + (size_t)yy * L->w;
        for (int xx = b.x0; xx < b.x1; xx++) {
            float c = clampf(0.5f - (rr_sd(xx + 0.5f, yy + 0.5f, x, y, w, h, r) - grow), 0, 1);
            if (c > row[xx]) row[xx] = c;
        }
    }
}
/* outline band: the shape minus the shape shrunk by lw */
static void layer_rr_ring(Layer *L, float x, float y, float w, float h, float r, float lw) {
    r = clampf(r, 0, minf(w, h) * 0.5f);
    float ex = g_shx != 0 ? fabsf(g_shx) * (h * 0.5f + 2 + (g_shc_on ? fabsf(y + h * 0.5f - g_shc) : 0)) : 0;
    IRect b = ir_from(x - 1 - ex, y - 1, x + w + 1 + ex, y + h + 1, L->w, L->h);
    if (ir_is_empty(b)) return;
    L->bb = ir_union(L->bb, b);
    for (int yy = b.y0; yy < b.y1; yy++) {
        float *row = L->cov + (size_t)yy * L->w;
        for (int xx = b.x0; xx < b.x1; xx++) {
            float d = rr_sd(xx + 0.5f, yy + 0.5f, x, y, w, h, r);
            float c = clampf(0.5f - d, 0, 1) - clampf(0.5f - (d + lw), 0, 1);
            if (c > row[xx]) row[xx] = c;
        }
    }
}
static float lum(RGBf c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }
static RGBf mixc(RGBf a, RGBf b, float t) { return (RGBf){ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t }; }
static RGBf shade(RGBf c, float f) { return (RGBf){ c.r * f, c.g * f, c.b * f }; }

/* ---- mouse: a smooth egg-like superellipse, drawn from 4x4 supersampled masks cached per size */
typedef struct { float W, H, lw, U, split, cx; } MouseGeo;
static float mouse_half_w(const MouseGeo *g, float y) { float t = clampf(y / g->H, 0, 1); return g->W * 0.5f * (0.90f + 0.10f * sqrtf(t)); }
static int in_body(const MouseGeo *g, float x, float y, float m) {
    float b = g->H * 0.5f - m, a = mouse_half_w(g, y) - m; if (a <= 0 || b <= 0) return 0;
    float dx = fabsf(x - g->cx) / a, dy = fabsf(y - g->H * 0.5f) / b;
    return powf(dx, 2.4f) + powf(dy, 2.9f) <= 1.0f;
}
/* x of the body's left edge at height y (m = inset) */
static float body_left(const MouseGeo *g, float y, float m) {
    float b = g->H * 0.5f - m, a = mouse_half_w(g, y) - m; if (a <= 0 || b <= 0) return g->cx;
    float dy = fminf(fabsf(y - g->H * 0.5f) / b, 1.0f);
    return g->cx - a * powf(fmaxf(0.0f, 1.0f - powf(dy, 2.9f)), 1.0f / 2.4f);
}
enum { MM_BODY, MM_L, MM_R, MM_LOW, MM_N };
static float *g_mm[MM_N]; static int g_mm_w, g_mm_h; static float g_mm_key[3];
static int mouse_region(const MouseGeo *g, int which, float x, float y) {
    float gap = g->lw;
    switch (which) {
    case MM_BODY: return in_body(g, x, y, 0);
    case MM_L: return in_body(g, x, y, g->lw) && x < g->cx - gap * 0.5f && y < g->split - gap * 0.5f;
    case MM_R: return in_body(g, x, y, g->lw) && x > g->cx + gap * 0.5f && y < g->split - gap * 0.5f;
    case MM_LOW: return in_body(g, x, y, g->lw) && y > g->split + gap * 0.5f;
    }
    return 0;
}
static int mouse_masks(const MouseGeo *g) {
    int w = (int)ceilf(g->W) + 2, h = (int)ceilf(g->H) + 2;
    if (g_mm[0] && g_mm_w == w && g_mm_h == h && g_mm_key[0] == g->W && g_mm_key[1] == g->H && g_mm_key[2] == g->lw) return 1;
    for (int i = 0; i < MM_N; i++) { free(g_mm[i]); g_mm[i] = (float *)calloc((size_t)w * h, sizeof(float)); if (!g_mm[i]) { g_mm_w = 0; return 0; } }
    g_mm_w = w; g_mm_h = h; g_mm_key[0] = g->W; g_mm_key[1] = g->H; g_mm_key[2] = g->lw;
    for (int i = 0; i < MM_N; i++)
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                /* corners agree: fully in or out; otherwise 4x4 samples */
                int c = mouse_region(g, i, (float)x, (float)y), n;
                if (c == mouse_region(g, i, x + 1.0f, (float)y) && c == mouse_region(g, i, (float)x, y + 1.0f) && c == mouse_region(g, i, x + 1.0f, y + 1.0f) &&
                    c == mouse_region(g, i, x + 0.5f, y + 0.5f)) n = c ? 16 : 0;
                else { n = 0; for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++) n += mouse_region(g, i, x + (sx + 0.5f) / 4.0f, y + (sy + 0.5f) / 4.0f); }
                g_mm[i][(size_t)y * w + x] = n / 16.0f;
            }
    return 1;
}
/* add mask (optionally minus another) into the layer at integer offset */
static float mask_at(int which, int minus, int mx, int my) {
    if (mx < 0 || mx >= g_mm_w || my < 0 || my >= g_mm_h) return 0;
    size_t i = (size_t)my * g_mm_w + mx; float c = g_mm[which][i];
    if (minus) c -= maxf(maxf(g_mm[MM_L][i], g_mm[MM_R][i]), g_mm[MM_LOW][i]);
    return c;
}
static void layer_mask(Layer *L, int which, int minus, int ox, int oy) {
    int ex = g_shx != 0 ? (int)ceilf(fabsf(g_shx) * g_mm_h * 0.5f) + 2 : 0;
    IRect b = ir_clip((IRect){ ox - ex, oy, ox + g_mm_w + ex, oy + g_mm_h }, L->w, L->h);
    if (ir_is_empty(b)) return;
    L->bb = ir_union(L->bb, b);
    for (int y = b.y0; y < b.y1; y++) {
        float *row = L->cov + (size_t)y * L->w;
        int my = y - oy;
        if (g_shx == 0) {
            for (int x = b.x0; x < b.x1; x++) { float c = mask_at(which, minus, x - ox, my); if (c > row[x]) row[x] = c; }
            continue;
        }
        float shift = g_shx * (g_shc - (y + 0.5f)); /* rows above the middle move right */
        for (int x = b.x0; x < b.x1; x++) {
            float u = (float)(x - ox) - shift; int u0 = (int)floorf(u); float t = u - u0;
            float c = mask_at(which, minus, u0, my) * (1 - t) + mask_at(which, minus, u0 + 1, my) * t;
            if (c > row[x]) row[x] = c;
        }
    }
}

/* the same, grown by `rad` pixels each way (a ring grows inward and outward) */
static void layer_mask_grown(Layer *L, int which, int minus, int ox, int oy, int rad) {
    int ex = g_shx != 0 ? (int)ceilf(fabsf(g_shx) * g_mm_h * 0.5f) + 2 : 0;
    IRect b = ir_clip((IRect){ ox - rad - ex, oy - rad, ox + g_mm_w + rad + ex, oy + g_mm_h + rad }, L->w, L->h);
    if (ir_is_empty(b)) return;
    L->bb = ir_union(L->bb, b);
    for (int y = b.y0; y < b.y1; y++) {
        float *row = L->cov + (size_t)y * L->w;
        for (int x = b.x0; x < b.x1; x++) {
            float best = 0;
            for (int dy = -rad; dy <= rad; dy++) {
                int my = y - oy + dy; if (my < 0 || my >= g_mm_h) continue;
                int sft = g_shx != 0 ? (int)lroundf(g_shx * (g_shc - (y + dy + 0.5f))) : 0;
                for (int dx = -rad; dx <= rad; dx++) {
                    int mx = x - ox + dx - sft; if (mx < 0 || mx >= g_mm_w || dx * dx + dy * dy > rad * rad + rad) continue;
                    size_t i = (size_t)my * g_mm_w + mx; float c = g_mm[which][i];
                    if (minus) c -= maxf(maxf(g_mm[MM_L][i], g_mm[MM_R][i]), g_mm[MM_LOW][i]);
                    if (c > best) best = c;
                }
            }
            if (best > row[x]) row[x] = best;
        }
    }
}

typedef struct { float x, y, w, h, r; float lv; } KShape;
/* side button i (0 = front, 1 = back), in mouse coordinates: fully inside the body, clear of its
   outline by `clear` px over the whole height of the button */
static KShape mouse_side_btn(const MouseGeo *g, int i, float clear) {
    float U = g->U, lw = g->lw;
    float sh = roundf(0.48f * U), sw = roundf(0.17f * U) + 2 * lw;
    float sy = roundf(g->H * 0.50f + i * (sh + maxf(roundf(0.10f * U), 2 * lw)));
    float edge = 0;
    for (float y = sy - 1; y <= sy + sh + 1; y += 0.5f) edge = maxf(edge, body_left(g, y, 0));
    KShape k = { ceilf(edge + lw + clear), sy, sw, sh, sw * 0.5f, 0 };
    return k;
}
/* wheel: centred on the middle line, in the middle of the top part */
static KShape mouse_wheel_shape(const MouseGeo *g) {
    float ww = roundf(0.30f * g->U) + 2 * g->lw, wh = roundf(0.62f * g->U) + 2 * g->lw;
    float top = 0; /* the body reaches y = 0 on the middle line */
    KShape k = { roundf(g->cx - ww * 0.5f), roundf((top + g->lw + g->split - g->lw * 0.5f - wh) * 0.5f), ww, wh, ww * 0.5f, 0 };
    return k;
}
/* one part: outline band in outline colour, then the inside in a fill colour.
   With an opaque fill the outline is a full opaque base under it, so every soft edge between the two
   lies inside solid pixels; only the outer silhouette meets the background. */
static IRect draw_part(Canvas *C, Layer *L, const KShape *k, float lw, RGBf line, RGBf fill, float fill_a) {
    IRect bb = ir_empty();
    if (fill_a >= 0.999f) layer_rr(L, k->x, k->y, k->w, k->h, k->r, 0);
    else layer_rr_ring(L, k->x, k->y, k->w, k->h, k->r, lw);
    bb = ir_union(bb, layer_composite(C, L, line, 1.0f));
    if (fill_a > 0.001f) {
        layer_rr(L, k->x + lw, k->y + lw, k->w - 2 * lw, k->h - 2 * lw, maxf(k->r - lw, 0), 0);
        bb = ir_union(bb, layer_composite(C, L, fill, fill_a));
    }
    return bb;
}
static IRect render_keys(Canvas *C, Layer *L, const Style *s, const Scene *sc) {
    IRect bb = ir_empty();
    KeysLay *o = (KeysLay *)malloc(sizeof(KeysLay));
    if (!o) return bb;
    keys_layout(s, sc, o);
    if (o->pw <= 0 || o->ph <= 0) { free(o); return bb; }
    int slant = s->keys_shape == 1;
    float U = o->U, lw = o->lw, g = maxf(roundf((slant ? 0.08f : 0.10f) * U), 2.0f), depth = s->keys_depth ? maxf(roundf(0.08f * U), 1.0f) : 0;
    int solid = s->solid;
    float idle_a = clampf(s->keys_idle_op, 0, 1);
    if (solid) idle_a = idle_a >= 0.5f ? 1.0f : 0.0f;
    RGBf on = s->keys_on, lab_on = lum(s->keys_on) > 0.55f ? (RGBf){ 0.06f, 0.07f, 0.09f } : (RGBf){ 1, 1, 1 };
    /* outline-only keys with a light outline get a thin dark edge on both sides, so they read on any
       background and stay clean on green (the soft edge of the light line falls on the dark edge) */
    float rim = (idle_a < 0.5f && lum(s->keys_outline) > 0.45f) ? maxf(1.0f, roundf(lw * 0.5f)) : 0;
    RGBf rimc = { 0.02f, 0.025f, 0.035f };
    /* optional backplate behind everything: also gives the soft edges something solid to blend into */
    if (s->keys_plate) {
        float pa = solid ? 1.0f : clampf(s->keys_plate_op, 0, 1);
        if (o->pw > 2 * o->pad) { layer_rr(L, o->px, o->py, o->pw, o->ph, roundf(0.36f * U), 0); bb = ir_union(bb, layer_composite(C, L, s->keys_plate_col, pa)); }
        if (o->msep) { layer_rr(L, o->mpx, o->mpy, o->mpw, o->mph, roundf(0.36f * U), 0); bb = ir_union(bb, layer_composite(C, L, s->keys_plate_col, pa)); }
    }
    /* keys */
    KShape ks[KEYS_MAX]; int n = o->nk;
    for (int i = 0; i < n; i++) {
        const KDef *d = &o->k[i];
        float x0 = roundf(o->kbx + d->x * U + g * 0.5f), x1 = roundf(o->kbx + (d->x + d->w) * U - g * 0.5f);
        float y0 = roundf(o->kby + d->y * U + g * 0.5f), y1 = roundf(o->kby + (d->y + 1) * U - g * 0.5f) - depth;
        float lv = sc->klevel ? clampf(sc->klevel[d->vk & 255], 0, 1) : ((sc->kdown && sc->kdown[d->vk & 255]) ? 1.0f : 0.0f);
        if (solid) lv = lv >= 0.5f ? 1.0f : 0.0f;
        ks[i] = (KShape){ x0, y0, x1 - x0, y1 - y0, roundf((slant ? 0.10f : 0.17f) * U), lv };
    }
    g_shx = slant ? KEY_SLANT : 0; g_shc_on = 0;
    if (s->keys_glow) {
        int any = 0;
        for (int i = 0; i < n; i++) if (ks[i].lv > 0.5f) { any = 1; layer_rr(L, ks[i].x, ks[i].y + depth, ks[i].w, ks[i].h, ks[i].r, 0); }
        if (any) bb = ir_union(bb, layer_glow(C, L, on, clampf(s->glow_strength > 0 ? s->glow_strength : 0.6f, 0, 1), maxf(s->glow_radius, 4.0f) * sc->scale, solid));
    }
    if (depth > 0) { /* key sides: a darker lip under each key; a held key sinks onto it */
        for (int i = 0; i < n; i++) layer_rr(L, ks[i].x, ks[i].y + depth, ks[i].w, ks[i].h, ks[i].r, 0);
        RGBf lip = idle_a > 0 ? shade(mixc(s->keys_idle, s->keys_outline, 0.35f), 0.55f) : shade(s->keys_outline, 0.6f);
        bb = ir_union(bb, layer_composite(C, L, lip, 1.0f));
    }
    if (rim > 0 && n > 0) {
        for (int i = 0; i < n; i++) { KShape k = ks[i]; k.y += roundf(depth * k.lv);
            layer_rr_ring(L, k.x - rim, k.y - rim, k.w + 2 * rim, k.h + 2 * rim, k.r + rim, lw + 2 * rim); }
        bb = ir_union(bb, layer_composite(C, L, rimc, 1.0f));
    }
    for (int i = 0; i < n; i++) {
        KShape k = ks[i]; k.y += roundf(depth * k.lv);
        float fa = idle_a + (1.0f - idle_a) * k.lv;
        RGBf fill = idle_a > 0 ? mixc(s->keys_idle, on, k.lv) : on;
        if (fa >= 0.999f) bb = ir_union(bb, draw_part(C, L, &k, lw, s->keys_outline, fill, 1.0f));
        else bb = ir_union(bb, draw_part(C, L, &k, lw, s->keys_outline, fill, fa));
        ks[i].y = k.y;
    }
    /* labels */
    g_shx = 0;
    /* slanted: big, bold, narrow letters leaning with the keys (0x200 narrow, 0x400 lean) */
    Style ts; memset(&ts, 0, sizeof ts); ts.font = (s->keys_font > 0 ? s->keys_font : 2) | (slant ? 0x600 : (s->keys_italic ? 0x100 : 0)); ts.text_bold = 1;
    for (int i = 0; i < n; i++) {
        const char *t = o->k[i].label; int len = (int)strlen(t);
        float h = roundf((len > 1 ? (slant ? 0.33f : 0.25f) : (slant ? 0.44f : 0.36f)) * U), tw = text_w(&ts, t, 0, h, 0);
        float room = ks[i].w - 2 * lw - 0.10f * U;
        if (tw > room && tw > 0) { h = floorf(h * room / tw); tw = text_w(&ts, t, 0, h, 0); }
        float lx = roundf(ks[i].x + (ks[i].w - tw) * 0.5f), ly = roundf(ks[i].y + (ks[i].h - h) * 0.5f);
        if (rim > 0 && ks[i].lv < 0.5f) { text_cov(L, &ts, t, lx, ly, h, 0, rim); bb = ir_union(bb, layer_composite(C, L, rimc, 1.0f)); }
        text_cov(L, &ts, t, lx, ly, h, 0, 0);
        bb = ir_union(bb, layer_composite(C, L, mixc(s->keys_label, lab_on, ks[i].lv), 1.0f));
    }
    /* mouse */
    if (s->keys_mouse) {
        MouseGeo mg = { o->mw, o->mh, lw, U, o->mh * 0.40f, o->mw * 0.5f };
        int ox = (int)o->mx, oy = (int)o->my;
        g_shx = slant ? KEY_SLANT : 0; g_shc_on = 1; g_shc = oy + mg.H * 0.5f;
        float lvl[5];
        for (int i = 0; i < 5; i++) { lvl[i] = sc->mlevel_set ? clampf(sc->mlevel[i], 0, 1) : (sc->mdown[i] ? 1.0f : 0.0f); if (solid) lvl[i] = lvl[i] >= 0.5f ? 1.0f : 0.0f; }
        if (mouse_masks(&mg)) {
            if (rim > 0) { layer_mask_grown(L, MM_BODY, 1, ox, oy, (int)minf(rim, 3.0f)); bb = ir_union(bb, layer_composite(C, L, rimc, 1.0f)); }
            layer_mask(L, MM_BODY, idle_a < 0.999f, ox, oy);
            bb = ir_union(bb, layer_composite(C, L, s->keys_outline, 1.0f));
            int reg[3] = { MM_L, MM_R, MM_LOW }; float rl[3] = { lvl[0], lvl[1], 0 };
            for (int r = 0; r < 3; r++) {
                float fa = idle_a + (1.0f - idle_a) * rl[r];
                if (fa <= 0.001f) continue;
                layer_mask(L, reg[r], 0, ox, oy);
                bb = ir_union(bb, layer_composite(C, L, idle_a > 0 ? mixc(s->keys_idle, on, rl[r]) : on, fa));
            }
        }
        /* wheel and side buttons */
        float wl = maxf(lvl[2], sc->wheel != 0 ? 1.0f : 0.0f);
        KShape wheel = mouse_wheel_shape(&mg); wheel.x += ox; wheel.y += oy; wheel.lv = wl;
        if (rim > 0) { layer_rr_ring(L, wheel.x - rim, wheel.y - rim, wheel.w + 2 * rim, wheel.h + 2 * rim, wheel.r + rim, lw + 2 * rim); bb = ir_union(bb, layer_composite(C, L, rimc, 1.0f)); }
        bb = ir_union(bb, draw_part(C, L, &wheel, lw, s->keys_outline, idle_a > 0 ? mixc(s->keys_idle, on, wl) : on, idle_a + (1.0f - idle_a) * wl));
        for (int i = 0; i < 2; i++) {
            KShape sb = mouse_side_btn(&mg, i, maxf(1.0f, roundf(0.06f * U)) + rim + 1.0f);
            sb.x += ox; sb.y += oy; sb.lv = lvl[3 + i];
            if (rim > 0) { layer_rr_ring(L, sb.x - rim, sb.y - rim, sb.w + 2 * rim, sb.h + 2 * rim, sb.r + rim, lw + 2 * rim); bb = ir_union(bb, layer_composite(C, L, rimc, 1.0f)); }
            float fa = idle_a + (1.0f - idle_a) * sb.lv;
            bb = ir_union(bb, draw_part(C, L, &sb, lw, s->keys_outline, idle_a > 0 ? mixc(s->keys_idle, on, sb.lv) : on, fa));
        }
        g_shx = 0; g_shc_on = 0;
    }
    if (s->keys_highlight)
        bb = ir_union(bb, draw_round_rect_outline(C, o->px - 6, o->py - 6, o->pw + 12, o->ph + 12, 8, maxf(2.0f * sc->scale, 1.5f), s->apm_label, 0.95f));
    if (s->keys_highlight && o->msep)
        bb = ir_union(bb, draw_round_rect_outline(C, o->mpx - 6, o->mpy - 6, o->mpw + 12, o->mph + 12, 8, maxf(2.0f * sc->scale, 1.5f), s->apm_label, 0.95f));
    free(o);
    return bb;
}

IRect render_scene(Canvas *C, Layer *L, const Style *s, const Scene *sc) {
    IRect bb = ir_empty();
    float k = sc->scale;
    float gs = clampf(s->glow_strength, 0, 1), gr = maxf(s->glow_radius, 1.0f) * k;

    float tol = (s->trail_outline && s->trail_outline_px > 0) ? s->trail_outline_px * k : 0.0f;
    if (s->trail_enabled && s->trail_len_s > 0.0f && sc->ntrail >= 2) {
        float len = s->trail_len_s, R = maxf(s->trail_width * 0.5f * k, 0.6f);
        int first = 0;
        while (first < sc->ntrail - 1 && sc->now - sc->trail[first + 1].t > len) first++;
        TP *p; int n = trail_points(s, sc, first, &p);
        if (n >= 2) {
            static Ramp tr, gl;
            int grad = s->trail_grad != 0;
            if (grad) ramp_make(&tr, s->trail, s->trail_tail);
            if (s->glow_trail && gs > 0) {
                const Ramp *gp = NULL; RGBf gc = s->glow_col;
                if (s->glow_mode == 0) { if (grad) gp = &tr; else gc = s->trail; }
                else if (s->glow_mode == 2) { ramp_make(&gl, s->glow_col, s->glow_col2); gp = &gl; }
                g_par_on = gp != NULL;
                trail_cov(L, s, p, n, R, 0, sc->now);
                g_par_on = 0;
                bb = ir_union(bb, layer_glow_ex(C, L, gc, gp, gs, gr, s->solid));
            }
            if (tol > 0) { trail_cov(L, s, p, n, R, tol, sc->now); bb = ir_union(bb, layer_composite(C, L, s->trail_outline_col, 1.0f)); }
            g_par_on = grad;
            trail_cov(L, s, p, n, R, 0, sc->now);
            g_par_on = 0;
            bb = ir_union(bb, grad ? layer_composite_ramp(C, L, &tr, 1.0f) : layer_composite(C, L, s->trail, 1.0f));
        }
    }
    if (s->trail_enabled && s->cursor_dot && sc->ntrail >= 1) {
        const TrailPt *h = &sc->trail[sc->ntrail - 1];
        float hx = h->x * k + sc->offx, hy = h->y * k + sc->offy, hr = maxf(s->trail_width * 0.6f * k, 1.0f);
        if (tol > 0) bb = ir_union(bb, draw_disc(C, hx, hy, hr + tol, s->trail_outline_col, 1.0f));
        bb = ir_union(bb, draw_disc(C, hx, hy, hr, s->trail, 1.0f));
    }

    if (s->click_fade_s > 0.0f) {
        for (int i = 0; i < sc->nclicks; i++) {
            const ClickEv *c = &sc->clicks[i];
            int btn = c->btn < 0 ? 0 : (c->btn > 3 ? 3 : c->btn);
            if (!s->click_show[btn]) continue;
            float age = (float)(sc->now - c->t);
            if (age < 0 || age >= s->click_fade_s) continue;
            float u = age / s->click_fade_s;
            RGBf col = btn == 0 ? s->lmb : (btn == 1 ? s->rmb : s->click_other);
            float x = c->x * k + sc->offx, y = c->y * k + sc->offy;
            float size = s->click_size * k;
            float grow = ease_out_cubic(age / 0.28f);
            float R = size * (0.35f + 0.65f * grow);
            float rw = maxf(clampf(s->click_ring_px, 0.5f, 20.0f) * k, 1.0f), dr = maxf(size * 0.24f, 1.5f);
            float ringA, dotA;
            if (s->solid) {
                ringA = dotA = 1.0f; rw *= 1.0f - u; dr *= 1.0f - u;
            } else {
                ringA = (1.0f - u * u) * 0.95f; dotA = powf(1.0f - u, 1.5f);
            }
            float minw = s->solid ? 1.2f : 0.35f;
            int ring = s->click_style != 2 && rw >= minw, dot = s->click_style != 1 && dr >= minw * 0.8f;
            if (!ring && !dot) continue;
            if (s->glow_clicks && gs > 0) {
                if (ring) ring_cov(L, x, y, R, rw, ringA);
                if (dot) layer_capsule(L, x, y, x, y, dr, dr, dotA, dotA);
                bb = ir_union(bb, layer_glow(C, L, s->glow_mode == 0 ? col : s->glow_col, gs, gr, s->solid));
            }
            float cot = (s->click_outline && s->click_outline_px > 0) ? s->click_outline_px * k : 0.0f;
            if (cot > 0) {
                if (ring) bb = ir_union(bb, draw_ring(C, x, y, R, rw + 2 * cot, s->click_outline_col, ringA));
                if (dot) bb = ir_union(bb, draw_disc(C, x, y, dr + cot, s->click_outline_col, dotA));
            }
            if (ring) bb = ir_union(bb, draw_ring(C, x, y, R, rw, col, ringA));
            if (dot) bb = ir_union(bb, draw_disc(C, x, y, dr, col, dotA));
        }
    }

    if (s->keys_kb || s->keys_mouse) bb = ir_union(bb, render_keys(C, L, s, sc));

    if (s->apm_enabled) {
        ApmLayout lay; apm_layout(s, sc, &lay);
        float u = lay.u;
        if (s->panel_enabled || s->apm_highlight) {
            float op = s->panel_enabled ? s->panel_opacity : 0.0f;
            if (s->apm_highlight && op < 0.55f) op = 0.55f;
            bb = ir_union(bb, draw_round_rect(C, lay.px, lay.py, lay.pw, lay.ph, clampf(s->panel_radius, 0, 60) * u, s->panel, op));
        }
        if (s->apm_highlight)
            bb = ir_union(bb, draw_round_rect_outline(C, lay.px, lay.py, lay.pw, lay.ph, clampf(s->panel_radius, 0, 60) * u, maxf(2.0f * u, 1.5f), s->apm_label, 0.95f));
        if (s->panel_enabled && s->panel_border) {
            RGBf bc = mixc(s->panel, lum(s->panel) > 0.5f ? (RGBf){ 0, 0, 0 } : (RGBf){ 1, 1, 1 }, 0.14f);
            float op = s->solid ? 1.0f : minf(1.0f, s->panel_opacity + 0.25f);
            bb = ir_union(bb, draw_round_rect_outline(C, lay.px, lay.py, lay.pw, lay.ph, clampf(s->panel_radius, 0, 60) * u, maxf(1.0f, roundf(u)), bc, op));
        }
        if (lay.bar > 0) {
            float pad = clampf(s->panel_pad, 0, 80) * u, bh = lay.ph - 2 * pad;
            layer_rr(L, lay.px + roundf(pad * 0.75f), lay.py + pad, lay.bar, bh, lay.bar * 0.5f, 0);
            bb = ir_union(bb, layer_composite(C, L, s->apm_label, 1.0f));
        }
        int nd = (int)strlen(lay.num);
        float x0 = lay.tx + (lay.cell > 0 ? lay.numW - 6.0f - nd * lay.cell : roundf(lay.numW - text_w(s, lay.num, 0, lay.numH, lay.wN))); /* right-aligned so the ones digit stays put */
        if (s->glow_apm && gs > 0) {
            if (lay.cell > 0) {
                for (int i = 0; i < nd; i++) { char d[2] = { lay.num[i], 0 }; TextMask m; text_mask(s, d, lay.numH, &m);
                    text_cov(L, s, d, roundf(x0 + i * lay.cell + (lay.cell - mask_adv(&m)) * 0.5f), lay.ty, lay.numH, lay.wN, 0); }
            } else text_cov(L, s, lay.num, x0, lay.ty, lay.numH, lay.wN, 0);
            bb = ir_union(bb, layer_glow(C, L, s->glow_mode == 0 ? s->apm_text : s->glow_col, gs, gr * u / maxf(k, 0.01f), s->solid));
        }
        if (lay.cell > 0) {
            for (int i = 0; i < nd; i++) {
                char d[2] = { lay.num[i], 0 }; TextMask m; text_mask(s, d, lay.numH, &m);
                bb = ir_union(bb, text_fx(C, L, s, k * res_k(s), d, roundf(x0 + i * lay.cell + (lay.cell - mask_adv(&m)) * 0.5f), lay.ty, lay.numH, lay.wN, s->apm_text, 1.0f));
            }
        } else bb = ir_union(bb, text_fx(C, L, s, k * res_k(s), lay.num, x0, lay.ty, lay.numH, lay.wN, s->apm_text, 1.0f));
        Style ls = label_style(s);
        if (s->label[0]) bb = ir_union(bb, text_fx_tr(C, L, &ls, k * res_k(s), s->label, lay.lx, lay.ly, lay.labH, lay.wL, lay.tr, s->apm_label, 1.0f));
        if (s->apm_show_avg) bb = ir_union(bb, text_fx_tr(C, L, &ls, k * res_k(s), lay.avg, lay.ax, lay.ay, lay.labH, lay.wL, lay.tr, mixc(s->apm_text, s->panel, 0.35f), 1.0f));
    }
    return bb;
}

IRect draw_line(Canvas *C, Layer *L, float x0, float y0, float x1, float y1, float width, RGBf col, float a) {
    layer_capsule(L, x0, y0, x1, y1, width * 0.5f, width * 0.5f, 1.0f, 1.0f);
    return layer_composite(C, L, col, a);
}

IRect render_align(Canvas *C, Layer *L, int x, int y, const char *const *lines, int n, float scale, int solid) {
    IRect bb = ir_empty();
    const RGBf ink = { 1.0f, 0.23f, 0.83f }, dark = { 0, 0, 0 };
    float cx = (float)x + 0.5f, cy = (float)y + 0.5f, arm = 46.0f * maxf(scale, 0.5f), gap = 5.0f;
    for (int pass = 0; pass < 2; pass++) {
        float w = pass ? 0.5f : 1.5f;
        layer_capsule(L, cx - arm, cy, cx - gap, cy, w, w, 1, 1);
        layer_capsule(L, cx + gap, cy, cx + arm, cy, w, w, 1, 1);
        layer_capsule(L, cx, cy - arm, cx, cy - gap, w, w, 1, 1);
        layer_capsule(L, cx, cy + gap, cx, cy + arm, w, w, 1, 1);
        bb = ir_union(bb, layer_composite(C, L, pass ? ink : dark, 1.0f));
    }
    /* exact hotspot pixel */
    IRect px = ir_clip((IRect){ x, y, x + 1, y + 1 }, C->w, C->h);
    if (!ir_is_empty(px)) { blend_px(C->px + (size_t)y * C->w + x, ink, 1.0f); bb = ir_union(bb, px); }
    if (n <= 0) return bb;
    Style ts; memset(&ts, 0, sizeof ts); ts.font = 3;
    float h = 9.0f * maxf(scale, 0.6f), lh = h * 1.9f, pad = h * 0.9f, tw = 0;
    for (int i = 0; i < n; i++) { float w = text_w(&ts, lines[i], 0, h, 0); if (w > tw) tw = w; }
    float pw = tw + pad * 2, ph = lh * n + pad * 2 - (lh - h * 1.4f);
    float bx = cx + 22, by = cy + 22;
    if (bx + pw > C->w) bx = cx - 22 - pw;
    if (by + ph > C->h) by = cy - 22 - ph;
    if (bx < 0) bx = 0;
    if (by < 0) by = 0;
    bb = ir_union(bb, draw_round_rect(C, bx, by, pw, ph, 6, (RGBf){ 0.04f, 0.05f, 0.07f }, solid ? 1.0f : 0.88f));
    for (int i = 0; i < n; i++) {
        text_cov(L, &ts, lines[i], floorf(bx + pad), floorf(by + pad + lh * i), h, 0, 0);
        bb = ir_union(bb, layer_composite(C, L, i == 0 ? ink : (RGBf){ 0.93f, 0.94f, 0.95f }, 1.0f));
    }
    return bb;
}

IRect render_marks(Canvas *C, Layer *L, float x, float y, float w, float h, float scale) {
    IRect bb = ir_empty();
    float arm = roundf(60.0f * maxf(scale, 0.5f)), t = maxf(roundf(4.0f * scale), 2.0f), e = maxf(roundf(2.0f * scale), 1.0f);
    float x0 = roundf(x), y0 = roundf(y), x1 = roundf(x + w), y1 = roundf(y + h), cx = roundf(x + w * 0.5f), cy = roundf(y + h * 0.5f);
    float r[10][4] = {
        { x0, y0, arm, t }, { x0, y0, t, arm }, { x1 - arm, y0, arm, t }, { x1 - t, y0, t, arm },
        { x0, y1 - t, arm, t }, { x0, y1 - arm, t, arm }, { x1 - arm, y1 - t, arm, t }, { x1 - t, y1 - arm, t, arm },
        { cx - arm * 0.5f, cy - t * 0.5f, arm, t }, { cx - t * 0.5f, cy - arm * 0.5f, t, arm } };
    for (int pass = 0; pass < 2; pass++) { /* dark edge, then white: readable on any background */
        for (int i = 0; i < 10; i++) layer_rr(L, r[i][0], r[i][1], r[i][2], r[i][3], 0, pass ? 0 : e);
        bb = ir_union(bb, layer_composite(C, L, pass ? (RGBf){ 1, 1, 1 } : (RGBf){ 0.02f, 0.02f, 0.03f }, 1.0f));
    }
    return bb;
}
