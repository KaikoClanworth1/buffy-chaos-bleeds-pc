/*
 * gpu_d3d11.c - the GPU layer's Direct3D 11 backend (see gpu.h).
 *
 * A thin layer: each object wraps its Direct3D one, and the context calls
 * pass straight through. Shaders are compiled with D3DCompile through the
 * disk cache the renderer always had (ShaderCache\<hash>.cso beside the exe,
 * the hash of source + entry + profile), so existing caches stay valid.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

struct GpuBuffer { ID3D11Buffer *b; uint32_t bytes; GpuUsage usage; volatile LONG refs; };
struct GpuTexture {
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
    ID3D11RenderTargetView *rtv;
    ID3D11DepthStencilView *dsv;
    GpuTextureDesc desc;
    volatile LONG refs;
};
struct GpuSampler { ID3D11SamplerState *s; volatile LONG refs; };
struct GpuShader { GpuStage stage; ID3D11VertexShader *vs; ID3D11PixelShader *ps; ID3DBlob *code; };
struct GpuLayout { ID3D11InputLayout *il; };
struct GpuBlendState { ID3D11BlendState *s; };
struct GpuDepthState { ID3D11DepthStencilState *s; };
struct GpuRasterState { ID3D11RasterizerState *s; };
struct GpuSwapchain { IDXGISwapChain *sc; HWND hwnd; GpuTexture *bb; };

static ID3D11Device *s_dev;
static ID3D11DeviceContext *s_ctx;
static ID3D11DeviceContext1 *s_ctx1;
static IDXGIFactory *s_factory;
static char s_adapter[128];
static int s_up;

/* ── device ── */

static int d3d_init(void)
{
    HRESULT hr;
    IDXGIDevice *dd = NULL;
    IDXGIAdapter *ad = NULL;
    if (s_up)
        return 1;
    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &s_dev, NULL, &s_ctx);
    if (FAILED(hr)) {
        fprintf(stderr, "  [GPU] Direct3D 11 device creation failed 0x%08lX\n", hr);
        return 0;
    }
    if (FAILED(ID3D11DeviceContext_QueryInterface(s_ctx, &IID_ID3D11DeviceContext1, (void **)&s_ctx1)))
        s_ctx1 = NULL;
    if (SUCCEEDED(ID3D11Device_QueryInterface(s_dev, &IID_IDXGIDevice, (void **)&dd))) {
        if (SUCCEEDED(IDXGIDevice_GetAdapter(dd, &ad))) {
            DXGI_ADAPTER_DESC d;
            if (SUCCEEDED(IDXGIAdapter_GetDesc(ad, &d)))
                WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, s_adapter, sizeof s_adapter, NULL, NULL);
            IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory, (void **)&s_factory);
            IDXGIAdapter_Release(ad);
        }
        IDXGIDevice_Release(dd);
    }
    s_up = 1;
    return 1;
}

int dx_init(void *window)
{
    (void)window;
    return d3d_init();
}

const char *dx_adapter_name(void) { return s_adapter; }
void dx_flush(void) { if (s_ctx) ID3D11DeviceContext_Flush(s_ctx); }

void *dx_device(void) { return s_dev; }
void *dx_context(void) { return s_ctx; }

/* ── formats ── */

