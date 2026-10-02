/*
 * The FPS counter and the debug overlay, drawn over window 1's picture just
 * before it is presented (so screenshots and bug-report images leave it out).
 * Included by nv2a_pb_d3d11.inc.
 *
 *   mode 1  a small counter: frames per second
 *   mode 2  a panel: frames per second, frame time (average, worst), a graph
 *           of the last two seconds of frame times, the renderer, the size
 *           drawn at, VSync, and the graphics card
 *
 * The panel is drawn on the CPU a few times a second -- text with GDI
 * (antialiased, on black: each pixel is the text colour times its coverage),
 * the graph and the rounded background by hand -- into a premultiplied BGRA
 * texture, then blended over the back buffer with a viewport the panel's
 * size (the present's full-screen triangle, a pixel shader of its own).
 * Sizes follow the window's height, so it stays sharp at any resolution.
 */
#pragma comment(lib, "gdi32.lib")

static volatile LONG s_ov_mode;            /* 0 off, 1 FPS, 2 debug panel */

void nv2a_gpu_set_overlay(int mode)
{
    InterlockedExchange(&s_ov_mode, mode < 0 ? 0 : mode > 2 ? 2 : mode);
}

/* A short message over the picture (both windows), low in the middle, for
 * `ms` milliseconds: it takes the counter's / panel's place meanwhile. */
static SRWLOCK  s_toast_lock = SRWLOCK_INIT;
static char     s_toast[160];
static volatile LONG s_toast_seq;
static LONG     s_toast_painted = -1;
static int      s_ov_painted_mode;

void nv2a_gpu_toast(const char *text, int ms)
{
    AcquireSRWLockExclusive(&s_toast_lock);
    strncpy_s(s_toast, sizeof s_toast, text ? text : "", _TRUNCATE);
    ReleaseSRWLockExclusive(&s_toast_lock);
    s_toast_until = GetTickCount() + (DWORD)ms;
    InterlockedIncrement(&s_toast_seq);
}

/* The FPS limit (nv2a_gpu_set_frame_limit): the panel's target line and what
 * its FPS colour is measured against. */
static volatile LONG s_ov_limit = 60;

void nv2a_gpu_set_frame_limit(int fps)
{
    InterlockedExchange(&s_ov_limit, fps < 1 ? 60 : fps);
}

#define OV_HIST 120                         /* frame times kept for the graph */
static float    s_ov_ft[OV_HIST];           /* milliseconds, oldest first after s_ov_pos */
static int      s_ov_pos, s_ov_have;
static LARGE_INTEGER s_ov_freq, s_ov_last;
static double   s_ov_fps;                   /* over the last half second */
static int      s_ov_frames;
static LARGE_INTEGER s_ov_fps_t0;
static DWORD    s_ov_drawn_at;              /* when the panel's picture was last made */

static GpuTexture    *s_ov_tex;
static GpuShader     *s_ov_ps;
static GpuBlendState *s_ov_bs;
static int      s_ov_tw, s_ov_th;           /* the texture's size */
#if defined(_WIN32) /* GDI text: the overlay is Windows-only for now */
static HDC      s_ov_dc;
static HBITMAP  s_ov_bmp;
static uint32_t *s_ov_px;                   /* the DIB's pixels, top-down BGRA */
static int      s_ov_bw, s_ov_bh;
static HFONT    s_ov_big, s_ov_small, s_ov_label;
static int      s_ov_font_scale;            /* the scale the fonts were made for (x100) */
#endif
static char     s_ov_gpu[128];

/* Once a frame shown in window 1: the time since the last one. */
static void gpu_overlay_tick(void)
{
    LARGE_INTEGER now;
    if (!s_ov_freq.QuadPart)
        QueryPerformanceFrequency(&s_ov_freq);
    QueryPerformanceCounter(&now);
    if (s_ov_last.QuadPart) {
        float ms = (float)((double)(now.QuadPart - s_ov_last.QuadPart) * 1000.0 / (double)s_ov_freq.QuadPart);
        if (ms > 1000.0f)
            ms = 1000.0f;
        s_ov_ft[s_ov_pos] = ms;
        s_ov_pos = (s_ov_pos + 1) % OV_HIST;
        if (s_ov_have < OV_HIST)
            s_ov_have++;
    }
    s_ov_last = now;
    if (!s_ov_fps_t0.QuadPart)
        s_ov_fps_t0 = now;
    s_ov_frames++;
    if (now.QuadPart - s_ov_fps_t0.QuadPart >= s_ov_freq.QuadPart / 2) {
        s_ov_fps = (double)s_ov_frames * (double)s_ov_freq.QuadPart / (double)(now.QuadPart - s_ov_fps_t0.QuadPart);
        s_ov_frames = 0;
        s_ov_fps_t0 = now;
    }
}

