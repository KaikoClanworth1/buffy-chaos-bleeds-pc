/*
 * gpu.h - the renderer's GPU layer: one interface, two backends
 * (gpu_d3d11.c: Direct3D 11, gpu_vulkan.c: Vulkan).
 *
 * Shaped after what the renderer already did with Direct3D 11, so moving it
 * over is mechanical: buffers, textures, samplers, shaders (HLSL, compiled by
 * the backend and cached on disk), input layouts, blend / depth / raster
 * state objects, one immediate context used from one thread at a time, and
 * swap chains for the windows.
 *
 * Differences from D3D11 that the callers see:
 *   - textures are bound, cleared and drawn to directly (the backends keep
 *     their own views);
 *   - a shader is made from HLSL source (register numbers as gpu_spirv.h
 *     lists them), never from bytecode;
 *   - every enum is this file's (the values of blend factors, comparisons,
 *     stencil ops, address modes and topologies are D3D11's own numbers, so
 *     the translations from NV2A / D3D8 state stay what they were).
 *
 * gpu_d3d11_*() (bottom) hand out the Direct3D 11 objects themselves, for the
 * code that stays Direct3D-only: the emulated (pushbuffer) renderer and some
 * debug readbacks. They return NULL under Vulkan.
 */
#ifndef GPU_H
#define GPU_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GpuBuffer      GpuBuffer;
typedef struct GpuTexture     GpuTexture;
typedef struct GpuSampler     GpuSampler;
typedef struct GpuShader      GpuShader;
typedef struct GpuLayout      GpuLayout;
typedef struct GpuBlendState  GpuBlendState;
typedef struct GpuDepthState  GpuDepthState;
typedef struct GpuRasterState GpuRasterState;
typedef struct GpuSwapchain   GpuSwapchain;

/* ── the device ──────────────────────────────────────────────────────── */

typedef enum { GPU_BACKEND_D3D11 = 0, GPU_BACKEND_VULKAN = 1 } GpuBackend;

/* Brings the device up (window: the HWND of the first window on Windows).
 * 1 on success. */
int        gpu_init(GpuBackend backend, void *window);
GpuBackend gpu_backend(void);
const char *gpu_adapter_name(void);          /* "NVIDIA GeForce ...", for logs and the overlay */
void       gpu_flush(void);                  /* submit what is recorded (no wait) */

/* ── formats ─────────────────────────────────────────────────────────── */

typedef enum {
    GPU_FMT_UNKNOWN = 0,
    /* textures and render targets */
    GPU_FMT_BGRA8, GPU_FMT_RGBA8, GPU_FMT_BC1, GPU_FMT_BC2, GPU_FMT_BC3, GPU_FMT_D24S8,
    /* vertex attributes and index data */
    GPU_FMT_R32G32B32A32_FLOAT, GPU_FMT_R32G32B32_FLOAT, GPU_FMT_R32G32_FLOAT, GPU_FMT_R32_FLOAT,
    GPU_FMT_R32_UINT, GPU_FMT_R16G16B16A16_SNORM, GPU_FMT_R16G16_SNORM, GPU_FMT_R16_SNORM,
    GPU_FMT_R8G8B8A8_UNORM, GPU_FMT_R8G8_UNORM, GPU_FMT_R8_UNORM, GPU_FMT_B8G8R8A8_UNORM,
    GPU_FMT_COUNT
} GpuFormat;

/* ── buffers ─────────────────────────────────────────────────────────── */

enum { GPU_BIND_VERTEX = 1, GPU_BIND_INDEX = 2, GPU_BIND_CONSTANT = 4 };
typedef enum {
    GPU_USAGE_DEFAULT = 0,     /* written with gpu_buffer_update */
    GPU_USAGE_DYNAMIC,         /* written with gpu_buffer_map (discard / no-overwrite) */
    GPU_USAGE_IMMUTABLE        /* written once, at creation */
} GpuUsage;
typedef enum { GPU_MAP_DISCARD = 0, GPU_MAP_NO_OVERWRITE = 1 } GpuMap;