static DXGI_FORMAT dxgi(GpuFormat f)
{
    switch (f) {
    case GPU_FMT_BGRA8: case GPU_FMT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case GPU_FMT_RGBA8: case GPU_FMT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case GPU_FMT_BC1: return DXGI_FORMAT_BC1_UNORM;
    case GPU_FMT_BC2: return DXGI_FORMAT_BC2_UNORM;
    case GPU_FMT_BC3: return DXGI_FORMAT_BC3_UNORM;
    case GPU_FMT_D24S8: return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case GPU_FMT_R32G32B32A32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case GPU_FMT_R32G32B32_FLOAT: return DXGI_FORMAT_R32G32B32_FLOAT;
    case GPU_FMT_R32G32_FLOAT: return DXGI_FORMAT_R32G32_FLOAT;
    case GPU_FMT_R32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case GPU_FMT_R32_UINT: return DXGI_FORMAT_R32_UINT;
    case GPU_FMT_R16G16B16A16_SNORM: return DXGI_FORMAT_R16G16B16A16_SNORM;
    case GPU_FMT_R16G16_SNORM: return DXGI_FORMAT_R16G16_SNORM;
    case GPU_FMT_R16_SNORM: return DXGI_FORMAT_R16_SNORM;
    case GPU_FMT_R8G8_UNORM: return DXGI_FORMAT_R8G8_UNORM;
    case GPU_FMT_R8_UNORM: return DXGI_FORMAT_R8_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

static GpuFormat from_dxgi(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_UNORM: return GPU_FMT_BGRA8;
    case DXGI_FORMAT_R8G8B8A8_UNORM: return GPU_FMT_RGBA8;
    case DXGI_FORMAT_BC1_UNORM: return GPU_FMT_BC1;
    case DXGI_FORMAT_BC2_UNORM: return GPU_FMT_BC2;
    case DXGI_FORMAT_BC3_UNORM: return GPU_FMT_BC3;
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return GPU_FMT_D24S8;
    default: return GPU_FMT_UNKNOWN;
    }
}

/* ── buffers ── */

GpuBuffer *dx_buffer_create(uint32_t bytes, unsigned bind, GpuUsage usage, const void *init)
{
    D3D11_BUFFER_DESC bd;
    D3D11_SUBRESOURCE_DATA sd;
    GpuBuffer *b = (GpuBuffer *)calloc(1, sizeof *b);
    if (!b)
        return NULL;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = (bind & GPU_BIND_CONSTANT) ? (bytes + 15) & ~15u : bytes;
    bd.BindFlags = ((bind & GPU_BIND_VERTEX) ? D3D11_BIND_VERTEX_BUFFER : 0)
                 | ((bind & GPU_BIND_INDEX) ? D3D11_BIND_INDEX_BUFFER : 0)
                 | ((bind & GPU_BIND_CONSTANT) ? D3D11_BIND_CONSTANT_BUFFER : 0);
    bd.Usage = usage == GPU_USAGE_DYNAMIC ? D3D11_USAGE_DYNAMIC
             : usage == GPU_USAGE_IMMUTABLE ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT;
    bd.CPUAccessFlags = usage == GPU_USAGE_DYNAMIC ? D3D11_CPU_ACCESS_WRITE : 0;
    sd.pSysMem = init;
    sd.SysMemPitch = sd.SysMemSlicePitch = 0;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, init ? &sd : NULL, &b->b))) {
        free(b);
        return NULL;
    }
    b->bytes = bytes;
    b->usage = usage;
    b->refs = 1;
    return b;
}

void dx_buffer_release(GpuBuffer *b)
{
    if (!b || InterlockedDecrement(&b->refs) != 0) return;
    if (b->b) ID3D11Buffer_Release(b->b);
    free(b);
}

void dx_buffer_addref(GpuBuffer *b)
{
    if (b) InterlockedIncrement(&b->refs);
}

void dx_buffer_copy(GpuBuffer *dst, uint32_t dst_off, GpuBuffer *src, uint32_t src_off, uint32_t bytes)
{
    D3D11_BOX box;
    box.left = src_off; box.right = src_off + bytes;
    box.top = 0; box.bottom = 1; box.front = 0; box.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)dst->b, 0, dst_off, 0, 0, (ID3D11Resource *)src->b, 0, &box);
}

void *dx_buffer_map(GpuBuffer *b, GpuMap how)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)b->b, 0,
                                       how == GPU_MAP_NO_OVERWRITE ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD,
                                       0, &m)))
        return NULL;
    return m.pData;
}

void dx_buffer_unmap(GpuBuffer *b)
{
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)b->b, 0);
}

void dx_buffer_update(GpuBuffer *b, const void *data)
{
    ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)b->b, 0, NULL, data, 0, 0);
}

/* ── textures ── */

