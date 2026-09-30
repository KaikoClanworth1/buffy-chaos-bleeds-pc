"""Writes gpu.c (the GPU layer's dispatch to its backends) from the list below.

    python gen_dispatch.py        (run in this folder after changing gpu.h)

Every function here exists in both backends as dx_<name> (gpu_d3d11.c) and
vk_<name> (gpu_vulkan.c); gpu.c calls the one gpu_init chose.
"""
SIGS = [
    # (return, name, params, defaults-on-failure)
    ("const char *", "adapter_name", "void", '""'),
    ("void", "flush", "void", None),
    ("GpuBuffer *", "buffer_create", "uint32_t bytes, unsigned bind, GpuUsage usage, const void *init", "NULL"),
    ("void", "buffer_release", "GpuBuffer *b", None),
    ("void *", "buffer_map", "GpuBuffer *b, GpuMap how", "NULL"),
    ("void", "buffer_unmap", "GpuBuffer *b", None),
    ("void", "buffer_update", "GpuBuffer *b, const void *data", None),
    ("void", "buffer_addref", "GpuBuffer *b", None),
    ("void", "buffer_copy", "GpuBuffer *dst, uint32_t dst_off, GpuBuffer *src, uint32_t src_off, uint32_t bytes", None),
    ("GpuTexture *", "texture_create", "const GpuTextureDesc *desc, const GpuSubresource *init", "NULL"),
    ("void", "texture_release", "GpuTexture *t", None),
    ("void", "texture_addref", "GpuTexture *t", None),
    ("void", "texture_desc", "const GpuTexture *t, GpuTextureDesc *out", None),
    ("void", "texture_update", "GpuTexture *t, uint32_t face, uint32_t mip, const void *data, uint32_t pitch", None),
    ("void", "texture_copy", "GpuTexture *dst, GpuTexture *src", None),
    ("void", "texture_copy_region", "GpuTexture *dst, uint32_t dx, uint32_t dy, GpuTexture *src, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h", None),
    ("int", "texture_read", "GpuTexture *t, void *out, int no_wait", "0"),
    ("GpuSampler *", "sampler_create", "const GpuSamplerDesc *desc", "NULL"),
    ("void", "sampler_release", "GpuSampler *s", None),
    ("void", "sampler_addref", "GpuSampler *s", None),
    ("GpuShader *", "shader_create", "GpuStage stage, const char *hlsl, const char *entry", "NULL"),
    ("void", "shader_release", "GpuShader *s", None),
    ("GpuLayout *", "layout_create", "const GpuLayoutElement *e, uint32_t n, GpuShader *vs", "NULL"),
    ("void", "layout_release", "GpuLayout *l", None),
    ("GpuBlendState *", "blend_create", "const GpuBlendDesc *d", "NULL"),
    ("GpuDepthState *", "depth_create", "const GpuDepthDesc *d", "NULL"),
    ("GpuRasterState *", "raster_create", "const GpuRasterDesc *d", "NULL"),
    ("void", "blend_release", "GpuBlendState *s", None),
    ("void", "depth_release", "GpuDepthState *s", None),
    ("void", "raster_release", "GpuRasterState *s", None),
    ("void", "set_targets", "GpuTexture *target, GpuTexture *depth", None),
    ("void", "set_viewport", "const GpuViewport *vp", None),
    ("void", "set_scissor", "int x, int y, int w, int h", None),
    ("void", "set_blend", "GpuBlendState *s", None),
    ("void", "set_depth", "GpuDepthState *s, uint32_t stencil_ref", None),
    ("void", "set_raster", "GpuRasterState *s", None),
    ("void", "set_layout", "GpuLayout *l", None),
    ("void", "set_vertex_buffers", "uint32_t first, uint32_t n, GpuBuffer *const *b, const uint32_t *strides, const uint32_t *offsets", None),
    ("void", "set_index_buffer", "GpuBuffer *b", None),
    ("void", "set_topology", "GpuTopology t", None),
    ("void", "set_shaders", "GpuShader *vs, GpuShader *ps", None),
    ("void", "set_cbuffer", "GpuStage stage, uint32_t slot, GpuBuffer *b, uint32_t first, uint32_t count", None),
    ("void", "set_textures", "uint32_t first, uint32_t n, GpuTexture *const *t", None),
    ("void", "set_samplers", "uint32_t first, uint32_t n, GpuSampler *const *s", None),
    ("void", "clear_target", "GpuTexture *t, const float rgba[4]", None),
    ("void", "clear_depth", "GpuTexture *t, unsigned flags, float z, uint8_t stencil", None),
    ("void", "draw", "uint32_t vertices, uint32_t first", None),
    ("void", "draw_indexed", "uint32_t indices, uint32_t first_index, int32_t base_vertex", None),
    ("void", "draw_indexed_instanced", "uint32_t indices, uint32_t instances, uint32_t first_index, int32_t base_vertex", None),
    ("GpuSwapchain *", "swapchain_create", "void *window", "NULL"),
    ("void", "swapchain_release", "GpuSwapchain *s", None),
    ("GpuTexture *", "swapchain_begin", "GpuSwapchain *s, int resize", "NULL"),
    ("void", "swapchain_present", "GpuSwapchain *s, int vsync", None),
]