GpuBuffer *gpu_buffer_create(uint32_t bytes, unsigned bind, GpuUsage usage, const void *init);
void       gpu_buffer_release(GpuBuffer *b);
void      *gpu_buffer_map(GpuBuffer *b, GpuMap how);     /* NULL on failure */
void       gpu_buffer_unmap(GpuBuffer *b);
void       gpu_buffer_update(GpuBuffer *b, const void *data);   /* the whole buffer */
void       gpu_buffer_addref(GpuBuffer *b);
/* bytes of src at src_off into dst at dst_off (on the GPU, in order with draws) */
void       gpu_buffer_copy(GpuBuffer *dst, uint32_t dst_off, GpuBuffer *src, uint32_t src_off, uint32_t bytes);

/* ── textures ────────────────────────────────────────────────────────── */

enum {
    GPU_TEX_SAMPLED = 1,       /* bound to shaders */
    GPU_TEX_TARGET  = 2,       /* a colour render target */
    GPU_TEX_DEPTH   = 4,       /* a depth / stencil target */
    GPU_TEX_CUBE    = 8,       /* six faces */
    GPU_TEX_READBACK = 16      /* CPU-readable copy destination (gpu_texture_read) */
};
typedef struct {
    uint32_t  width, height, mips;   /* mips 0: 1 */
    GpuFormat format;
    unsigned  flags;
} GpuTextureDesc;

/* init: when not NULL, one entry a subresource (face-major: face * mips + mip),
 * each with its row pitch in bytes (a row of 4x4 blocks for BC formats). */
typedef struct { const void *data; uint32_t pitch; } GpuSubresource;

GpuTexture *gpu_texture_create(const GpuTextureDesc *desc, const GpuSubresource *init);
void        gpu_texture_release(GpuTexture *t);
void        gpu_texture_addref(GpuTexture *t);
void        gpu_texture_desc(const GpuTexture *t, GpuTextureDesc *out);
void        gpu_texture_update(GpuTexture *t, uint32_t face, uint32_t mip, const void *data, uint32_t pitch);
/* The whole of src into dst (same size and format). */
void        gpu_texture_copy(GpuTexture *dst, GpuTexture *src);
/* A w x h region of src at (sx, sy) into dst at (dx, dy), mip 0. */
void        gpu_texture_copy_region(GpuTexture *dst, uint32_t dx, uint32_t dy,
                                    GpuTexture *src, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h);
/* Reads a READBACK texture's pixels (after a copy into it), waiting for the
 * GPU: `out` gets w * h pixels of 4 bytes (B, G, R, A), rows packed. With
 * no_wait, 0 when the copy has not finished yet. */
int         gpu_texture_read(GpuTexture *t, void *out, int no_wait);

/* ── samplers ────────────────────────────────────────────────────────── */

typedef enum {                 /* D3D11_TEXTURE_ADDRESS_MODE's values */
    GPU_ADDRESS_WRAP = 1, GPU_ADDRESS_MIRROR = 2, GPU_ADDRESS_CLAMP = 3, GPU_ADDRESS_BORDER = 4
} GpuAddress;
typedef struct {
    int        min_linear, mag_linear, mip_linear;
    GpuAddress u, v, w;
    float      border[4];      /* r, g, b, a */
    float      max_lod;        /* 0: no mips; 1000: all */
} GpuSamplerDesc;

GpuSampler *gpu_sampler_create(const GpuSamplerDesc *desc);
void        gpu_sampler_release(GpuSampler *s);
void        gpu_sampler_addref(GpuSampler *s);

/* ── shaders ─────────────────────────────────────────────────────────── */

typedef enum { GPU_VS = 0, GPU_PS = 1 } GpuStage;

/* Compiles HLSL (entry `entry`) for `stage`, through the disk cache
 * (ShaderCache\ beside the exe). NULL on failure (logged). */
GpuShader *gpu_shader_create(GpuStage stage, const char *hlsl, const char *entry);
void       gpu_shader_release(GpuShader *s);

/* ── input layouts ───────────────────────────────────────────────────── */

typedef struct {
    uint32_t  location;        /* the vertex shader input (ATTR<n>, or the n-th input in order) */
    GpuFormat format;
    uint32_t  slot;            /* vertex buffer slot, 0..15 */
    uint32_t  offset;          /* bytes into the vertex */
    int       per_instance;    /* advances per instance (step 1), not per vertex */
} GpuLayoutElement;

