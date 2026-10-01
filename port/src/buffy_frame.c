/**
 * Frame pacing on the game thread, where the Xbox paces it.
 *
 * On the console D3DDevice_Swap is tied to vertical blank, so the game loop
 * runs one iteration per 1/60 s and every system that measures time --
 * animation blending, combo input windows, AI -- sees an even step.
 *
 * The port first paced at the GPU's flip instead. The recompiled game thread
 * is far faster than the Xbox CPU, so it raced ahead until D3D's pushbuffer
 * throttling stopped it, then was released in bursts: frames reached the game
 * 3 ms, 29 ms, 18 ms apart (measured at Swap) while the screen showed a
 * steady 60. That uneven step made animations jitter and dropped combo
 * inputs. Waiting here, before each Swap, gives the game an even 60 Hz.
 *
 * BUFFY_UNCAPPED=1 turns it off.
 */
#include <windows.h>
#include <timeapi.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#include "recomp/gen/recomp_types.h"

#pragma comment(lib, "winmm.lib")

void D3DDevice_Swap_0013A070_orig(void);
void buffy_settings_frame(void);
void buffy_mods_frame(void);
void xbox_HangMonitorBeat(void);

static void pace_60hz(void)
{
    static int on = -1;
    static LARGE_INTEGER freq, next;
    LARGE_INTEGER now;
    LONGLONG period;

    if (on < 0) {
        on = !(getenv("BUFFY_UNCAPPED") && getenv("BUFFY_UNCAPPED")[0] == '1');
        QueryPerformanceFrequency(&freq);
        timeBeginPeriod(1);
    }
    {
        void buffy_profile_frame(void);
        buffy_profile_frame();                         /* (testing) BUFFY_PROFILE: buffy_profile.c */
    }
    {
        /* (testing) BUFFY_FPS_LOG=1: frames per second, every 5 s */
        static int log = -1;
        static LARGE_INTEGER t0;
        static int frames;
        if (log < 0)
            log = getenv("BUFFY_FPS_LOG") != NULL;
        if (log) {
            LARGE_INTEGER t;
            QueryPerformanceCounter(&t);
            if (!t0.QuadPart)
                t0 = t;
            frames++;
            if (t.QuadPart - t0.QuadPart >= freq.QuadPart * 5) {
                /* and the game thread's own work: CPU cycles a frame (other
                 * programs' load does not change it; the pacing wait is not in it) */
                static ULONG64 c0;
                ULONG64 c = 0;
                QueryThreadCycleTime(GetCurrentThread(), &c);
                fprintf(stderr, "[FPS] %.1f  (game thread %.2f Mcycles/frame)\n",
                        frames * (double)freq.QuadPart / (double)(t.QuadPart - t0.QuadPart),
                        c0 && frames ? (double)(c - c0) / frames / 1e6 : 0.0);
                c0 = c;
                frames = 0;
                t0 = t;
            }
        }
    }
    if (!on)
        return;
    {
        int buffy_settings_fps_limit(void);
        period = freq.QuadPart / buffy_settings_fps_limit();
    }
    QueryPerformanceCounter(&now);
    if (!next.QuadPart || now.QuadPart >= next.QuadPart) {
        /* On time or late: no wait, and no catching up afterwards either --
         * a burst of short frames is exactly what this exists to prevent. */
        next.QuadPart = now.QuadPart + period;
        return;
    }
    for (;;) {
        LONGLONG left;
        QueryPerformanceCounter(&now);
        left = next.QuadPart - now.QuadPart;
        if (left <= 0)
            break;
        if (left * 1000 / freq.QuadPart >= 2)
            Sleep((DWORD)(left * 1000 / freq.QuadPart) - 1);
        else
            SwitchToThread();
    }
    next.QuadPart += period;
}

