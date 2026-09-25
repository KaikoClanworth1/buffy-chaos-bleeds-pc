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
#include <stdint.h>
#include <stdlib.h>

#include "recomp/gen/recomp_types.h"

#pragma comment(lib, "winmm.lib")

void D3DDevice_Swap_0013A070_orig(void);
void buffy_settings_frame(void);
void buffy_mods_frame(void);

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
    if (!on)
        return;
    period = freq.QuadPart / 60;
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
        nv2a_gpu_queue_flip(1);
        D3DDevice_Swap_0013A070_orig();
        return;
    }
    {
        int split = buffy_coop_decide();
        if (split >= 0)
            nv2a_gpu_queue_flip(split ? 0 : 2);     /* player 1 only, or both */
    }
    buffy_frame_note_swap();
    buffy_settings_frame();                 /* widescreen flags, 16:9 or 4:3 frame */
    buffy_mods_frame();
    pace_60hz();
    D3DDevice_Swap_0013A070_orig();
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
        if (!mem++)
            buffy_memstats_start();
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
