/**
 * The native renderer (docs/native-renderer.md). Phase 1: the foundation.
 *
 * The game draws through the Xbox D3D8 library (translated with the rest of
 * the code). The native renderer hooks its draw calls and reads the library's
 * own state at each draw -- render states, texture stages, bound textures,
 * vertex streams, vertex shader -- to draw with the PC's GPU directly. No
 * Xbox GPU is emulated: the library runs in its own no-GPU mode, and the
 * NV2A pushbuffer it still writes is retired unread (main.c,
 * buffy_gpuwait.c).
 *
 * This phase captures that state and reports it (BUFFY_NATIVE_LOG): a
 * per-frame summary, and every draw of one frame in full, to check the
 * capture against what the emulated renderer draws. Drawing comes next.
 *
 * Renderer choice: [Display] Renderer = vulkan (the default) | d3d11 in
 * buffy_settings.ini, or BUFFY_RENDERER: the GPU layer's two backends
 * (vulkan falls back to Direct3D 11 without Vulkan 1.3; BUFFY_GPU_BACKEND
 * overrides). Older settings' "native" and "emulated" (the emulated Xbox GPU,
 * removed in 0.5) mean vulkan.
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "recomp/gen/recomp_types.h"
#include "buffy_settings.h"
#include "gpu.h"                  /* the GPU layer (xboxrecomp/src/gpu) */

int  nv2a_gpu_native_begin(void);
void nv2a_gpu_native_scale(float *sx, float *sy);
void nv2a_gpu_native_present(GpuTexture *tex, uint32_t w, uint32_t h, uint32_t pw, uint32_t ph);

/* getenv, remembered: debug switches are checked on every draw, and the
 * CRT's getenv walks the whole environment each time. Keyed by the name's
 * address (every caller passes a string literal): one pointer compare a
 * lookup -- a list searched with strcmp cost 6% of the game thread. */
static const char *nenv(const char *name)
{
    static struct { const char *name; const char *value; } tab[256];
    unsigned h = (unsigned)(((uintptr_t)name >> 3) * 2654435761u) >> 24, i;
    for (i = 0; i < 256; i++, h = (h + 1) & 255) {
        if (tab[h].name == name)
            return tab[h].value;
        if (!tab[h].name) {
            tab[h].value = getenv(name);
            tab[h].name = name;
            return tab[h].value;
        }
    }
    return getenv(name);
}

/* ── the native device and frame (game thread only) ────────────────────── */

static int s_gpu_up;                       /* the device is up (nv2a_gpu_native_begin) */
static struct {
    GpuTexture *tex, *z;                    /* colour (a target, sampled when shown), depth */
    uint32_t pw, ph;
} s_fb;                                     /* the frame: the game's 640x480 at the output scale */

static void fb_release(void)
{
    if (s_fb.tex) gpu_texture_release(s_fb.tex);
    if (s_fb.z) gpu_texture_release(s_fb.z);
    memset(&s_fb, 0, sizeof s_fb);
}

/* A colour target (sampled too) and its depth buffer, pw x ph. */
static int make_targets(uint32_t pw, uint32_t ph, GpuTexture **tex, GpuTexture **z)
{
    GpuTextureDesc td;
    memset(&td, 0, sizeof td);
    td.width = pw; td.height = ph; td.mips = 1;
    td.format = GPU_FMT_BGRA8;
    td.flags = GPU_TEX_TARGET | GPU_TEX_SAMPLED;
    if (!(*tex = gpu_texture_create(&td, NULL)))
        return 0;
    td.format = GPU_FMT_D24S8;
    td.flags = GPU_TEX_DEPTH;
    *z = gpu_texture_create(&td, NULL);
    return 1;
}

/* The frame's targets, (re)made to the current output scale. */
static int fb_ready(void)
{
    float sx, sy;
    uint32_t pw, ph;
    if (!s_gpu_up)
        return 0;
    nv2a_gpu_native_scale(&sx, &sy);
    pw = (uint32_t)(640 * sx + 0.5f);
    ph = (uint32_t)(480 * sy + 0.5f);
    if (s_fb.tex && s_fb.pw == pw && s_fb.ph == ph)
        return 1;
    fb_release();
    if (!make_targets(pw, ph, &s_fb.tex, &s_fb.z))
        return 0;
    s_fb.pw = pw;
    s_fb.ph = ph;
    fprintf(stderr, "[NATIVE] frame %ux%u\n", pw, ph);
    return 1;
}

/* The Xbox D3D8 library's state (addresses from the game's linker map,
 * relocated; see docs/native-renderer.md). */
#define D3D_DIRTY_FLAGS    0x145038u
#define D3D_TEXTURE_STATE  0x145040u        /* [4 stages][32] DWORDs */
#define D3D_RENDER_STATE   0x145240u        /* DWORD per state; 0..91 "simple" */
#define D3D_PDEVICE        0x1454D8u
#define D3D_STREAMS        0x147560u        /* 16 x { stride, ?, vertex buffer } */
#define D3D_SIMPLE_ENCODE  0x1962C8u        /* simple state -> NV2A method */
#define D3D_SIMPLE_COUNT   92
#define DEV_TEXTURES       0xB68u           /* device: bound texture per stage */
#define DEV_VSHADER        0x37Cu           /* device: current vertex shader */
#define D3D_RS_ZENABLE     0x14547Cu        /* D3D__RenderState[D3DRS_ZENABLE] */

/* The W-buffer's depth range: what D3D's CommonSetViewport computes for
 * W mode and hands the GPU as NV097_SET_CLIP_MIN/MAX (0x0394, 0x0398) --
 * written only into the pushbuffer, so read back from what it just wrote.
 * Kept from the last time W mode was on (Swap sets a Z range for its own
 * copy in between). 0, 0 until then. */
static float s_wclip[2];

void D3D_CommonSetViewport_001368E0_orig(void);
void D3D_CommonSetViewport_001368E0(void)
{
    D3D_CommonSetViewport_001368E0_orig();         /* returns the pushbuffer position after its writes */
    if (MEM32(D3D_RS_ZENABLE) == 2 && g_eax >= 12 && MEM32(g_eax - 12) == 0x80394u) {
        float mn = MEMF(g_eax - 8), mx = MEMF(g_eax - 4);
        if (mn > 0.0f && mx > mn) {
            s_wclip[0] = mn;
            s_wclip[1] = mx;
        }
    }
}