/* The engine's frame rate (EngineX's EXBaseApp statics).
 *
 * Each frame EXApp::MainUpdate picks a rate -- m_MaxFrameRate, or less when
 * the frame's CPU work (its own benchmark timer, measured against a
 * 1/m_MaxFrameRate frame) would not fit, but not below m_MinFrameRate -- and
 * UpdateFrameRate sets m_FrameLength = 1 / rate and the multipliers the game
 * and the engine scale every per-frame step by: nominal (60) / rate. It is
 * how the PAL (50 Hz) build ran at the right speed. The console's maximum
 * was 60; here it is the frame rate the game actually runs at: the FPS
 * limit, or less when frames take longer (vsync on a slower display, the
 * GPU) -- which the engine cannot see, as its timer only measures its own
 * work. So 120 frames a second are 120 half steps, and the game keeps its
 * own speed at any rate (at a 30 limit it no longer runs at half speed).
 * BUFFY_FRAME_RATE_LOG=1 prints the rate each second (testing). */
#define EXBASEAPP_MAX_FRAME_RATE   0x1B98ACu    /* m_MaxFrameRate (60) */
#define EXBASEAPP_MIN_FRAME_RATE   0x1B98A8u    /* m_MinFrameRate (30) */

/* What still counts frames rather than time: the game's frame count
 * (EXBaseStats, read for "every 10 / 20 frames" work -- plant growth, combat
 * music, thrown characters) and the monsters' per-frame counters
 * (TickCounters: attack and stun timers in frames, a boss's 300-frame
 * phases). Both stay on a 60 Hz beat: each frame owes 60 / rate ticks, and
 * they run once per whole tick -- every other frame at 120, as at 60 every
 * frame. At 30 the counters run twice a frame; the frame count still moves
 * once, so the "every N frames" work divides it as the console's did. */
static double s_tick_acc;
static int s_ticks60 = 1;

void buffy_frame_rate_apply(void)
{
    int buffy_settings_fps_limit(void);
    static LARGE_INTEGER freq, last;
    static double avg_ms;                          /* recent frame time (wall clock) */
    LARGE_INTEGER now;
    int limit = buffy_settings_fps_limit(), fps;
    uint32_t min = MEM32(EXBASEAPP_MIN_FRAME_RATE);

    if (getenv("BUFFY_UNCAPPED") && getenv("BUFFY_UNCAPPED")[0] == '1')
        limit = 360;                               /* (testing) no pacing: the measured rate */
    fps = limit;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    if (last.QuadPart) {
        double ms = (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)freq.QuadPart;
        if (ms > 1000.0 / 20)
            ms = 1000.0 / 20;                      /* a load stall: not a frame rate */
        avg_ms = avg_ms ? avg_ms + (ms - avg_ms) * 0.125 : ms;
        /* below the limit by more than 5%: the rate frames really come at */
        if (avg_ms > 1000.0 / limit * 1.05)
            fps = (int)(1000.0 / avg_ms + 0.5);
    }
    last = now;
    if (min && (uint32_t)fps < min)
        fps = (int)min;
    MEM32(EXBASEAPP_MAX_FRAME_RATE) = (uint32_t)fps;
    /* 60 Hz ticks owed by this frame (below) */
    s_tick_acc += 60.0 / fps;
    s_ticks60 = (int)s_tick_acc;
    s_tick_acc -= s_ticks60;
    if (s_ticks60 > 2) {
        s_ticks60 = 2;
        s_tick_acc = 0;
    }
    if (getenv("BUFFY_FRAME_RATE_LOG")) {
        static DWORD t;
        if (GetTickCount() - t >= 1000) {
            t = GetTickCount();
            fprintf(stderr, "[RATE] limit %d, engine %d (frame %.2f ms, step x%.3f)\n", limit, fps, avg_ms,
                    MEMF(0x1B98C0));
        }
    }
}

/* void EXBaseStats::FrameUpdate(int) -- 0x000D5C50 (thiscall, ret 4): the
 * frame count, once per 60 Hz tick. */
