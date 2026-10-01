# xbox_gpu: the renderer's GPU layer (gpu.h) and its backends.
#
# gpu.c dispatches to gpu_d3d11.c or gpu_vulkan.c; the Vulkan backend compiles
# HLSL to SPIR-V with glslang. spirv_check runs that over dumped shaders.

get_filename_component(XBOXRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(GLSLANG_DIR "${XBOXRECOMP_ROOT}/third_party/glslang")

# glslang: the library only (no tools, tests or optimizer), HLSL front end on.
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL ON CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(ENABLE_CTEST OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(${GLSLANG_DIR} ${CMAKE_CURRENT_BINARY_DIR}/glslang EXCLUDE_FROM_ALL)

# Vulkan: headers, volk (the loader, at run time) and VMA (memory); no SDK needed.
set(VK_HEADERS_DIR "${XBOXRECOMP_ROOT}/third_party/Vulkan-Headers/include")
set(VOLK_DIR "${XBOXRECOMP_ROOT}/third_party/volk")
set(VMA_DIR "${XBOXRECOMP_ROOT}/third_party/VulkanMemoryAllocator/include")

# Direct3D 11 on Windows only; elsewhere (Android) gpu.c sends every call to
# Vulkan. (Linux runs the Windows build under Proton.)
set(GPU_SOURCES gpu.c gpu_vulkan.c gpu_vma.cpp gpu_spirv.cpp ${VOLK_DIR}/volk.c)
if(WIN32)
    list(APPEND GPU_SOURCES gpu_d3d11.c)
endif()
add_library(xbox_gpu STATIC ${GPU_SOURCES})
target_include_directories(xbox_gpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(xbox_gpu PRIVATE ${GLSLANG_DIR} ${VK_HEADERS_DIR} ${VOLK_DIR} ${VMA_DIR})
target_compile_definitions(xbox_gpu PRIVATE ENABLE_HLSL _CRT_SECURE_NO_WARNINGS)   # (the HLSL half of glslang's API)
if(WIN32)
    target_compile_definitions(xbox_gpu PRIVATE VK_USE_PLATFORM_WIN32_KHR)
elseif(ANDROID)
    target_compile_definitions(xbox_gpu PRIVATE VK_USE_PLATFORM_ANDROID_KHR)
    target_link_libraries(xbox_gpu PRIVATE android dl)
endif()
target_link_libraries(xbox_gpu PRIVATE glslang::glslang glslang::SPIRV glslang::glslang-default-resource-limits)
set_target_properties(xbox_gpu PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

add_executable(spirv_check EXCLUDE_FROM_ALL spirv_check.c)
target_link_libraries(spirv_check PRIVATE xbox_gpu)
