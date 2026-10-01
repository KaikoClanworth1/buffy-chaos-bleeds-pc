# The Android build: libbuffy.so, loaded by a NativeActivity (AndroidManifest.xml).
#   bash port/android/build_apk.sh    (configures this with the NDK's toolchain)
#
# Milestone 0: the GPU layer's Vulkan backend on the device (src/android_main.c
# clears the screen through gpu.h). The game itself comes next.
cmake_minimum_required(VERSION 3.20)
project(buffy_android C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

get_filename_component(PORT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
get_filename_component(XBOXRECOMP_DIR "${PORT_DIR}/../xboxrecomp" ABSOLUTE)

# the GPU layer (gpu.c dispatches straight to Vulkan off Windows)
add_subdirectory(${XBOXRECOMP_DIR}/src/gpu ${CMAKE_BINARY_DIR}/gpu)

# the NDK's native activity glue
add_library(native_app_glue STATIC ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue)

add_library(buffy SHARED src/android_main.c)
target_link_libraries(buffy PRIVATE xbox_gpu native_app_glue android log)
# ANativeActivity_onCreate lives in the glue: keep it exported
set_target_properties(buffy PROPERTIES LINK_FLAGS "-u ANativeActivity_onCreate")
# 16 KB pages (Android 15+ devices may use them)
target_link_options(buffy PRIVATE "-Wl,-z,max-page-size=16384")