void EXBaseStats_FrameUpdate_000D5C50_orig(void);
void EXBaseStats_FrameUpdate_000D5C50(void)
{
    if (s_ticks60 <= 0) {
        g_esp += 8;                                     /* ret 4 */
        return;
    }
    EXBaseStats_FrameUpdate_000D5C50_orig();
}

/* void XItemHandler_Monster::TickCounters() -- 0x000638E0, and Kakistos's
 * override 0x0001A6E0 (which calls it first): thiscall, ret. Run once per
 * 60 Hz tick this frame owes (0, 1 or 2 times). */
static int s_in_tick;

static void tick_counters(void (*orig)(void))
{
    uint32_t self = g_ecx, ret = MEM32(g_esp);
    int i;
    if (s_in_tick) {                                    /* Kakistos -> Monster: already counted */
        orig();
        return;
    }
    if (s_ticks60 <= 0) {
        g_esp += 4;                                     /* ret */
        return;
    }
    s_in_tick = 1;
    for (i = 0; i < s_ticks60; i++) {
        if (i) {
            g_esp -= 4;
            MEM32(g_esp) = ret;                         /* the return address again */
        }
        g_ecx = self;
        orig();
    }
    s_in_tick = 0;
}

void XItemHandler_Monster_TickCounters_000638E0_orig(void);
void XItemHandler_Monster_TickCounters_000638E0(void)
{
    tick_counters(XItemHandler_Monster_TickCounters_000638E0_orig);
}

void XItemHandler_Boss_Kakistos_TickCounters_0001A6E0_orig(void);
void XItemHandler_Boss_Kakistos_TickCounters_0001A6E0(void)
{
    tick_counters(XItemHandler_Boss_Kakistos_TickCounters_0001A6E0_orig);
}

/* void D3DDevice_Swap(DWORD flags) -- 0x0013A070, wrapped. */
void buffy_frame_note_swap(void);

int  buffy_coop_in_pass2(void);
int  buffy_coop_decide(void);
void nv2a_gpu_queue_flip(int target);

void D3DDevice_Swap_0013A070(void)
{
    /* Two-screen co-op (buffy_mods.c): player 2's frame goes to window 2,
     * unpaced -- it belongs to the same game frame as player 1's. */
    if (buffy_coop_in_pass2()) {
        void buffy_native_present(void);
        nv2a_gpu_queue_flip(1);
        buffy_native_present();             /* native mode: player 2's view to its window / half */
        D3DDevice_Swap_0013A070_orig();
        return;
    }
    {
        int split = buffy_coop_decide();
        if (split >= 0)
            nv2a_gpu_queue_flip(split ? 0 : 2);     /* player 1 only, or both */
    }
    buffy_frame_note_swap();
    xbox_HangMonitorBeat();                 /* a frozen game thread gets logged (xbox_memory_layout.c) */
    if (getenv("RECOMP_VP_STATS")) {
        static DWORD t0;
        static unsigned swaps;
        swaps++;
        if (GetTickCount() - t0 >= 1000) {
            fprintf(stderr, "[FRAME] %u swaps/s\n", swaps);
            swaps = 0;
            t0 = GetTickCount();
        }
    }
    {
        void buffy_native_frame(void), buffy_native_check(void);
        int buffy_native_mode(void);
        static int checked;
        if (!checked++) {
            buffy_native_mode();
            buffy_native_check();
        }
        buffy_native_frame();               /* the native renderer (buffy_native.c) */
    }
    {
        void buffy_native_present(void);
        buffy_native_present();             /* native mode: its frame to the window */
    }
    {
        /* BUFFY_TEST_STALL=<frame>: sleep 20 s there, to try the hang report */
        static int frame;
        const char *st = getenv("BUFFY_TEST_STALL");
        if (st && ++frame == atoi(st))
            Sleep(20000);
    }
    buffy_settings_frame();                 /* widescreen flags, 16:9 or 4:3 frame */
    buffy_mods_frame();
    pace_60hz();
    D3DDevice_Swap_0013A070_orig();
    {
        /* the next frame's work starts now (frame interpolation, buffy_native.c) */
        extern LARGE_INTEGER g_frame_begin;
        QueryPerformanceCounter(&g_frame_begin);
    }
}