static int make_views(GpuTexture *t)
{
    const GpuTextureDesc *d = &t->desc;
    if (d->flags & GPU_TEX_SAMPLED) {
        D3D11_SHADER_RESOURCE_VIEW_DESC v;
        memset(&v, 0, sizeof v);
        v.Format = dxgi(d->format);
        if (d->flags & GPU_TEX_CUBE) {
            v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            v.TextureCube.MipLevels = (UINT)-1;
        } else {
            v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            v.Texture2D.MipLevels = (UINT)-1;
        }
        if (FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)t->tex, &v, &t->srv)))
            return 0;
    }
    if ((d->flags & GPU_TEX_TARGET)
            && FAILED(ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)t->tex, NULL, &t->rtv)))
        return 0;
    if ((d->flags & GPU_TEX_DEPTH)
            && FAILED(ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)t->tex, NULL, &t->dsv)))
        return 0;
    return 1;
}

static void tex_free(GpuTexture *t)
{
    if (t->srv) ID3D11ShaderResourceView_Release(t->srv);
    if (t->rtv) ID3D11RenderTargetView_Release(t->rtv);
    if (t->dsv) ID3D11DepthStencilView_Release(t->dsv);
    if (t->tex) ID3D11Texture2D_Release(t->tex);
    free(t);
}

GpuTexture *dx_texture_create(const GpuTextureDesc *desc, const GpuSubresource *init)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sd[6 * 16];
    GpuTexture *t = (GpuTexture *)calloc(1, sizeof *t);
    uint32_t mips = desc->mips ? desc->mips : 1, faces = (desc->flags & GPU_TEX_CUBE) ? 6 : 1, i;
    if (!t || mips > 16)
        return (free(t), (GpuTexture *)NULL);
    t->desc = *desc;
    t->desc.mips = mips;
    t->refs = 1;
    memset(&td, 0, sizeof td);
    td.Width = desc->width;
    td.Height = desc->height;
    td.MipLevels = mips;
    td.ArraySize = faces;
    td.Format = dxgi(desc->format);
    td.SampleDesc.Count = 1;
    if (desc->flags & GPU_TEX_READBACK) {
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    } else {
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = ((desc->flags & GPU_TEX_SAMPLED) ? D3D11_BIND_SHADER_RESOURCE : 0)
                     | ((desc->flags & GPU_TEX_TARGET) ? D3D11_BIND_RENDER_TARGET : 0)
                     | ((desc->flags & GPU_TEX_DEPTH) ? D3D11_BIND_DEPTH_STENCIL : 0);
        td.MiscFlags = (desc->flags & GPU_TEX_CUBE) ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    }
    if (init)
        for (i = 0; i < mips * faces; i++) {
            sd[i].pSysMem = init[i].data;
            sd[i].SysMemPitch = init[i].pitch;
            sd[i].SysMemSlicePitch = 0;
        }
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, init ? sd : NULL, &t->tex)) || !make_views(t)) {
        tex_free(t);
        return NULL;
    }
    return t;
}

GpuTexture *dx_wrap_texture(void *tex, void *srv)
{
    D3D11_TEXTURE2D_DESC td;
    GpuTexture *t = (GpuTexture *)calloc(1, sizeof *t);
    if (!t || !tex)
        return (free(t), (GpuTexture *)NULL);
    t->tex = (ID3D11Texture2D *)tex;
    ID3D11Texture2D_AddRef(t->tex);
    ID3D11Texture2D_GetDesc(t->tex, &td);
    t->desc.width = td.Width;
    t->desc.height = td.Height;
    t->desc.mips = td.MipLevels;
    t->desc.format = from_dxgi(td.Format);
    t->desc.flags = ((td.BindFlags & D3D11_BIND_SHADER_RESOURCE) ? GPU_TEX_SAMPLED : 0)
                  | ((td.BindFlags & D3D11_BIND_RENDER_TARGET) ? GPU_TEX_TARGET : 0)
                  | ((td.BindFlags & D3D11_BIND_DEPTH_STENCIL) ? GPU_TEX_DEPTH : 0)
                  | ((td.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) ? GPU_TEX_CUBE : 0);
    t->refs = 1;
    if (srv) {
        t->srv = (ID3D11ShaderResourceView *)srv;
        ID3D11ShaderResourceView_AddRef(t->srv);
    }
    if (td.BindFlags & D3D11_BIND_RENDER_TARGET)
        ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)t->tex, NULL, &t->rtv);
    if (td.BindFlags & D3D11_BIND_DEPTH_STENCIL)
        ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)t->tex, NULL, &t->dsv);
    return t;
}

