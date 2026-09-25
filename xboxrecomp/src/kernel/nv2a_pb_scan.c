/*
 * Read-only survey of the pushbuffer a title submits.
 *
 * The title builds NV2A commands in guest RAM and advances DMA_PUT; nothing
 * here executes them, so the framebuffer stays black however far the game
 * gets. Before any of that can be made to draw, the question is what it
 * actually asks for -- which methods, on which object classes, how many of
 * them -- because that is the difference between "the existing PGRAPH
 * translator nearly covers this" and "this needs a real one".
 *
 * Purely a reader: it walks the buffer and counts, and never writes to guest
 * memory or to the GPU state. Enabled with RECOMP_PB_SCAN.
 *
 * Pushbuffer encoding (NV20/NV2A), one dword per command header:
 *   (w & 0xE0030003) == 0x00000000  increasing methods
 *   (w & 0xE0030003) == 0x40000000  non-increasing (same method, count params)
 *   (w & 0x00000003) == 0x00000001  jump
 *   (w & 0x00000003) == 0x00000002  call
 *   (w & 0xFFFF0003) == 0x00020000  return
 * For a method header: count = (w >> 18) & 0x7FF, subchannel = (w >> 13) & 7,
 * method = w & 0x1FFC.
 */
#include <stdio.h>
#if defined(_WIN32)
#include <windows.h>  /* GetTickCount: slow-method timing */
#endif
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define PB_MAX_METHODS 4096

static struct { uint32_t method, subch, count; } s_seen[PB_MAX_METHODS];
static int s_seen_count;

/* Parse health. An inventory is only worth reading if the walk stayed in step
 * with the command stream: a decoder that desynchronises produces plausible
 * looking method numbers out of parameter data, and the counts then describe
 * nothing. Unrecognised words are the tell. */
static uint32_t s_tot_words, s_tot_unknown, s_tot_jumps, s_tot_segments;

/* Executing is opt-in separately from surveying: a survey is read-only, while
 * the executor writes to guest memory. */
extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern void nv2a_pb_exec_report(void);
static int s_exec_enabled = -1;

static void note(uint32_t subch, uint32_t method)
{
    for (int i = 0; i < s_seen_count; i++) {
        if (s_seen[i].method == method && s_seen[i].subch == subch) {
            s_seen[i].count++;
            return;
        }
    }
    if (s_seen_count >= PB_MAX_METHODS) {
        /* Silently dropping past the cap is how a truncated inventory reads as
         * "the title never does that" -- exactly the wrong conclusion when the
         * inventory is being used to decide what to implement. */
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[PB] method table full at %d -- inventory is"
                            " truncated\n", PB_MAX_METHODS);
        }
    }
    if (s_seen_count < PB_MAX_METHODS) {
        s_seen[s_seen_count].method = method;
        s_seen[s_seen_count].subch  = subch;
        s_seen[s_seen_count].count  = 1;
        s_seen_count++;
    }
}

/* NV097 (Kelvin 3D class) methods worth naming. The point of the survey is to
 * decide what a translator has to implement, and a bare method number does not
 * answer that -- "0x1808 x412" only means something once it reads
 * INLINE_ARRAY. Unnamed ones still get counted. */