/* ── stall sampler (BUFFY_STALLS=1) ─────────────────────────────────────
 * When the game thread goes 150 ms without reaching Swap, sample where it is
 * (host function names through the PDB) every 25 ms until it returns. */
#include <dbghelp.h>
#include <stdio.h>
#pragma comment(lib, "dbghelp.lib")

static HANDLE s_game_thread;
static volatile LONGLONG s_last_swap;

static DWORD WINAPI stall_monitor(LPVOID unused)
{
    LARGE_INTEGER f, now;
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    (void)unused;
    QueryPerformanceFrequency(&f);
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    for (;;) {
        Sleep(25);
        QueryPerformanceCounter(&now);
        if (s_last_swap && (now.QuadPart - s_last_swap) * 1000 / f.QuadPart > 150) {
            CONTEXT c;
            DWORD64 disp = 0;
            memset(&c, 0, sizeof c);
            c.ContextFlags = CONTEXT_CONTROL;
            if (SuspendThread(s_game_thread) != (DWORD)-1) {
                if (GetThreadContext(s_game_thread, &c)) {
                    memset(buf, 0, sizeof buf);
                    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                    sym->MaxNameLen = 255;
                    if (SymFromAddr(GetCurrentProcess(), c.Rip, &disp, sym))
                        fprintf(stderr, "  [STALL] %lld ms: %s+0x%llX\n",
                                (now.QuadPart - s_last_swap) * 1000 / f.QuadPart, sym->Name,
                                (unsigned long long)disp);
                }
                ResumeThread(s_game_thread);
            }
        }
    }
}

void buffy_frame_note_swap(void)
{
    LARGE_INTEGER now;
    {
        static int mem;
        void buffy_memstats_start(void);
        if (!mem++) {
            void buffy_preload_start(void);
            buffy_memstats_start();
            buffy_preload_start();          /* (the disc's archives into the file cache) */
        }
    }
    if (!s_game_thread && getenv("BUFFY_STALLS")) {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                        &s_game_thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
        CloseHandle(CreateThread(NULL, 0, stall_monitor, NULL, 0, NULL));
    }
    QueryPerformanceCounter(&now);
    s_last_swap = now.QuadPart;
}

/* ── memory sampler (BUFFY_MEMSTATS=1) ──────────────────────────────────
 * Every 10 s: private bytes, working set and peak, and the GPU's local
 * memory in use, to catch anything that grows without bound. */
#include <psapi.h>
#define COBJMACROS
#include <dxgi1_4.h>
#pragma comment(lib, "psapi.lib")

