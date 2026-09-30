/**
 * Controller input at the XAPI level.
 *
 * The game reads pads through XAPI (XGetDevices / XInputOpen / XInputGetState),
 * which sits on XPP's USB driver talking to the OHCI controller. Emulating the
 * USB wire protocol well enough for that driver needs bus-master DMA the
 * runtime's split memory model cannot express yet (buffers in the image and in
 * the contiguous window share physical addresses). These replacements answer
 * at the API instead: up to four gamepads, their state from the PC.
 *
 * Ports: Xbox port 0 is always connected -- the first XInput pad on the PC,
 * plus the keyboard while the game window has focus. Each further connected
 * XInput pad is the next port (1, 2, 3), so two PC controllers are players 1
 * and 2 in multiplayer. The game polls XGetDevices for which ports have a
 * pad, so plugging one in or out mid-game works as on the console.
 *
 * Test scripts: BUFFY_PAD_SCRIPT="secs:BUTTON[*hold],..." (port 0, from the
 * first read) and BUFFY_PAD_SCRIPT2 (port 0, timed from when the main menu
 * is built); BUFFY_PAD2_SCRIPT (port 1, from the main menu) connects a
 * second pad. Buttons: START BACK A B X Y BLACK WHITE LT RT UP DOWN LEFT RIGHT LSU LSD
 * LSL LSR RSU RSD RSL RSR.
 *
 * All are stdcall: [esp] is the return address, arguments follow, the callee
 * pops them.
 */
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "recomp/gen/recomp_types.h"
#include "../../xboxrecomp/src/input/xinput_xbox.h"
#include <xinput.h>
#include <math.h>
#include <stdio.h>
#pragma comment(lib, "xinput.lib")

#define GAMEPAD_TYPE_TABLE  0x0018E914u   /* XDEVICE_TYPE_GAMEPAD_TABLE */
#define FAKE_HANDLE         0x0BADF00Du   /* + port; opaque, only we interpret it */
#define MAX_PORTS           4

#define BTN_UP     0x0001
#define BTN_DOWN   0x0002
#define BTN_LEFT   0x0004
#define BTN_RIGHT  0x0008
#define BTN_START  0x0010
#define BTN_BACK   0x0020

static void ret_stdcall(uint32_t result, uint32_t arg_bytes)
{
    g_eax = result;
    g_esp += 4 + arg_bytes;
}

static int handle_port(uint32_t h)
{
    return (h >= FAKE_HANDLE && h < FAKE_HANDLE + MAX_PORTS) ? (int)(h - FAKE_HANDLE) : -1;
}

/* ── PC controllers -> Xbox ports ──────────────────────────────────────── */

static int s_slot[MAX_PORTS] = { -1, -1, -1, -1 };   /* XInput slot per port */

/* Port n = the n-th connected XInput pad. Empty slots are slow to query, so
 * the list is rebuilt at most once a second (or when a pad stops answering). */
static void scan_pads(int force)
{
    static DWORD last;
    XINPUT_STATE st;
    int p, n = 0, old[MAX_PORTS];
    if (!force && last && GetTickCount() - last < 1000)
        return;
    last = GetTickCount();
    memcpy(old, s_slot, sizeof old);
    for (p = 0; p < MAX_PORTS; p++)
        s_slot[p] = -1;
    for (p = 0; p < 4; p++)
        if (XInputGetState((DWORD)p, &st) == ERROR_SUCCESS)
            s_slot[n++] = p;
    for (p = 0; p < MAX_PORTS; p++)
        if (s_slot[p] != old[p])
            fprintf(stderr, "  [INPUT] port %d: %s\n", p + 1,
                    s_slot[p] >= 0 ? "controller connected" : "no controller");
}

static int s_pad2_script = -1;          /* BUFFY_PAD2_SCRIPT given */

/* Which Xbox ports have a pad (bit per port). Port 0 is always there. */
static uint32_t connected_mask(void)
{
    uint32_t m = 1;
    int p;
    if (s_pad2_script < 0)
        s_pad2_script = getenv("BUFFY_PAD2_SCRIPT") != NULL;
    scan_pads(0);
    for (p = 1; p < MAX_PORTS; p++)
        if (s_slot[p] >= 0)
            m |= 1u << p;
    if (s_pad2_script)
        m |= 2;
    return m;
}

