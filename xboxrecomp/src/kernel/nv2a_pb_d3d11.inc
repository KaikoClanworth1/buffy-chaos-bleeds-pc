/*
 * Direct3D 11 backend for the pushbuffer executor.
 * Included by nv2a_pb_exec.c after nv2a_pb_vp.inc (shares their state).
 *
 * The software rasteriser is correct enough to show a title's 3D scenes and
 * nowhere near fast enough to play them. This keeps the parts that already
 * work -- command decoding and the CPU vertex-program interpreter -- and hands
 * everything after the vertex stage to the host GPU:
 *
 *   vertices   the program's oPos is screen space (Xbox D3D appends the
 *              viewport transform to every shader), so it is turned back into
 *              clip space with the title's own viewport constants (c[58]
 *              scale, c[59] offset): clip = ((s - off) / scale * w, w). That is
 *              exact even for vertices behind the eye, so the GPU clips.
 *   surfaces   each NV2A colour surface (by physical offset) is a render
 *              target with its own depth buffer; a flip presents one.
 *   textures   cached by address/format/size/content hash; DXT1/3/5 upload
 *              as BC1/2/3, the rest decode through the executor's sampler.
 *              A texture at a render target's address samples that target.
 *   pixels     stage 0 modulated by diffuse (register combiners are next),
 *              alpha test in the shader, blend/depth/colour-mask as states.
 *
 * On by default when a device can be created; RECOMP_GPU=0 keeps the software
 * rasteriser.
 */
#if defined(_WIN32)
#include <d3d11.h>
#include <d3dcompiler.h>
#include <mmsystem.h>
#if defined(_MSC_VER)
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "winmm.lib")
#endif

typedef struct {
    float pos[4];
    float c0[4];
    float c1[4];
    float t[4][4];
    float fog[4];
} GpuVertex;

typedef struct {
    uint32_t offset, w, h;                 /* NV2A size, in guest pixels */
    uint32_t pw, ph;                       /* host size: w, h times the render scale */
    ID3D11Texture2D          *tex;
    ID3D11RenderTargetView   *rtv;
    ID3D11ShaderResourceView *srv;
    ID3D11Texture2D          *ztex;
    ID3D11DepthStencilView   *dsv;
    int      used;
} GpuSurface;

typedef struct {
    uint32_t offset, fmt, w, h, pitch, hash, levels;
    uint32_t checked_frame;                /* contents last verified in this frame */
    ID3D11Texture2D          *tex;
    ID3D11ShaderResourceView *srv;
    DWORD    last_use;
    int      replaced;                     /* a texture pack's image (nv2a_texpack.inc) */
} GpuTexture;

#define GPU_MAX_SURF 16
#define GPU_MAX_TEX  512
#define GPU_TEX_INDEX 4096
/* Unique vertices per batch. Xbox index buffers are 16-bit, so a batch
 * names at most 65536; the vertex rings (host copy and D3D buffer, 256 bytes
 * a vertex on the GPU-program path) are sized for one such batch. */
#define GPU_MAX_VERTS 65536
#define GPU_MAX_IDX   393216       /* triangle-list indices per batch */

static int                     s_gpu_state = -1;     /* -1 untried, 0 off, 1 on */
static HWND                    s_gpu_hwnd;
static ID3D11Device           *s_dev;
static ID3D11DeviceContext    *s_ctx;
static IDXGISwapChain         *s_swap;
static ID3D11VertexShader     *s_vs;
static ID3D11PixelShader      *s_ps;
static ID3D11InputLayout      *s_layout;
static ID3D11Buffer           *s_vb, *s_cb;
static ID3D11SamplerState     *s_samp[2];            /* wrap, clamp */
static GpuSurface              s_surf[GPU_MAX_SURF];
static GpuTexture              s_texc[GPU_MAX_TEX];
static int                     s_texidx[GPU_TEX_INDEX];   /* identity hash -> slot + 1 */
static GpuVertex              *s_gv;
static uint32_t               *s_gi;                  /* indices into s_gv */
static ID3D11Buffer           *s_ib;
/* Per-batch map guest index -> converted vertex, so each vertex is run and
 * uploaded once however many triangles share it (strips reuse ~3x). */
#define GPU_VMAP 4096
static int s_use_gvp;                 /* this batch runs its program on the GPU */
static int s_vsh_ok;
static int vsh_init(void);
static uint32_t s_vmap_key[GPU_VMAP], s_vmap_gen[GPU_VMAP], s_vmap_slot[GPU_VMAP], s_vmap_g, s_nv;
/* RECOMP_VP_STATS: why texture lookups fail. */
static uint64_t s_tx_why[8];   /* 0 disabled, 1 no offset, 2 bad size, 3 rt, 4 cached, 5 new, 6 create fail, 7 tiny dxt */
static uint32_t s_tx_last_ctl0, s_tx_last_fmt, s_tx_last_off;
static DWORD    s_gpu_t0;
static uint32_t s_frame_no;           /* presents so far */
static uint32_t s_draw_in_frame;      /* draws since the last present */
static int      s_ps_count;             /* defined with the shader cache */
static uint64_t s_stage_mode_seen[32];

/* Display options (nv2a_gpu_set_display): the size the game's 640x480 is
 * rendered at (any aspect: 1920x1080 scales x by 3 and y by 2.25), vsync,
 * borderless fullscreen. Requested from any thread, applied by the GPU thread
 * at the next flip. */
static volatile LONG s_opt_w = 1280, s_opt_h = 960, s_opt_vsync = 1, s_opt_fullscreen, s_opt_dirty = 1;
static volatile LONG s_resize_pending;

/* Second window (two-screen co-op, nv2a_gpu_enable_second_window): its own
 * window and swap chain on the same device. Each D3D Swap the title makes is
 * queued with where it goes (nv2a_gpu_queue_flip: 0 window 1, 1 window 2,
 * 2 both) and the flips arrive in the same order, one per Swap. */
static volatile LONG s_want_win2;
static HWND          s_gpu_hwnd2;
static IDXGISwapChain *s_swap2;
static volatile LONG s_resize2_pending;
static RECT          s_windowed_rect2;
static int           s_win2_w, s_win2_h;       /* outer size, as window 1's */
static volatile LONG s_win2_hidden;
static volatile LONG s_split_on;               /* split screen: window 1 shows both, side by side */
#define WM_GPU_MAKE_WINDOW2 (WM_APP + 1)
static volatile LONG s_flipq[64];
static volatile LONG s_flipq_w, s_flipq_r;
static volatile LONG s_frame_wide;            /* present this frame 16:9, not 4:3 */
static int      s_rw = 640, s_rh = 480;       /* applied render size */
static float    s_sx = 1.0f, s_sy = 1.0f;     /* ... as scale factors from 640x480 */
static int      s_fullscreen_applied;
static RECT     s_windowed_rect;
static void   (*s_display_cb)(int fullscreen);
static ID3D11VertexShader *s_blit_vs;
static ID3D11PixelShader  *s_blit_ps;
static ID3D11RasterizerState *s_blit_rs;



/* Compile HLSL through a disk cache (ShaderCache\ beside the exe, keyed by a
 * hash of source + entry + profile). A compile costs 20-90 ms and happened on
 * the GPU thread the first time an effect appeared -- a visible hitch in
 * combat. Cached, each shader is compiled once, ever. */
static HRESULT gpu_compile(const char *src, const char *entry, const char *profile, ID3DBlob **out)
{
    static char dir[MAX_PATH];
    char path[MAX_PATH];
    uint64_t h = 1469598103934665603ull;
    const char *p;
    FILE *f;
    ID3DBlob *err = NULL;
    HRESULT hr;

    if (!dir[0]) {
        char *slash;
        GetModuleFileNameA(NULL, dir, MAX_PATH);
        slash = strrchr(dir, '\\');
        if (slash) strcpy_s(slash + 1, MAX_PATH - (size_t)(slash + 1 - dir), "ShaderCache");
        CreateDirectoryA(dir, NULL);
    }
    for (p = src; *p; p++) h = (h ^ (uint8_t)*p) * 1099511628211ull;
    for (p = entry; *p; p++) h = (h ^ (uint8_t)*p) * 1099511628211ull;
    for (p = profile; *p; p++) h = (h ^ (uint8_t)*p) * 1099511628211ull;
    sprintf_s(path, sizeof path, "%s\\%016llx.cso", dir, (unsigned long long)h);
    if (fopen_s(&f, path, "rb") == 0 && f) {
        long n;
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && SUCCEEDED(D3DCreateBlob((SIZE_T)n, out))
                && fread(ID3D10Blob_GetBufferPointer(*out), 1, (size_t)n, f) == (size_t)n) {
            fclose(f);
            return S_OK;
        }
        if (*out) { ID3D10Blob_Release(*out); *out = NULL; }
        fclose(f);
    }
    hr = D3DCompile(src, strlen(src), entry, NULL, NULL, entry, profile,
                    D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, out, &err);
    if (FAILED(hr)) {
        static int shown;
        if (shown++ < 3)
            fprintf(stderr, "  [GPU11] %s shader failed: %s\n%s\n", profile,
                    err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?", src);
        if (err) ID3D10Blob_Release(err);
        return hr;
    }
    if (err) ID3D10Blob_Release(err);
    if (fopen_s(&f, path, "wb") == 0 && f) {
        fwrite(ID3D10Blob_GetBufferPointer(*out), 1, ID3D10Blob_GetBufferSize(*out), f);
        fclose(f);
    }
    return hr;
}

/* Vertex pass-through, plus a fallback pixel shader (vertex colour) used
 * only if a generated combiner shader fails to compile. */
static const char *s_gpu_hlsl =
"struct V { float4 p:POSITION; float4 c0:COLOR0; float4 c1:COLOR1;\n"
"           float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
"           float4 fog:TEXCOORD4; };\n"
"struct P { float4 p:SV_Position; float4 c0:COLOR0; float4 c1:COLOR1;\n"
"           float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
"           float4 fog:TEXCOORD4; };\n"
"P vs(V v) { P o; o.p = v.p; o.c0 = v.c0; o.c1 = v.c1; o.t0 = v.t0; o.t1 = v.t1;\n"
"            o.t2 = v.t2; o.t3 = v.t3; o.fog = v.fog; return o; }\n"
"float4 ps(P i) : SV_Target { return i.c0; }\n";

/* ── window ────────────────────────────────────────────────────────────── */

static void gpu_create_window2(void);
static void gpu_window2_fullscreen(int fs);

static LRESULT CALLBACK gpu_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CLOSE) {
        fprintf(stderr, "[GPU11] window closed: exiting\n");
        fflush(stderr);
        ExitProcess(0);                     /* closing the game window quits */
        return 0;
    }
    if (m == WM_GPU_MAKE_WINDOW2) {
        gpu_create_window2();
        return 0;
    }
    if (m == WM_SIZE) {
        InterlockedExchange(h == s_gpu_hwnd2 && h ? &s_resize2_pending : &s_resize_pending, 1);
        return 0;
    }
    /* Alt+Enter or F11: windowed <-> borderless fullscreen. */
    if ((m == WM_SYSKEYDOWN && w == VK_RETURN && (l & (1 << 29))) || (m == WM_KEYDOWN && w == VK_F11)) {
        if (!(l & (1 << 30))) {                     /* not auto-repeat */
            InterlockedExchange(&s_opt_fullscreen, !s_opt_fullscreen);
            InterlockedExchange(&s_opt_dirty, 1);
            if (s_display_cb)
                s_display_cb((int)s_opt_fullscreen);
        }
        return 0;
    }
    if (m == WM_SYSCHAR && w == VK_RETURN)
        return 0;                                   /* no beep on Alt+Enter */
    return DefWindowProcA(h, m, w, l);
}

