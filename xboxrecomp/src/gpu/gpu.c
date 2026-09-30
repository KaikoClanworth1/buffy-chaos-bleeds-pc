/* gpu.c - the GPU layer's dispatch (see gpu.h). Written by gen_dispatch.py: edit that. */
#include "gpu.h"

/* Without Direct3D 11 (not Windows: Android), every call is Vulkan's. */
#if !defined(_WIN32) && !defined(GPU_NO_D3D11)
#define GPU_NO_D3D11
#endif

int vk_init(void *window);
const char * vk_adapter_name(void);
void vk_flush(void);
GpuBuffer * vk_buffer_create(uint32_t bytes, unsigned bind, GpuUsage usage, const void *init);
void vk_buffer_release(GpuBuffer *b);
void * vk_buffer_map(GpuBuffer *b, GpuMap how);
void vk_buffer_unmap(GpuBuffer *b);
void vk_buffer_update(GpuBuffer *b, const void *data);
void vk_buffer_addref(GpuBuffer *b);
void vk_buffer_copy(GpuBuffer *dst, uint32_t dst_off, GpuBuffer *src, uint32_t src_off, uint32_t bytes);
GpuTexture * vk_texture_create(const GpuTextureDesc *desc, const GpuSubresource *init);
void vk_texture_release(GpuTexture *t);
void vk_texture_addref(GpuTexture *t);
void vk_texture_desc(const GpuTexture *t, GpuTextureDesc *out);
void vk_texture_update(GpuTexture *t, uint32_t face, uint32_t mip, const void *data, uint32_t pitch);
void vk_texture_copy(GpuTexture *dst, GpuTexture *src);
void vk_texture_copy_region(GpuTexture *dst, uint32_t dx, uint32_t dy, GpuTexture *src, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h);
int vk_texture_read(GpuTexture *t, void *out, int no_wait);
GpuSampler * vk_sampler_create(const GpuSamplerDesc *desc);
void vk_sampler_release(GpuSampler *s);
void vk_sampler_addref(GpuSampler *s);
GpuShader * vk_shader_create(GpuStage stage, const char *hlsl, const char *entry);
void vk_shader_release(GpuShader *s);
GpuLayout * vk_layout_create(const GpuLayoutElement *e, uint32_t n, GpuShader *vs);
void vk_layout_release(GpuLayout *l);
GpuBlendState * vk_blend_create(const GpuBlendDesc *d);
GpuDepthState * vk_depth_create(const GpuDepthDesc *d);
GpuRasterState * vk_raster_create(const GpuRasterDesc *d);
void vk_blend_release(GpuBlendState *s);
void vk_depth_release(GpuDepthState *s);
void vk_raster_release(GpuRasterState *s);
void vk_set_targets(GpuTexture *target, GpuTexture *depth);
void vk_set_viewport(const GpuViewport *vp);
void vk_set_scissor(int x, int y, int w, int h);
void vk_set_blend(GpuBlendState *s);
void vk_set_depth(GpuDepthState *s, uint32_t stencil_ref);
void vk_set_raster(GpuRasterState *s);
void vk_set_layout(GpuLayout *l);
void vk_set_vertex_buffers(uint32_t first, uint32_t n, GpuBuffer *const *b, const uint32_t *strides, const uint32_t *offsets);
void vk_set_index_buffer(GpuBuffer *b);
void vk_set_topology(GpuTopology t);
void vk_set_shaders(GpuShader *vs, GpuShader *ps);
void vk_set_cbuffer(GpuStage stage, uint32_t slot, GpuBuffer *b, uint32_t first, uint32_t count);
void vk_set_textures(uint32_t first, uint32_t n, GpuTexture *const *t);
void vk_set_samplers(uint32_t first, uint32_t n, GpuSampler *const *s);
void vk_clear_target(GpuTexture *t, const float rgba[4]);
void vk_clear_depth(GpuTexture *t, unsigned flags, float z, uint8_t stencil);
void vk_draw(uint32_t vertices, uint32_t first);
void vk_draw_indexed(uint32_t indices, uint32_t first_index, int32_t base_vertex);
void vk_draw_indexed_instanced(uint32_t indices, uint32_t instances, uint32_t first_index, int32_t base_vertex);
GpuSwapchain * vk_swapchain_create(void *window);
void vk_swapchain_release(GpuSwapchain *s);
GpuTexture * vk_swapchain_begin(GpuSwapchain *s, int resize);
void vk_swapchain_present(GpuSwapchain *s, int vsync);

