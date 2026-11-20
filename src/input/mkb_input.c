/*
 * mkb_input.c -- mouse + keyboard as a virtual Xbox gamepad.
 *
 * A hidden window on its own thread registers for Raw Input with
 * RIDEV_INPUTSINK, so mouse deltas and key presses arrive whether or
 * not the game's own window is the one with focus, and no matter which
 * window the runtime happens to present through (the GDI debug window,
 * the FMV window, a D3D11 swap chain -- the input layer should not
 * have to care).
 *
 * The poll side drains the accumulated deltas and turns them into a
 * stick deflection that is proportional to the movement since the last
 * poll: the title integrates deflection over its own frame, so the
 * total rotation comes out proportional to total mouse travel at any
 * frame rate, which is the property mouse aim has to keep.
 *
 * Small movements get a floor of RECOMP_MKB_DZ of full throw, because
 * every console title applies its own dead zone to the stick, and a
 * mouse correction smaller than it would be silently eaten.
 */

/* Raw input needs a modern _WIN32_WINNT; the toolkit's platform header
 * includes windows.h first, so these have to land before anything else. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif
#define _CRT_SECURE_NO_WARNINGS

#include "mkb_input.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

/* ---- configuration ----------------------------------------------------- */

typedef struct {
    double sens;        /* multiplier on the aim deflection          */
    double dz;          /* assumed game deadzone, 0..1               */
    double acc;         /* aim acceleration exponent, 1 = linear     */
    long   full;        /* mouse counts for full throw, per poll     */
    int    invert_y;    /* invert the vertical aim axis              */
    int    swap_sticks; /* mouse aims with the left stick instead    */
    int    mouse;       /* MKB_MOUSE_*                               */
    int    drag_btn;    /* mouse button mask that drags (mouse=drag) */
    int    drag_y;      /* drag moves the stick vertically too       */
    double walk;        /* left stick length while "walk" is held    */
    int    crosshair;   /* mouse = cursor: draw the pointer as a sight */
    unsigned crosshair_color; /* 0xRRGGBB                             */
} MkbConfig;

enum { MKB_MOUSE_AIM, MKB_MOUSE_DRAG, MKB_MOUSE_OFF, MKB_MOUSE_CURSOR };

static MkbConfig s_cfg = { 1.0, 0.18, 1.0, 8, 0, 0, MKB_MOUSE_AIM, 4, 0, 0.5, 1, 0xFFFFFF };

/* ---- bindings ----------------------------------------------------------
 *
 * "Input = target[+target...]" lines in the config bind keys and mouse
 * buttons to pad inputs; the first such line replaces the whole built-in
 * layout (below), so a title's config file states every binding it wants.
 *
 *   inputs:  A..Z, 0..9, F1..F12, Space, Enter, Esc, Tab, Backspace, Shift,
 *            Ctrl, Alt, Up, Down, Left, Right, Mouse1..Mouse5 (left, right,
 *            middle, back, forward), WheelUp, WheelDown
 *   targets: a b x y white black lt rt start back lthumb rthumb
 *            dup ddown dleft dright  lup ldown lleft lright
 *            rup rdown rleft rright  walk (left stick at "walk" length)
 *
 * A wheel step presses its target for a few frames, then lets go, so a
 * title that looks for button edges sees one press per step. */
enum {
    MKB_SRC_MOUSE = 256,                /* + button index 0..4 */
    MKB_SRC_WHEEL_UP = 261,
    MKB_SRC_WHEEL_DOWN = 262
};
enum {                                  /* targets past the 8 analog buttons */
    MKB_T_DIGITAL = 8,                  /* + bit index of wButtons (0..7) */
    MKB_T_STICK = 16,                   /* + 0..7: lup ldown lleft lright rup rdown rleft rright */
    MKB_T_WALK = 24
};
typedef struct { short src, target; } MkbBind;
#define MKB_MAX_BINDS 96
static MkbBind s_bind[MKB_MAX_BINDS];
static int s_nbind, s_bind_custom;

