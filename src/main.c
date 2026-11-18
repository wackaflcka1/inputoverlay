/* 255 Input Overlay - mouse trail, click markers and an APM counter for
 * streaming. Win32 + plain C, single static exe.
 *
 * Safety / privacy design:
 *  - Never opens, reads or touches any other process (no OpenProcess,
 *    ReadProcessMemory, DLL injection, window hooks into other apps).
 *  - Input comes from standard Windows APIs only: the cursor position plus Raw Input
 *    (default, passive, RIDEV_INPUTSINK) or optional low-level hooks.
 *  - Keys are only COUNTED. The key code is used for one thing: a 256-entry
 *    "is this key currently held" table so auto-repeat is not counted. Nothing
 *    about which key was pressed is stored, logged, written or sent.
 *  - No network code at all. Settings live in an .ini next to the exe. */
#define WIN32_LEAN_AND_MEAN
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <dwmapi.h>
#define COBJMACROS
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mmsystem.h>
#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>
#include "render.h"
#include "demo.h"
#include "apm.h"

/* ---- DPI: every thread runs per-monitor v2, cursor read in physical pixels ---- */
typedef BOOL(WINAPI *PFN_GPCP)(LPPOINT);
typedef HANDLE(WINAPI *PFN_STDAC)(HANDLE);
typedef HANDLE(WINAPI *PFN_GTDAC)(void);
typedef int(WINAPI *PFN_GAFDAC)(HANDLE);
typedef BOOL(WINAPI *PFN_ADACE)(HANDLE, HANDLE);
typedef HRESULT(WINAPI *PFN_GDFM)(HMONITOR, int, UINT *, UINT *);
static PFN_GPCP p_GetPhysicalCursorPos;
static PFN_STDAC p_SetThreadDpiAwarenessContext;
static PFN_GTDAC p_GetThreadDpiAwarenessContext;
static PFN_GAFDAC p_GetAwarenessFromDpiAwarenessContext;
static PFN_ADACE p_AreDpiAwarenessContextsEqual;
static PFN_GDFM p_GetDpiForMonitor;
#define CTX_PMV2 ((HANDLE)(INT_PTR)-4)
static void dpi_load(void) {
    HMODULE u = GetModuleHandleW(L"user32.dll"), sh = LoadLibraryW(L"shcore.dll");
    if (u) {
        p_GetPhysicalCursorPos = (PFN_GPCP)(void *)GetProcAddress(u, "GetPhysicalCursorPos");
        p_SetThreadDpiAwarenessContext = (PFN_STDAC)(void *)GetProcAddress(u, "SetThreadDpiAwarenessContext");
        p_GetThreadDpiAwarenessContext = (PFN_GTDAC)(void *)GetProcAddress(u, "GetThreadDpiAwarenessContext");
        p_GetAwarenessFromDpiAwarenessContext = (PFN_GAFDAC)(void *)GetProcAddress(u, "GetAwarenessFromDpiAwarenessContext");
        p_AreDpiAwarenessContextsEqual = (PFN_ADACE)(void *)GetProcAddress(u, "AreDpiAwarenessContextsEqual");
    }
    if (sh) p_GetDpiForMonitor = (PFN_GDFM)(void *)GetProcAddress(sh, "GetDpiForMonitor");
}
static void thread_pmv2(void) { if (p_SetThreadDpiAwarenessContext) p_SetThreadDpiAwarenessContext(CTX_PMV2); }
/* physical cursor position; input events only say "something happened", the position always comes from here */
static void cursor_pos(POINT *p) {
    if (p_GetPhysicalCursorPos && p_GetPhysicalCursorPos(p)) return;
    GetCursorPos(p);
}
static const char *awareness_name(void) {
    if (!p_GetThreadDpiAwarenessContext) return "system (old Windows)";
    HANDLE c = p_GetThreadDpiAwarenessContext();
    if (p_AreDpiAwarenessContextsEqual && p_AreDpiAwarenessContextsEqual(c, CTX_PMV2)) return "per-monitor v2";
    int a = p_GetAwarenessFromDpiAwarenessContext ? p_GetAwarenessFromDpiAwarenessContext(c) : -1;
    return a == 2 ? "per-monitor" : a == 1 ? "system" : a == 0 ? "unaware" : "unknown";
}

#define APP_NAME L"255 Input Overlay"
#define WM_TRAY (WM_APP + 1)
#define WM_OPEN_SETTINGS (WM_APP + 2)
enum { TIMER_FRAME = 1, TIMER_SAVE, TIMER_WATCH, TIMER_STATUS, TIMER_GUIDE };
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
enum { HK_TOGGLE = 1 };

/* ------------------------------------------------------------------ settings */
typedef struct {
    Style st;
    int mode;              /* 0 overlay, 1 chroma window */
    RGBf chroma;
    int chroma_w, chroma_h, chroma_virtual, chroma_topmost;
    int chroma_marks; /* corner + centre marks in the green screen / stream window */
    int chroma_screen; /* green screen / stream window shows: 0 game screen, 1 screen 1 (main), 2 screen 2, 3 follow mouse, 4 whole desktop, 5 follow League client */
    int chroma_hard, chroma_fit; /* green screen: hard edges (Color Key) or smooth (Chroma Key); window as big as the screen */
    float apm_window_s;
    int count_keys, count_clicks, input_hook, fps;
    int max_draw;          /* redraws per second at most (0 = every screen refresh) */
    float idle_hide_s;     /* 0 = never */
    int align_check;       /* crosshair + numbers at the cursor */
    float fix_dx, fix_dy, fix_sx, fix_sy; /* manual position correction: screen * scale + offset */
    UINT hk_mods, hk_vk;   /* MOD_* and VK */
    int mouse_attach;      /* mouse sits next to the keyboard; off = it has its own spot */
    int scale_screen;      /* size the APM box and keys with the screen height (base 1080p) */
    int keys_bg, apm_bg;   /* 0 invisible, 1 dark plate/box, 2 custom colour */
    int stream_clear;      /* stream window: 1 see-through on the desktop, 0 black */
    int stream_guide;      /* dashed outline on this PC while Settings is open */
    int set_x, set_y, set_pos; /* last Settings window position, in screen pixels */
} Settings;

static Settings S;
static int g_dpi = 96, g_syncing, g_dpi_cur = 96;
static wchar_t g_ini[MAX_PATH];
static FILETIME g_ini_mtime;

/* one-click looks: colours, fonts and surfaces only; what is shown, where and how big stays as it is */
#define HX(v) ((RGBf){ (((v) >> 16) & 255) / 255.f, (((v) >> 8) & 255) / 255.f, ((v) & 255) / 255.f })
static void apply_preset(Settings *s, int p) {
    Style *t = &s->st;
    t->text_shadow = 0; t->keys_glow = 0; t->trail_grad = 0; t->glow_trail = t->glow_clicks = t->glow_apm = 0;
    t->click_style = 0; t->click_ring_px = 2.5f; t->keys_line = 2; t->outline_px = 2;
    switch (p) {
    default: /* Clean dark */
        t->trail = HX(0x5CE1FF); t->lmb = HX(0xFFB547); t->rmb = HX(0xFF5C8A); t->click_other = HX(0xB38CFF);
        t->trail_outline = 0; t->click_outline = 0;
        t->font = 2; t->text_bold = 1; t->apm_text = HX(0xF4F6F8); t->apm_label = HX(0x8E99A8);
        t->lab_bold = 1; t->lab_outline = 0; t->text_outline = 0; t->outline_col = HX(0x05070A);
        t->panel_enabled = 1; t->panel = HX(0x0D1015); t->panel_opacity = 0.84f; t->panel_border = 1; t->panel_accent = 0; t->panel_radius = 10; t->panel_pad = 14;
        t->keys_outline = HX(0x2C323C); t->keys_idle = HX(0x161A20); t->keys_idle_op = 0.92f; t->keys_on = HX(0x5CE1FF); t->keys_label = HX(0xD5DAE1);
        t->keys_plate = 1; t->keys_plate_col = HX(0x0A0C10); t->keys_plate_op = 0.78f; t->keys_depth = 1; t->keys_italic = 0; t->keys_font = 2;
        s->keys_bg = 1; s->apm_bg = 1;
        break;
    case 1: /* Outline */
        t->trail = HX(0xFFFFFF); t->lmb = HX(0xFFFFFF); t->rmb = HX(0xFF5C8A); t->click_other = HX(0xB38CFF);
        t->trail_outline = 1; t->trail_outline_px = 1.5f; t->trail_outline_col = HX(0x05070A);
        t->click_outline = 1; t->click_outline_px = 1.5f; t->click_outline_col = HX(0x05070A);
        t->font = 2; t->text_bold = 1; t->apm_text = HX(0xFFFFFF); t->apm_label = HX(0xFFFFFF);
        t->lab_bold = 1; t->lab_outline = 1; t->text_outline = 1; t->outline_col = HX(0x05070A);
        t->panel_enabled = 0; t->panel_border = 0; t->panel_accent = 0;
        t->keys_outline = HX(0xFFFFFF); t->keys_idle = HX(0x0D1015); t->keys_idle_op = 0.0f; t->keys_on = HX(0xFFFFFF); t->keys_label = HX(0xFFFFFF);
        t->keys_plate = 0; t->keys_plate_col = HX(0x0A0C10); t->keys_plate_op = 0.7f; t->keys_depth = 0; t->keys_italic = 0; t->keys_font = 2; t->keys_line = 2.5f;
        s->keys_bg = 0; s->apm_bg = 0;
        break;
    case 2: /* Broadcast */
        t->trail = HX(0xF2B53A); t->trail_tail = HX(0x47618F); t->trail_grad = 1; t->lmb = HX(0xF2B53A); t->rmb = HX(0x4FA3FF); t->click_other = HX(0xE8EDF5);
        t->trail_outline = 0; t->click_outline = 0;
        t->font = 2; t->text_bold = 1; t->apm_text = HX(0xFFFFFF); t->apm_label = HX(0xF2B53A);
        t->lab_bold = 1; t->lab_outline = 0; t->text_outline = 0; t->outline_col = HX(0x05070A);
        t->panel_enabled = 1; t->panel = HX(0x0B1322); t->panel_opacity = 0.9f; t->panel_border = 1; t->panel_accent = 1; t->panel_radius = 6; t->panel_pad = 14;
        t->keys_outline = HX(0x2A3A57); t->keys_idle = HX(0x111B2E); t->keys_idle_op = 0.94f; t->keys_on = HX(0xF2B53A); t->keys_label = HX(0xE8EDF5);
        t->keys_plate = 1; t->keys_plate_col = HX(0x070C17); t->keys_plate_op = 0.86f; t->keys_depth = 1; t->keys_italic = 1; t->keys_font = 2;
        s->keys_bg = 2; s->apm_bg = 2;
        break;
    case 3: /* Light */
        t->trail = HX(0xFFFFFF); t->lmb = HX(0xFFFFFF); t->rmb = HX(0xFF5C8A); t->click_other = HX(0xC9CED6);
        t->trail_outline = 0; t->click_outline = 0;
        t->font = 2; t->text_bold = 1; t->apm_text = HX(0x111418); t->apm_label = HX(0x6B7380);
        t->lab_bold = 1; t->lab_outline = 0; t->text_outline = 0; t->outline_col = HX(0xFFFFFF);
        t->panel_enabled = 1; t->panel = HX(0xF3F4F6); t->panel_opacity = 0.94f; t->panel_border = 1; t->panel_accent = 0; t->panel_radius = 10; t->panel_pad = 14;
        t->keys_outline = HX(0xCDD1D8); t->keys_idle = HX(0xF6F7F9); t->keys_idle_op = 0.95f; t->keys_on = HX(0x111418); t->keys_label = HX(0x1A1E24);
        t->keys_plate = 0; t->keys_plate_col = HX(0xE6E8EC); t->keys_plate_op = 0.9f; t->keys_depth = 1; t->keys_italic = 0; t->keys_font = 2;
        s->keys_bg = 0; s->apm_bg = 2;
        break;
    case 4: /* Slanted: black keys, thin white outline, big leaning letters, pressed keys turn white */
        t->trail = HX(0xFFFFFF); t->lmb = HX(0xFFFFFF); t->rmb = HX(0xFF5C8A); t->click_other = HX(0xB38CFF);
        t->trail_outline = 1; t->trail_outline_px = 1.5f; t->trail_outline_col = HX(0x000000);
        t->click_outline = 1; t->click_outline_px = 1.5f; t->click_outline_col = HX(0x000000);
        t->font = 2; t->text_bold = 1; t->apm_text = HX(0xFFFFFF); t->apm_label = HX(0xFFFFFF);
        t->lab_bold = 1; t->lab_outline = 0; t->text_outline = 0; t->outline_col = HX(0x000000);
        t->panel_enabled = 1; t->panel = HX(0x000000); t->panel_opacity = 0.92f; t->panel_border = 1; t->panel_accent = 0; t->panel_radius = 6; t->panel_pad = 14;
        t->keys_outline = HX(0xFFFFFF); t->keys_idle = HX(0x000000); t->keys_idle_op = 1.0f; t->keys_on = HX(0xFFFFFF); t->keys_label = HX(0xFFFFFF);
        t->keys_plate = 0; t->keys_plate_col = HX(0x000000); t->keys_plate_op = 0.8f; t->keys_depth = 0; t->keys_italic = 0; t->keys_font = 2; t->keys_line = 1.5f;
        t->keys_shape = 1;
        s->keys_bg = 0; s->apm_bg = 2;
        break;
    }
    if (p != 4) t->keys_shape = 0;
    t->glow_col = t->trail; t->glow_col2 = t->trail_grad ? t->trail_tail : HX(0xFF5C8A);
}

static void defaults(Settings *s) {
    memset(s, 0, sizeof(*s));
    s->st.trail = (RGBf){ 0x5C / 255.f, 0xE1 / 255.f, 1.f };
    s->st.lmb = (RGBf){ 1.f, 0xB5 / 255.f, 0x47 / 255.f };
    s->st.rmb = (RGBf){ 1.f, 0x5C / 255.f, 0x8A / 255.f };
    s->st.apm_text = (RGBf){ 1, 1, 1 };
    s->st.apm_label = s->st.trail;
    s->st.panel = (RGBf){ 0x0B / 255.f, 0x0F / 255.f, 0x17 / 255.f };
    s->st.trail_len_s = 0.6f;
    s->st.trail_width = 7.f;
    s->st.click_fade_s = 1.0f;
    s->st.click_size = 16.f;
    s->st.cursor_dot = 0;
    s->st.apm_enabled = 1;
    s->st.apm_corner = APM_BR;
    s->st.apm_off_x = 40; s->st.apm_off_y = 40;
    s->st.trail_taper = 0.65f;
    s->st.trail_outline = 0; s->st.trail_outline_px = 1.5f; s->st.trail_outline_col = (RGBf){ 0, 0, 0 };
    s->st.click_style = 0; s->st.click_ring_px = 2.5f;
    s->st.click_outline = 0; s->st.click_outline_px = 1.5f; s->st.click_outline_col = (RGBf){ 0, 0, 0 };
    s->st.num_scale = 1.0f; s->st.panel_pad = 14.f; s->st.panel_radius = 10.f;
    s->st.text_bold = 0; s->st.text_outline = 0; s->st.outline_px = 2.f;
    s->st.outline_col = (RGBf){ 0, 0, 0 }; s->st.text_shadow = 0;
    s->st.apm_scale = 1.0f;
    s->st.panel_enabled = 1;
    s->st.panel_opacity = 0.6f;
    s->mode = 0;
    s->chroma = (RGBf){ 0, 1, 0 };
    s->chroma_w = 1280; s->chroma_h = 720; s->chroma_hard = 0; s->chroma_fit = 1;
    s->apm_window_s = 30.f;
    s->st.apm_show_avg = 0;
    s->count_keys = 1; s->count_clicks = 1;
    s->fps = 0; s->max_draw = 60;
    s->hk_mods = MOD_CONTROL | MOD_SHIFT; s->hk_vk = VK_F10;
    s->st.trail_enabled = 1;
    for (int i = 0; i < 4; i++) s->st.click_show[i] = 1;
    s->st.click_other = (RGBf){ 0xB3 / 255.f, 0x8C / 255.f, 1.f };
    s->st.lab_scale = 0.45f; s->st.lab_bold = 1; s->st.lab_outline = 1; s->st.lab_below = 0; s->scale_screen = 1;
    s->mouse_attach = 1; s->st.mouse_corner = APM_BR; s->st.mouse_off_x = 40; s->st.mouse_off_y = 40;
    s->st.keys_kb = 0; s->st.keys_mouse = 0; s->st.keys_layout = 0; strcpy(s->st.keys_custom, "Q W E R / D F");
    s->st.keys_outline = (RGBf){ 1, 1, 1 }; s->st.keys_on = s->st.trail; s->st.keys_label = (RGBf){ 1, 1, 1 };
    s->st.keys_idle = (RGBf){ 0x0B / 255.f, 0x0F / 255.f, 0x17 / 255.f }; s->st.keys_idle_op = 0.35f; s->st.keys_scale = 1.0f;
    s->st.keys_corner = APM_BL; s->st.keys_off_x = 40; s->st.keys_off_y = 40; s->st.keys_glow = 0;
    s->st.trail_smooth = 3;
    s->st.glow_strength = 0.6f; s->st.glow_radius = 10.f; s->st.glow_mode = 0; s->st.glow_col = s->st.trail; s->st.glow_col2 = (RGBf){ 1.f, 0x5C / 255.f, 0x8A / 255.f };
    s->st.trail_grad = 0; s->st.trail_tail = (RGBf){ 0, 0, 0 };
    strcpy(s->st.label, "APM");
    s->st.font = 0;
    s->idle_hide_s = 0;
    s->align_check = 0; s->fix_dx = s->fix_dy = 0; s->fix_sx = s->fix_sy = 1.0f;
    s->keys_bg = 1; s->apm_bg = 1; s->stream_clear = 1; s->stream_guide = 1;
    s->set_x = 0; s->set_y = 0; s->set_pos = 0;
    apply_preset(s, 0);
}

static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static COLORREF to_cr(RGBf c) { return RGB((int)(c.r * 255 + .5f), (int)(c.g * 255 + .5f), (int)(c.b * 255 + .5f)); }
static RGBf from_cr(COLORREF c) { return (RGBf){ GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f }; }

static const wchar_t *SEC = L"overlay";
static void ini_str(const wchar_t *k, const wchar_t *def, wchar_t *buf, int n) { GetPrivateProfileStringW(SEC, k, def, buf, n, g_ini); }
static float ini_f(const wchar_t *k, float def) {
    wchar_t b[64]; ini_str(k, L"", b, 64);
    if (!b[0]) return def;
    wchar_t *e; double v = wcstod(b, &e);
    return (e == b || !isfinite(v) || fabs(v) > 1e6) ? def : (float)v;
}
static int ini_i(const wchar_t *k, int def) { return (int)GetPrivateProfileIntW(SEC, k, def, g_ini); }
static RGBf ini_col(const wchar_t *k, RGBf def) {
    wchar_t b[32]; ini_str(k, L"", b, 32);
    wchar_t *p = b; if (*p == L'#') p++;
    if (wcslen(p) != 6) return def;
    wchar_t *e; unsigned long v = wcstoul(p, &e, 16);
    if (*e) return def;
    return (RGBf){ ((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f };
}
static void put_s(const wchar_t *k, const wchar_t *v) { WritePrivateProfileStringW(SEC, k, v, g_ini); }
static void put_f(const wchar_t *k, float v) { wchar_t b[64]; swprintf(b, 64, L"%g", (double)v); put_s(k, b); }
static void put_i(const wchar_t *k, int v) { wchar_t b[32]; swprintf(b, 32, L"%d", v); put_s(k, b); }
static void put_col(const wchar_t *k, RGBf c) { COLORREF r = to_cr(c); wchar_t b[16]; swprintf(b, 16, L"#%02X%02X%02X", GetRValue(r), GetGValue(r), GetBValue(r)); put_s(k, b); }

/* hotkey text <-> MOD_/VK */
static void vk_name(UINT vk, wchar_t *b, int n) {
    if (vk >= VK_F1 && vk <= VK_F24) swprintf(b, n, L"F%u", vk - VK_F1 + 1);
    else if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) swprintf(b, n, L"%c", (wchar_t)vk);
    else {
        const wchar_t *s = NULL;
        switch (vk) {
        case VK_INSERT: s = L"Insert"; break; case VK_DELETE: s = L"Delete"; break;
        case VK_HOME: s = L"Home"; break; case VK_END: s = L"End"; break;
        case VK_PRIOR: s = L"PageUp"; break; case VK_NEXT: s = L"PageDown"; break;
        case VK_PAUSE: s = L"Pause"; break; case VK_SCROLL: s = L"ScrollLock"; break;
        case VK_SPACE: s = L"Space"; break;
        }
        if (s) swprintf(b, n, L"%ls", s); else swprintf(b, n, L"0x%02X", vk);
    }
}
static void hotkey_text(UINT mods, UINT vk, wchar_t *b, int n) {
    wchar_t k[32]; vk_name(vk, k, 32);
    swprintf(b, n, L"%ls%ls%ls%ls%ls", (mods & MOD_CONTROL) ? L"Ctrl+" : L"", (mods & MOD_SHIFT) ? L"Shift+" : L"",
             (mods & MOD_ALT) ? L"Alt+" : L"", (mods & MOD_WIN) ? L"Win+" : L"", k);
}
static int parse_hotkey(const wchar_t *str, UINT *mods, UINT *vk) {
    wchar_t b[128]; wcsncpy(b, str, 127); b[127] = 0;
    UINT m = 0, v = 0; wchar_t *ctx = NULL;
    for (wchar_t *t = wcstok(b, L"+ ", &ctx); t; t = wcstok(NULL, L"+ ", &ctx)) {
        if (!_wcsicmp(t, L"ctrl") || !_wcsicmp(t, L"control")) m |= MOD_CONTROL;
        else if (!_wcsicmp(t, L"shift")) m |= MOD_SHIFT;
        else if (!_wcsicmp(t, L"alt")) m |= MOD_ALT;
        else if (!_wcsicmp(t, L"win")) m |= MOD_WIN;
        else if ((t[0] == L'F' || t[0] == L'f') && t[1] >= L'1' && t[1] <= L'9') { int f = _wtoi(t + 1); if (f >= 1 && f <= 24) v = VK_F1 + f - 1; }
        else if (!t[1] && ((t[0] >= L'A' && t[0] <= L'Z') || (t[0] >= L'0' && t[0] <= L'9'))) v = t[0];
        else if (!t[1] && t[0] >= L'a' && t[0] <= L'z') v = t[0] - 32;
        else if (t[0] == L'0' && (t[1] == L'x' || t[1] == L'X')) v = (UINT)wcstoul(t, NULL, 16);
        else {
            static const struct { const wchar_t *n; UINT vk; } names[] = {
                { L"Insert", VK_INSERT }, { L"Delete", VK_DELETE }, { L"Home", VK_HOME }, { L"End", VK_END },
                { L"PageUp", VK_PRIOR }, { L"PageDown", VK_NEXT }, { L"Pause", VK_PAUSE }, { L"ScrollLock", VK_SCROLL }, { L"Space", VK_SPACE } };
            for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) if (!_wcsicmp(t, names[i].n)) v = names[i].vk;
        }
    }
    if (!v) return 0;
    *mods = m; *vk = v; return 1;
}

static const wchar_t *corner_names[] = { L"top-left", L"top-right", L"bottom-left", L"bottom-right" };

static void load_settings(void) {
    Settings d; defaults(&d);
    Settings s = d;
    wchar_t b[128];
    ini_str(L"mode", L"overlay", b, 128); s.mode = !_wcsicmp(b, L"chroma") ? 1 : !_wcsicmp(b, L"stream") ? 2 : 0;
    s.st.trail = ini_col(L"trail_color", d.st.trail);
    s.st.lmb = ini_col(L"left_click_color", d.st.lmb);
    s.st.rmb = ini_col(L"right_click_color", d.st.rmb);
    s.st.apm_text = ini_col(L"apm_color", d.st.apm_text);
    s.st.apm_label = ini_col(L"apm_label_color", d.st.apm_label);
    s.st.panel = ini_col(L"apm_background_color", d.st.panel);
    s.chroma = ini_col(L"chroma_color", d.chroma);
    s.st.trail_len_s = clampf(ini_i(L"trail_length_ms", 600) / 1000.f, 0.f, 5.f);
    s.st.trail_width = clampf(ini_f(L"trail_width", d.st.trail_width), 1.f, 60.f);
    s.st.cursor_dot = ini_i(L"cursor_dot", 0) != 0;
    s.st.click_fade_s = clampf(ini_i(L"click_fade_ms", 1000) / 1000.f, 0.f, 10.f);
    s.st.click_size = clampf(ini_f(L"click_size", d.st.click_size), 2.f, 120.f);
    s.st.apm_enabled = ini_i(L"apm_enabled", 1) != 0;
    ini_str(L"apm_position", L"bottom-right", b, 128);
    for (int i = 0; i < 4; i++) if (!_wcsicmp(b, corner_names[i])) s.st.apm_corner = i;
    {   /* offsets from the anchor corner; migrate v1.x apm_margin / custom apm_x,apm_y */
        wchar_t t[32]; ini_str(L"apm_offset_x", L"", t, 32);
        if (t[0]) {
            s.st.apm_off_x = ini_f(L"apm_offset_x", 40.f); s.st.apm_off_y = ini_f(L"apm_offset_y", 40.f);
        } else if (!_wcsicmp(b, L"custom")) {
            s.st.apm_corner = APM_TL; s.st.apm_off_x = ini_f(L"apm_x", 40.f); s.st.apm_off_y = ini_f(L"apm_y", 40.f);
        } else {
            s.st.apm_off_x = s.st.apm_off_y = ini_f(L"apm_margin", 40.f);
        }
        s.st.apm_off_x = clampf(s.st.apm_off_x, 0.f, 8000.f); s.st.apm_off_y = clampf(s.st.apm_off_y, 0.f, 8000.f);
    }
    s.st.trail_taper = clampf(ini_i(L"trail_taper_pct", 65) / 100.f, 0.f, 1.f);
    s.st.trail_enabled = ini_i(L"trail_enabled", 1) != 0;
    s.st.trail_smooth = ini_i(L"trail_smoothing", d.st.trail_smooth); if (s.st.trail_smooth < 0) s.st.trail_smooth = 0; if (s.st.trail_smooth > 4) s.st.trail_smooth = 4;
    s.st.click_show[0] = ini_i(L"click_left", 1) != 0;
    s.st.click_show[1] = ini_i(L"click_right", 1) != 0;
    s.st.click_show[2] = ini_i(L"click_middle", 1) != 0;
    s.st.click_show[3] = ini_i(L"click_side", 1) != 0;
    s.st.click_other = ini_col(L"other_click_color", d.st.click_other);
    s.st.glow_trail = ini_i(L"glow_trail", 0) != 0;
    s.st.glow_clicks = ini_i(L"glow_clicks", 0) != 0;
    s.st.glow_apm = ini_i(L"glow_apm", 0) != 0;
    s.st.glow_strength = clampf(ini_i(L"glow_strength_pct", 60) / 100.f, 0.f, 1.f);
    s.st.glow_radius = clampf(ini_f(L"glow_size", d.st.glow_radius), 2.f, 40.f);
    s.st.glow_mode = ini_i(L"glow_color_mode", ini_i(L"glow_match_color", 1) ? 0 : 1);
    if (s.st.glow_mode < 0 || s.st.glow_mode > 2) s.st.glow_mode = 0;
    s.st.glow_col = ini_col(L"glow_color", d.st.glow_col);
    s.st.glow_col2 = ini_col(L"glow_color_end", d.st.glow_col2);
    ini_str(L"trail_color_style", L"single", b, 128); s.st.trail_grad = !_wcsicmp(b, L"two");
    s.st.trail_tail = ini_col(L"trail_tail_color", d.st.trail_tail);
    {
        wchar_t lb[32]; ini_str(L"apm_label_text", L"\x01", lb, 32);
        if (lb[0] == 1 && !lb[1]) strcpy(s.st.label, "APM");
        else { int n = 0; for (int i = 0; lb[i] && n < 10; i++) if (lb[i] >= 32 && lb[i] < 127) s.st.label[n++] = (char)lb[i]; s.st.label[n] = 0; }
    }
    s.st.font = ini_i(L"apm_font", d.st.font); if (s.st.font < 0 || s.st.font > 5) s.st.font = 0;
    s.st.lab_scale = clampf(ini_i(L"apm_label_size_pct", 45) / 100.f, 0.2f, 1.f);
    s.st.lab_bold = ini_i(L"apm_label_bold", d.st.lab_bold) != 0;
    s.st.lab_outline = ini_i(L"apm_label_outline", d.st.lab_outline) != 0;
    ini_str(L"apm_label_position", L"right", b, 128); s.st.lab_below = !_wcsicmp(b, L"under");
    s.scale_screen = ini_i(L"scale_with_screen", 1) != 0;
    s.mouse_attach = ini_i(L"mouse_next_to_keys", 1) != 0;
    ini_str(L"mouse_position", L"bottom-right", b, 128);
    for (int i = 0; i < 4; i++) if (!_wcsicmp(b, corner_names[i])) s.st.mouse_corner = i;
    s.st.mouse_off_x = clampf(ini_f(L"mouse_offset_x", 40.f), 0.f, 8000.f); s.st.mouse_off_y = clampf(ini_f(L"mouse_offset_y", 40.f), 0.f, 8000.f);
    s.st.keys_kb = ini_i(L"keys_show_keyboard", 0) != 0;
    s.st.keys_mouse = ini_i(L"keys_show_mouse", 0) != 0;
    ini_str(L"keys_layout", L"league", b, 128); /* "league" was the left-side block before v1.9.1 */
    s.st.keys_layout = (!_wcsicmp(b, L"compact") || !_wcsicmp(b, L"league-keys")) ? 1 : !_wcsicmp(b, L"custom") ? 2 : !_wcsicmp(b, L"wasd") ? 3 : !_wcsicmp(b, L"full") ? 4 : 0;
    ini_str(L"keys_shape", d.st.keys_shape ? L"slanted" : L"rounded", b, 128); s.st.keys_shape = !_wcsicmp(b, L"slanted");
    {
        static wchar_t kc[2048]; ini_str(L"keys_custom", L"\x01", kc, 2048);
        if (!(kc[0] == 1 && !kc[1])) { int n = 0; for (int i = 0; kc[i] && n < 2040; i++) if (kc[i] >= 32 && kc[i] < 127) s.st.keys_custom[n++] = (char)kc[i]; s.st.keys_custom[n] = 0; }
    }
    s.st.keys_outline = ini_col(L"keys_outline_color", d.st.keys_outline);
    s.st.keys_on = ini_col(L"keys_pressed_color", d.st.keys_on);
    s.st.keys_label = ini_col(L"keys_label_color", d.st.keys_label);
    s.st.keys_idle = ini_col(L"keys_idle_color", d.st.keys_idle);
    s.st.keys_idle_op = clampf(ini_i(L"keys_idle_opacity_pct", (int)lroundf(d.st.keys_idle_op * 100)) / 100.f, 0.f, 1.f);
    s.st.keys_scale = clampf(ini_i(L"keys_size_pct", 100) / 100.f, 0.5f, 2.5f);
    ini_str(L"keys_position", L"bottom-left", b, 128);
    for (int i = 0; i < 4; i++) if (!_wcsicmp(b, corner_names[i])) s.st.keys_corner = i;
    s.st.keys_off_x = clampf(ini_f(L"keys_offset_x", 40.f), 0.f, 8000.f); s.st.keys_off_y = clampf(ini_f(L"keys_offset_y", 40.f), 0.f, 8000.f);
    s.st.keys_glow = ini_i(L"keys_glow", 0) != 0;
    s.st.keys_plate = ini_i(L"keys_backplate", d.st.keys_plate) != 0;
    s.st.keys_plate_col = ini_col(L"keys_backplate_color", d.st.keys_plate_col);
    s.st.keys_plate_op = clampf(ini_i(L"keys_backplate_opacity_pct", (int)lroundf(d.st.keys_plate_op * 100)) / 100.f, 0.f, 1.f);
    {
        wchar_t kb[32]; ini_str(L"keys_background", L"", kb, 32);
        if (kb[0]) { int v = ini_i(L"keys_background", 1); if (v >= 0 && v <= 2) s.keys_bg = v; }
        else {
            wchar_t oldv[32]; ini_str(L"keys_backplate", L"", oldv, 32);
            if (oldv[0]) s.keys_bg = s.st.keys_plate ? 2 : 0;
        }
    }
    s.st.keys_depth = ini_i(L"keys_3d", d.st.keys_depth) != 0;
    s.st.keys_italic = ini_i(L"keys_italic", d.st.keys_italic) != 0;
    s.st.keys_font = ini_i(L"keys_font", d.st.keys_font); if (s.st.keys_font < 1 || s.st.keys_font > 5) s.st.keys_font = 2;
    s.st.keys_line = clampf(ini_f(L"keys_outline_px", d.st.keys_line), 1.f, 6.f);
    s.st.panel_border = ini_i(L"apm_border", d.st.panel_border) != 0;
    s.st.panel_accent = ini_i(L"apm_accent_bar", d.st.panel_accent) != 0;
    s.align_check = ini_i(L"alignment_check", 0) != 0;
    s.fix_dx = clampf(ini_f(L"nudge_x_px", 0), -400, 400); s.fix_dy = clampf(ini_f(L"nudge_y_px", 0), -400, 400);
    s.fix_sx = clampf(ini_f(L"stretch_x_pct", 100) / 100.f, 0.5f, 1.5f); s.fix_sy = clampf(ini_f(L"stretch_y_pct", 100) / 100.f, 0.5f, 1.5f);
    s.idle_hide_s = ini_i(L"hide_when_idle_s", 0); if (s.idle_hide_s < 0) s.idle_hide_s = 0; if (s.idle_hide_s > 120) s.idle_hide_s = 120;
    s.st.trail_outline = ini_i(L"trail_outline", d.st.trail_outline) != 0;
    s.st.trail_outline_px = clampf(ini_f(L"trail_outline_px", d.st.trail_outline_px), 0.5f, 4.f);
    s.st.trail_outline_col = ini_col(L"trail_outline_color", d.st.trail_outline_col);
    ini_str(L"click_style", L"ring+dot", b, 128); s.st.click_style = !_wcsicmp(b, L"ring") ? 1 : (!_wcsicmp(b, L"dot") ? 2 : 0);
    s.st.click_ring_px = clampf(ini_f(L"click_ring_px", d.st.click_ring_px), 0.5f, 10.f);
    s.st.click_outline = ini_i(L"click_outline", d.st.click_outline) != 0;
    s.st.click_outline_px = clampf(ini_f(L"click_outline_px", d.st.click_outline_px), 0.5f, 4.f);
    s.st.click_outline_col = ini_col(L"click_outline_color", d.st.click_outline_col);
    s.st.num_scale = clampf(ini_i(L"apm_number_size_pct", 100) / 100.f, 0.5f, 2.5f);
    s.st.panel_pad = clampf(ini_f(L"apm_padding", d.st.panel_pad), 0.f, 40.f);
    s.st.panel_radius = clampf(ini_f(L"apm_corner_radius", d.st.panel_radius), 0.f, 30.f);
    s.st.text_bold = ini_i(L"apm_bold", d.st.text_bold) != 0;
    s.st.text_outline = ini_i(L"apm_outline", d.st.text_outline) != 0;
    s.st.outline_px = clampf(ini_f(L"apm_outline_px", 2.f), 0.5f, 4.f);
    s.st.outline_col = ini_col(L"apm_outline_color", d.st.outline_col);
    s.st.text_shadow = ini_i(L"apm_shadow", d.st.text_shadow) != 0;
    s.st.apm_scale = clampf(ini_f(L"apm_scale", d.st.apm_scale), 0.3f, 6.f);
    s.st.panel_enabled = ini_i(L"apm_background", d.st.panel_enabled) != 0;
    s.st.panel_opacity = clampf(ini_f(L"apm_background_opacity", d.st.panel_opacity), 0.f, 1.f);
    {
        wchar_t ab[32]; ini_str(L"apm_box_background", L"", ab, 32);
        if (ab[0]) { int v = ini_i(L"apm_box_background", 1); if (v >= 0 && v <= 2) s.apm_bg = v; }
        else {
            wchar_t oldv[32]; ini_str(L"apm_background", L"", oldv, 32);
            if (oldv[0]) s.apm_bg = s.st.panel_enabled ? 2 : 0;
        }
    }
    s.apm_window_s = clampf(ini_f(L"apm_window_s", 30.f), 1.f, 120.f); /* v1 key apm_window_seconds is ignored */
    if (ini_i(L"apm_version", 1) < 2 && fabsf(s.apm_window_s - 5.f) < 0.01f) s.apm_window_s = 30.f; /* v1.1 default -> new default */
    s.st.apm_show_avg = ini_i(L"apm_show_average", 0) != 0;
    s.count_keys = ini_i(L"count_keys", 1) != 0;
    s.count_clicks = ini_i(L"count_clicks", 1) != 0;
    ini_str(L"input", L"rawinput", b, 128); s.input_hook = !_wcsicmp(b, L"hook");
    s.fps = ini_i(L"fps", 0); if (s.fps < 0) s.fps = 0; if (s.fps > 500) s.fps = 500;
    s.max_draw = ini_i(L"max_redraw_fps", 60); if (s.max_draw < 0) s.max_draw = 0; if (s.max_draw > 500) s.max_draw = 500;
    s.chroma_w = ini_i(L"chroma_width", 1280); s.chroma_h = ini_i(L"chroma_height", 720);
    if (s.chroma_w < 160) s.chroma_w = 160; if (s.chroma_h < 90) s.chroma_h = 90;
    if (s.chroma_w > 7680) s.chroma_w = 7680; if (s.chroma_h > 4320) s.chroma_h = 4320;
    ini_str(L"chroma_source", L"primary", b, 128); s.chroma_virtual = !_wcsicmp(b, L"virtual");
    { wchar_t g[32]; ini_str(L"green_screen_shows", L"", g, 32); /* new key: older versions always saved "primary" */
      s.chroma_screen = s.chroma_virtual ? 4 : !_wcsicmp(g, L"screen1") ? 1 : !_wcsicmp(g, L"screen2") ? 2 : !_wcsicmp(g, L"mouse") ? 3 : !_wcsicmp(g, L"client") ? 5 : 0; }
    s.chroma_topmost = ini_i(L"chroma_topmost", 0) != 0;
    s.stream_clear = ini_i(L"stream_window_clear", d.stream_clear) != 0;
    s.stream_guide = ini_i(L"stream_window_guide", d.stream_guide) != 0;
    s.set_x = ini_i(L"settings_x", 0);
    s.set_y = ini_i(L"settings_y", 0);
    s.set_pos = ini_i(L"settings_pos", 0) != 0;
    s.chroma_marks = ini_i(L"green_screen_corner_marks", 0) != 0;
    ini_str(L"green_screen_edges", L"smooth", b, 128); s.chroma_hard = !_wcsicmp(b, L"hard");
    /* new key: older versions could switch to custom by themselves when the window was dragged */
    ini_str(L"green_screen_window", L"screen", b, 128); s.chroma_fit = _wcsicmp(b, L"custom") != 0;
    ini_str(L"hotkey", L"Ctrl+Shift+F10", b, 128);
    if (!parse_hotkey(b, &s.hk_mods, &s.hk_vk)) { s.hk_mods = d.hk_mods; s.hk_vk = d.hk_vk; }
    S = s;
}