static const struct { uint32_t m; const char *name; } NV097_NAMES[] = {
    { 0x0000, "SET_OBJECT" },
    { 0x0100, "NO_OPERATION" },
    { 0x0104, "SET_WARNING_ENABLE" },
    { 0x0130, "SET_FLIP_READ" },
    { 0x0200, "SET_SURFACE_CLIP_HORIZONTAL" },
    { 0x0204, "SET_SURFACE_CLIP_VERTICAL" },
    { 0x0208, "SET_SURFACE_FORMAT" },
    { 0x020C, "SET_SURFACE_PITCH" },
    { 0x0210, "SET_SURFACE_COLOR_OFFSET" },
    { 0x0214, "SET_SURFACE_ZETA_OFFSET" },
    { 0x0300, "SET_ALPHA_TEST_ENABLE" },
    { 0x0304, "SET_BLEND_ENABLE" },
    { 0x030C, "SET_DEPTH_TEST_ENABLE" },
    { 0x0310, "SET_DITHER_ENABLE" },
    { 0x0314, "SET_LIGHTING_ENABLE" },
    { 0x033C, "SET_CULL_FACE_ENABLE" },
    { 0x0340, "SET_DEPTH_MASK" },
    { 0x0350, "SET_CLEAR_DEPTH_VALUE" },
    { 0x1D8C, "SET_CLEAR_DEPTH" },
    { 0x1D90, "SET_COLOR_CLEAR_VALUE" },
    { 0x1D94, "CLEAR_SURFACE" },
    { 0x1D6C, "SET_ZSTENCIL_CLEAR" },
    { 0x0B80, "SET_TRANSFORM_PROGRAM" },
    { 0x0B00, "SET_TRANSFORM_CONSTANT" },
    { 0x1720, "SET_VERTEX_DATA_ARRAY_OFFSET" },
    { 0x1760, "SET_VERTEX_DATA_ARRAY_FORMAT" },
    { 0x17FC, "SET_BEGIN_END" },
    { 0x1800, "ARRAY_ELEMENT16" },
    { 0x1808, "INLINE_ARRAY" },
    { 0x1810, "DRAW_ARRAYS" },
    { 0x1B00, "SET_TEXTURE_OFFSET" },
    { 0x1B04, "SET_TEXTURE_FORMAT" },
    { 0x1B08, "SET_TEXTURE_ADDRESS" },
    { 0x1B0C, "SET_TEXTURE_CONTROL0" },
    { 0x1B14, "SET_TEXTURE_IMAGE_RECT" },
    { 0x0FD8, "SET_COMBINER_*" },
    { 0x0000, NULL },
};

static const char *nv097_name(uint32_t m)
{
    int i;
    for (i = 0; NV097_NAMES[i].name; i++)
        if (NV097_NAMES[i].m == m)
            return NV097_NAMES[i].name;
    return "";
}

void nv2a_pb_scan_report(void)
{
    int i;

    if (s_exec_enabled > 0)
        nv2a_pb_exec_report();
    if (!s_seen_count || !getenv("RECOMP_PB_SCAN"))
        return;
    fprintf(stderr, "[PB] %u segments, %u words, %u jumps, %u unrecognised"
                    " -- %d distinct (subchannel, method) pairs\n",
            s_tot_segments, s_tot_words, s_tot_jumps, s_tot_unknown,
            s_seen_count);
    for (i = 0; i < s_seen_count; i++)
        fprintf(stderr, "  [PB]   subch %u  method 0x%04X  x%-6u %s\n",
                s_seen[i].subch, s_seen[i].method, s_seen[i].count,
                nv097_name(s_seen[i].method));
    fflush(stderr);
}