#ifdef GPU_NO_D3D11
static int dx_init(void *window) { (void)window; return 0; }
#define dx_adapter_name vk_adapter_name
#define dx_flush vk_flush
#define dx_buffer_create vk_buffer_create
#define dx_buffer_release vk_buffer_release
#define dx_buffer_map vk_buffer_map
#define dx_buffer_unmap vk_buffer_unmap
#define dx_buffer_update vk_buffer_update
#define dx_buffer_addref vk_buffer_addref
#define dx_buffer_copy vk_buffer_copy
#define dx_texture_create vk_texture_create
#define dx_texture_release vk_texture_release
#define dx_texture_addref vk_texture_addref
#define dx_texture_desc vk_texture_desc
#define dx_texture_update vk_texture_update
#define dx_texture_copy vk_texture_copy
#define dx_texture_copy_region vk_texture_copy_region
#define dx_texture_read vk_texture_read
#define dx_sampler_create vk_sampler_create
#define dx_sampler_release vk_sampler_release
#define dx_sampler_addref vk_sampler_addref
#define dx_shader_create vk_shader_create
#define dx_shader_release vk_shader_release
#define dx_layout_create vk_layout_create
#define dx_layout_release vk_layout_release
#define dx_blend_create vk_blend_create
#define dx_depth_create vk_depth_create
#define dx_raster_create vk_raster_create
#define dx_blend_release vk_blend_release
#define dx_depth_release vk_depth_release
#define dx_raster_release vk_raster_release
#define dx_set_targets vk_set_targets
#define dx_set_viewport vk_set_viewport
#define dx_set_scissor vk_set_scissor
#define dx_set_blend vk_set_blend
#define dx_set_depth vk_set_depth
#define dx_set_raster vk_set_raster
#define dx_set_layout vk_set_layout
#define dx_set_vertex_buffers vk_set_vertex_buffers
#define dx_set_index_buffer vk_set_index_buffer
#define dx_set_topology vk_set_topology
#define dx_set_shaders vk_set_shaders
#define dx_set_cbuffer vk_set_cbuffer
#define dx_set_textures vk_set_textures
#define dx_set_samplers vk_set_samplers
#define dx_clear_target vk_clear_target
#define dx_clear_depth vk_clear_depth
#define dx_draw vk_draw
#define dx_draw_indexed vk_draw_indexed
#define dx_draw_indexed_instanced vk_draw_indexed_instanced
#define dx_swapchain_create vk_swapchain_create
#define dx_swapchain_release vk_swapchain_release
#define dx_swapchain_begin vk_swapchain_begin
#define dx_swapchain_present vk_swapchain_present
static void * dx_device(void) { return NULL; }
static void * dx_context(void) { return NULL; }
static void * dx_texture(GpuTexture *t) { (void)t; return NULL; }
static void * dx_srv(GpuTexture *t) { (void)t; return NULL; }
static void * dx_swapchain(GpuSwapchain *s) { (void)s; return NULL; }
static GpuTexture * dx_wrap_texture(void *tex, void *srv) { (void)tex; (void)srv; return NULL; }
static void * dx_shader(GpuShader *s) { (void)s; return NULL; }
static void * dx_sampler(GpuSampler *s) { (void)s; return NULL; }
#else
int dx_init(void *window);
const char * dx_adapter_name(void);
void dx_flush(void);
GpuBuffer * dx_buffer_create(uint32_t bytes, unsigned bind, GpuUsage usage, const void *init);
void dx_buffer_release(GpuBuffer *b);
void * dx_buffer_map(GpuBuffer *b, GpuMap how);
void dx_buffer_unmap(GpuBuffer *b);
void dx_buffer_update(GpuBuffer *b, const void *data);
void dx_buffer_addref(GpuBuffer *b);
void dx_buffer_copy(GpuBuffer *dst, uint32_t dst_off, GpuBuffer *src, uint32_t src_off, uint32_t bytes);
GpuTexture * dx_texture_create(const GpuTextureDesc *desc, const GpuSubresource *init);
void dx_texture_release(GpuTexture *t);
void dx_texture_addref(GpuTexture *t);
void dx_texture_desc(const GpuTexture *t, GpuTextureDesc *out);
void dx_texture_update(GpuTexture *t, uint32_t face, uint32_t mip, const void *data, uint32_t pitch);
void dx_texture_copy(GpuTexture *dst, GpuTexture *src);
void dx_texture_copy_region(GpuTexture *dst, uint32_t dx, uint32_t dy, GpuTexture *src, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h);
int dx_texture_read(GpuTexture *t, void *out, int no_wait);
GpuSampler * dx_sampler_create(const GpuSamplerDesc *desc);
void dx_sampler_release(GpuSampler *s);
void dx_sampler_addref(GpuSampler *s);
GpuShader * dx_shader_create(GpuStage stage, const char *hlsl, const char *entry);
void dx_shader_release(GpuShader *s);
GpuLayout * dx_layout_create(const GpuLayoutElement *e, uint32_t n, GpuShader *vs);
void dx_layout_release(GpuLayout *l);
GpuBlendState * dx_blend_create(const GpuBlendDesc *d);
GpuDepthState * dx_depth_create(const GpuDepthDesc *d);
GpuRasterState * dx_raster_create(const GpuRasterDesc *d);
void dx_blend_release(GpuBlendState *s);
void dx_depth_release(GpuDepthState *s);
void dx_raster_release(GpuRasterState *s);
void dx_set_targets(GpuTexture *target, GpuTexture *depth);
void dx_set_viewport(const GpuViewport *vp);
void dx_set_scissor(int x, int y, int w, int h);
void dx_set_blend(GpuBlendState *s);
void dx_set_depth(GpuDepthState *s, uint32_t stencil_ref);
void dx_set_raster(GpuRasterState *s);
void dx_set_layout(GpuLayout *l);
void dx_set_vertex_buffers(uint32_t first, uint32_t n, GpuBuffer *const *b, const uint32_t *strides, const uint32_t *offsets);
void dx_set_index_buffer(GpuBuffer *b);
void dx_set_topology(GpuTopology t);
void dx_set_shaders(GpuShader *vs, GpuShader *ps);
void dx_set_cbuffer(GpuStage stage, uint32_t slot, GpuBuffer *b, uint32_t first, uint32_t count);
void dx_set_textures(uint32_t first, uint32_t n, GpuTexture *const *t);
void dx_set_samplers(uint32_t first, uint32_t n, GpuSampler *const *s);
void dx_clear_target(GpuTexture *t, const float rgba[4]);
void dx_clear_depth(GpuTexture *t, unsigned flags, float z, uint8_t stencil);
void dx_draw(uint32_t vertices, uint32_t first);
void dx_draw_indexed(uint32_t indices, uint32_t first_index, int32_t base_vertex);
void dx_draw_indexed_instanced(uint32_t indices, uint32_t instances, uint32_t first_index, int32_t base_vertex);
GpuSwapchain * dx_swapchain_create(void *window);
void dx_swapchain_release(GpuSwapchain *s);
GpuTexture * dx_swapchain_begin(GpuSwapchain *s, int resize);
void dx_swapchain_present(GpuSwapchain *s, int vsync);
void * dx_device(void);
void * dx_context(void);
void * dx_texture(GpuTexture *t);
void * dx_srv(GpuTexture *t);
void * dx_swapchain(GpuSwapchain *s);
GpuTexture * dx_wrap_texture(void *tex, void *srv);
void * dx_shader(GpuShader *s);
void * dx_sampler(GpuSampler *s);
#endif