/* DWORD XGetDevices(PXPP_DEVICE_TYPE) -- 0x0018F7FD.
 * Same bookkeeping as XAPI (report current, clear changes, remember it as the
 * previous state); the gamepad type shows the ports that have a pad. */
void XGetDevices_0018F7FD(void)
{
    uint32_t type = MEM32(g_esp + 4);
    uint32_t cur;

    if (type == GAMEPAD_TYPE_TABLE)
        MEM32(type) = connected_mask();
    {
        static int n;
        if (getenv("BUFFY_INPUT_LOG") && n++ < 20)
            fprintf(stderr, "  [INPUT] XGetDevices type %08X -> %X\n", type, MEM32(type));
    }
    cur = MEM32(type);
    MEM32(type + 4) = 0;
    MEM32(type + 8) = cur;
    ret_stdcall(cur, 4);
}

/* HANDLE XInputOpen(type, port, slot, PXINPUT_POLLING_PARAMETERS) -- 0x0018FA98 */
void XInputOpen_0018FA98(void)
{
    uint32_t type = MEM32(g_esp + 4);
    uint32_t port = MEM32(g_esp + 8);
    if (getenv("BUFFY_INPUT_LOG"))
        fprintf(stderr, "  [INPUT] XInputOpen type %08X port %u (mask %X)\n", type, port, connected_mask());
    ret_stdcall((type == GAMEPAD_TYPE_TABLE && port < MAX_PORTS && (connected_mask() >> port & 1))
                    ? FAKE_HANDLE + port : 0, 16);
}

/* VOID XInputClose(HANDLE) -- 0x0018FAEE */
void XInputClose_0018FAEE(void)
{
    ret_stdcall(0, 4);
}

/* DWORD XInputSetState(HANDLE, PXINPUT_FEEDBACK) -- 0x0018FB6D.
 * Rumble goes to the port's PC pad and is completed at once: the header's
 * status reads ERROR_SUCCESS so a title polling it for completion moves on. */
static void rumble_off(void)
{
    XINPUT_VIBRATION v = { 0, 0 };
    int p;
    for (p = 0; p < MAX_PORTS; p++)
        if (s_slot[p] >= 0)
            XInputSetState((DWORD)s_slot[p], &v);
}

void XInputSetState_0018FB6D(void)
{
    /* XINPUT_FEEDBACK: header { DWORD status; HANDLE event; BYTE reserved[58] }
     * then XINPUT_RUMBLE { WORD left, right } at +66. */
    static int init, off;
    int port = handle_port(MEM32(g_esp + 4));
    uint32_t fb = MEM32(g_esp + 8);
    if (!init) {
        init = 1;
        off = getenv("BUFFY_NO_RUMBLE") && getenv("BUFFY_NO_RUMBLE")[0] == '1';
        atexit(rumble_off);
    }
    if (fb) {
        if (port >= 0 && s_slot[port] >= 0 && !off) {
            XINPUT_VIBRATION v;
            v.wLeftMotorSpeed = MEM16(fb + 66);
            v.wRightMotorSpeed = MEM16(fb + 68);
            XInputSetState((DWORD)s_slot[port], &v);
        }
        MEM32(fb) = 0;
    }
    ret_stdcall(0, 8);
}

/* ── state sources ─────────────────────────────────────────────────────── */

