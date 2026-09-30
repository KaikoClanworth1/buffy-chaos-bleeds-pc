/*
 * gpu_spirv.h - HLSL to SPIR-V, at run time (glslang), for the Vulkan backend.
 *
 * The renderer writes its shaders as HLSL while it runs (the NV2A vertex
 * programs and register combiners it translates); Direct3D 11 compiles them
 * with D3DCompile, Vulkan with this. The register numbers become Vulkan
 * bindings in the one descriptor set every draw uses:
 *
 *   b0, b1  constant buffers     -> bindings 0, 1
 *   t0..t7  textures (2D, cube)  -> bindings 2..9
 *   s0..s3  samplers             -> bindings 10..13
 *
 * and a vertex shader's inputs take locations in declaration order
 * (ATTR0..15 -> 0..15).
 */
#ifndef GPU_SPIRV_H
#define GPU_SPIRV_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GPU_SPIRV_BIND_CB     0
#define GPU_SPIRV_BIND_TEX    2
#define GPU_SPIRV_BIND_SAMPLER 10

enum { GPU_STAGE_VERTEX = 0, GPU_STAGE_PIXEL = 1 };

/* Compiles `src` (entry point `entry`) for `stage`. On success returns 1 and
 * a malloc'd array of SPIR-V words in *words (free() it); on failure 0 and
 * the compiler's message in err. */
int gpu_hlsl_to_spirv(const char *src, const char *entry, int stage,
                      uint32_t **words, size_t *count, char *err, size_t errn);

#ifdef __cplusplus
}
#endif

#endif