#ifdef GPU_NO_D3D11
static GpuBackend s_backend = GPU_BACKEND_VULKAN;
#else
static GpuBackend s_backend = GPU_BACKEND_D3D11;
#endif

int gpu_init(GpuBackend backend, void *window)
{
    s_backend = backend;
    return backend == GPU_BACKEND_VULKAN ? vk_init(window) : dx_init(window);
}

GpuBackend gpu_backend(void)
{
    return s_backend;
}

const char * gpu_adapter_name(void)
{
    return s_backend == GPU_BACKEND_VULKAN ? vk_adapter_name() : dx_adapter_name();
}

void gpu_flush(void)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_flush(); else dx_flush();
}

GpuBuffer * gpu_buffer_create(uint32_t bytes, unsigned bind, GpuUsage usage, const void *init)
{
    return s_backend == GPU_BACKEND_VULKAN ? vk_buffer_create(bytes, bind, usage, init) : dx_buffer_create(bytes, bind, usage, init);
}

void gpu_buffer_release(GpuBuffer *b)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_buffer_release(b); else dx_buffer_release(b);
}

void * gpu_buffer_map(GpuBuffer *b, GpuMap how)
{
    return s_backend == GPU_BACKEND_VULKAN ? vk_buffer_map(b, how) : dx_buffer_map(b, how);
}