enum { PATH_INDEXED, PATH_UP, PATH_BEGIN, PATH_PUSH, PATH_CLEAR, PATH_RT, PATH_COUNT };
static const char *k_path_name[PATH_COUNT] = { "indexed", "up", "begin/end", "push", "clear", "rendertarget" };

static int s_mode = -1;                     /* 1 once the device is up */
static int s_log = -1;
static long s_frame;
static unsigned s_count[PATH_COUNT], s_prims;
static long s_detail_frame = -1;            /* the frame logged draw by draw */

int buffy_native_mode(void)
{
    if (s_mode < 0) {
        const char *e = getenv("BUFFY_RENDERER");
        char v[32] = "";
        const char *ini = buffy_settings_path();
        if (e)
            strncpy_s(v, sizeof v, e, _TRUNCATE);
        else if (ini && *ini)
            GetPrivateProfileStringA("Display", "Renderer", "vulkan", v, sizeof v, ini);
        if (!getenv("BUFFY_GPU_BACKEND"))
            _putenv_s("BUFFY_GPU_BACKEND", !_stricmp(v, "d3d11") ? "d3d11" : "vulkan");
        /* the device comes up here, on this (the game's) thread, which owns
         * its context from now on */
        if (!(s_gpu_up = nv2a_gpu_native_begin())) {
            fprintf(stderr, "[NATIVE] no GPU device (Vulkan 1.3 or Direct3D 11)\n");
            if (!getenv("BUFFY_NO_WINDOW"))
                MessageBoxA(NULL, "No graphics device came up.\n\n"
                            "Buffy needs a GPU with Vulkan 1.3 or Direct3D 11. "
                            "Updating the graphics driver usually fixes this.",
                            "Buffy the Vampire Slayer: Chaos Bleeds", MB_ICONERROR);
            ExitProcess(1);
        }
        {
            void nv2a_native_const_pool(void);
            nv2a_native_const_pool();
            fprintf(stderr, "[NATIVE] native renderer\n");
        }
        s_mode = 1;
    }
    return s_mode;
}

static int log_on(void)
{
    if (s_log < 0) {
        const char *e = nenv("BUFFY_NATIVE_LOG");
        s_log = e ? 1 : 0;
        if (e && *e == 'f')                     /* f<frame>: that frame, draw by draw */
            s_detail_frame = atol(e + 1);
    }
    return s_log;
}

/* A texture header (D3DBaseTexture: Common, Data, Lock, Format, Size). */
static void log_texture(int stage, uint32_t t)
{
    uint32_t fmt, w, h;
    if (!t) return;
    fmt = MEM32(t + 0xC);
    w = 1u << ((fmt >> 20) & 0xF);
    h = 1u << ((fmt >> 24) & 0xF);
    if (MEM32(t + 0x10)) {                      /* linear: Size holds width/height */
        w = (MEM32(t + 0x10) & 0xFFF) + 1;
        h = ((MEM32(t + 0x10) >> 12) & 0xFFF) + 1;
    }
    fprintf(stderr, " t%d=%08X(fmt %02X %ux%u data %08X)", stage, t, (fmt >> 8) & 0xFF, w, h, MEM32(t + 4));
}

static void capture(int path, uint32_t prim, uint32_t count, uint32_t data, uint32_t stride)
{
    uint32_t dev = MEM32(D3D_PDEVICE);
    int i;
    s_count[path]++;
    if (path <= PATH_BEGIN)
        s_prims += count;
    if (!log_on() || s_frame != s_detail_frame || !dev)
        return;
    fprintf(stderr, "[NATIVE] f%ld %s prim %u count %u", s_frame, k_path_name[path], prim, count);
    if (path == PATH_UP)
        fprintf(stderr, " data %08X stride %u", data, stride);
    if (path == PATH_INDEXED) {
        fprintf(stderr, " idx %08X s0 %08X/%u", data, MEM32(D3D_STREAMS + 8), MEM32(D3D_STREAMS));
    }
    fprintf(stderr, " vs %08X", MEM32(dev + DEV_VSHADER));
    for (i = 0; i < 4; i++)
        log_texture(i, MEM32(dev + DEV_TEXTURES + i * 4));
    /* the states that most change a draw's look, by their NV2A method */
    fprintf(stderr, " | zfunc %X blend %u src %X dst %X atest %u aref %02X",
            MEM32(D3D_RENDER_STATE + 57 * 4), MEM32(D3D_RENDER_STATE + 59 * 4), MEM32(D3D_RENDER_STATE + 62 * 4),
            MEM32(D3D_RENDER_STATE + 63 * 4), MEM32(D3D_RENDER_STATE + 60 * 4), MEM32(D3D_RENDER_STATE + 61 * 4));
    fprintf(stderr, " combiners %u\n", MEM32(D3D_RENDER_STATE + 53 * 4) & 0xF);
    {
        /* BUFFY_NATIVE_DUMP: raw structures of the first draws of each path,
         * for working out their layouts */
        static int dumped[PATH_COUNT];
        if (nenv("BUFFY_NATIVE_DUMP") && dumped[path]++ < 2) {
            uint32_t vs = MEM32(dev + DEV_VSHADER), k;
            fprintf(stderr, "  vshader %08X:", vs);
            for (k = 0; vs && k < 0x60; k += 4)
                fprintf(stderr, "%s%08X", k % 32 ? " " : "\n    ", MEM32(vs + k));
            fprintf(stderr, "\n  texture state stage 0:");
            for (k = 0; k < 32 * 4; k += 4)
                fprintf(stderr, "%s%08X", k % 32 ? " " : "\n    ", MEM32(D3D_TEXTURE_STATE + k));
            fprintf(stderr, "\n  render state 57..:");
            for (k = 57 * 4; k < 0x298 - 8; k += 4)
                fprintf(stderr, "%s%08X", (k - 57 * 4) % 32 ? " " : "\n    ", MEM32(D3D_RENDER_STATE + k));
            if (path == PATH_UP) {
                fprintf(stderr, "\n  vertices:");
                for (k = 0; k < stride * 2 && k < 64; k += 4)
                    fprintf(stderr, " %08X", MEM32(data + k));
            }
            fprintf(stderr, "\n");
        }
    }
}