void nv2a_pb_scan(uint32_t start_va, uint32_t end_va)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t va = start_va;
    uint32_t words = 0, jumps = 0, unknown = 0;

    if (s_exec_enabled < 0)
        s_exec_enabled = getenv("RECOMP_PB_EXEC") != NULL;
    if (!(getenv("RECOMP_PB_SCAN") || s_exec_enabled) || end_va <= start_va)
        return;
    if (end_va - start_va > 0x400000u)        /* a sane single-frame bound */
        end_va = start_va + 0x400000u;

    while (va < end_va && words < 0x100000u) {
        uint32_t w = *(const uint32_t *)(mem + va);
        va += 4;
        words++;

        if ((w & 3u) == 1u || (w & 0xE0000003u) == 0x20000000u) {
            jumps++;
            break;                            /* a jump ends this segment */
        }
        if ((w & 3u) == 2u || (w & 0xFFFF0003u) == 0x00020000u)
            continue;
        if ((w & 0x00030003u) == 0u) {
            uint32_t count  = (w >> 18) & 0x7FFu;
            uint32_t subch  = (w >> 13) & 7u;
            uint32_t method =  w & 0x1FFCu;
            int noninc = (w & 0xE0000000u) == 0x40000000u;

            for (uint32_t i = 0; i < count && va < end_va; i++) {
                uint32_t m = noninc ? method : method + i * 4;
                note(subch, m);
                /* Same walk, two consumers: the survey counts, the executor
                 * acts. Keeping them on one decode means they can never
                 * disagree about what the stream said. */
                if (s_exec_enabled) {
                    DWORD t0 = GetTickCount();
                    nv2a_pb_exec_method(subch, m,
                                        *(const uint32_t *)(mem + va));
                    if (GetTickCount() - t0 > 50) {
                        static int slow_shown;
                        if (slow_shown++ < 20)
                            fprintf(stderr, "  [PB] slow method subch %u 0x%04X param 0x%08X: %lu ms\n",
                                    subch, m, *(const uint32_t *)(mem + va),
                                    (unsigned long)(GetTickCount() - t0));
                    }
                }
                va += 4;
                words++;
            }
            continue;
        }
        unknown++;
    }

    s_tot_words += words;
    s_tot_unknown += unknown;
    s_tot_jumps += jumps;
    s_tot_segments++;
}

#if defined(_WIN32)
/* ── The GPU's FIFO puller, on its own thread ───────────────────────────────
 *
 * On hardware the NV2A reads the pushbuffer from DMA_GET towards DMA_PUT,
 * following jumps and calls, and advances GET as it consumes commands. The
 * service thread in xbox_memory_layout.c used to report everything consumed
 * the moment PUT moved (GET = PUT) and executed the new span inline. Two
 * problems with that once a title submits real work: D3D was free to reuse
 * ring space the executor had not read yet, and while the executor chewed
 * through a backlog nothing cleared the busy bits D3D was spinning on (Buffy:
 * CDevice::KickOff waiting on the PFB flush bit for as long as the executor
 * took). This thread is the consumer; GET moves only past executed commands,
 * so D3D throttles on real progress the way it does against the GPU.
 *
 * Addresses in the stream are physical: the pushbuffer DMA object starts at
 * physical 0, and the contiguous window is the physical view. */
#define PB_PHYS(p) (XBOX_PB_CONTIG_BASE | ((p) & 0x0FFFFFFFu))
#define XBOX_PB_CONTIG_BASE 0x80000000u

/* PUT value the puller has drained up to and is idle at; the service thread
 * acknowledges D3D's flush handshake (PFB 0x100410) only once this matches the
 * current PUT, so a kickoff waits for the GPU as it does on hardware. */
volatile uint32_t g_pb_idle_put = 0xFFFFFFFFu;

static int s_fence_trace;

/* RECOMP_PB_PROFILE: cumulative host time per method, top 10 every 10 s. */
static int s_prof;
static int s_slow;           /* RECOMP_PB_SLOW: log commands >= 30 ms */
static uint64_t s_prof_ticks[0x800], s_prof_count[0x800];

static void pb_prof_report(void)
{
    static DWORD last;
    DWORD now = GetTickCount();
    LARGE_INTEGER f;
    int k, i, best;
    if (now - last < 10000)
        return;
    last = now;
    QueryPerformanceFrequency(&f);
    fprintf(stderr, "  [PB] profile (ms total / calls):");
    for (k = 0; k < 10; k++) {
        best = -1;
        for (i = 0; i < 0x800; i++)
            if (s_prof_ticks[i] && (best < 0 || s_prof_ticks[i] > s_prof_ticks[best]))
                best = i;
        if (best < 0)
            break;
        fprintf(stderr, " %04X=%.0f/%llu", best << 2,
                s_prof_ticks[best] * 1000.0 / (double)f.QuadPart,
                (unsigned long long)s_prof_count[best]);
        s_prof_ticks[best] = 0;
        s_prof_count[best] = 0;
    }
    fprintf(stderr, "\n");
}
extern int xbox_Nv2aSoftwareMethod(uint32_t subch, uint32_t method, uint32_t data);
extern void xbox_gpu_idle(int which);
extern void xbox_gpu_poke(void);
extern uint32_t nv2a_pb_kelvin(uint32_t method);

