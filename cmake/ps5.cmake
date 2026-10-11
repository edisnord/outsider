# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 the PS5 port contributors
# Outsider for the PlayStation 5, built with the ps5-payload-sdk (see scripts/build-outsider.sh of the
# easyrpg-ps5 project). Included from the top-level CMakeLists.txt when -DRMMZ_PS5=ON; it replaces the
# desktop build (SDL3, SDL_GPU, fetched dependencies) with:
#
#   * SDL2 from the SDK (platform_sdl2.c and the compatibility headers in src/platform/ps5_compat)
#   * QuickJS-NG and SoLoud built from source trees passed in by the build script
#   * the OpenGL 4.5 renderer, running on Mesa's software rasteriser (OSMesa, loaded by SDL at run time)
#   * a shim that makes relative file paths work on the console (see shim/ps5path.c)
#
# Inputs (all set by scripts/build-outsider.sh):
#   RMMZ_PS5_PREFIX     SDK prefix with the installed libraries (.../target/user/homebrew)
#   RMMZ_PS5_SDL_CFLAGS / RMMZ_PS5_SDL_LIBS   output of pkg-config for sdl2 (lists)
#   RMMZ_PS5_SOLOUD_DIR  SoLoud source tree
#   RMMZ_PS5_SHIM_DIR    directory with ps5path.c and wrap-flags.txt
set(RMMZ_RENDER_BACKEND gl)
set(RMMZ_USE_EFFEKSEER OFF)

set(RMMZ_PS5_PREFIX "" CACHE PATH "Installed libraries of the PS5 SDK")
set(RMMZ_PS5_SDL_CFLAGS "" CACHE STRING "SDL2 compiler flags")
set(RMMZ_PS5_SDL_LIBS "" CACHE STRING "SDL2 linker flags")
set(RMMZ_PS5_SOLOUD_DIR "" CACHE PATH "SoLoud source tree")
set(RMMZ_PS5_SHIM_DIR "" CACHE PATH "Directory of ps5path.c")
# RMMZ_PS5_HOST=ON builds the same SDL2 runtime for the machine you build on (Linux), without the console's
# path shim: handy to profile games and try fixes without a console.
option(RMMZ_PS5_HOST "Build the SDL2 port for the host instead of the console" OFF)
foreach(v RMMZ_PS5_PREFIX RMMZ_PS5_SOLOUD_DIR)
    if(NOT ${v})
        message(FATAL_ERROR "${v} must be set")
    endif()
endforeach()
# RMMZ_PS5_VULKAN=ON: the Vulkan build, on SDL3's GPU API instead of the SDL2 and OpenGL port below
option(RMMZ_PS5_VULKAN "Build for Vulkan (SDL3 GPU, RADV) as a library with outsider_main()" OFF)
if(RMMZ_PS5_VULKAN)
    include(${CMAKE_CURRENT_LIST_DIR}/ps5-vulkan.cmake)
    return()
endif()
if(NOT RMMZ_PS5_HOST AND NOT RMMZ_PS5_SHIM_DIR)
    message(FATAL_ERROR "RMMZ_PS5_SHIM_DIR must be set")
endif()

set(RMMZ_DEFAULT_WIDTH 816 CACHE STRING "Default window width")
set(RMMZ_DEFAULT_HEIGHT 624 CACHE STRING "Default window height")

# --- QuickJS-NG (installed by the quickjs-ng package) -------------------------------------------
add_library(qjs STATIC IMPORTED)
set_target_properties(qjs PROPERTIES
    IMPORTED_LOCATION ${RMMZ_PS5_PREFIX}/lib/libqjs.a
    INTERFACE_INCLUDE_DIRECTORIES ${RMMZ_PS5_PREFIX}/include)