/* Once a frame, from the Swap hook (buffy_frame.c). */
void buffy_native_frame(void)
{
    static DWORD last;
    int i;
    if (log_on() && GetTickCount() - last >= 2000) {
        last = GetTickCount();
        fprintf(stderr, "[NATIVE] frame %ld:", s_frame);
        for (i = 0; i < PATH_COUNT; i++)
            fprintf(stderr, " %s %u", k_path_name[i], s_count[i]);
        fprintf(stderr, ", %u vertices drawn\n", s_prims);
    }
    if (nenv("BUFFY_NATIVE_FRAMELOG")) {
        /* every frame: what was drawn and how long it took (flicker hunting) */
        static LARGE_INTEGER f, t0;
        LARGE_INTEGER t;
        extern double g_native_present_ms;
        if (!f.QuadPart) QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t);
        extern double g_native_draw_ms;
        fprintf(stderr, "[NFRAME] %ld draws %.2f ms dt %.2f present %.2f idx %u up %u begin %u push %u clear %u rt %u\n", s_frame, g_native_draw_ms,
                t0.QuadPart ? (double)(t.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart : 0.0,
                g_native_present_ms, s_count[PATH_INDEXED], s_count[PATH_UP], s_count[PATH_BEGIN],
                s_count[PATH_PUSH], s_count[PATH_CLEAR], s_count[PATH_RT]);
        t0 = t;
    }
    {
        extern double g_native_draw_ms;
        g_native_draw_ms = 0;
    }
    memset(s_count, 0, sizeof s_count);
    s_prims = 0;
    s_frame++;
}

/* Startup check: the simple-state table must be the one the addresses above
 * were worked out from (a different build of the game would not match). */
void buffy_native_check(void)
{
    if (MEM32(D3D_SIMPLE_ENCODE + 57 * 4) != 0x00040354u)       /* ZFUNC -> NV097_SET_DEPTH_FUNC */
        fprintf(stderr, "[NATIVE] warning: the D3D state table is not where expected (%08X)\n",
                MEM32(D3D_SIMPLE_ENCODE + 57 * 4));
}

/* ── native draws ─────────────────────────────────────────────────────── */

/* nv2a_native.inc's interface */
typedef struct { uint32_t va, stride, fmt; } NativeAttr;
typedef struct { uint32_t res, palette, data, format; GpuTexture *srv; } NativeTex;
typedef struct {
    uint32_t prim, count, index_va;
    NativeAttr attr[16];
    int passthrough;
    uint32_t vp_start;
    NativeTex tex[4];
    int pixel_shader;
    const uint32_t *rs, *tss;
    float clip_min, clip_max;          /* the depth range D3D clips to (nv2a_native.inc) */
} NativeDraw;
int  nv2a_native_draw(const NativeDraw *d, GpuTexture *rt, GpuTexture *ds,
                      uint32_t pw, uint32_t ph, uint32_t sw, uint32_t sh);
void nv2a_native_vp_load(uint32_t slot, const uint32_t *code, uint32_t dwords);
void nv2a_native_const(uint32_t slot, const float *v, uint32_t vectors);
void nv2a_native_stats(unsigned long long out[4]);
uint32_t nv2a_native_resolve(uint32_t phys);

#define DEV_PSHADER        0x36Cu           /* device: current pixel shader, 0 = texture stages */
#define DEV_VP_START       0x384u           /* device: program start slot */
#define DEV_INPUT          0x6E0u           /* device: SetVertexShaderInput override */
#define DEV_INPUT_STREAMS  0x6E8u           /* ... its streams { VB, stride, offset } */
#define D3D_ATTR_REMAP     0x26CB40u        /* attribute -> vertex shader slot */
#define DIRTY_INPUT        0x40000000u      /* the override is in force */

static uint32_t s_cur_rt, s_backbuffer;     /* render target now; the one shown at Swap */

/* Render-to-texture: a D3D11 target per guest surface the game renders into
 * (keyed by its memory, the surface header's Data), sampled instead of that
 * memory when a texture over the same memory is used. Output-scaled like the
 * frame. */
typedef struct {
    uint32_t data, w, h;
    uint32_t pw, ph;
    GpuTexture *tex, *z;
    DWORD last_use;
} NativeRT;
#define NATIVE_RTS 32
static NativeRT s_rts[NATIVE_RTS];

/* The target for guest surface `surf` (a D3DSurface header: Common, Data,
 * Lock, Format, Size), made on first use. */
static NativeRT *rt_for(uint32_t surf)
{
    uint32_t data = MEM32(surf + 4), size = MEM32(surf + 0x10), w, h;
    float sx, sy;
    int i, lru = 0;
    NativeRT *r;
    if (!data)
        return NULL;
    if (size) {                                 /* linear: D3DSIZE */
        w = (size & 0xFFF) + 1;
        h = ((size >> 12) & 0xFFF) + 1;
    } else {                                    /* swizzled: log2 sizes in the format */
        uint32_t fmt = MEM32(surf + 0xC);
        w = 1u << ((fmt >> 20) & 0xF);
        h = 1u << ((fmt >> 24) & 0xF);
    }
    for (i = 0; i < NATIVE_RTS; i++) {
        if (s_rts[i].tex && s_rts[i].data == data && s_rts[i].w == w && s_rts[i].h == h) {
            s_rts[i].last_use = GetTickCount();
            return &s_rts[i];
        }
        if (!s_rts[i].tex) { lru = i; break; }
        if (s_rts[i].last_use < s_rts[lru].last_use) lru = i;
    }
    r = &s_rts[lru];
    if (r->tex) gpu_texture_release(r->tex);     /* (a draw recorded for interpolation keeps its own reference) */
    if (r->z) gpu_texture_release(r->z);
    memset(r, 0, sizeof *r);
    nv2a_gpu_native_scale(&sx, &sy);
    r->pw = (uint32_t)(w * sx + 0.5f);
    r->ph = (uint32_t)(h * sy + 0.5f);
    if (!make_targets(r->pw, r->ph, &r->tex, &r->z))
        return NULL;
    r->data = data; r->w = w; r->h = h;
    r->last_use = GetTickCount();
    {
        float black[4] = { 0, 0, 0, 0 };
        gpu_clear_target(r->tex, black);
        if (r->z)
            gpu_clear_depth(r->z, GPU_CLEAR_DEPTH | GPU_CLEAR_STENCIL, 1.0f, 0);
    }
    if (log_on())
        fprintf(stderr, "[NATIVE] render target %08X %ux%u\n", data, w, h);
    return r;
}