/* `vs`: a vertex shader whose inputs the elements feed (Direct3D checks the
 * layout against it; it must stay alive while the layout is used with it). */
GpuLayout *gpu_layout_create(const GpuLayoutElement *e, uint32_t n, GpuShader *vs);
void       gpu_layout_release(GpuLayout *l);

/* ── fixed-function state ────────────────────────────────────────────── */

typedef enum {                 /* D3D11_BLEND's values */
    GPU_BLEND_ZERO = 1, GPU_BLEND_ONE = 2, GPU_BLEND_SRC_COLOR = 3, GPU_BLEND_INV_SRC_COLOR = 4,
    GPU_BLEND_SRC_ALPHA = 5, GPU_BLEND_INV_SRC_ALPHA = 6, GPU_BLEND_DEST_ALPHA = 7, GPU_BLEND_INV_DEST_ALPHA = 8,
    GPU_BLEND_DEST_COLOR = 9, GPU_BLEND_INV_DEST_COLOR = 10, GPU_BLEND_SRC_ALPHA_SAT = 11,
    GPU_BLEND_FACTOR = 14, GPU_BLEND_INV_FACTOR = 15
} GpuBlend;
typedef enum {                 /* D3D11_BLEND_OP's values */
    GPU_BLENDOP_ADD = 1, GPU_BLENDOP_SUBTRACT = 2, GPU_BLENDOP_REV_SUBTRACT = 3, GPU_BLENDOP_MIN = 4, GPU_BLENDOP_MAX = 5
} GpuBlendOp;
enum { GPU_WRITE_R = 1, GPU_WRITE_G = 2, GPU_WRITE_B = 4, GPU_WRITE_A = 8, GPU_WRITE_ALL = 15 };
typedef struct {
    int        enable;
    GpuBlend   src, dst, src_alpha, dst_alpha;
    GpuBlendOp op, op_alpha;
    unsigned   write_mask;
} GpuBlendDesc;

typedef enum {                 /* D3D11_COMPARISON_FUNC's values */
    GPU_CMP_NEVER = 1, GPU_CMP_LESS = 2, GPU_CMP_EQUAL = 3, GPU_CMP_LESS_EQUAL = 4, GPU_CMP_GREATER = 5,
    GPU_CMP_NOT_EQUAL = 6, GPU_CMP_GREATER_EQUAL = 7, GPU_CMP_ALWAYS = 8
} GpuCmp;
typedef enum {                 /* D3D11_STENCIL_OP's values */
    GPU_STENCIL_KEEP = 1, GPU_STENCIL_ZERO = 2, GPU_STENCIL_REPLACE = 3, GPU_STENCIL_INCR_SAT = 4,
    GPU_STENCIL_DECR_SAT = 5, GPU_STENCIL_INVERT = 6, GPU_STENCIL_INCR = 7, GPU_STENCIL_DECR = 8
} GpuStencilOp;
typedef struct {
    int          depth_enable, depth_write;
    GpuCmp       depth_func;
    int          stencil_enable;
    uint8_t      stencil_read_mask, stencil_write_mask;
    GpuCmp       stencil_func;             /* both faces alike */
    GpuStencilOp stencil_fail, stencil_depth_fail, stencil_pass;
} GpuDepthDesc;

typedef enum { GPU_CULL_NONE = 1, GPU_CULL_FRONT = 2, GPU_CULL_BACK = 3 } GpuCull;
typedef struct {
    GpuCull cull;
    int     front_ccw;
    int     depth_clip;
    int     scissor;
} GpuRasterDesc;

GpuBlendState  *gpu_blend_create(const GpuBlendDesc *d);
GpuDepthState  *gpu_depth_create(const GpuDepthDesc *d);
GpuRasterState *gpu_raster_create(const GpuRasterDesc *d);
void            gpu_blend_release(GpuBlendState *s);
void            gpu_depth_release(GpuDepthState *s);
void            gpu_raster_release(GpuRasterState *s);

/* ── the context ─────────────────────────────────────────────────────── */

typedef enum {                 /* D3D11_PRIMITIVE_TOPOLOGY's values */
    GPU_TOPO_POINTS = 1, GPU_TOPO_LINES = 2, GPU_TOPO_LINE_STRIP = 3, GPU_TOPO_TRIANGLES = 4, GPU_TOPO_TRIANGLE_STRIP = 5
} GpuTopology;

