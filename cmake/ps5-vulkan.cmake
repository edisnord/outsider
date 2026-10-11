# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 the PS5 port contributors
# Outsider for the PlayStation 5 on Vulkan (Vulkan build): included from cmake/ps5.cmake when
# -DRMMZ_PS5_VULKAN=ON. Unlike the OpenGL builds it keeps upstream's desktop runtime:
#
#   * SDL3 and its GPU API (src/rendering/*_gpu.c, platform_sdl3.c), from RMMZ_PS5_SDL3_PREFIX: the PS5 port of SDL3
#     port, whose video driver hands SDL_GPU the RADV driver (Mesa, mihawk-99/PS5_Mesa) linked into the program
#   * the SPIR-V shaders of src/rendering/shaders; no run-time shader compiler (RMMZ_RUNTIME_SHADERS), so a
#     plugin's own GLSL filter draws unfiltered
#   * SoLoud with Outsider's SDL3 back-end
#
# The result is liboutsider.a with outsider_main() (RMMZ_EMBEDDED), for the PS5 Vulkan program.
set(RMMZ_RENDER_BACKEND gpu)
set(RMMZ_DEFAULT_WIDTH 816 CACHE STRING "Default window width")
set(RMMZ_DEFAULT_HEIGHT 624 CACHE STRING "Default window height")
set(RMMZ_USE_EFFEKSEER OFF)
set(RMMZ_PS5_SDL3_PREFIX "" CACHE PATH "SDL3 for the PS5 (built for the Vulkan program)")
if(NOT RMMZ_PS5_SDL3_PREFIX)
    message(FATAL_ERROR "RMMZ_PS5_SDL3_PREFIX must be set")
endif()

add_library(SDL3::SDL3 STATIC IMPORTED)
set_target_properties(SDL3::SDL3 PROPERTIES
    IMPORTED_LOCATION ${RMMZ_PS5_SDL3_PREFIX}/lib/libSDL3.a
    INTERFACE_INCLUDE_DIRECTORIES ${RMMZ_PS5_SDL3_PREFIX}/include)

# RMMZ_JS_ENGINE=jsc: JavaScriptCore (JIT) instead of QuickJS-NG. src/engine/jsc/quickjs.h implements the
# QuickJS API Outsider uses on JSC's C API, so the bindings are unchanged. RMMZ_JSC_HEADERS is the
# JavaScriptCore/Headers directory of a JSCOnly build; the final link adds JSC itself (title-link.sh).
set(RMMZ_JS_ENGINE "quickjs" CACHE STRING "JavaScript engine: quickjs or jsc")
set(RMMZ_JSC_HEADERS "" CACHE PATH "JavaScriptCore/Headers of the JSCOnly build (RMMZ_JS_ENGINE=jsc)")
if(RMMZ_JS_ENGINE STREQUAL "jsc" AND NOT RMMZ_JSC_HEADERS)
    message(FATAL_ERROR "RMMZ_JS_ENGINE=jsc needs RMMZ_JSC_HEADERS")
endif()

add_library(qjs STATIC IMPORTED)
set_target_properties(qjs PROPERTIES
    IMPORTED_LOCATION ${RMMZ_PS5_PREFIX}/lib/libqjs.a
    INTERFACE_INCLUDE_DIRECTORIES ${RMMZ_PS5_PREFIX}/include)

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
    ${CMAKE_SOURCE_DIR}/src/audio/soloud_sdl3_backend.cpp
    ${SOLOUD_SRC}/backend/null/soloud_null.cpp
)
target_include_directories(soloud PUBLIC ${RMMZ_PS5_SOLOUD_DIR}/include)
target_compile_definitions(soloud PRIVATE WITH_SDL2_STATIC WITH_NULL)
target_link_libraries(soloud PRIVATE SDL3::SDL3)

add_library(outsider STATIC
    src/main.c
    src/rendering/gpu_backend.c
    src/rendering/sprite_batch_gpu.c
    src/rendering/renderer_gpu.c
    src/rendering/filters_gpu.c
    src/rendering/glsl_translate.c
    src/rendering/gpu_shader_runtime.c
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
target_include_directories(outsider PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pl_mpeg
)
target_compile_definitions(outsider PRIVATE
    RMMZ_DEFAULT_WIDTH=${RMMZ_DEFAULT_WIDTH}
    RMMZ_DEFAULT_HEIGHT=${RMMZ_DEFAULT_HEIGHT}
    RMMZ_RENDER_GPU
    RMMZ_PS5
    RMMZ_PS5_VULKAN
    RMMZ_EMBEDDED
)
if(EXISTS ${RMMZ_PS5_PREFIX}/lib/libwebp.a AND EXISTS ${RMMZ_PS5_PREFIX}/include/webp/decode.h)
    target_compile_definitions(outsider PRIVATE RMMZ_HAVE_WEBP)
    target_include_directories(outsider PRIVATE ${RMMZ_PS5_PREFIX}/include)
endif()
if(EXISTS ${RMMZ_PS5_PREFIX}/lib/libavformat.a AND EXISTS ${RMMZ_PS5_PREFIX}/include/libavformat/avformat.h)
    target_compile_definitions(outsider PRIVATE RMMZ_HAVE_FFMPEG)
    target_include_directories(outsider PRIVATE ${RMMZ_PS5_PREFIX}/include)
endif()
if(RMMZ_JS_ENGINE STREQUAL "jsc")
    target_sources(outsider PRIVATE src/engine/jsc/qjs_jsc.c)
    # before everything else: the SDK prefix has a quickjs.h of its own
    target_include_directories(outsider BEFORE PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/jsc ${RMMZ_JSC_HEADERS})
    target_compile_definitions(outsider PRIVATE RMMZ_JS_JSC)
    target_link_libraries(outsider PRIVATE SDL3::SDL3 soloud)
else()
    target_link_libraries(outsider PRIVATE SDL3::SDL3 qjs soloud)
endif()