static void read_ini_mtime(FILETIME *ft) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (GetFileAttributesExW(g_ini, GetFileExInfoStandard, &a)) *ft = a.ftLastWriteTime;
    else memset(ft, 0, sizeof(*ft));
}

static void save_settings(void) {
    if (GetFileAttributesW(g_ini) == INVALID_FILE_ATTRIBUTES) {
        HANDLE f = CreateFileW(g_ini, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            const char *hdr =
                "; 255 Input Overlay settings. Edited live by the Settings window (tray icon).\r\n"
                "; You can also edit this file while the app runs; it reloads automatically.\r\n"
                "; mode: overlay (on top of game), chroma (green screen window) or stream (stream window)\r\n"
                "; input: rawinput (Normal) or hook (Backup)\r\n"
                "; apm_position / keys_position: top-left, top-right, bottom-left, bottom-right\r\n"
                "; fps: 0 matches your monitor, or a fixed number like 60, 120 or 144\r\n"
                "; max_redraw_fps: most redraws per second (60, 120, 144, or 0 for every screen refresh)\r\n\r\n[overlay]\r\n";
            DWORD w; WriteFile(f, hdr, (DWORD)strlen(hdr), &w, NULL); CloseHandle(f);
        }
    }
    wchar_t b[64];
    put_s(L"mode", S.mode == 2 ? L"stream" : S.mode ? L"chroma" : L"overlay");
    put_col(L"trail_color", S.st.trail);
    put_f(L"trail_width", S.st.trail_width);
    put_i(L"trail_length_ms", (int)lroundf(S.st.trail_len_s * 1000));
    put_i(L"cursor_dot", S.st.cursor_dot);
    put_col(L"left_click_color", S.st.lmb);
    put_col(L"right_click_color", S.st.rmb);
    put_f(L"click_size", S.st.click_size);
    put_i(L"click_fade_ms", (int)lroundf(S.st.click_fade_s * 1000));
    put_i(L"apm_enabled", S.st.apm_enabled);
    put_s(L"apm_position", corner_names[S.st.apm_corner]);
    put_f(L"apm_offset_x", roundf(S.st.apm_off_x)); put_f(L"apm_offset_y", roundf(S.st.apm_off_y));
    put_s(L"apm_x", NULL); put_s(L"apm_y", NULL); put_s(L"apm_margin", NULL); /* v1.x keys */
    put_s(L"apm_show_icon", NULL); put_s(L"apm_show_255", NULL); /* v1.3 beta keys: branding is not drawn in the output */
    put_i(L"trail_taper_pct", (int)lroundf(S.st.trail_taper * 100));
    put_i(L"trail_enabled", S.st.trail_enabled);
    put_i(L"trail_smoothing", S.st.trail_smooth);
    put_i(L"click_left", S.st.click_show[0]); put_i(L"click_right", S.st.click_show[1]);
    put_i(L"click_middle", S.st.click_show[2]); put_i(L"click_side", S.st.click_show[3]);
    put_col(L"other_click_color", S.st.click_other);
    put_i(L"glow_trail", S.st.glow_trail); put_i(L"glow_clicks", S.st.glow_clicks); put_i(L"glow_apm", S.st.glow_apm);
    put_i(L"glow_strength_pct", (int)lroundf(S.st.glow_strength * 100));
    put_f(L"glow_size", S.st.glow_radius);
    put_s(L"glow_match_color", NULL);
    put_i(L"glow_color_mode", S.st.glow_mode);
    put_col(L"glow_color", S.st.glow_col);
    put_col(L"glow_color_end", S.st.glow_col2);
    put_s(L"trail_color_style", S.st.trail_grad ? L"two" : L"single");
    put_col(L"trail_tail_color", S.st.trail_tail);
    { wchar_t lb[32]; int i = 0; for (; S.st.label[i] && i < 15; i++) lb[i] = (wchar_t)(unsigned char)S.st.label[i]; lb[i] = 0;
      put_s(L"apm_label_text", lb[0] ? lb : L"\"\""); }
    put_i(L"apm_font", S.st.font);
    put_i(L"hide_when_idle_s", (int)lroundf(S.idle_hide_s));
    put_i(L"apm_label_size_pct", (int)lroundf(S.st.lab_scale * 100));
    put_i(L"apm_label_bold", S.st.lab_bold); put_i(L"apm_label_outline", S.st.lab_outline);
    put_s(L"apm_label_position", S.st.lab_below ? L"under" : L"right");
    put_i(L"scale_with_screen", S.scale_screen);
    put_i(L"mouse_next_to_keys", S.mouse_attach); put_s(L"mouse_position", corner_names[S.st.mouse_corner]);
    put_f(L"mouse_offset_x", roundf(S.st.mouse_off_x)); put_f(L"mouse_offset_y", roundf(S.st.mouse_off_y));
    put_i(L"keys_show_keyboard", S.st.keys_kb); put_i(L"keys_show_mouse", S.st.keys_mouse);
    { static const wchar_t *kl[] = { L"left", L"league-keys", L"custom", L"wasd", L"full" }; put_s(L"keys_layout", kl[S.st.keys_layout % 5]); }
    put_s(L"keys_shape", S.st.keys_shape ? L"slanted" : L"rounded");
    { static wchar_t kc[2048]; int i = 0; for (; S.st.keys_custom[i] && i < 2047; i++) kc[i] = (wchar_t)(unsigned char)S.st.keys_custom[i]; kc[i] = 0;
      put_s(L"keys_custom", kc[0] ? kc : L"\"\""); }
    put_col(L"keys_outline_color", S.st.keys_outline); put_col(L"keys_pressed_color", S.st.keys_on);
    put_col(L"keys_label_color", S.st.keys_label); put_col(L"keys_idle_color", S.st.keys_idle);
    put_i(L"keys_idle_opacity_pct", (int)lroundf(S.st.keys_idle_op * 100));
    put_i(L"keys_size_pct", (int)lroundf(S.st.keys_scale * 100));
    put_s(L"keys_position", corner_names[S.st.keys_corner]);
    put_f(L"keys_offset_x", roundf(S.st.keys_off_x)); put_f(L"keys_offset_y", roundf(S.st.keys_off_y));
    put_i(L"keys_glow", S.st.keys_glow);
    put_i(L"keys_backplate", S.st.keys_plate); put_col(L"keys_backplate_color", S.st.keys_plate_col);
    put_i(L"keys_backplate_opacity_pct", (int)lroundf(S.st.keys_plate_op * 100));
    put_i(L"keys_background", S.keys_bg);
    put_i(L"keys_3d", S.st.keys_depth); put_i(L"keys_italic", S.st.keys_italic); put_i(L"keys_font", S.st.keys_font);
    put_f(L"keys_outline_px", S.st.keys_line);
    put_i(L"apm_border", S.st.panel_border); put_i(L"apm_accent_bar", S.st.panel_accent);
    put_i(L"alignment_check", S.align_check);
    put_f(L"nudge_x_px", S.fix_dx); put_f(L"nudge_y_px", S.fix_dy);
    put_f(L"stretch_x_pct", roundf(S.fix_sx * 1000) / 10); put_f(L"stretch_y_pct", roundf(S.fix_sy * 1000) / 10);
    put_i(L"trail_outline", S.st.trail_outline);
    put_f(L"trail_outline_px", S.st.trail_outline_px);
    put_col(L"trail_outline_color", S.st.trail_outline_col);
    { static const wchar_t *cs[] = { L"ring+dot", L"ring", L"dot" }; put_s(L"click_style", cs[S.st.click_style % 3]); }
    put_f(L"click_ring_px", S.st.click_ring_px);
    put_i(L"click_outline", S.st.click_outline);
    put_f(L"click_outline_px", S.st.click_outline_px);
    put_col(L"click_outline_color", S.st.click_outline_col);
    put_i(L"apm_number_size_pct", (int)lroundf(S.st.num_scale * 100));
    put_f(L"apm_padding", S.st.panel_pad);
    put_f(L"apm_corner_radius", S.st.panel_radius);
    put_i(L"apm_bold", S.st.text_bold);
    put_i(L"apm_outline", S.st.text_outline);
    put_f(L"apm_outline_px", S.st.outline_px);
    put_col(L"apm_outline_color", S.st.outline_col);
    put_i(L"apm_shadow", S.st.text_shadow);
    put_f(L"apm_scale", S.st.apm_scale);
    put_col(L"apm_color", S.st.apm_text);
    put_col(L"apm_label_color", S.st.apm_label);
    put_i(L"apm_background", S.st.panel_enabled);
    put_col(L"apm_background_color", S.st.panel);
    put_f(L"apm_background_opacity", S.st.panel_opacity);
    put_i(L"apm_box_background", S.apm_bg);
    put_f(L"apm_window_s", S.apm_window_s);
    put_i(L"apm_version", 2);
    put_i(L"apm_show_average", S.st.apm_show_avg);
    put_s(L"apm_window_seconds", NULL); /* remove v1 key */
    put_i(L"count_keys", S.count_keys);
    put_i(L"count_clicks", S.count_clicks);
    put_s(L"input", S.input_hook ? L"hook" : L"rawinput");
    put_i(L"fps", S.fps); put_i(L"max_redraw_fps", S.max_draw);
    put_col(L"chroma_color", S.chroma);
    put_i(L"chroma_width", S.chroma_w); put_i(L"chroma_height", S.chroma_h);
    put_s(L"chroma_source", S.chroma_screen == 4 ? L"virtual" : L"primary");
    { static const wchar_t *gs[] = { L"game", L"screen1", L"screen2", L"mouse", L"game", L"client" }; put_s(L"green_screen_shows", gs[S.chroma_screen % 6]); }
    put_i(L"chroma_topmost", S.chroma_topmost);
    put_i(L"stream_window_clear", S.stream_clear);
    put_i(L"stream_window_guide", S.stream_guide);
    put_i(L"settings_x", S.set_x); put_i(L"settings_y", S.set_y); put_i(L"settings_pos", S.set_pos);
    put_i(L"green_screen_corner_marks", S.chroma_marks);
    put_s(L"green_screen_edges", S.chroma_hard ? L"hard" : L"smooth"); put_s(L"green_screen_window", S.chroma_fit ? L"screen" : L"custom");
    hotkey_text(S.hk_mods, S.hk_vk, b, 64); put_s(L"hotkey", b);
    read_ini_mtime(&g_ini_mtime);
}

/* ------------------------------------------------------------------ input */
static CRITICAL_SECTION g_lock;
static LARGE_INTEGER g_qpf;
static double now_s(void) { LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)g_qpf.QuadPart; }

#define TRAIL_CAP 16384
#define CLICK_CAP 512
static TrailPt g_tr[TRAIL_CAP]; static unsigned g_tr_n;
static ClickEv g_ck[CLICK_CAP]; static unsigned g_ck_n;
static ApmState g_apm; /* action timestamps only */
static volatile double g_last_input;

static void push_point(POINT p, double t) {
    EnterCriticalSection(&g_lock);
    if (g_tr_n) {
        TrailPt *l = &g_tr[(g_tr_n - 1) % TRAIL_CAP];
        if (l->x == (float)p.x && l->y == (float)p.y) { LeaveCriticalSection(&g_lock); return; }
    }
    TrailPt *q = &g_tr[g_tr_n % TRAIL_CAP]; q->t = t; q->x = (float)p.x; q->y = (float)p.y; g_tr_n++;
    g_last_input = t;
    LeaveCriticalSection(&g_lock);
}
static void push_action(double t) {
    EnterCriticalSection(&g_lock);
    apm_add(&g_apm, t, S.apm_window_s);
    g_last_input = t;
    LeaveCriticalSection(&g_lock);
}
static void push_click(POINT p, double t, int btn) {
    EnterCriticalSection(&g_lock);
    ClickEv *c = &g_ck[g_ck_n % CLICK_CAP]; c->t = t; c->x = (float)p.x; c->y = (float)p.y; c->btn = btn; g_ck_n++;
    g_last_input = t;
    LeaveCriticalSection(&g_lock);
    if (S.count_clicks) push_action(t);
}
static unsigned char g_kdown[256]; /* held-state only, used to skip auto-repeat and to draw held keys; never stored */
static volatile double g_krel[256], g_mrel[5]; /* release times for the fade-out only */
static volatile unsigned char g_mbtn[5]; /* mouse buttons held: left, right, middle, side 1, side 2 */
static volatile double g_wheel_t; static volatile int g_wheel_dir;
static void mouse_btn(int b, int down) { if (b >= 0 && b < 5) { if (!down && g_mbtn[b]) g_mrel[b] = now_s(); g_mbtn[b] = (unsigned char)(down != 0); } }
static void mouse_wheel(int delta) { if (delta) { g_wheel_dir = delta > 0 ? 1 : -1; g_wheel_t = now_s(); } }
static void key_event(unsigned vk, int down) {
    vk &= 255;
    if (!down && g_kdown[vk]) g_krel[vk] = now_s();
    if (down) { if (!g_kdown[vk]) { g_kdown[vk] = 1; if (S.count_keys) push_action(now_s()); } }
    else g_kdown[vk] = 0;
}

static HINSTANCE g_inst;
static HANDLE g_input_thread; static DWORD g_input_tid;
static HHOOK g_mh, g_kh;

static LRESULT CALLBACK ll_mouse(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION) {
        double t = now_s(); POINT p; cursor_pos(&p);
        switch (w) {
        case WM_MOUSEMOVE: push_point(p, t); break;
        case WM_LBUTTONDOWN: mouse_btn(0, 1); push_click(p, t, 0); break;
        case WM_RBUTTONDOWN: mouse_btn(1, 1); push_click(p, t, 1); break;
        case WM_MBUTTONDOWN: mouse_btn(2, 1); push_click(p, t, 2); break;
        case WM_XBUTTONDOWN: mouse_btn(HIWORD(((MSLLHOOKSTRUCT *)l)->mouseData) == 2 ? 4 : 3, 1); push_click(p, t, 3); break;
        case WM_LBUTTONUP: mouse_btn(0, 0); break;
        case WM_RBUTTONUP: mouse_btn(1, 0); break;
        case WM_MBUTTONUP: mouse_btn(2, 0); break;
        case WM_XBUTTONUP: mouse_btn(HIWORD(((MSLLHOOKSTRUCT *)l)->mouseData) == 2 ? 4 : 3, 0); break;
        case WM_MOUSEWHEEL: mouse_wheel((short)HIWORD(((MSLLHOOKSTRUCT *)l)->mouseData)); break;
        }
    }
    return CallNextHookEx(NULL, code, w, l);
}
static LRESULT CALLBACK ll_key(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION) {
        KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)l;
        key_event(k->vkCode, w == WM_KEYDOWN || w == WM_SYSKEYDOWN);
    }
    return CallNextHookEx(NULL, code, w, l);
}
static LRESULT CALLBACK input_wndproc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_INPUT) {
        union { RAWINPUT ri; BYTE b[256]; } u;
        UINT sz = sizeof(u);
        if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &u, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1) {
            if (u.ri.header.dwType == RIM_TYPEMOUSE) {
                USHORT f = u.ri.data.mouse.usButtonFlags;
                POINT p; cursor_pos(&p); double t = now_s();
                push_point(p, t);
                if (f & RI_MOUSE_LEFT_BUTTON_DOWN) push_click(p, t, 0);
                if (f & RI_MOUSE_RIGHT_BUTTON_DOWN) push_click(p, t, 1);
                if (f & RI_MOUSE_MIDDLE_BUTTON_DOWN) push_click(p, t, 2);
                if (f & (RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_5_DOWN)) push_click(p, t, 3);
                if (f & RI_MOUSE_LEFT_BUTTON_DOWN) mouse_btn(0, 1);
                if (f & RI_MOUSE_LEFT_BUTTON_UP) mouse_btn(0, 0);
                if (f & RI_MOUSE_RIGHT_BUTTON_DOWN) mouse_btn(1, 1);
                if (f & RI_MOUSE_RIGHT_BUTTON_UP) mouse_btn(1, 0);
                if (f & RI_MOUSE_MIDDLE_BUTTON_DOWN) mouse_btn(2, 1);
                if (f & RI_MOUSE_MIDDLE_BUTTON_UP) mouse_btn(2, 0);
                if (f & RI_MOUSE_BUTTON_4_DOWN) mouse_btn(3, 1);
                if (f & RI_MOUSE_BUTTON_4_UP) mouse_btn(3, 0);
                if (f & RI_MOUSE_BUTTON_5_DOWN) mouse_btn(4, 1);
                if (f & RI_MOUSE_BUTTON_5_UP) mouse_btn(4, 0);
                if (f & RI_MOUSE_WHEEL) mouse_wheel((short)u.ri.data.mouse.usButtonData);
            } else if (u.ri.header.dwType == RIM_TYPEKEYBOARD) {
                USHORT vk = u.ri.data.keyboard.VKey;
                if (vk > 0 && vk < 255) key_event(vk, !(u.ri.data.keyboard.Flags & RI_KEY_BREAK));
            }
        }
        return DefWindowProcW(h, msg, w, l);
    }
    return DefWindowProcW(h, msg, w, l);
}
static DWORD WINAPI input_thread(LPVOID arg) {
    HANDLE ready = (HANDLE)arg;
    thread_pmv2();
    HWND h = CreateWindowExW(0, L"MAOInputSink", L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, g_inst, NULL);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    RAWINPUTDEVICE rid[2] = { { 1, 2, RIDEV_INPUTSINK, h }, { 1, 6, RIDEV_INPUTSINK, h } };
    int hook = S.input_hook;
    if (hook) {
        g_mh = SetWindowsHookExW(WH_MOUSE_LL, ll_mouse, g_inst, 0);
        g_kh = SetWindowsHookExW(WH_KEYBOARD_LL, ll_key, g_inst, 0);
    } else {
        RegisterRawInputDevices(rid, 2, sizeof(rid[0]));
    }
    SetEvent(ready);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) DispatchMessageW(&m);
    if (hook) { if (g_mh) UnhookWindowsHookEx(g_mh); if (g_kh) UnhookWindowsHookEx(g_kh); g_mh = g_kh = NULL; }
    else { rid[0].dwFlags = rid[1].dwFlags = RIDEV_REMOVE; rid[0].hwndTarget = rid[1].hwndTarget = NULL; RegisterRawInputDevices(rid, 2, sizeof(rid[0])); }
    DestroyWindow(h);
    return 0;
}
static int g_input_is_hook = -1;
static void input_start(void) {
    if (g_input_thread && g_input_is_hook == S.input_hook) return;
    if (g_input_thread) {
        PostThreadMessageW(g_input_tid, WM_QUIT, 0, 0);
        WaitForSingleObject(g_input_thread, 2000); CloseHandle(g_input_thread); g_input_thread = NULL;
    }
    memset(g_kdown, 0, sizeof(g_kdown)); for (int i = 0; i < 5; i++) g_mbtn[i] = 0;
    HANDLE ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_input_thread = CreateThread(NULL, 0, input_thread, ready, 0, &g_input_tid);
    WaitForSingleObject(ready, 2000); CloseHandle(ready);
    g_input_is_hook = S.input_hook;
}
static void input_stop(void) {
    if (!g_input_thread) return;
    PostThreadMessageW(g_input_tid, WM_QUIT, 0, 0);
    WaitForSingleObject(g_input_thread, 2000); CloseHandle(g_input_thread); g_input_thread = NULL;
}

/* snapshot for one frame */
static TrailPt s_tr[TRAIL_CAP]; static int s_ntr;
static ClickEv s_ck[CLICK_CAP]; static int s_nck;
static void snapshot(double now) {
    EnterCriticalSection(&g_lock);
    double tmin = now - S.st.trail_len_s - 0.05;
    unsigned n = g_tr_n, k = 0;
    while (k < n && k < TRAIL_CAP - 1) {
        const TrailPt *p = &g_tr[(n - 1 - k) % TRAIL_CAP];
        k++;
        if (p->t < tmin) break; /* keep one point older than the window for the tail */
    }
    s_ntr = (int)k;
    for (unsigned i = 0; i < k; i++) s_tr[i] = g_tr[(n - k + i) % TRAIL_CAP];
    for (unsigned i = 0; i < k; i++) { s_tr[i].x = s_tr[i].x * S.fix_sx + S.fix_dx; s_tr[i].y = s_tr[i].y * S.fix_sy + S.fix_dy; }
    s_nck = 0;
    unsigned c = g_ck_n;
    for (unsigned i = 0; i < c && i < CLICK_CAP; i++) {
        const ClickEv *e = &g_ck[(c - 1 - i) % CLICK_CAP];
        if (now - e->t >= S.st.click_fade_s) break;
        ClickEv c2 = *e; c2.x = c2.x * S.fix_sx + S.fix_dx; c2.y = c2.y * S.fix_sy + S.fix_dy;
        s_ck[s_nck++] = c2;
    }
    LeaveCriticalSection(&g_lock);
}
static int g_apm_avg;
static int apm_now(double now) { /* call once per frame: advances the smoothing */
    EnterCriticalSection(&g_lock);
    double v = apm_tick(&g_apm, now, S.apm_window_s);
    g_apm_avg = (int)lround(apm_avg(&g_apm, now));
    LeaveCriticalSection(&g_lock);
    return (int)lround(v);
}

/* ------------------------------------------------------------------ surfaces */
typedef struct {
    Canvas C; Layer L; uint32_t *out; uint32_t *bg; int w, h;
    HBITMAP dib; HDC mdc; HGDIOBJ old;
} Surface;

static void surface_free(Surface *s) {
    if (s->mdc) { SelectObject(s->mdc, s->old); DeleteDC(s->mdc); }
    if (s->dib) DeleteObject(s->dib); else free(s->C.px);
    free(s->out); free(s->bg); layer_free(&s->L);
    memset(s, 0, sizeof(*s));
}
static BITMAPINFO bmi_for(int w, int h) {
    BITMAPINFO b; memset(&b, 0, sizeof(b));
    b.bmiHeader.biSize = sizeof(b.bmiHeader); b.bmiHeader.biWidth = w; b.bmiHeader.biHeight = -h;
    b.bmiHeader.biPlanes = 1; b.bmiHeader.biBitCount = 32; b.bmiHeader.biCompression = BI_RGB;
    return b;
}
static int surface_alloc(Surface *s, int w, int h, int with_dib) {
    surface_free(s);
    if (w < 1) w = 1; if (h < 1) h = 1;
    s->w = w; s->h = h; s->C.w = w; s->C.h = h;
    if (with_dib) {
        BITMAPINFO b = bmi_for(w, h); void *bits = NULL;
        HDC sdc = GetDC(NULL);
        s->dib = CreateDIBSection(sdc, &b, DIB_RGB_COLORS, &bits, NULL, 0);
        s->mdc = CreateCompatibleDC(sdc);
        ReleaseDC(NULL, sdc);
        if (!s->dib || !s->mdc) return 0;
        s->old = SelectObject(s->mdc, s->dib);
        s->C.px = (uint32_t *)bits;
        memset(bits, 0, (size_t)w * h * 4);
    } else {
        s->C.px = (uint32_t *)calloc((size_t)w * h, 4);
        s->out = (uint32_t *)malloc((size_t)w * h * 4);
        if (!s->C.px || !s->out) return 0;
    }
    return layer_init(&s->L, w, h);
}
/* out = premult src over (solid colour or bg image).
   keyed (green screen): never blend with the key colour. Pixels under half coverage become pure key,
   the rest keep their full colour, so there is no green fringe and no dark rim. Shapes are drawn on a
   solid base, so their soft edges are already inside opaque pixels. */
static int g_sw_clear; /* stream swap chain is premultiplied (see-through desktop) */
static int g_saved_flash;
static void compose(Surface *s, uint32_t solid, const uint32_t *bg, int keyed) {
    size_t n = (size_t)s->w * s->h;
    if (keyed == 2) { /* stream window: colour + alpha per pixel, nothing behind it */
        if (g_sw_clear) { memcpy(s->out, s->C.px, n * 4); return; } /* premultiplied, for a see-through swap chain */
        for (size_t i = 0; i < n; i++) {
            uint32_t v = s->C.px[i], a = v >> 24;
            if (!a) { s->out[i] = 0; continue; }
            uint32_t r = ((v >> 16) & 255) * 255 / a, g = ((v >> 8) & 255) * 255 / a, bl = (v & 255) * 255 / a;
            s->out[i] = (a << 24) | ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (bl > 255 ? 255 : bl);
        }
        return;
    }
    for (size_t i = 0; i < n; i++) {
        uint32_t v = s->C.px[i], b = bg ? bg[i] : solid;
        uint32_t a = v >> 24;
        if (keyed) {
            if (a < 128) { s->out[i] = b; continue; }
            uint32_t r = ((v >> 16) & 255) * 255 / a, g = ((v >> 8) & 255) * 255 / a, bl = (v & 255) * 255 / a;
            s->out[i] = ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (bl > 255 ? 255 : bl);
            continue;
        }
        if (!a) { s->out[i] = b; continue; }
        uint32_t ia = 255 - a;
        uint32_t r = ((v >> 16) & 255) + (((b >> 16) & 255) * ia + 127) / 255;
        uint32_t g = ((v >> 8) & 255) + (((b >> 8) & 255) * ia + 127) / 255;
        uint32_t bl = (v & 255) + ((b & 255) * ia + 127) / 255;
        if (r > 255) r = 255; if (g > 255) g = 255; if (bl > 255) bl = 255;
        s->out[i] = (r << 16) | (g << 8) | bl;
    }
}
static void present(HWND hwnd, Surface *s) {
    HDC dc = GetDC(hwnd);
    BITMAPINFO b = bmi_for(s->w, s->h);
    SetDIBitsToDevice(dc, 0, 0, s->w, s->h, 0, 0, 0, s->h, s->out, &b, DIB_RGB_COLORS);
    ReleaseDC(hwnd, dc);
}

/* ------------------------------------------------------------------ display */
static HWND g_main, g_overlay, g_chroma, g_set, g_preview, g_ked, g_guide;
static Surface g_ov, g_ch, g_pv, g_gd;
static int g_guide_flash, g_gl_x, g_gl_y, g_gl_w, g_gl_h, g_guide_drag, g_guide_dx, g_guide_dy, g_set_iconic;
static DWORD g_guide_quiet;
static RECT g_virt, g_prim, g_src; /* g_src: the screen the green screen / stream window shows */
static int g_hidden, g_force, g_ov_first, g_quit, g_idle;
/* closing the program from anywhere (Settings X, Alt+F4, tray Quit): hide everything at once,
   then the main loop ends and cleans up */
static DWORD WINAPI quit_watchdog(LPVOID p) { (void)p; Sleep(3000); ExitProcess(0); }
static void app_quit(void) {
    if (g_quit) return;
    g_quit = 1;
    if (g_ked) ShowWindow(g_ked, SW_HIDE);
    if (g_set) ShowWindow(g_set, SW_HIDE);
    if (g_overlay) ShowWindow(g_overlay, SW_HIDE);
    if (g_chroma) ShowWindow(g_chroma, SW_HIDE);
    if (g_guide) ShowWindow(g_guide, SW_HIDE);
    g_guide_flash = 0;
    PostQuitMessage(0);
}
static IRect g_prev_bb;
static int g_last_apm = -1, g_last_static;
/* APM box drag ("Move APM box") */
typedef struct { int valid, which; float k, ax, ay, aw, ah, px, py, pw, ph; } BoxLay; /* which: 0 APM box, 1 keys, 2 mouse */
static BoxLay g_ov_lay, g_ch_lay, g_ov_klay, g_ch_klay, g_ov_mlay, g_ch_mlay;
static int g_move, g_drag; static float g_grab_dx, g_grab_dy; static BoxLay *g_drag_lay;
static void capture_lay(BoxLay *b, const Style *st, const Scene *sc) {
    b->k = sc->scale * (st->apm_res > 0 ? st->apm_res : 1.0f); b->ax = sc->area_x; b->ay = sc->area_y; b->aw = sc->area_w; b->ah = sc->area_h;
    apm_panel_rect(st, sc, &b->px, &b->py, &b->pw, &b->ph); b->valid = st->apm_enabled; b->which = 0;
}
static void capture_klay(BoxLay *b, const Style *st, const Scene *sc) {
    b->k = sc->scale * (st->apm_res > 0 ? st->apm_res : 1.0f); b->ax = sc->area_x; b->ay = sc->area_y; b->aw = sc->area_w; b->ah = sc->area_h; b->which = 1;
    b->valid = st->keys_kb || st->keys_mouse;
    if (b->valid) keys_panel_rect(st, sc, &b->px, &b->py, &b->pw, &b->ph);
    if (b->pw <= 0 || b->ph <= 0) b->valid = 0;
}
/* the mouse rect is kept even while it sits next to the keys, so it can stay put when it is split off */
static void capture_mlay(BoxLay *b, const Style *st, const Scene *sc) {
    b->k = sc->scale * (st->apm_res > 0 ? st->apm_res : 1.0f); b->ax = sc->area_x; b->ay = sc->area_y; b->aw = sc->area_w; b->ah = sc->area_h; b->which = 2;
    mouse_panel_rect(st, sc, &b->px, &b->py, &b->pw, &b->ph);
    b->valid = st->keys_mouse && st->keys_kb && st->mouse_sep && b->pw > 0 && b->ph > 0;
}
static int lay_hit(const BoxLay *b, float x, float y) { return b->valid && x >= b->px && x < b->px + b->pw && y >= b->py && y < b->py + b->ph; }
static float screen_factor(void) {
    float h = (float)(g_prim.bottom - g_prim.top);
    return (S.scale_screen && h > 0) ? clampf(h / 1080.0f, 0.4f, 4.0f) : 1.0f;
}
static void style_apply_bg(Style *t, const Settings *s) {
    if (s->keys_bg == 0) t->keys_plate = 0;
    else if (s->keys_bg == 1) {
        t->keys_plate = 1;
        t->keys_plate_col = (RGBf){ 0x0A / 255.f, 0x0C / 255.f, 0x10 / 255.f };
        t->keys_plate_op = 0.78f;
    } else t->keys_plate = 1;
    if (s->apm_bg == 0) { t->panel_enabled = 0; t->text_outline = 1; }
    else if (s->apm_bg == 1) {
        t->panel_enabled = 1;
        t->panel = (RGBf){ 0x0B / 255.f, 0x0F / 255.f, 0x17 / 255.f };
        t->panel_opacity = 0.78f;
    } else t->panel_enabled = 1;
}
static Style frame_style(int chroma) {
    Style st = S.st;
    style_apply_bg(&st, &S);
    st.apm_res = screen_factor();
    st.mouse_sep = !S.mouse_attach;
    if (chroma) { /* nothing semi-transparent may touch the key colour: soft edges fall on a solid darker rim instead */
        st.panel_opacity = 1.0f; st.solid = 1;
        if (S.chroma_hard && !st.trail_outline) { st.trail_outline = 1; st.trail_outline_px = 1.0f; st.trail_outline_col = (RGBf){ st.trail.r * 0.4f, st.trail.g * 0.4f, st.trail.b * 0.4f }; }
        if (S.chroma_hard && !st.click_outline) { st.click_outline = 1; st.click_outline_px = 1.0f; st.click_outline_col = (RGBf){ 0.06f, 0.07f, 0.09f }; }
    }
    if (g_move) { st.apm_enabled = 1; st.apm_highlight = 1; st.keys_highlight = 1; if (!chroma) { st.trail_len_s = 0; st.click_fade_s = 0; st.cursor_dot = 0; } }
    return st;
}
static double g_last_frame;

static void update_monitors(void) {
    g_virt.left = GetSystemMetrics(SM_XVIRTUALSCREEN); g_virt.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_virt.right = g_virt.left + GetSystemMetrics(SM_CXVIRTUALSCREEN); g_virt.bottom = g_virt.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    POINT o = { 0, 0 }; MONITORINFO mi; memset(&mi, 0, sizeof(mi)); mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(MonitorFromPoint(o, MONITOR_DEFAULTTOPRIMARY), &mi)) g_prim = mi.rcMonitor;
    else { g_prim.left = 0; g_prim.top = 0; g_prim.right = GetSystemMetrics(SM_CXSCREEN); g_prim.bottom = GetSystemMetrics(SM_CYSCREEN); }
}