/* The GPU keeps the clear values in PGRAPH registers, and Xbox D3D uses them
 * to hand CMiniport::SoftwareMethod its InsertCallback arguments: the
 * callback in SET_ZSTENCIL_CLEAR_VALUE (PGRAPH 0x1A88), its context in
 * SET_COLOR_CLEAR_VALUE (PGRAPH 0x186C). Mirror them before each software
 * method, or every callback reads a null pointer and is skipped. */
static void pgraph_mirror_clear_values(void)
{
    uint8_t *m = (uint8_t *)xbox_GetMemoryOffset();
    *(volatile uint32_t *)(m + 0xFD401A88u) = nv2a_pb_kelvin(0x1D8C);
    *(volatile uint32_t *)(m + 0xFD40186Cu) = nv2a_pb_kelvin(0x1D90);
}

static DWORD WINAPI pb_fifo_thread(LPVOID param)
{
    volatile uint32_t *put_reg = (volatile uint32_t *)((char *)param + 0x800040u);
    volatile uint32_t *get_reg = (volatile uint32_t *)((char *)param + 0x800044u);
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t get = 0, ret_addr = 0;

    s_exec_enabled = 1;
    s_fence_trace = getenv("RECOMP_PB_FENCE_TRACE") != NULL;
    s_prof = getenv("RECOMP_PB_PROFILE") != NULL;
    s_slow = getenv("RECOMP_PB_SLOW") != NULL;
    for (;;) {
        uint32_t put = *put_reg & 0x0FFFFFFFu;   /* physical, like every address below */
        uint32_t budget = 0;

        if (!get) {
            /* D3D's first PUT is the ring start with nothing submitted
             * before it (CDevice::InitializePushBuffer); pull from there. It
             * never writes GET itself -- that is the GPU's job, i.e. ours. */
            get = (*get_reg ? *get_reg : put) & 0x0FFFFFFFu;
            if (!get) { Sleep(1); continue; }
            *get_reg = get;
        }
        if (get == put) {
            g_pb_idle_put = put;   /* drained: flush acks may proceed */
            xbox_gpu_idle(0);      /* sleeps unless the title is waiting on us */
            continue;
        }
        xbox_gpu_poke();
        while (get != put && budget < 0x4000u) {
            uint32_t w;
            /* GET is published per command, not per batch. D3D::BlockOnTime
             * decides whether it may patch an interrupt request into a fence
             * by measuring fence - GET (>= 0x2000 means "the GPU cannot have
             * fetched it yet"). A GET that lags the puller by a whole batch
             * let D3D patch a fence already executed, then sleep forever on
             * an interrupt that could no longer arrive (Buffy, first Swap). */
            if (get >= 0x04000000u) {
                /* Walked off the end of RAM: the stream is lost. */
                fprintf(stderr, "  [PB] GET %08X left RAM, resync to PUT %08X\n", get, put);
                get = put;
                break;
            }
            {
                static DWORD last_diag;
                DWORD now = GetTickCount();
                if (s_prof && now - last_diag > 2000) {
                    last_diag = now;
                    fprintf(stderr, "  [PB] t=%lu get %08X put %08X gpu time %u d3d next fence %u\n",
                            (unsigned long)now, get, put,
                            *(const uint32_t *)(mem + 0x82C81000u),
                            *(const uint32_t *)(mem + 0x0014550Cu));
                }
            }
            *get_reg = get;
            w = *(const uint32_t *)(mem + PB_PHYS(get));
            budget++;
            {
                /* Command history, dumped when the stream calls into memory
                 * that was never written (heap fill 0x51515151). */
                static uint32_t hist[48][3];
                static unsigned hi;
                static int dumped;
                /* Only control flow and the first header after it: a desync
                 * starts at a jump, call or return. */
                if (w) {
                    hist[hi % 48][0] = get; hist[hi % 48][1] = w; hist[hi % 48][2] = put;
                    hi++;
                }
                int bad_target = ((w & 3u) == 1u || (w & 3u) == 2u)
                    && ((w & 0x0FFFFFFCu) >= 0x04000000u
                        || *(const uint32_t *)(mem + PB_PHYS(w & 0x0FFFFFFCu)) == 0x51515151u);
                if (bad_target) {
                    /* Never follow a jump off the end of RAM or into memory
                     * nothing wrote: drop what is pending and resync at PUT. */
                    static int resyncs;
                    if (resyncs++ < 5)
                        fprintf(stderr, "  [PB] bad jump/call %08X at %08X, resync to PUT %08X"
                                        " (diag: gpu time %u, d3d next fence %u)\n",
                                w, get, put,
                                *(const uint32_t *)(mem + 0x82C81000u),
                                *(const uint32_t *)(mem + 0x0014550Cu));
                }
                if (bad_target && !dumped) {
                    unsigned k;
                    dumped = 1;
                    fprintf(stderr, "  [PB] bad call history (get, word, put):\n");
                    for (k = hi > 48 ? hi - 48 : 0; k < hi; k++)
                        fprintf(stderr, "    %08X %08X %08X\n",
                                hist[k % 48][0], hist[k % 48][1], hist[k % 48][2]);
                }
                if (bad_target) {
                    get = put;
                    continue;
                }
            }
            if ((w & 0xE0000003u) == 0x20000000u) {        /* old-style jump */
                get = w & 0x0FFFFFFCu;
                continue;
            }
            if ((w & 3u) == 1u) {                          /* jump */
                get = w & 0x0FFFFFFCu;
                continue;
            }
            if ((w & 3u) == 2u) {                          /* call */
                {
                    static int shown;
                    uint32_t t = w & 0x0FFFFFFCu;
                    if (shown++ < 20)
                        fprintf(stderr, "  [PB] call @%08X -> %08X  word@phys-window %08X  word@VA %08X\n",
                                get, t, *(const uint32_t *)(mem + PB_PHYS(t)),
                                t < 0x04000000u ? *(const uint32_t *)(mem + t) : 0);
                }
                ret_addr = get + 4;
                get = w & 0x0FFFFFFCu;
                continue;
            }
            if ((w & 0xFFFF0003u) == 0x00020000u) {        /* return */
                get = ret_addr;
                continue;
            }
            /* A command is consumed whole or not at all. PUT can land inside
             * one (D3D::MakeRequestedSpace publishes what is written so far);
             * the pusher then waits for the rest instead of reading
             * parameters that are not there yet -- which previously carried
             * GET past PUT and off into stale memory for good. */
            if ((w & 0x00030003u) == 0u && get < put
                    && get + 4u + (((w >> 18) & 0x7FFu) << 2) > put)
                { g_pb_idle_put = put; break; }   /* waiting for the rest of a command */
            get += 4;
            if ((w & 0x00030003u) == 0u) {
                uint32_t count  = (w >> 18) & 0x7FFu;
                uint32_t subch  = (w >> 13) & 7u;
                uint32_t method =  w & 0x1FFCu;
                int noninc = (w & 0xE0000000u) == 0x40000000u;
                uint32_t i;
                for (i = 0; i < count; i++) {
                    uint32_t m = noninc ? method : method + i * 4;
                    uint32_t v = *(const uint32_t *)(mem + PB_PHYS(get));
                    if (s_fence_trace && (m == 0x100 || m == 0x1A4 || m == 0x50
                            || (m >= 0x1D60 && m <= 0x1D80) || subch != 0)) {
                        static int n;
                        if (n++ < 200)
                            fprintf(stderr, "  [FENCE] @%08X subch %u m 0x%04X = 0x%08X\n",
                                    get, subch, m, v);
                    }
                    if (s_prof) {
                        LARGE_INTEGER t0, t1;
                        QueryPerformanceCounter(&t0);
                        nv2a_pb_exec_method(subch, m, v);
                        QueryPerformanceCounter(&t1);
                        s_prof_ticks[(m >> 2) & 0x7FF] += (uint64_t)(t1.QuadPart - t0.QuadPart);
                        s_prof_count[(m >> 2) & 0x7FF]++;
                        pb_prof_report();
                    } else if (s_slow) {
                        LARGE_INTEGER t0, t1, fq;
                        QueryPerformanceCounter(&t0);
                        nv2a_pb_exec_method(subch, m, v);
                        QueryPerformanceCounter(&t1);
                        QueryPerformanceFrequency(&fq);
                        if ((t1.QuadPart - t0.QuadPart) * 1000 / fq.QuadPart >= 30)
                            fprintf(stderr, "  [PBSLOW] method %04X took %lld ms\n", m,
                                    (t1.QuadPart - t0.QuadPart) * 1000 / fq.QuadPart);
                    } else {
                        nv2a_pb_exec_method(subch, m, v);
                    }
                    /* NO_OPERATION with a parameter is a software-method
                     * interrupt; the GPU stalls until it is handled. */
                    if (subch == 0 && m == 0x100 && v) {
                        *get_reg = get + 4;
                        if (s_prof) {
                            /* Charged to pseudo-method 0x1FFC: time the
                             * pusher spends stalled on software methods. */
                            LARGE_INTEGER t0, t1;
                            QueryPerformanceCounter(&t0);
                            pgraph_mirror_clear_values();
                            xbox_Nv2aSoftwareMethod(subch, m, v);
                            QueryPerformanceCounter(&t1);
                            s_prof_ticks[0x7FF] += (uint64_t)(t1.QuadPart - t0.QuadPart);
                            s_prof_count[0x7FF]++;
                        } else {
                            LARGE_INTEGER t0, t1, fq;
                            QueryPerformanceCounter(&t0);
                            pgraph_mirror_clear_values();
                            xbox_Nv2aSoftwareMethod(subch, m, v);
                            QueryPerformanceCounter(&t1);
                            QueryPerformanceFrequency(&fq);
                            if (s_slow && (t1.QuadPart - t0.QuadPart) * 1000 / fq.QuadPart >= 30)
                                fprintf(stderr, "  [PBSLOW] software method %08X took %lld ms\n", v,
                                        (t1.QuadPart - t0.QuadPart) * 1000 / fq.QuadPart);
                        }
                        /* Xbox D3D's flip: code 1, surface address above. */
                        if ((v & 0x1Fu) == 1u) {
                            extern void nv2a_gpu_present(uint32_t phys);
                            nv2a_gpu_present((v >> 5) & 0x0FFFFFF0u);
                        }
                    }
                    get += 4;
                }
            }
        }
        *get_reg = get;
    }
    return 0;
}

/* Start the puller if RECOMP_PB_EXEC asks for execution. Returns 1 when it
 * owns DMA_GET, in which case the service thread must not touch it. */
int nv2a_pb_fifo_start(void *nv2a_regs)
{
    HANDLE h;
    if (!getenv("RECOMP_PB_EXEC") || !nv2a_regs)
        return 0;
    h = CreateThread(NULL, 0, pb_fifo_thread, nv2a_regs, 0, NULL);
    if (!h)
        return 0;
    CloseHandle(h);
    fprintf(stderr, "  NV2A FIFO puller: executing the pushbuffer on its own thread\n");
    return 1;
}
#endif