void dx_texture_release(GpuTexture *t)
{
    if (t && InterlockedDecrement(&t->refs) == 0)
        tex_free(t);
}

void dx_texture_addref(GpuTexture *t)
{
    if (t) InterlockedIncrement(&t->refs);
}

void dx_texture_desc(const GpuTexture *t, GpuTextureDesc *out)
{
    *out = t->desc;
}

void *dx_texture(GpuTexture *t) { return t ? t->tex : NULL; }
void *dx_srv(GpuTexture *t) { return t ? t->srv : NULL; }

void dx_texture_update(GpuTexture *t, uint32_t face, uint32_t mip, const void *data, uint32_t pitch)
{
    ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)t->tex, face * t->desc.mips + mip, NULL, data, pitch, 0);
}

void dx_texture_copy(GpuTexture *dst, GpuTexture *src)
{
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)dst->tex, (ID3D11Resource *)src->tex);
}

void dx_texture_copy_region(GpuTexture *dst, uint32_t dx, uint32_t dy,
                                   GpuTexture *src, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h)
{
    D3D11_BOX b;
    b.left = sx; b.top = sy; b.front = 0;
    b.right = sx + w; b.bottom = sy + h; b.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)dst->tex, 0, dx, dy, 0,
                                              (ID3D11Resource *)src->tex, 0, &b);
}

int dx_texture_read(GpuTexture *t, void *out, int no_wait)
{
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t y, x, w = t->desc.width, h = t->desc.height;
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)t->tex, 0, D3D11_MAP_READ,
                                       no_wait ? D3D11_MAP_FLAG_DO_NOT_WAIT : 0, &m)))
        return 0;
    for (y = 0; y < h; y++) {
        const uint8_t *src = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
        uint8_t *dst = (uint8_t *)out + (size_t)y * w * 4;
        if (t->desc.format == GPU_FMT_RGBA8) {
            for (x = 0; x < w; x++) {
                dst[x * 4 + 0] = src[x * 4 + 2];
                dst[x * 4 + 1] = src[x * 4 + 1];
                dst[x * 4 + 2] = src[x * 4 + 0];
                dst[x * 4 + 3] = src[x * 4 + 3];
            }
        } else {
            memcpy(dst, src, (size_t)w * 4);
        }
    }
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)t->tex, 0);
    return 1;
}

/* ── samplers ── */

GpuSampler *dx_sampler_create(const GpuSamplerDesc *d)
{
    D3D11_SAMPLER_DESC s;
    GpuSampler *g = (GpuSampler *)calloc(1, sizeof *g);
    if (!g)
        return NULL;
    memset(&s, 0, sizeof s);
    s.Filter = (D3D11_FILTER)((d->min_linear ? 0x10 : 0) | (d->mag_linear ? 0x04 : 0) | (d->mip_linear ? 0x01 : 0));
    s.AddressU = (D3D11_TEXTURE_ADDRESS_MODE)d->u;
    s.AddressV = (D3D11_TEXTURE_ADDRESS_MODE)d->v;
    s.AddressW = (D3D11_TEXTURE_ADDRESS_MODE)d->w;
    memcpy(s.BorderColor, d->border, sizeof s.BorderColor);
    s.MaxLOD = d->max_lod >= 1000.0f ? D3D11_FLOAT32_MAX : d->max_lod;
    s.ComparisonFunc = D3D11_COMPARISON_NEVER;
    if (FAILED(ID3D11Device_CreateSamplerState(s_dev, &s, &g->s))) {
        free(g);
        return NULL;
    }
    g->refs = 1;
    return g;
}