typedef struct { RECT r[16]; int n; } MonList;
static BOOL CALLBACK mon_enum(HMONITOR m, HDC dc, LPRECT r, LPARAM l) {
    (void)m; (void)dc; MonList *L = (MonList *)l; if (L->n < 16) L->r[L->n++] = *r; return TRUE;
}
/* which screen the green screen / stream window maps, in real screen pixels */
static RECT mon_rect_of_window(HWND w) {
    MONITORINFO mi; memset(&mi, 0, sizeof(mi)); mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi)) return mi.rcMonitor;
    return g_prim;
}
/* screen 2: the first screen that is not the main one, left to right; 0 if there is none */
static int second_screen(RECT *out) {
    MonList L; L.n = 0; EnumDisplayMonitors(NULL, NULL, mon_enum, (LPARAM)&L);
    int best = -1;
    for (int i = 0; i < L.n; i++) {
        if (EqualRect(&L.r[i], &g_prim)) continue;
        if (best < 0 || L.r[i].left < L.r[best].left || (L.r[i].left == L.r[best].left && L.r[i].top < L.r[best].top)) best = i;
    }
    if (best < 0) return 0;
    *out = L.r[best]; return 1;
}
/* the League window, found only by its window class / title (never opens or reads the game) */
static HWND find_league(void) {
    static const wchar_t *const cls[] = { L"RiotWindowClass", L"RCLIENT" };
    static const wchar_t *const titles[] = { L"League of Legends (TM) Client", L"League of Legends" };
    for (int i = 0; i < 2; i++) { HWND w = FindWindowW(cls[i], NULL); if (w && IsWindowVisible(w) && !IsIconic(w)) return w; }
    for (int i = 0; i < 2; i++) { HWND w = FindWindowW(NULL, titles[i]); if (w && IsWindowVisible(w) && !IsIconic(w)) return w; }
    return NULL;
}
/* League's game window (in a match) and its client (lobby), by class / title only */
static HWND visible_win(const wchar_t *cls, const wchar_t *title) {
    HWND w = FindWindowW(cls, title);
    return w && IsWindowVisible(w) && !IsIconic(w) ? w : NULL;
}
static HWND league_game(void) {
    HWND w = visible_win(L"RiotWindowClass", NULL);
    return w ? w : visible_win(NULL, L"League of Legends (TM) Client");
}
static HWND league_client(RECT *r) {
    HWND w = visible_win(L"RCLIENT", NULL);
    if (!w) w = visible_win(NULL, L"League of Legends");
    if (!w) return NULL;
    if (FAILED(DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS, r, sizeof(*r)))) GetWindowRect(w, r); /* the visible edges, no shadow */
    return r->right - r->left > 50 && r->bottom - r->top > 50 ? w : NULL;
}
static RECT mouse_screen(void) {
    POINT p; cursor_pos(&p);
    MONITORINFO mi; memset(&mi, 0, sizeof(mi)); mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &mi)) return mi.rcMonitor;
    return g_prim;
}
static RECT game_screen(void) { /* where League is; else the window in front (not this program); else the mouse */
    HWND w = find_league();
    if (w) return mon_rect_of_window(w);
    HWND f = GetForegroundWindow(); DWORD pid = 0;
    if (f) GetWindowThreadProcessId(f, &pid);
    if (f && pid != GetCurrentProcessId() && f != GetShellWindow() && IsWindowVisible(f) && !IsIconic(f)) return mon_rect_of_window(f);
    return mouse_screen();
}
static RECT pick_src(void) {
    switch (S.chroma_screen) {
    case 1: return g_prim;
    case 2: { RECT r; return second_screen(&r) ? r : g_prim; }
    case 3: return mouse_screen();
    case 4: return g_virt;
    case 5: { /* follow League client: the window covers the screen the game or the client is on */
        RECT r; HWND w = league_game();
        if (!w) w = league_client(&r);
        return w ? mon_rect_of_window(w) : game_screen();
    }
    default: return game_screen();
    }
}
static void clear_trail(void);
/* put the window ON the shown screen: "Same as screen" covers exactly that screen; Custom keeps its
   size and starts at that screen's top-left. Moved first, then sized, so it takes that screen's scaling
   before its size is set; checked and redone if Windows moved it. */