# Direct3D-only accessors: the D3D11 backend's, NULL under Vulkan.
D3D_ONLY = [
    ("void *", "device", "void"),
    ("void *", "context", "void"),
    ("void *", "texture", "GpuTexture *t"),
    ("void *", "srv", "GpuTexture *t"),
    ("void *", "swapchain", "GpuSwapchain *s"),
    ("GpuTexture *", "wrap_texture", "void *tex, void *srv"),
    ("void *", "shader", "GpuShader *s"),
    ("void *", "sampler", "GpuSampler *s"),
]


def args(params):
    if params == "void":
        return ""
    out = []
    for p in params.split(","):
        p = p.strip()
        name = p.replace("[4]", "").split()[-1].lstrip("*")
        out.append(name)
    return ", ".join(out)


def main():
    lines = [
        "/* gpu.c - the GPU layer's dispatch (see gpu.h). Written by gen_dispatch.py: edit that. */",
        '#include "gpu.h"',
        "",
        "/* Without Direct3D 11 (not Windows: Android), every call is Vulkan's. */",
        "#if !defined(_WIN32) && !defined(GPU_NO_D3D11)",
        "#define GPU_NO_D3D11",
        "#endif",
        "",
        "int vk_init(void *window);",
    ]
    for ret, name, params, _ in SIGS:
        lines.append(f"{ret} vk_{name}({params});")
    lines.append("")
    lines.append("#ifdef GPU_NO_D3D11")
    lines.append("static int dx_init(void *window) { (void)window; return 0; }")
    for ret, name, params, _ in SIGS:
        lines.append(f"#define dx_{name} vk_{name}")
    for ret, name, params in D3D_ONLY:
        unused = "".join(f"(void){a}; " for a in args(params).split(", ") if a)
        lines.append(f"static {ret} dx_{name}({params}) {{ {unused}return NULL; }}")
    lines.append("#else")
    lines.append("int dx_init(void *window);")
    for ret, name, params, _ in SIGS:
        lines.append(f"{ret} dx_{name}({params});")
    for ret, name, params in D3D_ONLY:
        lines.append(f"{ret} dx_{name}({params});")
    lines.append("#endif")
    lines += [
        "",
        "#ifdef GPU_NO_D3D11",
        "static GpuBackend s_backend = GPU_BACKEND_VULKAN;",
        "#else",
        "static GpuBackend s_backend = GPU_BACKEND_D3D11;",
        "#endif",
        "",
        "int gpu_init(GpuBackend backend, void *window)",
        "{",
        "    s_backend = backend;",
        "    return backend == GPU_BACKEND_VULKAN ? vk_init(window) : dx_init(window);",
        "}",
        "",
        "GpuBackend gpu_backend(void)",
        "{",
        "    return s_backend;",
        "}",
        "",
    ]
    for ret, name, params, _ in SIGS:
        call = f"({args(params)})"
        body = f"s_backend == GPU_BACKEND_VULKAN ? vk_{name}{call} : dx_{name}{call}"
        lines.append(f"{ret} gpu_{name}({params})")
        lines.append("{")
        if ret == "void":
            lines.append(f"    if (s_backend == GPU_BACKEND_VULKAN) vk_{name}{call}; else dx_{name}{call};")
        else:
            lines.append(f"    return {body};")
        lines.append("}")
        lines.append("")
    for ret, name, params in D3D_ONLY:
        lines.append(f"{ret} gpu_d3d11_{name}({params})")
        lines.append("{")
        lines.append(f"    return s_backend == GPU_BACKEND_D3D11 ? dx_{name}({args(params)}) : NULL;")
        lines.append("}")
        lines.append("")
    lines.append("#ifdef GPU_NO_D3D11")
    lines.append("static long dx_compile(const char *src, const char *entry, const char *profile, void **blob)")
    lines.append("{ (void)src; (void)entry; (void)profile; *blob = NULL; return -1; }")
    lines.append("#else")
    lines.append("long dx_compile(const char *src, const char *entry, const char *profile, void **blob);")
    lines.append("#endif")
    lines.append("")
    lines.append("/* The pushbuffer renderer's own compiles (Direct3D only): the backend's cache. */")
    lines.append("long gpu_d3d11_compile(const char *src, const char *entry, const char *profile, void **blob)")
    lines.append("{")
    lines.append("    return dx_compile(src, entry, profile, blob);")
    lines.append("}")
    open("gpu.c", "w", newline="\n").write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