/* The render target a texture's texels are, if the game rendered them. */
static GpuTexture *rt_texture(uint32_t data)
{
    int i;
    for (i = 0; i < NATIVE_RTS; i++)
        if (s_rts[i].tex && s_rts[i].data == data)
            return s_rts[i].tex;
    return NULL;
}

/* Where draws and clears go now: the frame, or a render target. */
typedef struct {
    GpuTexture *rt, *ds;
    uint32_t pw, ph, sw, sh;
} NativeTarget;

static int target_now(NativeTarget *t)
{
    if (!fb_ready())
        return 0;
    if (s_backbuffer && s_cur_rt && s_cur_rt != s_backbuffer) {
        NativeRT *r = rt_for(s_cur_rt);
        if (!r)
            return 0;
        t->rt = r->tex; t->ds = r->z; t->pw = r->pw; t->ph = r->ph; t->sw = r->w; t->sh = r->h;
        return 1;
    }
    t->rt = s_fb.tex; t->ds = s_fb.z; t->pw = s_fb.pw; t->ph = s_fb.ph; t->sw = 640; t->sh = 480;
    return 1;
}
/* SwitchTexture: a stage's texels and format set straight into the GPU,
 * without the device's texture array; in force while that array still holds
 * what it held then. */
static struct { uint32_t data, format, under; } s_switch[4];
static unsigned s_skipped_rt;

/* The vertex shader's attributes, as the D3D library would program them. */
static void native_attrs(NativeDraw *d, uint32_t dev, uint32_t up_data, uint32_t up_stride)
{
    uint32_t vs = MEM32(dev + DEV_VSHADER), flags = vs ? MEM32(vs + 4) : 0;
    int a;
    memset(d->attr, 0, sizeof d->attr);
    if (!vs)
        return;
    if (!up_data && (MEM32(D3D_DIRTY_FLAGS) & DIRTY_INPUT) && MEM32(dev + DEV_INPUT)) {
        uint32_t in = MEM32(dev + DEV_INPUT) & ~1u;
        for (a = 0; a < 16; a++) {
            uint32_t s = MEM32(in + 0x14 + a * 16), off = MEM32(in + 0x18 + a * 16), fmt = MEM32(in + 0x1C + a * 16);
            uint32_t st = dev + DEV_INPUT_STREAMS + s * 12, vb;
            if ((fmt & 0xFF) == 2 || !(fmt & 0xF0) || s >= 16)
                continue;
            vb = MEM32(st);
            if (!vb)
                continue;
            d->attr[a].va = nv2a_native_resolve(MEM32(vb + 4)) + MEM32(st + 8) + off;
            d->attr[a].stride = MEM32(st + 4);
            d->attr[a].fmt = fmt & 0xFF;
        }
        return;
    }
    for (a = 0; a < 16; a++) {
        uint32_t slot = MEM8(D3D_ATTR_REMAP + (flags & 0x10) + a), e = vs + slot * 16;
        uint32_t s = MEM32(e + 0x14), off = MEM32(e + 0x18), fmt = MEM32(e + 0x1C);
        if ((fmt & 0xFF) == 2 || !(fmt & 0xF0))
            continue;
        if (up_data) {
            d->attr[a].va = up_data + off;
            d->attr[a].stride = up_stride;
        } else {
            uint32_t vb = s < 16 ? MEM32(D3D_STREAMS + s * 12 + 8) : 0;
            if (!vb)
                continue;
            d->attr[a].va = nv2a_native_resolve(MEM32(vb + 4)) + off;
            d->attr[a].stride = MEM32(D3D_STREAMS + s * 12);
        }
        d->attr[a].fmt = fmt & 0xFF;
    }
}

/* The viewport transform the library folds into c58 (scale) and c59
 * (offset) -- written only into the pushbuffer, so asked of the library's
 * own D3DDevice_GetViewportOffsetAndScale(D3DVECTOR4 *pOffset, *pScale). */
void D3DDevice_GetViewportOffsetAndScale_00135DA0(void);
uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);
static int s_vp_dirty = 1;                 /* viewport, target or constant mode changed */
static void native_viewport_consts(void)
{
    static uint32_t buf;
    if (!s_vp_dirty)
        return;
    s_vp_dirty = 0;
    uint32_t esp0 = g_esp, eax = g_eax, ecx = g_ecx, edx = g_edx;
    if (!buf && !(buf = xbox_HeapAlloc(32, 16)))
        return;
    g_esp -= 4; MEM32(g_esp) = buf + 16;               /* pScale */
    g_esp -= 4; MEM32(g_esp) = buf;                    /* pOffset */
    g_esp -= 4; MEM32(g_esp) = 0;                      /* return address */
    D3DDevice_GetViewportOffsetAndScale_00135DA0();
    g_esp = esp0;
    g_eax = eax; g_ecx = ecx; g_edx = edx;
    nv2a_native_const(58, (const float *)XBOX_PTR(buf + 16), 1);
    nv2a_native_const(59, (const float *)XBOX_PTR(buf), 1);
}