void dx_sampler_release(GpuSampler *s)
{
    if (!s || InterlockedDecrement(&s->refs) != 0) return;
    if (s->s) ID3D11SamplerState_Release(s->s);
    free(s);
}

void dx_sampler_addref(GpuSampler *s)
{
    if (s) InterlockedIncrement(&s->refs);
}

void *dx_sampler(GpuSampler *s) { return s ? s->s : NULL; }
void *dx_shader(GpuShader *s) { return !s ? NULL : s->vs ? (void *)s->vs : (void *)s->ps; }

/* ── shaders (and the disk cache) ── */

static HRESULT compile_cached(const char *src, const char *entry, const char *profile, ID3DBlob **out)
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
    {
        /* (testing) BUFFY_SHADER_DUMP=<dir>: every shader's source, once */
        static const char *dump = (const char *)-1;
        if (dump == (const char *)-1)
            dump = getenv("BUFFY_SHADER_DUMP");
        if (dump) {
            char dp[MAX_PATH];
            sprintf_s(dp, sizeof dp, "%s\\%016llx_%s_%s.hlsl", dump, (unsigned long long)h, entry, profile);
            if (GetFileAttributesA(dp) == INVALID_FILE_ATTRIBUTES && fopen_s(&f, dp, "wb") == 0 && f) {
                fwrite(src, 1, strlen(src), f);
                fclose(f);
            }
        }
    }
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
    hr = D3DCompile(src, strlen(src), entry, NULL, NULL, entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, out, &err);
    if (FAILED(hr)) {
        static int shown;
        if (shown++ < 3)
            fprintf(stderr, "  [GPU] %s shader failed: %s\n%s\n", profile,
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

/* (for the pushbuffer renderer, which still compiles its own) */
long dx_compile(const char *src, const char *entry, const char *profile, void **blob)
{
    return (long)compile_cached(src, entry, profile, (ID3DBlob **)blob);
}

GpuShader *dx_shader_create(GpuStage stage, const char *hlsl, const char *entry)
{
    GpuShader *s = (GpuShader *)calloc(1, sizeof *s);
    ID3DBlob *b = NULL;
    HRESULT hr;
    if (!s)
        return NULL;
    if (FAILED(compile_cached(hlsl, entry, stage == GPU_VS ? "vs_4_0" : "ps_4_0", &b)) || !b) {
        free(s);
        return NULL;
    }
    s->stage = stage;
    if (stage == GPU_VS)
        hr = ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s->vs);
    else
        hr = ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s->ps);
    if (FAILED(hr)) {
        ID3D10Blob_Release(b);
        free(s);
        return NULL;
    }
    if (stage == GPU_VS)
        s->code = b;                           /* (input layouts are checked against it) */
    else
        ID3D10Blob_Release(b);
    return s;
}

void dx_shader_release(GpuShader *s)
{
    if (!s) return;
    if (s->vs) ID3D11VertexShader_Release(s->vs);
    if (s->ps) ID3D11PixelShader_Release(s->ps);
    if (s->code) ID3D10Blob_Release(s->code);
    free(s);
}

/* ── input layouts ── */

GpuLayout *dx_layout_create(const GpuLayoutElement *e, uint32_t n, GpuShader *vs)
{
    D3D11_INPUT_ELEMENT_DESC d[16];
    GpuLayout *l;
    uint32_t i;
    if (n > 16 || !vs || !vs->code)
        return NULL;
    l = (GpuLayout *)calloc(1, sizeof *l);
    if (!l)
        return NULL;
    for (i = 0; i < n; i++) {
        d[i].SemanticName = "ATTR";
        d[i].SemanticIndex = e[i].location;
        d[i].Format = dxgi(e[i].format);
        d[i].InputSlot = e[i].slot;
        d[i].AlignedByteOffset = e[i].offset;
        d[i].InputSlotClass = e[i].per_instance ? D3D11_INPUT_PER_INSTANCE_DATA : D3D11_INPUT_PER_VERTEX_DATA;
        d[i].InstanceDataStepRate = e[i].per_instance ? 1 : 0;
    }
    if (FAILED(ID3D11Device_CreateInputLayout(s_dev, d, n, ID3D10Blob_GetBufferPointer(vs->code),
                                              ID3D10Blob_GetBufferSize(vs->code), &l->il))) {
        free(l);
        return NULL;
    }
    return l;
}