/* Render size (the game's 640x480 drawn at width x height), vsync on/off,
 * borderless fullscreen on/off. Safe from any thread. */
void nv2a_gpu_set_display(int width, int height, int vsync, int fullscreen)
{
    if (width < 640) width = 640;
    if (height < 480) height = 480;
    if (width > 7680) width = 7680;
    if (height > 4320) height = 4320;
    InterlockedExchange(&s_opt_w, width);
    InterlockedExchange(&s_opt_h, height);
    InterlockedExchange(&s_opt_vsync, vsync != 0);
    InterlockedExchange(&s_opt_fullscreen, fullscreen != 0);
    InterlockedExchange(&s_opt_dirty, 1);
}

/* Told when the player toggles fullscreen from the keyboard, to save it. */
void nv2a_gpu_set_display_callback(void (*cb)(int fullscreen))
{
    s_display_cb = cb;
}

/* Ask for the second window: at start-up (before the first draw creates the
 * device), or at any time later -- the window thread makes the window, and the
 * next flip gives it its swap chain. */
void nv2a_gpu_enable_second_window(void)
{
    InterlockedExchange(&s_want_win2, 1);
    if (s_gpu_hwnd && !s_gpu_hwnd2)
        PostMessageA(s_gpu_hwnd, WM_GPU_MAKE_WINDOW2, 0, 0);
}

int nv2a_gpu_has_second_window(void)
{
    return (s_swap2 != NULL && !s_win2_hidden) || (s_split_on && s_swap != NULL);
}

/* Split screen: player 1's frames (flip target 0) go to the left half of
 * window 1 and player 2's (target 1) to the right half, presented together;
 * frames for both (target 2) fill the window. */
void nv2a_gpu_set_split(int on)
{
    InterlockedExchange(&s_split_on, on != 0);
}

/* Width / height of one half of window 1, for the game's projection. */
float nv2a_gpu_split_half_aspect(void)
{
    RECT r;
    if (s_gpu_hwnd && GetClientRect(s_gpu_hwnd, &r) && r.bottom > r.top && r.right > r.left)
        return (float)(r.right - r.left) * 0.5f / (float)(r.bottom - r.top);
    return 8.0f / 9.0f;
}

/* Hide window 2 (player 2 left) or show it again. */
void nv2a_gpu_show_second_window(int show)
{
    InterlockedExchange(&s_win2_hidden, !show);
    if (s_gpu_hwnd2 && !getenv("BUFFY_NO_WINDOW")) {
        ShowWindowAsync(s_gpu_hwnd2, show ? SW_SHOWNA : SW_HIDE);
        if (show) {
            /* back on a screen, over the others */
            RECT r;
            if (GetWindowRect(s_gpu_hwnd2, &r) && !MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
                MONITORINFO mi;
                mi.cbSize = sizeof mi;
                GetMonitorInfoA(MonitorFromRect(&r, MONITOR_DEFAULTTOPRIMARY), &mi);
                SetWindowPos(s_gpu_hwnd2, HWND_TOP, mi.rcWork.left + 20, mi.rcWork.top + 20, 0, 0,
                             SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
            } else
                SetWindowPos(s_gpu_hwnd2, HWND_TOP, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        }
    }
    fprintf(stderr, "  [GPU11] player 2's window %s\n", show ? (s_gpu_hwnd2 ? "shown" : "to be made") : "hidden");
}

/* Where the next Swap's flip goes: 0 window 1, 1 window 2, 2 both. */
void nv2a_gpu_queue_flip(int target)
{
    LONG w = s_flipq_w;
    if (w - s_flipq_r >= 63)
        return;                                 /* no consumer yet: drop */
    s_flipq[w & 63] = target;
    InterlockedExchange(&s_flipq_w, w + 1);
}

static int gpu_flip_target(void)
{
    LONG r = s_flipq_r;
    int t;
    if (r == s_flipq_w)
        return s_swap2 && !s_win2_hidden ? 2 : 0;   /* not queued: both, when there are two */
    t = (int)s_flipq[r & 63];
    InterlockedExchange(&s_flipq_r, r + 1);
    return t;
}

/* Whether the frames being flipped are widescreen (anamorphic 16:9 in the
 * 640x480 surface) or 4:3 -- the title's own flag, changing with what is on
 * screen (menus and movies stay 4:3). */
void nv2a_gpu_set_frame_widescreen(int wide)
{
    InterlockedExchange(&s_frame_wide, wide != 0);
}

static DWORD WINAPI gpu_window_thread(LPVOID ready)
{
    WNDCLASSA wc;
    MSG msg;
    RECT r = { 0, 0, 1280, 960 }, wa;
    int cw = (int)s_opt_w, ch = (int)s_opt_h;

    /* Client area: the render size (at least 1280 wide), shrunk to fit the
     * desktop, same shape. */
    if (cw < 1280) { ch = ch * 1280 / cw; cw = 1280; }
    if (SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0)) {
        int mw = wa.right - wa.left - 20, mh = wa.bottom - wa.top - 60;
        if (cw > mw) { ch = ch * mw / cw; cw = mw; }
        if (ch > mh) { cw = cw * mh / ch; ch = mh; }
    }
    r.right = cw; r.bottom = ch;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = gpu_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "BuffyRecompGpu";
    RegisterClassA(&wc);
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    s_gpu_hwnd = CreateWindowA(wc.lpszClassName, "Buffy the Vampire Slayer: Chaos Bleeds",
                               WS_OVERLAPPEDWINDOW | (getenv("BUFFY_NO_WINDOW") ? 0 : WS_VISIBLE),
                               CW_USEDEFAULT, CW_USEDEFAULT,
                               r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    s_win2_w = r.right - r.left;
    s_win2_h = r.bottom - r.top;
    if (s_want_win2)
        gpu_create_window2();
    SetEvent((HANDLE)ready);
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}

/* Player 2's window: on the second monitor when there is one, else beside
 * the first window. On the window thread. */
static void gpu_create_window2(void)
{
    if (s_gpu_hwnd2 || !s_gpu_hwnd)
        return;
    {
        RECT r1;
        int x = CW_USEDEFAULT, y = CW_USEDEFAULT, n = GetSystemMetrics(SM_CMONITORS);
        GetWindowRect(s_gpu_hwnd, &r1);
        if (n >= 2) {
            POINT pt;
            HMONITOR m1 = MonitorFromWindow(s_gpu_hwnd, MONITOR_DEFAULTTOPRIMARY), m2 = NULL;
            MONITORINFO mi;
            /* the monitor to the right of (or else left of) the first */
            pt.x = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) - 10;
            pt.y = GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN) / 2;
            m2 = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
            if (m2 == m1) {
                pt.x = GetSystemMetrics(SM_XVIRTUALSCREEN) + 10;
                m2 = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
            }
            mi.cbSize = sizeof mi;
            if (m2 != m1 && GetMonitorInfoA(m2, &mi)) {
                x = mi.rcWork.left + 20;
                y = mi.rcWork.top + 20;
            }
        } else {
            /* one monitor: beside window 1, but on the screen (window 1 may
             * fill it) */
            MONITORINFO mi;
            x = r1.right;
            y = r1.top;
            mi.cbSize = sizeof mi;
            if (GetMonitorInfoA(MonitorFromWindow(s_gpu_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
                if (x + s_win2_w > mi.rcWork.right)
                    x = mi.rcWork.right - s_win2_w;
                if (x < mi.rcWork.left)
                    x = mi.rcWork.left;
                if (y + s_win2_h > mi.rcWork.bottom)
                    y = mi.rcWork.bottom - s_win2_h;
                if (y < mi.rcWork.top)
                    y = mi.rcWork.top;
            }
        }
        s_gpu_hwnd2 = CreateWindowA("BuffyRecompGpu", "Buffy the Vampire Slayer: Chaos Bleeds - Player 2",
                                    WS_OVERLAPPEDWINDOW | (getenv("BUFFY_NO_WINDOW") ? 0 : WS_VISIBLE),
                                    x, y, s_win2_w, s_win2_h, NULL, NULL, GetModuleHandleA(NULL), NULL);
    }
}

/* ── init ──────────────────────────────────────────────────────────────── */

static int gpu_init(void)
{
    const char *env = getenv("RECOMP_GPU");
    DXGI_SWAP_CHAIN_DESC sd;
    ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
    D3D11_BUFFER_DESC bd;
    D3D11_SAMPLER_DESC smp;
    HANDLE ready;
    HRESULT hr;
    int i;
    static const D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,   D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 64,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 80,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 96,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 4, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 112, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    if (env && env[0] == '0')
        return 0;

    ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, gpu_window_thread, ready, 0, NULL));
    WaitForSingleObject(ready, 5000);
    CloseHandle(ready);
    if (!s_gpu_hwnd)
        return 0;

    memset(&sd, 0, sizeof sd);
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;                   /* the window's client size */
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = s_gpu_hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                                       D3D11_SDK_VERSION, &sd, &s_swap, &s_dev, NULL, &s_ctx);
    if (FAILED(hr)) {
        fprintf(stderr, "  [GPU11] device creation failed 0x%08lX; software rasteriser\n", hr);
        return 0;
    }

    {
        /* DXGI's own Alt+Enter would switch to exclusive mode; the window
         * procedure does borderless fullscreen instead. */
        IDXGIFactory *fac = NULL;
        if (SUCCEEDED(IDXGISwapChain_GetParent(s_swap, &IID_IDXGIFactory, (void **)&fac))) {
            IDXGIFactory_MakeWindowAssociation(fac, s_gpu_hwnd, DXGI_MWA_NO_ALT_ENTER);
            IDXGIFactory_Release(fac);
        }
    }
    {
        /* Present: the flipped surface, filtered into a 4:3 rectangle of the
         * back buffer (one full-screen triangle). */
        static const char blit[] =
            "Texture2D t0 : register(t0); SamplerState s0 : register(s0);\n"
            "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
            "V vs(uint id : SV_VertexID) { V o; o.uv = float2((id << 1) & 2, id & 2);\n"
            "  o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
            "float4 ps(V i) : SV_Target { return float4(t0.Sample(s0, i.uv).rgb, 1); }\n";
        ID3DBlob *b = NULL;
        D3D11_RASTERIZER_DESC rd;
        if (SUCCEEDED(gpu_compile(blit, "vs", "vs_4_0", &b))) {
            ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s_blit_vs);
            ID3D10Blob_Release(b);
        }
        if (SUCCEEDED(gpu_compile(blit, "ps", "ps_4_0", &b))) {
            ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s_blit_ps);
            ID3D10Blob_Release(b);
        }
        memset(&rd, 0, sizeof rd);
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_blit_rs);
    }

    hr = D3DCompile(s_gpu_hlsl, strlen(s_gpu_hlsl), "nv2a", NULL, NULL, "vs", "vs_4_0", 0, 0, &vsb, &err);
    if (FAILED(hr)) goto shader_error;
    hr = D3DCompile(s_gpu_hlsl, strlen(s_gpu_hlsl), "nv2a", NULL, NULL, "ps", "ps_4_0", 0, 0, &psb, &err);
    if (FAILED(hr)) goto shader_error;
    ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &s_vs);
    ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &s_ps);
    ID3D11Device_CreateInputLayout(s_dev, il, 8, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &s_layout);
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);

    /* s_vb / s_gv, the CPU vertex-program path's buffers, are made on first
     * use (gpu_cpu_buffers): with the GPU path that is rare. */
    memset(&bd, 0, sizeof bd);
    /* Filled with UpdateSubresource, so DEFAULT usage (dynamic buffers
     * reject it and would stay all zeros). */
    bd.ByteWidth = 32 * 16;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.CPUAccessFlags = 0;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cb);

    memset(&smp, 0, sizeof smp);
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11Device_CreateSamplerState(s_dev, &smp, &s_samp[0]);
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ID3D11Device_CreateSamplerState(s_dev, &smp, &s_samp[1]);

    s_gi = (uint32_t *)malloc(sizeof(uint32_t) * GPU_MAX_IDX);
    bd.ByteWidth = sizeof(uint32_t) * GPU_MAX_IDX;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_ib);
    s_vsh_ok = !getenv("RECOMP_CPU_VP") && vsh_init();
    for (i = 0; i < GPU_MAX_SURF; i++)
        s_surf[i].used = 0;
    {
        /* Stats times count from process start. */
        FILETIME c, e, k, u, now;
        ULARGE_INTEGER a, b;
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
        GetSystemTimeAsFileTime(&now);
        a.LowPart = c.dwLowDateTime; a.HighPart = c.dwHighDateTime;
        b.LowPart = now.dwLowDateTime; b.HighPart = now.dwHighDateTime;
        s_gpu_t0 = GetTickCount() - (DWORD)((b.QuadPart - a.QuadPart) / 10000);
    }
    fprintf(stderr, "  [GPU11] Direct3D 11 backend up at t=%lus\n", (GetTickCount() - s_gpu_t0) / 1000);
    return s_gi != NULL;