static void native_draw(uint32_t prim, uint32_t count, uint32_t index_va, uint32_t up_data, uint32_t up_stride)
{
    NativeDraw d;
    NativeTarget tg;
    uint32_t dev = MEM32(D3D_PDEVICE), vs;
    int st;
    if (s_mode != 1 || !dev || !target_now(&tg))
        return;
    memset(&d, 0, sizeof d);
    d.prim = prim;
    d.count = count;
    d.index_va = index_va;
    vs = MEM32(dev + DEV_VSHADER);
    d.passthrough = vs && (MEM32(vs + 4) & 2);
    if (nenv("BUFFY_NATIVE_VSLOG")) {
        /* debug: each distinct vertex shader object once, with its flags */
        static uint32_t seen[64];
        static int n;
        int i;
        for (i = 0; i < n && seen[i] != vs; i++) ;
        if (i == n && n < 64) {
            seen[n++] = vs;
            fprintf(stderr, "[VSLOG] vs %08X handle-word %08X flags %08X +8 %08X +C %08X start %u | prim %u n %u up %u stride %u ix %08X\n",
                    vs, MEM32(dev + DEV_VSHADER), vs ? MEM32(vs + 4) : 0, vs ? MEM32(vs + 8) : 0, vs ? MEM32(vs + 0xC) : 0,
                    MEM32(dev + DEV_VP_START), prim, count, up_data != 0, up_stride, index_va);
            if (vs) {
                uint32_t k, nd = MEM32(vs + 0xC);
                void nv2a_native_dump_program(uint32_t);
                fprintf(stderr, "[VSLOG]   handle %08X block:", MEM32(dev + 0x380));
                for (k = 0; k < nd && k < 24; k++)
                    fprintf(stderr, " %08X", MEM32(vs + 0x114 + k * 4));
                fputc('\n', stderr);
                nv2a_native_dump_program(MEM32(dev + DEV_VP_START));
            }
        }
    }
    d.vp_start = MEM32(dev + DEV_VP_START);
    /* the depth range W-buffering maps W through (CommonSetViewport, below) */
    d.clip_min = s_wclip[0];
    d.clip_max = s_wclip[1];
    native_attrs(&d, dev, up_data, up_stride);
    if (up_data && up_stride == 16 && count == 4 && nenv("BUFFY_NATIVE_RECTLOG")) {
        /* debug: full-screen solid rects (EXWnd::_SolidRect2D) drawn with some alpha, and who asked */
        uint32_t c0 = MEM32(up_data + 12), c1 = MEM32(up_data + 28), c2 = MEM32(up_data + 44), c3 = MEM32(up_data + 60);
        static unsigned n;
        if (((c0 | c1 | c2 | c3) & 0xFF000000u) && *(float *)XBOX_PTR(up_data + 32) - *(float *)XBOX_PTR(up_data) > 600.0f
                && *(float *)XBOX_PTR(up_data + 36) - *(float *)XBOX_PTR(up_data + 4) > 400.0f && n++ < 60) {
            uint32_t k, shown = 0;
            fprintf(stderr, "[RECT] frame %ld data %08X colours %08X %08X %08X %08X pos %g,%g..%g,%g callers:", s_frame, up_data,
                    c0, c1, c2, c3, *(float *)XBOX_PTR(up_data), *(float *)XBOX_PTR(up_data + 4),
                    *(float *)XBOX_PTR(up_data + 32), *(float *)XBOX_PTR(up_data + 36));
            for (k = 0; k < 64 && shown < 8; k++) {
                uint32_t v = MEM32(g_esp + k * 4);
                if (v >= 0x11000 && v < 0x135140) { fprintf(stderr, " %06X", v); shown++; }
            }
            fputc(10, stderr);
            fprintf(stderr, "[RECT]   stack:");
            for (k = 0; k < 24; k++)
                fprintf(stderr, " %08X", MEM32(g_esp + k * 4));
            fputc(10, stderr);
        }
    }
    for (st = 0; st < 4; st++) {
        d.tex[st].res = MEM32(dev + DEV_TEXTURES + st * 4);
        if (s_switch[st].data && s_switch[st].under == d.tex[st].res) {
            d.tex[st].data = s_switch[st].data;
            d.tex[st].format = s_switch[st].format;
        }
        if (d.tex[st].res || d.tex[st].data)             /* rendered here, not decoded from memory */
            d.tex[st].srv = rt_texture(d.tex[st].data ? d.tex[st].data : MEM32(d.tex[st].res + 4));
    }
    d.pixel_shader = MEM32(dev + DEV_PSHADER) != 0;
    d.rs = (const uint32_t *)XBOX_PTR(D3D_RENDER_STATE);
    d.tss = (const uint32_t *)XBOX_PTR(D3D_TEXTURE_STATE);
    native_viewport_consts();
    if (nenv("BUFFY_NATIVE_UPLOG") && up_data && up_stride != 16) {
        /* debug: the 2D draws' layout (text, sprites) */
        static int shown;
        if (shown++ < 6) {
            int a;
            fprintf(stderr, "[NATIVE] UP prim %u n %u stride %u vs %08X flags %08X start %u ps %u tex0 %08X:",
                    prim, count, up_stride, vs, vs ? MEM32(vs + 4) : 0, d.vp_start, MEM32(dev + DEV_PSHADER), d.tex[0].res);
            for (a = 0; a < 16; a++)
                if (d.attr[a].fmt)
                    fprintf(stderr, " a%d@%u:%02X", a, d.attr[a].va - up_data, d.attr[a].fmt);
            {
                const uint32_t *t = (const uint32_t *)XBOX_PTR(D3D_TEXTURE_STATE);
                const uint32_t *r = (const uint32_t *)XBOX_PTR(D3D_RENDER_STATE);
                fprintf(stderr, "\n   tss0 cop %u %X %X %X aop %u %X %X %X res %X | tss1 cop %u aop %u | blend %u %X %X atest %u %X ref %u | tfactor %08X",
                        t[12], t[13], t[14], t[15], t[16], t[17], t[18], t[19], t[20], t[32 + 12], t[32 + 16],
                        r[59], r[62], r[63], r[60], r[58], r[61], r[148]);
            }
            if (shown == 1) { void nv2a_native_dump_program(uint32_t); nv2a_native_dump_program(d.vp_start); }
            fprintf(stderr, "\n   raw:");
            for (a = 0; a < (int)up_stride / 4 && a < 10; a++)
                fprintf(stderr, " %08X", MEM32(up_data + a * 4));
            fprintf(stderr, "\n");
        }
    }
    if (nenv("BUFFY_NATIVE_TRACE")) {
        static int shown;
        if (shown++ < 4)
            fprintf(stderr, "[NATIVE] vs %08X flags %08X start %u c58 %g %g %g %g c59 %g %g %g %g\n", vs,
                    vs ? MEM32(vs + 4) : 0, d.vp_start, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    }
    {
        /* (BUFFY_NATIVE_FRAMELOG: the draws' own time a frame) */
        extern double g_native_draw_ms;
        LARGE_INTEGER a, b, f;
        QueryPerformanceCounter(&a);
        nv2a_native_draw(&d, tg.rt, tg.ds, tg.pw, tg.ph, tg.sw, tg.sh);
        QueryPerformanceCounter(&b);
        QueryPerformanceFrequency(&f);
        g_native_draw_ms += (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
    }
}

/* ── vertex programs and constants (the library writes them to the GPU
 * only; the native renderer keeps its own copy) ───────────────────────── */

void D3DDevice_LoadVertexShader_00137D70_orig(void);
void D3DDevice_SetVertexShaderConstant1Fast_00137AC0_orig(void);
void D3DDevice_SetVertexShaderConstantNotInlineFast_00137BC0_orig(void);
void D3DDevice_SetVertexShaderConstant4_00137B10_orig(void);

/* void LoadVertexShader(DWORD Handle, DWORD Address): the program at
 * (Handle - 1) + 0x114, [+0xC] DWORDs, into program memory at Address. */
void D3DDevice_LoadVertexShader_00137D70(void)
{
    uint32_t vs = MEM32(g_esp + 4) - 1, addr = MEM32(g_esp + 8);
    if (s_mode == 1 && vs)
        nv2a_native_vp_load(addr, (const uint32_t *)XBOX_PTR(vs + 0x114), MEM32(vs + 0xC));
    D3DDevice_LoadVertexShader_00137D70_orig();
}

/* fastcall: ecx = constant slot (register + 96), edx = the values */
void D3DDevice_SetVertexShaderConstant1Fast_00137AC0(void)
{
    if (s_mode == 1)
        nv2a_native_const(g_ecx, (const float *)XBOX_PTR(g_edx), 1);
    D3DDevice_SetVertexShaderConstant1Fast_00137AC0_orig();
}

/* fastcall: ecx = slot, edx = values, [esp+4] = DWORD count */
void D3DDevice_SetVertexShaderConstantNotInlineFast_00137BC0(void)
{
    if (s_mode == 1)
        nv2a_native_const(g_ecx, (const float *)XBOX_PTR(g_edx), MEM32(g_esp + 4) / 4);
    D3DDevice_SetVertexShaderConstantNotInlineFast_00137BC0_orig();
}

/* fastcall: ecx = slot, edx = four vectors */
void D3DDevice_SetVertexShaderConstant4_00137B10(void)
{
    if (s_mode == 1)
        nv2a_native_const(g_ecx, (const float *)XBOX_PTR(g_edx), 4);
    D3DDevice_SetVertexShaderConstant4_00137B10_orig();
}

/* void SetShaderConstantMode(Mode): also writes the library's constant
 * pool (c60..62), to the pushbuffer only. */
void nv2a_native_const_pool(void);
void D3DDevice_SetShaderConstantMode_00137E70_orig(void);
void D3DDevice_SetShaderConstantMode_00137E70(void)
{
    D3DDevice_SetShaderConstantMode_00137E70_orig();
    s_vp_dirty = 1;
    if (s_mode == 1)
        nv2a_native_const_pool();
}

/* HRESULT SetViewport(CONST D3DVIEWPORT8 *): c58/c59 change with it. */
void D3DDevice_SetViewport_00138AB0_orig(void);
void D3DDevice_SetViewport_00138AB0(void)
{
    s_vp_dirty = 1;
    D3DDevice_SetViewport_00138AB0_orig();
}

/* void SwitchTexture(ecx = the stage's 0x1B00/0x1B04 method header,
 * edx = texel address, [esp+4] = format): the engine's fast texture change. */
void D3DDevice_SwitchTexture_00138500_orig(void);
void D3DDevice_SwitchTexture_00138500(void)
{
    uint32_t m = g_ecx & 0x1FFC, dev = MEM32(D3D_PDEVICE);
    if (m >= 0x1B00 && m < 0x1C00 && dev) {
        int st = (int)((m - 0x1B00) / 0x40);
        s_switch[st].data = g_edx;
        s_switch[st].format = MEM32(g_esp + 4);
        s_switch[st].under = MEM32(dev + DEV_TEXTURES + st * 4);
    }
    D3DDevice_SwitchTexture_00138500_orig();
}

/* ── the hooked draw calls (all stdcall) ─────────────────────────────── */

void D3DDevice_DrawIndexedVertices_0013A940_orig(void);
void D3DDevice_DrawVerticesUP_0013A7D0_orig(void);
void D3DDevice_Begin_0013ABE0_orig(void);
void D3DDevice_BeginPush_001354E0_orig(void);
void D3DDevice_Clear_0013A350_orig(void);
void D3DDevice_SetRenderTarget_001387A0_orig(void);

/* The draws are the native renderer's alone: the library's own versions
 * only turned them into NV2A commands for a GPU that is not there (its lazy
 * state flush, the indices or vertices copied into the pushbuffer, a fence)
 * -- the renderer reads the state the setters keep, not those commands.
 * (testing) BUFFY_D3D_DRAWS=1 runs the library's as well. */
static int d3d_draws(void)
{
    static int on = -1;
    if (on < 0)
        on = nenv("BUFFY_D3D_DRAWS") != NULL;
    return on;
}

/* void DrawIndexedVertices(D3DPRIMITIVETYPE, UINT VertexCount, CONST PVOID pIndexData) */
void D3DDevice_DrawIndexedVertices_0013A940(void)
{
    capture(PATH_INDEXED, MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp + 12), 0);
    native_draw(MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp + 12), 0, 0);
    if (!d3d_draws()) {
        g_esp += 16;                                     /* stdcall, ret 12 */
        return;
    }
    D3DDevice_DrawIndexedVertices_0013A940_orig();
}