void dx_layout_release(GpuLayout *l)
{
    if (!l) return;
    if (l->il) ID3D11InputLayout_Release(l->il);
    free(l);
}

/* ── state objects ── */

GpuBlendState *dx_blend_create(const GpuBlendDesc *d)
{
    D3D11_BLEND_DESC b;
    GpuBlendState *s = (GpuBlendState *)calloc(1, sizeof *s);
    if (!s)
        return NULL;
    memset(&b, 0, sizeof b);
    b.RenderTarget[0].BlendEnable = d->enable != 0;
    b.RenderTarget[0].SrcBlend = (D3D11_BLEND)d->src;
    b.RenderTarget[0].DestBlend = (D3D11_BLEND)d->dst;
    b.RenderTarget[0].BlendOp = (D3D11_BLEND_OP)d->op;
    b.RenderTarget[0].SrcBlendAlpha = (D3D11_BLEND)d->src_alpha;
    b.RenderTarget[0].DestBlendAlpha = (D3D11_BLEND)d->dst_alpha;
    b.RenderTarget[0].BlendOpAlpha = (D3D11_BLEND_OP)d->op_alpha;
    b.RenderTarget[0].RenderTargetWriteMask = (UINT8)d->write_mask;
    if (FAILED(ID3D11Device_CreateBlendState(s_dev, &b, &s->s))) {
        free(s);
        return NULL;
    }
    return s;
}

GpuDepthState *dx_depth_create(const GpuDepthDesc *d)
{
    D3D11_DEPTH_STENCIL_DESC z;
    GpuDepthState *s = (GpuDepthState *)calloc(1, sizeof *s);
    if (!s)
        return NULL;
    memset(&z, 0, sizeof z);
    z.DepthEnable = d->depth_enable != 0;
    z.DepthWriteMask = d->depth_write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    z.DepthFunc = (D3D11_COMPARISON_FUNC)(d->depth_func ? d->depth_func : GPU_CMP_ALWAYS);
    z.StencilEnable = d->stencil_enable != 0;
    z.StencilReadMask = d->stencil_read_mask;
    z.StencilWriteMask = d->stencil_write_mask;
    z.FrontFace.StencilFunc = (D3D11_COMPARISON_FUNC)(d->stencil_func ? d->stencil_func : GPU_CMP_ALWAYS);
    z.FrontFace.StencilFailOp = (D3D11_STENCIL_OP)(d->stencil_fail ? d->stencil_fail : GPU_STENCIL_KEEP);
    z.FrontFace.StencilDepthFailOp = (D3D11_STENCIL_OP)(d->stencil_depth_fail ? d->stencil_depth_fail : GPU_STENCIL_KEEP);
    z.FrontFace.StencilPassOp = (D3D11_STENCIL_OP)(d->stencil_pass ? d->stencil_pass : GPU_STENCIL_KEEP);
    z.BackFace = z.FrontFace;
    if (FAILED(ID3D11Device_CreateDepthStencilState(s_dev, &z, &s->s))) {
        free(s);
        return NULL;
    }
    return s;
}

GpuRasterState *dx_raster_create(const GpuRasterDesc *d)
{
    D3D11_RASTERIZER_DESC r;
    GpuRasterState *s = (GpuRasterState *)calloc(1, sizeof *s);
    if (!s)
        return NULL;
    memset(&r, 0, sizeof r);
    r.FillMode = D3D11_FILL_SOLID;
    r.CullMode = (D3D11_CULL_MODE)(d->cull ? d->cull : GPU_CULL_NONE);
    r.FrontCounterClockwise = d->front_ccw != 0;
    r.DepthClipEnable = d->depth_clip != 0;
    r.ScissorEnable = d->scissor != 0;
    if (FAILED(ID3D11Device_CreateRasterizerState(s_dev, &r, &s->s))) {
        free(s);
        return NULL;
    }
    return s;
}

