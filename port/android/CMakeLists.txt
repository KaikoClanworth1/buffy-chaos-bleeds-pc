# The Android build: libbuffy.so, loaded by a NativeActivity (AndroidManifest.xml).
#   bash port/android/build_apk.sh    (configures this with the NDK's toolchain)
#
# The same game as the Windows build -- the recompiled code (src/recomp/gen),
# the xboxrecomp kernel and the port's sources -- with the Windows-only parts
# swapped for Android ones (src/android_*.c): Win32 calls the port makes go
# through xboxrecomp's POSIX shim (platform/win32_compat) and include/windows.h
# here, and the renderer draws through the GPU layer's Vulkan backend.
cmake_minimum_required(VERSION 3.20)
project(buffy_android C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

get_filename_component(PORT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
get_filename_component(XBOXRECOMP_DIR "${PORT_DIR}/../xboxrecomp" ABSOLUTE)
set(XR_SRC ${XBOXRECOMP_DIR}/src)

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX BUFFY_ANDROID XBOXRECOMP_NO_GPU_EMULATION)
# MSVC-isms the sources rely on (__stdcall, __declspec, anonymous structs)
add_compile_options(-fsigned-char -ffp-contract=off -ferror-limit=0 -fms-extensions -Wno-microsoft -fno-strict-aliasing -fwrapv
                    -Wno-unused-parameter -Wno-parentheses-equality -Wno-unused-variable
                    -Wno-unused-but-set-variable -Wno-unused-function -Wno-unused-label
                    -Wno-incompatible-pointer-types-discards-qualifiers -Wno-pointer-sign
                    # with -fshort-wchar, LLVM turns a 16-bit "count to the 0" loop (w16_wcslen's
                    # own) into a call to libc's wcslen -- bionic's counts 32-bit characters, so
                    # every wide length came out halved (texture pack paths cut short)
                    -fno-builtin-wcslen)

# include/windows.h (and friends): the Win32 the port uses, on Android
set(ANDROID_INC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# ...and the MSVC C runtime's _s functions, in every file but the generated code
# (and 16-bit wchar_t, as on Windows: include/android_wide.h)
set(FORCE_WIN32 -fshort-wchar -include ${ANDROID_INC}/android_win32.h)

# the GPU layer (gpu.c dispatches straight to Vulkan off Windows)
add_subdirectory(${XR_SRC}/gpu ${CMAKE_BINARY_DIR}/gpu)

# xboxrecomp's POSIX Win32 shim
add_library(platform STATIC ${XR_SRC}/platform/win32_compat.c)
target_include_directories(platform PUBLIC ${ANDROID_INC} ${XR_SRC} ${XR_SRC}/platform)

# the kernel (and the native renderer, included by nv2a_pb_exec.c)
add_library(xbox_kernel STATIC
    ${XR_SRC}/kernel/kernel_path.c ${XR_SRC}/kernel/kernel_pool.c ${XR_SRC}/kernel/kernel_rtl.c
    ${XR_SRC}/kernel/kernel_memory.c ${XR_SRC}/kernel/kernel_file.c ${XR_SRC}/kernel/kernel_thread.c
    ${XR_SRC}/kernel/kernel_sync.c ${XR_SRC}/kernel/kernel_hal.c ${XR_SRC}/kernel/kernel_xbox.c
    ${XR_SRC}/kernel/kernel_ob.c ${XR_SRC}/kernel/kernel_io.c ${XR_SRC}/kernel/kernel_crypto.c
    ${XR_SRC}/kernel/kernel_thunks.c ${XR_SRC}/kernel/kernel_bridge.c ${XR_SRC}/kernel/xbox_memory_layout.c
    ${XR_SRC}/kernel/icall_feedback.c ${XR_SRC}/kernel/recomp_trace.c
    ${XR_SRC}/kernel/nv2a_pb_scan.c ${XR_SRC}/kernel/nv2a_pb_exec.c)
target_include_directories(xbox_kernel PUBLIC ${XR_SRC}/kernel ${XR_SRC})
target_link_libraries(xbox_kernel PUBLIC platform xbox_gpu)
target_include_directories(xbox_kernel PUBLIC ${ANDROID_INC})
target_compile_options(xbox_kernel PRIVATE -O2 ${FORCE_WIN32})

# custom GPU drivers: libadrenotools (BSD-2-Clause) and its hook libraries,
# which go beside libbuffy.so in the APK (build_apk.sh)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/third_party/libadrenotools ${CMAKE_BINARY_DIR}/adrenotools EXCLUDE_FROM_ALL)

# the NDK's native activity glue
add_library(native_app_glue STATIC ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue)

# the recompiled game: big translation units, built on their own
file(GLOB RECOMP_GEN_SOURCES ${PORT_DIR}/src/recomp/gen/recomp_*.c)
add_library(buffy_gen STATIC ${RECOMP_GEN_SOURCES} ${PORT_DIR}/src/recomp_manual.c)
target_include_directories(buffy_gen PRIVATE ${PORT_DIR}/src ${PORT_DIR}/src/recomp ${XBOXRECOMP_DIR}/include)
target_link_libraries(buffy_gen PRIVATE xbox_kernel)
target_compile_options(buffy_gen PRIVATE -O2 -w -fshort-wchar)

# the port: the Windows build's sources, less the Windows-only ones (their
# Android counterparts are src/android_*.c)
set(PORT_SOURCES
    main.c buffy_crt.c buffy_d3dx.c buffy_frame.c buffy_settings.c buffy_menu.c
    buffy_gpuwait.c buffy_mods.c buffy_sfxtrace.c buffy_preload.c
    buffy_saves.c buffy_savefmt.c buffy_native.c buffy_dsound.c buffy_input.c buffy_import.c)
# not here: buffy_movie.c (src/android_movie.c); (src/android_stubs.c) buffy_export.c buffy_debug.c
# buffy_profile.c buffy_xbecheck.c buffy_platform.c
list(TRANSFORM PORT_SOURCES PREPEND ${PORT_DIR}/src/)

add_library(buffy SHARED ${PORT_SOURCES}
    src/android_main.c src/android_win32.c src/android_wide.c src/android_stubs.c src/android_input.c
    src/android_touch.c src/android_activity.c src/android_canvas.c src/android_driver.c src/android_movie.c)
target_include_directories(buffy PRIVATE ${PORT_DIR}/src ${PORT_DIR}/src/recomp ${XBOXRECOMP_DIR}/include)
target_compile_options(buffy PRIVATE ${FORCE_WIN32})
target_include_directories(buffy PRIVATE ${XR_SRC}/gpu ${XBOXRECOMP_DIR}/third_party/Vulkan-Headers/include)
add_dependencies(buffy main_hook file_redirect_hook gsl_alloc_hook hook_impl)
target_link_libraries(buffy PRIVATE buffy_gen xbox_kernel xbox_gpu native_app_glue adrenotools android log aaudio mediandk jnigraphics m dl)
# ANativeActivity_onCreate lives in the glue: keep it exported
set_target_properties(buffy PROPERTIES LINK_FLAGS "-u ANativeActivity_onCreate -u buffy_activity_create")
# 16 KB pages (Android 15+ devices may use them)
target_link_options(buffy PRIVATE "-Wl,-z,max-page-size=16384"
    "-Wl,--no-dependent-libraries")   # (the Windows sources' #pragma comment(lib, ...))
