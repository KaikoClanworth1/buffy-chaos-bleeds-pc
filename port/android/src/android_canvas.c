/* A CPU picture for the Android screens (android_canvas.h). */
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "android_canvas.h"

uint32_t *cv_px;
int cv_w, cv_h;
float cv_s = 1.0f;

int cv_alloc(int w, int h, float scale)
{
    if (w != cv_w || h != cv_h || !cv_px) {
        free(cv_px);
        cv_px = (uint32_t *)malloc((size_t)w * h * 4);
        cv_w = cv_px ? w : 0;
        cv_h = cv_px ? h : 0;
    }
    cv_s = scale;
    return cv_px != NULL;
}

void cv_clear(uint32_t argb)
{
    int i, n = cv_w * cv_h;
    for (i = 0; i < n; i++)
        cv_px[i] = argb;
}

/* blend a colour at coverage a (0..1) into one pixel (premultiplied) */
static void put(int x, int y, uint32_t rgb, float a)
{
    uint32_t *p, d;
    float r, g, b, da, ia;
    if (x < 0 || y < 0 || x >= cv_w || y >= cv_h || a <= 0.0f)
        return;
    if (a > 1.0f) a = 1.0f;
    p = &cv_px[y * cv_w + x];
    d = *p;
    ia = 1.0f - a;
    b = (float)(rgb & 255) * a + (float)(d & 255) * ia;
    g = (float)((rgb >> 8) & 255) * a + (float)((d >> 8) & 255) * ia;
    r = (float)((rgb >> 16) & 255) * a + (float)((d >> 16) & 255) * ia;
    da = a * 255.0f + (float)(d >> 24) * ia;
    *p = ((uint32_t)da << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

void cv_disc(float cx, float cy, float r, float ring, uint32_t rgb, float a)
{
    int x, y, x0, x1, y0, y1;
    cx *= cv_s; cy *= cv_s; r *= cv_s; ring *= cv_s;
    x0 = (int)(cx - r - 2); x1 = (int)(cx + r + 2);
    y0 = (int)(cy - r - 2); y1 = (int)(cy + r + 2);
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++) {
            float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
            float cov = r - d + 0.5f;
            if (ring > 0) {
                float inner = d - (r - ring) + 0.5f;
                if (inner < cov) cov = inner;
            }
            if (cov > 0)
                put(x, y, rgb, a * (cov > 1 ? 1 : cov));
        }
}

void cv_rbox(const CvBox *b, float rad, float ring, uint32_t rgb, float a)
{
    int x, y;
    float x0 = b->x0 * cv_s, y0 = b->y0 * cv_s, x1 = b->x1 * cv_s, y1 = b->y1 * cv_s;
    rad *= cv_s; ring *= cv_s;
    for (y = (int)y0 - 1; y <= (int)y1 + 1; y++)
        for (x = (int)x0 - 1; x <= (int)x1 + 1; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float qx = fmaxf(fmaxf(x0 + rad - px, px - (x1 - rad)), 0.0f);
            float qy = fmaxf(fmaxf(y0 + rad - py, py - (y1 - rad)), 0.0f);
            float d, cov;
            if (rad > 0) {
                d = sqrtf(qx * qx + qy * qy) - rad;              /* <0 inside */
            } else {
                d = fmaxf(fmaxf(x0 - px, px - x1), fmaxf(y0 - py, py - y1));
            }
            cov = 0.5f - d;
            if (ring > 0 && d < -0.5f) {
                float in = (qx > 0 || qy > 0) ? -d
                         : fminf(fminf(px - x0, x1 - px), fminf(py - y0, y1 - py));
                cov = fminf(cov, ring - in + 0.5f);
            }
            if (cov > 0)
                put(x, y, rgb, a * (cov > 1 ? 1 : cov));
        }
}

void cv_arrow(float cx, float cy, float size, int dir, uint32_t rgb, float a)
{
    int x, y;
    cx *= cv_s; cy *= cv_s; size *= cv_s;
    for (y = (int)(cy - size) - 1; y <= (int)(cy + size) + 1; y++)
        for (x = (int)(cx - size) - 1; x <= (int)(cx + size) + 1; x++) {
            float u = (x + 0.5f - cx) / size, v = (y + 0.5f - cy) / size, t, s;
            switch (dir) {
            case 0: t = -v; s = u; break;
            case 1: t = v; s = u; break;
            case 2: t = -u; s = v; break;
            default: t = u; s = v; break;
            }
            if (t <= 0.6f && t >= -0.4f && fabsf(s) <= (0.6f - t) * 0.8f)
                put(x, y, rgb, a);
        }
}

/* 5x7 letters: each row's five bits, the leftmost highest */
static const struct { char c; uint8_t row[7]; } k_font[] = {
    { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } }, { 'B', { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E } },
    { 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } }, { 'D', { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E } },
    { 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } }, { 'F', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 } },
    { 'G', { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F } }, { 'H', { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'I', { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E } }, { 'J', { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C } },
    { 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } }, { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
    { 'M', { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 } }, { 'N', { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 } },
    { 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } }, { 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
    { 'Q', { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D } }, { 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
    { 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } }, { 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
    { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } }, { 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 } },
    { 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A } }, { 'X', { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 } },
    { 'Y', { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 } }, { 'Z', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F } },
    { '0', { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E } }, { '1', { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E } },
    { '2', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F } }, { '3', { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E } },
    { '4', { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 } }, { '5', { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E } },
    { '6', { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E } }, { '7', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
    { '8', { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E } }, { '9', { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C } },
    { '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C } }, { ',', { 0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08 } },
    { ':', { 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00 } }, { '/', { 0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10 } },
    { '-', { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 } }, { '_', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F } },
    { '(', { 0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02 } }, { ')', { 0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08 } },
    { '%', { 0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03 } }, { '\'', { 0x04, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00 } },
    { '!', { 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04 } }, { '?', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04 } },
    { '=', { 0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00 } }, { '+', { 0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00 } },
    { '>', { 0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08 } }, { '*', { 0x00, 0x15, 0x0E, 0x1F, 0x0E, 0x15, 0x00 } },
};