/* Per-thread CPU over the last interval, by start function (BUFFY_MEMSTATS=2). */
#include <tlhelp32.h>
#include <winternl.h>
static void thread_report(void)
{
    static struct { DWORD id; ULONGLONG t; } prev[64];
    static int nprev;
    static ULONGLONG last_wall;
    typedef LONG (NTAPI *QIT)(HANDLE, int, PVOID, ULONG, PULONG);
    static QIT qit;
    ULONGLONG wall = GetTickCount64();
    HANDLE snap;
    THREADENTRY32 te;
    int n = 0;
    struct { DWORD id; ULONGLONG t; } cur[64];

    if (!getenv("BUFFY_MEMSTATS") || getenv("BUFFY_MEMSTATS")[0] != '2')
        return;
    if (!qit)
        qit = (QIT)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok && n < 64; ok = Thread32Next(snap, &te)) {
        HANDLE th;
        FILETIME c, e, k, u;
        ULONGLONG t, d = 0;
        void *start = NULL;
        int i;
        if (te.th32OwnerProcessID != GetCurrentProcessId())
            continue;
        th = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!th)
            continue;
        GetThreadTimes(th, &c, &e, &k, &u);
        if (qit)
            qit(th, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof start, NULL);
        CloseHandle(th);
        t = ((((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime)
           + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime)) / 10000;
        for (i = 0; i < nprev; i++)
            if (prev[i].id == te.th32ThreadID)
                d = t - prev[i].t;
        cur[n].id = te.th32ThreadID;
        cur[n].t = t;
        n++;
        if (last_wall && d * 100 >= (wall - last_wall) * 3) {       /* >= 3% */
            char buf[sizeof(SYMBOL_INFO) + 128];
            SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
            DWORD64 disp = 0;
            memset(buf, 0, sizeof buf);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 127;
            if (!SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)start, &disp, sym))
                strcpy_s(sym->Name, 128, "?");
            fprintf(stderr, "[MEM]   thread %5lu %-40s %3.0f%%\n", te.th32ThreadID, sym->Name,
                    100.0 * (double)d / (double)(wall - last_wall));
        }
    }
    CloseHandle(snap);
    memcpy(prev, cur, sizeof(cur[0]) * (size_t)n);
    nprev = n;
    last_wall = wall;
}

static DWORD WINAPI mem_monitor(LPVOID unused)
{
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    IDXGIFactory4 *fac = NULL;
    IDXGIAdapter3 *ad = NULL;
    (void)unused;
    if (SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory4, (void **)&fac))) {
        IDXGIAdapter1 *a1 = NULL;
        if (SUCCEEDED(IDXGIFactory4_EnumAdapters1(fac, 0, &a1))) {
            IDXGIAdapter1_QueryInterface(a1, &IID_IDXGIAdapter3, (void **)&ad);
            IDXGIAdapter1_Release(a1);
        }
    }
    Sleep(60000);
    {
        /* Once, a minute in: the large committed private regions. */
        MEMORY_BASIC_INFORMATION mbi;
        uint8_t *p = NULL;
        while (VirtualQuery(p, &mbi, sizeof mbi) == sizeof mbi) {
            if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.RegionSize >= (4u << 20))
                fprintf(stderr, "[MEM] region %p: %zu MB committed, alloc base %p\n", mbi.BaseAddress,
                        mbi.RegionSize >> 20, mbi.AllocationBase);
            p = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        }
    }
    for (;;) {
        PROCESS_MEMORY_COUNTERS_EX pm;
        DXGI_QUERY_VIDEO_MEMORY_INFO vm;
        memset(&vm, 0, sizeof vm);
        Sleep(10000);
        pm.cb = sizeof pm;
        GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof pm);
        if (ad)
            IDXGIAdapter3_QueryVideoMemoryInfo(ad, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vm);
        {
            static ULONGLONG last_cpu, last_wall;
            FILETIME c, e, k, u;
            ULONGLONG cpu, wall = GetTickCount64();
            GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
            cpu = ((((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime)
                 + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime)) / 10000;   /* ms */
            fprintf(stderr, "[MEM] private %zu MB, working set %zu MB (peak %zu MB), gpu %llu MB, cpu %.0f%% of a core\n",
                    pm.PrivateUsage >> 20, pm.WorkingSetSize >> 20, pm.PeakWorkingSetSize >> 20,
                    (unsigned long long)(vm.CurrentUsage >> 20),
                    last_wall ? 100.0 * (double)(cpu - last_cpu) / (double)(wall - last_wall) : 0.0);
            last_cpu = cpu;
            last_wall = wall;
        }
        thread_report();
    }
}

void buffy_memstats_start(void)
{
    if (getenv("BUFFY_MEMSTATS"))
        CloseHandle(CreateThread(NULL, 0, mem_monitor, NULL, 0, NULL));
}
