/**
 * Bug reports: click the left stick (any controller) or press F12, and the
 * game saves what is happening right now to bug_reports\<date>_<time>\ beside
 * the exe:
 *
 *   screen.png      the frame on screen (screen_p2.png: player 2's window)
 *   report.txt      the game's state -- level, players, co-op, settings,
 *                   mods -- and where the game thread is, as the hang
 *                   monitor reports it (xbox_WatchdogReportTo)
 *   log.txt         buffy_log.txt so far
 *
 * The buttons are watched from a thread of our own, so a report can be made
 * while the game is frozen (the screenshot then does not come: the render
 * thread saves it at the next flip). A beep says it was saved, and the title
 * bar says so for a few seconds. The click still reaches the game.
 *
 * buffy_settings.ini [Debug] ReportButton: 1 left stick + F12 (default),
 * 2 F12 only, 0 off.
 */
#include <windows.h>
#include <xinput.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "recomp/gen/recomp_types.h"
#include "buffy_settings.h"

#pragma comment(lib, "xinput.lib")

void nv2a_gpu_request_snapshot(const wchar_t *dir);
void nv2a_gpu_title_note(const char *text, unsigned ms);
void xbox_WatchdogReportTo(void *out, const char *why);
long xbox_HangMonitorFrames(void);
unsigned long xbox_HangMonitorIdleMs(void);
void buffy_mods_debug_report(FILE *f);

#define G_SCENE_HASH     0x1B7FACu        /* the scene being played */
#define G_FRONTEND_HASH  0x1B7F68u        /* the front end's file (menus) */
#define G_PLAYERS        0x26DC54u        /* player items, slots 0..3 */

static int s_mode = 1;

static float fget(uint32_t va) { uint32_t u = MEM32(va); float f; memcpy(&f, &u, 4); return f; }

static void write_report(const wchar_t *dir, const char *trigger)
{
    wchar_t path[MAX_PATH], exe[MAX_PATH], *slash;
    FILE *f;
    SYSTEMTIME t;
    int i;

    swprintf_s(path, MAX_PATH, L"%s\\report.txt", dir);
    if (_wfopen_s(&f, path, L"w") || !f)
        return;
    GetLocalTime(&t);
    fprintf(f, "Buffy the Vampire Slayer: Chaos Bleeds -- bug report\n");
    fprintf(f, "Saved %04u-%02u-%02u %02u:%02u:%02u (%s)\n\n", t.wYear, t.wMonth, t.wDay,
            t.wHour, t.wMinute, t.wSecond, trigger);

    fprintf(f, "== Game\n");
    fprintf(f, "frames drawn: %ld, last frame %lu ms ago%s\n", xbox_HangMonitorFrames(),
            xbox_HangMonitorIdleMs(), xbox_HangMonitorIdleMs() > 3000 ? "  <-- the game thread is not moving" : "");
    fprintf(f, "scene %08X, front end file %08X (%s)\n", MEM32(G_SCENE_HASH), MEM32(G_FRONTEND_HASH),
            MEM32(G_SCENE_HASH) == MEM32(G_FRONTEND_HASH) ? "menus" : "in a level");
    for (i = 0; i < 4; i++) {
        uint32_t it = MEM32(G_PLAYERS + i * 4);
        if (!it)
            continue;
        fprintf(f, "player %d: item %08X  position %.2f %.2f %.2f  facing %.2f  flags %08X\n", i + 1, it,
                fget(it + 0xAC), fget(it + 0xB0), fget(it + 0xB4), fget(it + 0xBC), MEM32(it + 0x10));
    }

    {
        const char *buffy_platform_text(void);
        fprintf(f, "\n== System\n%s\n", buffy_platform_text());
    }

    fprintf(f, "\n== Settings\n");
    fprintf(f, "resolution %dx%d%s, vsync %s, %s, widescreen view %s, invert camera x %s\n",
            buffy_settings_res_width(), buffy_settings_res_height(),
            buffy_settings_widescreen() ? " (16:9)" : " (4:3)", buffy_settings_vsync() ? "on" : "off",
            buffy_settings_display_mode_name(buffy_settings_fullscreen()),
            buffy_settings_widescreen_wide() ? "wide" : "original", buffy_settings_invert_camera_x() ? "on" : "off");
    {
        const char *ini = buffy_settings_path();
        if (ini && *ini)
            fprintf(f, "texture packs: load %d, prefetch %d, dump %d\n",
                    GetPrivateProfileIntA("Textures", "Load", 0, ini), GetPrivateProfileIntA("Textures", "Prefetch", 0, ini),
                    GetPrivateProfileIntA("Textures", "Dump", 0, ini));
    }

    fprintf(f, "\n== Mods and co-op\n");
    buffy_mods_debug_report(f);

    fprintf(f, "\n== Where the game is (game thread, every thread, guest stack)\n");
    fflush(f);
    xbox_WatchdogReportTo(f, "bug report");
    fclose(f);

    /* The log so far. */
    fflush(stderr);
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    slash = wcsrchr(exe, L'\\');
    if (slash) {
        wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - exe), L"buffy_log.txt");
        swprintf_s(path, MAX_PATH, L"%s\\log.txt", dir);
        CopyFileW(exe, path, FALSE);
    }
}