/* void DrawVerticesUP(D3DPRIMITIVETYPE, UINT VertexCount, CONST void *pData, UINT Stride) */
void D3DDevice_DrawVerticesUP_0013A7D0(void)
{
    capture(PATH_UP, MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp + 12), MEM32(g_esp + 16));
    native_draw(MEM32(g_esp + 4), MEM32(g_esp + 8), 0, MEM32(g_esp + 12), MEM32(g_esp + 16));
    if (!d3d_draws()) {
        g_esp += 20;                                     /* stdcall, ret 16 */
        return;
    }
    D3DDevice_DrawVerticesUP_0013A7D0_orig();
}

/* void Begin(D3DPRIMITIVETYPE) -- immediate mode, vertices by SetVertexData*, End */
void D3DDevice_Begin_0013ABE0(void)
{
    capture(PATH_BEGIN, MEM32(g_esp + 4), 0, 0, 0);
    D3DDevice_Begin_0013ABE0_orig();
}

/* HRESULT BeginPush(DWORD Count, DWORD **ppPush) -- raw commands (particles) */
static uint32_t s_push_start;
void D3DDevice_BeginPush_001354E0(void)
{
    uint32_t pp = MEM32(g_esp + 8);
    capture(PATH_PUSH, 0, MEM32(g_esp + 4), 0, 0);
    D3DDevice_BeginPush_001354E0_orig();
    s_push_start = pp ? MEM32(pp) : 0;
}

/* The bytes of one vertex the current vertex shader describes (its slots'
 * offsets and formats), for vertices packed inline. */