void gpu_buffer_unmap(GpuBuffer *b)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_buffer_unmap(b); else dx_buffer_unmap(b);
}

void gpu_buffer_update(GpuBuffer *b, const void *data)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_buffer_update(b, data); else dx_buffer_update(b, data);
}

void gpu_buffer_addref(GpuBuffer *b)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_buffer_addref(b); else dx_buffer_addref(b);
}

void gpu_buffer_copy(GpuBuffer *dst, uint32_t dst_off, GpuBuffer *src, uint32_t src_off, uint32_t bytes)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_buffer_copy(dst, dst_off, src, src_off, bytes); else dx_buffer_copy(dst, dst_off, src, src_off, bytes);
}

GpuTexture * gpu_texture_create(const GpuTextureDesc *desc, const GpuSubresource *init)
{
    return s_backend == GPU_BACKEND_VULKAN ? vk_texture_create(desc, init) : dx_texture_create(desc, init);
}

void gpu_texture_release(GpuTexture *t)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_texture_release(t); else dx_texture_release(t);
}

void gpu_texture_addref(GpuTexture *t)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_texture_addref(t); else dx_texture_addref(t);
}

void gpu_texture_desc(const GpuTexture *t, GpuTextureDesc *out)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_texture_desc(t, out); else dx_texture_desc(t, out);
}

void gpu_texture_update(GpuTexture *t, uint32_t face, uint32_t mip, const void *data, uint32_t pitch)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_texture_update(t, face, mip, data, pitch); else dx_texture_update(t, face, mip, data, pitch);
}

void gpu_texture_copy(GpuTexture *dst, GpuTexture *src)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_texture_copy(dst, src); else dx_texture_copy(dst, src);
}

void gpu_texture_copy_region(GpuTexture *dst, uint32_t dx, uint32_t dy, GpuTexture *src, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_texture_copy_region(dst, dx, dy, src, sx, sy, w, h); else dx_texture_copy_region(dst, dx, dy, src, sx, sy, w, h);
}

int gpu_texture_read(GpuTexture *t, void *out, int no_wait)
{
    return s_backend == GPU_BACKEND_VULKAN ? vk_texture_read(t, out, no_wait) : dx_texture_read(t, out, no_wait);
}

