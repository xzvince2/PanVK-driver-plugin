cmake_minimum_required(VERSION 3.18)
project(panvk_shim C)

set(CMAKE_C_STANDARD 11)

add_library(vulkan_panvk_shim SHARED panvk_shim.c)
target_compile_options(vulkan_panvk_shim PRIVATE -Wall -Wextra -fvisibility=hidden)

# 1 = diagnostic build (TRACE + PROF + stderr logging to latest_game.log)
# 0 = clean build (logcat only). Correctness fixes stay active in both.
set(PANVK_SHIM_DIAG 1 CACHE STRING "Shim diagnostics: 1 or 0")
target_compile_definitions(vulkan_panvk_shim PRIVATE PANVK_SHIM_DIAG=${PANVK_SHIM_DIAG})

target_link_libraries(vulkan_panvk_shim PRIVATE log dl android)