static uint32_t vs_vertex_bytes(void)
{
    uint32_t dev = MEM32(D3D_PDEVICE), vs = dev ? MEM32(dev + DEV_VSHADER) : 0, flags, bytes = 0;
    int a;
    if (!vs)
        return 0;
    flags = MEM32(vs + 4);
    for (a = 0; a < 16; a++) {
        uint32_t e = vs + MEM8(D3D_ATTR_REMAP + (flags & 0x10) + a) * 16;
        uint32_t off = MEM32(e + 0x18), fmt = MEM32(e + 0x1C) & 0xFF, type = fmt & 0xF, size = fmt >> 4, sz;
        if (fmt == 2 || !size)
            continue;
        sz = type == 0 ? 4 : type == 6 ? 4 : (type == 1 || type == 5) ? size * 2 : type == 4 ? size : size * 4;
        if (off + sz > bytes)
            bytes = off + sz;
    }
    return bytes;
}

/* void EndPush(DWORD *pPush): the block from BeginPush is the particle
 * system's own commands -- constants (0x1EA4 slot, 0x0B80.. values), then per
 * batch SET_BEGIN_END (0x17FC prim ... 0) around INLINE_ARRAY vertex data
 * (0x1818). Walked here: the constants kept, each batch drawn natively. */
void D3DDevice_EndPush_00135500_orig(void);
static void native_push_block(uint32_t p, uint32_t end)
{
    static uint32_t scratch;
    uint32_t cslot = 0, cword = 0, prim = 0, used = 0, stride = vs_vertex_bytes();
    if (!scratch && !(scratch = xbox_HeapAlloc(256 * 1024, 16)))
        return;
    while (p + 4 <= end) {
        uint32_t h = MEM32(p), count = (h >> 18) & 0x7FF, method = h & 0x1FFC, k;
        int noninc = (h & 0x40000000u) != 0;
        p += 4;
        if ((h & 3) || !count)
            continue;
        for (k = 0; k < count && p + 4 <= end; k++, p += 4) {
            uint32_t m = noninc ? method : method + k * 4, v = MEM32(p);
            if (m == 0x1EA4) {
                cslot = v; cword = 0;
            } else if (m >= 0x0B80 && m < 0x0C00) {
                float f;
                memcpy(&f, &v, 4);
                if (cslot + cword / 4 < 192) {
                    float vec[4];
                    void nv2a_native_const_get(uint32_t slot, float out[4]);
                    nv2a_native_const_get(cslot + cword / 4, vec);
                    vec[cword & 3] = f;
                    nv2a_native_const(cslot + cword / 4, vec, 1);
                }
                cword++;
            } else if (m == 0x17FC) {
                if (v) {
                    prim = v;
                    used = 0;
                } else if (prim && stride && used >= stride) {
                    native_draw(prim, used / stride, 0, scratch, stride);
                    prim = 0;
                }
            } else if (m == 0x1818 && prim) {
                if (used + 4 <= 256 * 1024) {
                    MEM32(scratch + used) = v;
                    used += 4;
                }
            }
        }
    }
}

void D3DDevice_EndPush_00135500(void)
{
    uint32_t end = MEM32(g_esp + 4);
    if (s_mode == 1 && s_push_start && end > s_push_start && end - s_push_start < 4u << 20 && !nenv("BUFFY_NATIVE_NOPUSH"))
        native_push_block(s_push_start, end);
    s_push_start = 0;
    D3DDevice_EndPush_00135500_orig();
}

/* HRESULT Clear(Count, pRects, Flags, Color, Z, Stencil). Flags: 0xF0 the
 * colour channels, 1 depth, 2 stencil. */