shader_error:
    fprintf(stderr, "  [GPU11] shader compile failed: %s\n",
            err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
    return 0;
}

static int gpu_active(void)
{
    if (s_gpu_state < 0)
        s_gpu_state = gpu_init();
    return s_gpu_state;
}

/* ── surfaces ──────────────────────────────────────────────────────────── */

static GpuSurface *gpu_surface(uint32_t offset, uint32_t w, uint32_t h)
{
    int i, free_i = -1;
    D3D11_TEXTURE2D_DESC td;
    GpuSurface *s;

    if (!w || !h)
        return NULL;
    for (i = 0; i < GPU_MAX_SURF; i++) {
        if (s_surf[i].used && s_surf[i].offset == offset) {
            /* A smaller clip is the title drawing into part of the surface
             * (the HUD, for one) -- not a new surface. Rebuilding on every
             * size change wiped the 3D scene each frame. Only grow. */
            if (w <= s_surf[i].w && h <= s_surf[i].h)
                return &s_surf[i];
            if (s_surf[i].w > w) w = s_surf[i].w;
            if (s_surf[i].h > h) h = s_surf[i].h;
            free_i = i;                     /* grew: rebuild in place */
            break;
        }
        if (!s_surf[i].used && free_i < 0)
            free_i = i;
    }
    if (free_i < 0)
        free_i = 0;
    s = &s_surf[free_i];
    if (s->used) {
        if (s->rtv) ID3D11RenderTargetView_Release(s->rtv);
        if (s->srv) ID3D11ShaderResourceView_Release(s->srv);
        if (s->tex) ID3D11Texture2D_Release(s->tex);
        if (s->dsv) ID3D11DepthStencilView_Release(s->dsv);
        if (s->ztex) ID3D11Texture2D_Release(s->ztex);
    }
    memset(s, 0, sizeof *s);
    memset(&td, 0, sizeof td);
    td.Width = (UINT)(w * s_sx + 0.5f); td.Height = (UINT)(h * s_sy + 0.5f); td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s->tex)))
        return NULL;
    ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)s->tex, NULL, &s->rtv);
    ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s->tex, NULL, &s->srv);
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s->ztex);
    if (s->ztex)
        ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)s->ztex, NULL, &s->dsv);
    s->offset = offset; s->w = w; s->h = h; s->used = 1;
    s->pw = td.Width; s->ph = td.Height;
    if (getenv("RECOMP_PB_SLOW"))
        fprintf(stderr, "  [SURF] created %08X %ux%u in slot %d\n", offset, w, h, free_i);
    {
        float black[4] = { 0, 0, 0, 1 };
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s->rtv, black);
        if (s->dsv)
            ID3D11DeviceContext_ClearDepthStencilView(s_ctx, s->dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    }
    return s;
}

static GpuSurface *gpu_current_surface(void)
{
    return gpu_surface(s_gpu.color_offset, s_gpu.clip_x + s_gpu.clip_w, s_gpu.clip_y + s_gpu.clip_h);
}

/* CLEAR_SURFACE: bit 0 depth, bit 1 stencil, bits 4-7 colour channels. */
/* BUFFY_DRAWLOG=<frame> or t<seconds>: is the current frame the traced one? */
static int gpu_drawlog_frame(void)
{
    static int64_t target = -2;
    const char *e;
    if (target == -2) {
        e = getenv("BUFFY_DRAWLOG");
        target = !e ? -1 : (e[0] == 't' ? -3 - atoi(e + 1) : atoi(e));
    }
    if (target <= -3 && s_gpu_t0 && (int64_t)(GetTickCount() - s_gpu_t0) / 1000 >= -3 - target)
        target = s_frame_no + 1;              /* the next whole frame */
    return target >= 0 && s_frame_no == (uint32_t)target;
}

static void gpu_clear(uint32_t param)
{
    GpuSurface *s;
    if (!gpu_active())
        return;
    s = gpu_current_surface();
    if (!s)
        return;
    {
        static int want = -2;
        (void)want;
        if (gpu_drawlog_frame())
            fprintf(stderr, "  [DRAW] f%u clear %08X surf %08X after draw %u color %08X\n",
                    s_frame_no, param, s->offset, s_draw_in_frame, s_gpu.clear_color);
    }
    if (param & 0xF0) {
        uint32_t c = s_gpu.clear_color;
        float f[4] = { ((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f,
                       (c & 0xFF) / 255.0f, (c >> 24) / 255.0f };
        /* The clear value is in the surface's own pixel format
         * (SET_SURFACE_FORMAT colour field): the title clears its R5G6B5
         * shadow map with 0xFFFF, which is white, not ARGB cyan. */
        switch (s_kel[0x0208 / 4] & 0xF) {
        case 1: case 2:                          /* X1R5G5B5 */
            f[0] = ((c >> 10) & 31) / 31.0f; f[1] = ((c >> 5) & 31) / 31.0f;
            f[2] = (c & 31) / 31.0f; f[3] = 1.0f;
            break;
        case 3:                                  /* R5G6B5 */
            f[0] = ((c >> 11) & 31) / 31.0f; f[1] = ((c >> 5) & 63) / 63.0f;
            f[2] = (c & 31) / 31.0f; f[3] = 1.0f;
            break;
        case 9:                                  /* B8 */
            f[0] = f[1] = f[2] = f[3] = (c & 0xFF) / 255.0f;
            break;
        case 0xA:                                /* G8B8 */
            f[0] = 0.0f; f[1] = ((c >> 8) & 0xFF) / 255.0f;
            f[2] = (c & 0xFF) / 255.0f; f[3] = 1.0f;
            break;
        default:
            break;
        }
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s->rtv, f);
    }
    if ((param & 3) && s->dsv)
        ID3D11DeviceContext_ClearDepthStencilView(s_ctx, s->dsv,
            ((param & 1) ? D3D11_CLEAR_DEPTH : 0) | ((param & 2) ? D3D11_CLEAR_STENCIL : 0),
            (float)(s_kel[0x1D8C / 4] >> 8) / 16777215.0f, (UINT8)(s_kel[0x1D8C / 4] & 0xFF));
}

/* BUFFY_SHOTS=<dir>: every 3 s, the presented surface as a 32-bit BMP. */
/* A texture (the frame surface, or a back buffer) to dir\name as a BMP. */
static void gpu_save_bmp(ID3D11Texture2D *tex, uint32_t w, uint32_t h, const char *path)
{
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *st = NULL;
    D3D11_MAPPED_SUBRESOURCE map;
    FILE *f;
    uint32_t hdr[13], y;
    ID3D11Texture2D_GetDesc(tex, &td);
    if (w > td.Width) w = td.Width;
    if (h > td.Height) h = td.Height;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &st)))
        return;
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)st, (ID3D11Resource *)tex);
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &map))) {
        f = fopen(path, "wb");
        if (f) {
            memset(hdr, 0, sizeof hdr);
            fwrite("BM", 1, 2, f);
            hdr[0] = 54 + w * h * 4; hdr[2] = 54; hdr[3] = 40; hdr[4] = w;
            hdr[5] = (uint32_t)-(int32_t)h;
            hdr[6] = 1 | (32 << 16);
            fwrite(hdr, 4, 13, f);
            for (y = 0; y < h; y++)
                fwrite((const uint8_t *)map.pData + (size_t)y * map.RowPitch, 4, w, f);
            fclose(f);
        }
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
    }
    ID3D11Texture2D_Release(st);
}

/* Split screen: the composed window (both halves), as shotNNN_split.bmp. */
static int s_split_shot = -1;
static void gpu_screenshot_split(void)
{
    ID3D11Texture2D *bb = NULL;
    D3D11_TEXTURE2D_DESC bd;
    char path[MAX_PATH];
    const char *dir = getenv("BUFFY_SHOTS");
    if (!dir || s_split_shot < 0)
        return;
    if (SUCCEEDED(IDXGISwapChain_GetBuffer(s_swap, 0, &IID_ID3D11Texture2D, (void **)&bb))) {
        ID3D11Texture2D_GetDesc(bb, &bd);
        sprintf_s(path, sizeof path, "%s\\shot%03d_split.bmp", dir, s_split_shot);
        gpu_save_bmp(bb, bd.Width, bd.Height, path);
        ID3D11Texture2D_Release(bb);
    }
    s_split_shot = -1;
}