GpuSampler * gpu_sampler_create(const GpuSamplerDesc *desc)
{
    return s_backend == GPU_BACKEND_VULKAN ? vk_sampler_create(desc) : dx_sampler_create(desc);
}

void gpu_sampler_release(GpuSampler *s)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_sampler_release(s); else dx_sampler_release(s);
}

void gpu_sampler_addref(GpuSampler *s)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_sampler_addref(s); else dx_sampler_addref(s);
}

GpuShader * gpu_shader_create(GpuStage stage, const char *hlsl, const char *entry)
{
    return s_backend == GPU_BACKEND_VULKAN ? vk_shader_create(stage, hlsl, entry) : dx_shader_create(stage, hlsl, entry);
}

void gpu_shader_release(GpuShader *s)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_shader_release(s); else dx_shader_release(s);
}

GpuLayout * gpu_layout_create(const GpuLayoutElement *e, uint32_t n, GpuShader *vs)
{
    return s_backend == GPU_BACKEND_VULKAN ? vk_layout_create(e, n, vs) : dx_layout_create(e, n, vs);
}

void gpu_layout_release(GpuLayout *l)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_layout_release(l); else dx_layout_release(l);
}

GpuBlendState * gpu_blend_create(const GpuBlendDesc *d)
{
    return s_backend == GPU_BACKEND_VULKAN ? vk_blend_create(d) : dx_blend_create(d);
}

GpuDepthState * gpu_depth_create(const GpuDepthDesc *d)
{
    return s_backend == GPU_BACKEND_VULKAN ? vk_depth_create(d) : dx_depth_create(d);
}

GpuRasterState * gpu_raster_create(const GpuRasterDesc *d)
{
    return s_backend == GPU_BACKEND_VULKAN ? vk_raster_create(d) : dx_raster_create(d);
}

void gpu_blend_release(GpuBlendState *s)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_blend_release(s); else dx_blend_release(s);
}

void gpu_depth_release(GpuDepthState *s)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_depth_release(s); else dx_depth_release(s);
}

void gpu_raster_release(GpuRasterState *s)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_raster_release(s); else dx_raster_release(s);
}

void gpu_set_targets(GpuTexture *target, GpuTexture *depth)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_set_targets(target, depth); else dx_set_targets(target, depth);
}

void gpu_set_viewport(const GpuViewport *vp)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_set_viewport(vp); else dx_set_viewport(vp);
}

void gpu_set_scissor(int x, int y, int w, int h)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_set_scissor(x, y, w, h); else dx_set_scissor(x, y, w, h);
}

void gpu_set_blend(GpuBlendState *s)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_set_blend(s); else dx_set_blend(s);
}

void gpu_set_depth(GpuDepthState *s, uint32_t stencil_ref)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_set_depth(s, stencil_ref); else dx_set_depth(s, stencil_ref);
}

void gpu_set_raster(GpuRasterState *s)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_set_raster(s); else dx_set_raster(s);
}

void gpu_set_layout(GpuLayout *l)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_set_layout(l); else dx_set_layout(l);
}

void gpu_set_vertex_buffers(uint32_t first, uint32_t n, GpuBuffer *const *b, const uint32_t *strides, const uint32_t *offsets)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_set_vertex_buffers(first, n, b, strides, offsets); else dx_set_vertex_buffers(first, n, b, strides, offsets);
}

void gpu_set_index_buffer(GpuBuffer *b)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_set_index_buffer(b); else dx_set_index_buffer(b);
}

void gpu_set_topology(GpuTopology t)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_set_topology(t); else dx_set_topology(t);
}

void gpu_set_shaders(GpuShader *vs, GpuShader *ps)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_set_shaders(vs, ps); else dx_set_shaders(vs, ps);
}

void gpu_set_cbuffer(GpuStage stage, uint32_t slot, GpuBuffer *b, uint32_t first, uint32_t count)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_set_cbuffer(stage, slot, b, first, count); else dx_set_cbuffer(stage, slot, b, first, count);
}

