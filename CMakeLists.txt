cmake_minimum_required(VERSION 3.20)
project(outsider C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build type defaults to Debug
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# PlayStation 5 (ps5-payload-sdk): a self-contained build, see cmake/ps5.cmake.
option(RMMZ_PS5 "Build for the PlayStation 5 with the ps5-payload-sdk" OFF)
if(RMMZ_PS5)
    include(cmake/ps5.cmake)
    return()
endif()

# Options
include(CMakeDependentOption)
option(RMMZ_BUILD_TESTS "Build tests" ON)
# The tests are desktop executables; an Android build produces only the
# runtime library.
if(ANDROID)
    set(RMMZ_BUILD_TESTS OFF)
endif()
option(RMMZ_WIN32_CONSOLE "Windows: build outsider as a console application instead of a GUI application" OFF)

# Rendering backend. "gpu" targets SDL3's GPU API (Vulkan, D3D12 or Metal)
# with the shader bytecode in src/rendering/shaders; "gl" is the original
# OpenGL 4.5 path, kept as a fallback for machines without a supported GPU
# backend.
set(RMMZ_RENDER_BACKEND "gpu" CACHE STRING "Rendering backend: gpu or gl")
set_property(CACHE RMMZ_RENDER_BACKEND PROPERTY STRINGS gpu gl)
if(NOT RMMZ_RENDER_BACKEND MATCHES "^(gl|gpu)$")
    message(FATAL_ERROR "RMMZ_RENDER_BACKEND must be gpu or gl")
endif()

# Apple's OpenGL stops at 4.1 core and is deprecated; the GL renderer is built
# on 4.5 direct state access, so that backend cannot run here at all. Metal
# always exists instead, which is what the GPU backend uses.
if(APPLE AND RMMZ_RENDER_BACKEND STREQUAL "gl")
    message(FATAL_ERROR
        "RMMZ_RENDER_BACKEND=gl needs OpenGL 4.5, and macOS provides at most "
        "4.1. Build the GPU backend (the default) instead: "
        "-DRMMZ_RENDER_BACKEND=gpu")
endif()

# Compile shaders a game's plugins supply at run time. Only the GPU backend
# needs this: SDL3 takes bytecode rather than source, so without it a custom
# plugin filter draws unfiltered.
cmake_dependent_option(RMMZ_RUNTIME_SHADERS
    "Compile game-supplied shaders at run time (GPU backend)" ON
    "RMMZ_RENDER_BACKEND STREQUAL gpu" OFF)

# SDL3 takes shader bytecode, and only runs a backend whose format we ship:
# DXIL for D3D12, SPIR-V for Vulkan, MSL for Metal. Check the generated header
# carries the one this platform needs, so a missing format fails here instead
# of producing a runtime that builds and then cannot create a device.
if(RMMZ_RENDER_BACKEND STREQUAL "gpu")
    if(APPLE)
        set(RMMZ_NEEDED_SHADER "msl")
    elseif(WIN32)
        set(RMMZ_NEEDED_SHADER "dxil")
    else()
        set(RMMZ_NEEDED_SHADER "spirv")
    endif()
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/rendering/shaders/shaders_generated.h"
         RMMZ_SHADER_BLOBS_SRC)
    string(FIND "${RMMZ_SHADER_BLOBS_SRC}" "_${RMMZ_NEEDED_SHADER}[]" RMMZ_HAVE_SHADER)
    unset(RMMZ_SHADER_BLOBS_SRC)
    if(RMMZ_HAVE_SHADER EQUAL -1)
        message(FATAL_ERROR
            "RMMZ_RENDER_BACKEND=gpu needs ${RMMZ_NEEDED_SHADER} shaders on this "
            "platform, and src/rendering/shaders/shaders_generated.h has none.\n"
            "Regenerate them with SDL_shadercross:\n"
            "    python tools/compile_shaders.py --shadercross /path/to/shadercross\n"
            "or build the OpenGL backend with -DRMMZ_RENDER_BACKEND=gl.")
    endif()