static int mkb_src_parse(const char *k)
{
    static const struct { const char *n; int v; } names[] = {
        { "space", VK_SPACE }, { "enter", VK_RETURN }, { "return", VK_RETURN },
        { "esc", VK_ESCAPE }, { "escape", VK_ESCAPE }, { "tab", VK_TAB },
        { "backspace", VK_BACK }, { "shift", VK_SHIFT }, { "ctrl", VK_CONTROL },
        { "control", VK_CONTROL }, { "alt", VK_MENU }, { "up", VK_UP },
        { "down", VK_DOWN }, { "left", VK_LEFT }, { "right", VK_RIGHT },
        { "mouse1", MKB_SRC_MOUSE + 0 }, { "mouse2", MKB_SRC_MOUSE + 1 },
        { "mouse3", MKB_SRC_MOUSE + 2 }, { "mouse4", MKB_SRC_MOUSE + 3 },
        { "mouse5", MKB_SRC_MOUSE + 4 }, { "wheelup", MKB_SRC_WHEEL_UP },
        { "wheeldown", MKB_SRC_WHEEL_DOWN }
    };
    size_t i;
    if (k[0] && !k[1]) {
        char c = (char)toupper((unsigned char)k[0]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            return c;
    }
    if ((k[0] == 'f' || k[0] == 'F') && atoi(k + 1) >= 1 && atoi(k + 1) <= 12)
        return VK_F1 + atoi(k + 1) - 1;
    for (i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!_stricmp(k, names[i].n))
            return names[i].v;
    return -1;
}

static int mkb_target_parse(const char *t)
{
    static const char *const names[] = {
        "a", "b", "x", "y", "black", "white", "lt", "rt",
        "dup", "ddown", "dleft", "dright", "start", "back", "lthumb", "rthumb",
        "lup", "ldown", "lleft", "lright", "rup", "rdown", "rleft", "rright",
        "walk"
    };
    int i;
    for (i = 0; i < (int)(sizeof names / sizeof names[0]); i++)
        if (!_stricmp(t, names[i]))
            return i;
    return -1;
}

/* "Mouse1" = "a+rt": returns 1 when the key named an input. */
static int mkb_bind_line(const char *key, const char *val)
{
    char buf[128], *tok, *next;
    int src = mkb_src_parse(key);
    if (src < 0)
        return 0;
    if (!s_bind_custom) {               /* first binding: drop the defaults */
        s_bind_custom = 1;
        s_nbind = 0;
    }
    snprintf(buf, sizeof buf, "%s", val);
    for (tok = buf; tok && *tok; tok = next) {
        int t;
        next = strchr(tok, '+');
        if (next) *next++ = 0;
        t = mkb_target_parse(tok);
        if (t < 0) {
            fprintf(stderr, "  [MKB] %s: unknown target '%s'\n", key, tok);
            continue;
        }
        if (s_nbind < MKB_MAX_BINDS) {
            s_bind[s_nbind].src = (short)src;
            s_bind[s_nbind].target = (short)t;
            s_nbind++;
        }
    }
    return 1;
}

/* The built-in layout: a PC shooter's (the layer's first title). */
static void mkb_default_binds(void)
{
    static const char *const d[][2] = {
        { "Mouse1", "rt" }, { "Mouse2", "lt" }, { "Space", "a" }, { "R", "x" },
        { "E", "b" }, { "Q", "white" }, { "F", "black" }, { "Mouse4", "white" },
        { "Mouse5", "black" }, { "Enter", "start" }, { "Esc", "start" },
        { "Tab", "back" }, { "Shift", "lthumb" }, { "Ctrl", "rthumb" },
        { "Up", "dup" }, { "Down", "ddown" }, { "Left", "dleft" },
        { "Right", "dright" }, { "WheelUp", "y" }, { "WheelDown", "y" },
        { "Mouse3", "y" }, { "W", "lup" }, { "S", "ldown" }, { "A", "lleft" },
        { "D", "lright" }
    };
    size_t i;
    for (i = 0; i < sizeof d / sizeof d[0]; i++)
        mkb_bind_line(d[i][0], d[i][1]);
    s_bind_custom = 0;                  /* a config's first line replaces these */
}

/* One "key=value" line; returns 1 when the key was recognised. */
static int mkb_apply_kv(const char *key, const char *val)
{
    double d;
    long   l;

    if (!strcmp(key, "sens")) {
        d = strtod(val, NULL); if (d > 0.0 && d < 100.0) s_cfg.sens = d;
    } else if (!strcmp(key, "dz")) {
        d = strtod(val, NULL); if (d >= 0.0 && d < 0.9) s_cfg.dz = d;
    } else if (!strcmp(key, "acc")) {
        d = strtod(val, NULL); if (d > 0.0 && d < 10.0) s_cfg.acc = d;
    } else if (!strcmp(key, "full")) {
        l = strtol(val, NULL, 0); if (l > 0 && l < 1000) s_cfg.full = l;
    } else if (!strcmp(key, "invert_y")) {
        s_cfg.invert_y = atoi(val) != 0;
    } else if (!strcmp(key, "swap_sticks")) {
        s_cfg.swap_sticks = atoi(val) != 0;
    } else if (!strcmp(key, "mouse")) {
        s_cfg.mouse = !_stricmp(val, "drag") ? MKB_MOUSE_DRAG
                    : !_stricmp(val, "off") ? MKB_MOUSE_OFF
                    : !_stricmp(val, "cursor") ? MKB_MOUSE_CURSOR : MKB_MOUSE_AIM;
    } else if (!strcmp(key, "drag_button")) {
        l = mkb_src_parse(val) - MKB_SRC_MOUSE;
        if (l >= 0 && l < 5) s_cfg.drag_btn = 1 << l;
    } else if (!strcmp(key, "drag_y")) {
        s_cfg.drag_y = atoi(val) != 0;
    } else if (!strcmp(key, "crosshair")) {
        s_cfg.crosshair = atoi(val) != 0;
    } else if (!strcmp(key, "crosshair_color")) {
        s_cfg.crosshair_color = (unsigned)strtoul(val, NULL, 16) & 0xFFFFFFu;
    } else if (!strcmp(key, "walk")) {
        d = strtod(val, NULL); if (d > 0.0 && d <= 1.0) s_cfg.walk = d;
    } else {
        return mkb_bind_line(key, val);
    }
    return 1;
}

static void mkb_load_config(void)
{
    static const char *kv[] = {
        "RECOMP_MKB_SENS", "sens",
        "RECOMP_MKB_DZ",   "dz",
        "RECOMP_MKB_ACC",  "acc",
        "RECOMP_MKB_FULL", "full",
        "RECOMP_MKB_INVERT_Y",  "invert_y",
        "RECOMP_MKB_SWAP_STICKS", "swap_sticks",
        "RECOMP_MKB_MOUSE", "mouse",
        NULL
    };
    char path[MAX_PATH];
    const char *ini;
    DWORD n;
    FILE *f;
    int i;

    mkb_default_binds();
    for (i = 0; kv[i]; i += 2) {
        const char *v = getenv(kv[i]);
        if (v && *v)
            mkb_apply_kv(kv[i + 1], v);
    }

    ini = getenv("RECOMP_MKB_INI");
    if (ini && *ini) {
        snprintf(path, sizeof path, "%s", ini);
    } else {
        n = GetModuleFileNameA(NULL, path, sizeof path - 12);
        if (!n || n >= sizeof path - 12)
            return;
        snprintf(path + n, 12, "\\..\\mkb.ini");   /* beside the .exe */
    }
    f = fopen(path, "r");
    if (!f)
        return;
    fprintf(stderr, "  [MKB] config %s\n", path);
    {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            char *eq, *k, *v;
            line[strcspn(line, ";#")] = 0;      /* comments */
            eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            k = line; v = eq + 1;
            while (*k == ' ' || *k == '\t') k++;
            while (*v == ' ' || *v == '\t') v++;
            k[strcspn(k, " \t\r\n")] = 0;
            v[strcspn(v, " \t\r\n")] = 0;
            if (*k && *v && !mkb_apply_kv(k, v))
                fprintf(stderr, "  [MKB] %s: unknown setting '%s'\n", path, k);
        }
    }
    fclose(f);
}