typedef struct { float x, y, w, h, min_z, max_z; } GpuViewport;

/* NULL target: none (depth only), NULL depth: none. */
void gpu_set_targets(GpuTexture *target, GpuTexture *depth);
void gpu_set_viewport(const GpuViewport *vp);
void gpu_set_scissor(int x, int y, int w, int h);
void gpu_set_blend(GpuBlendState *s);            /* NULL: no blending, all channels */
void gpu_set_depth(GpuDepthState *s, uint32_t stencil_ref);   /* NULL: no depth test */
void gpu_set_raster(GpuRasterState *s);          /* NULL: no culling, depth clip */
void gpu_set_layout(GpuLayout *l);               /* NULL: no vertex input (SV_VertexID draws) */
void gpu_set_vertex_buffers(uint32_t first, uint32_t n, GpuBuffer *const *b,
                            const uint32_t *strides, const uint32_t *offsets);
void gpu_set_index_buffer(GpuBuffer *b);         /* 32-bit indices */
void gpu_set_topology(GpuTopology t);
void gpu_set_shaders(GpuShader *vs, GpuShader *ps);
/* A constant buffer for `stage` in register b<slot>: constants (float4s)
 * [first, first + count) of `b` (first a multiple of 16; count 0: all). */
void gpu_set_cbuffer(GpuStage stage, uint32_t slot, GpuBuffer *b, uint32_t first, uint32_t count);
/* Pixel-shader textures t<first>.. and samplers s<first>.. (NULL: none). */
void gpu_set_textures(uint32_t first, uint32_t n, GpuTexture *const *t);
void gpu_set_samplers(uint32_t first, uint32_t n, GpuSampler *const *s);

void gpu_clear_target(GpuTexture *t, const float rgba[4]);
enum { GPU_CLEAR_DEPTH = 1, GPU_CLEAR_STENCIL = 2 };
void gpu_clear_depth(GpuTexture *t, unsigned flags, float z, uint8_t stencil);

void gpu_draw(uint32_t vertices, uint32_t first);
void gpu_draw_indexed(uint32_t indices, uint32_t first_index, int32_t base_vertex);
void gpu_draw_indexed_instanced(uint32_t indices, uint32_t instances, uint32_t first_index, int32_t base_vertex);

/* ── swap chains (one per window) ────────────────────────────────────── */

GpuSwapchain *gpu_swapchain_create(void *window);
void          gpu_swapchain_release(GpuSwapchain *s);
/* The back buffer to draw this frame into (a colour target; not to be kept
 * past the present). Resizes to the window first when asked to. */
GpuTexture   *gpu_swapchain_begin(GpuSwapchain *s, int resize);
void          gpu_swapchain_present(GpuSwapchain *s, int vsync);

/* ── Direct3D 11 only ────────────────────────────────────────────────── */

/* The device, context and a texture's objects (ID3D11Device *, ...), NULL
 * under Vulkan. */
void *gpu_d3d11_device(void);
void *gpu_d3d11_context(void);
void *gpu_d3d11_texture(GpuTexture *t);          /* ID3D11Texture2D * */
void *gpu_d3d11_srv(GpuTexture *t);              /* ID3D11ShaderResourceView * */
void *gpu_d3d11_swapchain(GpuSwapchain *s);      /* IDXGISwapChain * */
void *gpu_d3d11_shader(GpuShader *s);            /* ID3D11VertexShader * or ID3D11PixelShader * */
void *gpu_d3d11_sampler(GpuSampler *s);          /* ID3D11SamplerState * */
/* A texture wrapping an existing ID3D11Texture2D (the pushbuffer renderer's
 * surfaces), with the given SRV (may be NULL). Takes its own references. */
GpuTexture *gpu_d3d11_wrap_texture(void *tex, void *srv);
/* HLSL through the backend's disk cache (ID3DBlob ** in `blob`), for the
 * pushbuffer renderer's own shaders; an HRESULT. */
long gpu_d3d11_compile(const char *src, const char *entry, const char *profile, void **blob);

#ifdef __cplusplus
}
#endif

#endif