static void gpu_screenshot(GpuSurface *s, int window2)
{
    static DWORD last;
    static int n, want_p2 = -1;
    int idx;
    const char *dir = getenv("BUFFY_SHOTS");
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *st = NULL;
    D3D11_MAPPED_SUBRESOURCE map;
    char path[MAX_PATH];
    FILE *f;
    uint32_t hdr[13], y;

    if (!dir)
        return;
    if (window2) {
        /* Player 2's window: the frame right after each window-1 shot, as
         * shotNNN_p2.bmp. */
        if (want_p2 < 0)
            return;
        idx = want_p2;
        want_p2 = -1;
        if (s_split_on)
            s_split_shot = idx;
    } else {
        if (GetTickCount() - last < (DWORD)(getenv("BUFFY_SHOTS_MS") ? atoi(getenv("BUFFY_SHOTS_MS")) : 3000)
                || (getenv("BUFFY_SHOTS_FROM") && (GetTickCount() - s_gpu_t0) / 1000 < (DWORD)atoi(getenv("BUFFY_SHOTS_FROM"))))
            return;
        last = GetTickCount();
        idx = n++;
        if (s_swap2 || s_split_on)
            want_p2 = idx;
    }
    ID3D11Texture2D_GetDesc(s->tex, &td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &st)))
        return;
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)st, (ID3D11Resource *)s->tex);
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &map))) {
        sprintf_s(path, sizeof path, window2 ? "%s\\shot%03d_p2.bmp" : "%s\\shot%03d.bmp", dir, idx);
        f = fopen(path, "wb");
        if (getenv("BUFFY_SHOTS_LOG"))
            fprintf(stderr, "[SHOT] t=%lu %s\n", GetTickCount() / 100 % 100000, path);
        if (f) {
            memset(hdr, 0, sizeof hdr);
            fwrite("BM", 1, 2, f);
            hdr[0] = 54 + s->pw * s->ph * 4; hdr[2] = 54; hdr[3] = 40; hdr[4] = s->pw;
            hdr[5] = (uint32_t)-(int32_t)s->ph;
            hdr[6] = 1 | (32 << 16);
            fwrite(hdr, 4, 13, f);
            for (y = 0; y < s->ph; y++)
                fwrite((const uint8_t *)map.pData + (size_t)y * map.RowPitch, 4, s->pw, f);
            fclose(f);
        }
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
    }
    ID3D11Texture2D_Release(st);
}

/* The Xbox flips on vertical blank, and Xbox D3D keeps only a few frames
 * queued, so the GPU's flip rate is what paces the title. Flipping at once
 * let the menus run at 300-400 fps. Hold each flip to a 60 Hz slot
 * (BUFFY_UNCAPPED=1 turns it off). */
static void gpu_pace_frame(void)
{
    static int on = -1;
    static LARGE_INTEGER freq, next;
    LARGE_INTEGER now;
    if (on < 0) {
        /* The game thread is paced at D3DDevice_Swap now (port/src/buffy_frame.c);
         * pacing here as well only reintroduced uneven frame steps. */
        on = getenv("RECOMP_GPU_PACE") && getenv("RECOMP_GPU_PACE")[0] == '1';
        QueryPerformanceFrequency(&freq);
        timeBeginPeriod(1);
    }
    if (!on)
        return;
    QueryPerformanceCounter(&now);
    if (!next.QuadPart || now.QuadPart - next.QuadPart > freq.QuadPart / 10)
        next = now;                            /* first frame, or fell far behind */
    next.QuadPart += freq.QuadPart / 60;
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
}

/* Release every render target (the render scale changed); the next draws
 * recreate them at the new size. */
static void gpu_drop_surfaces(void)
{
    int i;
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
    for (i = 0; i < GPU_MAX_SURF; i++) {
        GpuSurface *s = &s_surf[i];
        if (!s->used)
            continue;
        if (s->rtv) ID3D11RenderTargetView_Release(s->rtv);
        if (s->srv) ID3D11ShaderResourceView_Release(s->srv);
        if (s->tex) ID3D11Texture2D_Release(s->tex);
        if (s->dsv) ID3D11DepthStencilView_Release(s->dsv);
        if (s->ztex) ID3D11Texture2D_Release(s->ztex);
        memset(s, 0, sizeof *s);
    }
}