static void chroma_place(int custom_too) {
    if (!g_chroma || IsIconic(g_chroma)) return;
    if (!S.chroma_fit && !custom_too) return;
    RECT want = g_src;
    if (!S.chroma_fit) { want.right = want.left + S.chroma_w; want.bottom = want.top + S.chroma_h; }
    RECT r = want; AdjustWindowRectEx(&r, (DWORD)GetWindowLongPtrW(g_chroma, GWL_STYLE), FALSE, (DWORD)GetWindowLongPtrW(g_chroma, GWL_EXSTYLE));
    for (int tries = 0; tries < 3; tries++) {
        RECT wr; GetWindowRect(g_chroma, &wr);
        if (EqualRect(&wr, &r)) return;
        SetWindowPos(g_chroma, NULL, r.left, r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(g_chroma, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
}
static void chroma_fit_size(void) { chroma_place(0); } /* "Same as screen": the inside of the window is exactly the shown screen */
static HICON g_icon_big, g_icon_small;

static void sw_free(void);
static int sw_init(HWND h, int w, int ht);
static void settings_status(void);

enum { GUIDE_EDGE = 10, GUIDE_FLASH_MS = 2500 };
static void guide_destroy(void);
static void guide_update(void);

static int settings_open(void) {
    if (!g_set || !IsWindow(g_set) || g_set_iconic || IsIconic(g_set)) return 0;
    return IsWindowVisible(g_set);
}
static int guide_wanted(void) {
    if (g_quit || S.mode != 2 || !g_chroma || !IsWindow(g_chroma)) return 0;
    if (IsIconic(g_chroma) || !IsWindowVisible(g_chroma)) return 0;
    if (g_guide_flash) return 1;
    return S.stream_guide && settings_open();
}
static void guide_dash(Canvas *C, Layer *L, float x0, float y0, float x1, float y1, float lw, RGBf col, float a) {
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    if (len < 1.f) return;
    float ux = dx / len, uy = dy / len, t = 0.f;
    const float on = 11.f, off = 7.f;
    while (t < len) {
        float t1 = t + on; if (t1 > len) t1 = len;
        draw_line(C, L, x0 + ux * t, y0 + uy * t, x0 + ux * t1, y0 + uy * t1, lw, col, a);
        t += on + off;
    }
}
static void guide_over(uint32_t *d, unsigned sr, unsigned sg, unsigned sb, unsigned sa) {
    if (!sa) return;
    unsigned dr = (*d >> 16) & 255, dg = (*d >> 8) & 255, db = *d & 255, da = *d >> 24, ia = 255 - sa;
    unsigned or = sr + (dr * ia + 127) / 255, og = sg + (dg * ia + 127) / 255, ob = sb + (db * ia + 127) / 255;
    unsigned oa = sa + (da * ia + 127) / 255;
    if (or > 255) or = 255; if (og > 255) og = 255; if (ob > 255) ob = 255; if (oa > 255) oa = 255;
    *d = (oa << 24) | (or << 16) | (og << 8) | ob;
}
static void guide_label(Surface *s) {
    const wchar_t *txt = APP_NAME L" (stream window)";
    HFONT f = CreateFontW(-13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    if (!f) f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    int own = f && f != (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HDC sdc = GetDC(NULL);
    HGDIOBJ of = SelectObject(sdc, f);
    SIZE sz; GetTextExtentPoint32W(sdc, txt, (int)wcslen(txt), &sz);
    SelectObject(sdc, of);
    int pad_x = 8, pad_y = 5, bw = sz.cx + pad_x * 2, bh = sz.cy + pad_y * 2;
    int x = 14, y = 14;
    if (x + bw > s->w - 10) x = s->w - 10 - bw;
    if (y + bh > s->h - 10) y = s->h - 10 - bh;
    if (x < 10) x = 10; if (y < 10) y = 10;
    if (bw < 8) bw = 8; if (bh < 8) bh = 8;
    g_gl_x = x; g_gl_y = y; g_gl_w = bw; g_gl_h = bh;
    draw_round_rect(&s->C, (float)x, (float)y, (float)bw, (float)bh, 5.f, (RGBf){ 0.06f, 0.07f, 0.09f }, 0.78f);
    draw_round_rect_outline(&s->C, (float)x, (float)y, (float)bw, (float)bh, 5.f, 1.1f, (RGBf){ 0.95f, 0.82f, 0.40f }, 0.7f);
    BITMAPINFO b = bmi_for(bw, bh); void *bits = NULL;
    HBITMAP dib = CreateDIBSection(sdc, &b, DIB_RGB_COLORS, &bits, NULL, 0);
    HDC mdc = CreateCompatibleDC(sdc);
    if (dib && mdc && bits) {
        HGDIOBJ ob = SelectObject(mdc, dib), of2 = SelectObject(mdc, f);
        memset(bits, 0, (size_t)bw * bh * 4);
        SetBkMode(mdc, TRANSPARENT);
        SetTextColor(mdc, RGB(245, 245, 238));
        TextOutW(mdc, pad_x, pad_y, txt, (int)wcslen(txt));
        uint32_t *tp = (uint32_t *)bits;
        for (int yy = 0; yy < bh; yy++) for (int xx = 0; xx < bw; xx++) {
            uint32_t p = tp[(size_t)yy * bw + xx];
            unsigned r = (p >> 16) & 255, g = (p >> 8) & 255, bl = p & 255;
            unsigned cov = r > g ? r : g; if (bl > cov) cov = bl;
            if (!cov) continue;
            int dx = x + xx, dy = y + yy;
            if ((unsigned)dx >= (unsigned)s->w || (unsigned)dy >= (unsigned)s->h) continue;
            guide_over(&s->C.px[(size_t)dy * s->w + dx], r, g, bl, cov);
        }
        SelectObject(mdc, of2); SelectObject(mdc, ob);
    }
    if (mdc) DeleteDC(mdc); if (dib) DeleteObject(dib); ReleaseDC(NULL, sdc);
    if (own) DeleteObject(f);
}
static void guide_paint(void) {
    if (!g_guide || !g_gd.C.px) return;
    int w = g_gd.w, h = g_gd.h;
    canvas_clear(&g_gd.C, (IRect){ 0, 0, w, h });
    if (g_gd.L.cov) memset(g_gd.L.cov, 0, (size_t)w * h * sizeof(float));
    if (g_gd.L.par) memset(g_gd.L.par, 0, (size_t)w * h * sizeof(float));
    float x0 = 3.5f, y0 = 3.5f, x1 = (float)w - 3.5f, y1 = (float)h - 3.5f;
    RGBf dark = { 0.05f, 0.05f, 0.05f }, gold = { 0.95f, 0.80f, 0.32f };
    guide_dash(&g_gd.C, &g_gd.L, x0, y0, x1, y0, 4.0f, dark, 0.45f);
    guide_dash(&g_gd.C, &g_gd.L, x1, y0, x1, y1, 4.0f, dark, 0.45f);
    guide_dash(&g_gd.C, &g_gd.L, x1, y1, x0, y1, 4.0f, dark, 0.45f);
    guide_dash(&g_gd.C, &g_gd.L, x0, y1, x0, y0, 4.0f, dark, 0.45f);
    guide_dash(&g_gd.C, &g_gd.L, x0, y0, x1, y0, 2.2f, gold, 0.85f);
    guide_dash(&g_gd.C, &g_gd.L, x1, y0, x1, y1, 2.2f, gold, 0.85f);
    guide_dash(&g_gd.C, &g_gd.L, x1, y1, x0, y1, 2.2f, gold, 0.85f);
    guide_dash(&g_gd.C, &g_gd.L, x0, y1, x0, y0, 2.2f, gold, 0.85f);
    guide_label(&g_gd);
}
static void guide_present(void) {
    if (!g_guide || !g_gd.mdc) return;
    GdiFlush();
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    POINT src = { 0, 0 }, dst;
    SIZE sz = { g_gd.w, g_gd.h };
    RECT wr; GetWindowRect(g_guide, &wr);
    dst.x = wr.left; dst.y = wr.top;
    UpdateLayeredWindow(g_guide, NULL, &dst, &sz, g_gd.mdc, &src, 0, &bf, ULW_ALPHA);
}
static int guide_hit(int x, int y) {
    int w = g_gd.w, h = g_gd.h;
    if (x < GUIDE_EDGE || y < GUIDE_EDGE || x >= w - GUIDE_EDGE || y >= h - GUIDE_EDGE) return 1;
    if (x >= g_gl_x && x < g_gl_x + g_gl_w && y >= g_gl_y && y < g_gl_y + g_gl_h) return 1;
    return 0;
}
static LRESULT CALLBACK guide_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); return 0; }
    case WM_NCHITTEST: {
        if (!g_move) return HTTRANSPARENT;
        POINT p = { (short)LOWORD(l), (short)HIWORD(l) }; ScreenToClient(h, &p);
        return guide_hit(p.x, p.y) ? HTCLIENT : HTTRANSPARENT;
    }
    case WM_LBUTTONDOWN: {
        if (!g_move || !g_chroma) return 0;
        POINT p; GetCursorPos(&p); RECT r; GetWindowRect(g_chroma, &r);
        g_guide_drag = 1; g_guide_dx = p.x - r.left; g_guide_dy = p.y - r.top;
        SetCapture(h); return 0;
    }
    case WM_MOUSEMOVE:
        if (g_guide_drag && g_chroma) {
            POINT p; GetCursorPos(&p);
            SetWindowPos(g_chroma, NULL, p.x - g_guide_dx, p.y - g_guide_dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            guide_update();
        }
        return 0;
    case WM_LBUTTONUP: case WM_CAPTURECHANGED:
        if (g_guide_drag) { g_guide_drag = 0; ReleaseCapture(); if (g_main) SetTimer(g_main, TIMER_SAVE, 200, NULL); }
        return 0;
    case WM_DESTROY:
        if (h == g_guide) g_guide = NULL;
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}
static void guide_destroy(void) {
    g_guide_flash = 0; g_guide_drag = 0;
    if (g_main) KillTimer(g_main, TIMER_GUIDE);
    if (g_guide) { HWND w = g_guide; g_guide = NULL; DestroyWindow(w); }
    surface_free(&g_gd);
}
static void guide_update(void) {
    if (!guide_wanted()) {
        if (g_guide) ShowWindow(g_guide, SW_HIDE);
        return;
    }
    RECT wr; GetWindowRect(g_chroma, &wr);
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    if (w < 8) w = 8; if (h < 8) h = 8;
    if (!g_guide) {
        DWORD ex = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST;
        if (!g_move) ex |= WS_EX_TRANSPARENT;
        g_guide = CreateWindowExW(ex, L"MAOGuide", L"", WS_POPUP, wr.left, wr.top, w, h, NULL, NULL, g_inst, NULL);
        if (!g_guide) return;
        SetWindowDisplayAffinity(g_guide, WDA_EXCLUDEFROMCAPTURE);
        if (!surface_alloc(&g_gd, w, h, 1)) return;
        SetWindowPos(g_guide, HWND_TOPMOST, wr.left, wr.top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        guide_paint(); guide_present();
        ShowWindow(g_guide, SW_SHOWNOACTIVATE);
        return;
    }
    LONG_PTR ex = GetWindowLongPtrW(g_guide, GWL_EXSTYLE);
    LONG_PTR want = g_move ? (ex & ~(LONG_PTR)WS_EX_TRANSPARENT) : (ex | (LONG_PTR)WS_EX_TRANSPARENT);
    if (want != ex) SetWindowLongPtrW(g_guide, GWL_EXSTYLE, want);
    RECT cur; GetWindowRect(g_guide, &cur);
    int moved = cur.left != wr.left || cur.top != wr.top || cur.right != wr.right || cur.bottom != wr.bottom;
    int hidden = !IsWindowVisible(g_guide);
    if (moved || hidden || g_gd.w != w || g_gd.h != h) {
        if (g_gd.w != w || g_gd.h != h) surface_alloc(&g_gd, w, h, 1);
        SetWindowPos(g_guide, HWND_TOPMOST, wr.left, wr.top, w, h, SWP_NOACTIVATE | (hidden ? SWP_SHOWWINDOW : 0));
        guide_paint(); guide_present();
        if (hidden) ShowWindow(g_guide, SW_SHOWNOACTIVATE);
    } else {
        SetWindowPos(g_guide, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

static int chroma_click_through_wanted(void) {
    return !g_move && S.mode == 2 && S.stream_clear && S.chroma_fit && g_sw_clear;
}
static void chroma_apply_click_through(void) {
    if (!g_chroma) return;
    LONG_PTR ex = GetWindowLongPtrW(g_chroma, GWL_EXSTYLE);
    LONG_PTR want = chroma_click_through_wanted()
        ? (ex | (LONG_PTR)WS_EX_TRANSPARENT)
        : (ex & ~(LONG_PTR)WS_EX_TRANSPARENT);
    if (want == ex) return;
    SetWindowLongPtrW(g_chroma, GWL_EXSTYLE, want);
    SetWindowPos(g_chroma, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}
static void display_destroy(void) {
    guide_destroy();
    if (g_overlay) { DestroyWindow(g_overlay); g_overlay = NULL; }
    sw_free();
    if (g_chroma) { HWND c = g_chroma; g_chroma = NULL; DestroyWindow(c); }
    surface_free(&g_ov); surface_free(&g_ch);
}
static void display_create(void) {
    display_destroy();
    update_monitors();
    g_src = pick_src();
    clear_trail();
    g_force = 1; g_prev_bb = ir_empty();
    if (S.mode == 0) {
        int w = g_virt.right - g_virt.left, h = g_virt.bottom - g_virt.top;
        g_overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                    L"MAOOverlay", APP_NAME L" (overlay)", WS_POPUP, g_virt.left, g_virt.top, w, h, NULL, NULL, g_inst, NULL);
        surface_alloc(&g_ov, w, h, 1);
        g_ov_first = 1;
        if (g_move) SetWindowLongPtrW(g_overlay, GWL_EXSTYLE, GetWindowLongPtrW(g_overlay, GWL_EXSTYLE) & ~(LONG_PTR)WS_EX_TRANSPARENT);
        if (!g_hidden) {
            ShowWindow(g_overlay, SW_SHOWNOACTIVATE);
            SetWindowPos(g_overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
    } else {
        /* same size as the screen by default, so OBS gets it 1:1 and never has to enlarge it */
        if (S.chroma_fit) { S.chroma_w = g_src.right - g_src.left; S.chroma_h = g_src.bottom - g_src.top; }
        DWORD st = WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX; /* no frame or title: the capture is exactly the drawing */
        int want_clear = (S.mode == 2 && S.stream_clear);
        for (int pass = 0; pass < 2; pass++) {
            int clear = want_clear && pass == 0;
            DWORD ex = S.chroma_topmost ? WS_EX_TOPMOST : 0;
            if (S.mode == 2) ex |= WS_EX_APPWINDOW;
            if (clear) {
                ex |= WS_EX_NOREDIRECTIONBITMAP;
                if (S.chroma_fit && !g_move) ex |= WS_EX_TRANSPARENT;
            }
            RECT r = { 0, 0, S.chroma_w, S.chroma_h };
            AdjustWindowRectEx(&r, st, FALSE, ex);
            if (g_chroma) { HWND old = g_chroma; g_chroma = NULL; DestroyWindow(old); }
            g_chroma = CreateWindowExW(ex, L"MAOChroma", S.mode == 2 ? APP_NAME L" (stream window)" : APP_NAME L" (green screen)", st,
                                       CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, g_inst, NULL);
            if (!g_chroma) continue;
            { DWORD none = 0xFFFFFFFE, sq = 1; /* no coloured border, square corners */
              DwmSetWindowAttribute(g_chroma, 34 /* border colour */, &none, sizeof(none));
              DwmSetWindowAttribute(g_chroma, 33 /* corner style */, &sq, sizeof(sq)); }
            chroma_place(1);
            ShowWindow(g_chroma, SW_SHOWNOACTIVATE);
            chroma_place(1);
            RECT c; GetClientRect(g_chroma, &c);
            surface_alloc(&g_ch, c.right, c.bottom, 0);
            if (clear) {
                if (c.right > 0 && c.bottom > 0 && sw_init(g_chroma, c.right, c.bottom) && g_sw_clear) break;
                sw_free();
                continue; /* try again as a normal black window */
            }
            break;
        }
        if (g_set) settings_status();
        chroma_apply_click_through();
        g_guide_quiet = GetTickCount() + 800;
    }
    guide_update();
}

/* Resolution / scaling / monitor changes (e.g. a game switching to exclusive fullscreen at another
   resolution): re-read the desktop bounds, move + resize the overlay, rebuild its bitmap, drop stale
   trail points. Called from the display messages and once a second as a fallback. */
static void clear_trail(void) {
    EnterCriticalSection(&g_lock);
    g_tr_n = 0; g_ck_n = 0;
    LeaveCriticalSection(&g_lock);
}
static int display_check(int force) {
    RECT ov = g_virt, op = g_prim, os = g_src;
    update_monitors();
    g_src = pick_src();
    int changed = force || !EqualRect(&ov, &g_virt) || !EqualRect(&op, &g_prim) || (S.mode != 0 && !EqualRect(&os, &g_src));
    int w = g_virt.right - g_virt.left, h = g_virt.bottom - g_virt.top;
    if (g_overlay) {
        RECT wr;
        if (GetWindowRect(g_overlay, &wr) && (wr.left != g_virt.left || wr.top != g_virt.top || wr.right - wr.left != w || wr.bottom - wr.top != h)) changed = 1;
        if (g_ov.w != w || g_ov.h != h) changed = 1;
    }
    if (!changed) return 0;
    if (g_overlay) {
        SetWindowPos(g_overlay, HWND_TOPMOST, g_virt.left, g_virt.top, w, h, SWP_NOACTIVATE);
        if (g_ov.w != w || g_ov.h != h || !g_ov.C.px) surface_alloc(&g_ov, w, h, 1);
        else canvas_clear(&g_ov.C, (IRect){ 0, 0, g_ov.w, g_ov.h });
        g_ov_first = 1;
    }
    chroma_fit_size(); /* a game changed the screen size: keep the capture 1:1 */
    g_prev_bb = ir_empty();
    g_ov_lay.valid = g_ch_lay.valid = g_ov_klay.valid = g_ch_klay.valid = g_ov_mlay.valid = g_ch_mlay.valid = 0;
    clear_trail();
    g_force = 1;
    return 1;
}

/* live key / button state for the on-screen keys: copied each frame, drawn, never stored or sent */
static unsigned char g_kdisp[256]; static int g_mdisp[5], g_wdisp; static unsigned g_kdisp_sig, g_kdisp_last;
static float g_klev[256], g_mlev[5];
static float release_level(int held, double rel, double now) {
    if (held) return 1.0f;
    double t = (now - rel) / 0.14; /* short ease-out after letting go */
    if (t >= 1 || t < 0) return 0.0f;
    float u = 1.0f - (float)t; return u * u;
}
static void keys_snapshot(double now) {
    for (int i = 0; i < 256; i++) g_kdisp[i] = g_kdown[i];
    g_kdisp[0x10] |= g_kdown[0xA0] | g_kdown[0xA1]; g_kdisp[0x11] |= g_kdown[0xA2] | g_kdown[0xA3]; g_kdisp[0x12] |= g_kdown[0xA4] | g_kdown[0xA5];
    for (int i = 0; i < 5; i++) g_mdisp[i] = g_mbtn[i];
    g_wdisp = now - g_wheel_t < 0.15 ? g_wheel_dir : 0;
    for (int i = 0; i < 256; i++) g_klev[i] = release_level(g_kdown[i], g_krel[i], now);
    static const int gen[3][3] = { { 0x10, 0xA0, 0xA1 }, { 0x11, 0xA2, 0xA3 }, { 0x12, 0xA4, 0xA5 } };
    for (int j = 0; j < 3; j++) g_klev[gen[j][0]] = fmaxf(g_klev[gen[j][0]], fmaxf(g_klev[gen[j][1]], g_klev[gen[j][2]]));
    for (int i = 0; i < 5; i++) g_mlev[i] = release_level(g_mbtn[i], g_mrel[i], now);
    g_kdisp[0x05] = (unsigned char)g_mbtn[3]; g_kdisp[0x06] = (unsigned char)g_mbtn[4]; /* mouse side buttons as keys (M4, M5) */
    g_klev[0x05] = g_mlev[3]; g_klev[0x06] = g_mlev[4];
    unsigned h = 2166136261u;
    for (int i = 0; i < 256; i++) h = (h ^ g_kdisp[i]) * 16777619u;
    for (int i = 0; i < 5; i++) h = (h ^ (unsigned)g_mdisp[i]) * 16777619u;
    for (int i = 0; i < 256; i++) h = (h ^ (unsigned)(g_klev[i] * 16)) * 16777619u;
    for (int i = 0; i < 5; i++) h = (h ^ (unsigned)(g_mlev[i] * 16)) * 16777619u;
    g_kdisp_sig = (h ^ (unsigned)(g_wdisp + 2)) * 16777619u;
}
static void build_scene(Scene *sc, double now, float scale, float offx, float offy, RECT area_src) {
    memset(sc, 0, sizeof(*sc));
    sc->trail = s_tr; sc->ntrail = s_ntr; sc->clicks = s_ck; sc->nclicks = s_nck;
    sc->apm_avg = g_apm_avg; sc->now = now;
    sc->scale = scale; sc->offx = offx; sc->offy = offy;
    sc->area_x = area_src.left * scale + offx; sc->area_y = area_src.top * scale + offy;
    sc->area_w = (area_src.right - area_src.left) * scale; sc->area_h = (area_src.bottom - area_src.top) * scale;
    sc->kdown = g_kdisp; for (int i = 0; i < 5; i++) { sc->mdown[i] = g_mdisp[i]; sc->mlevel[i] = g_mlev[i]; } sc->wheel = g_wdisp;
    sc->klevel = g_klev; sc->mlevel_set = 1;
}

/* "Alignment check": text lines describing exactly how the cursor is being mapped */
static char g_al[8][96]; static const char *g_alp[8];
static int align_lines(POINT cur, POINT drawn, RECT win) {
    POINT lg; GetCursorPos(&lg);
    HMONITOR hm = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi; memset(&mi, 0, sizeof(mi)); mi.cbSize = sizeof(mi); GetMonitorInfoW(hm, &mi);
    UINT dx = 0, dy = 0; if (!p_GetDpiForMonitor || FAILED(p_GetDpiForMonitor(hm, 0, &dx, &dy))) dx = 0;
    g_dpi_cur = dx ? (int)dx : 96;
    RECT m = mi.rcMonitor;
    snprintf(g_al[0], 96, "cursor %ld, %ld", cur.x, cur.y);
    snprintf(g_al[1], 96, "drawn at %ld, %ld", drawn.x, drawn.y);
    snprintf(g_al[2], 96, "windows reports %ld, %ld", lg.x, lg.y);
    snprintf(g_al[3], 96, "overlay %ld, %ld  size %ld x %ld", win.left, win.top, win.right - win.left, win.bottom - win.top);
    if (dx) snprintf(g_al[4], 96, "monitor %ld x %ld at %ld, %ld  dpi %u (%u%%)", m.right - m.left, m.bottom - m.top, m.left, m.top, dx, dx * 100 / 96);
    else snprintf(g_al[4], 96, "monitor %ld x %ld at %ld, %ld", m.right - m.left, m.bottom - m.top, m.left, m.top);
    snprintf(g_al[5], 96, "scaling mode %s", awareness_name());
    snprintf(g_al[6], 96, "fix %+.0f, %+.0f px  %.1f%%, %.1f%%", (double)S.fix_dx, (double)S.fix_dy, (double)S.fix_sx * 100, (double)S.fix_sy * 100);
    for (int i = 0; i < 7; i++) g_alp[i] = g_al[i];
    return 7;
}
static POINT corrected(POINT p) {
    POINT q = { lroundf(p.x * S.fix_sx + S.fix_dx), lroundf(p.y * S.fix_sy + S.fix_dy) };
    return q;
}

static void render_overlay(double now, int apm, int is_static) {
    if (!g_overlay || !g_ov.C.px) return;
    /* every frame: the overlay must exactly cover the current virtual screen, and its bitmap must match the window */
    RECT wr;
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vx != g_virt.left || vy != g_virt.top || vw != g_virt.right - g_virt.left || vh != g_virt.bottom - g_virt.top) display_check(1);
    else if (GetWindowRect(g_overlay, &wr) && (wr.left != g_virt.left || wr.top != g_virt.top || wr.right - wr.left != g_ov.w || wr.bottom - wr.top != g_ov.h))
        display_check(1);
    else if (g_ov.w != vw || g_ov.h != vh) display_check(1);
    if (!g_force && is_static && g_last_static && apm == g_last_apm && !g_ov_first) return;
    Scene sc; build_scene(&sc, now, 1.f, (float)-g_virt.left, (float)-g_virt.top, g_prim);
    sc.apm = apm;
    IRect clr = g_prev_bb;
    canvas_clear(&g_ov.C, clr);
    Style st = frame_style(0);
    capture_lay(&g_ov_lay, &st, &sc); capture_klay(&g_ov_klay, &st, &sc); capture_mlay(&g_ov_mlay, &st, &sc);
    IRect bb = ((g_hidden || g_idle) && !g_move) ? ir_empty() : render_scene(&g_ov.C, &g_ov.L, &st, &sc);
    if (S.align_check && !g_hidden) {
        POINT cur; cursor_pos(&cur); POINT d = corrected(cur);
        int n = align_lines(cur, d, g_virt);
        bb = ir_union(bb, render_align(&g_ov.C, &g_ov.L, d.x - g_virt.left, d.y - g_virt.top, g_alp, n, (float)g_dpi_cur / 96.0f, 0));
    }
    IRect dirty = ir_union(clr, bb);
    g_prev_bb = bb;
    if (ir_is_empty(dirty) && !g_ov_first) return;
    GdiFlush();
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    POINT src = { 0, 0 }, dst = { g_virt.left, g_virt.top };
    SIZE sz = { g_ov.w, g_ov.h };
    RECT dr = { dirty.x0, dirty.y0, dirty.x1, dirty.y1 };
    UPDATELAYEREDWINDOWINFO info; memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(info); info.pptDst = &dst; info.psize = &sz; info.hdcSrc = g_ov.mdc; info.pptSrc = &src;
    info.pblend = &bf; info.dwFlags = ULW_ALPHA; info.prcDirty = g_ov_first ? NULL : &dr;
    if (!UpdateLayeredWindowIndirect(g_overlay, &info)) {
        info.prcDirty = NULL; UpdateLayeredWindowIndirect(g_overlay, &info);
    }
    g_ov_first = 0;
}

static uint32_t rgbf_px(RGBf c) { COLORREF r = to_cr(c); return ((uint32_t)GetRValue(r) << 16) | ((uint32_t)GetGValue(r) << 8) | GetBValue(r); }

/* stream window: a DirectX 11 swap chain. Its pixels keep their transparency, so OBS / Streamlabs
   Game Capture with "Allow Transparency" shows only the drawing, with no key colour at all. */
static ID3D11Device *g_dx; static ID3D11DeviceContext *g_dxc; static IDXGISwapChain *g_sw; static ID3D11Texture2D *g_swtex;
static int g_sw_w, g_sw_h, g_sw_failed; static double g_sw_last;
static void settings_status(void);
static uint32_t *g_sw_copy; /* what the stream texture holds now, so only changed rows are sent */
/* DirectComposition is loaded at runtime so a missing dcomp.dll just uses the black window */
typedef struct IDCompUnk IDCompUnk;
typedef struct IDCompUnkVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDCompUnk *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(IDCompUnk *);
    ULONG (STDMETHODCALLTYPE *Release)(IDCompUnk *);
    HRESULT (STDMETHODCALLTYPE *Commit)(IDCompUnk *);
    HRESULT (STDMETHODCALLTYPE *WaitForCommitCompletion)(IDCompUnk *);
    HRESULT (STDMETHODCALLTYPE *GetFrameStatistics)(IDCompUnk *, void *);
    HRESULT (STDMETHODCALLTYPE *CreateTargetForHwnd)(IDCompUnk *, HWND, BOOL, IDCompUnk **);
    HRESULT (STDMETHODCALLTYPE *CreateVisual)(IDCompUnk *, IDCompUnk **);
} IDCompUnkVtbl;
struct IDCompUnk { IDCompUnkVtbl *lpVtbl; };
typedef HRESULT (STDMETHODCALLTYPE *PFN_IDCV_SetContent)(IDCompUnk *, IUnknown *);
typedef HRESULT (STDMETHODCALLTYPE *PFN_IDCT_SetRoot)(IDCompUnk *, IDCompUnk *);
typedef HRESULT (WINAPI *PFN_DCompCreate)(IUnknown *, REFIID, void **);
static IDCompUnk *g_dcd, *g_dct, *g_dcv;
static const IID IID_IDCompositionDevice = { 0xc37ea93a, 0xe7aa, 0x450d, { 0xb1, 0x6f, 0x97, 0x46, 0xcb, 0x04, 0x07, 0xf3 } };
#ifndef DXGI_ALPHA_MODE_PREMULTIPLIED
#define DXGI_ALPHA_MODE_PREMULTIPLIED 1
#endif
static void dcomp_free(void) {
    if (g_dcv) { g_dcv->lpVtbl->Release(g_dcv); g_dcv = NULL; }
    if (g_dct) { g_dct->lpVtbl->Release(g_dct); g_dct = NULL; }
    if (g_dcd) { g_dcd->lpVtbl->Release(g_dcd); g_dcd = NULL; }
}
static void sw_free(void) {
    dcomp_free();
    g_sw_clear = 0;
    free(g_sw_copy); g_sw_copy = NULL;
    if (g_swtex) ID3D11Texture2D_Release(g_swtex);
    if (g_sw) IDXGISwapChain_Release(g_sw);
    if (g_dxc) ID3D11DeviceContext_Release(g_dxc);
    if (g_dx) ID3D11Device_Release(g_dx);
    g_swtex = NULL; g_sw = NULL; g_dxc = NULL; g_dx = NULL; g_sw_w = g_sw_h = 0;
}
static int sw_assoc(HWND h) {
    IDXGIFactory *f = NULL; IDXGIDevice *dd = NULL; IDXGIAdapter *ad = NULL;
    if (SUCCEEDED(ID3D11Device_QueryInterface(g_dx, &IID_IDXGIDevice, (void **)&dd))) {
        if (SUCCEEDED(IDXGIDevice_GetAdapter(dd, &ad))) {
            if (SUCCEEDED(IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory, (void **)&f))) { IDXGIFactory_MakeWindowAssociation(f, h, DXGI_MWA_NO_ALT_ENTER); IDXGIFactory_Release(f); }
            IDXGIAdapter_Release(ad);
        }
        IDXGIDevice_Release(dd);
    }
    return 1;
}
static int sw_make_tex(int w, int ht) {
    D3D11_TEXTURE2D_DESC td; memset(&td, 0, sizeof td);
    td.Width = (UINT)w; td.Height = (UINT)ht; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    return SUCCEEDED(ID3D11Device_CreateTexture2D(g_dx, &td, NULL, &g_swtex));
}
static int sw_init_dcomp(HWND h, int w, int ht) {
    static PFN_DCompCreate p_create;
    static int tried;
    if (!tried) {
        tried = 1;
        HMODULE m = LoadLibraryW(L"dcomp.dll");
        if (m) p_create = (PFN_DCompCreate)GetProcAddress(m, "DCompositionCreateDevice");
    }
    if (!p_create) return 0;
    static const D3D_DRIVER_TYPE drv[2] = { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP };
    for (int d = 0; d < 2; d++) {
        if (FAILED(D3D11CreateDevice(NULL, drv[d], NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION, &g_dx, NULL, &g_dxc))) {
            sw_free(); continue;
        }
        IDXGIDevice *dd = NULL; IDXGIAdapter *ad = NULL; IDXGIFactory2 *f2 = NULL; IDXGISwapChain1 *sc1 = NULL;
        int ok = 0;
        if (SUCCEEDED(ID3D11Device_QueryInterface(g_dx, &IID_IDXGIDevice, (void **)&dd)) &&
            SUCCEEDED(p_create((IUnknown *)dd, &IID_IDCompositionDevice, (void **)&g_dcd)) &&
            SUCCEEDED(IDXGIDevice_GetAdapter(dd, &ad)) &&
            SUCCEEDED(IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory2, (void **)&f2))) {
            DXGI_SWAP_CHAIN_DESC1 sd; memset(&sd, 0, sizeof sd);
            sd.Width = (UINT)w; sd.Height = (UINT)ht; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
            sd.Scaling = DXGI_SCALING_STRETCH; sd.SwapEffect = (DXGI_SWAP_EFFECT)4;
            sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
            if (SUCCEEDED(IDXGIFactory2_CreateSwapChainForComposition(f2, (IUnknown *)g_dx, &sd, NULL, &sc1)) &&
                sw_make_tex(w, ht) &&
                SUCCEEDED(g_dcd->lpVtbl->CreateTargetForHwnd(g_dcd, h, TRUE, &g_dct)) &&
                SUCCEEDED(g_dcd->lpVtbl->CreateVisual(g_dcd, &g_dcv))) {
                /* SetRoot is vtable slot 3 on the target; SetContent is slot 15 on the visual */
                PFN_IDCT_SetRoot set_root = ((PFN_IDCT_SetRoot *)g_dct->lpVtbl)[3];
                PFN_IDCV_SetContent set_content = ((PFN_IDCV_SetContent *)g_dcv->lpVtbl)[15];
                if (SUCCEEDED(set_content(g_dcv, (IUnknown *)sc1)) && SUCCEEDED(set_root(g_dct, g_dcv)) &&
                    SUCCEEDED(g_dcd->lpVtbl->Commit(g_dcd))) {
                    g_sw = (IDXGISwapChain *)sc1; sc1 = NULL;
                    IDXGIFactory2_MakeWindowAssociation(f2, h, DXGI_MWA_NO_ALT_ENTER);
                    g_sw_w = w; g_sw_h = ht; g_sw_failed = 0; g_sw_clear = 1; ok = 1;
                }
            }
        }
        if (sc1) IDXGISwapChain1_Release(sc1);
        if (f2) IDXGIFactory2_Release(f2);
        if (ad) IDXGIAdapter_Release(ad);
        if (dd) IDXGIDevice_Release(dd);
        if (ok) return 1;
        sw_free();
    }
    return 0;
}
static int sw_init(HWND h, int w, int ht) {
    sw_free();
    if (!h || w < 1 || ht < 1) return 0;
    if (S.mode == 2 && S.stream_clear && sw_init_dcomp(h, w, ht)) return 1;
    static const DXGI_SWAP_EFFECT fx[2] = { (DXGI_SWAP_EFFECT)4 /* flip discard */, DXGI_SWAP_EFFECT_DISCARD };
    static const D3D_DRIVER_TYPE drv[2] = { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP };
    for (int d = 0; d < 2; d++)
        for (int e = 0; e < 2; e++) {
            DXGI_SWAP_CHAIN_DESC sd; memset(&sd, 0, sizeof sd);
            sd.BufferDesc.Width = (UINT)w; sd.BufferDesc.Height = (UINT)ht; sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = e ? 1 : 2;
            sd.OutputWindow = h; sd.Windowed = TRUE; sd.SwapEffect = fx[e];
            if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(NULL, drv[d], NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION, &sd, &g_sw, &g_dx, NULL, &g_dxc))) {
                if (sw_make_tex(w, ht)) {
                    sw_assoc(h);
                    g_sw_w = w; g_sw_h = ht; g_sw_failed = 0; g_sw_clear = 0; return 1;
                }
            }
            sw_free();
        }
    g_sw_failed = 1;
    if (g_set) settings_status();
    return 0;
}
static void sw_present(HWND h, const uint32_t *px, int w, int ht, int upload) {
    if (!g_sw || w != g_sw_w || ht != g_sw_h) { if (!sw_init(h, w, ht)) return; upload = 1; }
    if (upload) {
        size_t row = (size_t)w * 4;
        int y0 = 0, y1 = ht;
        if (g_sw_copy) {
            while (y0 < ht && !memcmp(g_sw_copy + (size_t)y0 * w, px + (size_t)y0 * w, row)) y0++;
            while (y1 > y0 && !memcmp(g_sw_copy + (size_t)(y1 - 1) * w, px + (size_t)(y1 - 1) * w, row)) y1--;
        } else g_sw_copy = (uint32_t *)malloc(row * ht);
        if (y1 > y0) {
            D3D11_BOX box = { 0, (UINT)y0, 0, (UINT)w, (UINT)y1, 1 };
            ID3D11DeviceContext_UpdateSubresource(g_dxc, (ID3D11Resource *)g_swtex, 0, &box, px + (size_t)y0 * w, (UINT)row, 0);
            if (g_sw_copy) memcpy(g_sw_copy + (size_t)y0 * w, px + (size_t)y0 * w, row * (y1 - y0));
        }
    }
    ID3D11Texture2D *bb = NULL;
    if (SUCCEEDED(IDXGISwapChain_GetBuffer(g_sw, 0, &IID_ID3D11Texture2D, (void **)&bb))) {
        ID3D11DeviceContext_CopyResource(g_dxc, (ID3D11Resource *)bb, (ID3D11Resource *)g_swtex);
        ID3D11Texture2D_Release(bb);
    }
    HRESULT hr = IDXGISwapChain_Present(g_sw, 0, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) sw_free(); /* rebuilt on the next frame */
    g_sw_last = now_s();
}

/* "Follow League client": while the client is open (and no match running) only its window is mapped */
static RECT g_map; static int g_map_client, g_map_game;
static void map_update(double now) {
    static double last; if (now - last < 0.1 && !g_force) return; last = now; /* window lookups 10 times a second */
    RECT r = { 0, 0, 0, 0 }; HWND game = league_game();
    int cl = !game && league_client(&r);
    g_map_game = game != NULL;
    if (cl != g_map_client || (cl && !EqualRect(&r, &g_map))) {
        g_map_client = cl; g_map = r; clear_trail(); snapshot(now); g_force = 1;
    }
    RECT ns = pick_src(); /* the game started or the client moved to another screen */
    if (!EqualRect(&ns, &g_src)) { g_src = ns; clear_trail(); snapshot(now); g_force = 1; chroma_fit_size(); }
}
static void render_chroma(double now, int apm, int is_static) {
    if (!g_chroma || !g_ch.C.px || IsIconic(g_chroma)) return;
    if (!g_force && is_static && g_last_static && apm == g_last_apm) {
        /* nothing new: still show a frame now and then so a capture that starts later picks it up */
        if (S.mode == 2 && g_ch.out && now - g_sw_last > 0.5) sw_present(g_chroma, g_ch.out, g_ch.w, g_ch.h, 0);
        return;
    }
    if (S.chroma_screen == 3) { /* follow mouse: switch screens when the mouse does */
        RECT ns = pick_src();
        if (!EqualRect(&ns, &g_src)) { g_src = ns; clear_trail(); snapshot(now); g_force = 1; chroma_fit_size(); }
    }
    if (S.chroma_screen == 5) map_update(now);
    RECT src = g_src;
    float sw = (float)(src.right - src.left), sh = (float)(src.bottom - src.top);
    float k = fminf(g_ch.w / sw, g_ch.h / sh);
    float offx = (g_ch.w - sw * k) * 0.5f - src.left * k, offy = (g_ch.h - sh * k) * 0.5f - src.top * k;
    float mx = 1, my = 1; /* League client: its window is stretched over the whole green window */
    int client = S.chroma_screen == 5 && g_map_client;
    if (client) {
        RECT m = g_map; mx = g_ch.w / (float)(m.right - m.left); my = g_ch.h / (float)(m.bottom - m.top);
        for (int i = 0; i < s_ntr; i++) { s_tr[i].x = (s_tr[i].x - m.left) * mx; s_tr[i].y = (s_tr[i].y - m.top) * my; }
        for (int i = 0; i < s_nck; i++) { s_ck[i].x = (s_ck[i].x - m.left) * mx; s_ck[i].y = (s_ck[i].y - m.top) * my; }
        k = 1; offx = offy = 0; src = (RECT){ 0, 0, g_ch.w, g_ch.h }; /* outside the client: off the window, not drawn */
    }
    Scene sc; build_scene(&sc, now, k, offx, offy, src);
    sc.apm = apm;
    Style st = frame_style(S.mode == 1);
    capture_lay(&g_ch_lay, &st, &sc); capture_klay(&g_ch_klay, &st, &sc); capture_mlay(&g_ch_mlay, &st, &sc);
    canvas_clear(&g_ch.C, (IRect){ 0, 0, g_ch.w, g_ch.h });
    if ((!g_hidden && !g_idle) || g_move) render_scene(&g_ch.C, &g_ch.L, &st, &sc);
    if (S.align_check && !g_hidden) {
        POINT cur; cursor_pos(&cur); POINT d = corrected(cur);
        int n = align_lines(cur, d, g_virt);
        if (client) {
            snprintf(g_al[3], 96, "shows League client %ld x %ld at %ld, %ld", g_map.right - g_map.left, g_map.bottom - g_map.top, g_map.left, g_map.top);
            d.x = lroundf((d.x - g_map.left) * mx); d.y = lroundf((d.y - g_map.top) * my);
            snprintf(g_al[n], 96, "window inside %d x %d  stretch %.3f, %.3f", g_ch.w, g_ch.h, (double)mx, (double)my); g_alp[n] = g_al[n]; n++;
        } else {
            snprintf(g_al[3], 96, "shows %s %ld x %ld at %ld, %ld", S.chroma_screen == 5 ? (g_map_game ? "game screen" : "screen (no League client)") : "screen",
                     src.right - src.left, src.bottom - src.top, src.left, src.top);
            snprintf(g_al[n], 96, "window inside %d x %d  scale %.3f", g_ch.w, g_ch.h, (double)k); g_alp[n] = g_al[n]; n++;
            d.x = (LONG)floorf(d.x * k + offx); d.y = (LONG)floorf(d.y * k + offy);
        }
        render_align(&g_ch.C, &g_ch.L, (int)d.x, (int)d.y, g_alp, n, 1.0f, 1);
    }
    if (S.chroma_marks) render_marks(&g_ch.C, &g_ch.L, src.left * k + offx, src.top * k + offy, sw * k, sh * k, screen_factor());
    if (S.mode == 2) { compose(&g_ch, 0, NULL, 2); sw_present(g_chroma, g_ch.out, g_ch.w, g_ch.h, 1); return; }
    compose(&g_ch, rgbf_px(S.chroma), NULL, S.chroma_hard);
    present(g_chroma, &g_ch);
}

/* settings live preview: same renderer, demo motion, true pixel size */
static void preview_bg(Surface *s) {
    free(s->bg);
    s->bg = (uint32_t *)malloc((size_t)s->w * s->h * 4);
    int step = MulDiv(32, g_dpi, 96);
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++) {
            float t = (float)y / s->h;
            int r = (int)(22 - 4 * t), g = (int)(25 - 4 * t), b = (int)(31 - 4 * t);
            if (x % step == 0 || y % step == 0) { r += 4; g += 4; b += 5; }
            s->bg[(size_t)y * s->w + x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
}
static void round_corners(Surface *s, float r, uint32_t bg) {
    int n = (int)ceilf(r);
    for (int y = 0; y < n && y < s->h; y++)
        for (int x = 0; x < n && x < s->w; x++) {
            float dx = r - (x + 0.5f), dy = r - (y + 0.5f), d = sqrtf(dx * dx + dy * dy);
            float c = r - d + 0.5f; c = c < 0 ? 0 : (c > 1 ? 1 : c);
            if (c >= 1) continue;
            int xs[2] = { x, s->w - 1 - x }, ys[2] = { y, s->h - 1 - y };
            for (int i = 0; i < 2; i++) for (int j = 0; j < 2; j++) {
                uint32_t *p = s->out + (size_t)ys[j] * s->w + xs[i], v = *p, o = 0;
                for (int sh = 0; sh < 24; sh += 8) {
                    float a = (float)((v >> sh) & 255), b = (float)((bg >> sh) & 255);
                    o |= (uint32_t)(b + (a - b) * c + 0.5f) << sh;
                }
                *p = o;
            }
        }
}
static TrailPt p_tr[4096]; static ClickEv p_ck[128];
enum { PG_TRAIL, PG_CLICKS, PG_TEXT, PG_BOX, PG_KEYS, PG_GLOW, PG_GENERAL, PG_N };
static int g_cur_page;
static void render_preview(double now, int apm) {
    if (!g_preview || !g_pv.C.px) return;
    Scene sc; memset(&sc, 0, sizeof(sc));
    float area_h = g_pv.h - 0.f;
    sc.ntrail = demo_trail(now, (float)g_pv.w, area_h, S.st.trail_len_s, p_tr, 4096);
    sc.trail = p_tr;
    sc.nclicks = demo_clicks(now, (float)g_pv.w, area_h, S.st.click_fade_s, p_ck, 128);
    sc.clicks = p_ck;
    sc.apm = apm; sc.apm_avg = g_apm_avg; sc.now = now; sc.scale = 1.f;
    sc.area_w = (float)g_pv.w; sc.area_h = (float)g_pv.h;
    Style st = S.st;
    style_apply_bg(&st, &S);
    if (st.apm_off_x > 14) st.apm_off_x = 14;
    if (st.apm_off_y > 14) st.apm_off_y = 14;
    if (S.mode == 1) { st.panel_opacity = 1.0f; st.solid = 1; }
    static unsigned char demo_k[256];
    if (g_cur_page == PG_KEYS) {
        /* keys page: show only the keys display, with a few demo presses, fitted to the preview */
        st.trail_enabled = 0; st.click_fade_s = 0; st.apm_enabled = 0; st.cursor_dot = 0; sc.nclicks = 0; sc.ntrail = 0;
        if (!st.keys_kb && !st.keys_mouse) st.keys_kb = st.keys_mouse = 1;
        static const unsigned char seq[] = { 'Q', 'W', 'E', 'R', 'D', 'F', '1', 0x20, 'A', 0x10 };
        int step = (int)(now / 0.4) % (int)sizeof(seq);
        memset(demo_k, 0, sizeof(demo_k)); demo_k[seq[step]] = 1;
        sc.kdown = demo_k; sc.mdown[0] = step % 3 == 0; sc.mdown[1] = step % 5 == 2; sc.wheel = step == 7 ? 1 : 0;
        st.keys_corner = APM_TL; st.keys_off_x = st.keys_off_y = 0; st.keys_highlight = 0; st.mouse_sep = 0;
        float x, y, w, h; keys_panel_rect(&st, &sc, &x, &y, &w, &h);
        float fit = fminf(1.0f, fminf((g_pv.w - 32.0f) / fmaxf(w, 1), (g_pv.h - 32.0f) / fmaxf(h, 1)));
        st.keys_scale *= fit; keys_panel_rect(&st, &sc, &x, &y, &w, &h);
        st.keys_off_x = floorf((g_pv.w - w) * 0.5f); st.keys_off_y = floorf((g_pv.h - h) * 0.5f);
    } else st.keys_kb = st.keys_mouse = 0;
    canvas_clear(&g_pv.C, (IRect){ 0, 0, g_pv.w, g_pv.h });
    render_scene(&g_pv.C, &g_pv.L, &st, &sc);
    if (S.mode == 1) compose(&g_pv, rgbf_px(S.chroma), NULL, S.chroma_hard); else compose(&g_pv, 0, g_pv.bg, 0);
    round_corners(&g_pv, 8.0f * (float)g_dpi / 96.0f, 0x0F1115);
    present(g_preview, &g_pv);
}

static void frame(void) {
    double now = now_s();
    g_last_frame = now;
    POINT p;
    cursor_pos(&p); push_point(p, now);
    snapshot(now);
    keys_snapshot(now);
    int apm = apm_now(now);
    /* input is read every refresh, but drawing is capped (60 a second by default): stream video is
       60 fps, and it keeps CPU use low while the mouse moves all game long */
    static double last_draw;
    if (!g_force && !g_drag && S.max_draw > 0 && now - last_draw < 1.0 / S.max_draw - 0.0005) return;
    last_draw = now;
    int idle = S.idle_hide_s > 0 && now - g_last_input > S.idle_hide_s;
    if (idle != g_idle) { g_idle = idle; g_force = 1; }
    int live_trail = s_ntr >= 2 && now - s_tr[s_ntr - 1].t < S.st.trail_len_s + 0.05;
    static int last_avg = -1;
    int is_static = !live_trail && s_nck == 0 && !(S.st.apm_show_avg && g_apm_avg != last_avg) && !g_drag && !S.align_check
                    && !((S.st.keys_kb || S.st.keys_mouse) && g_kdisp_sig != g_kdisp_last);
    g_kdisp_last = g_kdisp_sig;
    last_avg = g_apm_avg;
    render_overlay(now, apm, is_static);
    render_chroma(now, apm, is_static);
    if (g_set && IsWindowVisible(g_set) && !IsIconic(g_set)) render_preview(now, apm);
    g_force = 0; g_last_static = is_static; g_last_apm = apm;
}

/* ------------------------------------------------------------------ APM fonts */
static const wchar_t *const font_faces[] = { NULL, L"Segoe UI", L"Bahnschrift", L"Consolas", L"Arial", L"Impact" };
typedef struct { char s[100]; int hq, bold, font; TextMask m; unsigned char *buf; unsigned use; } TxCache;
static TxCache g_tx[96]; static unsigned g_tx_clock;
static int gdi_text_raster(const char *str, float cap_h, int bold, int font, TextMask *out) {
    int italic = (font >> 8) & 1, face = font & 0xFF;
    int narrow = (font >> 9) & 1, lean = (font >> 10) & 1; /* slanted keys: 82% wide letters leaning like the keys */
    const float NX = narrow ? 0.82f : 1.0f, LX = lean ? 0.194f : 0.0f;
    if (lean) italic = 0;
    if (face < 1 || face > 5 || cap_h < 2 || strlen(str) >= sizeof(g_tx[0].s)) return 0;
    int hq = (int)lroundf(cap_h * 4);
    TxCache *e = NULL, *old = &g_tx[0];
    for (int i = 0; i < 96; i++) {
        TxCache *c = &g_tx[i];
        if (c->buf && c->hq == hq && c->bold == bold && c->font == font && !strcmp(c->s, str)) { e = c; break; }
        if (c->use < old->use) old = c;
    }
    if (e) { e->use = ++g_tx_clock; *out = e->m; return 1; }
    wchar_t ws[100]; int n = 0; for (; str[n]; n++) ws[n] = (wchar_t)(unsigned char)str[n]; ws[n] = 0;
    /* drawn at 4x size and averaged down 4x4: smooth, even edges at every size (no hinting steps) */
    enum { SS = 4 };
    float ch4 = cap_h * SS;
    HDC dc = CreateCompatibleDC(NULL);
    HFONT f = CreateFontW(-(int)lroundf(ch4 * 1.42f), 0, 0, 0, bold ? FW_BOLD : FW_SEMIBOLD, (DWORD)italic, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, 0,
                          ANTIALIASED_QUALITY, 0, font_faces[face]);
    HGDIOBJ of = SelectObject(dc, f);
    TEXTMETRICW tm; GetTextMetricsW(dc, &tm);
    /* scale so the real cap height matches cap_h (fonts differ a lot here) */
    GLYPHMETRICS gm; MAT2 mat = { { 0, 1 }, { 0, 0 }, { 0, 0 }, { 0, 1 } };
    if (GetGlyphOutlineW(dc, L'0', GGO_METRICS, &gm, 0, NULL, &mat) != GDI_ERROR && gm.gmBlackBoxY > 0) {
        float k = ch4 / (float)gm.gmBlackBoxY;
        if (k < 0.95f || k > 1.05f) {
            SelectObject(dc, of); DeleteObject(f);
            f = CreateFontW(-(int)lroundf(ch4 * 1.42f * k), 0, 0, 0, bold ? FW_BOLD : FW_SEMIBOLD, (DWORD)italic, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, 0,
                            ANTIALIASED_QUALITY, 0, font_faces[face]);
            of = SelectObject(dc, f); GetTextMetricsW(dc, &tm);
        }
    }
    SIZE sz = { 0, 0 }; GetTextExtentPoint32W(dc, ws, n, &sz);
    int pad = 2, ink = (int)((sz.cx * NX + tm.tmOverhang + SS - 1) / SS) + 2 + (italic ? (int)(cap_h * 0.45f) : 0);
    int h = (tm.tmHeight + SS - 1) / SS + pad * 2;
    if (lean) ink += (int)ceilf(LX * h);
    int w = ink + pad * 2;
    if (w < 1 || h < 1 || w > 4096 || h > 1024) { SelectObject(dc, of); DeleteObject(f); DeleteDC(dc); return 0; }
    int w4 = w * SS, h4 = h * SS;
    BITMAPINFO b = bmi_for(w4, h4); void *bits = NULL;
    HBITMAP bm = CreateDIBSection(dc, &b, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bm) { SelectObject(dc, of); DeleteObject(f); DeleteDC(dc); return 0; }
    HGDIOBJ ob = SelectObject(dc, bm);
    memset(bits, 0, (size_t)w4 * h4 * 4);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
    if (narrow || lean) { /* x' = NX * x - LX * y + LX * h4: narrower, and the top leans right */
        SetGraphicsMode(dc, GM_ADVANCED);
        XFORM xf = { NX, 0, -LX, 1, LX * (float)h4, 0 };
        SetWorldTransform(dc, &xf);
        TextOutW(dc, (int)lroundf(pad * SS / NX), pad * SS, ws, n);
        ModifyWorldTransform(dc, NULL, MWT_IDENTITY);
    } else TextOutW(dc, pad * SS, pad * SS, ws, n);
    GdiFlush();
    unsigned char *a = (unsigned char *)malloc((size_t)w * h);
    if (a) {
        const uint32_t *px = (const uint32_t *)bits;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                unsigned sum = 0;
                for (int sy = 0; sy < SS; sy++) { const uint32_t *r = px + (size_t)(y * SS + sy) * w4 + x * SS; for (int sx = 0; sx < SS; sx++) sum += (r[sx] >> 8) & 255; }
                a[(size_t)y * w + x] = (unsigned char)((sum + SS * SS / 2) / (SS * SS));
            }
    }
    int base = pad + (tm.tmAscent + SS / 2) / SS;
    SelectObject(dc, ob); DeleteObject(bm); SelectObject(dc, of); DeleteObject(f); DeleteDC(dc);
    if (!a) return 0;
    free(old->buf);
    old->buf = a; strcpy(old->s, str); old->hq = hq; old->bold = bold; old->font = font; old->use = ++g_tx_clock;
    old->m.w = w; old->m.h = h; old->m.base = base; old->m.a = a;
    *out = old->m;
    return 1;
}

/* ------------------------------------------------------------------ hotkey / tray */
static int g_hk_ok;
static void register_hotkey(void) {
    UnregisterHotKey(g_main, HK_TOGGLE);
    g_hk_ok = RegisterHotKey(g_main, HK_TOGGLE, S.hk_mods | 0x4000 /* MOD_NOREPEAT */, S.hk_vk) != 0;
}
static void set_hidden(int h) {
    g_hidden = h; g_force = 1;
    if (g_overlay) {
        if (h) ShowWindow(g_overlay, SW_HIDE);
        else { ShowWindow(g_overlay, SW_SHOWNOACTIVATE); g_ov_first = 1; g_prev_bb = (IRect){ 0, 0, g_ov.w, g_ov.h }; }
    }
}

static NOTIFYICONDATAW g_nid;
static UINT g_msg_taskbar;
static void tray_add(void) {
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid); g_nid.hWnd = g_main; g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; g_nid.uCallbackMessage = WM_TRAY; g_nid.hIcon = g_icon_small;
    wcscpy(g_nid.szTip, APP_NAME);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

/* ------------------------------------------------------------------ settings window */
enum {
    ID_TRAIL_COL = 100, ID_TRAIL_W, ID_TRAIL_LEN, ID_DOT, ID_LMB_COL, ID_RMB_COL, ID_CLICK_SIZE, ID_CLICK_FADE,
    ID_APM_ON, ID_APM_POS, ID_APM_SIZE, ID_APM_COL, ID_APM_LABCOL, ID_APM_BG, ID_APM_BGOP,
    ID_MODE, ID_CHROMA_COL, ID_HOTKEY, ID_RESET, ID_OPENINI, ID_CLOSE, ID_APM_RESET, ID_INPUT, ID_APM_WIN, ID_APM_AVG, ID_APM_OFFX, ID_APM_OFFY, ID_APM_MOVE,
    ID_TXT_BOLD, ID_TXT_OUTLINE, ID_TXT_SHADOW, ID_OUTLINE_W, ID_OUTLINE_COL,
    ID_TRAIL_TAPER, ID_TRAIL_OL, ID_TRAIL_OL_W, ID_TRAIL_OL_COL, ID_CLICK_STYLE, ID_CLICK_RING, ID_CLICK_OL, ID_CLICK_OL_W, ID_CLICK_OL_COL,
    ID_NUM_SIZE, ID_APM_PAD, ID_APM_RAD, ID_APM_BGCOL, ID_PREVIEW,
    ID_TRAIL_ON, ID_TRAIL_SMOOTH, ID_TRAIL_GRAD, ID_TRAIL_TAIL_COL, ID_CK_L, ID_CK_R, ID_CK_M, ID_CK_X, ID_OTHER_COL,
    ID_GLOW_TRAIL, ID_GLOW_CLICKS, ID_GLOW_APM, ID_GLOW_STR, ID_GLOW_SIZE, ID_GLOW_MODE, ID_GLOW_COL, ID_GLOW_COL2,
    ID_LABEL_TXT, ID_FONT, ID_IDLE, ID_ALIGN, ID_FIX_DX, ID_FIX_DY, ID_FIX_SX, ID_FIX_SY, ID_FIX_RESET, ID_WIN_MIN, ID_WIN_CLOSE,
    ID_LAB_SIZE, ID_LAB_BOLD, ID_LAB_OL, ID_LAB_POS, ID_SCALE_SCREEN,
    ID_KB_ON, ID_MS_ON, ID_KB_LAYOUT, ID_KB_CUSTOM, ID_KB_OUT_COL, ID_KB_ON_COL, ID_KB_LAB_COL, ID_KB_IDLE_COL, ID_KB_IDLE_OP, ID_KB_SIZE,
    ID_KB_GLOW, ID_KB_POS, ID_KB_OFFX, ID_KB_OFFY, ID_KB_MOVE,
    ID_KB_PLATE, ID_KB_PLATE_COL, ID_KB_PLATE_OP, ID_KB_3D, ID_KB_ITALIC, ID_KB_FONT, ID_KB_LINE, ID_APM_BORDER, ID_APM_ACCENT,
    ID_PRESET0, ID_PRESET1, ID_PRESET2, ID_PRESET3, ID_PRESET4, ID_KB_SHAPE, ID_KB_EDIT,
    ID_MS_ATTACH, ID_MS_POS, ID_MS_OFFX, ID_MS_OFFY,
    ID_CH_EDGES, ID_CH_BG, ID_CH_SIZE, ID_MAXFPS, ID_CH_SCREEN, ID_CH_MARKS,
    ID_EXIT, ID_KB_BG, ID_APM_BGMODE, ID_STREAM_DESK, ID_STREAM_GUIDE
};
static int SC(int v) { return MulDiv(v, g_dpi, 96); }
static float SF(float v) { return v * (float)g_dpi / 96.0f; }

static void apply_change(int structural);
static void keyed_open(void); static void ke_load_text(const char *txt);
static void sync_controls(void);

static RGBf *color_target(int id) {
    switch (id) {
    case ID_TRAIL_COL: return &S.st.trail; case ID_LMB_COL: return &S.st.lmb; case ID_RMB_COL: return &S.st.rmb;
    case ID_APM_COL: return &S.st.apm_text; case ID_APM_LABCOL: return &S.st.apm_label; case ID_CHROMA_COL: return &S.chroma;
    case ID_OUTLINE_COL: return &S.st.outline_col; case ID_TRAIL_OL_COL: return &S.st.trail_outline_col;
    case ID_CLICK_OL_COL: return &S.st.click_outline_col; case ID_APM_BGCOL: return &S.st.panel;
    case ID_TRAIL_TAIL_COL: return &S.st.trail_tail; case ID_OTHER_COL: return &S.st.click_other;
    case ID_GLOW_COL: return &S.st.glow_col; case ID_GLOW_COL2: return &S.st.glow_col2;
    case ID_KB_OUT_COL: return &S.st.keys_outline; case ID_KB_ON_COL: return &S.st.keys_on;
    case ID_KB_LAB_COL: return &S.st.keys_label; case ID_KB_IDLE_COL: return &S.st.keys_idle; case ID_KB_PLATE_COL: return &S.st.keys_plate_col;
    }
    return NULL;
}
static int chroma_bg_index(void) {
    const RGBf c = S.chroma;
    if (c.r < 0.02f && c.g > 0.98f && c.b < 0.02f) return 0;
    if (c.r > 0.98f && c.g < 0.02f && c.b > 0.98f) return 1;
    if (c.r < 0.02f && c.g < 0.02f && c.b < 0.02f) return 2;
    return -1;
}
static int *check_target(int id) {
    switch (id) {
    case ID_DOT: return &S.st.cursor_dot; case ID_TRAIL_OL: return &S.st.trail_outline; case ID_CLICK_OL: return &S.st.click_outline;
    case ID_APM_ON: return &S.st.apm_enabled; case ID_APM_AVG: return &S.st.apm_show_avg;
    case ID_TXT_BOLD: return &S.st.text_bold; case ID_TXT_OUTLINE: return &S.st.text_outline; case ID_TXT_SHADOW: return &S.st.text_shadow;
    case ID_TRAIL_ON: return &S.st.trail_enabled; case ID_ALIGN: return &S.align_check; case ID_CH_MARKS: return &S.chroma_marks;
    case ID_CK_L: return &S.st.click_show[0]; case ID_CK_R: return &S.st.click_show[1];
    case ID_CK_M: return &S.st.click_show[2]; case ID_CK_X: return &S.st.click_show[3];
    case ID_LAB_BOLD: return &S.st.lab_bold; case ID_LAB_OL: return &S.st.lab_outline; case ID_SCALE_SCREEN: return &S.scale_screen; case ID_MS_ATTACH: return &S.mouse_attach;
    case ID_KB_3D: return &S.st.keys_depth; case ID_KB_ITALIC: return &S.st.keys_italic;
    case ID_APM_BORDER: return &S.st.panel_border; case ID_APM_ACCENT: return &S.st.panel_accent;
    case ID_KB_ON: return &S.st.keys_kb; case ID_MS_ON: return &S.st.keys_mouse; case ID_KB_GLOW: return &S.st.keys_glow;
    case ID_GLOW_TRAIL: return &S.st.glow_trail; case ID_GLOW_CLICKS: return &S.st.glow_clicks; case ID_GLOW_APM: return &S.st.glow_apm;
    case ID_STREAM_GUIDE: return &S.stream_guide;
    }
    return NULL;
}

typedef struct { int id; int lo, hi; const wchar_t *unit; int div; float *f; float mul; } SliderDef;
static const SliderDef sliders[] = {
    { ID_TRAIL_W, 1, 40, L"px", 1, &S.st.trail_width, 1 },          { ID_TRAIL_LEN, 0, 3000, L"ms", 1, &S.st.trail_len_s, 1000 },
    { ID_TRAIL_TAPER, 0, 100, L"%", 1, &S.st.trail_taper, 100 },     { ID_TRAIL_OL_W, 1, 8, L"px", 2, &S.st.trail_outline_px, 2 },
    { ID_CLICK_SIZE, 4, 80, L"px", 1, &S.st.click_size, 1 },         { ID_CLICK_FADE, 0, 4000, L"ms", 1, &S.st.click_fade_s, 1000 },
    { ID_CLICK_RING, 1, 20, L"px", 2, &S.st.click_ring_px, 2 },      { ID_CLICK_OL_W, 1, 8, L"px", 2, &S.st.click_outline_px, 2 },
    { ID_APM_SIZE, 50, 400, L"%", 1, &S.st.apm_scale, 100 },         { ID_NUM_SIZE, 50, 250, L"%", 1, &S.st.num_scale, 100 },
    { ID_APM_BGOP, 0, 100, L"%", 1, &S.st.panel_opacity, 100 },      { ID_APM_WIN, 5, 60, L"s", 1, &S.apm_window_s, 1 },
    { ID_OUTLINE_W, 1, 8, L"px", 2, &S.st.outline_px, 2 },           { ID_APM_PAD, 0, 40, L"px", 1, &S.st.panel_pad, 1 },
    { ID_APM_RAD, 0, 30, L"px", 1, &S.st.panel_radius, 1 },
    { ID_GLOW_STR, 0, 100, L"%", 1, &S.st.glow_strength, 100 },     { ID_GLOW_SIZE, 2, 40, L"px", 1, &S.st.glow_radius, 1 },
    { ID_IDLE, 0, 60, L"s", 1, &S.idle_hide_s, 1 },
    { ID_FIX_DX, -100, 100, L"px", 1, &S.fix_dx, 1 },               { ID_FIX_DY, -100, 100, L"px", 1, &S.fix_dy, 1 },
    { ID_FIX_SX, 950, 1050, L"%", 10, &S.fix_sx, 1000 },            { ID_FIX_SY, 950, 1050, L"%", 10, &S.fix_sy, 1000 },
    { ID_LAB_SIZE, 20, 100, L"%", 1, &S.st.lab_scale, 100 },
    { ID_KB_IDLE_OP, 0, 100, L"%", 1, &S.st.keys_idle_op, 100 },     { ID_KB_PLATE_OP, 0, 100, L"%", 1, &S.st.keys_plate_op, 100 },
    { ID_KB_LINE, 2, 12, L"px", 2, &S.st.keys_line, 2 },     { ID_KB_SIZE, 50, 250, L"%", 1, &S.st.keys_scale, 100 },
};
static const SliderDef *slider_def(int id) {
    for (size_t i = 0; i < sizeof(sliders) / sizeof(sliders[0]); i++) if (sliders[i].id == id) return &sliders[i];
    return NULL;
}
static int slider_get_model(int id) { const SliderDef *d = slider_def(id); return d ? (int)lroundf(*d->f * d->mul) : 0; }
static void slider_set_model(int id, int v) { const SliderDef *d = slider_def(id); if (d) *d->f = (float)v / d->mul; }

static int seg_get(int id) {
    switch (id) {
    case ID_CLICK_STYLE: return S.st.click_style; case ID_APM_POS: return S.st.apm_corner; case ID_MODE: return S.mode; case ID_CH_EDGES: return S.chroma_hard; case ID_CH_SIZE: return S.chroma_fit ? 0 : 1; case ID_CH_SCREEN: return S.chroma_screen <= 3 ? S.chroma_screen : S.chroma_screen == 5 ? 4 : -1; case ID_CH_BG: return chroma_bg_index(); case ID_INPUT: return S.input_hook; case ID_MAXFPS: return S.max_draw == 60 ? 0 : S.max_draw == 120 ? 1 : S.max_draw == 144 ? 2 : S.max_draw == 0 ? 3 : -1;
    case ID_TRAIL_SMOOTH: return S.st.trail_smooth; case ID_TRAIL_GRAD: return S.st.trail_grad; case ID_GLOW_MODE: return S.st.glow_mode; case ID_FONT: return S.st.font;
    case ID_KB_FONT: return S.st.keys_font - 1;
    case ID_LAB_POS: return S.st.lab_below; case ID_KB_LAYOUT: { static const int ui[5] = { 1, 0, 4, 2, 3 }; return ui[S.st.keys_layout % 5]; } case ID_KB_SHAPE: return S.st.keys_shape; case ID_KB_POS: return S.st.keys_corner; case ID_MS_POS: return S.st.mouse_corner;
    case ID_KB_BG: return S.keys_bg; case ID_APM_BGMODE: return S.apm_bg; case ID_STREAM_DESK: return S.stream_clear ? 0 : 1;
    }
    return 0;
}

#define HEXC(v) RGB(((v) >> 16) & 255, ((v) >> 8) & 255, (v) & 255)
#define HEXF(v) ((RGBf){ (((v) >> 16) & 255) / 255.f, (((v) >> 8) & 255) / 255.f, ((v) & 255) / 255.f })
enum {
    C_BG = 0x0E1013, C_RAISED = 0x1C2027, C_RAISED_HI = 0x242932, C_LINE = 0x2A2F37, C_LINE_HI = 0x3A404B,
    C_CARD = 0x14171C, C_CARD_LINE = 0x22262D, C_ROW_HOT = 0x1A1E24, C_DIV = 0x1E2228,
    C_TEXT = 0xECEDEF, C_LABEL = 0xB9BEC7, C_MUTED = 0x7D8491, C_ACCENT = 0xFFB23E, C_ACCENT_HI = 0xFFC566, C_ON_ACCENT = 0x1A1306,
    C_TRACK = 0x2A2F38, C_KNOB_OFF = 0x8A909C, C_SEG_ON = 0x30363F,
    C_DONE = 0xF2C14E, C_DONE_HOT = 0xF6D36A, C_DONE_DOWN = 0xD4A43A, C_DONE_LINE = 0xC9A33A, C_DONE_INK = 0x1A1A1A
};

static HFONT g_f_body, g_f_semi, g_f_title, g_f_cap, g_f_note, g_f_hint, g_f_done;
static HBRUSH g_br_raised, g_br_bg;
static HFONT mkfont(const wchar_t *face, int px, int weight) {
    return CreateFontW(-MulDiv(px, g_dpi, 96), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
}
static int g_ui_dpi; static POINT g_ui_pos; static int g_ui_pos_set;
static void free_fonts(void) {
    HFONT *f[] = { &g_f_body, &g_f_semi, &g_f_title, &g_f_cap, &g_f_note, &g_f_hint, &g_f_done };
    for (size_t i = 0; i < sizeof(f) / sizeof(f[0]); i++) if (*f[i]) { DeleteObject(*f[i]); *f[i] = NULL; }
}
static void ensure_fonts(void) {
    if (g_f_body) return;
    HDC dc = GetDC(NULL); g_dpi = g_ui_dpi ? g_ui_dpi : GetDeviceCaps(dc, LOGPIXELSY);
    const wchar_t *face = L"Segoe UI";
    HFONT t = mkfont(L"Segoe UI Variable Text", 13, FW_NORMAL); HGDIOBJ o = SelectObject(dc, t);
    wchar_t got[64] = { 0 }; GetTextFaceW(dc, 64, got); SelectObject(dc, o); DeleteObject(t);
    if (!_wcsicmp(got, L"Segoe UI Variable Text")) face = L"Segoe UI Variable Text";
    ReleaseDC(NULL, dc);
    g_f_body = mkfont(face, 13, FW_NORMAL);
    g_f_semi = mkfont(face, 13, FW_SEMIBOLD);
    g_f_title = mkfont(face, 14, FW_SEMIBOLD);
    g_f_cap = mkfont(face, 11, FW_SEMIBOLD);
    g_f_note = mkfont(face, 12, FW_NORMAL);
    g_f_hint = mkfont(face, 12, FW_NORMAL);
    g_f_done = mkfont(face, 15, FW_BOLD);
    if (!g_br_raised) g_br_raised = CreateSolidBrush(HEXC(C_RAISED));
    if (!g_br_bg) g_br_bg = CreateSolidBrush(HEXC(C_BG));
}

static const wchar_t *page_names[PG_N] = { L"Trail", L"Clicks", L"APM text", L"APM box", L"Keys", L"Glow", L"General" };
enum { W_SECTION, W_LABEL, W_SLIDER, W_SWATCH, W_TOGGLE, W_SEG, W_BUTTON, W_PRIMARY, W_FIELD, W_HOTKEY, W_NOTE, W_STATUS, W_TAB, W_HINT, W_CAPBTN, W_CARD, W_ROW, W_DIV };
typedef struct {
    int type, page, id;
    int x, y, w, h;
    int hx, hy, hw, hh;
    int by, hby; /* unscrolled y */
    int oy, hoy; /* y as first laid out, before rows are hidden */
    int show;    /* 0 always, 1 keys custom colour, 2 APM custom colour, 3 APM box chrome, 4 stream-window option */
    const wchar_t *text; const wchar_t *const *items; int n, cols;
} Wd;
static Wd g_wd[600]; static int g_last_py, g_hrow = -1; static int g_nwd, g_cur_page, g_hot = -1, g_active = -1, g_rec;
static HWND g_edit_x, g_edit_y, g_edit_lab, g_edit_kx, g_edit_ky, g_edit_kc, g_edit_mx, g_edit_my;
static int g_scroll;
static wchar_t g_status[256];
static Surface g_ui;

enum { SET_W = 968, SET_H = 680, TITLE_H = 48, TABS_Y = 48, TABS_H = 44, VIEW_Y = 93, CONTENT_Y = 112, LX = 24, RL = 40, CX = 232, RX = 528, CR = 544, PANE_X = 572, PV_X = 592, PV_W = 352, PV_H = 334, FOOT_Y = 608 };

static int g_wd_show;
static void show_if(int s) { g_wd_show = s; }
static int wd_shown(const Wd *d) {
    if (d->show == 1) return S.keys_bg == 2;
    if (d->show == 2) return S.apm_bg == 2;
    if (d->show == 3) return S.apm_bg != 0;
    if (d->show == 4) return S.mode == 2;
    return 1;
}
static Wd *wd_add(int type, int page, int id, int x, int y, int w, int h, const wchar_t *text) {
    if (g_nwd >= (int)(sizeof(g_wd) / sizeof(g_wd[0]))) return &g_wd[g_nwd - 1];
    Wd *d = &g_wd[g_nwd++]; memset(d, 0, sizeof(*d));
    d->type = type; d->page = page; d->id = id; d->x = x; d->y = y; d->w = w; d->h = h; d->text = text;
    d->hx = x; d->hy = y; d->hw = w; d->hh = h; d->by = y; d->hby = y;
    d->oy = y; d->hoy = y; d->show = g_wd_show;
    return d;
}
/* every row: plain label on the left, control on the right, one muted line under it saying what it does.
   A hint whose id is a colour id turns into a warning when that colour equals the chroma key. */
static int row_y; static Wd *g_row, *g_card; static int g_card_rows;
/* each section is a card; each setting is a row in it: label left, control right, one plain line under it */
static void end_card(void) { if (g_card) { g_card->h = row_y - g_card->by; g_card->hh = g_card->h; row_y += 4; } g_card = NULL; }
static void sec(int p, const wchar_t *t) {
    end_card();
    if (row_y > CONTENT_Y) row_y += 18;
    wd_add(W_SECTION, p, 0, LX + 2, row_y, CR - LX, 18, t); row_y += 24;
    g_card = wd_add(W_CARD, p, 0, LX, row_y, CR - LX, 0, NULL); g_card_rows = 0; row_y += 2;
}
static void row_begin(int p) {
    if (g_card && g_card_rows++ > 0) wd_add(W_DIV, p, 0, RL, row_y, CR - RL - 16, 1, NULL);
    g_row = wd_add(W_ROW, p, 0, LX + 4, row_y, CR - LX - 8, 0, NULL);
    row_y += 10;
}
static void row_end(void) { row_y += 10; if (g_row) { g_row->h = row_y - g_row->by; g_row->hh = g_row->h; } g_row = NULL; }
static void lab(int p, const wchar_t *t) { row_begin(p); wd_add(W_LABEL, p, 0, RL, row_y, CX - RL - 8, 32, t); }
static void hint(int p, const wchar_t *t, int warn_id, int ctl_h) {
    if (t) { wd_add(W_HINT, p, warn_id, RL, row_y + ctl_h, RX - RL, 18, t); row_y += ctl_h + 20; }
    else row_y += ctl_h;
    row_end();
}
static void r_slider(int p, const wchar_t *t, int id, const wchar_t *h) { lab(p, t); wd_add(W_SLIDER, p, id, CX, row_y, RX - CX, 32, NULL); hint(p, h, 0, 30); }
static void r_swatch(int p, const wchar_t *t, int id, const wchar_t *h) { lab(p, t); wd_add(W_SWATCH, p, id, CX, row_y + 2, 132, 28, NULL); hint(p, h, id, 32); }
static void r_toggle(int p, const wchar_t *t, int id, const wchar_t *h) {
    lab(p, t); Wd *d = wd_add(W_TOGGLE, p, id, CX, row_y + 6, 36, 20, NULL);
    d->hx = RL; d->hy = row_y; d->hw = CX + 44 - RL; d->hh = 32; d->hby = row_y; d->hoy = row_y; hint(p, h, 0, 32);
}
static void r_toggle_swatch(int p, const wchar_t *t, int tid, int cid, const wchar_t *h) {
    lab(p, t); Wd *d = wd_add(W_TOGGLE, p, tid, CX, row_y + 6, 36, 20, NULL);
    d->hx = RL; d->hy = row_y; d->hw = CX + 44 - RL; d->hh = 32; d->hby = row_y; d->hoy = row_y;
    wd_add(W_SWATCH, p, cid, CX + 56, row_y + 2, 132, 28, NULL); hint(p, h, cid, 32);
}
static void r_seg(int p, const wchar_t *t, int id, const wchar_t *const *items, int n, const wchar_t *h) {
    lab(p, t); Wd *d = wd_add(W_SEG, p, id, CX, row_y + 2, RX - CX, 28, NULL); d->items = items; d->n = n; d->cols = n; hint(p, h, 0, 32);
}
static void r_seg_grid(int p, const wchar_t *t, int id, const wchar_t *const *items, int n, int cols, const wchar_t *h) {
    lab(p, t); int rows = (n + cols - 1) / cols;
    Wd *d = wd_add(W_SEG, p, id, CX, row_y + 2, RX - CX, 28 * rows, NULL); d->items = items; d->n = n; d->cols = cols;
    hint(p, h, 0, 28 * rows + 6);
}
static void layout_scroll(void);
static int page_bottom(int p) {
    int b = 0;
    for (int i = 0; i < g_nwd; i++) if (g_wd[i].page == p && wd_shown(&g_wd[i]) && g_wd[i].by + g_wd[i].h > b) b = g_wd[i].by + g_wd[i].h;
    return b;
}
static void layout_collapse(void) {
    for (int p = 0; p < PG_N; p++) {
        int acc = 0, hid0 = -1, card_bot = 0;
        Wd *card = NULL;
        for (int i = 0; i < g_nwd; i++) {
            Wd *d = &g_wd[i];
            if (d->page != p) continue;
            if (!wd_shown(d)) {
                if (hid0 < 0) hid0 = d->oy;
                d->by = d->oy; d->hby = d->hoy;
                continue;
            }
            if (hid0 >= 0) { acc += d->oy - hid0; hid0 = -1; }
            d->by = d->oy - acc;
            d->hby = d->hoy - acc;
            if (d->type == W_SECTION && card) {
                card->h = card->hh = (card_bot > card->by + 8) ? card_bot - card->by : 0;
                card = NULL; card_bot = 0;
            }
            if (d->type == W_CARD) {
                if (card) card->h = card->hh = (card_bot > card->by + 8) ? card_bot - card->by : 0;
                card = d; card_bot = 0;
            } else if (card) {
                int bot = d->by + d->h;
                if (bot > card_bot) card_bot = bot;
            }
        }
        if (card) card->h = card->hh = (card_bot > card->by + 8) ? card_bot - card->by : 0;
    }
}
static void layout_relayout(void) {
    layout_collapse();
    layout_scroll();
    if (g_set) InvalidateRect(g_set, NULL, FALSE);
}
static int max_scroll(void) { int m = page_bottom(g_cur_page) + 20 - FOOT_Y; return m > 0 ? m : 0; }
static void edit_place(HWND e, const Wd *d);
static HWND field_edit(int id) {
    switch (id) {
    case ID_APM_OFFX: return g_edit_x; case ID_APM_OFFY: return g_edit_y; case ID_LABEL_TXT: return g_edit_lab;
    case ID_KB_OFFX: return g_edit_kx; case ID_KB_OFFY: return g_edit_ky; case ID_KB_CUSTOM: return g_edit_kc;
    case ID_MS_OFFX: return g_edit_mx; case ID_MS_OFFY: return g_edit_my;
    }
    return NULL;
}
static void layout_scroll(void) {
    int m = max_scroll(); if (g_scroll > m) g_scroll = m; if (g_scroll < 0) g_scroll = 0;
    for (int i = 0; i < g_nwd; i++) {
        Wd *d = &g_wd[i];
        if (d->page < 0) continue;
        d->y = d->by - g_scroll; d->hy = d->hby - g_scroll;
    }
    for (int i = 0; i < g_nwd; i++) {
        Wd *d = &g_wd[i]; HWND e;
        if (d->type != W_FIELD || !(e = field_edit(d->id))) continue;
        int show = d->page == g_cur_page && wd_shown(d) && d->y >= VIEW_Y && d->y + d->h <= FOOT_Y;
        if (show) edit_place(e, d);
        ShowWindow(e, show ? SW_SHOWNA : SW_HIDE);
    }
}

static void settings_status(void) {
    if (!g_set) return;
    swprintf(g_status, 256, L"%ls",
             !g_hk_ok ? L"This shortcut could not be registered. Another app may be using it."
             : (S.mode == 2 && g_sw_failed) ? L"The stream window needs DirectX 11. Use Green screen instead."
             : (S.mode == 2 && S.stream_clear && !g_sw_clear) ? L"See-through was not available. The stream window is black on this screen."
             : L"Keys are only counted and shown live. Nothing is saved or sent.");
    InvalidateRect(g_set, NULL, FALSE);
}

static void edit_place(HWND e, const Wd *d) {
    int fh = SC(18), ox = d->text ? 28 : 10;
    SetWindowPos(e, NULL, SC(d->x + ox), SC(d->y) + (SC(d->h) - fh) / 2, SC(d->w - ox - 10), fh, SWP_NOZORDER | SWP_NOACTIVATE);
}
static void show_page(int p) {
    g_cur_page = p; g_hot = -1; g_hrow = -1; g_scroll = 0;
    layout_relayout();
}

static void dark_title(HWND h) {
    BOOL on = TRUE;
    if (FAILED(DwmSetWindowAttribute(h, 20, &on, sizeof(on)))) DwmSetWindowAttribute(h, 19, &on, sizeof(on));
}

static int settings_pos_ok(int x, int y) {
    RECT wr = { x, y, x + SC(SET_W), y + SC(TITLE_H) };
    return MonitorFromRect(&wr, MONITOR_DEFAULTTONULL) != NULL;
}
static void settings_remember(void) {
    RECT r;
    if (!g_set || !GetWindowRect(g_set, &r)) return;
    S.set_x = r.left; S.set_y = r.top; S.set_pos = 1;
    SetTimer(g_main, TIMER_SAVE, 600, NULL);
}
static void settings_raise(HWND h) {
    if (!h) return;
    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
    else ShowWindow(h, SW_SHOWNORMAL);
    SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(h);
}
static void settings_create(void) {
    if (g_set) { settings_raise(g_set); return; }
    ensure_fonts();
    /* custom chrome: a normal captioned window (taskbar entry, Alt+F4, Win+arrow snap, minimise animation)
       whose client area covers the whole window; the dark header row acts as the title bar */
    DWORD st = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    int sx = CW_USEDEFAULT, sy = CW_USEDEFAULT;
    if (S.set_pos && settings_pos_ok(S.set_x, S.set_y)) { sx = S.set_x; sy = S.set_y; }
    g_set = CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_APPWINDOW, L"MAOSettings", APP_NAME L" - Settings", st, sx, sy,
                            SC(SET_W), SC(SET_H), NULL, NULL, g_inst, NULL);
    dark_title(g_set);
    {
        MARGINS m = { 0, 0, 0, 1 }; DwmExtendFrameIntoClientArea(g_set, &m); /* keeps the DWM shadow */
        int corner = 2; DwmSetWindowAttribute(g_set, 33, &corner, sizeof(corner)); /* rounded corners on Windows 11 */
        COLORREF border = HEXC(C_LINE); DwmSetWindowAttribute(g_set, 34, &border, sizeof(border));
        SetWindowPos(g_set, NULL, 0, 0, SC(SET_W), SC(SET_H), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    g_nwd = 0; g_cur_page = PG_TRAIL; g_hot = g_active = -1; g_rec = 0;
    for (int i = 0; i < PG_N; i++) wd_add(W_TAB, -1, i, 0, TABS_Y, 0, TABS_H, page_names[i]);
    wd_add(W_CAPBTN, -1, ID_WIN_MIN, SET_W - 92, 0, 46, TITLE_H, NULL);
    wd_add(W_CAPBTN, -1, ID_WIN_CLOSE, SET_W - 46, 0, 46, TITLE_H, NULL);

    static const wchar_t *const smooth[] = { L"Off", L"Low", L"Medium", L"High", L"Very high" };
    static const wchar_t *const grads[] = { L"One colour", L"Two colours" };
    end_card(); row_y = CONTENT_Y;
    sec(PG_TRAIL, L"TRAIL");
    r_toggle(PG_TRAIL, L"Show trail", ID_TRAIL_ON, L"A line that follows your mouse.");
    r_seg(PG_TRAIL, L"Colours", ID_TRAIL_GRAD, grads, 2, L"Two colours fades from the mouse end to the tail end.");
    r_swatch(PG_TRAIL, L"Colour", ID_TRAIL_COL, L"Colour of the line, at the mouse end.");
    r_swatch(PG_TRAIL, L"Tail colour", ID_TRAIL_TAIL_COL, L"Colour at the end of the line. Only with Two colours.");
    r_slider(PG_TRAIL, L"Thickness", ID_TRAIL_W, L"How thick the line is.");
    r_slider(PG_TRAIL, L"Length", ID_TRAIL_LEN, L"How long the line stays behind the mouse.");
    r_slider(PG_TRAIL, L"Thinner at the end", ID_TRAIL_TAPER, L"0% = same width all the way. 100% = ends in a point.");
    r_seg_grid(PG_TRAIL, L"Smoothing", ID_TRAIL_SMOOTH, smooth, 5, 3, L"Makes the whole trail flow smoothly instead of looking jagged. Higher is smoother.");
    r_toggle(PG_TRAIL, L"Dot on the mouse", ID_DOT, L"A small dot right at the mouse tip.");
    sec(PG_TRAIL, L"BORDER");
    r_toggle_swatch(PG_TRAIL, L"Border", ID_TRAIL_OL, ID_TRAIL_OL_COL, L"Thin border so the line stands out on bright scenes.");
    r_slider(PG_TRAIL, L"Border thickness", ID_TRAIL_OL_W, L"How thick the border is.");

    static const wchar_t *const styles[] = { L"Ring + dot", L"Ring", L"Dot" };
    end_card(); row_y = CONTENT_Y;
    sec(PG_CLICKS, L"BUTTONS");
    r_toggle_swatch(PG_CLICKS, L"Left click", ID_CK_L, ID_LMB_COL, L"Show a mark where you left-click, in this colour.");
    r_toggle_swatch(PG_CLICKS, L"Right click", ID_CK_R, ID_RMB_COL, L"Show a mark where you right-click, in this colour.");
    r_toggle_swatch(PG_CLICKS, L"Wheel click", ID_CK_M, ID_OTHER_COL, L"Pressing the mouse wheel. Side buttons use this colour too.");
    r_toggle(PG_CLICKS, L"Side buttons", ID_CK_X, L"The back / forward buttons on the side of the mouse.");
    sec(PG_CLICKS, L"LOOK");
    r_seg(PG_CLICKS, L"Shape", ID_CLICK_STYLE, styles, 3, L"What appears where you click.");
    r_slider(PG_CLICKS, L"Size", ID_CLICK_SIZE, L"How big the mark gets.");
    r_slider(PG_CLICKS, L"Ring thickness", ID_CLICK_RING, L"How thick the ring is.");
    r_slider(PG_CLICKS, L"How long it stays", ID_CLICK_FADE, L"How long a mark stays before it fades away.");
    sec(PG_CLICKS, L"BORDER");
    r_toggle_swatch(PG_CLICKS, L"Border", ID_CLICK_OL, ID_CLICK_OL_COL, L"Thin border around the marks.");
    r_slider(PG_CLICKS, L"Border thickness", ID_CLICK_OL_W, L"How thick the border is.");

    static const wchar_t *const fonts[] = { L"Simple", L"Segoe UI", L"Bahnschrift", L"Consolas", L"Arial", L"Impact" };
    static const wchar_t *const lpos[] = { L"Right of number", L"Under number" };
    end_card(); row_y = CONTENT_Y;
    sec(PG_TEXT, L"COUNTER");
    r_toggle(PG_TEXT, L"Show APM", ID_APM_ON, L"Actions per minute: your clicks plus key presses.");
    r_slider(PG_TEXT, L"Count over", ID_APM_WIN, L"Longer = steadier number. Shorter = reacts faster.");
    r_toggle(PG_TEXT, L"Show average", ID_APM_AVG, L"A small line with your average APM since you started.");
    sec(PG_TEXT, L"NUMBER");
    r_seg_grid(PG_TEXT, L"Font", ID_FONT, fonts, 6, 3, L"Simple is drawn by the app. The others come with Windows.");
    r_swatch(PG_TEXT, L"Colour", ID_APM_COL, L"Colour of the number.");
    r_slider(PG_TEXT, L"Size", ID_NUM_SIZE, L"How big the number is.");
    r_toggle(PG_TEXT, L"Bold", ID_TXT_BOLD, L"Thicker number.");
    sec(PG_TEXT, L"LABEL");
    lab(PG_TEXT, L"Label text");
    wd_add(W_FIELD, PG_TEXT, ID_LABEL_TXT, CX, row_y + 2, 160, 28, NULL);
    hint(PG_TEXT, L"The word next to the number, like APM. Leave empty to hide it.", 0, 32);
    r_seg(PG_TEXT, L"Label position", ID_LAB_POS, lpos, 2, L"Where the label sits.");
    r_slider(PG_TEXT, L"Label size", ID_LAB_SIZE, L"Size compared to the number. 40% or more reads well on stream.");
    r_swatch(PG_TEXT, L"Label colour", ID_APM_LABCOL, L"Colour of the label.");
    r_toggle(PG_TEXT, L"Label bold", ID_LAB_BOLD, L"Thicker label, easier to read when the stream is small.");
    r_toggle(PG_TEXT, L"Label border", ID_LAB_OL, L"Border around the label. Uses the border colour below.");
    sec(PG_TEXT, L"BORDER");
    r_toggle_swatch(PG_TEXT, L"Number border", ID_TXT_OUTLINE, ID_OUTLINE_COL, L"Border around the number so it reads on any background.");
    r_slider(PG_TEXT, L"Border thickness", ID_OUTLINE_W, L"How thick the border is (number and label).");
    r_toggle(PG_TEXT, L"Shadow", ID_TXT_SHADOW, L"A shadow just below the text, in the border colour.");

    static const wchar_t *const corners[] = { L"Top left", L"Top right", L"Bottom left", L"Bottom right" };
    end_card(); row_y = CONTENT_Y;
    sec(PG_BOX, L"BACKGROUND");
    {
        static const wchar_t *const abgs[] = { L"Invisible", L"Dark box", L"Custom colour" };
        r_seg(PG_BOX, L"Background", ID_APM_BGMODE, abgs, 3, L"Invisible shows only the number, with a thin outline for readability.");
    }
    show_if(2);
    r_swatch(PG_BOX, L"Colour", ID_APM_BGCOL, L"Colour of the box behind the counter.");
    r_slider(PG_BOX, L"See-through", ID_APM_BGOP, L"0% = invisible, 100% = solid. Always solid in the green screen window.");
    show_if(3);
    r_slider(PG_BOX, L"Space inside", ID_APM_PAD, L"Gap between the text and the edge of the box.");
    r_slider(PG_BOX, L"Rounded corners", ID_APM_RAD, L"How round the corners are.");
    r_toggle(PG_BOX, L"Thin edge", ID_APM_BORDER, L"A fine line around the box.");
    r_toggle(PG_BOX, L"Accent bar", ID_APM_ACCENT, L"A short bar in the label colour on the left of the box.");
    show_if(0);
    sec(PG_BOX, L"SIZE");
    r_slider(PG_BOX, L"Size", ID_APM_SIZE, L"Makes the whole counter bigger or smaller.");
    r_toggle(PG_BOX, L"Scale with screen size", ID_SCALE_SCREEN, L"Same look on 1080p, 1440p and 4K. Also sizes the Keys display.");
    sec(PG_BOX, L"POSITION");
    r_seg_grid(PG_BOX, L"Corner", ID_APM_POS, corners, 4, 2, L"Which corner of the screen it sticks to.");
    lab(PG_BOX, L"Distance from corner");
    Wd *fx = wd_add(W_FIELD, PG_BOX, ID_APM_OFFX, CX, row_y + 2, 96, 28, L"X");
    Wd *fy = wd_add(W_FIELD, PG_BOX, ID_APM_OFFY, CX + 104, row_y + 2, 96, 28, L"Y");
    hint(PG_BOX, L"Pixels sideways (X) and up or down (Y) from that corner.", 0, 32);
    lab(PG_BOX, L"Move by dragging");
    wd_add(W_BUTTON, PG_BOX, ID_APM_MOVE, CX, row_y, 180, 32, L"Move on screen");
    hint(PG_BOX, L"Drag it with the left mouse button. Click the button again when done.", 0, 32);

    static const wchar_t *const layouts[] = { L"League", L"Left side", L"WASD", L"Full", L"My keys" };
    end_card(); row_y = CONTENT_Y;
    sec(PG_KEYS, L"SHOW");
    r_toggle(PG_KEYS, L"Show keyboard", ID_KB_ON, L"Shows keys on screen. They light up while you hold them.");
    r_toggle(PG_KEYS, L"Show mouse", ID_MS_ON, L"Shows a mouse. Buttons and wheel light up when used.");
    r_seg_grid(PG_KEYS, L"Keys shown", ID_KB_LAYOUT, layouts, 5, 3, L"League: 1-6, Tab QWER, Shift D F B, Ctrl Alt Space. Left side: the left half of a keyboard.");
    {
        static const wchar_t *const kbgs[] = { L"Invisible", L"Dark plate", L"Custom colour" };
        r_seg(PG_KEYS, L"Background", ID_KB_BG, kbgs, 3, L"Invisible shows only the keys, with nothing behind them.");
    }
    show_if(1);
    r_swatch(PG_KEYS, L"Colour", ID_KB_PLATE_COL, L"Colour of the panel behind the keys and mouse.");
    r_slider(PG_KEYS, L"See-through", ID_KB_PLATE_OP, L"0% = invisible, 100% = solid. Always solid in the green screen window.");
    show_if(0);
    static const wchar_t *const kshapes[] = { L"Rounded", L"Slanted" };
    r_seg(PG_KEYS, L"Key shape", ID_KB_SHAPE, kshapes, 2, L"Slanted: leaning keys and big narrow letters. The mouse leans too.");
    lab(PG_KEYS, L"My keys");
    wd_add(W_BUTTON, PG_KEYS, ID_KB_EDIT, CX, row_y, 180, 32, L"Edit my keys");
    hint(PG_KEYS, L"Click keys to add them and drag them where you want.", 0, 32);
    lab(PG_KEYS, L"Typed keys");
    wd_add(W_FIELD, PG_KEYS, ID_KB_CUSTOM, CX, row_y + 2, RX - CX, 28, NULL);
    hint(PG_KEYS, L"Or type them: spaces between keys, / for a new row. Example: Q W E R / D F", 0, 32);
    sec(PG_KEYS, L"LOOK");
    r_swatch(PG_KEYS, L"Outline colour", ID_KB_OUT_COL, L"Colour of the key edges and the mouse outline.");
    r_swatch(PG_KEYS, L"Pressed colour", ID_KB_ON_COL, L"Colour a key or button fills with while held.");
    r_swatch(PG_KEYS, L"Letter colour", ID_KB_LAB_COL, L"Colour of the letters on the keys.");
    r_swatch(PG_KEYS, L"Inside colour", ID_KB_IDLE_COL, L"Colour inside keys that are not pressed.");
    r_slider(PG_KEYS, L"Inside see-through", ID_KB_IDLE_OP, L"0% = outline only. In the green screen window, under 50% is empty.");
    r_slider(PG_KEYS, L"Size", ID_KB_SIZE, L"Makes the keys and mouse bigger or smaller.");
    static const wchar_t *const kfonts[] = { L"Segoe UI", L"Bahnschrift", L"Consolas", L"Arial", L"Impact" };
    r_slider(PG_KEYS, L"Outline thickness", ID_KB_LINE, L"How thick the key edges are, at 1080p.");
    r_seg_grid(PG_KEYS, L"Letter font", ID_KB_FONT, kfonts, 5, 3, L"Font for the letters on the keys.");
    r_toggle(PG_KEYS, L"Italic letters", ID_KB_ITALIC, L"Slanted letters for a sporty look.");
    r_toggle(PG_KEYS, L"3D keys", ID_KB_3D, L"A darker solid edge under each key. Held keys sink into it. The edge stays solid even when Background is Invisible.");
    r_toggle(PG_KEYS, L"Glow when pressed", ID_KB_GLOW, L"Soft light around a key while held. Strength and size are on Glow.");
    sec(PG_KEYS, L"POSITION");
    r_seg_grid(PG_KEYS, L"Corner", ID_KB_POS, corners, 4, 2, L"Which corner of the screen the keyboard sticks to.");
    lab(PG_KEYS, L"Distance from corner");
    wd_add(W_FIELD, PG_KEYS, ID_KB_OFFX, CX, row_y + 2, 96, 28, L"X");
    wd_add(W_FIELD, PG_KEYS, ID_KB_OFFY, CX + 104, row_y + 2, 96, 28, L"Y");
    hint(PG_KEYS, L"Pixels sideways (X) and up or down (Y) from that corner.", 0, 32);
    lab(PG_KEYS, L"Move by dragging");
    wd_add(W_BUTTON, PG_KEYS, ID_KB_MOVE, CX, row_y, 180, 32, L"Move on screen");
    hint(PG_KEYS, L"Drag with the left mouse button. Click the button again when done.", 0, 32);
    sec(PG_KEYS, L"MOUSE DISPLAY");
    r_toggle(PG_KEYS, L"Keep mouse next to keyboard", ID_MS_ATTACH, L"Turn off to place the mouse on its own.");
    r_seg_grid(PG_KEYS, L"Mouse corner", ID_MS_POS, corners, 4, 2, L"Which corner the mouse sticks to when it is on its own.");
    lab(PG_KEYS, L"Mouse distance");
    wd_add(W_FIELD, PG_KEYS, ID_MS_OFFX, CX, row_y + 2, 96, 28, L"X");
    wd_add(W_FIELD, PG_KEYS, ID_MS_OFFY, CX + 104, row_y + 2, 96, 28, L"Y");
    hint(PG_KEYS, L"Pixels from that corner. Or use Move on screen and drag it.", 0, 32);

    static const wchar_t *const gmodes[] = { L"Match", L"One colour", L"Two colours" };
    end_card(); row_y = CONTENT_Y;
    sec(PG_GLOW, L"WHAT GLOWS");
    r_toggle(PG_GLOW, L"Trail", ID_GLOW_TRAIL, L"Soft light around the mouse line.");
    r_toggle(PG_GLOW, L"Click marks", ID_GLOW_CLICKS, L"Soft light around the click marks.");
    r_toggle(PG_GLOW, L"APM number", ID_GLOW_APM, L"Soft light around the number.");
    sec(PG_GLOW, L"LOOK");
    r_slider(PG_GLOW, L"Strength", ID_GLOW_STR, L"How bright the glow is.");
    r_slider(PG_GLOW, L"Size", ID_GLOW_SIZE, L"How far the glow spreads.");
    r_seg(PG_GLOW, L"Colour", ID_GLOW_MODE, gmodes, 3, L"Match uses the colour of whatever is glowing.");
    r_swatch(PG_GLOW, L"Glow colour", ID_GLOW_COL, L"Used for One colour, and as the start colour for Two colours.");
    r_swatch(PG_GLOW, L"Second colour", ID_GLOW_COL2, L"The end colour of the trail glow with Two colours.");
    row_y += 4;
    row_begin(PG_GLOW); wd_add(W_NOTE, PG_GLOW, 0, RL, row_y, RX - RL, 20, L"In the green screen window the glow is drawn in solid steps, so it stays clean in OBS."); row_y += 20; row_end();

    static const wchar_t *const modes[] = { L"On top of game", L"Green screen", L"Stream window" };
    static const wchar_t *const inputs[] = { L"Normal", L"Backup" };
    end_card(); row_y = CONTENT_Y;
    sec(PG_GENERAL, L"DISPLAY");
    {   /* three long names: give this row a wider control */
        lab(PG_GENERAL, L"Show as");
        Wd *d = wd_add(W_SEG, PG_GENERAL, ID_MODE, CX - 80, row_y + 2, RX - CX + 80, 28, NULL); d->items = modes; d->n = 3; d->cols = 3;
        hint(PG_GENERAL, L"Stream window: see-through, for Game Capture with Allow Transparency.", 0, 32);
    }
    r_slider(PG_GENERAL, L"Hide when idle", ID_IDLE, L"Hides everything after this many seconds of no input. 0 = never.");
    sec(PG_GENERAL, L"GREEN SCREEN AND STREAM WINDOW");
    static const wchar_t *const edges[] = { L"Smooth", L"Hard" }, *const bgs[] = { L"Green", L"Magenta", L"Black" }, *const sizes[] = { L"Same as screen", L"Custom" };
    static wchar_t sc1[48], sc2[48];
    static const wchar_t *screens[] = { L"Game screen", sc1, sc2, L"Follow mouse", L"Follow League client" };
    {   update_monitors(); RECT r2;
        swprintf(sc1, 48, L"1: %ldx%ld (main)", g_prim.right - g_prim.left, g_prim.bottom - g_prim.top);
        if (second_screen(&r2)) swprintf(sc2, 48, L"2: %ldx%ld", r2.right - r2.left, r2.bottom - r2.top); else swprintf(sc2, 48, L"2: none");
    }
    r_seg_grid(PG_GENERAL, L"Screen to show", ID_CH_SCREEN, screens, 5, 2, L"League client: lines up with a client source stretched to fill the canvas.");
    r_seg(PG_GENERAL, L"Edges", ID_CH_EDGES, edges, 2, L"Smooth: use the Chroma Key filter. Hard: use the Color Key filter.");
    r_seg(PG_GENERAL, L"Background", ID_CH_BG, bgs, 3, L"The colour your filter removes. Black works with no filter if Keys and APM box Background are Dark or Custom colour.");
    r_swatch(PG_GENERAL, L"Exact colour", ID_CHROMA_COL, L"Any other background colour.");
    r_seg(PG_GENERAL, L"Window size", ID_CH_SIZE, sizes, 2, L"Same as screen keeps it sharp in OBS. Custom: drag the window edges.");
    show_if(4);
    {
        static const wchar_t *const desks[] = { L"See-through", L"Black" };
        r_seg(PG_GENERAL, L"Stream window on my screen", ID_STREAM_DESK, desks, 2,
              L"See-through leaves your desktop visible. Pick Black if a capture app needs a solid window.");
    }
    r_toggle(PG_GENERAL, L"Show where the stream window is", ID_STREAM_GUIDE,
             L"A dashed outline on your screen marks the window. It is not captured on stream.");
    show_if(0);
    r_toggle(PG_GENERAL, L"Corner marks", ID_CH_MARKS, L"Marks the 4 corners and the middle. On stream they must sit in the corners.");
    sec(PG_GENERAL, L"CONTROLS");
    r_seg(PG_GENERAL, L"Input mode", ID_INPUT, inputs, 2, L"Use Normal. Try Backup only if APM stays at 0.");
    static const wchar_t *const rates[] = { L"60", L"120", L"144", L"Screen" };
    r_seg(PG_GENERAL, L"Redraws per second", ID_MAXFPS, rates, 4, L"60 matches stream video and uses the least CPU. Screen = every refresh.");
    lab(PG_GENERAL, L"Show / hide shortcut"); wd_add(W_HOTKEY, PG_GENERAL, ID_HOTKEY, CX, row_y + 2, 200, 28, NULL);
    hint(PG_GENERAL, L"Click the box, then press the keys you want.", 0, 32);
    row_y -= 10; wd_add(W_STATUS, PG_GENERAL, 0, RL, row_y - 4, RX - RL, 20, NULL); row_y += 26;
    sec(PG_GENERAL, L"MOUSE POSITION");
    r_toggle(PG_GENERAL, L"Check position", ID_ALIGN, L"Shows a crosshair and numbers at the mouse. Screenshot it if the trail is off.");
    r_slider(PG_GENERAL, L"Move left / right", ID_FIX_DX, L"Moves the trail if it is off by the same amount everywhere.");
    r_slider(PG_GENERAL, L"Move up / down", ID_FIX_DY, L"Moves the trail if it is off by the same amount everywhere.");
    r_slider(PG_GENERAL, L"Stretch left / right", ID_FIX_SX, L"Use if the trail is right on the left but drifts off to the right.");
    r_slider(PG_GENERAL, L"Stretch up / down", ID_FIX_SY, L"Use if the trail is right at the top but drifts off lower down.");
    lab(PG_GENERAL, L"Undo"); wd_add(W_BUTTON, PG_GENERAL, ID_FIX_RESET, CX, row_y, 132, 32, L"Back to normal");
    hint(PG_GENERAL, L"Normally leave these at 0 and 100%. Shift + scroll on a slider = small steps.", 0, 32);
    sec(PG_GENERAL, L"APM");
    lab(PG_GENERAL, L"Start over"); wd_add(W_BUTTON, PG_GENERAL, ID_APM_RESET, CX, row_y, 132, 32, L"Reset APM");
    hint(PG_GENERAL, L"Clears the counter and the average.", 0, 32);

    end_card();
    wd_add(W_SECTION, -1, 0, PV_X, CONTENT_Y, PV_W, 18, L"LIVE PREVIEW");
    wd_add(W_NOTE, -1, 1, PV_X + 120, CONTENT_Y, PV_W - 120, 18, L"Changes save automatically");
    wd_add(W_SECTION, -1, 0, PV_X, CONTENT_Y + 24 + PV_H + 16, PV_W, 18, L"STYLE");
    {
        static const wchar_t *const pn[] = { L"Clean dark", L"Outline", L"Broadcast", L"Light", L"Slanted" };
        int bw = (PV_W - 2 * 8) / 3, bw2 = (PV_W - 8) / 2, y0 = CONTENT_Y + 24 + PV_H + 40;
        for (int i = 0; i < 3; i++) wd_add(W_BUTTON, -1, ID_PRESET0 + i, PV_X + i * (bw + 8), y0, i == 2 ? PV_W - 2 * (bw + 8) : bw, 32, pn[i]);
        for (int i = 0; i < 2; i++) wd_add(W_BUTTON, -1, ID_PRESET3 + i, PV_X + i * (bw2 + 8), y0 + 38, i == 1 ? PV_W - bw2 - 8 : bw2, 32, pn[3 + i]);
    }
    wd_add(W_BUTTON, -1, ID_EXIT, LX, SET_H - 56, 148, 32, L"Exit program");
    wd_add(W_NOTE, -1, 0, LX + 156, SET_H - 54, PANE_X - LX - 168, 28, L"Closes the overlay completely.");
    { int dw = 112, dh = 45, gap = 8, rem = PV_W - dw - gap, fw = (rem - gap) / 2;
      int dy = SET_H - 24 - dh, oy = dy + (dh - 32) / 2;
      wd_add(W_BUTTON, -1, ID_CLOSE, PV_X, dy, dw, dh, L"Done");
      wd_add(W_BUTTON, -1, ID_RESET, PV_X + dw + gap, oy, fw, 32, L"Reset to defaults");
      wd_add(W_BUTTON, -1, ID_OPENINI, PV_X + dw + gap + fw + gap, oy, PV_W - (dw + 2 * gap + fw), 32, L"Open settings file");
      wd_add(W_HINT, -1, 0, PV_X, SET_H - 20, PV_W, 18, L"Locks the overlay in place if you were moving things."); }

    g_preview = CreateWindowExW(0, L"MAOPreview", L"", WS_CHILD | WS_VISIBLE, SC(PV_X), SC(CONTENT_Y + 24), SC(PV_W), SC(PV_H),
                                g_set, (HMENU)(INT_PTR)ID_PREVIEW, g_inst, NULL);
    { RECT pc; GetClientRect(g_preview, &pc); surface_alloc(&g_pv, pc.right, pc.bottom, 0); preview_bg(&g_pv); }
    g_edit_x = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, 0, 0, 10, 10, g_set, (HMENU)(INT_PTR)ID_APM_OFFX, g_inst, NULL);
    g_edit_y = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, 0, 0, 10, 10, g_set, (HMENU)(INT_PTR)ID_APM_OFFY, g_inst, NULL);
    g_edit_lab = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 10, g_set, (HMENU)(INT_PTR)ID_LABEL_TXT, g_inst, NULL);
    SendMessageW(g_edit_x, WM_SETFONT, (WPARAM)g_f_body, TRUE); SendMessageW(g_edit_y, WM_SETFONT, (WPARAM)g_f_body, TRUE);
    SendMessageW(g_edit_lab, WM_SETFONT, (WPARAM)g_f_body, TRUE);
    SendMessageW(g_edit_x, EM_SETLIMITTEXT, 4, 0); SendMessageW(g_edit_y, EM_SETLIMITTEXT, 4, 0); SendMessageW(g_edit_lab, EM_SETLIMITTEXT, 10, 0);
    g_edit_kx = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, 0, 0, 10, 10, g_set, (HMENU)(INT_PTR)ID_KB_OFFX, g_inst, NULL);
    g_edit_ky = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, 0, 0, 10, 10, g_set, (HMENU)(INT_PTR)ID_KB_OFFY, g_inst, NULL);
    g_edit_kc = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 10, g_set, (HMENU)(INT_PTR)ID_KB_CUSTOM, g_inst, NULL);
    { HWND e[3] = { g_edit_kx, g_edit_ky, g_edit_kc }; for (int i = 0; i < 3; i++) SendMessageW(e[i], WM_SETFONT, (WPARAM)g_f_body, TRUE); }
    SendMessageW(g_edit_kx, EM_SETLIMITTEXT, 4, 0); SendMessageW(g_edit_ky, EM_SETLIMITTEXT, 4, 0); SendMessageW(g_edit_kc, EM_SETLIMITTEXT, 2040, 0);
    g_edit_mx = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, 0, 0, 10, 10, g_set, (HMENU)(INT_PTR)ID_MS_OFFX, g_inst, NULL);
    g_edit_my = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, 0, 0, 10, 10, g_set, (HMENU)(INT_PTR)ID_MS_OFFY, g_inst, NULL);
    { HWND e[2] = { g_edit_mx, g_edit_my }; for (int i = 0; i < 2; i++) { SendMessageW(e[i], WM_SETFONT, (WPARAM)g_f_body, TRUE); SendMessageW(e[i], EM_SETLIMITTEXT, 4, 0); } }
    (void)fx; (void)fy;
    show_page(PG_TRAIL);
    sync_controls();
    ShowWindow(g_set, SW_SHOWNORMAL);
    SetForegroundWindow(g_set);
    guide_update();
}

