# thebigs2 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(thebigs2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Cross builds (Windows from Linux, packaging/windows/) can't run the target's
# rexglue.exe for codegen; use the host SDK's. The generated code is the same.
if(CMAKE_CROSSCOMPILING)
    find_program(REXGLUE_HOST_TOOL rexglue
        HINTS "$ENV{HOME}/rexsdk-install/bin" NO_CMAKE_FIND_ROOT_PATH)
    if(NOT REXGLUE_HOST_TOOL)
        message(FATAL_ERROR "Cross build: set REXGLUE_HOST_TOOL to a host rexglue binary")
    endif()
    set_target_properties(rex::rexglue PROPERTIES IMPORTED_LOCATION_RELEASE "${REXGLUE_HOST_TOOL}")
endif()

# Sources
set(THEBIGS2_SOURCES
    src/main.cpp
    src/hooks.cpp
    src/renderdoc_capture.cpp
    src/settings.cpp
    src/settings_menu.cpp
    src/imgui_gamepad.cpp
    src/frame_stats.cpp
    src/first_launch.cpp
    src/render_tweaks.cpp
    src/native_capture.cpp
    src/native/buffers.cpp
    src/native/renderer.cpp
    src/native/shaders.cpp
    src/native/textures.cpp
    src/native/plume_device.cpp
)

if(WIN32)
    add_executable(thebigs2 WIN32 ${THEBIGS2_SOURCES} packaging/windows/thebigs2.rc)
else()
    add_executable(thebigs2 ${THEBIGS2_SOURCES})
endif()

# SDL3 is linked into librexruntime; we only need its headers (window mode,
# file picker).
if(NOT TARGET SDL3::Headers)
    find_package(SDL3 CONFIG QUIET)
endif()
if(TARGET SDL3::Headers)
    target_link_libraries(thebigs2 PRIVATE SDL3::Headers)
elseif(DEFINED REXSDK_DIR AND EXISTS "${REXSDK_DIR}/thirdparty/sdl3/include")
    target_include_directories(thebigs2 PRIVATE "${REXSDK_DIR}/thirdparty/sdl3/include")
else()
    message(FATAL_ERROR "SDL3 headers not found (they ship with the ReXGlue SDK)")
endif()

# Native renderer (docs/native-renderer.md): the SDK's Xenos -> SPIR-V shader
# translator, copied from the SDK sources (the GPU plugin doesn't export it).
file(GLOB TB2_XENOS_SOURCES CONFIGURE_DEPENDS src/native/xenos/*.cpp)
add_library(tb2_xenos STATIC ${TB2_XENOS_SOURCES})
target_include_directories(tb2_xenos SYSTEM PUBLIC src/native/xenos/glslang)
target_link_libraries(tb2_xenos PUBLIC rex::runtime rex::renderdoc rex::xxhash rex::SPIRV)
if(NOT MSVC AND NOT CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    target_compile_options(tb2_xenos PRIVATE -Wno-unused-variable -Wno-unused-function)
endif()
target_link_libraries(thebigs2 PRIVATE tb2_xenos)

# Plume (src/native/plume/README.md), the native renderer's rendering API:
# Vulkan backend only, built against the SDK's Vulkan headers, SDL3 windows.
# Hidden visibility keeps volk's vkXxx function pointers out of the exports.
add_library(tb2_plume STATIC src/native/plume/plume_vulkan.cpp)
target_include_directories(tb2_plume SYSTEM PUBLIC
    src/native/plume src/native/plume/contrib/volk
    src/native/plume/contrib/VulkanMemoryAllocator/include)
target_compile_definitions(tb2_plume PUBLIC PLUME_SDL3_VULKAN_ENABLED)
target_link_libraries(tb2_plume PUBLIC rex::runtime)
if(TARGET SDL3::Headers)
    target_link_libraries(tb2_plume PUBLIC SDL3::Headers)
else()
    target_include_directories(tb2_plume SYSTEM PUBLIC "${REXSDK_DIR}/thirdparty/sdl3/include")
endif()
set_target_properties(tb2_plume PROPERTIES CXX_VISIBILITY_PRESET hidden)
if(NOT MSVC AND NOT CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    target_compile_options(tb2_plume PRIVATE -w)
endif()
target_link_libraries(thebigs2 PRIVATE tb2_plume)

# A user's disc image is accepted only if its default.xex is the one this
# build was recompiled from.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/game/default.xex")
    file(MD5 "${CMAKE_CURRENT_SOURCE_DIR}/game/default.xex" TB2_XEX_MD5)
    target_compile_definitions(thebigs2 PRIVATE TB2_XEX_MD5="${TB2_XEX_MD5}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${CMAKE_CURRENT_SOURCE_DIR}/game/default.xex")
endif()

# Render call tracing for the native-renderer work (docs/native-renderer.md).
# Wraps every function in the XDK D3D library and the engine's renderlib with a
# call counter; F8 records a frame. Off by default (adds ~2,000 wrappers).
option(TB2_RENDER_TRACE "Build with render call tracing (F8)" OFF)
if(TB2_RENDER_TRACE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_trace_cpp "${CMAKE_CURRENT_BINARY_DIR}/render_trace_hooks.cpp")
    set(_codegen_stamp "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/codegen.build.stamp")
    add_custom_command(
        OUTPUT "${_trace_cpp}"
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_render_trace.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/generated/default" "${CMAKE_CURRENT_SOURCE_DIR}/src"
                "${_trace_cpp}" d3d:823A6400-823DE000 renderlib:82460000-8249B800
                d3d:824D92E8-824DC3E8
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_render_trace.py" "${_codegen_stamp}"
                ${THEBIGS2_SOURCES}
        COMMENT "Generating render trace wrappers"
        VERBATIM)
    target_sources(thebigs2 PRIVATE src/render_trace.cpp "${_trace_cpp}")
    target_compile_definitions(thebigs2 PRIVATE TB2_RENDER_TRACE=1)
endif()

# GPU emulation is opt-in on the development branch; ship the Xenos plugin.
rexglue_setup_target(thebigs2 GPU_PLUGINS xenos)

# Keep the executable loadable on glibc older than the build machine's, e.g.
# SteamOS (see src/glibc_compat.h), including the recompiled sources.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    foreach(_t thebigs2 thebigs2_recomp tb2_xenos tb2_plume)
        if(TARGET ${_t})
            target_compile_options(${_t} PRIVATE
                "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/src/glibc_compat.h")
        endif()
    endforeach()
endif()

# The SDK sets a $ORIGIN rpath on Linux but (unlike Windows/macOS) never stages
# librexruntime.so / libTracyClient.so next to the binary, so it won't start.
if(UNIX AND NOT APPLE)
    foreach(_lib rex::runtime rexruntime rex::TracyClient TracyClient)
        if(TARGET ${_lib})
            add_custom_command(TARGET thebigs2 POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:${_lib}> $<TARGET_FILE_DIR:thebigs2>
                VERBATIM)
        endif()
    endforeach()
endif()
