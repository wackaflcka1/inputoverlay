/* render.h - portable software renderer shared by the Windows app and the
 * preview tool. Draws into a premultiplied 32-bit BGRA buffer. No OS calls. */
#ifndef MAO_RENDER_H
#define MAO_RENDER_H
#include <stdint.h>

typedef struct { int x0, y0, x1, y1; } IRect;            /* x1/y1 exclusive */
typedef struct { float r, g, b; } RGBf;                    /* 0..1 */
typedef struct { uint32_t *px; int w, h; } Canvas;         /* premult BGRA, top-down */
typedef struct { float *cov; float *par; int w, h; IRect bb; } Layer;  /* coverage scratch (+ optional colour position 0..1) */

typedef struct { double t; float x, y; } TrailPt;
typedef struct { double t; float x, y; int btn; } ClickEv; /* btn 0=left 1=right 2=middle 3=side */
typedef struct { int w, h, base; const unsigned char *a; } TextMask;
typedef int (*TextRasterFn)(const char *s, float cap_h, int bold, int font, TextMask *out);

enum { APM_TL = 0, APM_TR, APM_BL, APM_BR };

typedef struct {
    RGBf trail, lmb, rmb, apm_text, apm_label, panel;
    float trail_len_s;    /* seconds of trail */
    float trail_width;    /* px at head */
    float click_fade_s;   /* seconds a click marker lives */
    float click_size;     /* px, ring radius when fully grown */
    int   trail_enabled;
    int   trail_smooth;   /* 0 Off .. 4 Very high */
    int   click_show[4];  /* left, right, middle, side */
    RGBf  click_other;    /* middle + side marker colour */
    int   glow_trail, glow_clicks, glow_apm;
    float glow_strength;  /* 0..1 */
    float glow_radius;    /* px */
    int   glow_mode;      /* 0 same as element, 1 one custom colour, 2 two custom colours (trail start -> end) */
    RGBf  glow_col, glow_col2;
    int   trail_grad;     /* 0 single colour, 1 fade from `trail` (cursor) to `trail_tail` */
    RGBf  trail_tail;
    char  label[16];      /* text after the number; empty hides it */
    int   font;           /* 0 built-in, >0 app-provided raster font */
    int   cursor_dot;     /* draw a small dot at the cursor head */
    float trail_taper;    /* 0..1: how much the tail narrows */
    int   click_style;    /* 0 ring + dot, 1 ring only, 2 dot only */
    float click_ring_px;  /* ring stroke width */
    int   click_outline; float click_outline_px; RGBf click_outline_col;
    float num_scale;      /* APM number size relative to the box */
    float panel_pad, panel_radius; /* px at scale 1 */
    int   trail_outline;  /* edge around the trail (and cursor dot) */
    float trail_outline_px;
    RGBf  trail_outline_col;
    int   solid;          /* chroma mode: fade by shrinking, never by alpha */
    int   apm_enabled;
    int   apm_corner;       /* anchor corner of the reference monitor */
    float apm_off_x, apm_off_y; /* px from the anchor corner, inward */
    float apm_scale;
    int   apm_highlight;    /* "move" mode: outline + solid panel */
    int   text_bold;        /* heavier strokes */
    int   text_outline;     /* outline around APM text */
    float outline_px;       /* outline thickness, px (0-4) */
    RGBf  outline_col;      /* outline + drop-shadow colour */
    int   text_shadow;      /* solid offset shadow (no blur) */
    int   panel_enabled;
    float panel_opacity;
    int   apm_show_avg;   /* small session-average line */
    float lab_scale;      /* label height relative to the number */
    int   lab_bold, lab_outline, lab_below;
    int   panel_border, panel_accent;  /* hairline edge, accent bar at the left */
    float apm_res;        /* screen-size factor for the box and keys (1 = as set) */
    /* on-screen keys + mouse */
    int   keys_kb, keys_mouse, keys_layout; /* layout 0 left side, 1 League, 2 custom, 3 WASD, 4 full */
    char  keys_custom[2048];                /* "Q W E R / D F", or placed keys "Q@1.5,1 SPACE@3,4:3" */
    RGBf  keys_outline, keys_on, keys_label, keys_idle;
    float keys_idle_op, keys_scale;
    int   keys_corner; float keys_off_x, keys_off_y;
    int   keys_glow;
    int   mouse_sep, mouse_corner; float mouse_off_x, mouse_off_y; /* mouse placed on its own instead of next to the keys */
    int   keys_highlight;                   /* move mode */
    int   keys_plate, keys_depth, keys_italic, keys_font;
    RGBf  keys_plate_col; float keys_plate_op, keys_line;
    int   keys_shape;                       /* 0 rounded keys, 1 slanted (leaning parallelogram keys and letters) */
} Style;