/* Apply requested display options (GPU thread, between frames). */
static void gpu_apply_display(void)
{
    int fs;
    if (!InterlockedExchange(&s_opt_dirty, 0))
        return;
    if ((int)s_opt_w != s_rw || (int)s_opt_h != s_rh) {
        s_rw = (int)s_opt_w;
        s_rh = (int)s_opt_h;
        s_sx = (float)s_rw / 640.0f;
        s_sy = (float)s_rh / 480.0f;
        gpu_drop_surfaces();
        fprintf(stderr, "  [GPU11] render size %dx%d\n", s_rw, s_rh);
    }
    fs = (int)s_opt_fullscreen;
    if (fs != s_fullscreen_applied && !getenv("BUFFY_NO_WINDOW")) {
        s_fullscreen_applied = fs;
        if (fs) {
            MONITORINFO mi;
            mi.cbSize = sizeof mi;
            GetWindowRect(s_gpu_hwnd, &s_windowed_rect);
            GetMonitorInfoA(MonitorFromWindow(s_gpu_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
            SetWindowLongPtrA(s_gpu_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowPos(s_gpu_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        } else {
            SetWindowLongPtrA(s_gpu_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
            SetWindowPos(s_gpu_hwnd, HWND_NOTOPMOST, s_windowed_rect.left, s_windowed_rect.top,
                         s_windowed_rect.right - s_windowed_rect.left,
                         s_windowed_rect.bottom - s_windowed_rect.top,
                         SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        }
        InterlockedExchange(&s_resize_pending, 1);
        gpu_window2_fullscreen(fs);            /* on its own monitor */
    }
}

/* Window 2 to borderless fullscreen on its own monitor, or back. */
static void gpu_window2_fullscreen(int fs)
{
    if (!s_gpu_hwnd2 || getenv("BUFFY_NO_WINDOW"))
        return;
    if (fs) {
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        GetWindowRect(s_gpu_hwnd2, &s_windowed_rect2);
        GetMonitorInfoA(MonitorFromWindow(s_gpu_hwnd2, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrA(s_gpu_hwnd2, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(s_gpu_hwnd2, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongPtrA(s_gpu_hwnd2, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(s_gpu_hwnd2, HWND_NOTOPMOST, s_windowed_rect2.left, s_windowed_rect2.top,
                     s_windowed_rect2.right - s_windowed_rect2.left,
                     s_windowed_rect2.bottom - s_windowed_rect2.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    }
    InterlockedExchange(&s_resize2_pending, 1);
}

/* Window 2's swap chain, once its window exists (render thread). */
static void gpu_make_swap2(void)
{
    IDXGIFactory *fac = NULL;
    DXGI_SWAP_CHAIN_DESC sd;
    if (s_swap2 || !s_gpu_hwnd2 || !s_swap)
        return;
    if (FAILED(IDXGISwapChain_GetDesc(s_swap, &sd)))
        return;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.OutputWindow = s_gpu_hwnd2;
    sd.Windowed = TRUE;
    if (SUCCEEDED(IDXGISwapChain_GetParent(s_swap, &IID_IDXGIFactory, (void **)&fac))) {
        if (SUCCEEDED(IDXGIFactory_CreateSwapChain(fac, (IUnknown *)s_dev, &sd, &s_swap2))) {
            IDXGIFactory_MakeWindowAssociation(fac, s_gpu_hwnd2, DXGI_MWA_NO_ALT_ENTER);
            fprintf(stderr, "  [GPU11] second window up (player 2)\n");
            if (s_fullscreen_applied)
                gpu_window2_fullscreen(1);
        }
        IDXGIFactory_Release(fac);
    }
}

/* Draw surface `s` into a swap chain's back buffer, 4:3 (or 16:9) and centred. */
static void gpu_blit_region(GpuSurface *s, IDXGISwapChain *swap, volatile LONG *resize, int half);

static void gpu_blit_to(GpuSurface *s, IDXGISwapChain *swap, volatile LONG *resize)
{
    gpu_blit_region(s, swap, resize, 0);
}

/* half: 0 the whole back buffer (4:3 or 16:9, centred, cleared around);
 * 1 / 2 the left / right half, filled (split screen; the game drew the frame
 * for that shape). The left half clears the buffer; the right does not. */
static void gpu_blit_region(GpuSurface *s, IDXGISwapChain *swap, volatile LONG *resize, int half)
{
    ID3D11Texture2D *bb = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    D3D11_TEXTURE2D_DESC bd;
    D3D11_VIEWPORT vp;
    float black[4] = { 0, 0, 0, 1 };
    ID3D11ShaderResourceView *none = NULL;

    if (InterlockedExchange(resize, 0)) {
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
        IDXGISwapChain_ResizeBuffers(swap, 0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
    }
    if (FAILED(IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&bb)))
        return;
    ID3D11Texture2D_GetDesc(bb, &bd);
    ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)bb, NULL, &rtv);
    ID3D11Texture2D_Release(bb);
    if (!rtv)
        return;
    if (half != 2)
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx, rtv, black);
    if (s_blit_vs && s_blit_ps) {
        /* The part of the surface the game shows (640x480 of it), at 4:3 or,
         * for a widescreen frame, 16:9 -- letterboxed or pillarboxed to the
         * window. */
        float aspect = s_frame_wide ? 16.0f / 9.0f : 4.0f / 3.0f;
        float bw = (float)bd.Width, bh = (float)bd.Height, w = bw, h = bw / aspect;
        float fw = s->w > 640 ? 640.0f / (float)s->w : 1.0f, fh = s->h > 480 ? 480.0f / (float)s->h : 1.0f;
        if (h > bh) { h = bh; w = bh * aspect; }
        vp.TopLeftX = (float)(int)((bw - w) * 0.5f);
        vp.TopLeftY = (float)(int)((bh - h) * 0.5f);
        /* A surface larger than 640x480: stretch the viewport so only the
         * top-left 640x480 lands inside the 4:3 rectangle. */
        vp.Width = w / fw; vp.Height = h / fh;
        if (half) {
            vp.TopLeftX = half == 1 ? 0.0f : (float)(int)(bw * 0.5f);
            vp.TopLeftY = 0.0f;
            vp.Width = bw * 0.5f / fw;
            vp.Height = bh / fh;
        }
        vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &rtv, NULL);
        ID3D11DeviceContext_OMSetBlendState(s_ctx, NULL, NULL, 0xFFFFFFFFu);
        ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, NULL, 0);
        ID3D11DeviceContext_RSSetState(s_ctx, s_blit_rs);
        ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
        ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
        ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(s_ctx, s_blit_vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(s_ctx, s_blit_ps, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &s->srv);
        ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &s_samp[1]);
        ID3D11DeviceContext_Draw(s_ctx, 3, 0);
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &none);
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
    }
    ID3D11RenderTargetView_Release(rtv);
}

/* Flip: show the surface at this physical offset. */
static void gpu_present(uint32_t offset)
{
    int i;
    if (s_gpu_state != 1)
        return;
    for (i = 0; i < GPU_MAX_SURF; i++) {
        if (s_surf[i].used && s_surf[i].offset == offset) {
            GpuSurface *shown = &s_surf[i];
            int target;
            if (s_gpu_hwnd2 && !s_swap2)
                gpu_make_swap2();
            target = gpu_flip_target();
            if (s_split_on && target != 2) {
                /* split screen: left half, then the right half and present */
                gpu_blit_region(shown, s_swap, &s_resize_pending, target == 0 ? 1 : 2);
                if (target == 1) {
                    gpu_screenshot(shown, 1);          /* marks the composed shot below */
                    gpu_screenshot_split();
                    IDXGISwapChain_Present(s_swap, s_opt_vsync ? 1 : 0, 0);
                }
            } else if (target != 1 || !s_swap2 || s_win2_hidden) {
                gpu_blit_to(shown, s_swap, &s_resize_pending);
                IDXGISwapChain_Present(s_swap, s_opt_vsync ? 1 : 0, 0);
            }
            if (s_swap2 && !s_win2_hidden && target != 0 && !s_split_on) {
                gpu_blit_to(shown, s_swap2, &s_resize2_pending);
                IDXGISwapChain_Present(s_swap2, 0, 0);   /* one vsync wait a frame: window 1's;
                                                        * the compositor keeps this tear-free */
            }
            gpu_screenshot(shown, target == 1);
            gpu_apply_display();          /* may drop every surface, s_surf[i] too */
            gpu_pace_frame();
            s_frame_no++;
            s_draw_in_frame = 0;
            {
                /* Frames per second in the title bar. */
                static DWORD t0;
                static unsigned frames;
                frames++;
                if (GetTickCount() - t0 >= 1000) {
                    char title[128];
                    sprintf_s(title, sizeof title,
                              "Buffy the Vampire Slayer: Chaos Bleeds  -  %u fps", frames);
                    SetWindowTextA(s_gpu_hwnd, title);
                    if (getenv("RECOMP_VP_STATS"))
                        fprintf(stderr, "  [GPU11] %u fps at t=%lus\n", frames, (GetTickCount() - s_gpu_t0) / 1000);
                    {
                        /* BUFFY_PEEK=va,va,...: guest dwords (hex and as float) once a second. */
                        const char *pk = getenv("BUFFY_PEEK");
                        const uint8_t *pmem = (const uint8_t *)xbox_GetMemoryOffset();
                        while (pk && *pk) {
                            char *end;
                            unsigned long va = strtoul(pk, &end, 16);
                            if (end == pk) break;
                            fprintf(stderr, "  [PEEK] %08lX = %08X (%g)\n", va, *(const uint32_t *)(pmem + va),
                                    *(const float *)(pmem + va));
                            if (*end != ',') break;
                            pk = end + 1;
                        }
                    }
                    frames = 0;
                    t0 = GetTickCount();
                }
            }
            {
                static DWORD last;
                if (getenv("RECOMP_VP_STATS") && GetTickCount() - last > 5000) {
                    last = GetTickCount();
                    fprintf(stderr, "  [GPU11] tex: disabled %llu nooff %llu badsize %llu rt %llu cached %llu new %llu fail %llu tiny %llu | last st0 ctl0 %08X fmt %08X off %08X\n",
                            s_tx_why[0], s_tx_why[1], s_tx_why[2], s_tx_why[3], s_tx_why[4], s_tx_why[5], s_tx_why[6], s_tx_why[7],
                            s_tx_last_ctl0, s_tx_last_fmt, s_tx_last_off);
                    memset(s_tx_why, 0, sizeof s_tx_why);
                    fprintf(stderr, "  [GPU11] shaders %d | stage modes", s_ps_count);
                    for (i = 0; i < 32; i++)
                        if (s_stage_mode_seen[i])
                            fprintf(stderr, " %d:%llu", i, s_stage_mode_seen[i]);
                    fprintf(stderr, "\n");
                }
            }
            return;
        }
    }
    {
        static int shown;
        if (shown++ < 10) {
            fprintf(stderr, "  [GPU11] flip to %08X: no such surface (have", offset);
            for (i = 0; i < GPU_MAX_SURF; i++)
                if (s_surf[i].used) fprintf(stderr, " %08X", s_surf[i].offset);
            fprintf(stderr, ")\n");
        }
    }
}

/* ── textures ──────────────────────────────────────────────────────────── */

static uint32_t tex_reg(int stage, uint32_t method)
{
    return s_tex_reg[(method + stage * 0x40 - NV_TEX_FIRST) / 4];
}

static uint32_t gpu_tex_hash(const uint8_t *p, size_t bytes)
{
    uint32_t h = 2166136261u;
    size_t step = bytes / 64 + 1, i;
    for (i = 0; i + 4 <= bytes; i += step * 4)
        h = (h ^ *(const uint32_t *)(p + i)) * 16777619u;
    return h;
}

#include "nv2a_pb_psh.inc"
#include "nv2a_pb_vsh.inc"

/* Bytes of one mip level. */
static uint32_t tx_level_bytes(uint32_t fmt, uint32_t block, uint32_t w, uint32_t h)
{
    if (block)
        return ((w + 3) / 4) * ((h + 3) / 4) * block;
    return w * h * tx_bpp(fmt);
}

#include "nv2a_texpack.inc"

/* A shader resource for texture stage `st`, or NULL. *norm receives the
 * coordinate multiplier (1 for normalised, 1/size for linear textures, which
 * NV2A addresses in texels); *mips whether it has a mip chain; *cube whether
 * it is a cube map (bind at t4+st). */
static ID3D11ShaderResourceView *gpu_texture(int st, float norm[2], int *mips, int *cube)
{
    uint32_t ctl0 = tex_reg(st, 0x1B0C), fmtreg = tex_reg(st, 0x1B04);
    uint32_t off = tex_reg(st, 0x1B00), fmt = (fmtreg >> 8) & 0xFF;
    uint32_t w, h, pitch = tex_reg(st, 0x1B10) >> 16, va, hash, bytes, levels, faces, l, f;
    uint32_t palreg = tex_reg(st, 0x1B20), face_bytes;
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint32_t *pal = NULL;
    int i, lru = 0, linear, is_cube;
    DWORD now = GetTickCount();
    GpuTexture *t;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sub[6 * 13];
    uint32_t *pix[6 * 13];
    int npix = 0;
    uint32_t block = d3d8_format_dxt_block_bytes(fmt);
    LARGE_INTEGER tx_t0;
    uint64_t tp_hash = 0;
    int tp_want_dump = 0;

    norm[0] = norm[1] = 1.0f;
    *mips = 0;
    *cube = 0;
    if (st == 0) { s_tx_last_ctl0 = ctl0; s_tx_last_fmt = fmtreg; s_tx_last_off = off; }
    if (!(ctl0 & 0x40000000u)) { s_tx_why[0]++; return NULL; }
    if (!off) { s_tx_why[1]++; return NULL; }
    linear = !block && !tx_swizzled(fmt);
    is_cube = (fmtreg & 4) != 0 && !linear;
    if (linear) {
        uint32_t rect = tex_reg(st, 0x1B1C);
        w = rect >> 16; h = rect & 0xFFFF;
    } else {
        w = 1u << ((fmtreg >> 20) & 0xF);
        h = 1u << ((fmtreg >> 24) & 0xF);
    }
    if (!w || !h || w > 4096 || h > 4096) { s_tx_why[2]++; return NULL; }
    if (linear) { norm[0] = 1.0f / (float)w; norm[1] = 1.0f / (float)h; }

    /* Rendered to earlier? Sample the target. */
    for (i = 0; i < GPU_MAX_SURF; i++)
        if (s_surf[i].used && s_surf[i].offset == off) { s_tx_why[3]++; return s_surf[i].srv; }

    levels = linear ? 1 : (fmtreg >> 16) & 0xF;
    if (!levels) levels = 1;
    for (l = 1; l < levels && ((w >> l) || (h >> l)); l++) ;
    levels = l;
    if (block && (w < 4 || h < 4))
        levels = 1;
    face_bytes = 0;
    for (l = 0; l < levels; l++) {
        uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
        face_bytes += linear ? pitch * h : tx_level_bytes(fmt, block, lw, lh);
    }
    faces = is_cube ? 6 : 1;
    if (is_cube)
        face_bytes = (face_bytes + 127) & ~127u;
    bytes = face_bytes * faces;
    if (!bytes || bytes > 64u << 20) { s_tx_why[2]++; return NULL; }

    va = dma_resolve(off);

    /* Index by identity; verify contents only once per frame. Searching 512
     * entries and re-hashing texture memory for every stage of every draw
     * was ~90 us a draw -- a fifth of the frame in gameplay. */
    {
        uint32_t key = (off * 2654435761u) ^ (fmt * 40503u) ^ (w << 16) ^ h ^ (levels << 28)
                     ^ (is_cube ? 0x80000000u : 0);
        uint32_t fmtk = fmt | (is_cube ? 0x100u : 0);
        uint32_t slot = (key ^ (key >> 13)) & (GPU_TEX_INDEX - 1);
        int idx = s_texidx[slot] - 1;
        if (idx >= 0) {
            t = &s_texc[idx];
            if (t->srv && t->offset == off && t->fmt == fmtk && t->w == w && t->h == h && t->levels == levels) {
                if (t->checked_frame != s_frame_no) {
                    hash = gpu_tex_hash(mem + va, bytes);
                    if (fmt == 0x0B) {
                        uint32_t n = 256u >> ((palreg >> 2) & 3);
                        pal = (const uint32_t *)(mem + dma_resolve(palreg & ~0x3Fu));
                        hash ^= gpu_tex_hash((const uint8_t *)pal, n * 4) * 31u;
                    }
                    if (t->hash != hash) {
                        i = idx;                  /* same texture, new contents: re-upload */
                        goto reupload;
                    }
                    t->checked_frame = s_frame_no;
                }
                t->last_use = now;
                s_tx_why[4]++;
                *mips = t->replaced ? 2 : levels > 1;
                *cube = is_cube;
                return t->srv;
            }
        }
        hash = gpu_tex_hash(mem + va, bytes);
        if (fmt == 0x0B) {
            uint32_t n = 256u >> ((palreg >> 2) & 3);
            pal = (const uint32_t *)(mem + dma_resolve(palreg & ~0x3Fu));
            hash ^= gpu_tex_hash((const uint8_t *)pal, n * 4) * 31u;
        }
        for (i = 0; i < GPU_MAX_TEX; i++) {
            if (!s_texc[i].srv) { lru = i; break; }
            if (s_texc[i].last_use < s_texc[lru].last_use)
                lru = i;
        }
        i = lru;
        s_texidx[slot] = i + 1;
    }
reupload:
    if (fmt == 0x0B && !pal)
        pal = (const uint32_t *)(mem + dma_resolve(palreg & ~0x3Fu));
    t = &s_texc[i];
    if (t->srv) { ID3D11ShaderResourceView_Release(t->srv); t->srv = NULL; }
    if (t->tex) { ID3D11Texture2D_Release(t->tex); t->tex = NULL; }
    t->replaced = 0;

    /* Texture packs: a replacement from load\, or this one dumped below. */
    if (!s_tp.inited)
        tp_init();
    if (tp_active() && !is_cube) {
        tp_hash = tp_texture_hash(mem + va, fmt, block, w, h, linear, pitch, pal,
                                  fmt == 0x0B ? 256u >> ((palreg >> 2) & 3) : 0);
        if (tp_replacement(tp_hash, &t->tex, &t->srv)) {
            s_tx_why[5]++;
            t->offset = off; t->fmt = fmt; t->w = w; t->h = h; t->pitch = pitch;
            t->levels = levels; t->hash = hash; t->last_use = now; t->checked_frame = s_frame_no;
            t->replaced = 1;
            *mips = 2;
            *cube = 0;
            return t->srv;
        }
        tp_want_dump = s_tp.dump && !tp_dumped(tp_hash, 0);
    }

    QueryPerformanceCounter(&tx_t0);
    memset(&td, 0, sizeof td);
    td.Width = w; td.Height = h; td.MipLevels = levels; td.ArraySize = faces;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = is_cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (block && w >= 4 && h >= 4)
        td.Format = block == 8 ? DXGI_FORMAT_BC1_UNORM : (fmt == 0x0E ? DXGI_FORMAT_BC2_UNORM : DXGI_FORMAT_BC3_UNORM);

    for (f = 0; f < faces; f++) {
        uint32_t lo = va + f * face_bytes;
        for (l = 0; l < levels; l++) {
            uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
            D3D11_SUBRESOURCE_DATA *sd = &sub[f * levels + l];
            if (td.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
                sd->pSysMem = mem + lo;
                sd->SysMemPitch = ((lw + 3) / 4) * block;
            } else {
                uint32_t *px = (uint32_t *)malloc((size_t)lw * lh * 4);
                if (!px)
                    goto out;
                pix[npix++] = px;
                if (block) {
                    uint32_t x, y;
                    for (y = 0; y < lh; y++)
                        for (x = 0; x < lw; x++)
                            if (!d3d8_dxt_decode_texel(mem + lo, fmt, x, y, lw, &px[y * lw + x]))
                                px[y * lw + x] = 0xFFFF00FFu;
                } else {
                    tx_decode_level(fmt, mem + lo, lw, lh, linear ? pitch : 0, pal, px);
                }
                sd->pSysMem = px;
                sd->SysMemPitch = lw * 4;
            }
            sd->SysMemSlicePitch = 0;
            lo += linear ? pitch * h : tx_level_bytes(fmt, block, lw, lh);
        }
    }
    if (tp_want_dump) {
        if (td.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
            tp_dump(tp_hash, fmt, w, h, pix[0]);          /* face 0, level 0 */
        } else {
            uint32_t *px = (uint32_t *)malloc((size_t)w * h * 4), x, y;
            if (px) {
                for (y = 0; y < h; y++)
                    for (x = 0; x < w; x++)
                        if (!d3d8_dxt_decode_texel(mem + va, fmt, x, y, w, &px[y * w + x]))
                            px[y * w + x] = 0xFFFF00FFu;
                tp_dump(tp_hash, fmt, w, h, px);
                free(px);
            }
        }
    }
    ID3D11Device_CreateTexture2D(s_dev, &td, sub, &t->tex);
    {
        static LARGE_INTEGER f;
        LARGE_INTEGER b;
        if (!f.QuadPart) QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&b);
        if (getenv("RECOMP_PB_PROFILE") && (b.QuadPart - tx_t0.QuadPart) * 1000 / f.QuadPart >= 2)
            fprintf(stderr, "  [HITCH] texture %ux%u fmt %02X x%u levels built in %.1f ms\n", w, h, fmt, levels,
                    (b.QuadPart - tx_t0.QuadPart) * 1000.0 / f.QuadPart);
    }
out:
    while (npix)
        free(pix[--npix]);
    if (!t->tex) { s_tx_why[6]++; return NULL; }
    s_tx_why[5]++;
    ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)t->tex, NULL, &t->srv);
    t->offset = off; t->fmt = fmt | (is_cube ? 0x100u : 0); t->w = w; t->h = h; t->pitch = pitch;
    t->levels = levels; t->hash = hash; t->last_use = now; t->checked_frame = s_frame_no;
    *mips = levels > 1;
    *cube = is_cube;
    return t->srv;
}

/* ── state ─────────────────────────────────────────────────────────────── */

static D3D11_BLEND gpu_blend(uint32_t f)
{
    switch (f) {
    case 0x0000: return D3D11_BLEND_ZERO;
    case 0x0001: return D3D11_BLEND_ONE;
    case 0x0300: return D3D11_BLEND_SRC_COLOR;
    case 0x0301: return D3D11_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D11_BLEND_SRC_ALPHA;
    case 0x0303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D11_BLEND_DEST_ALPHA;
    case 0x0305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x0306: return D3D11_BLEND_DEST_COLOR;
    case 0x0307: return D3D11_BLEND_INV_DEST_COLOR;
    case 0x0308: return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: return D3D11_BLEND_BLEND_FACTOR;     /* CONSTANT_COLOR */
    case 0x8002: return D3D11_BLEND_INV_BLEND_FACTOR;
    default:     return D3D11_BLEND_ONE;
    }
}

static D3D11_COMPARISON_FUNC gpu_cmp(uint32_t f)
{
    return (D3D11_COMPARISON_FUNC)((f & 7) + 1);     /* GL 0x200..0x207 -> D3D11 1..8 */
}

static D3D11_STENCIL_OP gpu_stencil_op(uint32_t op)
{
    switch (op) {
    case 0x0000: return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default:     return D3D11_STENCIL_OP_KEEP;     /* 0x1E00 */
    }
}

static void gpu_apply_state(void)
{
    static ID3D11BlendState *bcache[64]; static uint32_t bkey[64];
    static ID3D11RasterizerState *rs;
    uint32_t k;
    int i;

    /* blend */
    k = (s_rs_blend ? 1u : 0u) | (s_rs_sfactor & 0xFFFF) << 1 | (s_rs_dfactor & 0x7FFF) << 17;
    k ^= (s_rs_color_mask & 0x01010101u) * 3u;
    for (i = 0; i < 64 && bcache[i] && bkey[i] != k; i++) ;
    if (i < 64 && !bcache[i]) {
        D3D11_BLEND_DESC d;
        memset(&d, 0, sizeof d);
        d.RenderTarget[0].BlendEnable = s_rs_blend != 0;
        d.RenderTarget[0].SrcBlend = gpu_blend(s_rs_sfactor);
        d.RenderTarget[0].DestBlend = gpu_blend(s_rs_dfactor);
        d.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        d.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        d.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        d.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        d.RenderTarget[0].RenderTargetWriteMask =
            ((s_rs_color_mask & 0x00010000u) ? D3D11_COLOR_WRITE_ENABLE_RED : 0)
          | ((s_rs_color_mask & 0x00000100u) ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0)
          | ((s_rs_color_mask & 0x00000001u) ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0)
          | ((s_rs_color_mask & 0x01000000u) ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0);
        ID3D11Device_CreateBlendState(s_dev, &d, &bcache[i]);
        bkey[i] = k;
    }
    if (i < 64)
        ID3D11DeviceContext_OMSetBlendState(s_ctx, bcache[i], NULL, 0xFFFFFFFFu);

    /* depth + stencil. The title masks its title-screen scene with the
     * stencil buffer, so both must be honoured together. */
    {
        uint32_t st_en = KEL(0x032C), st_wmask = KEL(0x0360) & 0xFF, st_func = KEL(0x0364);
        uint32_t st_ref = KEL(0x0368) & 0xFF, st_rmask = KEL(0x036C) & 0xFF;
        uint32_t op_f = KEL(0x0370), op_zf = KEL(0x0374), op_zp = KEL(0x0378);
        uint32_t key[4];
        static ID3D11DepthStencilState *dss[128];
        static uint32_t dssk[128][4];
        static int nd;
        key[0] = (s_rs_depth_test ? 1u : 0u) | (s_rs_depth_func & 7) << 1 | (s_rs_depth_mask ? 16u : 0u)
               | (st_en ? 32u : 0u) | (st_func & 7) << 6 | st_wmask << 9 | st_rmask << 17;
        key[1] = op_f; key[2] = op_zf; key[3] = op_zp;
        if (!st_en) key[1] = key[2] = key[3] = 0, key[0] &= 0x3Fu;
        for (i = 0; i < nd && memcmp(dssk[i], key, sizeof key); i++) ;
        if (i == nd && nd < 128) {
            D3D11_DEPTH_STENCIL_DESC d;
            memset(&d, 0, sizeof d);
            d.DepthEnable = s_rs_depth_test != 0 && !getenv("BUFFY_NODEPTH");
            d.DepthWriteMask = s_rs_depth_mask ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
            d.DepthFunc = gpu_cmp(s_rs_depth_func);
            if (!d.DepthEnable) {
                /* D3D11 drops depth writes with the test; NV2A too. */
                d.DepthFunc = D3D11_COMPARISON_ALWAYS;
            }
            d.StencilEnable = st_en != 0;
            d.StencilReadMask = (UINT8)st_rmask;
            d.StencilWriteMask = (UINT8)st_wmask;
            d.FrontFace.StencilFunc = gpu_cmp(st_func);
            d.FrontFace.StencilFailOp = gpu_stencil_op(op_f);
            d.FrontFace.StencilDepthFailOp = gpu_stencil_op(op_zf);
            d.FrontFace.StencilPassOp = gpu_stencil_op(op_zp);
            d.BackFace = d.FrontFace;
            ID3D11Device_CreateDepthStencilState(s_dev, &d, &dss[nd]);
            memcpy(dssk[nd], key, sizeof key);
            nd++;
        }
        if (i < nd)
            ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, dss[i], st_ref);
    }

    if (!rs) {
        D3D11_RASTERIZER_DESC d;
        memset(&d, 0, sizeof d);
        d.FillMode = D3D11_FILL_SOLID;
        d.CullMode = D3D11_CULL_NONE;
        d.DepthClipEnable = FALSE;             /* NV2A clamps depth, it does not clip it */
        ID3D11Device_CreateRasterizerState(s_dev, &d, &rs);
    }
    ID3D11DeviceContext_RSSetState(s_ctx, rs);
}

/* ── vertices ──────────────────────────────────────────────────────────── */

/* Fog factor from the program's oFog.x (FOG_MODE / FOG_PARAMS, as xemu). */
static float gpu_fog_factor(float d)
{
    float p0, p1, f;
    uint32_t mode;
    if (!KEL(0x02A4))                         /* SET_FOG_ENABLE */
        return 1.0f;
    mode = KEL(0x029C);
    memcpy(&p0, &KEL(0x09C0), 4);
    memcpy(&p1, &KEL(0x09C4), 4);
    if (mode >= 0x803 && mode <= 0x806)       /* *_ABS modes */
        d = fabsf(d);
    switch (mode) {
    case 0x800: case 0x803:                   /* EXP */
        f = p0 + exp2f(d * p1 * 16.0f) - 1.0f;
        break;
    case 0x801: case 0x804:                   /* EXP2 */
        f = p0 + exp2f(-d * d * p1 * p1 * 32.0f) - 1.0f;
        break;
    default:                                  /* LINEAR */
        f = p0 + d * p1 - 1.0f;
        break;
    }
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

static void gpu_vertex(uint32_t index, GpuVertex *g, float sw, float sh)
{
    const VpVert *v = vp_vertex(index);
    const float *sc = s_vp_const[58], *of = s_vp_const[59];
    float w = v->rhw != 0.0f ? 1.0f / v->rhw : 1.0f;
    int k;

    /* Depth as NV2A computes it: W itself when W-buffering (CONTROL0 bit 16,
     * which this title uses), else the screen z; either mapped linearly
     * through SET_CLIP_MIN/MAX. Z from the viewport constants alone packs the
     * whole scene into the last few units below 2^24 and loses ordering. */
    {
        float cmin, cmax, zd = (KEL(0x0290) & 0x10000) ? w : v->z, zn;
        memcpy(&cmin, &KEL(0x0394), 4);
        memcpy(&cmax, &KEL(0x0398), 4);
        if (cmax > cmin)
            zn = (zd - cmin) / (cmax - cmin);
        else
            zn = sc[2] != 0.0f ? (v->z - of[2]) / sc[2] : 0.5f;
        if (s_vp_uses_viewport && sc[0] != 0.0f && sc[1] != 0.0f) {
            g->pos[0] = (v->x - of[0]) / sc[0] * w;
            g->pos[1] = (v->y - of[1]) / sc[1] * w;
            g->pos[2] = zn * w;
            g->pos[3] = w;
        } else {
            /* Screen-space (pass-through) vertices: pixels over the surface. */
            g->pos[0] = v->x / (sw * 0.5f) - 1.0f;
            g->pos[1] = 1.0f - v->y / (sh * 0.5f);
            g->pos[2] = zn < 0.0f ? 0.0f : (zn > 1.0f ? 1.0f : zn);
            g->pos[3] = 1.0f;
        }
    }
    for (k = 0; k < 4; k++) {
        g->c0[k] = v->col[k];
        g->c1[k] = v->spec[k];
    }
    memcpy(g->t, v->tex, sizeof g->t);
    g->fog[0] = gpu_fog_factor(v->fog);
    g->fog[1] = g->fog[2] = 0.0f;
    g->fog[3] = 1.0f;
}

/* The CPU vertex path's host array and vertex ring, made on first use. */
static int gpu_cpu_buffers(void)
{
    D3D11_BUFFER_DESC bd;
    if (s_vb && s_gv)
        return 1;
    if (!s_gv)
        s_gv = (GpuVertex *)malloc(sizeof(GpuVertex) * GPU_MAX_VERTS);
    if (!s_vb) {
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = sizeof(GpuVertex) * GPU_MAX_VERTS;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vb);
    }
    return s_vb && s_gv;
}

static uint32_t gpu_vidx(uint32_t index, float sw, float sh)
{
    uint32_t h = (index * 2654435761u) >> 20;       /* 12 bits */
    if (s_vmap_gen[h] == s_vmap_g && s_vmap_key[h] == index)
        return s_vmap_slot[h];
    if (s_nv >= GPU_MAX_VERTS) {
        static int warned;
        if (!warned++)
            fprintf(stderr, "  [GPU11] a batch has more than %d vertices; the rest are dropped\n", GPU_MAX_VERTS);
        return 0;                                   /* batch too large: degenerate */
    }
    if (s_use_gvp)
        vsh_input(index, &s_gin[s_nv]);
    else
        gpu_vertex(index, &s_gv[s_nv], sw, sh);
    s_vmap_gen[h] = s_vmap_g;
    s_vmap_key[h] = index;
    s_vmap_slot[h] = s_nv;
    return s_nv++;
}

static void gpu_argb(uint32_t c, float *o)
{
    o[0] = ((c >> 16) & 0xFF) / 255.0f;
    o[1] = ((c >> 8) & 0xFF) / 255.0f;
    o[2] = (c & 0xFF) / 255.0f;
    o[3] = (c >> 24) / 255.0f;
}

static void gpu_draw_batch(void)
{
    GpuSurface *surf = gpu_current_surface();
    D3D11_MAPPED_SUBRESOURCE map;
    uint32_t n = 0, i, st;
    UINT stride = sizeof(GpuVertex), offs = 0;
    float sw, sh;
    D3D11_VIEWPORT vp;
    float cb[128];
    ID3D11ShaderResourceView *srv[8];
    ID3D11SamplerState *smp[4];
    ID3D11PixelShader *ps;
    uint32_t bound = 0, cubes = 0;
    UINT vb_first = 0, ib_first = 0;
    ID3D11VertexShader *vsh;
    LARGE_INTEGER q0, q1, q2, q3;
    QueryPerformanceCounter(&q0);

    if (!surf)
        return;
    sw = (float)surf->w;
    sh = (float)surf->h;

    s_vmap_g++;
    s_nv = 0;
    /* Vertex program on the GPU unless tracing this frame or unavailable. */
    vsh = NULL;
    s_use_gvp = 0;
    if (s_vsh_ok && s_vp_batch && !gpu_drawlog_frame()) {
        vsh = vsh_get();
        s_use_gvp = vsh != NULL;
    }
    if (!s_use_gvp && !gpu_cpu_buffers())
        return;
#define GV(i_) (s_gi[n++] = gpu_vidx(s_gpu.idx[i_], sw, sh))
    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < s_gpu.idx_count && n + 3 <= GPU_MAX_IDX; i += 3) { GV(i); GV(i + 1); GV(i + 2); }
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < s_gpu.idx_count && n + 3 <= GPU_MAX_IDX; i++) { GV(i); GV(i + 1); GV(i + 2); }
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < s_gpu.idx_count && n + 3 <= GPU_MAX_IDX; i++) { GV(0); GV(i); GV(i + 1); }
        break;
    case NV_PRIM_QUADS:
        for (i = 0; i + 3 < s_gpu.idx_count && n + 6 <= GPU_MAX_IDX; i += 4) {
            GV(i); GV(i + 1); GV(i + 2); GV(i); GV(i + 2); GV(i + 3);
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        for (i = 0; i + 3 < s_gpu.idx_count && n + 6 <= GPU_MAX_IDX; i += 2) {
            GV(i); GV(i + 1); GV(i + 3); GV(i); GV(i + 3); GV(i + 2);
        }
        break;
    default:
        break;
    }