void gpu_set_textures(uint32_t first, uint32_t n, GpuTexture *const *t)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_set_textures(first, n, t); else dx_set_textures(first, n, t);
}

void gpu_set_samplers(uint32_t first, uint32_t n, GpuSampler *const *s)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_set_samplers(first, n, s); else dx_set_samplers(first, n, s);
}

void gpu_clear_target(GpuTexture *t, const float rgba[4])
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_clear_target(t, rgba); else dx_clear_target(t, rgba);
}

void gpu_clear_depth(GpuTexture *t, unsigned flags, float z, uint8_t stencil)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_clear_depth(t, flags, z, stencil); else dx_clear_depth(t, flags, z, stencil);
}

void gpu_draw(uint32_t vertices, uint32_t first)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_draw(vertices, first); else dx_draw(vertices, first);
}

void gpu_draw_indexed(uint32_t indices, uint32_t first_index, int32_t base_vertex)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_draw_indexed(indices, first_index, base_vertex); else dx_draw_indexed(indices, first_index, base_vertex);
}

void gpu_draw_indexed_instanced(uint32_t indices, uint32_t instances, uint32_t first_index, int32_t base_vertex)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_draw_indexed_instanced(indices, instances, first_index, base_vertex); else dx_draw_indexed_instanced(indices, instances, first_index, base_vertex);
}

GpuSwapchain * gpu_swapchain_create(void *window)
{
    return s_backend == GPU_BACKEND_VULKAN ? vk_swapchain_create(window) : dx_swapchain_create(window);
}

void gpu_swapchain_release(GpuSwapchain *s)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_swapchain_release(s); else dx_swapchain_release(s);
}

GpuTexture * gpu_swapchain_begin(GpuSwapchain *s, int resize)
{
    return s_backend == GPU_BACKEND_VULKAN ? vk_swapchain_begin(s, resize) : dx_swapchain_begin(s, resize);
}

void gpu_swapchain_present(GpuSwapchain *s, int vsync)
{
    if (s_backend == GPU_BACKEND_VULKAN) vk_swapchain_present(s, vsync); else dx_swapchain_present(s, vsync);
}

void * gpu_d3d11_device(void)
{
    return s_backend == GPU_BACKEND_D3D11 ? dx_device() : NULL;
}

void * gpu_d3d11_context(void)
{
    return s_backend == GPU_BACKEND_D3D11 ? dx_context() : NULL;
}

void * gpu_d3d11_texture(GpuTexture *t)
{
    return s_backend == GPU_BACKEND_D3D11 ? dx_texture(t) : NULL;
}

void * gpu_d3d11_srv(GpuTexture *t)
{
    return s_backend == GPU_BACKEND_D3D11 ? dx_srv(t) : NULL;
}

void * gpu_d3d11_swapchain(GpuSwapchain *s)
{
    return s_backend == GPU_BACKEND_D3D11 ? dx_swapchain(s) : NULL;
}

GpuTexture * gpu_d3d11_wrap_texture(void *tex, void *srv)
{
    return s_backend == GPU_BACKEND_D3D11 ? dx_wrap_texture(tex, srv) : NULL;
}

void * gpu_d3d11_shader(GpuShader *s)
{
    return s_backend == GPU_BACKEND_D3D11 ? dx_shader(s) : NULL;
}

void * gpu_d3d11_sampler(GpuSampler *s)
{
    return s_backend == GPU_BACKEND_D3D11 ? dx_sampler(s) : NULL;
}

#ifdef GPU_NO_D3D11
static long dx_compile(const char *src, const char *entry, const char *profile, void **blob)
{ (void)src; (void)entry; (void)profile; *blob = NULL; return -1; }
#else
long dx_compile(const char *src, const char *entry, const char *profile, void **blob);
#endif

/* The pushbuffer renderer's own compiles (Direct3D only): the backend's cache. */
long gpu_d3d11_compile(const char *src, const char *entry, const char *profile, void **blob)
{
    return dx_compile(src, entry, profile, blob);
}