static void ui_rect(int x, int y, int w, int h, unsigned c) {
    IRect r = { x < 0 ? 0 : x, y < 0 ? 0 : y, x + w > g_ui.w ? g_ui.w : x + w, y + h > g_ui.h ? g_ui.h : y + h };
    uint32_t v = 0xFF000000u | (uint32_t)c;
    for (int yy = r.y0; yy < r.y1; yy++) for (int xx = r.x0; xx < r.x1; xx++) g_ui.C.px[(size_t)yy * g_ui.w + xx] = v;
}
static void ui_rr(int x, int y, int w, int h, float rad, unsigned c) {
    draw_round_rect(&g_ui.C, SF((float)x), SF((float)y), SF((float)w), SF((float)h), SF(rad), HEXF(c), 1.0f);
}
static void ui_rrf(float x, float y, float w, float h, float rad, RGBf c) { draw_round_rect(&g_ui.C, x, y, w, h, rad, c, 1.0f); }
static void ui_frame(int x, int y, int w, int h, float rad, unsigned fill, unsigned line) {
    ui_rr(x, y, w, h, rad, line);
    float b = (float)SC(1);
    draw_round_rect(&g_ui.C, SF((float)x) + b, SF((float)y) + b, SF((float)w) - 2 * b, SF((float)h) - 2 * b, SF(rad) - b, HEXF(fill), 1.0f);
}
static void ui_text(HFONT f, unsigned c, int x, int y, int w, int h, UINT fl, const wchar_t *s) {
    RECT r = { SC(x), SC(y), SC(x + w), SC(y + h) };
    SelectObject(g_ui.mdc, f); SetTextColor(g_ui.mdc, HEXC(c));
    DrawTextW(g_ui.mdc, s, -1, &r, fl | DT_NOPREFIX);
}
static int text_px(HFONT f, const wchar_t *s) {
    SIZE sz; SelectObject(g_ui.mdc, f); GetTextExtentPoint32W(g_ui.mdc, s, (int)wcslen(s), &sz); return sz.cx;
}
static int visible(const Wd *d) { return (d->page < 0 || d->page == g_cur_page) && wd_shown(d); }