static void make_report(const char *trigger)
{
    wchar_t dir[MAX_PATH], *slash;
    SYSTEMTIME t;
    GetModuleFileNameW(NULL, dir, MAX_PATH);
    slash = wcsrchr(dir, L'\\');
    if (!slash)
        return;
    GetLocalTime(&t);
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - dir), L"bug_reports");
    CreateDirectoryW(dir, NULL);
    swprintf_s(slash + 1, MAX_PATH - (slash + 1 - dir), L"bug_reports\\%04u-%02u-%02u_%02u-%02u-%02u",
               t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    CreateDirectoryW(dir, NULL);
    nv2a_gpu_request_snapshot(dir);        /* first: the frame on screen as the button went down */
    write_report(dir, trigger);
    fwprintf(stderr, L"[DEBUG] bug report saved: %s\n", dir);
    nv2a_gpu_title_note("Bug report saved", 4000);
    MessageBeep(MB_ICONASTERISK);
}

/* Is one of our windows in front (so F12 is meant for us)? */
static int ours_in_front(void)
{
    DWORD pid = 0;
    HWND w = GetForegroundWindow();
    if (!w)
        return 0;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

static DWORD WINAPI debug_thread(LPVOID arg)
{
    WORD prev[4] = { 0 };
    int f12_prev = 0;
    DWORD last = 0, scan = 0;
    int live[4] = { 1, 1, 1, 1 };
    (void)arg;
    for (;;) {
        int p, fire = 0, f12;
        const char *why = NULL;
        Sleep(30);
        if (GetTickCount() - scan > 2000) {            /* empty slots are slow to ask: recheck now and then */
            scan = GetTickCount();
            for (p = 0; p < 4; p++)
                live[p] = 1;
        }
        if (s_mode == 1) {
            for (p = 0; p < 4; p++) {
                XINPUT_STATE st;
                if (!live[p])
                    continue;
                if (XInputGetState((DWORD)p, &st) != ERROR_SUCCESS) {
                    live[p] = 0;
                    prev[p] = 0;
                    continue;
                }
                if ((st.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) && !(prev[p] & XINPUT_GAMEPAD_LEFT_THUMB)) {
                    fire = 1;
                    why = p == 0 ? "left stick, controller 1" : p == 1 ? "left stick, controller 2"
                        : p == 2 ? "left stick, controller 3" : "left stick, controller 4";
                }
                prev[p] = st.Gamepad.wButtons;
            }
        }
        f12 = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
        if (f12 && !f12_prev && ours_in_front()) {
            fire = 1;
            why = "F12";
        }
        f12_prev = f12;
        if (fire && GetTickCount() - last > 1500) {
            last = GetTickCount();
            make_report(why);
        }
    }
}

/* Test runs: BUFFY_TEST_REPORT_AT=<seconds> makes a report then. */
static DWORD WINAPI test_thread(LPVOID arg)
{
    (void)arg;
    Sleep((DWORD)atoi(getenv("BUFFY_TEST_REPORT_AT")) * 1000u);
    make_report("test");
    return 0;
}

/* Once, at start-up (main.c). */
void buffy_debug_start(void)
{
    const char *ini = buffy_settings_path();
    HANDLE h;
    if (ini && *ini)
        s_mode = (int)GetPrivateProfileIntA("Debug", "ReportButton", 1, ini);
    if (getenv("BUFFY_REPORT_BUTTON"))
        s_mode = atoi(getenv("BUFFY_REPORT_BUTTON"));
    if (!s_mode)
        return;
    h = CreateThread(NULL, 0, debug_thread, NULL, 0, NULL);
    if (h)
        CloseHandle(h);
    if (getenv("BUFFY_TEST_REPORT_AT")) {
        h = CreateThread(NULL, 0, test_thread, NULL, 0, NULL);
        if (h)
            CloseHandle(h);
    }
}