typedef struct {
    const TrailPt *trail; int ntrail;   /* oldest -> newest */
    const ClickEv *clicks; int nclicks;
    int apm;
    int apm_avg;                        /* session average, shown if apm_show_avg */
    double now;
    float scale, offx, offy;            /* screen px -> canvas px */
    float area_x, area_y, area_w, area_h; /* canvas rect used to place APM */
    const unsigned char *kdown;         /* 256 entries: virtual key held right now (display only) */
    int mdown[5];                       /* left, right, middle, side 1, side 2 held */
    int wheel;                          /* -1 down, +1 up, 0 none (recent scroll) */
    const float *klevel;                /* optional 0..1 per key (fades out on release) */
    float mlevel[5]; int mlevel_set;
} Scene;

IRect ir_empty(void);
int   ir_is_empty(IRect r);
IRect ir_union(IRect a, IRect b);

int   layer_init(Layer *L, int w, int h);
void  layer_free(Layer *L);

void  canvas_clear(Canvas *C, IRect r);
void  render_set_text_raster(TextRasterFn f);
/* thin anti-aliased line (used for window buttons) */
IRect draw_line(Canvas *C, Layer *L, float x0, float y0, float x1, float y1, float width, RGBf col, float a);
/* alignment check: 1px crosshair centred on pixel (x, y) and a small text readout next to it */
IRect render_align(Canvas *C, Layer *L, int x, int y, const char *const *lines, int n, float scale, int solid);
/* corner brackets and a centre cross over an area, to check a capture shows all of it */
IRect render_marks(Canvas *C, Layer *L, float x, float y, float w, float h, float scale);
void keys_panel_rect(const Style *s, const Scene *sc, float *x, float *y, float *w, float *h);
void mouse_panel_rect(const Style *s, const Scene *sc, float *x, float *y, float *w, float *h);
int  keys_vk(const char *name, float *width_units);
typedef struct { char name[8], label[8]; int vk; float x, y, w; } KeyItem;
int  keys_items(const Style *s, KeyItem *out, int cap);        /* the keys of the chosen layout, in key units */
int  keys_name_info(const char *name, float *w, char *label);  /* 0 if the name is not a key */
void keys_geom(const Style *s, const Scene *sc, float *x0, float *y0, float *unit); /* where key unit 0,0 is drawn */
#define KEYS_MAX 96
IRect render_scene(Canvas *C, Layer *L, const Style *s, const Scene *sc);
/* APM box rectangle in canvas px (clamped inside the area); same as drawn */
void  apm_panel_rect(const Style *s, const Scene *sc, float *x, float *y, float *w, float *h);

/* building blocks, also used for the tray icon */
IRect draw_ring(Canvas *C, float cx, float cy, float R, float w, RGBf col, float a);
IRect draw_disc(Canvas *C, float cx, float cy, float r, RGBf col, float a);
IRect draw_round_rect(Canvas *C, float x, float y, float w, float h, float rad, RGBf col, float a);
IRect draw_round_rect_outline(Canvas *C, float x, float y, float w, float h, float rad, float lw, RGBf col, float a);
#endif