/* ---- raw input thread -------------------------------------------------- */

/* Accumulators, written by the input thread and drained by the poll.
 * InterlockedExchange is the whole protocol: the input thread adds
 * with InterlockedAdd, the poll takes everything with Exchange(0). */
static volatile LONG s_dx, s_dy;
static volatile LONG s_wheel;        /* wheel steps since last poll   */
static volatile LONG s_mouse_btn;    /* held: 1 L, 2 R, 4 M, 8 X1, 16 X2 */
static volatile LONG s_mouse_click;  /* pressed since the last poll   */
static volatile unsigned char s_vk_down[256];
static volatile unsigned char s_vk_tap[256]; /* pressed since the last poll */

static volatile LONG s_active = 1;   /* F9 toggles this               */
static volatile LONG s_thread_up;

/* F9 is read on the input thread, so the toggle works even while the
 * game is stalled on the poll side. */
#define MKB_TOGGLE_VK  VK_F9

static void mkb_handle_raw(HRAWINPUT h)
{
    BYTE buf[sizeof(RAWINPUT) + 64];
    UINT size = sizeof buf;
    RAWINPUT *ri = (RAWINPUT *)buf;

    if (GetRawInputData(h, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER))
            == (UINT)-1)
        return;

    if (ri->header.dwType == RIM_TYPEMOUSE) {
        const RAWMOUSE *m = &ri->data.mouse;

        if (!(m->usFlags & MOUSE_MOVE_ABSOLUTE)) {
            if (m->lLastX) InterlockedAdd(&s_dx, m->lLastX);
            if (m->lLastY) InterlockedAdd(&s_dy, m->lLastY);
        }
        if (m->usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) {
            InterlockedOr(&s_mouse_btn, 1);
            InterlockedOr(&s_mouse_click, 1);
        }
        if (m->usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP)
            InterlockedAnd(&s_mouse_btn, ~1);
        if (m->usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN) {
            InterlockedOr(&s_mouse_btn, 2);
            InterlockedOr(&s_mouse_click, 2);
        }
        if (m->usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP)
            InterlockedAnd(&s_mouse_btn, ~2);
        if (m->usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_DOWN) {
            InterlockedOr(&s_mouse_btn, 4);
            InterlockedOr(&s_mouse_click, 4);
        }
        if (m->usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_UP)
            InterlockedAnd(&s_mouse_btn, ~4);
        if (m->usButtonFlags & RI_MOUSE_BUTTON_4_DOWN) {
            InterlockedOr(&s_mouse_btn, 8);
            InterlockedOr(&s_mouse_click, 8);
        }
        if (m->usButtonFlags & RI_MOUSE_BUTTON_4_UP)
            InterlockedAnd(&s_mouse_btn, ~8);
        if (m->usButtonFlags & RI_MOUSE_BUTTON_5_DOWN) {
            InterlockedOr(&s_mouse_btn, 16);
            InterlockedOr(&s_mouse_click, 16);
        }
        if (m->usButtonFlags & RI_MOUSE_BUTTON_5_UP)
            InterlockedAnd(&s_mouse_btn, ~16);
        if (m->usButtonFlags & RI_MOUSE_WHEEL) {
            SHORT d = (SHORT)m->usButtonData;
            InterlockedAdd(&s_wheel, d / WHEEL_DELTA);
        }
    } else if (ri->header.dwType == RIM_TYPEKEYBOARD) {
        const RAWKEYBOARD *k = &ri->data.keyboard;
        UINT vk = k->VKey;

        if (vk == MKB_TOGGLE_VK) {
            if (k->Message == WM_KEYDOWN && !s_vk_down[vk & 0xFF]) {
                LONG now = !s_active;
                InterlockedExchange(&s_active, now);
                fprintf(stderr, "  [MKB] %s\n", now ? "captured"
                                                    : "released (F9)");
                fflush(stderr);
            }
        }
        if (vk && vk < 256) {
            if (k->Message == WM_KEYDOWN || k->Message == WM_SYSKEYDOWN) {
                s_vk_down[vk] = 1;
                s_vk_tap[vk] = 1;
            }
            else
                s_vk_down[vk] = 0;
        }
    }
}