static void ov_gpu_name(void)
{
    if (!s_ov_gpu[0])
        strcpy_s(s_ov_gpu, sizeof s_ov_gpu, gpu_adapter_name()[0] ? gpu_adapter_name() : "?");
}

#if defined(_WIN32) /* guard */
static int ov_init(void)
{
    static const char src[] =
        "Texture2D t0 : register(t0); SamplerState s0 : register(s0);\n"
        "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
        "float4 ps(V i) : SV_Target { return t0.Sample(s0, i.uv); }\n";
    GpuBlendDesc bd;
    if (s_ov_ps)
        return 1;
    if (!s_blit_vs || !(s_ov_ps = gpu_shader_create(GPU_PS, src, "ps")))
        return 0;
    memset(&bd, 0, sizeof bd);
    bd.enable = 1;
    bd.src = GPU_BLEND_ONE;                                     /* premultiplied */
    bd.dst = GPU_BLEND_INV_SRC_ALPHA;
    bd.op = GPU_BLENDOP_ADD;
    bd.src_alpha = GPU_BLEND_ONE;
    bd.dst_alpha = GPU_BLEND_INV_SRC_ALPHA;
    bd.op_alpha = GPU_BLENDOP_ADD;
    bd.write_mask = GPU_WRITE_ALL;
    s_ov_bs = gpu_blend_create(&bd);
    ov_gpu_name();
    return s_ov_ps && s_ov_bs;
}

