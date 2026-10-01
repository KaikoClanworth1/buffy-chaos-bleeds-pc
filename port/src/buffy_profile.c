/**
 * A sampling profiler for the game thread (testing): where its time goes.
 *
 *   BUFFY_PROFILE=start:secs   from `start` seconds after the first frame,
 *                              for `secs` seconds
 *   BUFFY_PROFILE_ALL=1        every thread of ours, each on its own (the
 *                              GPU threads the game waits on, audio...)
 *
 * A thread of our own stops the game thread every millisecond, notes its
 * instruction pointer and lets it go on; at the end the functions the samples
 * fell in are named (the exe's pdb) and the top 60 logged as [PROFILE] lines,
 * with their share of the samples. Other programs' load does not skew it:
 * only where the game thread is counts, not how long a frame took.
 */
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")
#include <timeapi.h>

#define PROF_SLOTS 65536                   /* instruction pointers seen (open addressing) */

typedef struct { DWORD64 ip; unsigned n; DWORD tid; } ProfHit;
typedef struct { char name[160]; unsigned n; } ProfFn;

static ProfHit *s_hits;
static HANDLE s_game;
static DWORD s_game_tid;
static unsigned s_total;                   /* the game thread's samples */

static void prof_note(DWORD64 ip, DWORD tid)
{
    unsigned h = (unsigned)(((ip >> 2) ^ tid) * 2654435761u) & (PROF_SLOTS - 1), i;
    for (i = 0; i < PROF_SLOTS; i++) {
        ProfHit *e = &s_hits[(h + i) & (PROF_SLOTS - 1)];
        if (e->ip == ip && e->tid == tid) { e->n++; return; }
        if (!e->ip) { e->ip = ip; e->tid = tid; e->n = 1; return; }
    }
}

/* One thread's context now: its instruction pointer, or 0. */
static DWORD64 prof_ip(HANDLE th)
{
    CONTEXT c;
    DWORD64 ip = 0;
    if (SuspendThread(th) == (DWORD)-1)
        return 0;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_CONTROL;
    if (GetThreadContext(th, &c))
        ip = c.Rip;
    ResumeThread(th);
    return ip;
}

static int prof_cmp(const void *a, const void *b)
{
    return (int)((const ProfFn *)b)->n - (int)((const ProfFn *)a)->n;
}

static DWORD WINAPI prof_thread(LPVOID arg)
{
    const char *e = (const char *)arg;
    double start = atof(e), secs = strchr(e, ':') ? atof(strchr(e, ':') + 1) : 10.0;
    DWORD t_end;
    ProfFn *fns;
    unsigned nf = 0, i, k;
    timeBeginPeriod(1);
    Sleep((DWORD)(start * 1000));
    fprintf(stderr, "[PROFILE] sampling the game thread for %.0f s\n", secs);
    t_end = GetTickCount() + (DWORD)(secs * 1000);
    {
        /* the threads: the game's, or all of ours but this one */
        static HANDLE th[64];
        static DWORD tids[64];
        int nth = 0, t;
        if (getenv("BUFFY_PROFILE_ALL")) {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te;
            te.dwSize = sizeof te;
            if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) {
                do {
                    if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != GetCurrentThreadId() && nth < 64
                            && (th[nth] = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID)) != NULL)
                        tids[nth++] = te.th32ThreadID;
                } while (Thread32Next(snap, &te));
            }
            if (snap != INVALID_HANDLE_VALUE)
                CloseHandle(snap);
        } else {
            th[0] = s_game;
            tids[0] = GetThreadId(s_game);
            nth = 1;
        }
        s_game_tid = GetThreadId(s_game);
        while ((int)(GetTickCount() - t_end) < 0) {
            for (t = 0; t < nth; t++) {
                DWORD64 ip = prof_ip(th[t]);
                if (ip) {
                    prof_note(ip, tids[t]);
                    if (tids[t] == s_game_tid)
                        s_total++;
                }
            }
            Sleep(1);
        }
    }
    /* name them: samples by function */
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    fns = (ProfFn *)calloc(4096, sizeof *fns);
    {
    /* each thread seen, the game thread first; idle ones (all in waits) left out */
    DWORD tlist[64];
    unsigned ntl = 0, ti;
    tlist[ntl++] = s_game_tid;
    for (i = 0; i < PROF_SLOTS && ntl < 64; i++)
        if (s_hits[i].ip) {
            for (ti = 0; ti < ntl && tlist[ti] != s_hits[i].tid; ti++)
                ;
            if (ti == ntl)
                tlist[ntl++] = s_hits[i].tid;
        }
    for (ti = 0; ti < ntl; ti++) {
    unsigned tot = 0;
    nf = 0;
    if (fns)
        memset(fns, 0, 4096 * sizeof *fns);
    for (i = 0; fns && i < PROF_SLOTS; i++) {
        if (!s_hits[i].ip || s_hits[i].tid != tlist[ti])
            continue;
        tot += s_hits[i].n;
        char buf[sizeof(SYMBOL_INFO) + 160];
        SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
        DWORD64 disp = 0;
        const char *name = "?";
        if (!s_hits[i].ip)
            continue;
        memset(buf, 0, sizeof buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 159;
        if (SymFromAddr(GetCurrentProcess(), s_hits[i].ip, &disp, sym))
            name = sym->Name;
        else {
            /* outside our pdb: name the module */
            static char mod[160];
            HMODULE m = NULL;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)(uintptr_t)s_hits[i].ip, &m) && GetModuleFileNameA(m, mod, sizeof mod)) {
                const char *slash = strrchr(mod, '\\');
                name = slash ? slash + 1 : mod;
            }
        }
        for (k = 0; k < nf && strcmp(fns[k].name, name); k++)
            ;
        if (k == nf) {
            if (nf == 4096)
                continue;
            strncpy_s(fns[nf].name, sizeof fns[nf].name, name, _TRUNCATE);
            nf++;
        }
        fns[k].n += s_hits[i].n;
    }
    if (fns && tot) {
        unsigned idle = 0;
        qsort(fns, nf, sizeof *fns, prof_cmp);
        for (k = 0; k < nf; k++)
            if (strstr(fns[k].name, "Wait") || strstr(fns[k].name, "Delay") || strstr(fns[k].name, "ZwRemove")
                    || strstr(fns[k].name, "NtRemove"))
                idle += fns[k].n;
        if (tlist[ti] != s_game_tid && idle * 100 >= tot * 95)
            continue;                                   /* (a thread that only waits) */
        fprintf(stderr, "[PROFILE] thread %lu%s: %u samples, %u functions, %.1f%% waiting\n", tlist[ti],
                tlist[ti] == s_game_tid ? " (game)" : "", tot, nf, 100.0 * idle / tot);
        for (k = 0; k < nf && k < (tlist[ti] == s_game_tid ? 40u : 12u); k++)
            fprintf(stderr, "[PROFILE] %5.1f%%  %s\n", 100.0 * fns[k].n / tot, fns[k].name);
    }
    }
    }
    free(fns);
    timeEndPeriod(1);
    return 0;
}

/* From the frame hook (the game thread), each frame: starts it once. */
void buffy_profile_frame(void)
{
    static int started;
    const char *e;
    if (started)
        return;
    started = 1;
    if (!(e = getenv("BUFFY_PROFILE")))
        return;
    if (!(s_hits = (ProfHit *)calloc(PROF_SLOTS, sizeof *s_hits)))
        return;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_game,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    CloseHandle(CreateThread(NULL, 0, prof_thread, (LPVOID)e, 0, NULL));
}