static LRESULT CALLBACK mkb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_INPUT) {
        mkb_handle_raw((HRAWINPUT)l);
        return 0;
    }
    if (m == WM_CLOSE || m == WM_DESTROY)
        return 0;
    return DefWindowProcA(h, m, w, l);
}

static DWORD WINAPI mkb_thread(LPVOID unused)
{
    WNDCLASSA wc;
    HWND hwnd;
    RAWINPUTDEVICE rid[2];
    MSG msg;

    (void)unused;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = mkb_wndproc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.lpszClassName = "XboxRecompMKB";
    RegisterClassA(&wc);
    /* Not visible: it exists to own the WM_INPUT delivery, nothing else. */
    hwnd = CreateWindowExA(0, "XboxRecompMKB", "mkb", WS_OVERLAPPED,
                           0, 0, 0, 0, NULL, NULL,
                           GetModuleHandleA(NULL), NULL);
    if (!hwnd) {
        fprintf(stderr, "  [MKB] window creation failed (%lu)\n",
                (unsigned long)GetLastError());
        return 0;
    }

    memset(rid, 0, sizeof rid);
    rid[0].usUsagePage = 0x01;  rid[0].usUsage = 0x02;  /* mouse     */
    rid[1].usUsagePage = 0x01;  rid[1].usUsage = 0x06;  /* keyboard  */
    rid[0].dwFlags = RIDEV_INPUTSINK;   /* deliver without focus */
    rid[1].dwFlags = RIDEV_INPUTSINK;
    rid[0].hwndTarget = hwnd;
    rid[1].hwndTarget = hwnd;
    if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE))) {
        fprintf(stderr, "  [MKB] RegisterRawInputDevices failed (%lu)\n",
                (unsigned long)GetLastError());
        return 0;
    }

    InterlockedExchange(&s_thread_up, 1);
    fprintf(stderr, "  [MKB] mouse+keyboard captured (F9 releases; "
            "sens=%.2f dz=%.2f full=%ld)\n",
            s_cfg.sens, s_cfg.dz, s_cfg.full);
    fflush(stderr);

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    InterlockedExchange(&s_thread_up, 0);
    return 0;
}