/* The DIB to draw into, w x h, cleared to black. */
static int ov_canvas(int w, int h)
{
    if (w <= 0 || h <= 0 || w > 4096 || h > 2048)
        return 0;                                   /* (never a runaway size) */
    if (!s_ov_dc) {
        s_ov_dc = CreateCompatibleDC(NULL);
        if (!s_ov_dc)
            return 0;
        SetBkMode(s_ov_dc, TRANSPARENT);
    }
    if (getenv("BUFFY_OVERLAY_LOG")) {
        static int n;
        if (n++ < 20)
            fprintf(stderr, "[OVERLAY] canvas %d x %d (was %d x %d)\n", w, h, s_ov_bw, s_ov_bh);
    }
    if (w != s_ov_bw || h != s_ov_bh) {
        BITMAPINFO bi;
        void *bits = NULL;
        if (s_ov_bmp)
            DeleteObject(s_ov_bmp);
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;                             /* top-down */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        s_ov_bmp = CreateDIBSection(s_ov_dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (!s_ov_bmp)
            return 0;
        SelectObject(s_ov_dc, s_ov_bmp);
        s_ov_px = (uint32_t *)bits;
        s_ov_bw = w;
        s_ov_bh = h;
    }
    GdiFlush();
    memset(s_ov_px, 0, (size_t)w * h * 4);
    return 1;
}

static HFONT ov_font(int px, int weight)
{
    return CreateFontA(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                       CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
}

static void ov_fonts(float s)
{
    int k = (int)(s * 100.0f + 0.5f);
    if (k == s_ov_font_scale && s_ov_big)
        return;
    if (s_ov_big) DeleteObject(s_ov_big);
    if (s_ov_small) DeleteObject(s_ov_small);
    if (s_ov_label) DeleteObject(s_ov_label);
    s_ov_big = ov_font((int)(30.0f * s), FW_SEMIBOLD);
    s_ov_small = ov_font((int)(15.0f * s), FW_NORMAL);
    s_ov_label = ov_font((int)(13.0f * s), FW_SEMIBOLD);
    s_ov_font_scale = k;
}

static void ov_text(HFONT f, COLORREF c, int x, int y, const char *t)
{
    SelectObject(s_ov_dc, f);
    SetTextColor(s_ov_dc, c);
    TextOutA(s_ov_dc, x, y, t, (int)strlen(t));
}

static int ov_text_w(HFONT f, const char *t)
{
    SIZE sz = { 0, 0 };
    /* (the first paint measures before ov_canvas has made the DC: without
     * one the size came back as garbage -- a 9-million-pixel canvas) */
    if (!s_ov_dc && (s_ov_dc = CreateCompatibleDC(NULL)) != NULL)
        SetBkMode(s_ov_dc, TRANSPARENT);
    if (!s_ov_dc)
        return 0;
    SelectObject(s_ov_dc, f);
    if (!GetTextExtentPoint32A(s_ov_dc, t, (int)strlen(t), &sz))
        return 0;
    return sz.cx;
}

static void ov_rect(int x0, int y0, int x1, int y1, uint32_t bgr)
{
    int x, y;
    for (y = y0 < 0 ? 0 : y0; y < y1 && y < s_ov_bh; y++)
        for (x = x0 < 0 ? 0 : x0; x < x1 && x < s_ov_bw; x++)
            s_ov_px[y * s_ov_bw + x] = bgr;
}

static COLORREF ov_fps_colour(double fps, int cap)
{
    if (fps >= cap * 0.92) return RGB(112, 230, 140);           /* at the cap */
    if (fps >= cap * 0.6)  return RGB(255, 206, 84);
    return RGB(255, 106, 92);
}

#define OV_WELL 0x00010203u                 /* marks the graph's background before composing */

/* Premultiplied panel: a rounded, translucent dark card behind what was
 * drawn (black = nothing drawn; a drawn pixel's strongest channel is its
 * coverage). */
static void ov_compose(float s)
{
    const float bg_r = 13.0f, bg_g = 15.0f, bg_b = 21.0f, bg_a = 0.80f;
    float rad = 12.0f * s;
    int x, y, w = s_ov_bw, h = s_ov_bh;
    GdiFlush();
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            uint32_t p = s_ov_px[y * w + x];
            float r = (float)((p >> 16) & 255), g = (float)((p >> 8) & 255), b = (float)(p & 255);
            if (p == OV_WELL) {
                s_ov_px[y * w + x] = 0xEB0D0F14u;              /* the graph's well: darker, nearly opaque */
                continue;
            }
            float cov = (r > g ? (r > b ? r : b) : (g > b ? g : b)) / 255.0f;
            /* inside the rounded rectangle, with a soft edge */
            float cx = x + 0.5f < rad ? rad - (x + 0.5f) : x + 0.5f > w - rad ? (x + 0.5f) - (w - rad) : 0.0f;
            float cy = y + 0.5f < rad ? rad - (y + 0.5f) : y + 0.5f > h - rad ? (y + 0.5f) - (h - rad) : 0.0f;
            float d = sqrtf(cx * cx + cy * cy) - rad, inside = d < -0.5f ? 1.0f : d > 0.5f ? 0.0f : 0.5f - d;
            float a = bg_a * inside, border = (d > -1.5f * s && d <= 0.5f) ? 0.10f * inside : 0.0f;
            float ob = bg_b * a + 255.0f * border, og = bg_g * a + 255.0f * border, orr = bg_r * a + 255.0f * border;
            float oa = a;
            /* the drawn pixel over it */
            orr = orr * (1.0f - cov) + r;
            og = og * (1.0f - cov) + g;
            ob = ob * (1.0f - cov) + b;
            oa = oa * (1.0f - cov) + cov;
            if (orr > 255.0f) orr = 255.0f;
            if (og > 255.0f) og = 255.0f;
            if (ob > 255.0f) ob = 255.0f;
            s_ov_px[y * w + x] = (uint32_t)(oa * 255.0f + 0.5f) << 24 | (uint32_t)orr << 16 | (uint32_t)og << 8 | (uint32_t)ob;
        }
}

/* Draw the panel's picture (s_ov_px) for `mode` at scale `s`. */
static int ov_paint(int mode, float s, uint32_t bw, uint32_t bh)
{
    char t[160];
    int pad = (int)(14.0f * s), w, h, y, cap = (int)s_ov_limit;
    COLORREF fc;
    ov_fonts(s);
    if (s_opt_vsync) {
        /* with vsync, no faster than the monitor refreshes */
        DEVMODEA dm;
        memset(&dm, 0, sizeof dm);
        dm.dmSize = sizeof dm;
        if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1
                && (int)dm.dmDisplayFrequency < cap)
            cap = (int)dm.dmDisplayFrequency;
    }
    fc = ov_fps_colour(s_ov_fps, cap);
    if (mode == 3) {
        char msg[160];
        AcquireSRWLockShared(&s_toast_lock);
        strcpy_s(msg, sizeof msg, s_toast);
        ReleaseSRWLockShared(&s_toast_lock);
        w = pad * 2 + ov_text_w(s_ov_small, msg);
        h = (int)(40.0f * s);
        if (!ov_canvas(w, h))
            return 0;
        ov_text(s_ov_small, RGB(240, 244, 250), pad, (int)(9 * s), msg);
        ov_compose(s);
        return 1;
    }
    if (mode == 1) {
        sprintf_s(t, sizeof t, "%d", (int)(s_ov_fps + 0.5));
        w = pad * 2 + ov_text_w(s_ov_big, t) + (int)(6 * s) + ov_text_w(s_ov_label, "FPS");
        h = (int)(46.0f * s);
        if (!ov_canvas(w, h))
            return 0;
        ov_text(s_ov_big, fc, pad, (int)(4 * s), t);
        ov_text(s_ov_label, RGB(150, 160, 185), pad + ov_text_w(s_ov_big, t) + (int)(6 * s), (int)(20 * s), "FPS");
        ov_compose(s);
        return 1;
    }
    w = (int)(330.0f * s);
    h = (int)(206.0f * s);
    if (!ov_canvas(w, h))
        return 0;
    {
        /* line 1: FPS, frame times */
        float sum = 0.0f, worst = 0.0f;
        int i, n = s_ov_have;
        for (i = 0; i < n; i++) {
            sum += s_ov_ft[i];
            if (s_ov_ft[i] > worst)
                worst = s_ov_ft[i];
        }
        sprintf_s(t, sizeof t, "%d", (int)(s_ov_fps + 0.5));
        ov_text(s_ov_big, fc, pad, (int)(8 * s), t);
        ov_text(s_ov_label, RGB(150, 160, 185), pad + ov_text_w(s_ov_big, t) + (int)(6 * s), (int)(24 * s), "FPS");
        sprintf_s(t, sizeof t, "%.1f ms avg   %.1f ms max", n ? sum / n : 0.0f, worst);
        ov_text(s_ov_small, RGB(215, 222, 235), w - pad - ov_text_w(s_ov_small, t), (int)(20 * s), t);
    }
    {
        /* the graph: frame times, 0 to 40 ms, with the frame limit's line */
        int gx0 = pad, gx1 = w - pad, gy0 = (int)(52 * s), gy1 = (int)(104 * s), i;
        float scale = (float)(gy1 - gy0) / 40.0f, target = 1000.0f / (float)cap;
        ov_rect(gx0, gy0, gx1, gy1, OV_WELL);
        for (i = 0; i < OV_HIST; i++) {
            int k = (s_ov_pos + i) % OV_HIST, x0, x1, bar;
            float ms = i >= OV_HIST - s_ov_have ? s_ov_ft[k] : 0.0f;
            uint32_t colour;
            if (ms <= 0.0f)
                continue;
            x0 = gx0 + (gx1 - gx0) * i / OV_HIST;
            x1 = gx0 + (gx1 - gx0) * (i + 1) / OV_HIST - 1;           /* a gap between bars */
            if (x1 <= x0) x1 = x0 + 1;
            bar = (int)(ms * scale + 0.5f);
            if (bar > gy1 - gy0) bar = gy1 - gy0;
            if (bar < 1) bar = 1;
            colour = ms <= target * 1.1f ? 0x0070E68Cu : ms <= target * 1.8f ? 0x00FFCE54u : 0x00FF6A5Cu;
            ov_rect(x0, gy1 - bar, x1, gy1, colour);
        }
        y = gy1 - (int)(target * scale + 0.5f);
        ov_rect(gx0, y, gx1, y + (s >= 1.5f ? 2 : 1), 0x00A0AAC0u);
        sprintf_s(t, sizeof t, "%.1f ms", target);
        ov_text(s_ov_label, RGB(150, 160, 185), gx1 - ov_text_w(s_ov_label, t) - (int)(4 * s), y - (int)(16 * s), t);
    }
    {
        /* the facts */
        static const char *labels[3] = { "RENDERER", "RESOLUTION", "GPU" };
        char vals[3][160];
        int i, lx = pad, vx = pad + (int)(96 * s);
        strcpy_s(vals[0], sizeof vals[0], !s_native ? "Emulated Xbox GPU"
                 : gpu_backend() == GPU_BACKEND_VULKAN ? "Vulkan" : "Direct3D 11");
        sprintf_s(vals[1], sizeof vals[1], "%u x %u   VSync %s", bw, bh, s_opt_vsync ? "on" : "off");
        strcpy_s(vals[2], sizeof vals[2], s_ov_gpu);
        for (i = 0; i < 3; i++) {
            y = (int)((116 + i * 28) * s);
            ov_text(s_ov_label, RGB(150, 160, 185), lx, y + (int)(2 * s), labels[i]);
            {
                /* trim to fit */
                char v[160];
                strcpy_s(v, sizeof v, vals[i]);
                while (strlen(v) > 4 && vx + ov_text_w(s_ov_small, v) > w - pad) {
                    v[strlen(v) - 4] = 0;
                    strcat_s(v, sizeof v, "...");
                }
                ov_text(s_ov_small, RGB(235, 240, 248), vx, y, v);
            }
        }
    }
    ov_compose(s);
    return 1;
}