static int game_has_focus(void)
{
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (!fg)
        return 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static int key(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static void apply_button(XBOX_GAMEPAD *g, const char *b)
{
    if (!strcmp(b, "UP"))    g->wButtons |= BTN_UP;
    if (!strcmp(b, "DOWN"))  g->wButtons |= BTN_DOWN;
    if (!strcmp(b, "LEFT"))  g->wButtons |= BTN_LEFT;
    if (!strcmp(b, "RIGHT")) g->wButtons |= BTN_RIGHT;
    if (!strcmp(b, "START")) g->wButtons |= BTN_START;
    if (!strcmp(b, "BACK"))  g->wButtons |= BTN_BACK;
    if (!strcmp(b, "A"))     g->bAnalogButtons[0] = 0xFF;
    if (!strcmp(b, "B"))     g->bAnalogButtons[1] = 0xFF;
    if (!strcmp(b, "X"))     g->bAnalogButtons[2] = 0xFF;
    if (!strcmp(b, "Y"))     g->bAnalogButtons[3] = 0xFF;
    if (!strcmp(b, "BLACK")) g->bAnalogButtons[4] = 0xFF;
    if (!strcmp(b, "WHITE")) g->bAnalogButtons[5] = 0xFF;
    if (!strcmp(b, "LT"))    g->bAnalogButtons[6] = 0xFF;
    if (!strcmp(b, "RT"))    g->bAnalogButtons[7] = 0xFF;
    if (!strcmp(b, "LSU"))   g->sThumbLY = 32767;       /* left stick */
    if (!strcmp(b, "LSD"))   g->sThumbLY = -32768;
    if (!strcmp(b, "LSL"))   g->sThumbLX = -32768;
    if (!strcmp(b, "LSR"))   g->sThumbLX = 32767;
    if (!strcmp(b, "RSL"))   g->sThumbRX = -32768;      /* right stick */
    if (!strcmp(b, "RSR"))   g->sThumbRX = 32767;
    if (!strcmp(b, "RSU"))   g->sThumbRY = 32767;
    if (!strcmp(b, "RSD"))   g->sThumbRY = -32768;
}

/* Scripted input for tests (see the top of the file). */
typedef struct { DWORD ms, dur; char name[8]; } PadEvent;

static volatile LONG s_menu_ready;

void buffy_input_menu_ready(void)
{
    InterlockedCompareExchange(&s_menu_ready, (LONG)GetTickCount() | 1, 0);
}

/* (testing) BUFFY_PAD_LEVEL_CLOCK=1: the second scripts count from the first
 * level's start (player 1 made), loads taking what they take */
static volatile LONG s_level_ready;

void buffy_input_level_ready(void)
{
    InterlockedCompareExchange(&s_level_ready, (LONG)GetTickCount() | 1, 0);
}

static int parse_script(const char *s, PadEvent *ev, int max)
{
    int nev = 0;
    while (s && *s && nev < max) {
        char *end;
        double sec = strtod(s, &end);
        int n = 0;
        if (end == s || *end != ':')
            break;
        s = end + 1;
        while (*s && *s != ',' && *s != '*' && n < 7)
            ev[nev].name[n++] = *s++;
        ev[nev].name[n] = 0;
        ev[nev].ms = (DWORD)(sec * 1000.0);
        ev[nev].dur = 250;                       /* a tap; NAME*secs holds it */
        if (*s == '*')
            ev[nev].dur = (DWORD)(strtod(s + 1, (char **)&s) * 1000.0);
        nev++;
        if (*s == ',')
            s++;
    }
    return nev;
}

static void run_events(XBOX_GAMEPAD *g, const PadEvent *ev, int nev, DWORD rel)
{
    int k;
    for (k = 0; k < nev; k++)
        if (rel >= ev[k].ms && rel < ev[k].ms + ev[k].dur)
            apply_button(g, ev[k].name);
}

/* Testing: a button held on player 1's pad for the tests in buffy_mods.c. */
const char *volatile g_test_button;

int buffy_mouse_look_allowed(void);

static void apply_script(int port, XBOX_GAMEPAD *g)
{
    if (port == 0 && g_test_button)
        apply_button(g, g_test_button);
    static int inited, n1, n2, np2;
    static PadEvent e1[64], e2[64], ep2[64];
    static DWORD t0;
    LONG menu;

    if (!inited) {
        inited = 1;
        t0 = GetTickCount();
        n1 = parse_script(getenv("BUFFY_PAD_SCRIPT"), e1, 64);
        n2 = parse_script(getenv("BUFFY_PAD_SCRIPT2"), e2, 64);
        np2 = parse_script(getenv("BUFFY_PAD2_SCRIPT"), ep2, 64);
    }
    menu = s_menu_ready;
    if (getenv("BUFFY_PAD_LEVEL_CLOCK")) {
        menu = s_level_ready;
        if (atoi(getenv("BUFFY_PAD_LEVEL_CLOCK")) == 2) {
            /* =2: from when play has run for two seconds (cutscenes over) */
            static int since;
            static LONG play_ready;
            if (!play_ready) {
                since = buffy_mouse_look_allowed() ? since + 1 : 0;
                if (since > 120)
                    play_ready = (LONG)GetTickCount() | 1;
            }
            menu = play_ready;
        }
    }
    if (port == 1) {
        if (np2 && menu)
            run_events(g, ep2, np2, GetTickCount() - (DWORD)menu);
        return;
    }
    if (port != 0)
        return;
    /* With a second script the first stops at the menu, so it can keep
     * pressing Start to get past the title without choosing New Game. */
    if (!n2 || !menu)
        run_events(g, e1, n1, GetTickCount() - t0);
    else
        run_events(g, e2, n2, GetTickCount() - (DWORD)menu);
}

/* ── keyboard and mouse ───────────────────────────────────────────────────
 *
 * Player 1's pad also takes the keyboard and mouse while the game window has
 * focus. Every pad input is an action with up to two keys, from [Keys] in
 * buffy_settings.ini (the launcher's Controls tab writes it):
 *     Jump = Space, MouseMiddle        (names: see k_key_names)
 * Mouse look: while playing (not in a menu, the pause or a cutscene -- the
 * game says through buffy_mouse_look_allowed) the cursor is hidden and kept in
 * the window and its movement turns the camera, as the right stick does
 * ([Controls] MouseLook, MouseSensitivity, MouseInvertY). */
enum {
    ACT_A, ACT_B, ACT_X, ACT_Y, ACT_BLACK, ACT_WHITE, ACT_LT, ACT_RT, ACT_START, ACT_BACK,
    ACT_DUP, ACT_DDOWN, ACT_DLEFT, ACT_DRIGHT,
    ACT_MOVE_UP, ACT_MOVE_DOWN, ACT_MOVE_LEFT, ACT_MOVE_RIGHT,
    ACT_LOOK_UP, ACT_LOOK_DOWN, ACT_LOOK_LEFT, ACT_LOOK_RIGHT,
    ACT_LSTICK, ACT_RSTICK, ACT_COUNT
};
/* the [Keys] names (also the launcher's), and the defaults */
static const struct { const char *name, *def; } k_actions[ACT_COUNT] = {
    { "A", "Space" }, { "B", "Backspace, MouseMiddle" }, { "X", "E, MouseLeft" }, { "Y", "Q, MouseRight" },
    { "Black", "Z" }, { "White", "C" }, { "LeftTrigger", "LShift" }, { "RightTrigger", "F, RShift" },
    { "Start", "Enter" }, { "Back", "Escape" },
    { "DpadUp", "Up" }, { "DpadDown", "Down" }, { "DpadLeft", "Left, WheelDown" }, { "DpadRight", "Right, WheelUp" },
    { "MoveForward", "W" }, { "MoveBack", "S" }, { "MoveLeft", "A" }, { "MoveRight", "D" },
    { "LookUp", "I" }, { "LookDown", "K" }, { "LookLeft", "J" }, { "LookRight", "L" },
    { "LeftStickClick", "LCtrl" }, { "RightStickClick", "V" },
};
#define KEY_WHEEL_UP   0x1001
#define KEY_WHEEL_DOWN 0x1002
static const struct { const char *name; int vk; } k_key_names[] = {
    { "Space", VK_SPACE }, { "Enter", VK_RETURN }, { "Escape", VK_ESCAPE }, { "Backspace", VK_BACK }, { "Tab", VK_TAB },
    { "LShift", VK_LSHIFT }, { "RShift", VK_RSHIFT }, { "LCtrl", VK_LCONTROL }, { "RCtrl", VK_RCONTROL },
    { "LAlt", VK_LMENU }, { "RAlt", VK_RMENU }, { "Up", VK_UP }, { "Down", VK_DOWN }, { "Left", VK_LEFT }, { "Right", VK_RIGHT },
    { "Insert", VK_INSERT }, { "Delete", VK_DELETE }, { "Home", VK_HOME }, { "End", VK_END }, { "PageUp", VK_PRIOR },
    { "PageDown", VK_NEXT }, { "CapsLock", VK_CAPITAL },
    { "MouseLeft", VK_LBUTTON }, { "MouseRight", VK_RBUTTON }, { "MouseMiddle", VK_MBUTTON }, { "Mouse4", VK_XBUTTON1 },
    { "Mouse5", VK_XBUTTON2 }, { "WheelUp", KEY_WHEEL_UP }, { "WheelDown", KEY_WHEEL_DOWN },
    { "Num0", VK_NUMPAD0 }, { "Num1", VK_NUMPAD1 }, { "Num2", VK_NUMPAD2 }, { "Num3", VK_NUMPAD3 }, { "Num4", VK_NUMPAD4 },
    { "Num5", VK_NUMPAD5 }, { "Num6", VK_NUMPAD6 }, { "Num7", VK_NUMPAD7 }, { "Num8", VK_NUMPAD8 }, { "Num9", VK_NUMPAD9 },
    { "F1", VK_F1 }, { "F2", VK_F2 }, { "F3", VK_F3 }, { "F4", VK_F4 }, { "F5", VK_F5 }, { "F6", VK_F6 }, { "F7", VK_F7 },
    { "F8", VK_F8 }, { "F9", VK_F9 }, { "F10", VK_F10 }, { "F12", VK_F12 },
    { ";", VK_OEM_1 }, { "=", VK_OEM_PLUS }, { ",", VK_OEM_COMMA }, { "-", VK_OEM_MINUS }, { ".", VK_OEM_PERIOD },
    { "/", VK_OEM_2 }, { "`", VK_OEM_3 }, { "[", VK_OEM_4 }, { "\\", VK_OEM_5 }, { "]", VK_OEM_6 }, { "'", VK_OEM_7 },
};
static int   s_bind[ACT_COUNT][2];
static int   s_mouse_look = 1, s_mouse_invert_y;
static float s_mouse_sens = 1.0f;

static int key_code(const char *n)
{
    size_t i;
    if (n[0] && !n[1] && ((n[0] >= 'A' && n[0] <= 'Z') || (n[0] >= '0' && n[0] <= '9')))
        return n[0];
    if (n[0] && !n[1] && n[0] >= 'a' && n[0] <= 'z')
        return n[0] - 32;
    for (i = 0; i < sizeof k_key_names / sizeof k_key_names[0]; i++)
        if (!_stricmp(n, k_key_names[i].name))
            return k_key_names[i].vk;
    return 0;
}

static void parse_binding(const char *v, int *out)
{
    char buf[128], *tok, *ctx = NULL;
    int n = 0;
    out[0] = out[1] = 0;
    strncpy_s(buf, sizeof buf, v, _TRUNCATE);
    for (tok = strtok_s(buf, ",", &ctx); tok && n < 2; tok = strtok_s(NULL, ",", &ctx)) {
        while (*tok == ' ')
            tok++;
        {
            size_t l = strlen(tok);
            while (l && tok[l - 1] == ' ')
                tok[--l] = 0;
        }
        if ((out[n] = key_code(tok)) != 0)
            n++;
    }
}

const char *buffy_settings_path(void);

static void load_bindings(void)
{
    static int loaded;
    const char *ini = buffy_settings_path();
    int a;
    char v[128];
    if (loaded || !ini || !ini[0])
        return;
    loaded = 1;
    for (a = 0; a < ACT_COUNT; a++) {
        GetPrivateProfileStringA("Keys", k_actions[a].name, k_actions[a].def, v, sizeof v, ini);
        parse_binding(v, s_bind[a]);
    }
    s_mouse_look = GetPrivateProfileIntA("Controls", "MouseLook", 1, ini) != 0;
    s_mouse_invert_y = GetPrivateProfileIntA("Controls", "MouseInvertY", 0, ini) != 0;
    GetPrivateProfileStringA("Controls", "MouseSensitivity", "1.0", v, sizeof v, ini);
    s_mouse_sens = (float)atof(v);
    if (!(s_mouse_sens > 0.05f && s_mouse_sens < 20.0f))
        s_mouse_sens = 1.0f;
    if (getenv("BUFFY_MOUSE_LOG"))
        for (a = 0; a < ACT_COUNT; a++)
            fprintf(stderr, "[INPUT] key %s = %X %X\n", k_actions[a].name, s_bind[a][0], s_bind[a][1]);
}

/* The wheel this frame: notches up (+) / down (-), taken once a frame. */
int nv2a_gpu_take_wheel(void);
static int s_wheel_frame;

static int bound(int a, int captured)
{
    int k;
    for (k = 0; k < 2; k++) {
        int c = s_bind[a][k];
        if (!c)
            continue;
        if (c == KEY_WHEEL_UP ? s_wheel_frame > 0 : c == KEY_WHEEL_DOWN ? s_wheel_frame < 0 : 0)
            return 1;
        if (c == VK_LBUTTON || c == VK_RBUTTON || c == VK_MBUTTON || c == VK_XBUTTON1 || c == VK_XBUTTON2) {
            if (captured && key(c))              /* (mouse buttons only while playing) */
                return 1;
            continue;
        }
        if (c < 0x1000 && key(c))
            return 1;
    }
    return 0;
}

int  buffy_mouse_look_allowed(void);
void nv2a_gpu_mouse_capture(int on);
int  nv2a_gpu_mouse_captured(int *cx, int *cy);

static SHORT stick_max(SHORT a, int b)
{
    if (b > 32767) b = 32767;
    if (b < -32768) b = -32768;
    return (abs(b) > abs((int)a)) ? (SHORT)b : a;
}

static void apply_keyboard(XBOX_GAMEPAD *g)
{
    static DWORD last_poll;
    static int mdx, mdy;
    int cap = 0, cx, cy, focus = game_has_focus();
    load_bindings();
    /* the mouse: captured while playing; its movement once a frame */
    {
        /* (playing for half a second: not the moment a level is made) */
        static DWORD since;
        int ok = focus && s_mouse_look && buffy_mouse_look_allowed();
        if (!ok)
            since = 0;
        else if (!since)
            since = GetTickCount();
        nv2a_gpu_mouse_capture(ok && GetTickCount() - since >= 500);
    }
    if (GetTickCount() - last_poll >= 8) {
        last_poll = GetTickCount();
        s_wheel_frame = nv2a_gpu_take_wheel();
        mdx = mdy = 0;
        if (nv2a_gpu_mouse_captured(&cx, &cy)) {
            POINT p;
            if (GetCursorPos(&p)) {
                mdx = p.x - cx;
                mdy = p.y - cy;
                SetCursorPos(cx, cy);
            }
        }
    }
    cap = nv2a_gpu_mouse_captured(&cx, &cy);
    {
        /* (testing) BUFFY_TEST_MOUSE=secs:dx:holdsecs -- the mouse moving dx
         * a frame for a while, as if captured; BUFFY_MOUSE_LOG: when mouse
         * look would be on */
        static DWORD t0;
        const char *tm = getenv("BUFFY_TEST_MOUSE");
        if (!t0)
            t0 = GetTickCount();
        if (getenv("BUFFY_MOUSE_LOG")) {
            static int was = -1;
            int now = buffy_mouse_look_allowed();
            if (now != was)
                fprintf(stderr, "[INPUT] mouse look %s (t %.1f)\n", now ? "allowed" : "off", (GetTickCount() - t0) / 1000.0);
            was = now;
        }
        if (tm && buffy_mouse_look_allowed()) {
            double at = atof(tm), hold = 1;
            const char *q = strchr(tm, ':');
            int dx = q ? atoi(q + 1) : 0;
            if (q && (q = strchr(q + 1, ':')) != NULL)
                hold = atof(q + 1);
            if ((GetTickCount() - t0) / 1000.0 >= at && (GetTickCount() - t0) / 1000.0 < at + hold) {
                mdx = dx;
                mdy = 0;
                cap = 1;
                focus = 1;
            }
        }
    }
    if (!focus)
        return;
    if (bound(ACT_START, cap))      g->wButtons |= BTN_START;
    if (bound(ACT_BACK, cap))       g->wButtons |= BTN_BACK;
    if (bound(ACT_DUP, cap))        g->wButtons |= BTN_UP;
    if (bound(ACT_DDOWN, cap))      g->wButtons |= BTN_DOWN;
    if (bound(ACT_DLEFT, cap))      g->wButtons |= BTN_LEFT;
    if (bound(ACT_DRIGHT, cap))     g->wButtons |= BTN_RIGHT;
    if (bound(ACT_LSTICK, cap))     g->wButtons |= 0x0040;             /* left thumb */
    if (bound(ACT_RSTICK, cap))     g->wButtons |= 0x0080;             /* right thumb */
    if (bound(ACT_A, cap))          g->bAnalogButtons[0] = 0xFF;
    if (bound(ACT_B, cap))          g->bAnalogButtons[1] = 0xFF;
    if (bound(ACT_X, cap))          g->bAnalogButtons[2] = 0xFF;
    if (bound(ACT_Y, cap))          g->bAnalogButtons[3] = 0xFF;
    if (bound(ACT_BLACK, cap))      g->bAnalogButtons[4] = 0xFF;
    if (bound(ACT_WHITE, cap))      g->bAnalogButtons[5] = 0xFF;
    if (bound(ACT_LT, cap))         g->bAnalogButtons[6] = 0xFF;
    if (bound(ACT_RT, cap))         g->bAnalogButtons[7] = 0xFF;
    if (bound(ACT_MOVE_UP, cap))    g->sThumbLY = 32767;
    if (bound(ACT_MOVE_DOWN, cap))  g->sThumbLY = -32768;
    if (bound(ACT_MOVE_LEFT, cap))  g->sThumbLX = -32768;
    if (bound(ACT_MOVE_RIGHT, cap)) g->sThumbLX = 32767;
    if (bound(ACT_LOOK_UP, cap))    g->sThumbRY = 32767;
    if (bound(ACT_LOOK_DOWN, cap))  g->sThumbRY = -32768;
    if (bound(ACT_LOOK_LEFT, cap))  g->sThumbRX = -32768;
    if (bound(ACT_LOOK_RIGHT, cap)) g->sThumbRX = 32767;
    if (cap && (mdx || mdy)) {
        /* the camera turns as fast as the mouse moves: a stick tilt for this
         * frame's movement, past the game's stick dead zone */
        float k = 700.0f * s_mouse_sens;
        int rx = (int)(mdx * k), ry = (int)(-mdy * k) * (s_mouse_invert_y ? -1 : 1);
        if (rx) rx += rx > 0 ? 6000 : -6000;
        if (ry) ry += ry > 0 ? 6000 : -6000;
        g->sThumbRX = stick_max(g->sThumbRX, rx);
        g->sThumbRY = stick_max(g->sThumbRY, ry);
    }
}

/* Radial deadzone with rescale, so a resting stick reads zero and full tilt
 * still reaches full range. */
static void stick(SHORT x, SHORT y, float dz, SHORT *ox, SHORT *oy)
{
    float fx = x, fy = y, m = sqrtf(fx * fx + fy * fy), k;
    if (m <= dz) { *ox = *oy = 0; return; }
    k = (m - dz) / (32767.0f - dz);
    if (k > 1.0f) k = 1.0f;
    k = k * 32767.0f / m;
    fx *= k; fy *= k;
    *ox = (SHORT)(fx > 32767.0f ? 32767 : (fx < -32768.0f ? -32768 : fx));
    *oy = (SHORT)(fy > 32767.0f ? 32767 : (fy < -32768.0f ? -32768 : fy));
}

/* The PC controller behind an Xbox port, if any. */
static void pc_pad(int port, XBOX_GAMEPAD *g)
{
    XINPUT_STATE st;
    WORD b;
    int slot;

    scan_pads(0);
    slot = s_slot[port];
    if (slot < 0)
        return;
    if (XInputGetState((DWORD)slot, &st) != ERROR_SUCCESS) {
        scan_pads(1);                           /* unplugged: renumber */
        return;
    }
    b = st.Gamepad.wButtons;
    g->wButtons |= b & 0x00FF;                /* d-pad, start, back, thumbs: same bits */
    if (b & XINPUT_GAMEPAD_A)              g->bAnalogButtons[0] = 0xFF;
    if (b & XINPUT_GAMEPAD_B)              g->bAnalogButtons[1] = 0xFF;
    if (b & XINPUT_GAMEPAD_X)              g->bAnalogButtons[2] = 0xFF;
    if (b & XINPUT_GAMEPAD_Y)              g->bAnalogButtons[3] = 0xFF;
    if (b & XINPUT_GAMEPAD_LEFT_SHOULDER)  g->bAnalogButtons[4] = 0xFF;   /* Black */
    if (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) g->bAnalogButtons[5] = 0xFF;   /* White */
    if (st.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
        g->bAnalogButtons[6] = st.Gamepad.bLeftTrigger;
    if (st.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
        g->bAnalogButtons[7] = st.Gamepad.bRightTrigger;
    stick(st.Gamepad.sThumbLX, st.Gamepad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, &g->sThumbLX, &g->sThumbLY);
    stick(st.Gamepad.sThumbRX, st.Gamepad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE, &g->sThumbRX, &g->sThumbRY);
}

/* What controller `port` held when the game last read it, in XInput's bits
 * (Start 0x10, Back 0x20, A 0x1000, B 0x2000; the d-pad in the low four);
 * 0 when it is not connected or the game has not read it. */
uint16_t buffy_input_buttons(int port)
{
    XBOX_GAMEPAD g;
    if (port < 0 || port >= MAX_PORTS || !(connected_mask() >> port & 1))
        return 0;
    memset(&g, 0, sizeof g);                    /* read now: the game may not poll this pad */
    pc_pad(port, &g);
    if (port == 0)
        apply_keyboard(&g);
    apply_script(port, &g);
    return (uint16_t)(g.wButtons | (g.bAnalogButtons[0] > 0x40 ? 0x1000 : 0)
                      | (g.bAnalogButtons[1] > 0x40 ? 0x2000 : 0)
                      | (g.bAnalogButtons[2] > 0x40 ? 0x4000 : 0)
                      | (g.bAnalogButtons[3] > 0x40 ? 0x8000 : 0));
}

int buffy_coop_owns_back(void);
int buffy_coop_owns_x(int port);
int buffy_settings_invert_camera_x(void);

/* DWORD XInputGetState(HANDLE, PXINPUT_STATE) -- 0x0018FAFA */
void XInputGetState_0018FAFA(void)
{
    static DWORD packet[MAX_PORTS];
    static XBOX_GAMEPAD last[MAX_PORTS];
    int port = handle_port(MEM32(g_esp + 4));
    uint32_t out = MEM32(g_esp + 8);
    XBOX_GAMEPAD g;

    if (port < 0 || !out || !(connected_mask() >> port & 1)) {
        ret_stdcall(ERROR_DEVICE_NOT_CONNECTED, 8);
        return;
    }
    memset(&g, 0, sizeof g);
    pc_pad(port, &g);
    if (port == 0)
        apply_keyboard(&g);
    apply_script(port, &g);
    if (buffy_settings_invert_camera_x())      /* launcher: invert camera left / right */
        g.sThumbRX = g.sThumbRX == -32768 ? 32767 : (SHORT)-g.sThumbRX;
    if (port == 1 && buffy_coop_owns_back())
        g.wButtons &= (WORD)~BTN_BACK;              /* story co-op: Back brings player 2 to player 1 */
    if (buffy_coop_owns_x(port))
        g.bAnalogButtons[2] = 0;                    /* the in-level Character Select: X changes the outfit */

    if (memcmp(&g, &last[port], sizeof g)) {
        packet[port]++;
        last[port] = g;
    }
    /* XINPUT_STATE: DWORD packet, then XINPUT_GAMEPAD (18 bytes). */
    MEM32(out) = packet[port];
    memcpy((void *)XBOX_PTR(out + 4), &g, 18);
    ret_stdcall(0, 8);
}
