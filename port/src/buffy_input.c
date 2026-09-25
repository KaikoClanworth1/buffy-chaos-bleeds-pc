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

static void apply_script(int port, XBOX_GAMEPAD *g)
{
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

static void apply_keyboard(XBOX_GAMEPAD *g)
{
    if (!game_has_focus())
        return;
    if (key(VK_RETURN))                 g->wButtons |= BTN_START;
    if (key(VK_ESCAPE))                 g->wButtons |= BTN_BACK;
    if (key(VK_UP))                     g->wButtons |= BTN_UP;
    if (key(VK_DOWN))                   g->wButtons |= BTN_DOWN;
    if (key(VK_LEFT))                   g->wButtons |= BTN_LEFT;
    if (key(VK_RIGHT))                  g->wButtons |= BTN_RIGHT;
    if (key(VK_SPACE))                  g->bAnalogButtons[0] = 0xFF;   /* A */
    if (key(VK_BACK))                   g->bAnalogButtons[1] = 0xFF;   /* B */
    if (key('E'))                       g->bAnalogButtons[2] = 0xFF;   /* X */
    if (key('Q'))                       g->bAnalogButtons[3] = 0xFF;   /* Y */
    if (key('Z'))                       g->bAnalogButtons[4] = 0xFF;   /* Black */
    if (key('C'))                       g->bAnalogButtons[5] = 0xFF;   /* White */
    if (key(VK_LSHIFT))                 g->bAnalogButtons[6] = 0xFF;   /* L trigger */
    if (key(VK_RSHIFT) || key('F'))     g->bAnalogButtons[7] = 0xFF;   /* R trigger */
    if (key('W')) g->sThumbLY = 32767;
    if (key('S')) g->sThumbLY = -32768;
    if (key('A')) g->sThumbLX = -32768;
    if (key('D')) g->sThumbLX = 32767;
    if (key('I')) g->sThumbRY = 32767;
    if (key('K')) g->sThumbRY = -32768;
    if (key('J')) g->sThumbRX = -32768;
    if (key('L')) g->sThumbRX = 32767;
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
                      | (g.bAnalogButtons[1] > 0x40 ? 0x2000 : 0));
}

int buffy_coop_owns_back(void);
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

    if (memcmp(&g, &last[port], sizeof g)) {
        packet[port]++;
        last[port] = g;
    }
    /* XINPUT_STATE: DWORD packet, then XINPUT_GAMEPAD (18 bytes). */
    MEM32(out) = packet[port];
    memcpy((void *)XBOX_PTR(out + 4), &g, 18);
    ret_stdcall(0, 8);
}
