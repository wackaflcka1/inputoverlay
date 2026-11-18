/* demo.h - deterministic demo motion used by the settings live preview and the
 * preview image generator, so both show exactly the same thing. */
#ifndef MAO_DEMO_H
#define MAO_DEMO_H
#include "render.h"
int demo_trail(double now, float w, float h, float len_s, TrailPt *out, int cap);
int demo_clicks(double now, float w, float h, float fade_s, ClickEv *out, int cap);
#endif