void dx_blend_release(GpuBlendState *s) { if (s) { if (s->s) ID3D11BlendState_Release(s->s); free(s); } }
void dx_depth_release(GpuDepthState *s) { if (s) { if (s->s) ID3D11DepthStencilState_Release(s->s); free(s); } }
void dx_raster_release(GpuRasterState *s) { if (s) { if (s->s) ID3D11RasterizerState_Release(s->s); free(s); } }

/* ── the context ── */

void dx_set_targets(GpuTexture *target, GpuTexture *depth)
{
    ID3D11RenderTargetView *rtv = target ? target->rtv : NULL;
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, rtv ? 1 : 0, rtv ? &rtv : NULL, depth ? depth->dsv : NULL);
}

void dx_set_viewport(const GpuViewport *vp)
{
    D3D11_VIEWPORT v;
    v.TopLeftX = vp->x; v.TopLeftY = vp->y; v.Width = vp->w; v.Height = vp->h;
    v.MinDepth = vp->min_z; v.MaxDepth = vp->max_z;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &v);
}

void dx_set_scissor(int x, int y, int w, int h)
{
    D3D11_RECT r;
    r.left = x; r.top = y; r.right = x + w; r.bottom = y + h;
    ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &r);
}

void dx_set_blend(GpuBlendState *s)
{
    ID3D11DeviceContext_OMSetBlendState(s_ctx, s ? s->s : NULL, NULL, 0xFFFFFFFFu);
}

void dx_set_depth(GpuDepthState *s, uint32_t ref)
{
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, s ? s->s : NULL, ref);
}

void dx_set_raster(GpuRasterState *s)
{
    ID3D11DeviceContext_RSSetState(s_ctx, s ? s->s : NULL);
}

void dx_set_layout(GpuLayout *l)
{
    ID3D11DeviceContext_IASetInputLayout(s_ctx, l ? l->il : NULL);
}

void dx_set_vertex_buffers(uint32_t first, uint32_t n, GpuBuffer *const *b,
                                  const uint32_t *strides, const uint32_t *offsets)
{
    ID3D11Buffer *bufs[16];
    uint32_t i;
    for (i = 0; i < n && i < 16; i++)
        bufs[i] = b[i] ? b[i]->b : NULL;
    ID3D11DeviceContext_IASetVertexBuffers(s_ctx, first, n, bufs, strides, offsets);
}

void dx_set_index_buffer(GpuBuffer *b)
{
    ID3D11DeviceContext_IASetIndexBuffer(s_ctx, b ? b->b : NULL, DXGI_FORMAT_R32_UINT, 0);
}

void dx_set_topology(GpuTopology t)
{
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, (D3D11_PRIMITIVE_TOPOLOGY)t);
}

void dx_set_shaders(GpuShader *vs, GpuShader *ps)
{
    ID3D11DeviceContext_VSSetShader(s_ctx, vs ? vs->vs : NULL, NULL, 0);
    ID3D11DeviceContext_PSSetShader(s_ctx, ps ? ps->ps : NULL, NULL, 0);
}

void dx_set_cbuffer(GpuStage stage, uint32_t slot, GpuBuffer *b, uint32_t first, uint32_t count)
{
    ID3D11Buffer *buf = b ? b->b : NULL;
    if ((first || count) && s_ctx1) {
        UINT f = first, c = count ? count : ((b ? b->bytes : 0) + 15) / 16;
        c = (c + 15) & ~15u;
        if (stage == GPU_VS)
            ID3D11DeviceContext1_VSSetConstantBuffers1(s_ctx1, slot, 1, &buf, &f, &c);
        else
            ID3D11DeviceContext1_PSSetConstantBuffers1(s_ctx1, slot, 1, &buf, &f, &c);
        return;
    }
    if (stage == GPU_VS)
        ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, slot, 1, &buf);
    else
        ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, slot, 1, &buf);
}