static void slider_geom(const Wd *d, float *x0, float *x1) { *x0 = SF((float)d->x + 8); *x1 = SF((float)(d->x + d->w - 64)); }

static void paint_shapes(int content) {
    for (int i = 0; i < g_nwd; i++) {
        const Wd *d = &g_wd[i];
        if (!visible(d) || (d->page >= 0) != content) continue;
        int hot = (i == g_hot) || (i == g_active);
        switch (d->type) {
        case W_CARD: if (d->h > 0) ui_frame(d->x, d->y, d->w, d->h, 10, C_CARD, C_CARD_LINE); break;
        case W_ROW: if (i == g_hrow && d->h > 8) ui_rr(d->x, d->y + 3, d->w, d->h - 6, 7, C_ROW_HOT); break;
        case W_DIV: ui_rect(SC(d->x), SC(d->y), SC(d->w), 1, C_DIV); break;
        case W_SLIDER: {
            const SliderDef *s = slider_def(d->id); if (!s) break;
            float x0, x1; slider_geom(d, &x0, &x1);
            float cy = SF((float)d->y + d->h * 0.5f), th = SF(4);
            float u = (float)(slider_get_model(d->id) - s->lo) / (float)(s->hi - s->lo); u = u < 0 ? 0 : (u > 1 ? 1 : u);
            float kx = x0 + (x1 - x0) * u;
            ui_rrf(x0 - th * 0.5f, cy - th * 0.5f, x1 - x0 + th, th, th * 0.5f, HEXF(C_TRACK));
            ui_rrf(x0 - th * 0.5f, cy - th * 0.5f, kx - x0 + th, th, th * 0.5f, HEXF(C_ACCENT));
            if (hot) draw_disc(&g_ui.C, kx, cy, SF(10), HEXF(C_ACCENT), 0.18f);
            draw_disc(&g_ui.C, kx, cy, SF(7), HEXF(C_TEXT), 1.0f);
            break;
        }
        case W_SWATCH: case W_FIELD: case W_HOTKEY:
            ui_frame(d->x, d->y, d->w, d->h, 6, hot ? C_RAISED_HI : C_RAISED, (d->type == W_HOTKEY && g_rec) ? C_ACCENT : (hot ? C_LINE_HI : C_LINE));
            if (d->type == W_SWATCH) {
                RGBf *c = color_target(d->id);
                ui_rr(d->x + 7, d->y + 6, 16, 16, 4, C_LINE_HI);
                if (c) ui_rrf(SF((float)d->x + 8), SF((float)d->y + 7), SF(14), SF(14), SF(3), *c);
            }
            break;
        case W_TOGGLE: {
            int on = check_target(d->id) && *check_target(d->id);
            ui_rr(d->x, d->y, d->w, d->h, d->h * 0.5f, on ? (hot ? C_ACCENT_HI : C_ACCENT) : (hot ? C_LINE_HI : C_TRACK));
            float r = SF(7), cy = SF((float)d->y + d->h * 0.5f);
            float cx = on ? SF((float)(d->x + d->w) - 10) : SF((float)d->x + 10);
            draw_disc(&g_ui.C, cx, cy, r, HEXF(on ? C_ON_ACCENT : C_KNOB_OFF), 1.0f);
            break;
        }
        case W_SEG: {
            ui_frame(d->x, d->y, d->w, d->h, 6, C_RAISED, C_LINE);
            int cur = seg_get(d->id), rows = (d->n + d->cols - 1) / d->cols;
            float sw = (float)d->w / d->cols, sh = (float)d->h / rows;
            ui_rrf(SF(d->x + sw * (cur % d->cols) + 3), SF(d->y + sh * (cur / d->cols) + 3), SF(sw - 6), SF(sh - 6), SF(4), HEXF(C_SEG_ON));
            break;
        }
        case W_BUTTON: {
            int act = (d->id == ID_APM_MOVE || d->id == ID_KB_MOVE) && g_move;
            int ex = d->id == ID_EXIT, done = d->id == ID_CLOSE, down = i == g_active && i == g_hot;
            if (ex) ui_frame(d->x, d->y, d->w, d->h, 6, hot ? (down ? 0xA3261B : 0xC42B1C) : C_RAISED, hot ? 0xE04A3C : C_LINE);
            else if (done) ui_frame(d->x, d->y, d->w, d->h, 8, down ? C_DONE_DOWN : (hot ? C_DONE_HOT : C_DONE), down ? 0xB8892A : C_DONE_LINE);
            else ui_frame(d->x, d->y, d->w, d->h, 6, hot ? C_RAISED_HI : C_RAISED, act ? C_ACCENT : (hot ? C_LINE_HI : C_LINE));
            break;
        }
        case W_PRIMARY:
            ui_rr(d->x, d->y, d->w, d->h, 6, hot ? C_ACCENT_HI : C_ACCENT);
            break;
        case W_CAPBTN: {
            int close = d->id == ID_WIN_CLOSE, down = i == g_active && i == g_hot;
            if (hot && (i == g_hot)) ui_rect(SC(d->x), SC(d->y), SC(d->w), SC(d->h), close ? (down ? 0xA3261B : 0xC42B1C) : (down ? C_LINE_HI : C_RAISED_HI));
            RGBf ink = HEXF((close && i == g_hot) ? 0xFFFFFF : C_LABEL);
            float cx = SF(d->x + d->w * 0.5f), cy = SF(d->y + d->h * 0.5f), r = SF(5), lw = SF(1.1f);
            if (close) {
                draw_line(&g_ui.C, &g_ui.L, cx - r, cy - r, cx + r, cy + r, lw, ink, 1.0f);
                draw_line(&g_ui.C, &g_ui.L, cx - r, cy + r, cx + r, cy - r, lw, ink, 1.0f);
            } else {
                draw_line(&g_ui.C, &g_ui.L, cx - r, floorf(cy) + 0.5f, cx + r, floorf(cy) + 0.5f, lw, ink, 1.0f);
            }
            break;
        }
        }
    }
}
static void paint_texts(int content) {
    for (int i = 0; i < g_nwd; i++) {
        const Wd *d = &g_wd[i];
        if (!visible(d) || (d->page >= 0) != content) continue;
        int hot = (i == g_hot) || (i == g_active);
        wchar_t b[64];
        switch (d->type) {
        case W_TAB:
            ui_text(g_f_semi, d->id == g_cur_page ? C_TEXT : (hot ? C_LABEL : C_MUTED), d->x - 4, d->y, d->w + 8, d->h, DT_SINGLELINE | DT_VCENTER | DT_CENTER, d->text);
            break;
        case W_SECTION:
            SetTextCharacterExtra(g_ui.mdc, 1);
            ui_text(g_f_cap, C_MUTED, d->x, d->y, d->w, d->h, DT_SINGLELINE | DT_VCENTER, d->text);
            SetTextCharacterExtra(g_ui.mdc, 0);
            break;
        case W_LABEL: ui_text(g_f_body, C_LABEL, d->x, d->y, d->w, d->h, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS, d->text); break;
        case W_NOTE: ui_text(g_f_note, C_MUTED, d->x, d->y, d->w, d->h, d->h <= 28 ? (DT_SINGLELINE | DT_VCENTER | (d->id == 1 ? DT_RIGHT : 0)) : DT_WORDBREAK, d->text); break;
        case W_HINT: {
            RGBf *c = d->id ? color_target(d->id) : NULL;
            int warn = c && d->id != ID_CHROMA_COL && chroma_bg_index() != 2 && fabsf(c->r - S.chroma.r) + fabsf(c->g - S.chroma.g) + fabsf(c->b - S.chroma.b) < 0.12f;
            ui_text(g_f_hint, warn ? C_ACCENT : C_MUTED, d->x, d->y, d->w, d->h, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS,
                    warn ? L"Same as the green screen colour, so OBS would hide it. Pick another colour." : d->text);
            break;
        }
        case W_STATUS: ui_text(g_f_note, g_hk_ok ? C_MUTED : C_ACCENT, d->x, d->y, d->w, d->h, DT_WORDBREAK, g_status); break;
        case W_SLIDER: {
            const SliderDef *s = slider_def(d->id); if (!s) break;
            int v = slider_get_model(d->id);
            if (d->id == ID_IDLE && v == 0) wcscpy(b, L"Never");
            else if (s->div > 1) swprintf(b, 64, L"%g %ls", (double)v / s->div, s->unit); else swprintf(b, 64, L"%d %ls", v, s->unit);
            ui_text(g_f_body, C_TEXT, d->x + d->w - 56, d->y, 56, d->h, DT_SINGLELINE | DT_VCENTER | DT_RIGHT, b);
            break;
        }
        case W_SWATCH: {
            RGBf *c = color_target(d->id); if (!c) break;
            COLORREF r = to_cr(*c); swprintf(b, 64, L"#%02X%02X%02X", GetRValue(r), GetGValue(r), GetBValue(r));
            ui_text(g_f_body, C_TEXT, d->x + 32, d->y, d->w - 36, d->h, DT_SINGLELINE | DT_VCENTER, b);
            break;
        }
        case W_FIELD: ui_text(g_f_body, C_MUTED, d->x + 10, d->y, 16, d->h, DT_SINGLELINE | DT_VCENTER, d->text); break;
        case W_HOTKEY:
            if (g_rec) ui_text(g_f_body, C_ACCENT, d->x + 10, d->y, d->w - 20, d->h, DT_SINGLELINE | DT_VCENTER, L"Press a key combination");
            else { hotkey_text(S.hk_mods, S.hk_vk, b, 64); ui_text(g_f_body, C_TEXT, d->x + 10, d->y, d->w - 20, d->h, DT_SINGLELINE | DT_VCENTER, b); }
            break;
        case W_SEG: {
            int cur = seg_get(d->id), rows = (d->n + d->cols - 1) / d->cols;
            float sw = (float)d->w / d->cols, sh = (float)d->h / rows;
            for (int k = 0; k < d->n; k++) {
                int c = k % d->cols, r = k / d->cols;
                int x0 = d->x + (int)lroundf(sw * c), x1 = d->x + (int)lroundf(sw * (c + 1));
                int y0 = d->y + (int)lroundf(sh * r), y1 = d->y + (int)lroundf(sh * (r + 1));
                ui_text(k == cur ? g_f_semi : g_f_body, k == cur ? C_TEXT : C_LABEL, x0, y0, x1 - x0, y1 - y0, DT_SINGLELINE | DT_VCENTER | DT_CENTER, d->items[k]);
            }
            break;
        }
        case W_BUTTON: {
            const wchar_t *t = d->text;
            if ((d->id == ID_APM_MOVE || d->id == ID_KB_MOVE) && g_move) t = L"Done moving";
            if (d->id == ID_CLOSE && g_saved_flash) t = L"Saved";
            unsigned ink = d->id == ID_EXIT && i == g_hot ? 0xFFFFFF
                : ((d->id == ID_APM_MOVE || d->id == ID_KB_MOVE) && g_move) ? C_ACCENT
                : d->id == ID_CLOSE ? C_DONE_INK : C_TEXT;
            ui_text(d->id == ID_CLOSE ? g_f_done : g_f_semi, ink, d->x, d->y, d->w, d->h, DT_SINGLELINE | DT_VCENTER | DT_CENTER, t);
            break;
        }
        case W_PRIMARY: ui_text(g_f_semi, C_ON_ACCENT, d->x, d->y, d->w, d->h, DT_SINGLELINE | DT_VCENTER | DT_CENTER, d->text); break;
        }
        (void)hot;
    }
}
static void ui_paint(HWND h) {
    PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
    RECT cr; GetClientRect(h, &cr);
    if (!g_ui.mdc || g_ui.w != cr.right || g_ui.h != cr.bottom) surface_alloc(&g_ui, cr.right, cr.bottom, 1);
    if (!g_ui.mdc) { EndPaint(h, &ps); return; }
    GdiFlush();
    ui_rect(0, 0, g_ui.w, g_ui.h, C_BG);
    SetBkMode(g_ui.mdc, TRANSPARENT);
    layout_scroll();
    /* scrolling content first, clipped below the tab bar; then the fixed chrome on top */
    HRGN clip = CreateRectRgn(0, SC(VIEW_Y), SC(PANE_X), SC(FOOT_Y));
    SelectClipRgn(g_ui.mdc, clip);
    paint_shapes(1); GdiFlush(); paint_texts(1);
    SelectClipRgn(g_ui.mdc, NULL); DeleteObject(clip);
    GdiFlush();
    ui_rect(0, SC(FOOT_Y), SC(PANE_X), g_ui.h - SC(FOOT_Y), C_BG);
    ui_rect(0, SC(FOOT_Y), SC(PANE_X), 1, C_LINE);
    ui_rect(0, 0, SC(PANE_X), SC(VIEW_Y), C_BG);
    ui_rect(0, SC(TITLE_H), g_ui.w, SC(1) > 0 ? SC(1) : 1, C_LINE);
    ui_rect(0, SC(TABS_Y + TABS_H), SC(PANE_X), 1, C_LINE);
    ui_rect(SC(PANE_X), SC(TITLE_H), 1, g_ui.h - SC(TITLE_H), C_LINE);
    {
        int m = max_scroll();
        if (m > 0) {
            int vh = FOOT_Y - VIEW_Y, th = vh * vh / (vh + m); if (th < 32) th = 32;
            int ty = VIEW_Y + (vh - th) * g_scroll / m;
            ui_rr(PANE_X - 8, ty + 2, 4, th - 4, 2, C_LINE_HI);
        }
    }

    int tx = LX;
    for (int i = 0; i < g_nwd; i++) {
        Wd *d = &g_wd[i];
        if (d->type != W_TAB) continue;
        int tw = (text_px(g_f_semi, d->text) * 96 + g_dpi - 1) / g_dpi;
        d->x = tx; d->w = tw; d->hx = tx - 10; d->hw = tw + 20; d->hy = TABS_Y; d->hh = TABS_H;
        if (d->id == g_cur_page) ui_rr(tx, TABS_Y + TABS_H - 2, tw, 2, 1.0f, C_ACCENT);
        tx += tw + 28;
    }
    paint_shapes(0);
    GdiFlush();
    HICON ic = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, SC(20), SC(20), 0);
    if (ic) { DrawIconEx(g_ui.mdc, SC(LX), SC(14), ic, SC(20), SC(20), 0, NULL, DI_NORMAL); DestroyIcon(ic); }
    {
        int x = LX + 30, w255 = (text_px(g_f_title, L"255") * 96) / g_dpi;
        ui_text(g_f_title, C_TEXT, x, 0, 60, TITLE_H, DT_SINGLELINE | DT_VCENTER, L"255");
        ui_text(g_f_title, C_LABEL, x + w255 + 6, 0, 260, TITLE_H, DT_SINGLELINE | DT_VCENTER, L"Input Overlay");
        ui_text(g_f_body, C_MUTED, SET_W - 92 - 216, 0, 200, TITLE_H, DT_SINGLELINE | DT_VCENTER | DT_RIGHT, L"softcap.gg");
    }
    paint_texts(0);
    GdiFlush();
    ui_rect(0, 0, g_ui.w, 1, C_LINE); ui_rect(0, g_ui.h - 1, g_ui.w, 1, C_LINE);
    ui_rect(0, 0, 1, g_ui.h, C_LINE); ui_rect(g_ui.w - 1, 0, 1, g_ui.h, C_LINE);
    BitBlt(dc, 0, 0, g_ui.w, g_ui.h, g_ui.mdc, 0, 0, SRCCOPY);
    EndPaint(h, &ps);
}

static void sync_controls(void) {
    if (!g_set) return;
    g_syncing = 1;
    wchar_t b[16];
    swprintf(b, 16, L"%u", (unsigned)lroundf(S.st.apm_off_x)); SetWindowTextW(g_edit_x, b);
    swprintf(b, 16, L"%u", (unsigned)lroundf(S.st.apm_off_y)); SetWindowTextW(g_edit_y, b);
    { int i = 0; for (; S.st.label[i] && i < 15; i++) b[i] = (wchar_t)(unsigned char)S.st.label[i]; b[i] = 0; SetWindowTextW(g_edit_lab, b); }
    swprintf(b, 16, L"%u", (unsigned)lroundf(S.st.keys_off_x)); SetWindowTextW(g_edit_kx, b);
    swprintf(b, 16, L"%u", (unsigned)lroundf(S.st.keys_off_y)); SetWindowTextW(g_edit_ky, b);
    swprintf(b, 16, L"%u", (unsigned)lroundf(S.st.mouse_off_x)); SetWindowTextW(g_edit_mx, b);
    swprintf(b, 16, L"%u", (unsigned)lroundf(S.st.mouse_off_y)); SetWindowTextW(g_edit_my, b);
    { static wchar_t kc[2048]; int i = 0; for (; S.st.keys_custom[i] && i < 2047; i++) kc[i] = (wchar_t)(unsigned char)S.st.keys_custom[i]; kc[i] = 0; SetWindowTextW(g_edit_kc, kc); }
    g_syncing = 0;
    settings_status();
    InvalidateRect(g_set, NULL, FALSE);
}

static RGBf *g_cc_target;
static UINT_PTR CALLBACK cc_hook(HWND h, UINT msg, WPARAM w, LPARAM l) {
    (void)l;
    if (msg == WM_INITDIALOG) dark_title(h);
    if (msg == WM_COMMAND && HIWORD(w) == EN_CHANGE && LOWORD(w) >= 706 && LOWORD(w) <= 708 && g_cc_target) {
        BOOL o1, o2, o3;
        UINT r = GetDlgItemInt(h, 706, &o1, FALSE), g = GetDlgItemInt(h, 707, &o2, FALSE), b = GetDlgItemInt(h, 708, &o3, FALSE);
        if (o1 && o2 && o3 && r < 256 && g < 256 && b < 256) {
            *g_cc_target = from_cr(RGB(r, g, b)); g_force = 1;
            if (g_set) InvalidateRect(g_set, NULL, FALSE);
        }
    }
    return 0;
}
static void pick_color(int id) {
    static COLORREF custom[16];
    RGBf *t = color_target(id); if (!t) return;
    RGBf before = *t;
    CHOOSECOLORW cc; memset(&cc, 0, sizeof(cc));
    cc.lStructSize = sizeof(cc); cc.hwndOwner = g_set; cc.rgbResult = to_cr(*t); cc.lpCustColors = custom;
    cc.Flags = CC_RGBINIT | CC_FULLOPEN | CC_ENABLEHOOK; cc.lpfnHook = cc_hook;
    g_cc_target = t;
    if (ChooseColorW(&cc)) *t = from_cr(cc.rgbResult); else *t = before;
    g_cc_target = NULL;
    if (g_set) InvalidateRect(g_set, NULL, FALSE);
    apply_change(0);
}

static void apply_change(int structural) {
    g_force = 1;
    if (g_ked) InvalidateRect(g_ked, NULL, FALSE); /* the key editor shows the current look */
    if (structural) display_create();
    if (g_set) layout_relayout();
    SetTimer(g_main, TIMER_SAVE, 400, NULL);
    settings_status();
    guide_update();
}

static void set_move_mode(int on) {
    g_move = on; g_drag = 0; g_force = 1;
    if (g_overlay) {
        LONG_PTR ex = GetWindowLongPtrW(g_overlay, GWL_EXSTYLE);
        ex = on ? (ex & ~(LONG_PTR)WS_EX_TRANSPARENT) : (ex | WS_EX_TRANSPARENT);
        SetWindowLongPtrW(g_overlay, GWL_EXSTYLE, ex);
        if (on && g_hidden) set_hidden(0);
        g_ov_first = 1; g_prev_bb = (IRect){ 0, 0, g_ov.w, g_ov.h };
    }
    chroma_apply_click_through();
    if (g_set) InvalidateRect(g_set, NULL, FALSE);
    if (!on) SetTimer(g_main, TIMER_SAVE, 200, NULL);
    guide_update();
}
static void drag_box_to(BoxLay *b, float nx, float ny) {
    if (nx > b->ax + b->aw - b->pw) nx = b->ax + b->aw - b->pw;
    if (ny > b->ay + b->ah - b->ph) ny = b->ay + b->ah - b->ph;
    if (nx < b->ax) nx = b->ax;
    if (ny < b->ay) ny = b->ay;
    int left = nx + b->pw * 0.5f < b->ax + b->aw * 0.5f, top = ny + b->ph * 0.5f < b->ay + b->ah * 0.5f;
    float ox = left ? nx - b->ax : b->ax + b->aw - (nx + b->pw);
    float oy = top ? ny - b->ay : b->ay + b->ah - (ny + b->ph);
    int *corner = b->which == 2 ? &S.st.mouse_corner : b->which ? &S.st.keys_corner : &S.st.apm_corner;
    float *offx = b->which == 2 ? &S.st.mouse_off_x : b->which ? &S.st.keys_off_x : &S.st.apm_off_x;
    float *offy = b->which == 2 ? &S.st.mouse_off_y : b->which ? &S.st.keys_off_y : &S.st.apm_off_y;
    *corner = top ? (left ? APM_TL : APM_TR) : (left ? APM_BL : APM_BR);
    *offx = roundf(ox / b->k); *offy = roundf(oy / b->k);
    if (*offx < 0) *offx = 0;
    if (*offy < 0) *offy = 0;
    b->px = nx; b->py = ny; g_force = 1;
}
/* when the mouse is split off, keep it where it was drawn next to the keys */
static void mouse_split_here(void) {
    BoxLay *b = S.mode ? &g_ch_mlay : &g_ov_mlay;
    if (b->pw > 0 && b->ph > 0 && b->aw > 0) { BoxLay t = *b; t.which = 2; drag_box_to(&t, t.px, t.py); }
    sync_controls();
}
static int box_mouse(HWND h, BoxLay *b, UINT msg, LPARAM l) {
    float x = (float)GET_X_LPARAM(l), y = (float)GET_Y_LPARAM(l);
    switch (msg) {
    case WM_LBUTTONDOWN:
        if (g_move && !g_drag && lay_hit(b, x, y)) {
            g_drag = 1; g_drag_lay = b; g_grab_dx = x - b->px; g_grab_dy = y - b->py; SetCapture(h); return 1;
        }
        return 0;
    case WM_MOUSEMOVE:
        if (g_drag && g_drag_lay == b) { drag_box_to(b, x - g_grab_dx, y - g_grab_dy); return 1; }
        return 0;
    case WM_LBUTTONUP:
        if (g_drag && g_drag_lay == b) {
            drag_box_to(b, x - g_grab_dx, y - g_grab_dy); g_drag = 0; ReleaseCapture();
            sync_controls(); SetTimer(g_main, TIMER_SAVE, 200, NULL); return 1;
        }
        return 0;
    case WM_CAPTURECHANGED: if (g_drag_lay == b) g_drag = 0; return 0;
    }
    return 0;
}
static void reset_apm(void) { EnterCriticalSection(&g_lock); apm_init(&g_apm, now_s()); LeaveCriticalSection(&g_lock); g_force = 1; }