endif()

# Platform-specific system libraries and compiler settings.
# On Linux/macOS libm and pthread are separate link libraries; on Windows
# they are part of the CRT, so RMMZ_SYS_LIBS is empty there.
if(WIN32)
    set(RMMZ_SYS_LIBS "")
    add_compile_definitions(
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_WARNINGS
    )
elseif(ANDROID)
    # Bionic has pthread built in; log and android are for logcat and assets.
    set(RMMZ_SYS_LIBS m log android)
else()
    set(RMMZ_SYS_LIBS m pthread)
endif()
if(MSVC)
    # Source files are UTF-8; silence noisy narrowing/deprecation warnings.
    add_compile_options(/wd4244 /wd4267 /wd4996 /utf-8)
    # QuickJS throws a catchable "stack overflow" only when the OS stack is
    # larger than its own limit (JS_STACK_SIZE in js_engine.c). The MSVC
    # default of 1 MB is smaller, so deep recursion in game scripts would
    # crash the process instead. Applies to every executable, tests included.
    add_link_options(/STACK:16777216)
endif()

# Game default resolution
set(RMMZ_DEFAULT_WIDTH 816 CACHE STRING "Default window width")
set(RMMZ_DEFAULT_HEIGHT 624 CACHE STRING "Default window height")

# Third-party dependencies
add_subdirectory(third_party)

# Link OpenGL (the GL backend, and the tests, which build the headless
# rendering sources). Android has no desktop OpenGL: only the GPU backend
# (Vulkan) is available there.
if(ANDROID)
    if(NOT RMMZ_RENDER_BACKEND STREQUAL "gpu")
        message(FATAL_ERROR "Android supports only -DRMMZ_RENDER_BACKEND=gpu")
    endif()
    set(RMMZ_GL_LIB "")
else()
    find_package(OpenGL REQUIRED)
    set(RMMZ_GL_LIB OpenGL::GL)
endif()

if(RMMZ_RENDER_BACKEND STREQUAL "gpu")
    set(RMMZ_RENDER_SOURCES
        src/rendering/gpu_backend.c
        src/rendering/sprite_batch_gpu.c
        src/rendering/renderer_gpu.c
        src/rendering/filters_gpu.c
        src/rendering/glsl_translate.c
        src/rendering/gpu_shader_runtime.c
    )
else()
    set(RMMZ_RENDER_SOURCES
        src/rendering/gl_loader.c
        src/rendering/sprite_batch.c
        src/rendering/renderer.c
        src/rendering/filters.c
    )
endif()

# Main runtime sources
set(RMMZ_RUNTIME_SOURCES
    src/main.c
    ${RMMZ_RENDER_SOURCES}
    src/platform/platform_sdl3.c
    src/platform/win_console.c
    src/platform/android_support.c
    src/engine/js_engine.c
    src/engine/game_loop.c
    src/engine/error_handler.c
    src/engine/script_loader.c
    src/engine/deobf.c
    src/rendering/present_cpu.c
    src/io/file_io.c
    src/bindings/bind_io.c
    src/bindings/bind_image.c
    src/rendering/apng.c
    src/bindings/bind_canvas2d.c
    src/bindings/bind_renderer.c
    src/bindings/bind_font.c
    src/bindings/bind_tilemap.c
    src/bindings/bind_audio.c
    src/bindings/bind_input.c
    src/bindings/bind_effekseer.c
    src/bindings/bind_video.c
    src/bindings/bind_filters.c
    src/bindings/bind_platform.c
    src/audio/audio_engine.cpp
    src/effects/effekseer_backend.cpp
    src/video/video_player.c
    src/input/input_manager.c
    src/rendering/image_loader.c
    src/rendering/canvas2d.c
    src/rendering/woff.c
    src/rendering/font_manager.c
    src/rendering/tilemap.c
)