/* Upload the panel's picture, making the texture when its size changed. */
static int ov_upload(void)
{
    if (!s_ov_tex || s_ov_tw != s_ov_bw || s_ov_th != s_ov_bh) {
        GpuTextureDesc td;
        if (s_ov_tex) { gpu_texture_release(s_ov_tex); s_ov_tex = NULL; }
        memset(&td, 0, sizeof td);
        td.width = (uint32_t)s_ov_bw; td.height = (uint32_t)s_ov_bh; td.mips = 1;
        td.format = GPU_FMT_BGRA8;
        td.flags = GPU_TEX_SAMPLED;
        if (!(s_ov_tex = gpu_texture_create(&td, NULL)))
            return 0;
        s_ov_tw = s_ov_bw;
        s_ov_th = s_ov_bh;
    }
    gpu_texture_update(s_ov_tex, 0, 0, s_ov_px, (uint32_t)s_ov_bw * 4);
    return 1;
}

/* Over window 1's back buffer, after the frame's blit, before Present. */
#endif

static void gpu_overlay_draw(GpuSwapchain *sc)
{
#if defined(_WIN32) /* guard */
    int mode = (int)s_ov_mode;
    GpuTexture *bb, *none = NULL;
    GpuTextureDesc bd;
    GpuViewport vp;
    DWORD now = GetTickCount();
    float s;
    if ((int)(s_toast_until - now) > 0)
        mode = 3;                                   /* a message showing */
    if (!mode || !sc || !ov_init() || !(bb = gpu_swapchain_begin(sc, 0)))
        return;
    gpu_texture_desc(bb, &bd);
    s = (float)bd.height / 1080.0f;
    if (s < 0.75f) s = 0.75f;
    if (!s_ov_tex || (mode != 3 && now - s_ov_drawn_at >= (mode == 2 ? 100u : 250u)) || s_ov_font_scale != (int)(s * 100.0f + 0.5f)
            || s_ov_painted_mode != mode || (mode == 3 && s_toast_painted != s_toast_seq)
            || (mode == 1) != (s_ov_bw < (int)(200 * s))) {
        s_ov_drawn_at = now;
        s_ov_painted_mode = mode;
        s_toast_painted = s_toast_seq;
        if (!ov_paint(mode, s, (uint32_t)s_rw, (uint32_t)s_rh) || !ov_upload())
            return;
    }
    vp.x = mode == 3 ? (float)(int)(((int)bd.width - s_ov_tw) / 2) : (float)(int)(18.0f * s);
    vp.y = mode == 3 ? (float)(int)((float)bd.height * 0.80f) : (float)(int)(18.0f * s);
    vp.w = (float)s_ov_tw;
    vp.h = (float)s_ov_th;
    vp.min_z = 0.0f; vp.max_z = 1.0f;
    gpu_set_targets(bb, NULL);
    gpu_set_blend(s_ov_bs);
    gpu_set_depth(NULL, 0);
    gpu_set_raster(s_blit_rs);
    gpu_set_viewport(&vp);
    gpu_set_layout(NULL);
    gpu_set_topology(GPU_TOPO_TRIANGLES);
    gpu_set_shaders(s_blit_vs, s_ov_ps);
    gpu_set_textures(0, 1, &s_ov_tex);
    gpu_set_samplers(0, 1, &s_gsamp[0]);
    gpu_draw(3, 0);
    gpu_set_textures(0, 1, &none);
    gpu_set_targets(NULL, NULL);
    {
        /* BUFFY_OVERLAY_SHOT=<file.bmp> (testing): the back buffer with the
         * overlay, once, after three seconds of it */
        static DWORD first;
        static int done;
        const char *shot = getenv("BUFFY_OVERLAY_SHOT");
        if (!first)
            first = now;
        if (shot && !done && now - first > 3000) {
            done = 1;
            gpu_save_bmp(bb, bd.width, bd.height, shot);
        }
    }
#elif defined(BUFFY_ANDROID)
    /* the on-screen touch controls (port/android/src/android_touch.c) */
    void android_touch_draw(GpuSwapchain *sc);
    if (sc == s_main_sc)
        android_touch_draw(sc);
#else
    (void)sc;
#endif
}