void dx_set_textures(uint32_t first, uint32_t n, GpuTexture *const *t)
{
    ID3D11ShaderResourceView *v[16];
    uint32_t i;
    for (i = 0; i < n && i < 16; i++)
        v[i] = t && t[i] ? t[i]->srv : NULL;
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, first, n, v);
}

void dx_set_samplers(uint32_t first, uint32_t n, GpuSampler *const *s)
{
    ID3D11SamplerState *v[16];
    uint32_t i;
    for (i = 0; i < n && i < 16; i++)
        v[i] = s && s[i] ? s[i]->s : NULL;
    ID3D11DeviceContext_PSSetSamplers(s_ctx, first, n, v);
}

void dx_clear_target(GpuTexture *t, const float rgba[4])
{
    if (t && t->rtv)
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx, t->rtv, rgba);
}

void dx_clear_depth(GpuTexture *t, unsigned flags, float z, uint8_t stencil)
{
    if (t && t->dsv)
        ID3D11DeviceContext_ClearDepthStencilView(s_ctx, t->dsv,
            ((flags & GPU_CLEAR_DEPTH) ? D3D11_CLEAR_DEPTH : 0) | ((flags & GPU_CLEAR_STENCIL) ? D3D11_CLEAR_STENCIL : 0), z, stencil);
}

void dx_draw(uint32_t v, uint32_t first) { ID3D11DeviceContext_Draw(s_ctx, v, first); }
void dx_draw_indexed(uint32_t n, uint32_t first, int32_t base) { ID3D11DeviceContext_DrawIndexed(s_ctx, n, first, base); }
void dx_draw_indexed_instanced(uint32_t n, uint32_t inst, uint32_t first, int32_t base)
{
    ID3D11DeviceContext_DrawIndexedInstanced(s_ctx, n, inst, first, base, 0);
}

/* ── swap chains ── */

GpuSwapchain *dx_swapchain_create(void *window)
{
    DXGI_SWAP_CHAIN_DESC sd;
    GpuSwapchain *s = (GpuSwapchain *)calloc(1, sizeof *s);
    if (!s || !s_factory)
        return (free(s), (GpuSwapchain *)NULL);
    memset(&sd, 0, sizeof sd);
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;   /* (0 x 0: the window's client size) */
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = (HWND)window;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (FAILED(IDXGIFactory_CreateSwapChain(s_factory, (IUnknown *)s_dev, &sd, &s->sc))) {
        free(s);
        return NULL;
    }
    /* DXGI's own Alt+Enter would switch to exclusive mode; the window
     * procedure does borderless fullscreen instead. */
    IDXGIFactory_MakeWindowAssociation(s_factory, (HWND)window, DXGI_MWA_NO_ALT_ENTER);
    s->hwnd = (HWND)window;
    return s;
}

void dx_swapchain_release(GpuSwapchain *s)
{
    if (!s) return;
    if (s->bb) dx_texture_release(s->bb);
    if (s->sc) IDXGISwapChain_Release(s->sc);
    free(s);
}

void *dx_swapchain(GpuSwapchain *s) { return s ? s->sc : NULL; }

GpuTexture *dx_swapchain_begin(GpuSwapchain *s, int resize)
{
    ID3D11Texture2D *bb = NULL;
    if (resize) {
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
        if (s->bb) { dx_texture_release(s->bb); s->bb = NULL; }
        IDXGISwapChain_ResizeBuffers(s->sc, 0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
    }
    if (s->bb)
        return s->bb;                          /* (flip model: buffer 0 is always the one to draw) */
    if (FAILED(IDXGISwapChain_GetBuffer(s->sc, 0, &IID_ID3D11Texture2D, (void **)&bb)))
        return NULL;
    s->bb = dx_wrap_texture(bb, NULL);
    ID3D11Texture2D_Release(bb);
    return s->bb;
}

void dx_swapchain_present(GpuSwapchain *s, int vsync)
{
    IDXGISwapChain_Present(s->sc, vsync ? 1 : 0, 0);
}