# --- SoLoud, with its own SDL2 backend -----------------------------------------------------------
set(SOLOUD_SRC ${RMMZ_PS5_SOLOUD_DIR}/src)
add_library(soloud STATIC
    ${SOLOUD_SRC}/core/soloud.cpp
    ${SOLOUD_SRC}/core/soloud_audiosource.cpp
    ${SOLOUD_SRC}/core/soloud_bus.cpp
    ${SOLOUD_SRC}/core/soloud_core_3d.cpp
    ${SOLOUD_SRC}/core/soloud_core_basicops.cpp
    ${SOLOUD_SRC}/core/soloud_core_faderops.cpp
    ${SOLOUD_SRC}/core/soloud_core_filterops.cpp
    ${SOLOUD_SRC}/core/soloud_core_getters.cpp
    ${SOLOUD_SRC}/core/soloud_core_setters.cpp
    ${SOLOUD_SRC}/core/soloud_core_voicegroup.cpp
    ${SOLOUD_SRC}/core/soloud_core_voiceops.cpp
    ${SOLOUD_SRC}/core/soloud_fader.cpp
    ${SOLOUD_SRC}/core/soloud_fft.cpp
    ${SOLOUD_SRC}/core/soloud_fft_lut.cpp
    ${SOLOUD_SRC}/core/soloud_file.cpp
    ${SOLOUD_SRC}/core/soloud_filter.cpp
    ${SOLOUD_SRC}/core/soloud_misc.cpp
    ${SOLOUD_SRC}/core/soloud_queue.cpp
    ${SOLOUD_SRC}/core/soloud_thread.cpp
    ${SOLOUD_SRC}/audiosource/wav/soloud_wav.cpp
    ${SOLOUD_SRC}/audiosource/wav/soloud_wavstream.cpp
    ${SOLOUD_SRC}/audiosource/wav/stb_vorbis.c
    ${SOLOUD_SRC}/audiosource/wav/dr_impl.cpp
    ${SOLOUD_SRC}/backend/sdl2_static/soloud_sdl2_static.cpp
    ${SOLOUD_SRC}/backend/null/soloud_null.cpp
)
target_include_directories(soloud PUBLIC ${RMMZ_PS5_SOLOUD_DIR}/include)
target_compile_definitions(soloud PRIVATE WITH_SDL2_STATIC WITH_NULL)
target_compile_options(soloud PRIVATE ${RMMZ_PS5_SDL_CFLAGS})

# --- the runtime ----------------------------------------------------------------------------------
if(NOT RMMZ_PS5_HOST)
    set(RMMZ_PS5_WRAP_FILE ${RMMZ_PS5_SHIM_DIR}/wrap-flags.txt)
    file(READ ${RMMZ_PS5_WRAP_FILE} RMMZ_PS5_WRAP)
    string(STRIP "${RMMZ_PS5_WRAP}" RMMZ_PS5_WRAP)
    set(RMMZ_PS5_SHIM_SOURCE ${RMMZ_PS5_SHIM_DIR}/ps5path.c)
else()
    set(RMMZ_PS5_WRAP "")
    set(RMMZ_PS5_SHIM_SOURCE "")
endif()

add_executable(outsider
    src/main.c
    src/rendering/gl_loader.c
    src/rendering/gl_ps5.c
    src/rendering/sprite_batch.c
    src/rendering/renderer.c
    src/rendering/filters.c
    src/platform/platform_sdl2.c
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
    ${RMMZ_PS5_SHIM_SOURCE}
)

target_include_directories(outsider BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/ps5_compat)
target_include_directories(outsider PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pl_mpeg
)
target_compile_definitions(outsider PRIVATE
    RMMZ_DEFAULT_WIDTH=${RMMZ_DEFAULT_WIDTH}
    RMMZ_DEFAULT_HEIGHT=${RMMZ_DEFAULT_HEIGHT}
    RMMZ_HAS_GL
    RMMZ_PS5
)
target_compile_options(outsider PRIVATE ${RMMZ_PS5_SDL_CFLAGS})

# --export-dynamic: OSMesa is loaded with dlopen and imports libc functions from this executable; the
# console's runtime linker only resolves symbols that the executable exports.
# The version script keeps our gl* wrappers (gl_ps5.c) out of the exported symbols, see ps5-exports.map.
target_link_options(outsider PRIVATE -Wl,--export-dynamic
    -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/ps5-exports.map ${RMMZ_PS5_WRAP})
# WebP pictures (some games name them .png); libwebp comes from the same prefix as QuickJS
if(EXISTS ${RMMZ_PS5_PREFIX}/lib/libwebp.a AND EXISTS ${RMMZ_PS5_PREFIX}/include/webp/decode.h)
    target_compile_definitions(outsider PRIVATE RMMZ_HAVE_WEBP)
    target_include_directories(outsider PRIVATE ${RMMZ_PS5_PREFIX}/include)
    target_link_libraries(outsider PRIVATE ${RMMZ_PS5_PREFIX}/lib/libwebp.a)
endif()
# WebM movies (VP8/VP9 + Opus/Vorbis) through the lean FFmpeg build in the same prefix
if(EXISTS ${RMMZ_PS5_PREFIX}/lib/libavformat.a AND EXISTS ${RMMZ_PS5_PREFIX}/include/libavformat/avformat.h)
    target_compile_definitions(outsider PRIVATE RMMZ_HAVE_FFMPEG)
    target_include_directories(outsider PRIVATE ${RMMZ_PS5_PREFIX}/include)
    target_link_libraries(outsider PRIVATE ${RMMZ_PS5_PREFIX}/lib/libavformat.a ${RMMZ_PS5_PREFIX}/lib/libavcodec.a
                          ${RMMZ_PS5_PREFIX}/lib/libswscale.a ${RMMZ_PS5_PREFIX}/lib/libswresample.a
                          ${RMMZ_PS5_PREFIX}/lib/libavutil.a z)
endif()
target_link_libraries(outsider PRIVATE qjs soloud ${RMMZ_PS5_SDL_LIBS} m pthread)
