/*
 * gpu_vk.h - Vulkan headers as the backend uses them: loaded through volk
 * (no link-time Vulkan library; vulkan-1.dll / libvulkan.so at run time),
 * memory through VMA.
 */
#ifndef GPU_VK_H
#define GPU_VK_H

#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#if defined(__ANDROID__) && !defined(VK_USE_PLATFORM_ANDROID_KHR)
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <volk.h>
#include <vk_mem_alloc.h>

#endif