float cv_text_w(const char *t, float h)
{
    size_t n = strlen(t);
    return n ? (float)n * 6 * (h / 7.0f) - h / 7.0f : 0.0f;
}

void cv_text(const char *t, float cx, float cy, float h, uint32_t rgb, float a)
{
    float px = h / 7.0f, x0 = cx - cv_text_w(t, h) / 2, y0 = cy - h / 2;
    size_t i, k;
    int r, c;
    for (i = 0; t[i]; i++) {
        char ch = (char)toupper((unsigned char)t[i]);
        for (k = 0; k < sizeof k_font / sizeof k_font[0]; k++) {
            if (k_font[k].c != ch)
                continue;
            for (r = 0; r < 7; r++)
                for (c = 0; c < 5; c++)
                    if (k_font[k].row[r] & (0x10 >> c)) {
                        CvBox b = { x0 + (i * 6 + c) * px, y0 + r * px, x0 + (i * 6 + c + 1) * px, y0 + (r + 1) * px };
                        cv_rbox(&b, 0, 0, rgb, a);
                    }
            break;
        }
    }
}

/* ── on the GPU ── */

static int gpu_objects(CvGpu *g)
{
    static const char src[] =
        "Texture2D t0 : register(t0); SamplerState s0 : register(s0);\n"
        "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
        "V vs(uint id : SV_VertexID) { V o; o.uv = float2((id << 1) & 2, id & 2);\n"
        "  o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
        "float4 ps(V i) : SV_Target { return t0.Sample(s0, i.uv); }\n";
    GpuBlendDesc bd;
    GpuRasterDesc rd;
    GpuSamplerDesc sd;
    if (g->ps)
        return 1;
    g->vs = gpu_shader_create(GPU_VS, src, "vs");
    g->ps = gpu_shader_create(GPU_PS, src, "ps");
    memset(&bd, 0, sizeof bd);
    bd.enable = 1;
    bd.src = GPU_BLEND_ONE;                                     /* premultiplied */
    bd.dst = GPU_BLEND_INV_SRC_ALPHA;
    bd.op = GPU_BLENDOP_ADD;
    bd.src_alpha = GPU_BLEND_ONE;
    bd.dst_alpha = GPU_BLEND_INV_SRC_ALPHA;
    bd.op_alpha = GPU_BLENDOP_ADD;
    bd.write_mask = GPU_WRITE_ALL;
    g->bs = gpu_blend_create(&bd);
    memset(&rd, 0, sizeof rd);
    rd.cull = GPU_CULL_NONE;
    rd.depth_clip = 1;
    g->rs = gpu_raster_create(&rd);
    memset(&sd, 0, sizeof sd);
    sd.min_linear = sd.mag_linear = 1;
    sd.u = sd.v = sd.w = GPU_ADDRESS_CLAMP;
    g->samp = gpu_sampler_create(&sd);
    return g->vs && g->ps && g->bs && g->rs && g->samp;
}

int cv_upload(CvGpu *g)
{
    if (!cv_px || !gpu_objects(g))
        return 0;
    if (!g->tex || g->tw != cv_w || g->th != cv_h) {
        GpuTextureDesc td;
        if (g->tex)
            gpu_texture_release(g->tex);
        memset(&td, 0, sizeof td);
        td.width = (uint32_t)cv_w;
        td.height = (uint32_t)cv_h;
        td.mips = 1;
        td.format = GPU_FMT_BGRA8;
        td.flags = GPU_TEX_SAMPLED;
        g->tex = gpu_texture_create(&td, NULL);
        g->tw = cv_w;
        g->th = cv_h;
    }
    if (!g->tex)
        return 0;
    gpu_texture_update(g->tex, 0, 0, cv_px, (uint32_t)cv_w * 4);
    return 1;
}

void cv_draw(CvGpu *g, GpuTexture *bb, uint32_t bw, uint32_t bh)
{
    GpuTexture *none = NULL;
    GpuViewport vp;
    if (!g->tex || !gpu_objects(g))
        return;
    vp.x = 0; vp.y = 0;
    vp.w = (float)bw; vp.h = (float)bh;
    vp.min_z = 0.0f; vp.max_z = 1.0f;
    gpu_set_targets(bb, NULL);
    gpu_set_blend(g->bs);
    gpu_set_depth(NULL, 0);
    gpu_set_raster(g->rs);
    gpu_set_viewport(&vp);
    gpu_set_layout(NULL);
    gpu_set_topology(GPU_TOPO_TRIANGLES);
    gpu_set_shaders(g->vs, g->ps);
    gpu_set_textures(0, 1, &g->tex);
    gpu_set_samplers(0, 1, &g->samp);
    gpu_draw(3, 0);
    gpu_set_textures(0, 1, &none);
    gpu_set_targets(NULL, NULL);
}