void D3DDevice_Clear_0013A350(void)
{
    uint32_t flags = MEM32(g_esp + 12), color = MEM32(g_esp + 16), zbits = MEM32(g_esp + 20);
    float z;
    memcpy(&z, &zbits, 4);
    capture(PATH_CLEAR, flags, color, 0, 0);
    {
        void nv2a_native_trace_note(uint32_t kind, uint32_t a, uint32_t b, uint32_t c);
        nv2a_native_trace_note(1, flags, color, MEM32(g_esp + 4));
    }
    {
        NativeTarget tg;
        if (s_mode == 1 && target_now(&tg)) {
            {
                void nv2a_native_interp_clear(GpuTexture *rt, unsigned flags, const float c[4], float z, uint8_t stencil);
                float c[4] = { ((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f,
                               (color & 255) / 255.0f, (color >> 24) / 255.0f };
                if (flags & 0xF0)
                    gpu_clear_target(tg.rt, c);
                if (flags & 0xF3)
                    nv2a_native_interp_clear(tg.rt, flags, c, z, (uint8_t)MEM32(g_esp + 24));
            }
            if ((flags & 3) && tg.ds)
                gpu_clear_depth(tg.ds, ((flags & 1) ? GPU_CLEAR_DEPTH : 0) | ((flags & 2) ? GPU_CLEAR_STENCIL : 0),
                                z, (uint8_t)MEM32(g_esp + 24));
        }
    }
    D3DDevice_Clear_0013A350_orig();
}

/* At D3DDevice_Swap (buffy_frame.c): show the native frame. */
double g_native_present_ms;                 /* the last present, for BUFFY_NATIVE_FRAMELOG */

/* Frame interpolation: on when asked for, in one window (co-op's second
 * window and split screen show every frame as they are), and where a frame
 * between can be seen -- vsync off, or a display at 100 Hz or more. */
void nv2a_native_interp_setup(int on, GpuTexture *main);
int  nv2a_native_interp_render(float t, GpuTexture *main_tex, GpuTexture **out_tex);
void nv2a_native_interp_frame_end(void);
void nv2a_gpu_native_present_extra(GpuTexture *tex, uint32_t w, uint32_t h, uint32_t pw, uint32_t ph);
int  nv2a_gpu_vsync(void);
int  nv2a_gpu_has_second_window(void);
int  buffy_settings_frame_interpolation(void);

static LARGE_INTEGER s_last_real;          /* the last real frame's present */
double g_native_draw_ms;                   /* this frame's draws, host time (BUFFY_NATIVE_FRAMELOG) */
LARGE_INTEGER g_frame_begin;               /* this frame's work began (the last Swap returned) */
int buffy_settings_fps_limit(void);

static int interp_active(void)
{
    static int hz = -1;
    /* above a 60 FPS limit the real frames come that fast themselves */
    if (!buffy_settings_frame_interpolation() || nv2a_gpu_has_second_window() || buffy_settings_fps_limit() > 60)
        return 0;
    if (hz < 0) {
        DEVMODEW dm;
        memset(&dm, 0, sizeof dm);
        dm.dmSize = sizeof dm;
        hz = EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm) ? (int)dm.dmDisplayFrequency : 60;
        fprintf(stderr, "[NATIVE] frame interpolation: display at %d Hz%s\n", hz,
                hz < 100 ? " (with vsync on it only shows with vsync off)" : "");
    }
    return !nv2a_gpu_vsync() || hz >= 100 || getenv("BUFFY_INTERP");
}

/* Wait `ms` (the half frame before the real frame, vsync off). */
static void wait_ms(double ms)
{
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    if (ms > 2.0)
        Sleep((DWORD)(ms - 1.5));
    do
        QueryPerformanceCounter(&b);
    while ((double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart < ms);
}

void buffy_native_present(void)
{
    int interp;
    if (s_mode != 1 || !fb_ready())
        return;
    s_backbuffer = s_cur_rt;               /* what is shown is the back buffer */
    s_vp_dirty = 1;
    interp = interp_active();
    if (interp) {
        /* the in-between frame half a frame after the last real one, the real
         * one a frame after it -- or at once when the work ran past that
         * (the frame in between never holds the game back) */
        GpuTexture *it = NULL;
        LARGE_INTEGER f, now;
        double since, frame_ms = 1000.0 / buffy_settings_fps_limit();
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&now);
        since = s_last_real.QuadPart ? (double)(now.QuadPart - s_last_real.QuadPart) * 1000.0 / (double)f.QuadPart : 1e9;
        LARGE_INTEGER q0, q1, q2, q3;
        static double cost = 1.0;            /* what the frame in between took lately (ms) */
        double work;
        int ok;
        QueryPerformanceCounter(&q0);
        /* too late for one: this frame's work left no room for it. With vsync
         * (a 100 Hz+ display) it has to make the refresh halfway through the
         * frame -- one later and the real frame misses its own, a whole
         * refresh late (the pause menu, 11-13 ms of work: 40 fps); without,
         * it has to leave the real frame its time. The frame's own work is
         * measured from when the last Swap returned: the time since the last
         * present also holds the 60 fps limiter's wait. */
        work = g_frame_begin.QuadPart ? (double)(q0.QuadPart - g_frame_begin.QuadPart) * 1000.0 / (double)f.QuadPart : 0.0;
        ok = since < 200.0
             && work + cost < (nv2a_gpu_vsync() ? frame_ms * 0.5 - 0.5 : frame_ms - 1.0)
             && nv2a_native_interp_render(0.5f, s_fb.tex, &it);
        QueryPerformanceCounter(&q1);
        if (ok)
            cost = cost * 0.9 + 0.1 * ((double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)f.QuadPart + 0.2);
        q2 = q3 = q1;
        if (ok) {
            if (!nv2a_gpu_vsync() && since < frame_ms * 0.5)
                wait_ms(frame_ms * 0.5 - since);
            QueryPerformanceCounter(&q2);
            nv2a_gpu_native_present_extra(it, 640, 480, s_fb.pw, s_fb.ph);
            QueryPerformanceCounter(&q3);
            if (!nv2a_gpu_vsync()) {
                QueryPerformanceCounter(&now);
                since = (double)(now.QuadPart - s_last_real.QuadPart) * 1000.0 / (double)f.QuadPart;
                if (since < frame_ms - 0.5)
                    wait_ms(frame_ms - 0.5 - since);
            }
        }
        if (nenv("BUFFY_NATIVE_FRAMELOG"))
            fprintf(stderr, "[NINTERP] work %.2f since %.2f render %.2f wait %.2f extra %.2f ok %d\n",
                    work, (double)(q0.QuadPart - s_last_real.QuadPart) * 1000.0 / (double)f.QuadPart,
                    (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)f.QuadPart,
                    (double)(q2.QuadPart - q1.QuadPart) * 1000.0 / (double)f.QuadPart,
                    (double)(q3.QuadPart - q2.QuadPart) * 1000.0 / (double)f.QuadPart, ok);
    }
    {
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        nv2a_gpu_native_present(s_fb.tex, 640, 480, s_fb.pw, s_fb.ph);
        if (nenv("BUFFY_TEST_PRESENT_MS"))
            Sleep((DWORD)atoi(nenv("BUFFY_TEST_PRESENT_MS")));   /* test: a present that waits, as in fullscreen */
        QueryPerformanceCounter(&b);
        g_native_present_ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
    }
    QueryPerformanceCounter(&s_last_real);                /* (the in-between frame's clock) */
    nv2a_native_interp_frame_end();
    nv2a_native_interp_setup(interp, s_fb.tex);          /* (the next frame is recorded for it) */
    if (log_on()) {
        static DWORD last;
        if (GetTickCount() - last >= 2000) {
            unsigned long long st[4];
            last = GetTickCount();
            nv2a_native_stats(st);
            fprintf(stderr, "[NATIVE] draws: %llu drawn, %llu no vertex shader, %llu too big, %llu no pixel shader; "
                    "%u render-to-texture skipped\n", st[0], st[1], st[2], st[3], s_skipped_rt);
            s_skipped_rt = 0;
        }
    }
}

/* HRESULT SetRenderTarget(pRenderTarget, pNewZStencil) */
void D3DDevice_SetRenderTarget_001387A0(void)
{
    capture(PATH_RT, 0, 0, MEM32(g_esp + 4), MEM32(g_esp + 8));
    {
        void nv2a_native_trace_note(uint32_t kind, uint32_t a, uint32_t b, uint32_t c);
        nv2a_native_trace_note(2, MEM32(g_esp + 4), MEM32(g_esp + 8), 0);
    }
    if (MEM32(g_esp + 4))
        s_cur_rt = MEM32(g_esp + 4);
    s_vp_dirty = 1;
    if (nenv("BUFFY_NATIVE_RTLOG")) {
        static int shown;
        uint32_t sf = MEM32(g_esp + 4), z = MEM32(g_esp + 8);
        if (shown++ < 16 && sf)
            fprintf(stderr, "[NATIVE] SetRenderTarget %08X (common %08X data %08X fmt %08X size %08X parent %08X) z %08X backbuffer %08X\n",
                    sf, MEM32(sf), MEM32(sf + 4), MEM32(sf + 0xC), MEM32(sf + 0x10), MEM32(sf + 0x14), z, s_backbuffer);
    }
    D3DDevice_SetRenderTarget_001387A0_orig();
}