#undef GV
    QueryPerformanceCounter(&q1);
    if (!n)
        return;

    /* BUFFY_SKIPDRAW=a,b,..: drop those draws of every frame (debugging). */
    {
        static char skip[256];
        static int init;
        const char *e;
        uint32_t d = s_draw_in_frame++;
        if (!init) {
            init = 1;
            e = getenv("BUFFY_SKIPDRAW");
            while (e && *e) {
                int a = atoi(e), b = a;
                const char *dash = strchr(e, '-'), *comma = strchr(e, ',');
                if (dash && (!comma || dash < comma)) b = atoi(dash + 1);
                for (; a <= b && a < 256; a++) if (a >= 0) skip[a] = 1;
                e = comma ? comma + 1 : NULL;
            }
        }
        if (d < 256 && skip[d])
            return;
    }
    /* Ring buffers: append with NO_OVERWRITE, discard only on wrap. A
     * DISCARD per draw made the driver rename the whole 8 MB buffer every
     * time -- tens of microseconds, hundreds of thousands of times a second. */
    {
        static UINT vb_pos, ib_pos;
        D3D11_MAP how = D3D11_MAP_WRITE_NO_OVERWRITE;
        static UINT gin_pos;
        if (s_use_gvp) {
            if (gin_pos + s_nv > GPU_MAX_VERTS) {
                gin_pos = 0;
                how = D3D11_MAP_WRITE_DISCARD;
            }
            if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vs_vb, 0, how, 0, &map)))
                return;
            memcpy((GpuIn *)map.pData + gin_pos, s_gin, sizeof(GpuIn) * s_nv);
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vs_vb, 0);
            vb_first = gin_pos;
            gin_pos += s_nv;
        } else {
            if (vb_pos + s_nv > GPU_MAX_VERTS) {
                vb_pos = 0;
                how = D3D11_MAP_WRITE_DISCARD;
            }
            if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vb, 0, how, 0, &map)))
                return;
            memcpy((GpuVertex *)map.pData + vb_pos, s_gv, sizeof(GpuVertex) * s_nv);
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vb, 0);
            vb_first = vb_pos;
            vb_pos += s_nv;
        }

        how = D3D11_MAP_WRITE_NO_OVERWRITE;
        if (ib_pos + n > GPU_MAX_IDX) {
            ib_pos = 0;
            how = D3D11_MAP_WRITE_DISCARD;
        }
        if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_ib, 0, how, 0, &map)))
            return;
        memcpy((uint32_t *)map.pData + ib_pos, s_gi, sizeof(uint32_t) * n);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_ib, 0);
        ib_first = ib_pos;
        ib_pos += n;
    }

    /* Viewport: the title's own, when the program applied one. */
    memset(&vp, 0, sizeof vp);
    if (s_vp_uses_viewport && s_vp_const[58][0] != 0.0f) {
        float hx = fabsf(s_vp_const[58][0]), hy = fabsf(s_vp_const[58][1]);
        vp.TopLeftX = s_vp_const[59][0] - hx;
        vp.TopLeftY = s_vp_const[59][1] - hy;
        vp.Width = 2.0f * hx;
        vp.Height = 2.0f * hy;
    } else {
        vp.Width = sw;
        vp.Height = sh;
    }
    vp.TopLeftX *= s_sx; vp.TopLeftY *= s_sy;
    vp.Width *= s_sx;    vp.Height *= s_sy;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;

    memset(cb, 0, sizeof cb);
    cb[0] = (float)s_rs_alpha_ref;
    cb[1] = (float)(s_rs_alpha_func & 7);
    cb[2] = s_rs_alpha_test ? 1.0f : 0.0f;
    {
        uint32_t fc = KEL(0x02A8);            /* SET_FOG_COLOR: ABGR */
        cb[4] = (fc & 0xFF) / 255.0f;
        cb[5] = ((fc >> 8) & 0xFF) / 255.0f;
        cb[6] = ((fc >> 16) & 0xFF) / 255.0f;
        cb[7] = (fc >> 24) / 255.0f;
    }
    for (st = 0; st < 8; st++) {
        gpu_argb(KEL(M_FACTOR0 + 4 * st), &cb[24 + st * 4]);
        gpu_argb(KEL(M_FACTOR1 + 4 * st), &cb[56 + st * 4]);
    }
    gpu_argb(KEL(M_SPECFOG_FAC), &cb[88]);
    gpu_argb(KEL(M_SPECFOG_FAC + 4), &cb[92]);
    for (st = 0; st < 4; st++) {
        float norm[2];
        int mips, cube;
        ID3D11ShaderResourceView *v = gpu_texture((int)st, norm, &mips, &cube);
        srv[st] = srv[st + 4] = NULL;
        if (v) {
            srv[st + (cube ? 4 : 0)] = v;
            bound |= 1u << st;
            if (cube) cubes |= 1u << st;
        }
        smp[st] = v ? tx_sampler((int)st, mips) : s_samp[0];
        cb[8 + st * 4] = norm[0];
        cb[9 + st * 4] = norm[1];
        /* Bump env matrix and luminance: stage registers +0x28..+0x3C. */
        memcpy(&cb[96 + st * 4], &s_tex_reg[(0x1B28 + st * 0x40 - NV_TEX_FIRST) / 4], 16);
        memcpy(&cb[112 + st * 4], &s_tex_reg[(0x1B38 + st * 0x40 - NV_TEX_FIRST) / 4], 8);
    }
    ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s_cb, 0, NULL, cb, 0, 0);
    ps = psh_get(bound, cubes);
    QueryPerformanceCounter(&q2);

    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &surf->rtv, surf->dsv);
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    gpu_apply_state();
    if (s_use_gvp) {
        UINT gstride = sizeof(GpuIn);
        vsh_constants(sw, sh);
        ID3D11DeviceContext_IASetInputLayout(s_ctx, s_vs_layout);
        ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vs_vb, &gstride, &offs);
        ID3D11DeviceContext_VSSetShader(s_ctx, vsh, NULL, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 1, 1, &s_vs_cb);
    } else {
        ID3D11DeviceContext_IASetInputLayout(s_ctx, s_layout);
        ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vb, &stride, &offs);
        ID3D11DeviceContext_VSSetShader(s_ctx, s_vs, NULL, 0);
    }
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetIndexBuffer(s_ctx, s_ib, DXGI_FORMAT_R32_UINT, 0);
    ID3D11DeviceContext_PSSetShader(s_ctx, ps ? ps : s_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 8, srv);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 4, smp);
    ID3D11DeviceContext_DrawIndexed(s_ctx, n, ib_first, (INT)vb_first);
    QueryPerformanceCounter(&q3);
    {
        /* RECOMP_PB_PROFILE: where a draw's host time goes. */
        static LONGLONG t_vp, t_state, t_d3d, verts, draws;
        static DWORD last;
        t_vp += q1.QuadPart - q0.QuadPart;
        t_state += q2.QuadPart - q1.QuadPart;
        t_d3d += q3.QuadPart - q2.QuadPart;
        verts += n; draws++;
        if (GetTickCount() - last > 10000) {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            if (last && getenv("RECOMP_PB_PROFILE"))
                fprintf(stderr, "  [DRAWPROF] 10s: vertices %.0f ms, textures/state %.0f ms, d3d %.0f ms | %lld draws %lld verts\n",
                        t_vp * 1000.0 / f.QuadPart, t_state * 1000.0 / f.QuadPart, t_d3d * 1000.0 / f.QuadPart,
                        draws, verts);
            t_vp = t_state = t_d3d = verts = draws = 0;
            last = GetTickCount();
        }
    }
    {
        ID3D11ShaderResourceView *none[8] = { NULL };
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 8, none);
    }
    s_gpu.tris_drawn += n / 3;

    /* BUFFY_DRAWLOG=<frame>: describe every draw of that presented frame. */
    {
        static int want = -2;
        static int drawno;
        static uint32_t lastframe;
        (void)want;
        if (gpu_drawlog_frame()) {
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, u0 = 1e9f, u1 = -1e9f, zlo = 1e9f, zhi = -1e9f;
            int pi = -1;
            if (lastframe != s_frame_no) {
                drawno = 0; lastframe = s_frame_no;
                fprintf(stderr, "  [DRAW] control0 %08X surf_fmt %08X zclear %08X c58 (%g %g %g %g) c59 (%g %g %g %g) zminmax %08X clip %g..%g\n",
                        KEL(0x0290), KEL(0x0208), KEL(0x1D8C),
                        s_vp_const[58][0], s_vp_const[58][1], s_vp_const[58][2], s_vp_const[58][3],
                        s_vp_const[59][0], s_vp_const[59][1], s_vp_const[59][2], s_vp_const[59][3], KEL(0x1D78), *(float *)&KEL(0x0394), *(float *)&KEL(0x0398));
            }
            for (i = 0; i < s_nv; i++) {
                float w = s_gv[i].pos[3] != 0.0f ? s_gv[i].pos[3] : 1.0f;
                float x = (s_gv[i].pos[0] / w * 0.5f + 0.5f) * vp.Width + vp.TopLeftX;
                float y = (0.5f - s_gv[i].pos[1] / w * 0.5f) * vp.Height + vp.TopLeftY;
                if (x < x0) x0 = x; if (x > x1) x1 = x;
                if (y < y0) y0 = y; if (y > y1) y1 = y;
                if (s_gv[i].pos[2] / w < zlo) zlo = s_gv[i].pos[2] / w;
                if (s_gv[i].pos[2] / w > zhi) zhi = s_gv[i].pos[2] / w;
                if (s_gv[i].t[0][0] < u0) u0 = s_gv[i].t[0][0];
                if (s_gv[i].t[0][0] > u1) u1 = s_gv[i].t[0][0];
            }
            for (i = 0; i < (uint32_t)s_ps_count; i++)
                if (s_ps_cache[i].ps == ps) pi = (int)i;
            fprintf(stderr, "  [DRAW] f%u #%d prim %u n %u surf %08X ps %d prog %05X blend %u %X %X at %u/%X/%u z %u/%X/%u"
                            " zn %.5f..%.5f box (%.0f,%.0f)-(%.0f,%.0f) u %.2f..%.2f c0 %.2f %.2f %.2f %.2f w0 %.3f\n",
                    s_frame_no, drawno++, s_gpu.prim, n, surf->offset, pi, KEL(M_STAGE_PROG) & 0xFFFFF,
                    s_rs_blend, s_rs_sfactor, s_rs_dfactor, s_rs_alpha_test, s_rs_alpha_func, s_rs_alpha_ref,
                    s_rs_depth_test, s_rs_depth_func, s_rs_depth_mask,
                    zlo, zhi, x0, y0, x1, y1, u0, u1, s_gv[0].c0[0], s_gv[0].c0[1], s_gv[0].c0[2], s_gv[0].c0[3], s_gv[0].pos[3]);
            fprintf(stderr, "         stencil en %u func %X ref %u rmask %X wmask %X ops %X/%X/%X colmask %08X ctl0 %08X\n",
                    KEL(0x032C), KEL(0x0364), KEL(0x0368), KEL(0x036C), KEL(0x0360),
                    KEL(0x0370), KEL(0x0374), KEL(0x0378), s_rs_color_mask, KEL(0x0290));
            {
                uint32_t a, k2;
                fprintf(stderr, "         attrs:");
                for (a = 0; a < 16; a++)
                    if (s_gpu.attr[a].size)
                        fprintf(stderr, " %u:t%u s%u st%u @%08X", a, s_gpu.attr[a].type, s_gpu.attr[a].size,
                                s_gpu.attr[a].stride, s_gpu.attr[a].offset);
                fprintf(stderr, " | idx %u inline %d vp %d len %d start %u\n", s_gpu.idx_count,
                        s_gpu.inline_active, s_vp_batch, s_vp_prog.length, s_vp_start);
                if (getenv("BUFFY_VPDIS")) {
                    int k3, j3;
                    static const char *rt[] = { "r", "v", "c", "?" };
                    for (k3 = 0; k3 < s_vp_prog.length; k3++) {
                        const NV2AVshInstruction *q = &s_vp_prog.insns[k3];
                        fprintf(stderr, "           vp%d mac %d -> r%d o%d m%X | ilu %d -> r%d o%d m%X | src", k3,
                                q->mac_op, q->mac_dst.temp_reg, q->mac_dst.output_reg, q->mac_dst.write_mask,
                                q->ilu_op, q->ilu_dst.temp_reg, q->ilu_dst.output_reg, q->ilu_dst.write_mask);
                        for (j3 = 0; j3 < 3; j3++)
                            fprintf(stderr, " %s%s%d%s", q->mac_src[j3].negate ? "-" : "",
                                    rt[q->mac_src[j3].reg_type & 3], q->mac_src[j3].reg_index,
                                    q->mac_src[j3].rel_addr ? "[a0]" : "");
                        fprintf(stderr, " ilu %s%d\n", rt[q->ilu_src.reg_type & 3], q->ilu_src.reg_index);
                    }
                }
                if (s_gpu.inline_active && getenv("BUFFY_DRAWIDX")) {
                    uint32_t q;
                    fprintf(stderr, "         inline %u dw:", s_gpu.inline_count);
                    for (q = 0; q < s_gpu.inline_count && q < 28; q++)
                        fprintf(stderr, " %08X", ((const uint32_t *)s_gpu.inline_buf)[q]);
                    fprintf(stderr, "\n");
                    if (s_vp_prog.length == 15 || getenv("BUFFY_VPDIS")) {
                        int c, k3, j3;
                        static const char *rt[] = { "?", "r", "v", "c" };
                        for (k3 = 0; k3 < s_vp_prog.length; k3++) {
                            const NV2AVshInstruction *q = &s_vp_prog.insns[k3];
                            fprintf(stderr, "           insn %d mac %d -> t%d o%d m%X | ilu %d -> t%d o%d m%X | src", k3,
                                    q->mac_op, q->mac_dst.temp_reg, q->mac_dst.output_reg, q->mac_dst.write_mask,
                                    q->ilu_op, q->ilu_dst.temp_reg, q->ilu_dst.output_reg, q->ilu_dst.write_mask);
                            for (j3 = 0; j3 < 3; j3++)
                                fprintf(stderr, " %s%s%d%s", q->mac_src[j3].negate ? "-" : "",
                                        rt[q->mac_src[j3].reg_type & 3], q->mac_src[j3].reg_index,
                                        q->mac_src[j3].rel_addr ? "[a0]" : "");
                            fprintf(stderr, " ilu %s%d\n", rt[q->ilu_src.reg_type & 3], q->ilu_src.reg_index);
                        }
                        for (c = 0; c < 192; c++)
                            if (s_vp_const[c][0] != 0 || s_vp_const[c][1] != 0 || s_vp_const[c][2] != 0 || s_vp_const[c][3] != 0)
                                fprintf(stderr, "           c[%d] = %g %g %g %g\n", c, s_vp_const[c][0], s_vp_const[c][1], s_vp_const[c][2], s_vp_const[c][3]);
                    }
                }
                if (getenv("BUFFY_DRAWIDX")) {
                    fprintf(stderr, "         idx:");
                    for (k2 = 0; k2 < s_gpu.idx_count && k2 < 64; k2++) {
                        const VpVert *q = vp_vertex(s_gpu.idx[k2]);
                        fprintf(stderr, " %u(%.0f,%.0f)", s_gpu.idx[k2], q->x, q->y);
                    }
                    fprintf(stderr, "\n");
                }
                for (k2 = 0; k2 < 4 && k2 < s_gpu.idx_count; k2++) {
                    float in0[4], in1[4];
                    const VpVert *vv = vp_vertex(s_gpu.idx[k2]);
                    fetch_attr(&s_gpu.attr[0], s_gpu.idx[k2], in0);
                    fetch_attr(&s_gpu.attr[1], s_gpu.idx[k2], in1);
                    fprintf(stderr, "         v[%u]=%u in0 (%.3f %.3f %.3f %.3f) in1 (%.3f %.3f %.3f %.3f) -> (%.1f %.1f %.4f rhw %.3g)\n",
                            k2, s_gpu.idx[k2], in0[0], in0[1], in0[2], in0[3], in1[0], in1[1], in1[2], in1[3],
                            vv->x, vv->y, vv->z, vv->rhw);
                    {
                        float in3[4];
                        fetch_attr(&s_gpu.attr[3], s_gpu.idx[k2], in3);
                        fprintf(stderr, "           in3 (%.3f %.3f %.3f %.3f) -> oT0 (%.3f %.3f %.3f %.3f) oT1 (%.3f %.3f %.3f %.3f)\n",
                                in3[0], in3[1], in3[2], in3[3], vv->tex[0][0], vv->tex[0][1], vv->tex[0][2], vv->tex[0][3],
                                vv->tex[1][0], vv->tex[1][1], vv->tex[1][2], vv->tex[1][3]);
                    }
                }
            }
            for (st = 0; st < 4; st++)
                if (bound & (1u << st))
                    fprintf(stderr, "         st%u off %08X fmt %08X addr %08X filt %08X rect %08X\n", st,
                            tex_reg(st, 0x1B00), tex_reg(st, 0x1B04), tex_reg(st, 0x1B08),
                            tex_reg(st, 0x1B14), tex_reg(st, 0x1B1C));
        }
    }
}

#else
static int  gpu_active(void) { return 0; }
static void gpu_clear(uint32_t p) { (void)p; }
static void gpu_present(uint32_t o) { (void)o; }
void nv2a_gpu_set_display(int w, int h, int vsync, int fullscreen) { (void)w; (void)h; (void)vsync; (void)fullscreen; }
void nv2a_gpu_set_display_callback(void (*cb)(int)) { (void)cb; }
void nv2a_gpu_set_frame_widescreen(int wide) { (void)wide; }
void nv2a_gpu_enable_second_window(void) {}
int  nv2a_gpu_has_second_window(void) { return 0; }
void nv2a_gpu_set_split(int on) { (void)on; }
float nv2a_gpu_split_half_aspect(void) { return 8.0f / 9.0f; }
void nv2a_gpu_show_second_window(int show) { (void)show; }
void nv2a_gpu_queue_flip(int target) { (void)target; }
static void gpu_draw_batch(void) {}
#endif