static int ui_hit(int px, int py) {
    for (int i = 0; i < g_nwd; i++) {
        const Wd *d = &g_wd[i];
        if (!visible(d)) continue;
        if (d->type == W_SECTION || d->type == W_LABEL || d->type == W_NOTE || d->type == W_STATUS || d->type == W_FIELD || d->type == W_HINT ||
            d->type == W_CARD || d->type == W_ROW || d->type == W_DIV) continue;
        if (d->page >= 0 && (py < SC(VIEW_Y) || py >= SC(FOOT_Y) || px >= SC(PANE_X))) continue;
        if (px >= SC(d->hx) && px < SC(d->hx + d->hw) && py >= SC(d->hy) && py < SC(d->hy + d->hh)) return i;
    }
    return -1;
}
static void slider_from_x(const Wd *d, int px) {
    const SliderDef *s = slider_def(d->id); if (!s) return;
    float x0, x1; slider_geom(d, &x0, &x1);
    float u = ((float)px - x0) / (x1 - x0); u = u < 0 ? 0 : (u > 1 ? 1 : u);
    int v = s->lo + (int)lroundf(u * (float)(s->hi - s->lo));
    if (v != slider_get_model(d->id)) { slider_set_model(d->id, v); apply_change(0); }
    InvalidateRect(g_set, NULL, FALSE);
}
static void ui_activate(const Wd *d, int px) {
    switch (d->type) {
    case W_TAB: if (d->id != g_cur_page) show_page(d->id); break;
    case W_TOGGLE: { int *t = check_target(d->id); if (t) { *t = !*t; if (d->id == ID_MS_ATTACH && !*t) mouse_split_here(); apply_change(0); } break; }
    case W_SEG: {
        int rows = (d->n + d->cols - 1) / d->cols;
        int c = (int)((px - SC(d->x)) * d->cols / (SC(d->w) > 0 ? SC(d->w) : 1)), r = (int)((g_last_py - SC(d->y)) * rows / (SC(d->h) > 0 ? SC(d->h) : 1));
        if (c < 0) c = 0; if (c >= d->cols) c = d->cols - 1; if (r < 0) r = 0; if (r >= rows) r = rows - 1;
        int k = r * d->cols + c; if (k >= d->n) k = d->n - 1;
        switch (d->id) {
        case ID_CLICK_STYLE: S.st.click_style = k; apply_change(0); break;
        case ID_APM_POS: S.st.apm_corner = k; apply_change(0); break;
        case ID_MODE: if (k != S.mode) { S.mode = k; g_hidden = 0; apply_change(1); } break;
        case ID_CH_EDGES: S.chroma_hard = k; apply_change(0); break;
        case ID_MAXFPS: S.max_draw = k == 0 ? 60 : k == 1 ? 120 : k == 2 ? 144 : 0; apply_change(0); break;
        case ID_CH_BG: S.chroma = k == 1 ? (RGBf){ 1, 0, 1 } : k == 2 ? (RGBf){ 0, 0, 0 } : (RGBf){ 0, 1, 0 }; apply_change(0); break;
        case ID_CH_SIZE: S.chroma_fit = k == 0; apply_change(S.mode != 0); break;
        case ID_CH_SCREEN: S.chroma_screen = k == 4 ? 5 : k; S.chroma_virtual = 0; S.chroma_fit = 1; sync_controls(); apply_change(S.mode != 0); break; /* always: back to that screen's exact size */
        case ID_INPUT: S.input_hook = k == 1; input_start(); apply_change(0); break;
        case ID_TRAIL_SMOOTH: S.st.trail_smooth = k; apply_change(0); break;
        case ID_TRAIL_GRAD: S.st.trail_grad = k; apply_change(0); break;
        case ID_GLOW_MODE: S.st.glow_mode = k; apply_change(0); break;
        case ID_FONT: S.st.font = k; apply_change(0); break;
        case ID_LAB_POS: S.st.lab_below = k; apply_change(0); break;
        case ID_KB_LAYOUT: { static const int lay[5] = { 1, 0, 3, 4, 2 }; S.st.keys_layout = lay[k % 5]; apply_change(0); break; }
        case ID_KB_SHAPE: S.st.keys_shape = k; apply_change(0); break;
        case ID_KB_POS: S.st.keys_corner = k; apply_change(0); break;
        case ID_MS_POS: S.st.mouse_corner = k; apply_change(0); break;
        case ID_KB_FONT: S.st.keys_font = k + 1; apply_change(0); break;
        case ID_KB_BG:
            S.keys_bg = k;
            if (k == 0) { S.st.keys_plate = 0; S.st.keys_depth = 0; }
            else S.st.keys_plate = 1;
            apply_change(0); break;
        case ID_APM_BGMODE:
            S.apm_bg = k;
            if (k == 0) { S.st.panel_enabled = 0; S.st.text_outline = 1; }
            else S.st.panel_enabled = 1;
            apply_change(0); break;
        case ID_STREAM_DESK:
            if ((k == 0) != S.stream_clear) { S.stream_clear = (k == 0); apply_change(S.mode == 2); }
            break;
        }
        break;
    }
    case W_SWATCH: pick_color(d->id); break;
    case W_HOTKEY: g_rec = 1; SetFocus(g_set); break;
    case W_BUTTON: case W_PRIMARY: case W_CAPBTN:
        switch (d->id) {
        case ID_APM_MOVE: case ID_KB_MOVE: set_move_mode(!g_move); break;
        case ID_KB_EDIT: keyed_open(); break;
        case ID_APM_RESET: reset_apm(); break;
        case ID_PRESET0: case ID_PRESET1: case ID_PRESET2: case ID_PRESET3: case ID_PRESET4:
            apply_preset(&S, d->id - ID_PRESET0); sync_controls(); apply_change(0); break;
        case ID_FIX_RESET: S.fix_dx = S.fix_dy = 0; S.fix_sx = S.fix_sy = 1.0f; apply_change(0); break;
        case ID_WIN_MIN:
            g_active = -1; g_hot = -1; ReleaseCapture();
            g_guide_flash = 0; g_guide_quiet = GetTickCount() + 500;
            if (g_main) KillTimer(g_main, TIMER_GUIDE);
            ShowWindow(g_set, SW_MINIMIZE); guide_update(); return;
        case ID_WIN_CLOSE: g_active = -1; g_hot = -1; ReleaseCapture(); app_quit(); return;
        case ID_RESET: { int mode = S.mode; defaults(&S); S.mode = mode; register_hotkey(); input_start(); sync_controls(); apply_change(S.mode != 0); break; }
        case ID_OPENINI: save_settings(); ShellExecuteW(g_set, L"open", L"notepad.exe", g_ini, NULL, SW_SHOWNORMAL); break;
        case ID_CLOSE: {
            int moving = g_move;
            if (g_move) set_move_mode(0);
            save_settings();
            if (!moving) { g_saved_flash = 1; SetTimer(g_main, TIMER_STATUS, 1400, NULL); }
            if (g_set) InvalidateRect(g_set, NULL, FALSE);
            break;
        }
        case ID_EXIT: g_active = -1; g_hot = -1; ReleaseCapture(); app_quit(); return;
        }
        break;
    }
    if (g_set) InvalidateRect(g_set, NULL, FALSE);
}
static void hotkey_key(UINT vk) {
    if (vk == VK_ESCAPE) { g_rec = 0; InvalidateRect(g_set, NULL, FALSE); return; }
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN ||
        vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU) return;
    UINT mods = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) mods |= MOD_SHIFT;
    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) & 0x8000) mods |= MOD_ALT;
    int fn_only = vk >= VK_F13 && vk <= VK_F24;
    if (mods || fn_only || vk == VK_PAUSE || vk == VK_SCROLL) {
        S.hk_mods = mods; S.hk_vk = vk; register_hotkey(); apply_change(0);
        g_rec = 0;
    }
    InvalidateRect(g_set, NULL, FALSE);
}

static LRESULT CALLBACK settings_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_NCCALCSIZE: if (w) return 0; break; /* no Windows title bar: the client area is the whole window */
    case WM_NCHITTEST: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ScreenToClient(h, &p);
        RECT c; GetClientRect(h, &c);
        if (p.x < 0 || p.y < 0 || p.x >= c.right || p.y >= c.bottom) break;
        if (p.y < SC(TITLE_H)) {
            int i = ui_hit(p.x, p.y);
            return (i >= 0 && g_wd[i].type == W_CAPBTN) ? HTCLIENT : HTCAPTION;
        }
        return HTCLIENT;
    }
    case WM_NCACTIVATE: return DefWindowProcW(h, msg, w, -1); /* nothing to repaint outside the client area */
    case WM_ERASEBKGND: return 1;
    case WM_MOVE: case WM_EXITSIZEMOVE: settings_remember(); break;
    case WM_SIZE:
        g_set_iconic = (w == SIZE_MINIMIZED);
        if (g_set_iconic) {
            g_guide_flash = 0; g_guide_quiet = GetTickCount() + 500;
            if (g_main) KillTimer(g_main, TIMER_GUIDE);
        }
        guide_update();
        break;
    case WM_PAINT: ui_paint(h); return 0;
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)w, HEXC(C_TEXT)); SetBkColor((HDC)w, HEXC(C_RAISED));
        return (LRESULT)g_br_raised;
    case WM_MOUSEMOVE: {
        int px = GET_X_LPARAM(l), py = GET_Y_LPARAM(l);
        if (g_active >= 0) { if (g_wd[g_active].type == W_SLIDER) slider_from_x(&g_wd[g_active], px); return 0; }
        int hot = ui_hit(px, py), hr = -1;
        if (py >= SC(VIEW_Y) && px < SC(PANE_X))
            for (int i = 0; i < g_nwd; i++) { const Wd *d = &g_wd[i];
                if (d->type == W_ROW && d->page == g_cur_page && wd_shown(d) && px >= SC(d->x) && px < SC(d->x + d->w) && py >= SC(d->y) && py < SC(d->y + d->h)) { hr = i; break; } }
        if (hr != g_hrow) { g_hrow = hr; InvalidateRect(h, NULL, FALSE); TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t); }
        if (hot != g_hot) {
            g_hot = hot; InvalidateRect(h, NULL, FALSE);
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
        }
        return 0;
    }
    case WM_MOUSELEAVE: if ((g_hot >= 0 || g_hrow >= 0) && g_active < 0) { g_hot = -1; g_hrow = -1; InvalidateRect(h, NULL, FALSE); } return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT && g_hot >= 0) { SetCursor(LoadCursorW(NULL, g_wd[g_hot].type == W_CAPBTN ? IDC_ARROW : IDC_HAND)); return TRUE; }
        break;
    case WM_LBUTTONDOWN: {
        int px = GET_X_LPARAM(l), py = GET_Y_LPARAM(l);
        int i = ui_hit(px, py);
        g_last_py = py;
        if (g_rec && (i < 0 || g_wd[i].type != W_HOTKEY)) { g_rec = 0; InvalidateRect(h, NULL, FALSE); }
        SetFocus(h);
        if (i < 0) return 0;
        g_active = i; SetCapture(h);
        if (g_wd[i].type == W_SLIDER) slider_from_x(&g_wd[i], px);
        else if (g_wd[i].type == W_TOGGLE || g_wd[i].type == W_SEG || g_wd[i].type == W_TAB || g_wd[i].type == W_HOTKEY) ui_activate(&g_wd[i], px);
        return 0;
    }
    case WM_LBUTTONUP: {
        int i = g_active; g_active = -1; ReleaseCapture();
        if (i >= 0 && (g_wd[i].type == W_SWATCH || g_wd[i].type == W_BUTTON || g_wd[i].type == W_PRIMARY || g_wd[i].type == W_CAPBTN) &&
            ui_hit(GET_X_LPARAM(l), GET_Y_LPARAM(l)) == i) ui_activate(&g_wd[i], GET_X_LPARAM(l));
        if (g_set) InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_CAPTURECHANGED: g_active = -1; return 0;
    case WM_NCLBUTTONDOWN: case WM_NCLBUTTONUP: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ScreenToClient(h, &p);
        int i = ui_hit(p.x, p.y);
        if (i >= 0 && g_wd[i].type == W_CAPBTN) {
            if (msg == WM_NCLBUTTONUP) ui_activate(&g_wd[i], p.x);
            return 0;
        }
        if (w == HTCLOSE) { if (msg == WM_NCLBUTTONUP) app_quit(); return 0; }
        break;
    }
    case WM_MOUSEWHEEL: {
        /* the wheel scrolls the settings list; sliders are changed by dragging, so scrolling never edits a value */
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ScreenToClient(h, &p);
        if (GET_KEYSTATE_WPARAM(w) & MK_SHIFT) { /* fine adjust */
            int i = ui_hit(p.x, p.y);
            if (i >= 0 && g_wd[i].type == W_SLIDER) {
                const SliderDef *sd = slider_def(g_wd[i].id);
                int v = slider_get_model(sd->id) + (GET_WHEEL_DELTA_WPARAM(w) > 0 ? 1 : -1);
                if (v < sd->lo) v = sd->lo; if (v > sd->hi) v = sd->hi;
                slider_set_model(sd->id, v); apply_change(0); InvalidateRect(h, NULL, FALSE);
            }
            return 0;
        }
        if (p.x < SC(PANE_X) && g_active < 0) {
            int old = g_scroll;
            g_scroll -= GET_WHEEL_DELTA_WPARAM(w) * 56 / WHEEL_DELTA;
            layout_scroll();
            if (g_scroll != old) {
                g_hot = ui_hit(p.x, p.y);
                InvalidateRect(h, NULL, FALSE);
            }
        }
        return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (g_rec) { hotkey_key((UINT)w); return 0; }
        if (msg == WM_SYSKEYDOWN && w == VK_F4) { app_quit(); return 0; }
        if (w == VK_ESCAPE) return 0;
        break;
    case WM_SYSCHAR: case WM_CHAR: if (g_rec) return 0; break;
    case WM_COMMAND: {
        if (g_syncing) break;
        int id = LOWORD(w), code = HIWORD(w);
        if ((id == ID_APM_OFFX || id == ID_APM_OFFY) && code == EN_CHANGE) {
            BOOL ok; UINT v = GetDlgItemInt(h, id, &ok, FALSE);
            if (ok) { if (v > 8000) v = 8000; if (id == ID_APM_OFFX) S.st.apm_off_x = (float)v; else S.st.apm_off_y = (float)v; apply_change(0); }
            return 0;
        }
        if ((id == ID_KB_OFFX || id == ID_KB_OFFY) && code == EN_CHANGE) {
            BOOL ok; UINT v = GetDlgItemInt(h, id, &ok, FALSE);
            if (ok) { if (v > 8000) v = 8000; if (id == ID_KB_OFFX) S.st.keys_off_x = (float)v; else S.st.keys_off_y = (float)v; apply_change(0); }
            return 0;
        }
        if ((id == ID_MS_OFFX || id == ID_MS_OFFY) && code == EN_CHANGE) {
            BOOL ok; UINT v = GetDlgItemInt(h, id, &ok, FALSE);
            if (ok) { if (v > 8000) v = 8000; if (id == ID_MS_OFFX) S.st.mouse_off_x = (float)v; else S.st.mouse_off_y = (float)v; apply_change(0); }
            return 0;
        }
        if (id == ID_KB_CUSTOM && code == EN_CHANGE) {
            static wchar_t t[2048]; GetWindowTextW(g_edit_kc, t, 2048);
            int n = 0; for (int i = 0; t[i] && n < 2040; i++) if (t[i] >= 32 && t[i] < 127) S.st.keys_custom[n++] = (char)t[i];
            S.st.keys_custom[n] = 0; apply_change(0);
            if (g_ked) { ke_load_text(S.st.keys_custom); InvalidateRect(g_ked, NULL, FALSE); }
            return 0;
        }
        if (id == ID_LABEL_TXT && code == EN_CHANGE) {
            wchar_t t[32]; GetWindowTextW(g_edit_lab, t, 32);
            int n = 0; for (int i = 0; t[i] && n < 10; i++) if (t[i] >= 32 && t[i] < 127) S.st.label[n++] = (char)t[i];
            S.st.label[n] = 0; apply_change(0);
            return 0;
        }
        if (id == IDCANCEL) return 0;
        break;
    }
    case WM_CLOSE: app_quit(); return 0; /* X, Alt+F4 and taskbar Close all end the program */
    case WM_SYSCOMMAND:
        if ((w & 0xFFF0) == SC_CLOSE) { app_quit(); return 0; }
        break;
    case WM_DPICHANGED: {
        /* keep the window readable when dragged to a monitor with other scaling: rebuild at the new DPI */
        RECT *r = (RECT *)l;
        if ((int)HIWORD(w) != g_dpi) {
            g_ui_dpi = HIWORD(w) < 96 ? 96 : HIWORD(w);
            g_ui_pos.x = r->left; g_ui_pos.y = r->top; g_ui_pos_set = 1;
            PostMessageW(g_main, WM_APP + 3, 0, 0);
        }
        return 0;
    }
    case WM_DESTROY: g_set = NULL; g_preview = NULL; g_edit_lab = NULL; g_edit_kx = g_edit_ky = g_edit_kc = g_edit_mx = g_edit_my = NULL; g_rec = 0; surface_free(&g_pv); surface_free(&g_ui); guide_update(); return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

static LRESULT CALLBACK preview_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_PAINT) { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); if (g_pv.out) present(h, &g_pv); return 0; }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcW(h, msg, w, l);
}

/* ------------------------------------------------------------------ windows */
static LRESULT CALLBACK overlay_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_NCHITTEST) {
        if (!g_move) return HTTRANSPARENT;
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ScreenToClient(h, &p);
        return (g_drag || lay_hit(&g_ov_lay, (float)p.x, (float)p.y) || lay_hit(&g_ov_mlay, (float)p.x, (float)p.y) || lay_hit(&g_ov_klay, (float)p.x, (float)p.y)) ? HTCLIENT : HTTRANSPARENT;
    }
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_DPICHANGED) { display_check(1); return 0; } /* keep covering the whole desktop in physical pixels */
    if (msg == WM_SETCURSOR && g_move) { SetCursor(LoadCursorW(NULL, IDC_SIZEALL)); return TRUE; }
    if (msg == WM_LBUTTONDOWN || msg == WM_MOUSEMOVE || msg == WM_LBUTTONUP || msg == WM_CAPTURECHANGED)
        if (box_mouse(h, &g_ov_lay, msg, l) || box_mouse(h, &g_ov_mlay, msg, l) || box_mouse(h, &g_ov_klay, msg, l)) return 0;
    return DefWindowProcW(h, msg, w, l);
}
static int g_drag_size; /* the user is dragging the window edges */
static LRESULT CALLBACK chroma_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_GETMINMAXINFO: { /* allow a window a bit bigger than the screen (screen-sized inside + its frame) */
        MINMAXINFO *mi = (MINMAXINFO *)l; mi->ptMaxTrackSize.x = 16384; mi->ptMaxTrackSize.y = 16384; return 0; }
    case WM_ERASEBKGND: if (S.mode == 2 && g_sw_clear) return 1;
        { RECT c; GetClientRect(h, &c); HBRUSH b = CreateSolidBrush(S.mode == 1 ? to_cr(S.chroma) : 0);
        FillRect((HDC)w, &c, b); DeleteObject(b); return 1; }
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); g_force = 1; return 0; }
    case WM_SYSKEYDOWN: if (w == VK_RETURN) return 0; break; /* no fullscreen toggle */
    case WM_NCHITTEST: { /* no frame: drag anywhere to move; with Custom size, drag the edges to resize */
        if (g_move) break;
        POINT p = { (short)LOWORD(l), (short)HIWORD(l) }; RECT r; GetWindowRect(h, &r);
        if (!S.chroma_fit) {
            int e = 8, L = p.x < r.left + e, R = p.x >= r.right - e, T = p.y < r.top + e, B = p.y >= r.bottom - e;
            if (T && L) return HTTOPLEFT; if (T && R) return HTTOPRIGHT; if (B && L) return HTBOTTOMLEFT; if (B && R) return HTBOTTOMRIGHT;
            if (L) return HTLEFT; if (R) return HTRIGHT; if (T) return HTTOP; if (B) return HTBOTTOM;
        }
        return HTCAPTION; }
    case WM_SIZE:
        if (h == g_chroma && w != SIZE_MINIMIZED) {
            int cw = LOWORD(l), ch = HIWORD(l);
            if (cw > 0 && ch > 0 && (cw != g_ch.w || ch != g_ch.h)) {
                surface_alloc(&g_ch, cw, ch, 0); S.chroma_w = cw; S.chroma_h = ch; g_force = 1;
                SetTimer(g_main, TIMER_SAVE, 600, NULL);
            }
        }
        guide_update();
        return 0;
    case WM_WINDOWPOSCHANGED: guide_update(); break;
    case WM_ACTIVATE:
        if (S.mode == 2 && LOWORD(w) != WA_INACTIVE && GetTickCount() >= g_guide_quiet) {
            g_guide_flash = 1;
            if (g_main) SetTimer(g_main, TIMER_GUIDE, GUIDE_FLASH_MS, NULL);
            guide_update();
        }
        break;
    case WM_DPICHANGED: display_check(1); return 0; /* keep the capture size OBS sees */
    case WM_ENTERSIZEMOVE: g_drag_size = 1; break;
    case WM_EXITSIZEMOVE: g_drag_size = 0; chroma_fit_size(); break; /* Same as screen: the size stays exact */
    case WM_SYSCOMMAND: if ((w & 0xFFF0) == SC_MAXIMIZE) return 0; break;
    case WM_CLOSE: app_quit(); return 0;
    case WM_SETCURSOR:
        if (g_move && LOWORD(l) == HTCLIENT) {
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            int over = lay_hit(&g_ch_lay, (float)p.x, (float)p.y) || lay_hit(&g_ch_mlay, (float)p.x, (float)p.y) || lay_hit(&g_ch_klay, (float)p.x, (float)p.y);
            SetCursor(LoadCursorW(NULL, over || g_drag ? IDC_SIZEALL : IDC_ARROW)); return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: case WM_MOUSEMOVE: case WM_LBUTTONUP: case WM_CAPTURECHANGED:
        if (box_mouse(h, &g_ch_lay, msg, l) || box_mouse(h, &g_ch_mlay, msg, l) || box_mouse(h, &g_ch_klay, msg, l)) return 0;
        break;
    }
    return DefWindowProcW(h, msg, w, l);
}

typedef struct { const wchar_t *text; wchar_t accel[64]; int checked, sep; } MItem;
static MItem g_mi[16]; static int g_nmi;
static void mi_add(HMENU m, UINT id, const wchar_t *text, const wchar_t *accel, int checked) {
    MItem *it = &g_mi[g_nmi]; memset(it, 0, sizeof(*it));
    it->text = text; it->checked = checked; it->sep = text == NULL;
    if (accel) wcsncpy(it->accel, accel, 63);
    AppendMenuW(m, MF_OWNERDRAW | (it->sep ? MF_DISABLED : 0), id, (LPCWSTR)(INT_PTR)g_nmi);
    g_nmi++;
}
static void menu_measure(MEASUREITEMSTRUCT *m) {
    ensure_fonts();
    MItem *it = &g_mi[m->itemData % 16];
    if (it->sep) { m->itemWidth = SC(40); m->itemHeight = SC(9); return; }
    HDC dc = GetDC(g_main); SelectObject(dc, g_f_body);
    SIZE a, b = { 0, 0 }; GetTextExtentPoint32W(dc, it->text, (int)wcslen(it->text), &a);
    if (it->accel[0]) GetTextExtentPoint32W(dc, it->accel, (int)wcslen(it->accel), &b);
    ReleaseDC(g_main, dc);
    m->itemWidth = a.cx + (b.cx ? b.cx + SC(32) : 0) + SC(48); m->itemHeight = SC(30);
}
static void menu_draw(DRAWITEMSTRUCT *d) {
    MItem *it = &g_mi[d->itemData % 16];
    HDC dc = d->hDC; RECT r = d->rcItem;
    FillRect(dc, &r, g_br_bg);
    if (it->sep) { RECT l = { r.left + SC(10), (r.top + r.bottom) / 2, r.right - SC(10), (r.top + r.bottom) / 2 + 1 }; HBRUSH b = CreateSolidBrush(HEXC(C_LINE)); FillRect(dc, &l, b); DeleteObject(b); return; }
    if (d->itemState & ODS_SELECTED) {
        HBRUSH b = CreateSolidBrush(HEXC(C_RAISED_HI)); HPEN p = CreatePen(PS_SOLID, 1, HEXC(C_RAISED_HI));
        HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
        RoundRect(dc, r.left + SC(4), r.top + SC(1), r.right - SC(4), r.bottom - SC(1), SC(8), SC(8));
        SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); DeleteObject(p);
    }
    if (it->checked) {
        HPEN p = CreatePen(PS_SOLID, SC(2), HEXC(C_ACCENT)); HGDIOBJ op = SelectObject(dc, p);
        int cx = r.left + SC(16), cy = (r.top + r.bottom) / 2;
        POINT pts[3] = { { cx - SC(5), cy }, { cx - SC(2), cy + SC(3) }, { cx + SC(5), cy - SC(4) } };
        Polyline(dc, pts, 3); SelectObject(dc, op); DeleteObject(p);
    }
    SetBkMode(dc, TRANSPARENT); SelectObject(dc, g_f_body);
    RECT t = { r.left + SC(32), r.top, r.right - SC(14), r.bottom };
    SetTextColor(dc, HEXC(C_TEXT)); DrawTextW(dc, it->text, -1, &t, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    if (it->accel[0]) { SetTextColor(dc, HEXC(C_MUTED)); DrawTextW(dc, it->accel, -1, &t, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX); }
}
/* ------------------------------------------------------------------ key editor ("Edit my keys")
   Click keys on the keyboard to add or remove them; drag them on the layout; right-click for width. */
enum { KE_W = 900, KE_H = 640, KE_TITLE = 48, KE_PX = 40, KE_PU = 40, KE_PKH = 30,
       KE_CX = 24, KE_CY = 368, KE_CW = 852, KE_CH = 200, KE_BY = 588, KE_BH = 34 };
typedef struct { const char *name; const wchar_t *lab; float x; int row; float w; } PickKey;
static const PickKey g_pick[] = {
    { "ESC", L"Esc", 0, 0, 1 }, { "F1", L"F1", 2, 0, 1 }, { "F2", L"F2", 3, 0, 1 }, { "F3", L"F3", 4, 0, 1 }, { "F4", L"F4", 5, 0, 1 },
    { "F5", L"F5", 6.5f, 0, 1 }, { "F6", L"F6", 7.5f, 0, 1 }, { "F7", L"F7", 8.5f, 0, 1 }, { "F8", L"F8", 9.5f, 0, 1 },
    { "F9", L"F9", 11, 0, 1 }, { "F10", L"F10", 12, 0, 1 }, { "F11", L"F11", 13, 0, 1 }, { "F12", L"F12", 14, 0, 1 },
    { "~", L"~", 0, 1, 1 }, { "1", L"1", 1, 1, 1 }, { "2", L"2", 2, 1, 1 }, { "3", L"3", 3, 1, 1 }, { "4", L"4", 4, 1, 1 }, { "5", L"5", 5, 1, 1 },
    { "6", L"6", 6, 1, 1 }, { "7", L"7", 7, 1, 1 }, { "8", L"8", 8, 1, 1 }, { "9", L"9", 9, 1, 1 }, { "0", L"0", 10, 1, 1 },
    { "-", L"-", 11, 1, 1 }, { "=", L"=", 12, 1, 1 }, { "BKSP", L"Back", 13, 1, 2 },
    { "TAB", L"Tab", 0, 2, 1.5f }, { "Q", L"Q", 1.5f, 2, 1 }, { "W", L"W", 2.5f, 2, 1 }, { "E", L"E", 3.5f, 2, 1 }, { "R", L"R", 4.5f, 2, 1 },
    { "T", L"T", 5.5f, 2, 1 }, { "Y", L"Y", 6.5f, 2, 1 }, { "U", L"U", 7.5f, 2, 1 }, { "I", L"I", 8.5f, 2, 1 }, { "O", L"O", 9.5f, 2, 1 },
    { "P", L"P", 10.5f, 2, 1 }, { "[", L"[", 11.5f, 2, 1 }, { "]", L"]", 12.5f, 2, 1 }, { "BSLASH", L"\\", 13.5f, 2, 1.5f },
    { "CAPS", L"Caps", 0, 3, 1.75f }, { "A", L"A", 1.75f, 3, 1 }, { "S", L"S", 2.75f, 3, 1 }, { "D", L"D", 3.75f, 3, 1 }, { "F", L"F", 4.75f, 3, 1 },
    { "G", L"G", 5.75f, 3, 1 }, { "H", L"H", 6.75f, 3, 1 }, { "J", L"J", 7.75f, 3, 1 }, { "K", L"K", 8.75f, 3, 1 }, { "L", L"L", 9.75f, 3, 1 },
    { ";", L";", 10.75f, 3, 1 }, { "'", L"'", 11.75f, 3, 1 }, { "ENTER", L"Enter", 12.75f, 3, 2.25f },
    { "SHIFT", L"Shift", 0, 4, 2.25f }, { "Z", L"Z", 2.25f, 4, 1 }, { "X", L"X", 3.25f, 4, 1 }, { "C", L"C", 4.25f, 4, 1 }, { "V", L"V", 5.25f, 4, 1 },
    { "B", L"B", 6.25f, 4, 1 }, { "N", L"N", 7.25f, 4, 1 }, { "M", L"M", 8.25f, 4, 1 }, { "COMMA", L",", 9.25f, 4, 1 }, { ".", L".", 10.25f, 4, 1 },
    { "SLASH", L"/", 11.25f, 4, 1 }, { "SHIFT", L"Shift", 12.25f, 4, 2.75f },
    { "CTRL", L"Ctrl", 0, 5, 1.5f }, { "WIN", L"Win", 1.5f, 5, 1.25f }, { "ALT", L"Alt", 2.75f, 5, 1.25f }, { "SPACE", L"Space", 4, 5, 6.25f },
    { "ALT", L"Alt", 10.25f, 5, 1.25f }, { "CTRL", L"Ctrl", 11.5f, 5, 1.5f },
    { "INS", L"Ins", 15.5f, 1, 1 }, { "HOME", L"Home", 16.5f, 1, 1 }, { "PGUP", L"PgUp", 17.5f, 1, 1 },
    { "DEL", L"Del", 15.5f, 2, 1 }, { "END", L"End", 16.5f, 2, 1 }, { "PGDN", L"PgDn", 17.5f, 2, 1 },
    { "UP", L"\x2191", 16.5f, 4, 1 }, { "LEFT", L"\x2190", 15.5f, 5, 1 }, { "DOWN", L"\x2193", 16.5f, 5, 1 }, { "RIGHT", L"\x2192", 17.5f, 5, 1 },
    { "M4", L"M4", 19.25f, 1, 1.25f }, { "M5", L"M5", 19.25f, 2, 1.25f },
};
#define KE_NPICK ((int)(sizeof(g_pick) / sizeof(g_pick[0])))
enum { KB_CLOSE = 1, KB_W1, KB_W15, KB_W2, KB_WIDE, KB_LEAGUE, KB_LEFT, KB_WASD, KB_FULL, KB_CLEAR, KB_UNDO, KB_DONE, KB_N };
typedef struct { int x, y, w, h; const wchar_t *t; } KeBtn;
static KeBtn g_kbtn[KB_N];
static Surface g_kes;
static KeyItem g_kk[KEYS_MAX]; static int g_kn, g_ksel = -1, g_khot, g_kdrag, g_kmoved, g_kpress;
static float g_kgx, g_kgy, g_kU = 40, g_kfx, g_kfy, g_kscale = 1; static int g_kfrozen;
static POINT g_kdown_pt; static KeyItem g_korig; static char g_kpre[2048];
static char g_kundo[40][2048]; static int g_kundo_n;