if(ANDROID)
    # SDLActivity loads libmain.so and calls its SDL_main, so on Android the
    # runtime is a shared library rather than an executable.
    add_library(outsider SHARED ${RMMZ_RUNTIME_SOURCES})
    set_target_properties(outsider PROPERTIES OUTPUT_NAME main)
else()
    add_executable(outsider ${RMMZ_RUNTIME_SOURCES})
endif()

# Windows: embed an application manifest so the process uses the UTF-8 code
# page (game directories with non-ASCII names work with narrow CRT calls) and
# is declared per-monitor DPI aware.
if(WIN32)
    target_sources(outsider PRIVATE src/platform/outsider.manifest)
    # GUI subsystem: no console window on launch. SDL3 supplies WinMain.
    # Output still reaches an attached parent console or console.log
    # (see src/platform/win_console.c). RMMZ_WIN32_CONSOLE=ON restores the
    # classic console build for debugging.
    if(NOT RMMZ_WIN32_CONSOLE)
        set_target_properties(outsider PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()
endif()

target_include_directories(outsider PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pl_mpeg
)

target_compile_definitions(outsider PRIVATE
    RMMZ_DEFAULT_WIDTH=${RMMZ_DEFAULT_WIDTH}
    RMMZ_DEFAULT_HEIGHT=${RMMZ_DEFAULT_HEIGHT}
    $<$<BOOL:${RMMZ_USE_EFFEKSEER}>:RMMZ_HAS_EFFEKSEER>
)

if(RMMZ_RENDER_BACKEND STREQUAL "gpu")
    target_compile_definitions(outsider PRIVATE RMMZ_RENDER_GPU)
else()
    target_compile_definitions(outsider PRIVATE RMMZ_HAS_GL)
endif()

target_link_libraries(outsider PRIVATE
    SDL3::SDL3
    ${RMMZ_GL_LIB}
    qjs
    soloud
    ${RMMZ_SYS_LIBS}
)

if(RMMZ_RUNTIME_SHADERS)
    target_link_libraries(outsider PRIVATE
        sdl_shadercross glslang SPIRV glslang-default-resource-limits)
    target_compile_definitions(outsider PRIVATE RMMZ_RUNTIME_SHADERS)
endif()

if(RMMZ_USE_EFFEKSEER)
    target_link_libraries(outsider PRIVATE Effekseer EffekseerRendererGL)
endif()

# Install target for distribution packaging.
#
# Everything here belongs to the "runtime" component so a distribution can be
# staged with `cmake --install --component runtime`. Dependencies fetched into
# the build carry install rules of their own -- QuickJS installs its CLI,
# headers and import library unconditionally -- and without a component those
# land in the same prefix and ship inside the archive.
install(TARGETS outsider
    RUNTIME DESTINATION .
    LIBRARY DESTINATION .
    COMPONENT runtime
)

# Install shim JS files alongside executable
install(DIRECTORY src/shims/
    DESTINATION shims
    COMPONENT runtime
    FILES_MATCHING PATTERN "*.js"
)

# Game directory is set at install time via -DRMMZ_GAME_DIR
set(RMMZ_GAME_DIR "" CACHE PATH "Path to RPG Maker MZ game directory for packaging")
if(RMMZ_GAME_DIR)
    install(DIRECTORY "${RMMZ_GAME_DIR}/"
        DESTINATION game
        COMPONENT runtime
        PATTERN "*.exe" EXCLUDE
        PATTERN "*.dll" EXCLUDE
        PATTERN "*.pak" EXCLUDE
        PATTERN "*.dat" EXCLUDE
        PATTERN "node.dll" EXCLUDE
        PATTERN "chromedriver.exe" EXCLUDE
        PATTERN "notification_helper.exe" EXCLUDE
        PATTERN "locales" EXCLUDE
        PATTERN "lib" EXCLUDE
        PATTERN "icon" EXCLUDE
        PATTERN "css" EXCLUDE
    )
endif()

# Tests
if(RMMZ_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