/* ---- poll side --------------------------------------------------------- */

int mkb_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        /* Opt-in, like RECOMP_KEYBOARD: a keyboard silently acting as
         * player 1 is surprising when a real pad is what you meant to use.
         * RECOMP_MKB=1 turns the layer on. */
        const char *v = getenv("RECOMP_MKB");
        on = v && *v && *v != '0';
        if (on) {
            mkb_load_config();
            if (InterlockedCompareExchange(&s_thread_up, 0, 0) == 0) {
                static LONG started = 0;
                if (InterlockedCompareExchange(&started, 1, 0) == 0)
                    CloseHandle(CreateThread(NULL, 0, mkb_thread,
                                             NULL, 0, NULL));
            }
        }
    }
    return on;
}

/* The recompiled game's process is the foreground one. Raw input with
 * INPUTSINK keeps arriving while the user is in another app, and the
 * mouse must go on behaving normally there. */
static int game_is_foreground(void)
{
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (!fg)
        return 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

/* Hide and confine the pointer while captured. Aiming with raw deltas
 * does not move the pointer by itself, but Windows still moves it from
 * the same physical movement, so it would wander across the screen and
 * end up clicking other apps' title bars. */
static void mkb_set_sight(HWND hwnd, int on);

static void mkb_manage_cursor(int capture)
{
    static int hidden;          /* how many ShowCursor(FALSE) we owe  */
    static unsigned long last;

    unsigned long now = GetTickCount();
    if (now - last < 500 && (capture ? hidden : !hidden) == 0)
        return;
    last = now;

    if (capture) {
        RECT r;
        HWND fg = GetForegroundWindow();
        if (fg)
            mkb_set_sight(fg, 1);
        if (fg && GetClientRect(fg, &r)) {
            /* Client area in screen coordinates: the pointer cannot leave
             * the picture for the title bar or another app. */
            POINT a = { r.left, r.top }, b = { r.right, r.bottom };
            ClientToScreen(fg, &a);
            ClientToScreen(fg, &b);
            SetRect(&r, a.x, a.y, b.x, b.y);
            ClipCursor(&r);
        }
        /* A cursor the title aims at stays visible. */
        if (s_cfg.mouse != MKB_MOUSE_CURSOR && hidden < 5) { ShowCursor(FALSE); hidden++; }
        if (s_cfg.mouse == MKB_MOUSE_CURSOR && hidden > 0) { ShowCursor(TRUE); hidden--; }
    } else {
        mkb_set_sight(NULL, 0);
        if (hidden > 0) { ShowCursor(TRUE); hidden--; }
        ClipCursor(NULL);
    }
}

/* The pointer as a sight (mouse = cursor, crosshair = 1): a ring with four
 * ticks and a dot, in crosshair_color over a dark edge so it reads on sand
 * and on shadow alike. Drawn here, antialiased, 32x32 with the hot spot in
 * the middle. */
static float sight_cover(float x, float y, float grow)
{
    float r = sqrtf(x * x + y * y), c = 0.0f, d;
    float ax = fabsf(x), ay = fabsf(y);
    d = fabsf(r - 8.5f) - (0.9f + grow);                      /* ring */
    if (d < 0.5f) c = fmaxf(c, fminf(1.0f, 0.5f - d));
    if (r < 1.6f + grow) c = fmaxf(c, fminf(1.0f, 1.6f + grow - r)); /* dot */
    /* ticks: from r 4 to 13 on both axes, 1 px wide */
    if (ax >= 4.0f - grow && ax <= 13.0f + grow) {
        d = ay - (0.6f + grow);
        if (d < 0.5f) c = fmaxf(c, fminf(1.0f, 0.5f - d));
    }
    if (ay >= 4.0f - grow && ay <= 13.0f + grow) {
        d = ax - (0.6f + grow);
        if (d < 0.5f) c = fmaxf(c, fminf(1.0f, 0.5f - d));
    }
    return c;
}

static HCURSOR mkb_sight_cursor(void)
{
    static HCURSOR cur;
    BITMAPV5HEADER bh;
    ICONINFO ii;
    HBITMAP color, mask;
    uint32_t *px = NULL;
    HDC dc;
    int x, y;
    if (cur)
        return cur;
    memset(&bh, 0, sizeof bh);
    bh.bV5Size = sizeof bh;
    bh.bV5Width = 32;
    bh.bV5Height = -32;
    bh.bV5Planes = 1;
    bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask = 0x00FF0000;
    bh.bV5GreenMask = 0x0000FF00;
    bh.bV5BlueMask = 0x000000FF;
    bh.bV5AlphaMask = 0xFF000000;
    dc = GetDC(NULL);
    color = CreateDIBSection(dc, (BITMAPINFO *)&bh, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!color || !px)
        return NULL;
    for (y = 0; y < 32; y++)
        for (x = 0; x < 32; x++) {
            float fx = (float)x + 0.5f - 16.0f, fy = (float)y + 0.5f - 16.0f;
            float fg = sight_cover(fx, fy, 0.0f), edge = sight_cover(fx, fy, 1.0f) * 0.75f;
            float a = fg + edge * (1.0f - fg);
            float r = (float)((s_cfg.crosshair_color >> 16) & 0xFF) * fg;
            float g = (float)((s_cfg.crosshair_color >> 8) & 0xFF) * fg;
            float b = (float)(s_cfg.crosshair_color & 0xFF) * fg;
            /* premultiplied: the dark edge adds alpha, no colour */
            px[y * 32 + x] = (uint32_t)(a * 255.0f + 0.5f) << 24
                           | (uint32_t)(r + 0.5f) << 16 | (uint32_t)(g + 0.5f) << 8
                           | (uint32_t)(b + 0.5f);
        }
    mask = CreateBitmap(32, 32, 1, 1, NULL);
    memset(&ii, 0, sizeof ii);
    ii.fIcon = FALSE;
    ii.xHotspot = 16;
    ii.yHotspot = 16;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    cur = (HCURSOR)CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return cur;
}

/* The game window's own pointer: the sight while captured in cursor mode,
 * the arrow otherwise. The class cursor is what the window shows on every
 * WM_SETCURSOR, whichever thread owns it. */
static void mkb_set_sight(HWND hwnd, int on)
{
    static HWND set_on;
    HCURSOR c;
    if (on && (!s_cfg.crosshair || s_cfg.mouse != MKB_MOUSE_CURSOR))
        on = 0;
    if (on ? set_on == hwnd : !set_on)
        return;
    if (on) {
        if (!(c = mkb_sight_cursor()))
            return;
        SetClassLongPtrA(hwnd, GCLP_HCURSOR, (LONG_PTR)c);
        set_on = hwnd;
    } else {
        SetClassLongPtrA(set_on, GCLP_HCURSOR, (LONG_PTR)LoadCursorA(NULL, IDC_ARROW));
        set_on = NULL;
    }
    /* Shown from the next mouse move; nudge it there now. */
    {
        POINT pt;
        if (GetCursorPos(&pt))
            SetCursorPos(pt.x, pt.y);
    }
}

/* Radial deadzone compensation: an input radius in [0..1] comes out as
 * [dz..1], so the smallest mouse correction still clears the title's
 * own deadzone, and the response stays linear above it. */
static void mkb_aim_vector(double ax, double ay, SHORT *out_x, SHORT *out_y)
{
    double r = sqrt(ax * ax + ay * ay);
    double len, fx, fy;
    long v;

    if (r <= 0.0) { *out_x = 0; *out_y = 0; return; }
    if (r > 1.0) r = 1.0;
    len = s_cfg.dz + (1.0 - s_cfg.dz) * pow(r, s_cfg.acc);
    len *= s_cfg.sens;
    if (len > 1.0) len = 1.0;
    fx = ax * len / sqrt(ax * ax + ay * ay);
    fy = ay * len / sqrt(ax * ax + ay * ay);
    v = (long)(fx * 32767.0);
    *out_x = (SHORT)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    v = (long)(fy * 32767.0);
    *out_y = (SHORT)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

static unsigned char s_target_held[MKB_T_WALK + 1];

int mkb_target_held(const char *target)
{
    int t = mkb_target_parse(target);
    return t >= 0 ? s_target_held[t] : 0;
}

static mkb_cursor_hook s_cursor_hook;
void mkb_set_cursor_hook(mkb_cursor_hook fn) { s_cursor_hook = fn; }

/* Tell the title where the pointer is (mouse = cursor). */
static void mkb_cursor_report(int active)
{
    static int was_active;
    static int test = -1;
    static float tx, ty, ta = 4.0f / 3.0f;
    POINT pt;
    RECT rc;
    HWND hwnd;

    if (!s_cursor_hook)
        return;
    if (test < 0) {
        const char *e = getenv("RECOMP_MKB_CURSOR_TEST");
        test = e && sscanf(e, "%f,%f,%f", &tx, &ty, &ta) >= 2;
    }
    if (test) {
        s_cursor_hook(1, tx, ty, ta);
        return;
    }
    if (!active || s_cfg.mouse != MKB_MOUSE_CURSOR) {
        if (was_active)
            s_cursor_hook(0, 0.0f, 0.0f, 1.0f);
        was_active = 0;
        return;
    }
    hwnd = GetForegroundWindow();
    if (!hwnd || !GetCursorPos(&pt) || !ScreenToClient(hwnd, &pt)
        || !GetClientRect(hwnd, &rc) || rc.right <= 0 || rc.bottom <= 0)
        return;
    was_active = 1;
    s_cursor_hook(1, (float)pt.x / (float)rc.right, (float)pt.y / (float)rc.bottom,
                  (float)rc.right / (float)rc.bottom);
}

#define MK_VK(k)  ((unsigned)(k) < 256 && s_vk_down[(k)])

/* Wheel steps become presses of a few frames each (WHEEL_ON_MS held, then
 * WHEEL_OFF_MS released), queued, so every step is a separate edge. */
#define WHEEL_ON_MS  70
#define WHEEL_OFF_MS 70
static int wheel_pulse(int dir, LONG steps)
{
    static int queued[2];
    static ULONGLONG until[2];
    static int on[2];
    ULONGLONG now = GetTickCount64();
    queued[dir] += (int)steps;
    if (queued[dir] > 4) queued[dir] = 4;
    if (on[dir]) {
        if (now < until[dir])
            return 1;
        on[dir] = 0;
        until[dir] = now + WHEEL_OFF_MS;
        return 0;
    }
    if (queued[dir] > 0 && now >= until[dir]) {
        queued[dir]--;
        on[dir] = 1;
        until[dir] = now + WHEEL_ON_MS;
        return 1;
    }
    return 0;
}

int mkb_merge(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    SHORT aim_x = 0, aim_y = 0, mov_x = 0, mov_y = 0, rs_x = 0, rs_y = 0;
    WORD b = 0;
    BYTE analog[8];
    LONG dx, dy, wheel, btn;
    int any = 0, i, walk = 0, wheel_on[2];
    int stick[8];
    unsigned char tap[256];
    static DWORD packet;

    if (!pState || dwPort != 0 || !mkb_enabled())
        return 0;
    if (!s_thread_up)
        return 0;

    if (!s_active) {
        mkb_manage_cursor(0);
        mkb_cursor_report(0);
        return 0;
    }

    /* Foreground gate: while the user is elsewhere the mouse belongs to
     * whoever has it, and the game should read a pad at rest. */
    if (!game_is_foreground()) {
        mkb_manage_cursor(0);
        memset(s_target_held, 0, sizeof s_target_held);
        mkb_cursor_report(0);
        InterlockedExchange(&s_dx, 0);
        InterlockedExchange(&s_dy, 0);
        InterlockedExchange(&s_wheel, 0);
        InterlockedExchange(&s_mouse_click, 0);
        memset((void *)s_vk_tap, 0, sizeof s_vk_tap);
        return 0;
    }
    mkb_manage_cursor(1);

    dx    = InterlockedExchange(&s_dx, 0);
    dy    = InterlockedExchange(&s_dy, 0);
    wheel = InterlockedExchange(&s_wheel, 0);
    /* Held now, or clicked since the last poll: a click shorter than a
     * frame still reaches the title. */
    btn   = s_mouse_btn | InterlockedExchange(&s_mouse_click, 0);
    for (i = 0; i < 256; i++) {
        tap[i] = s_vk_tap[i];
        if (tap[i]) s_vk_tap[i] = 0;
    }
    wheel_on[0] = wheel_pulse(0, wheel > 0 ? wheel : 0);
    wheel_on[1] = wheel_pulse(1, wheel < 0 ? -wheel : 0);

    memset(analog, 0, sizeof analog);
    memset(stick, 0, sizeof stick);
    memset(s_target_held, 0, sizeof s_target_held);

    /* ---- bindings ---- */
    for (i = 0; i < s_nbind; i++) {
        int src = s_bind[i].src, t = s_bind[i].target, held;
        if (src < 256)
            held = s_vk_down[src] || tap[src];
        else if (src < MKB_SRC_WHEEL_UP)
            held = (btn >> (src - MKB_SRC_MOUSE)) & 1;
        else
            held = wheel_on[src == MKB_SRC_WHEEL_DOWN];
        if (!held)
            continue;
        s_target_held[t] = 1;
        if (t < MKB_T_DIGITAL)
            analog[t] = 255;
        else if (t < MKB_T_STICK)
            b |= (WORD)(1u << (t - MKB_T_DIGITAL));
        else if (t < MKB_T_WALK)
            stick[t - MKB_T_STICK] = 1;
        else
            walk = 1;
    }

    /* ---- keys on the sticks: full throw, opposite keys cancel ---- */
    {
        int mx = stick[3] - stick[2], my = stick[0] - stick[1];
        int rx = stick[7] - stick[6], ry = stick[4] - stick[5];
        double k = (mx && my) ? 0.7071 : 1.0;
        if (mx || my) {
            mkb_aim_vector(mx * k, my * k, &mov_x, &mov_y);
            if (walk) {
                mov_x = (SHORT)(mov_x * s_cfg.walk);
                mov_y = (SHORT)(mov_y * s_cfg.walk);
            }
        }
        k = (rx && ry) ? 0.7071 : 1.0;
        if (rx || ry) {
            rs_x = (SHORT)(rx * k * 32767.0);
            rs_y = (SHORT)(ry * k * 32767.0);
        }
    }

    /* ---- mouse movement ---- */
    /* aim: always; drag and cursor: while the drag button is held. */
    if ((dx || dy) && s_cfg.mouse != MKB_MOUSE_OFF &&
        (s_cfg.mouse == MKB_MOUSE_AIM || (btn & s_cfg.drag_btn))) {
        double ax =  (double)dx / (double)s_cfg.full;
        double ay = -(double)dy / (double)s_cfg.full;
        if (s_cfg.invert_y) ay = -ay;
        if (s_cfg.mouse != MKB_MOUSE_AIM && !s_cfg.drag_y) ay = 0.0;
        mkb_aim_vector(ax, ay, &aim_x, &aim_y);
    }
    if (!aim_x && !aim_y) {
        aim_x = rs_x;
        aim_y = rs_y;
    }

    /* ---- merge on top of whatever the host pad produced ---- */
    if (b)        { pState->Gamepad.wButtons |= b; any = 1; }
    for (i = 0; i < 8; i++) {
        if (analog[i]) {
            if (analog[i] > pState->Gamepad.bAnalogButtons[i])
                pState->Gamepad.bAnalogButtons[i] = analog[i];
            any = 1;
        }
    }
    if (aim_x || aim_y) {
        if (s_cfg.swap_sticks) {
            pState->Gamepad.sThumbLX = aim_x;
            pState->Gamepad.sThumbLY = aim_y;
        } else {
            pState->Gamepad.sThumbRX = aim_x;
            pState->Gamepad.sThumbRY = aim_y;
        }
        any = 1;
    }
    if (mov_x || mov_y) {
        if (s_cfg.swap_sticks) {
            pState->Gamepad.sThumbRX = mov_x;
            pState->Gamepad.sThumbRY = mov_y;
        } else {
            pState->Gamepad.sThumbLX = mov_x;
            pState->Gamepad.sThumbLY = mov_y;
        }
        any = 1;
    }

    mkb_cursor_report(1);

    /* Edge detection: a title polling for changes ignores a state whose
     * packet number did not move. */
    if (any)
        pState->dwPacketNumber = ++packet;
    return any;
}