static void ke_buttons(void) {
    static const KeBtn b[KB_N] = {
        [KB_CLOSE] = { KE_W - 46, 0, 46, KE_TITLE, L"" },
        [KB_W1] = { 620, 318, 56, 26, L"1x" }, [KB_W15] = { 680, 318, 56, 26, L"1.5x" }, [KB_W2] = { 740, 318, 56, 26, L"2x" }, [KB_WIDE] = { 800, 318, 76, 26, L"Wide" },
        [KB_LEAGUE] = { 116, KE_BY, 84, KE_BH, L"League" }, [KB_LEFT] = { 206, KE_BY, 96, KE_BH, L"Left side" }, [KB_WASD] = { 308, KE_BY, 80, KE_BH, L"WASD" },
        [KB_FULL] = { 394, KE_BY, 68, KE_BH, L"Full" }, [KB_CLEAR] = { 490, KE_BY, 76, KE_BH, L"Clear" }, [KB_UNDO] = { 572, KE_BY, 76, KE_BH, L"Undo" },
        [KB_DONE] = { KE_W - 24 - 120, KE_BY, 120, KE_BH, L"Done" },
    };
    memcpy(g_kbtn, b, sizeof(b));
}
static const char *ke_file_name(const char *n) {
    if (!strcmp(n, "/")) return "SLASH";
    if (!strcmp(n, ",")) return "COMMA";
    if (!strcmp(n, "\\")) return "BSLASH";
    if (!strcmp(n, "`")) return "~";
    return n;
}
static void ke_serialize(char *out, int cap) {
    int len = 0; out[0] = 0;
    for (int i = 0; i < g_kn; i++) {
        const KeyItem *k = &g_kk[i]; float dw = 1; keys_name_info(k->name, &dw, NULL);
        char b[64];
        if (fabsf(k->w - dw) > 0.01f) snprintf(b, sizeof(b), "%s%s@%g,%g:%g", len ? " " : "", ke_file_name(k->name), (double)k->x, (double)k->y, (double)k->w);
        else snprintf(b, sizeof(b), "%s%s@%g,%g", len ? " " : "", ke_file_name(k->name), (double)k->x, (double)k->y);
        int bl = (int)strlen(b);
        if (len + bl >= cap) break;
        memcpy(out + len, b, (size_t)bl + 1); len += bl;
    }
}
static void ke_load_text(const char *txt) {
    static Style t; t = S.st; t.keys_layout = 2; strncpy(t.keys_custom, txt, sizeof(t.keys_custom) - 1); t.keys_custom[sizeof(t.keys_custom) - 1] = 0;
    g_kn = keys_items(&t, g_kk, KEYS_MAX); g_ksel = -1;
}
static void ke_load_layout(int layout) {
    static Style t; t = S.st; t.keys_layout = layout;
    g_kn = keys_items(&t, g_kk, KEYS_MAX); g_ksel = -1;
}
static void ke_normalize(void) {
    if (g_kn <= 0) return;
    float mx = g_kk[0].x, my = g_kk[0].y;
    for (int i = 1; i < g_kn; i++) { if (g_kk[i].x < mx) mx = g_kk[i].x; if (g_kk[i].y < my) my = g_kk[i].y; }
    for (int i = 0; i < g_kn; i++) { g_kk[i].x -= mx; g_kk[i].y -= my; }
}
static void ke_push_text(const char *t) {
    if (g_kundo_n == 40) { memmove(g_kundo[0], g_kundo[1], sizeof(g_kundo[0]) * 39); g_kundo_n = 39; }
    strcpy(g_kundo[g_kundo_n++], t);
}
static void ke_push(void) { char t[2048]; ke_serialize(t, sizeof(t)); ke_push_text(t); }
static void ke_commit(void) { /* the keys on screen follow right away */
    ke_normalize();
    ke_serialize(S.st.keys_custom, sizeof(S.st.keys_custom));
    S.st.keys_layout = 2; S.st.keys_kb = 1;
    sync_controls(); apply_change(0);
    if (g_ked) InvalidateRect(g_ked, NULL, FALSE);
}
static int ke_overlaps(const KeyItem *k, int skip) {
    for (int j = 0; j < g_kn; j++) {
        if (j == skip) continue;
        const KeyItem *o = &g_kk[j];
        if (fabsf(k->y - o->y) < 0.99f && k->x < o->x + o->w - 0.01f && o->x < k->x + k->w - 0.01f) return 1;
    }
    return 0;
}
static int ke_pick_vk(int i) { float w; return keys_name_info(g_pick[i].name, &w, NULL); }
static int ke_pick_on(int i) { /* the 2nd Shift on the keyboard is lit when the layout has two */
    int vk = ke_pick_vk(i), occ = 1, have = 0;
    for (int j = 0; j < i; j++) if (ke_pick_vk(j) == vk) occ++;
    for (int j = 0; j < g_kn; j++) if (g_kk[j].vk == vk) have++;
    return have >= occ;
}
static void ke_toggle(int i) {
    int vk = ke_pick_vk(i);
    ke_push();
    if (ke_pick_on(i)) { /* remove the last one with that key */
        for (int j = g_kn - 1; j >= 0; j--) if (g_kk[j].vk == vk) { memmove(&g_kk[j], &g_kk[j + 1], sizeof(KeyItem) * (size_t)(g_kn - j - 1)); g_kn--; break; }
        g_ksel = -1;
    } else {
        if (g_kn >= KEYS_MAX) { g_kundo_n--; return; }
        KeyItem k; memset(&k, 0, sizeof(k)); float dw;
        k.vk = keys_name_info(g_pick[i].name, &dw, k.label);
        strncpy(k.name, g_pick[i].name, 7); k.w = g_pick[i].w; k.y = (float)(g_pick[i].row - 1); k.x = g_pick[i].x;
        float end = 0; int any = 0;
        for (int j = 0; j < g_kn; j++) if (fabsf(g_kk[j].y - k.y) < 0.99f) { any = 1; if (g_kk[j].x + g_kk[j].w > end) end = g_kk[j].x + g_kk[j].w; }
        if (any && k.x > end + 0.5f) k.x = end; /* no big gap: right after the last key in that row */
        if (ke_overlaps(&k, -1)) { /* its usual spot is taken: put it at the end of that row */
            k.x = any ? end : 0;
            if (ke_overlaps(&k, -1)) { float maxy = 0; for (int j = 0; j < g_kn; j++) if (g_kk[j].y > maxy) maxy = g_kk[j].y; k.y = maxy + 1; k.x = 0; }
        }
        g_kk[g_kn] = k; g_ksel = g_kn; g_kn++;
    }
    ke_commit();
}
static void ke_set_width(int i, float w) {
    if (i < 0 || i >= g_kn) return;
    ke_push();
    KeyItem old = g_kk[i]; float d = w - old.w;
    for (int j = 0; j < g_kn; j++) /* keys to its right in the same row move along, so it stays tight */
        if (j != i && fabsf(g_kk[j].y - old.y) < 0.5f && g_kk[j].x >= old.x + old.w - 0.01f) g_kk[j].x += d;
    g_kk[i].w = w;
    ke_commit();
}
static void ke_remove(int i) {
    if (i < 0 || i >= g_kn) return;
    ke_push(); memmove(&g_kk[i], &g_kk[i + 1], sizeof(KeyItem) * (size_t)(g_kn - i - 1)); g_kn--; g_ksel = -1; ke_commit();
}
static void ke_undo(void) {
    if (!g_kundo_n) return;
    ke_load_text(g_kundo[--g_kundo_n]); ke_commit();
}
/* the style and spot the layout is drawn with; sets g_kgx / g_kgy (key 0,0) and g_kU (one key) */
static void ke_scene(Style *st, Scene *sc) {
    *st = S.st;
    st->keys_kb = 1; st->keys_mouse = 0; st->keys_layout = 2; st->keys_corner = APM_TL; st->keys_off_x = st->keys_off_y = 0;
    st->keys_highlight = 0; st->keys_plate = 0; st->mouse_sep = 0;
    st->trail_enabled = 0; st->click_fade_s = 0; st->apm_enabled = 0; st->cursor_dot = 0;
    ke_serialize(st->keys_custom, sizeof(st->keys_custom));
    memset(sc, 0, sizeof(*sc)); sc->scale = 1; sc->now = now_s();
    float mx = 0, my = 0, W = 1, H = 1;
    for (int i = 0; i < g_kn; i++) {
        if (i == 0 || g_kk[i].x < mx) mx = g_kk[i].x;
        if (i == 0 || g_kk[i].y < my) my = g_kk[i].y;
        if (g_kk[i].x + g_kk[i].w > W) W = g_kk[i].x + g_kk[i].w;
        if (g_kk[i].y + 1 > H) H = g_kk[i].y + 1;
    }
    sc->area_w = SF(KE_CW) * 4; sc->area_h = SF(KE_CH) * 4;
    if (!g_kfrozen) {
        float want = fminf(SF(KE_CW - 40) / fmaxf(W, 8), SF(KE_CH - 40) / fmaxf(H, 4));
        want = fminf(fmaxf(want, SF(22)), SF(52));
        st->keys_scale = 1; float x0, y0, U0; keys_geom(st, sc, &x0, &y0, &U0);
        st->keys_scale = want / fmaxf(U0, 1); g_kscale = st->keys_scale;
        float px, py, pw, ph; keys_panel_rect(st, sc, &px, &py, &pw, &ph);
        if (g_kn == 0) pw = ph = 0;
        g_kfx = floorf(SF(KE_CX) + (SF(KE_CW) - pw) * 0.5f); g_kfy = floorf(SF(KE_CY) + (SF(KE_CH) - ph) * 0.5f);
    }
    st->keys_scale = g_kscale;
    sc->area_x = g_kfx; sc->area_y = g_kfy;
    float ox, oy; keys_geom(st, sc, &ox, &oy, &g_kU);
    g_kgx = ox; g_kgy = oy;           /* where key 0,0 is when nothing is dragged */
    sc->area_x = g_kfx + mx * g_kU; sc->area_y = g_kfy + my * g_kU; /* while dragging past the left/top edge */
}
static int ke_key_at(int px, int py) {
    for (int i = g_kn - 1; i >= 0; i--) {
        float x = g_kgx + g_kk[i].x * g_kU, y = g_kgy + g_kk[i].y * g_kU;
        if (px >= x && px < x + g_kk[i].w * g_kU && py >= y && py < y + g_kU) return i;
    }
    return -1;
}
/* hover / hit ids: 100 + button, 1000 + keyboard key, 3000 + layout key */
static int ke_hit(int px, int py) { /* px, py in screen pixels of the window */
    int lx = MulDiv(px, 96, g_dpi), ly = MulDiv(py, 96, g_dpi);
    for (int b = 1; b < KB_N; b++) { const KeBtn *k = &g_kbtn[b]; if (lx >= k->x && lx < k->x + k->w && ly >= k->y && ly < k->y + k->h) return 100 + b; }
    for (int i = 0; i < KE_NPICK; i++) {
        int x = KE_PX + (int)(g_pick[i].x * KE_PU), y = g_pick[i].row ? 124 + (g_pick[i].row - 1) * 34 : 84, w = (int)(g_pick[i].w * KE_PU) - 4;
        if (lx >= x && lx < x + w && ly >= y && ly < y + KE_PKH) return 1000 + i;
    }
    if (lx >= KE_CX && lx < KE_CX + KE_CW && ly >= KE_CY && ly < KE_CY + KE_CH) { int k = ke_key_at(px, py); return k >= 0 ? 3000 + k : 2999; }
    return 0;
}
static void ke_paint(HWND h) {
    PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
    RECT cr; GetClientRect(h, &cr);
    if (!g_kes.mdc || g_kes.w != cr.right || g_kes.h != cr.bottom) surface_alloc(&g_kes, cr.right, cr.bottom, 1);
    if (!g_kes.mdc) { EndPaint(h, &ps); return; }
    Surface save = g_ui; g_ui = g_kes; /* the Settings drawing helpers draw into g_ui */
    GdiFlush();
    ui_rect(0, 0, g_ui.w, g_ui.h, C_BG);
    SetBkMode(g_ui.mdc, TRANSPARENT);
    ui_rect(0, SC(KE_TITLE), g_ui.w, 1, C_LINE);
    /* keyboard */
    for (int i = 0; i < KE_NPICK; i++) {
        int x = KE_PX + (int)(g_pick[i].x * KE_PU), y = g_pick[i].row ? 124 + (g_pick[i].row - 1) * 34 : 84, w = (int)(g_pick[i].w * KE_PU) - 4;
        int on = ke_pick_on(i), hot = g_khot == 1000 + i;
        ui_frame(x, y, w, KE_PKH, 6, on ? (hot ? C_ACCENT_HI : C_ACCENT) : (hot ? C_RAISED_HI : C_RAISED), on ? C_ACCENT : (hot ? C_LINE_HI : C_LINE));
    }
    /* layout area */
    ui_frame(KE_CX, KE_CY, KE_CW, KE_CH, 10, C_CARD, C_CARD_LINE);
    Style st; Scene sc; ke_scene(&st, &sc);
    { /* half-key dots */
        float step = g_kU * 0.5f; if (step < 4) step = 4;
        int x0 = SC(KE_CX) + 4, x1 = SC(KE_CX + KE_CW) - 4, y0 = SC(KE_CY) + 4, y1 = SC(KE_CY + KE_CH) - 4;
        float sx = g_kgx - ceilf((g_kgx - x0) / step) * step, sy = g_kgy - ceilf((g_kgy - y0) / step) * step;
        for (float y = sy; y < y1; y += step) for (float x = sx; x < x1; x += step)
            if (x >= x0 && y >= y0) ui_rect((int)x, (int)y, SC(2) > 1 ? 2 : 1, SC(2) > 1 ? 2 : 1, C_LINE_HI);
    }
    GdiFlush();
    static unsigned char kd[256]; memset(kd, 0, sizeof(kd));
    if (g_ksel >= 0 && g_ksel < g_kn) kd[g_kk[g_ksel].vk & 255] = 1; /* the picked key shows pressed */
    sc.kdown = kd;
    if (g_kn) render_scene(&g_ui.C, &g_ui.L, &st, &sc);
    if (g_ksel >= 0 && g_ksel < g_kn) { /* and gets an accent frame */
        int x = (int)(g_kgx + g_kk[g_ksel].x * g_kU) - SC(3), y = (int)(g_kgy + g_kk[g_ksel].y * g_kU) - SC(3);
        int w = (int)(g_kk[g_ksel].w * g_kU) + SC(6), hh = (int)g_kU + SC(6), t = SC(2) > 1 ? SC(2) : 2;
        ui_rect(x, y, w, t, C_ACCENT); ui_rect(x, y + hh - t, w, t, C_ACCENT); ui_rect(x, y, t, hh, C_ACCENT); ui_rect(x + w - t, y, t, hh, C_ACCENT);
    }
    /* buttons */
    for (int b = 1; b < KB_N; b++) {
        const KeBtn *k = &g_kbtn[b]; int hot = g_khot == 100 + b;
        if (b == KB_CLOSE) {
            if (hot) ui_rect(SC(k->x), SC(k->y), SC(k->w), SC(k->h), 0xC42B1C);
            RGBf ink = HEXF(hot ? 0xFFFFFF : C_LABEL); float cx = SF(k->x + k->w * 0.5f), cy = SF(k->y + k->h * 0.5f), r = SF(5), lw = SF(1.1f);
            draw_line(&g_ui.C, &g_ui.L, cx - r, cy - r, cx + r, cy + r, lw, ink, 1.0f);
            draw_line(&g_ui.C, &g_ui.L, cx - r, cy + r, cx + r, cy - r, lw, ink, 1.0f);
            continue;
        }
        int dis = (b >= KB_W1 && b <= KB_WIDE && g_ksel < 0) || (b == KB_UNDO && !g_kundo_n);
        if (b == KB_DONE) ui_rr(k->x, k->y, k->w, k->h, 6, hot ? C_ACCENT_HI : C_ACCENT);
        else ui_frame(k->x, k->y, k->w, k->h, 6, hot && !dis ? C_RAISED_HI : C_RAISED, hot && !dis ? C_LINE_HI : C_LINE);
    }
    GdiFlush();
    /* texts */
    ui_text(g_f_title, C_TEXT, 24, 0, 400, KE_TITLE, DT_SINGLELINE | DT_VCENTER, L"Edit my keys");
    ui_text(g_f_cap, C_MUTED, 26, 60, 400, 18, DT_SINGLELINE | DT_VCENTER, L"KEYBOARD");
    ui_text(g_f_hint, C_MUTED, 26, 294, 850, 18, DT_SINGLELINE | DT_VCENTER, L"Click a key to add it, click it again to remove it. M4 and M5 are the mouse side buttons.");
    ui_text(g_f_cap, C_MUTED, 26, 322, 300, 18, DT_SINGLELINE | DT_VCENTER, L"YOUR LAYOUT");
    ui_text(g_f_hint, C_MUTED, 26, 344, 580, 18, DT_SINGLELINE | DT_VCENTER, L"Drag keys to move them. Right-click a key for its width or to remove it.");
    ui_text(g_f_body, g_ksel >= 0 ? C_LABEL : C_MUTED, 520, 318, 92, 26, DT_SINGLELINE | DT_VCENTER | DT_RIGHT, L"Key width");
    ui_text(g_f_body, C_LABEL, 24, KE_BY, 88, KE_BH, DT_SINGLELINE | DT_VCENTER, L"Start from");
    if (!g_kn) ui_text(g_f_body, C_MUTED, KE_CX, KE_CY, KE_CW, KE_CH, DT_SINGLELINE | DT_VCENTER | DT_CENTER, L"No keys yet. Click keys above, or start from a preset below.");
    for (int i = 0; i < KE_NPICK; i++) {
        int x = KE_PX + (int)(g_pick[i].x * KE_PU), y = g_pick[i].row ? 124 + (g_pick[i].row - 1) * 34 : 84, w = (int)(g_pick[i].w * KE_PU) - 4;
        ui_text(g_f_cap, ke_pick_on(i) ? C_ON_ACCENT : C_LABEL, x, y, w, KE_PKH, DT_SINGLELINE | DT_VCENTER | DT_CENTER, g_pick[i].lab);
    }
    for (int b = 1; b < KB_N; b++) {
        const KeBtn *k = &g_kbtn[b];
        int dis = (b >= KB_W1 && b <= KB_WIDE && g_ksel < 0) || (b == KB_UNDO && !g_kundo_n);
        if (b == KB_CLOSE) continue;
        ui_text(g_f_semi, b == KB_DONE ? C_ON_ACCENT : (dis ? C_MUTED : C_TEXT), k->x, k->y, k->w, k->h, DT_SINGLELINE | DT_VCENTER | DT_CENTER, k->t);
    }
    GdiFlush();
    ui_rect(0, 0, g_ui.w, 1, C_LINE); ui_rect(0, g_ui.h - 1, g_ui.w, 1, C_LINE);
    ui_rect(0, 0, 1, g_ui.h, C_LINE); ui_rect(g_ui.w - 1, 0, 1, g_ui.h, C_LINE);
    BitBlt(dc, 0, 0, g_ui.w, g_ui.h, g_ui.mdc, 0, 0, SRCCOPY);
    g_kes = g_ui; g_ui = save;
    EndPaint(h, &ps);
}
static void ke_width_menu(int i) {
    static const float ws[] = { 1, 1.25f, 1.5f, 1.75f, 2, 2.25f, 2.75f, 3, 6.25f };
    static const wchar_t *wl[] = { L"1x", L"1.25x", L"1.5x", L"1.75x", L"2x", L"2.25x", L"2.75x", L"3x", L"Space wide (6.25x)" };
    ensure_fonts();
    HMENU m = CreatePopupMenu(); g_nmi = 0;
    for (int j = 0; j < 9; j++) mi_add(m, 10 + j, wl[j], NULL, fabsf(g_kk[i].w - ws[j]) < 0.01f);
    mi_add(m, 900, NULL, NULL, 0);
    mi_add(m, 50, L"Remove key", L"Del", 0);
    MENUINFO mi; memset(&mi, 0, sizeof(mi)); mi.cbSize = sizeof(mi); mi.fMask = MIM_BACKGROUND; mi.hbrBack = g_br_bg; SetMenuInfo(m, &mi);
    POINT p; GetCursorPos(&p);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, p.x, p.y, 0, g_ked, NULL);
    DestroyMenu(m);
    if (cmd >= 10 && cmd < 19) ke_set_width(i, ws[cmd - 10]);
    else if (cmd == 50) ke_remove(i);
}
static void ke_button(int b) {
    switch (b) {
    case KB_CLOSE: case KB_DONE: DestroyWindow(g_ked); return;
    case KB_W1: ke_set_width(g_ksel, 1); break;
    case KB_W15: ke_set_width(g_ksel, 1.5f); break;
    case KB_W2: ke_set_width(g_ksel, 2); break;
    case KB_WIDE: ke_set_width(g_ksel, 6.25f); break;
    case KB_LEAGUE: case KB_LEFT: case KB_WASD: case KB_FULL: {
        static const int lay[] = { 1, 0, 3, 4 };
        ke_push(); ke_load_layout(lay[b - KB_LEAGUE]); ke_commit(); break; }
    case KB_CLEAR: if (g_kn) { ke_push(); g_kn = 0; g_ksel = -1; ke_commit(); } break;
    case KB_UNDO: ke_undo(); break;
    }
}
static LRESULT CALLBACK keyed_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: ke_paint(h); return 0;
    case WM_NCHITTEST: {
        POINT p = { (short)LOWORD(l), (short)HIWORD(l) }; ScreenToClient(h, &p);
        if (p.y < SC(KE_TITLE) && ke_hit(p.x, p.y) != 100 + KB_CLOSE) return HTCAPTION;
        return HTCLIENT; }
    case WM_MOUSEMOVE: {
        int px = (short)LOWORD(l), py = (short)HIWORD(l);
        if (g_kdrag && g_ksel >= 0) {
            float dx = (px - g_kdown_pt.x) / g_kU, dy = (py - g_kdown_pt.y) / g_kU;
            if (!g_kmoved && abs(px - g_kdown_pt.x) + abs(py - g_kdown_pt.y) < SC(4)) return 0;
            g_kmoved = 1;
            KeyItem *k = &g_kk[g_ksel];
            float rx = g_korig.x + dx, ry = g_korig.y + dy;
            float ny = roundf(ry * 2) * 0.5f, nx = roundf(rx * 2) * 0.5f, best = 0.3f;
            for (int j = 0; j < g_kn; j++) { /* snap flush against a key in the same row */
                if (j == g_ksel || fabsf(g_kk[j].y - ny) > 0.01f) continue;
                float a = g_kk[j].x + g_kk[j].w, b = g_kk[j].x - k->w;
                if (fabsf(rx - a) < best) { best = fabsf(rx - a); nx = a; }
                if (fabsf(rx - b) < best) { best = fabsf(rx - b); nx = b; }
            }
            /* stay inside the layout area */
            float minx = (SF(KE_CX) + 6 - g_kgx) / g_kU, maxx = (SF(KE_CX + KE_CW) - 6 - g_kgx) / g_kU - k->w;
            float miny = (SF(KE_CY) + 6 - g_kgy) / g_kU, maxy = (SF(KE_CY + KE_CH) - 6 - g_kgy) / g_kU - 1;
            if (nx < minx) nx = ceilf(minx * 2) * 0.5f;
            if (nx > maxx) nx = floorf(maxx * 2) * 0.5f;
            if (ny < miny) ny = ceilf(miny * 2) * 0.5f;
            if (ny > maxy) ny = floorf(maxy * 2) * 0.5f;
            if (nx != k->x || ny != k->y) { k->x = nx; k->y = ny; InvalidateRect(h, NULL, FALSE); }
            return 0;
        }
        int hot = ke_hit(px, py);
        if (hot != g_khot) { g_khot = hot; InvalidateRect(h, NULL, FALSE); }
        TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
        HCURSOR c = LoadCursorW(NULL, hot >= 3000 ? IDC_SIZEALL : (hot >= 100 && hot < 3000 && hot != 2999) ? IDC_HAND : IDC_ARROW); SetCursor(c);
        return 0; }
    case WM_MOUSELEAVE: if (g_khot) { g_khot = 0; InvalidateRect(h, NULL, FALSE); } return 0;
    case WM_SETCURSOR: if (LOWORD(l) == HTCLIENT) return TRUE; break;
    case WM_LBUTTONDOWN: {
        int px = (short)LOWORD(l), py = (short)HIWORD(l), hit = ke_hit(px, py);
        g_kpress = hit;
        if (hit >= 3000) {
            g_ksel = hit - 3000; g_kdrag = 1; g_kmoved = 0; g_kdown_pt.x = px; g_kdown_pt.y = py; g_korig = g_kk[g_ksel];
            ke_serialize(g_kpre, sizeof(g_kpre)); g_kfrozen = 1; SetCapture(h);
        } else if (hit == 2999) g_ksel = -1;
        InvalidateRect(h, NULL, FALSE); return 0; }
    case WM_LBUTTONUP: {
        int px = (short)LOWORD(l), py = (short)HIWORD(l);
        if (g_kdrag) {
            g_kdrag = 0; ReleaseCapture();
            if (g_kmoved && g_ksel >= 0) {
                if (ke_overlaps(&g_kk[g_ksel], g_ksel)) g_kk[g_ksel] = g_korig; /* would sit on another key: put it back */
                else { ke_push_text(g_kpre); g_kfrozen = 0; ke_commit(); }
            }
            g_kfrozen = 0; InvalidateRect(h, NULL, FALSE); return 0;
        }
        int hit = ke_hit(px, py);
        if (hit == g_kpress) {
            if (hit >= 1000 && hit < 1000 + KE_NPICK) ke_toggle(hit - 1000);
            else if (hit > 100 && hit < 100 + KB_N) ke_button(hit - 100);
        }
        g_kpress = 0; return 0; }
    case WM_CAPTURECHANGED: if (g_kdrag && (HWND)l != h) { g_kdrag = 0; g_kfrozen = 0; if (g_ksel >= 0 && g_ksel < g_kn) g_kk[g_ksel] = g_korig; InvalidateRect(h, NULL, FALSE); } break;
    case WM_RBUTTONUP: {
        int hit = ke_hit((short)LOWORD(l), (short)HIWORD(l));
        if (hit >= 3000) { g_ksel = hit - 3000; InvalidateRect(h, NULL, FALSE); ke_width_menu(g_ksel); }
        return 0; }
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) { DestroyWindow(h); return 0; }
        if (w == VK_DELETE || w == VK_BACK) { ke_remove(g_ksel); return 0; }
        if (w == 'Z' && (GetKeyState(VK_CONTROL) & 0x8000)) { ke_undo(); return 0; }
        break;
    case WM_MEASUREITEM: if (((MEASUREITEMSTRUCT *)l)->CtlType == ODT_MENU) { menu_measure((MEASUREITEMSTRUCT *)l); return TRUE; } break;
    case WM_DRAWITEM: if (((DRAWITEMSTRUCT *)l)->CtlType == ODT_MENU) { menu_draw((DRAWITEMSTRUCT *)l); return TRUE; } break;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: if (h == g_ked) { g_ked = NULL; surface_free(&g_kes); g_kdrag = 0; g_kfrozen = 0; } return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}
static void keyed_open(void) {
    if (g_ked) { ShowWindow(g_ked, SW_SHOWNORMAL); SetForegroundWindow(g_ked); return; }
    ensure_fonts(); ke_buttons();
    ke_load_layout(S.st.keys_layout); /* start from what is shown now */
    g_kundo_n = 0; g_ksel = -1; g_khot = 0;
    RECT sr = { 0, 0, SC(968), SC(680) }; if (g_set) GetWindowRect(g_set, &sr);
    int ww = SC(KE_W), wh = SC(KE_H);
    int x = sr.left + (sr.right - sr.left - ww) / 2, y = sr.top + (sr.bottom - sr.top - wh) / 2;
    g_ked = CreateWindowExW(0, L"MAOKeyEditor", APP_NAME L" - Edit my keys", WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX, x, y, ww, wh, g_set, NULL, g_inst, NULL);
    if (!g_ked) return;
    { DWORD none = 0xFFFFFFFE, sq = 1; DwmSetWindowAttribute(g_ked, 34, &none, sizeof(none)); DwmSetWindowAttribute(g_ked, 33, &sq, sizeof(sq)); (void)sq; }
    ShowWindow(g_ked, SW_SHOW); SetForegroundWindow(g_ked);
}
static void tray_menu(void) {
    ensure_fonts();
    HMENU m = CreatePopupMenu();
    wchar_t hk[64]; hotkey_text(S.hk_mods, S.hk_vk, hk, 64);
    g_nmi = 0;
    mi_add(m, 1, L"Settings", NULL, 0);
    mi_add(m, 2, g_hidden ? L"Show overlay" : L"Hide overlay", hk, 0);
    mi_add(m, 900, NULL, NULL, 0);
    mi_add(m, 10, L"On top of game", NULL, S.mode == 0);
    mi_add(m, 11, L"Green screen window", NULL, S.mode == 1);
    mi_add(m, 12, L"Stream window (see-through)", NULL, S.mode == 2);
    mi_add(m, 901, NULL, NULL, 0);
    mi_add(m, 5, g_move ? L"Done moving" : L"Move on screen", NULL, g_move);
    mi_add(m, 3, L"Reset APM", NULL, 0);
    mi_add(m, 902, NULL, NULL, 0);
    mi_add(m, 4, L"Exit program", NULL, 0);
    MENUINFO mi; memset(&mi, 0, sizeof(mi)); mi.cbSize = sizeof(mi); mi.fMask = MIM_BACKGROUND; mi.hbrBack = g_br_bg; SetMenuInfo(m, &mi);
    POINT p; GetCursorPos(&p);
    SetForegroundWindow(g_main);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, p.x, p.y, 0, g_main, NULL);
    PostMessageW(g_main, WM_NULL, 0, 0);
    DestroyMenu(m);
    switch (cmd) {
    case 1: settings_create(); break;
    case 2: set_hidden(!g_hidden); break;
    case 3: reset_apm(); break;
    case 4: app_quit(); break;
    case 5: set_move_mode(!g_move); break;
    case 10: case 11: case 12: if (S.mode != cmd - 10) { S.mode = cmd - 10; g_hidden = 0; sync_controls(); apply_change(1); } break;
    }
}

/* when another window comes to the front (for example League in borderless), put the overlay back on
   top right away instead of waiting for the once-a-second check. Only a notification; nothing is
   read from or done to the other program. */
static void CALLBACK on_foreground(HWINEVENTHOOK hk, DWORD ev, HWND w, LONG obj, LONG child, DWORD th, DWORD t) {
    (void)hk; (void)ev; (void)w; (void)obj; (void)child; (void)th; (void)t;
    if (g_overlay && !g_hidden) SetWindowPos(g_overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
static LRESULT CALLBACK main_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == g_msg_taskbar && g_msg_taskbar) { tray_add(); return 0; }
    switch (msg) {
    case WM_TRAY:
        if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_CONTEXTMENU) tray_menu();
        else if (LOWORD(l) == WM_LBUTTONDBLCLK || LOWORD(l) == WM_LBUTTONUP) settings_create();
        return 0;
    case WM_OPEN_SETTINGS: settings_create(); return 0;
    case WM_APP + 3:
        if (g_set) {
            int page = g_cur_page;
            DestroyWindow(g_set); free_fonts(); settings_create();
            if (g_set && g_ui_pos_set) SetWindowPos(g_set, NULL, g_ui_pos.x, g_ui_pos.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
            g_ui_pos_set = 0;
            if (g_set) show_page(page);
        }
        return 0;
    case WM_MEASUREITEM: if (((MEASUREITEMSTRUCT *)l)->CtlType == ODT_MENU) { menu_measure((MEASUREITEMSTRUCT *)l); return TRUE; } break;
    case WM_DRAWITEM: if (((DRAWITEMSTRUCT *)l)->CtlType == ODT_MENU) { menu_draw((DRAWITEMSTRUCT *)l); return TRUE; } break;
    case WM_HOTKEY: if (w == HK_TOGGLE) set_hidden(!g_hidden); return 0;
    case WM_DISPLAYCHANGE: case WM_DPICHANGED: display_check(1); return 0;
    case WM_SETTINGCHANGE: display_check(0); break;
    case WM_DEVICECHANGE: if (w == 0x0007 /* DBT_DEVNODES_CHANGED */) display_check(0); break;
    case WM_TIMER:
        if (w == TIMER_FRAME) { if (now_s() - g_last_frame > 0.03) frame(); /* keeps rendering inside modal loops (menus, colour picker) */ }
        else if (w == TIMER_SAVE) { KillTimer(h, TIMER_SAVE); save_settings(); }
        else if (w == TIMER_STATUS) { KillTimer(h, TIMER_STATUS); g_saved_flash = 0; if (g_set) InvalidateRect(g_set, NULL, FALSE); }
        else if (w == TIMER_GUIDE) { KillTimer(h, TIMER_GUIDE); g_guide_flash = 0; guide_update(); }
        else if (w == TIMER_WATCH) {
            FILETIME ft; read_ini_mtime(&ft);
            if (CompareFileTime(&ft, &g_ini_mtime) != 0 && (ft.dwLowDateTime || ft.dwHighDateTime)) {
                g_ini_mtime = ft;
                int old_mode = S.mode, ow = S.chroma_w, oh = S.chroma_h, ov = S.chroma_screen, ot = S.chroma_topmost, of = S.chroma_fit, oc = S.stream_clear;
                load_settings(); register_hotkey(); input_start(); sync_controls();
                if (S.mode != old_mode || (S.mode != 0 && (ow != S.chroma_w || oh != S.chroma_h || ov != S.chroma_screen || ot != S.chroma_topmost || of != S.chroma_fit || oc != S.stream_clear)))
                    display_create();
                g_force = 1;
            }
            display_check(0);
            if (g_overlay && !g_hidden) SetWindowPos(g_overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            guide_update();
        }
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

static void reg_class(const wchar_t *name, WNDPROC p, HBRUSH bg, UINT style) {
    WNDCLASSEXW wc; memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = p; wc.hInstance = g_inst; wc.lpszClassName = name;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW); wc.hbrBackground = bg; wc.style = style;
    wc.hIcon = g_icon_big; wc.hIconSm = g_icon_small;
    RegisterClassExW(&wc);
}

/* Per-monitor v2 before any window exists, so cursor positions and window positions are both
   physical pixels on every monitor. The manifest asks for the same; this covers older loaders. */
static void set_dpi_awareness(void) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    typedef BOOL(WINAPI * SPDAC)(HANDLE);
    typedef HANDLE(WINAPI * GTDAC)(void);
    typedef BOOL(WINAPI * ADACE)(HANDLE, HANDLE);
    SPDAC f = u ? (SPDAC)(void *)GetProcAddress(u, "SetProcessDpiAwarenessContext") : NULL;
    GTDAC g = u ? (GTDAC)(void *)GetProcAddress(u, "GetThreadDpiAwarenessContext") : NULL;
    ADACE eq = u ? (ADACE)(void *)GetProcAddress(u, "AreDpiAwarenessContextsEqual") : NULL;
    HANDLE PMV2 = (HANDLE)(INT_PTR)-4, PMV1 = (HANDLE)(INT_PTR)-3;
    if (f) {
        if (f(PMV2)) return;
        if (g && eq && (eq(g(), PMV2) || eq(g(), PMV1))) return; /* already set by the manifest */
        if (f(PMV1)) return;
    }
    HMODULE sh = LoadLibraryW(L"shcore.dll");
    typedef HRESULT(WINAPI * SPDA)(int);
    SPDA s = sh ? (SPDA)(void *)GetProcAddress(sh, "SetProcessDpiAwareness") : NULL;
    if (s && SUCCEEDED(s(2))) return;
    SetProcessDPIAware();
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show) {
    (void)prev; (void)show;
    g_inst = inst;
    set_dpi_awareness();
    dpi_load();
    thread_pmv2();
    HANDLE mtx = CreateMutexW(NULL, TRUE, L"Local\\MouseApmOverlay_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND set = FindWindowW(L"MAOSettings", NULL);
        HWND other = FindWindowW(L"MAOMain", NULL);
        if (set) {
            DWORD pid = 0;
            DWORD tid = GetWindowThreadProcessId(set, &pid);
            DWORD mine = GetCurrentThreadId();
            AttachThreadInput(mine, tid, TRUE);
            if (IsIconic(set)) ShowWindow(set, SW_RESTORE);
            else ShowWindow(set, SW_SHOWNORMAL);
            SetWindowPos(set, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            SetForegroundWindow(set);
            AttachThreadInput(mine, tid, FALSE);
            if (pid) AllowSetForegroundWindow(pid);
        }
        if (other) PostMessageW(other, WM_OPEN_SETTINGS, 0, 0);
        return 0;
    }
    QueryPerformanceFrequency(&g_qpf);
    InitializeCriticalSection(&g_lock);
    apm_init(&g_apm, now_s());
    g_last_input = now_s();
    render_set_text_raster(gdi_text_raster);
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_HOTKEY_CLASS | ICC_STANDARD_CLASSES | ICC_UPDOWN_CLASS | ICC_TAB_CLASSES };
    InitCommonControlsEx(&icc);

    GetModuleFileNameW(NULL, g_ini, MAX_PATH);
    wchar_t *slash = wcsrchr(g_ini, L'\\');
    if (slash) slash[1] = 0; else g_ini[0] = 0;
    wchar_t old_ini[MAX_PATH]; wcscpy(old_ini, g_ini);
    wcsncat(g_ini, L"input-overlay.ini", MAX_PATH - wcslen(g_ini) - 1);
    wcsncat(old_ini, L"mouse-apm-overlay.ini", MAX_PATH - wcslen(old_ini) - 1);
    int first_run = GetFileAttributesW(g_ini) == INVALID_FILE_ATTRIBUTES;
    if (first_run && GetFileAttributesW(old_ini) != INVALID_FILE_ATTRIBUTES && CopyFileW(old_ini, g_ini, TRUE)) first_run = 0; /* bring settings over from the old name */
    load_settings();
    if (first_run) save_settings(); else read_ini_mtime(&g_ini_mtime);

    g_icon_big = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    g_icon_small = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!g_icon_big) g_icon_big = LoadIconW(NULL, IDI_APPLICATION);
    if (!g_icon_small) g_icon_small = g_icon_big;

    reg_class(L"MAOMain", main_proc, NULL, 0);
    reg_class(L"MAOInputSink", input_wndproc, NULL, 0);
    reg_class(L"MAOOverlay", overlay_proc, NULL, 0);
    reg_class(L"MAOChroma", chroma_proc, NULL, CS_HREDRAW | CS_VREDRAW);
    reg_class(L"MAOGuide", guide_proc, NULL, 0);
    reg_class(L"MAOSettings", settings_proc, (HBRUSH)(COLOR_WINDOW + 1), 0);
    reg_class(L"MAOPreview", preview_proc, NULL, 0);
    reg_class(L"MAOKeyEditor", keyed_proc, NULL, 0);

    g_main = CreateWindowExW(WS_EX_TOOLWINDOW, L"MAOMain", APP_NAME, WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    g_msg_taskbar = RegisterWindowMessageW(L"TaskbarCreated");
    tray_add();
    register_hotkey();
    input_start();
    display_create();
    SetTimer(g_main, TIMER_FRAME, 10, NULL);
    SetTimer(g_main, TIMER_WATCH, 1000, NULL);
    HWINEVENTHOOK fg = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, on_foreground, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    (void)cmd;
    settings_create();

    timeBeginPeriod(1);
    int use_dwm = 1, fast_flush = 0;
    double next = now_s();
    while (!g_quit) {
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) { g_quit = 1; break; }
            if (g_set && !g_rec && IsDialogMessageW(g_set, &m)) continue;
            TranslateMessage(&m); DispatchMessageW(&m);
        }
        if (g_quit) break;
        frame();
        int nothing_visible = g_hidden && !(g_set && IsWindowVisible(g_set));
        if (nothing_visible) { MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT); continue; }
        if (S.fps == 0 && use_dwm) {
            /* vsync via the compositor; fall back to a timer if DwmFlush fails or
               returns instantly (no compositor), so we never spin a CPU core */
            double t = now_s();
            if (FAILED(DwmFlush())) use_dwm = 0;
            else if (now_s() - t < 0.0005) { if (++fast_flush > 30) use_dwm = 0; }
            else fast_flush = 0;
            if (!use_dwm) next = now_s();
        } else {
            int fps = S.fps ? S.fps : 144;
            next += 1.0 / fps;
            double t = now_s();
            if (next < t - 0.1) next = t;
            double ms = (next - t) * 1000.0;
            if (ms > 0) MsgWaitForMultipleObjects(0, NULL, FALSE, (DWORD)ms, QS_ALLINPUT);
        }
    }
    /* cleanup should take a moment; if anything ever hangs, end anyway so it never lingers */
    CreateThread(NULL, 0, quit_watchdog, NULL, 0, NULL);
    app_quit();
    timeEndPeriod(1);
    if (fg) UnhookWinEvent(fg);
    KillTimer(g_main, TIMER_SAVE);
    save_settings();
    input_stop();
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_ked) DestroyWindow(g_ked);
    if (g_set) DestroyWindow(g_set);
    display_destroy();
    UnregisterHotKey(g_main, HK_TOGGLE);
    DestroyWindow(g_main);
    if (mtx) { ReleaseMutex(mtx); CloseHandle(mtx); }
    ExitProcess(0);
}
