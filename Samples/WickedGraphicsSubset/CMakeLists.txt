cmake_minimum_required(VERSION 3.19)

if(NOT DEFINED WICKED_ROOT_DIR)
    set(WICKED_ROOT_DIR "${CMAKE_SOURCE_DIR}")
endif()

set(WICKED_SUBSET_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(WICKED_SUBSET_ENGINE_ROOT "${WICKED_ROOT_DIR}/WickedEngine")
set(WICKED_SUBSET_SRC_DIR "${WICKED_SUBSET_ROOT_DIR}/src")
set(WICKED_SUBSET_SHADER_DIR "${WICKED_SUBSET_SRC_DIR}/shaders")

if(NOT EXISTS "${WICKED_SUBSET_ENGINE_ROOT}/wiGraphicsDevice.h")
    message(FATAL_ERROR "WickedEngine source tree not found at: ${WICKED_SUBSET_ENGINE_ROOT}")
endif()

if(APPLE)
    enable_language(OBJC)
    enable_language(OBJCXX)
endif()

set(WICKED_SUBSET_BACKEND_VULKAN 0)
set(WICKED_SUBSET_BACKEND_DX12 1)
set(WICKED_SUBSET_BACKEND_METAL 2)

if(NOT DEFINED WICKED_ENGINECONFIG_SUBSET_USE_DX12)
    set(WICKED_ENGINECONFIG_SUBSET_USE_DX12 0)
endif()

if(NOT DEFINED WICKED_ENGINECONFIG_SUBSET_BACKEND)
    if(APPLE)
        set(WICKED_ENGINECONFIG_SUBSET_BACKEND ${WICKED_SUBSET_BACKEND_METAL})
    elseif(WIN32 AND WICKED_ENGINECONFIG_SUBSET_USE_DX12)
        set(WICKED_ENGINECONFIG_SUBSET_BACKEND ${WICKED_SUBSET_BACKEND_DX12})
    else()
        set(WICKED_ENGINECONFIG_SUBSET_BACKEND ${WICKED_SUBSET_BACKEND_VULKAN})
    endif()
endif()
if(NOT WICKED_ENGINECONFIG_SUBSET_BACKEND MATCHES "^[012]$")
    message(FATAL_ERROR "EngineConfig value WI_ENGINECONFIG_SUBSET_BACKEND must be 0 (Vulkan), 1 (DX12), or 2 (Metal).")
endif()

set(WICKED_ENGINECONFIG_SUBSET_BACKEND_NAME "Vulkan")
if(WICKED_ENGINECONFIG_SUBSET_BACKEND EQUAL WICKED_SUBSET_BACKEND_DX12)
    set(WICKED_ENGINECONFIG_SUBSET_BACKEND_NAME "DX12")
elseif(WICKED_ENGINECONFIG_SUBSET_BACKEND EQUAL WICKED_SUBSET_BACKEND_METAL)
    set(WICKED_ENGINECONFIG_SUBSET_BACKEND_NAME "Metal")
endif()
message(STATUS "Wicked subset EngineConfig backend: ${WICKED_ENGINECONFIG_SUBSET_BACKEND} (${WICKED_ENGINECONFIG_SUBSET_BACKEND_NAME})")

set(_subset_default_dx12 OFF)
set(_subset_default_vulkan OFF)
set(_subset_default_metal OFF)
if(WIN32)
    set(_subset_default_dx12 ON)
    set(_subset_default_vulkan ON)
elseif(APPLE)
    set(_subset_default_metal ON)
else()
    set(_subset_default_vulkan ON)
endif()
if(WICKED_ENGINECONFIG_SUBSET_BACKEND EQUAL WICKED_SUBSET_BACKEND_VULKAN)
    set(_subset_default_dx12 OFF)
    set(_subset_default_vulkan ON)
    set(_subset_default_metal OFF)
elseif(WICKED_ENGINECONFIG_SUBSET_BACKEND EQUAL WICKED_SUBSET_BACKEND_DX12)
    set(_subset_default_dx12 ON)
    set(_subset_default_vulkan OFF)
    set(_subset_default_metal OFF)
elseif(WICKED_ENGINECONFIG_SUBSET_BACKEND EQUAL WICKED_SUBSET_BACKEND_METAL)
    set(_subset_default_dx12 OFF)
    set(_subset_default_vulkan OFF)
    set(_subset_default_metal ON)
endif()

option(WICKED_SUBSET_ENABLE_SHADERCOMPILER "Compile wiShaderCompiler.cpp in subset mode" ON)
option(WICKED_SUBSET_ENABLE_DX12 "Compile wiGraphicsDevice_DX12.cpp in subset mode" ${_subset_default_dx12})
option(WICKED_SUBSET_ENABLE_VULKAN "Compile wiGraphicsDevice_Vulkan.cpp in subset mode" ${_subset_default_vulkan})
option(WICKED_SUBSET_ENABLE_METAL "Compile wiGraphicsDevice_Metal.cpp in subset mode" ${_subset_default_metal})
option(WICKED_SUBSET_BUILD_CUBE_DEMO "Build SDL3 cube demo using Wicked graphics subset" ON)
option(WICKED_SUBSET_BUILD_VISIBILITY_BENCHMARK_DEMO "Build visibility pipeline benchmark demo using Wicked graphics subset" ON)

if(WICKED_ENGINECONFIG_SUBSET_BACKEND EQUAL WICKED_SUBSET_BACKEND_VULKAN)
    set(WICKED_SUBSET_ENABLE_VULKAN ON CACHE BOOL "Compile wiGraphicsDevice_Vulkan.cpp in subset mode" FORCE)
    set(WICKED_SUBSET_ENABLE_DX12 OFF CACHE BOOL "Compile wiGraphicsDevice_DX12.cpp in subset mode" FORCE)
    set(WICKED_SUBSET_ENABLE_METAL OFF CACHE BOOL "Compile wiGraphicsDevice_Metal.cpp in subset mode" FORCE)
elseif(WICKED_ENGINECONFIG_SUBSET_BACKEND EQUAL WICKED_SUBSET_BACKEND_DX12)
    set(WICKED_SUBSET_ENABLE_VULKAN OFF CACHE BOOL "Compile wiGraphicsDevice_Vulkan.cpp in subset mode" FORCE)
    set(WICKED_SUBSET_ENABLE_DX12 ON CACHE BOOL "Compile wiGraphicsDevice_DX12.cpp in subset mode" FORCE)
    set(WICKED_SUBSET_ENABLE_METAL OFF CACHE BOOL "Compile wiGraphicsDevice_Metal.cpp in subset mode" FORCE)
elseif(WICKED_ENGINECONFIG_SUBSET_BACKEND EQUAL WICKED_SUBSET_BACKEND_METAL)
    set(WICKED_SUBSET_ENABLE_VULKAN OFF CACHE BOOL "Compile wiGraphicsDevice_Vulkan.cpp in subset mode" FORCE)
    set(WICKED_SUBSET_ENABLE_DX12 OFF CACHE BOOL "Compile wiGraphicsDevice_DX12.cpp in subset mode" FORCE)
    set(WICKED_SUBSET_ENABLE_METAL ON CACHE BOOL "Compile wiGraphicsDevice_Metal.cpp in subset mode" FORCE)
endif()

set(WICKED_SUBSET_SDL_SOURCE_DIR "" CACHE PATH "Path to local SDL3 source directory (must contain CMakeLists.txt)")
set(WICKED_SUBSET_MMGR_ROOT_DIR "${WICKED_ROOT_DIR}/forge-mmgr/FluidStudios/MemoryManager")
set(WICKED_SUBSET_MMGR_GLOBALS_CPP "${WICKED_SUBSET_MMGR_ROOT_DIR}/mmgr_new_delete.cpp")

set(WICKED_SUBSET_OBJECT_TARGETS)
set(WICKED_SUBSET_MMGR_SETTINGS_TARGET "")
set(WICKED_SUBSET_MMGR_RUNTIME_OBJECT_TARGET "")

function(wicked_subset_apply_common_flags target)
    target_include_directories(${target} PRIVATE
        "${WICKED_SUBSET_ENGINE_ROOT}"
        "${WICKED_SUBSET_ENGINE_ROOT}/Utility/metal"
    )

    if(DEFINED WICKED_VULKAN_PLATFORM_COMPILE_DEFINITIONS)
        target_compile_definitions(${target} PRIVATE
            ${WICKED_VULKAN_PLATFORM_COMPILE_DEFINITIONS}
        )
    endif()

    if(TARGET SDL3::SDL3)
        target_link_libraries(${target} PRIVATE SDL3::SDL3)
    endif()

    if(NOT MSVC AND NOT CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
        target_compile_options(${target} PRIVATE -fdeclspec -fms-extensions)
    endif()
endfunction()

function(wicked_subset_apply_mmgr_settings target)
    if(NOT WICKED_SUBSET_ENABLE_MMGR)
        return()
    endif()

    if(NOT TARGET "${WICKED_SUBSET_MMGR_SETTINGS_TARGET}")
        message(FATAL_ERROR "MMGR is enabled but settings target is not available.")
    endif()
    target_link_libraries(${target} PRIVATE "${WICKED_SUBSET_MMGR_SETTINGS_TARGET}")
endfunction()

function(wicked_subset_link_mmgr_runtime_object target)
    if(NOT WICKED_SUBSET_ENABLE_MMGR)
        return()
    endif()

    if(NOT TARGET "${WICKED_SUBSET_MMGR_RUNTIME_OBJECT_TARGET}")
        message(FATAL_ERROR "MMGR is enabled but runtime object target is not available.")
    endif()
    target_sources(${target} PRIVATE
        $<TARGET_OBJECTS:${WICKED_SUBSET_MMGR_RUNTIME_OBJECT_TARGET}>
    )
endfunction()

function(wicked_subset_add_object target)
    add_library(${target} OBJECT ${ARGN})
    wicked_subset_apply_common_flags(${target})
    wicked_subset_apply_mmgr_settings(${target})
    list(APPEND WICKED_SUBSET_OBJECT_TARGETS ${target})
    set(WICKED_SUBSET_OBJECT_TARGETS "${WICKED_SUBSET_OBJECT_TARGETS}" PARENT_SCOPE)
endfunction()

function(wicked_subset_add_mmgr_object target)
    add_library(${target} OBJECT
        "${WICKED_SUBSET_MMGR_ROOT_DIR}/mmgr.c"
        "${WICKED_SUBSET_MMGR_GLOBALS_CPP}"
    )
    target_include_directories(${target} PRIVATE
        "${WICKED_SUBSET_MMGR_ROOT_DIR}"
    )
    if(EXISTS "${WICKED_SUBSET_SDL_SOURCE_DIR}/include")
        target_include_directories(${target} PRIVATE
            "${WICKED_SUBSET_SDL_SOURCE_DIR}/include"
        )
    elseif(EXISTS "${WICKED_ROOT_DIR}/SDL/include")
        target_include_directories(${target} PRIVATE
            "${WICKED_ROOT_DIR}/SDL/include"
        )
    elseif(EXISTS "${WICKED_ROOT_DIR}/SDL3/include")
        target_include_directories(${target} PRIVATE
            "${WICKED_ROOT_DIR}/SDL3/include"
        )
    elseif(EXISTS "${WICKED_ROOT_DIR}/SDL-release-3.4.4 2/include")
        target_include_directories(${target} PRIVATE
            "${WICKED_ROOT_DIR}/SDL-release-3.4.4 2/include"
        )
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE
            "$<$<COMPILE_LANGUAGE:CXX>:/EHsc>"
        )
    else()
        if(CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
            target_compile_options(${target} PRIVATE
                "$<$<COMPILE_LANGUAGE:CXX>:-fexceptions>"
            )
        else()
            target_compile_options(${target} PRIVATE
                -fdeclspec
                -fms-extensions
                "$<$<COMPILE_LANGUAGE:CXX>:-fexceptions>"
            )
        endif()
    endif()
    list(APPEND WICKED_SUBSET_OBJECT_TARGETS ${target})
    set(WICKED_SUBSET_OBJECT_TARGETS "${WICKED_SUBSET_OBJECT_TARGETS}" PARENT_SCOPE)
endfunction()

set(WICKED_SUBSET_MMGR_COVERAGE_MODE "disabled")
if(WICKED_SUBSET_ENABLE_MMGR)
    if(NOT EXISTS "${WICKED_SUBSET_MMGR_ROOT_DIR}/mmgr.h")
        message(FATAL_ERROR "WICKED_SUBSET_ENABLE_MMGR is ON but mmgr header is missing: ${WICKED_SUBSET_MMGR_ROOT_DIR}/mmgr.h")
    endif()
    if(NOT EXISTS "${WICKED_SUBSET_MMGR_ROOT_DIR}/mmgr.c")
        message(FATAL_ERROR "WICKED_SUBSET_ENABLE_MMGR is ON but mmgr implementation is missing: ${WICKED_SUBSET_MMGR_ROOT_DIR}/mmgr.c")
    endif()
    if(NOT EXISTS "${WICKED_SUBSET_MMGR_GLOBALS_CPP}")
        message(FATAL_ERROR "WICKED_SUBSET_ENABLE_MMGR is ON but global operator override source is missing: ${WICKED_SUBSET_MMGR_GLOBALS_CPP}")
    endif()

    add_library(wicked_subset_mmgr_settings INTERFACE)
    target_include_directories(wicked_subset_mmgr_settings INTERFACE
        "${WICKED_SUBSET_MMGR_ROOT_DIR}"
    )
    set(WICKED_SUBSET_MMGR_SETTINGS_TARGET wicked_subset_mmgr_settings)

    wicked_subset_add_mmgr_object(wicked_subset_mmgr_obj)
    set(WICKED_SUBSET_MMGR_RUNTIME_OBJECT_TARGET wicked_subset_mmgr_obj)

    set(WICKED_SUBSET_MMGR_COVERAGE_MODE "explicit-include+runtime-object")
    message(STATUS "Wicked subset MMGR is enabled: explicit include mode for selected translation units, plus mmgr runtime object for global new/delete overrides.")
    message(STATUS "Wicked subset MMGR runtime object sources: ${WICKED_SUBSET_MMGR_ROOT_DIR}/mmgr.c, ${WICKED_SUBSET_MMGR_GLOBALS_CPP}")
endif()
message(STATUS "Wicked subset MMGR coverage mode: ${WICKED_SUBSET_MMGR_COVERAGE_MODE}")

set(WICKED_SUBSET_NEEDS_SDL3 OFF)
if(WICKED_SUBSET_BUILD_CUBE_DEMO OR WICKED_SUBSET_BUILD_VISIBILITY_BENCHMARK_DEMO)
    set(WICKED_SUBSET_NEEDS_SDL3 ON)
endif()
if(WICKED_SUBSET_ENABLE_VULKAN)
    set(WICKED_SUBSET_NEEDS_SDL3 ON)
endif()

if(WICKED_SUBSET_NEEDS_SDL3)
    set(_subset_default_sdl_dir "")
    if(EXISTS "${WICKED_ROOT_DIR}/SDL/CMakeLists.txt")
        set(_subset_default_sdl_dir "${WICKED_ROOT_DIR}/SDL")
    elseif(EXISTS "${WICKED_ROOT_DIR}/SDL3/CMakeLists.txt")
        set(_subset_default_sdl_dir "${WICKED_ROOT_DIR}/SDL3")
    elseif(EXISTS "${WICKED_ROOT_DIR}/SDL-release-3.4.4 2/CMakeLists.txt")
        set(_subset_default_sdl_dir "${WICKED_ROOT_DIR}/SDL-release-3.4.4 2")
    endif()

    if(WICKED_SUBSET_SDL_SOURCE_DIR STREQUAL "" AND NOT _subset_default_sdl_dir STREQUAL "")
        set(WICKED_SUBSET_SDL_SOURCE_DIR "${_subset_default_sdl_dir}" CACHE PATH "Path to local SDL3 source directory (must contain CMakeLists.txt)" FORCE)
    endif()

    if(WICKED_SUBSET_SDL_SOURCE_DIR STREQUAL "" OR NOT EXISTS "${WICKED_SUBSET_SDL_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "WICKED_SUBSET_NEEDS_SDL3 is ON but local SDL3 source was not found. "
            "Set WICKED_SUBSET_SDL_SOURCE_DIR to your local SDL3 checkout.")
    endif()

    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)

    message(STATUS "Wicked subset: configuring local SDL3 from '${WICKED_SUBSET_SDL_SOURCE_DIR}'")
    add_subdirectory("${WICKED_SUBSET_SDL_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/wicked_subset_sdl3" EXCLUDE_FROM_ALL)

    if(NOT TARGET SDL3::SDL3)
        if(TARGET SDL3::SDL3-static)
            add_library(SDL3::SDL3 ALIAS SDL3::SDL3-static)
        elseif(TARGET SDL3-static)
            add_library(SDL3::SDL3 ALIAS SDL3-static)
        elseif(TARGET SDL3)
            add_library(SDL3::SDL3 ALIAS SDL3)
        else()
            message(FATAL_ERROR "Local SDL build did not expose SDL3::SDL3")
        endif()
    endif()
endif()
if(WICKED_SUBSET_NEEDS_SDL3)
    message(STATUS "Wicked subset: SDL3 target available -> SDL3::SDL3")
endif()

if(WICKED_SUBSET_ENABLE_SHADERCOMPILER)
    wicked_subset_add_object(wicked_subset_shadercompiler_obj
        "${WICKED_SUBSET_ENGINE_ROOT}/wiShaderCompiler.cpp"
    )
endif()

if(WICKED_SUBSET_ENABLE_DX12)
    if(WIN32)
        wicked_subset_add_object(wicked_subset_graphics_dx12_obj
            "${WICKED_SUBSET_ENGINE_ROOT}/wiGraphicsDevice_DX12.cpp"
        )
    else()
        message(WARNING "WICKED_SUBSET_ENABLE_DX12 ignored on non-Windows platform")
    endif()
endif()

if(WICKED_SUBSET_ENABLE_VULKAN)
    wicked_subset_add_object(wicked_subset_graphics_vulkan_obj
        "${WICKED_SUBSET_ENGINE_ROOT}/wiGraphicsDevice_Vulkan.cpp"
    )
    target_compile_definitions(wicked_subset_graphics_vulkan_obj PRIVATE
        WICKEDENGINE_BUILD_VULKAN
    )
endif()

if(WICKED_SUBSET_ENABLE_METAL)
    if(APPLE)
        wicked_subset_add_object(wicked_subset_graphics_metal_obj
            "${WICKED_SUBSET_ENGINE_ROOT}/wiGraphicsDevice_Metal.cpp"
        )
        target_compile_options(wicked_subset_graphics_metal_obj PRIVATE
            "$<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>"
            "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>"
        )
    else()
        message(WARNING "WICKED_SUBSET_ENABLE_METAL ignored on non-Apple platform")
    endif()
endif()

if(WICKED_SUBSET_OBJECT_TARGETS STREQUAL "")
    message(FATAL_ERROR "No Wicked graphics/shader subset objects were enabled")
endif()

add_custom_target(wicked_graphics_subset_compile_min ALL)
foreach(_subset_tgt IN LISTS WICKED_SUBSET_OBJECT_TARGETS)
    add_dependencies(wicked_graphics_subset_compile_min ${_subset_tgt})
endforeach()

function(wicked_subset_copy_shadercompiler_runtime target)
    if(APPLE)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${WICKED_SUBSET_ENGINE_ROOT}/libdxcompiler.dylib"
                "$<TARGET_FILE_DIR:${target}>/libdxcompiler.dylib"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${WICKED_SUBSET_ENGINE_ROOT}/libmetalirconverter.dylib"
                "$<TARGET_FILE_DIR:${target}>/libmetalirconverter.dylib"
        )
    elseif(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${WICKED_SUBSET_ENGINE_ROOT}/dxcompiler.dll"
                "$<TARGET_FILE_DIR:${target}>/dxcompiler.dll"
        )
        if(EXISTS "${WICKED_SUBSET_ENGINE_ROOT}/dxil.dll")
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${WICKED_SUBSET_ENGINE_ROOT}/dxil.dll"
                    "$<TARGET_FILE_DIR:${target}>/dxil.dll"
            )
        endif()
    else()
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${WICKED_SUBSET_ENGINE_ROOT}/libdxcompiler.so"
                "$<TARGET_FILE_DIR:${target}>/libdxcompiler.so"
        )
    endif()
endfunction()

if(WICKED_SUBSET_BUILD_CUBE_DEMO)
    if(NOT WICKED_SUBSET_ENABLE_SHADERCOMPILER)
        message(FATAL_ERROR "WICKED_SUBSET_BUILD_CUBE_DEMO requires WICKED_SUBSET_ENABLE_SHADERCOMPILER=ON")
    endif()

    set(WICKED_SUBSET_DEMO_SOURCES
        "${WICKED_SUBSET_SRC_DIR}/wicked_subset_cube_demo.cpp"
        "${WICKED_SUBSET_SRC_DIR}/wicked_runtime_stubs.cpp"
        "${WICKED_SUBSET_SRC_DIR}/wicked_shadercompiler_support_stubs.cpp"
        "${WICKED_SUBSET_SHADER_DIR}/wicked_subset_cube_demo.hlsl"
        "${WICKED_SUBSET_SHADER_DIR}/wicked_subset_shader_compat.hlsli"
    )

    if(APPLE)
        list(APPEND WICKED_SUBSET_DEMO_SOURCES
            "${WICKED_SUBSET_SRC_DIR}/platform_window_helpers_apple.mm"
            "${WICKED_SUBSET_SRC_DIR}/wicked_runtime_apple.mm"
        )
    else()
        list(APPEND WICKED_SUBSET_DEMO_SOURCES
            "${WICKED_SUBSET_SRC_DIR}/platform_window_helpers_stub.cpp"
        )
    endif()

    add_executable(wicked_subset_cube_demo ${WICKED_SUBSET_DEMO_SOURCES})
    wicked_subset_apply_common_flags(wicked_subset_cube_demo)

    target_compile_definitions(wicked_subset_cube_demo PRIVATE
        WICKED_SUBSET_CUBE_SHADER_PATH="${WICKED_SUBSET_SHADER_DIR}/wicked_subset_cube_demo.hlsl"
        WICKED_SUBSET_ENGINE_SHADER_DIR="${WICKED_SUBSET_ENGINE_ROOT}/shaders"
    )

    wicked_subset_apply_mmgr_settings(wicked_subset_cube_demo)

    target_include_directories(wicked_subset_cube_demo PRIVATE
        "${WICKED_SUBSET_SRC_DIR}"
        "${WICKED_SUBSET_SHADER_DIR}"
        "${WICKED_SUBSET_ENGINE_ROOT}"
        "${WICKED_SUBSET_ENGINE_ROOT}/Utility/metal"
    )

    target_link_libraries(wicked_subset_cube_demo PRIVATE
        wicked_subset_shadercompiler_obj
    )

    wicked_subset_link_mmgr_runtime_object(wicked_subset_cube_demo)

    if(WICKED_ENGINECONFIG_SUBSET_BACKEND EQUAL WICKED_SUBSET_BACKEND_METAL)
        if(NOT APPLE)
            message(FATAL_ERROR "EngineConfig requests Metal backend (WI_ENGINECONFIG_SUBSET_BACKEND=2), but this platform is not Apple.")
        endif()
        if(NOT TARGET wicked_subset_graphics_metal_obj)
            message(FATAL_ERROR "EngineConfig requests Metal backend (WI_ENGINECONFIG_SUBSET_BACKEND=2), but WICKED_SUBSET_ENABLE_METAL is OFF.")
        endif()
        target_link_libraries(wicked_subset_cube_demo PRIVATE
            wicked_subset_graphics_metal_obj
        )
    elseif(WICKED_ENGINECONFIG_SUBSET_BACKEND EQUAL WICKED_SUBSET_BACKEND_DX12)
        if(NOT WIN32)
            message(FATAL_ERROR "EngineConfig requests DX12 backend (WI_ENGINECONFIG_SUBSET_BACKEND=1), but this platform is not Windows.")
        endif()
        if(NOT TARGET wicked_subset_graphics_dx12_obj)
            message(FATAL_ERROR "EngineConfig requests DX12 backend (WI_ENGINECONFIG_SUBSET_BACKEND=1), but WICKED_SUBSET_ENABLE_DX12 is OFF.")
        endif()
        target_link_libraries(wicked_subset_cube_demo PRIVATE
            wicked_subset_graphics_dx12_obj
        )
        target_link_libraries(wicked_subset_cube_demo PRIVATE d3d12 dxgi dxguid)
    elseif(WICKED_ENGINECONFIG_SUBSET_BACKEND EQUAL WICKED_SUBSET_BACKEND_VULKAN)
        if(NOT TARGET wicked_subset_graphics_vulkan_obj)
            message(FATAL_ERROR "EngineConfig requests Vulkan backend (WI_ENGINECONFIG_SUBSET_BACKEND=0), but WICKED_SUBSET_ENABLE_VULKAN is OFF.")
        endif()
        target_compile_definitions(wicked_subset_cube_demo PRIVATE
            WICKEDENGINE_BUILD_VULKAN
        )
        target_link_libraries(wicked_subset_cube_demo PRIVATE
            wicked_subset_graphics_vulkan_obj
        )
        target_sources(wicked_subset_cube_demo PRIVATE
            "${WICKED_SUBSET_ENGINE_ROOT}/Utility/spirv_reflect.c"
        )
    else()
        message(FATAL_ERROR "Unknown subset backend id: ${WICKED_ENGINECONFIG_SUBSET_BACKEND}")
    endif()

    if(APPLE)
        target_compile_options(wicked_subset_cube_demo PRIVATE
            "$<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>"
            "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>"
        )
    endif()

    target_link_libraries(wicked_subset_cube_demo PRIVATE SDL3::SDL3)

    if(APPLE)
        target_link_libraries(wicked_subset_cube_demo PRIVATE
            "-framework Cocoa"
            "-framework Foundation"
            "-framework Metal"
            "-framework MetalKit"
            "-framework MetalPerformanceShaders"
            "-framework QuartzCore"
            "-framework CoreGraphics"
            "-framework CoreVideo"
            "-framework IOKit"
        )
    elseif(UNIX)
        target_link_libraries(wicked_subset_cube_demo PRIVATE dl)
    endif()

    wicked_subset_copy_shadercompiler_runtime(wicked_subset_cube_demo)

    add_executable(wicked_subset_cube_demo_framebuffered ${WICKED_SUBSET_DEMO_SOURCES})
    wicked_subset_apply_common_flags(wicked_subset_cube_demo_framebuffered)

    target_compile_definitions(wicked_subset_cube_demo_framebuffered PRIVATE
        WICKED_SUBSET_CUBE_SHADER_PATH="${WICKED_SUBSET_SHADER_DIR}/wicked_subset_cube_demo.hlsl"
        WICKED_SUBSET_ENGINE_SHADER_DIR="${WICKED_SUBSET_ENGINE_ROOT}/shaders"
        WICKED_SUBSET_FRAME_BUFFERED=1
        WICKED_SUBSET_FRAME_SLOT_COUNT=16
    )

    wicked_subset_apply_mmgr_settings(wicked_subset_cube_demo_framebuffered)

    target_include_directories(wicked_subset_cube_demo_framebuffered PRIVATE
        "${WICKED_SUBSET_SRC_DIR}"
        "${WICKED_SUBSET_SHADER_DIR}"
        "${WICKED_SUBSET_ENGINE_ROOT}"
        "${WICKED_SUBSET_ENGINE_ROOT}/Utility/metal"
    )

    target_link_libraries(wicked_subset_cube_demo_framebuffered PRIVATE
        wicked_subset_shadercompiler_obj
    )

    wicked_subset_link_mmgr_runtime_object(wicked_subset_cube_demo_framebuffered)

    if(WICKED_ENGINECONFIG_SUBSET_BACKEND EQUAL WICKED_SUBSET_BACKEND_METAL)
        target_link_libraries(wicked_subset_cube_demo_framebuffered PRIVATE
            wicked_subset_graphics_metal_obj
        )
    elseif(WICKED_ENGINECONFIG_SUBSET_BACKEND EQUAL WICKED_SUBSET_BACKEND_DX12)
        target_link_libraries(wicked_subset_cube_demo_framebuffered PRIVATE
            wicked_subset_graphics_dx12_obj
        )
        target_link_libraries(wicked_subset_cube_demo_framebuffered PRIVATE d3d12 dxgi dxguid)
    elseif(WICKED_ENGINECONFIG_SUBSET_BACKEND EQUAL WICKED_SUBSET_BACKEND_VULKAN)
        target_compile_definitions(wicked_subset_cube_demo_framebuffered PRIVATE
            WICKEDENGINE_BUILD_VULKAN
        )
        target_link_libraries(wicked_subset_cube_demo_framebuffered PRIVATE
            wicked_subset_graphics_vulkan_obj
        )
        target_sources(wicked_subset_cube_demo_framebuffered PRIVATE
            "${WICKED_SUBSET_ENGINE_ROOT}/Utility/spirv_reflect.c"
        )
    endif()

    if(APPLE)
        target_compile_options(wicked_subset_cube_demo_framebuffered PRIVATE
            "$<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>"
            "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>"
        )
    endif()

    target_link_libraries(wicked_subset_cube_demo_framebuffered PRIVATE SDL3::SDL3)

    if(APPLE)
        target_link_libraries(wicked_subset_cube_demo_framebuffered PRIVATE
            "-framework Cocoa"
            "-framework Foundation"
            "-framework Metal"
            "-framework MetalKit"
            "-framework MetalPerformanceShaders"
            "-framework QuartzCore"
            "-framework CoreGraphics"
            "-framework CoreVideo"
            "-framework IOKit"
        )
    elseif(UNIX)
        target_link_libraries(wicked_subset_cube_demo_framebuffered PRIVATE dl)
    endif()

    wicked_subset_copy_shadercompiler_runtime(wicked_subset_cube_demo_framebuffered)

    set(WICKED_SUBSET_INDIRECT_COMPARE_DEMO_SOURCES
        "${WICKED_SUBSET_SRC_DIR}/wicked_subset_cube_indirect_compare_demo.cpp"
        "${WICKED_SUBSET_SRC_DIR}/wicked_runtime_stubs.cpp"
        "${WICKED_SUBSET_SRC_DIR}/wicked_shadercompiler_support_stubs.cpp"
        "${WICKED_SUBSET_SHADER_DIR}/wicked_subset_cube_indirect_compare_demo.hlsl"
        "${WICKED_SUBSET_SHADER_DIR}/wicked_subset_shader_compat.hlsli"
    )

    if(APPLE)
        list(APPEND WICKED_SUBSET_INDIRECT_COMPARE_DEMO_SOURCES
            "${WICKED_SUBSET_SRC_DIR}/platform_window_helpers_apple.mm"
            "${WICKED_SUBSET_SRC_DIR}/wicked_runtime_apple.mm"
        )
    else()
        list(APPEND WICKED_SUBSET_INDIRECT_COMPARE_DEMO_SOURCES
            "${WICKED_SUBSET_SRC_DIR}/platform_window_helpers_stub.cpp"
        )
    endif()

    add_executable(wicked_subset_cube_indirect_compare_demo ${WICKED_SUBSET_INDIRECT_COMPARE_DEMO_SOURCES})
    wicked_subset_apply_common_flags(wicked_subset_cube_indirect_compare_demo)

    target_compile_definitions(wicked_subset_cube_indirect_compare_demo PRIVATE
        WICKED_SUBSET_CUBE_INDIRECT_COMPARE_SHADER_PATH="${WICKED_SUBSET_SHADER_DIR}/wicked_subset_cube_indirect_compare_demo.hlsl"
        WICKED_SUBSET_ENGINE_SHADER_DIR="${WICKED_SUBSET_ENGINE_ROOT}/shaders"
    )

    wicked_subset_apply_mmgr_settings(wicked_subset_cube_indirect_compare_demo)

    target_include_directories(wicked_subset_cube_indirect_compare_demo PRIVATE
        "${WICKED_SUBSET_SRC_DIR}"
        "${WICKED_SUBSET_SHADER_DIR}"
        "${WICKED_SUBSET_ENGINE_ROOT}"
        "${WICKED_SUBSET_ENGINE_ROOT}/Utility/metal"
    )

    target_link_libraries(wicked_subset_cube_indirect_compare_demo PRIVATE
        wicked_subset_shadercompiler_obj
    )

    wicked_subset_link_mmgr_runtime_object(wicked_subset_cube_indirect_compare_demo)

    if(WICKED_ENGINECONFIG_SUBSET_BACKEND EQUAL WICKED_SUBSET_BACKEND_METAL)
        if(NOT APPLE)
            message(FATAL_ERROR "EngineConfig requests Metal backend (WI_ENGINECONFIG_SUBSET_BACKEND=2), but this platform is not Apple.")
        endif()
        if(NOT TARGET wicked_subset_graphics_metal_obj)
            message(FATAL_ERROR "EngineConfig requests Metal backend (WI_ENGINECONFIG_SUBSET_BACKEND=2), but WICKED_SUBSET_ENABLE_METAL is OFF.")
        endif()
        target_link_libraries(wicked_subset_cube_indirect_compare_demo PRIVATE
            wicked_subset_graphics_metal_obj
        )
    elseif(WICKED_ENGINECONFIG_SUBSET_BACKEND EQUAL WICKED_SUBSET_BACKEND_DX12)
        if(NOT WIN32)
            message(FATAL_ERROR "EngineConfig requests DX12 backend (WI_ENGINECONFIG_SUBSET_BACKEND=1), but this platform is not Windows.")
        endif()
        if(NOT TARGET wicked_subset_graphics_dx12_obj)
            message(FATAL_ERROR "EngineConfig requests DX12 backend (WI_ENGINECONFIG_SUBSET_BACKEND=1), but WICKED_SUBSET_ENABLE_DX12 is OFF.")
        endif()
        target_link_libraries(wicked_subset_cube_indirect_compare_demo PRIVATE
            wicked_subset_graphics_dx12_obj
        )
        target_link_libraries(wicked_subset_cube_indirect_compare_demo PRIVATE d3d12 dxgi dxguid)
    elseif(WICKED_ENGINECONFIG_SUBSET_BACKEND EQUAL WICKED_SUBSET_BACKEND_VULKAN)
        if(NOT TARGET wicked_subset_graphics_vulkan_obj)
            message(FATAL_ERROR "EngineConfig requests Vulkan backend (WI_ENGINECONFIG_SUBSET_BACKEND=0), but WICKED_SUBSET_ENABLE_VULKAN is OFF.")
        endif()
        target_compile_definitions(wicked_subset_cube_indirect_compare_demo PRIVATE
            WICKEDENGINE_BUILD_VULKAN
        )
        target_link_libraries(wicked_subset_cube_indirect_compare_demo PRIVATE
            wicked_subset_graphics_vulkan_obj
        )
        target_sources(wicked_subset_cube_indirect_compare_demo PRIVATE
            "${WICKED_SUBSET_ENGINE_ROOT}/Utility/spirv_reflect.c"
        )
    else()
        message(FATAL_ERROR "Unknown subset backend id: ${WICKED_ENGINECONFIG_SUBSET_BACKEND}")
    endif()

    if(APPLE)
        target_compile_options(wicked_subset_cube_indirect_compare_demo PRIVATE
            "$<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>"
            "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>"
        )
    endif()

    target_link_libraries(wicked_subset_cube_indirect_compare_demo PRIVATE SDL3::SDL3)

    if(APPLE)
        target_link_libraries(wicked_subset_cube_indirect_compare_demo PRIVATE
            "-framework Cocoa"
            "-framework Foundation"
            "-framework Metal"
            "-framework MetalKit"
            "-framework MetalPerformanceShaders"
            "-framework QuartzCore"
            "-framework CoreGraphics"
            "-framework CoreVideo"
            "-framework IOKit"
        )
    elseif(UNIX)
        target_link_libraries(wicked_subset_cube_indirect_compare_demo PRIVATE dl)
    endif()

    wicked_subset_copy_shadercompiler_runtime(wicked_subset_cube_indirect_compare_demo)

    add_executable(wicked_subset_cube_indirect_compare_demo_framebuffered ${WICKED_SUBSET_INDIRECT_COMPARE_DEMO_SOURCES})
    wicked_subset_apply_common_flags(wicked_subset_cube_indirect_compare_demo_framebuffered)

    target_compile_definitions(wicked_subset_cube_indirect_compare_demo_framebuffered PRIVATE
        WICKED_SUBSET_CUBE_INDIRECT_COMPARE_SHADER_PATH="${WICKED_SUBSET_SHADER_DIR}/wicked_subset_cube_indirect_compare_demo.hlsl"
        WICKED_SUBSET_ENGINE_SHADER_DIR="${WICKED_SUBSET_ENGINE_ROOT}/shaders"
        WICKED_SUBSET_FRAME_BUFFERED=1
        WICKED_SUBSET_FRAME_SLOT_COUNT=16
    )

    wicked_subset_apply_mmgr_settings(wicked_subset_cube_indirect_compare_demo_framebuffered)

    target_include_directories(wicked_subset_cube_indirect_compare_demo_framebuffered PRIVATE
        "${WICKED_SUBSET_SRC_DIR}"
        "${WICKED_SUBSET_SHADER_DIR}"
        "${WICKED_SUBSET_ENGINE_ROOT}"
        "${WICKED_SUBSET_ENGINE_ROOT}/Utility/metal"
    )

    target_link_libraries(wicked_subset_cube_indirect_compare_demo_framebuffered PRIVATE
        wicked_subset_shadercompiler_obj
    )

    wicked_subset_link_mmgr_runtime_object(wicked_subset_cube_indirect_compare_demo_framebuffered)

    if(WICKED_ENGINECONFIG_SUBSET_BACKEND EQUAL WICKED_SUBSET_BACKEND_METAL)
        target_link_libraries(wicked_subset_cube_indirect_compare_demo_framebuffered PRIVATE
            wicked_subset_graphics_metal_obj
        )
    elseif(WICKED_ENGINECONFIG_SUBSET_BACKEND EQUAL WICKED_SUBSET_BACKEND_DX12)
        target_link_libraries(wicked_subset_cube_indirect_compare_demo_framebuffered PRIVATE
            wicked_subset_graphics_dx12_obj
        )
        target_link_libraries(wicked_subset_cube_indirect_compare_demo_framebuffered PRIVATE d3d12 dxgi dxguid)
    elseif(WICKED_ENGINECONFIG_SUBSET_BACKEND EQUAL WICKED_SUBSET_BACKEND_VULKAN)
        target_compile_definitions(wicked_subset_cube_indirect_compare_demo_framebuffered PRIVATE
            WICKEDENGINE_BUILD_VULKAN
        )
        target_link_libraries(wicked_subset_cube_indirect_compare_demo_framebuffered PRIVATE
            wicked_subset_graphics_vulkan_obj
        )
        target_sources(wicked_subset_cube_indirect_compare_demo_framebuffered PRIVATE
            "${WICKED_SUBSET_ENGINE_ROOT}/Utility/spirv_reflect.c"
        )
    endif()

    if(APPLE)
        target_compile_options(wicked_subset_cube_indirect_compare_demo_framebuffered PRIVATE
            "$<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>"
            "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>"
        )
    endif()

    target_link_libraries(wicked_subset_cube_indirect_compare_demo_framebuffered PRIVATE SDL3::SDL3)

    if(APPLE)
        target_link_libraries(wicked_subset_cube_indirect_compare_demo_framebuffered PRIVATE
            "-framework Cocoa"
            "-framework Foundation"
            "-framework Metal"
            "-framework MetalKit"
            "-framework MetalPerformanceShaders"
            "-framework QuartzCore"
            "-framework CoreGraphics"
            "-framework CoreVideo"
            "-framework IOKit"
        )
    elseif(UNIX)
        target_link_libraries(wicked_subset_cube_indirect_compare_demo_framebuffered PRIVATE dl)
    endif()

    wicked_subset_copy_shadercompiler_runtime(wicked_subset_cube_indirect_compare_demo_framebuffered)
endif()

if(WICKED_SUBSET_BUILD_VISIBILITY_BENCHMARK_DEMO)
    set(WICKED_SUBSET_VISIBILITY_BENCHMARK_DEMO_SOURCES
        "${WICKED_SUBSET_SRC_DIR}/wicked_subset_visibility_pipeline_benchmark.cpp"
        "${WICKED_SUBSET_SRC_DIR}/wicked_runtime_stubs.cpp"
        "${WICKED_SUBSET_SRC_DIR}/wicked_shadercompiler_support_stubs.cpp"
        "${WICKED_SUBSET_SHADER_DIR}/wicked_subset_visibility_pipeline_benchmark.hlsl"
        "${WICKED_SUBSET_SHADER_DIR}/wicked_subset_shader_compat.hlsli"
    )

    if(APPLE)
        list(APPEND WICKED_SUBSET_VISIBILITY_BENCHMARK_DEMO_SOURCES
            "${WICKED_SUBSET_SRC_DIR}/platform_window_helpers_apple.mm"
            "${WICKED_SUBSET_SRC_DIR}/wicked_runtime_apple.mm"
        )
    else()
        list(APPEND WICKED_SUBSET_VISIBILITY_BENCHMARK_DEMO_SOURCES
            "${WICKED_SUBSET_SRC_DIR}/platform_window_helpers_stub.cpp"
        )
    endif()

    add_executable(wicked_subset_visibility_pipeline_benchmark ${WICKED_SUBSET_VISIBILITY_BENCHMARK_DEMO_SOURCES})
    wicked_subset_apply_common_flags(wicked_subset_visibility_pipeline_benchmark)

    target_compile_definitions(wicked_subset_visibility_pipeline_benchmark PRIVATE
        WICKED_SUBSET_VISIBILITY_BENCHMARK_SHADER_PATH="${WICKED_SUBSET_SHADER_DIR}/wicked_subset_visibility_pipeline_benchmark.hlsl"
        WICKED_SUBSET_ENGINE_SHADER_DIR="${WICKED_SUBSET_ENGINE_ROOT}/shaders"
        WICKED_SUBSET_BACKEND=${WICKED_ENGINECONFIG_SUBSET_BACKEND}
    )

    wicked_subset_apply_mmgr_settings(wicked_subset_visibility_pipeline_benchmark)

    target_include_directories(wicked_subset_visibility_pipeline_benchmark PRIVATE
        "${WICKED_SUBSET_SRC_DIR}"
        "${WICKED_SUBSET_SHADER_DIR}"
        "${WICKED_SUBSET_ENGINE_ROOT}"
        "${WICKED_SUBSET_ENGINE_ROOT}/Utility/metal"
    )

    target_sources(wicked_subset_visibility_pipeline_benchmark PRIVATE
        $<TARGET_OBJECTS:wicked_subset_shadercompiler_obj>
    )

    wicked_subset_link_mmgr_runtime_object(wicked_subset_visibility_pipeline_benchmark)

    if(WICKED_ENGINECONFIG_SUBSET_BACKEND EQUAL WICKED_SUBSET_BACKEND_METAL)
        if(NOT APPLE)
            message(FATAL_ERROR "EngineConfig requests Metal backend (WI_ENGINECONFIG_SUBSET_BACKEND=2), but this platform is not Apple.")
        endif()
        if(NOT TARGET wicked_subset_graphics_metal_obj)
            message(FATAL_ERROR "EngineConfig requests Metal backend (WI_ENGINECONFIG_SUBSET_BACKEND=2), but WICKED_SUBSET_ENABLE_METAL is OFF.")
        endif()
        target_sources(wicked_subset_visibility_pipeline_benchmark PRIVATE
            $<TARGET_OBJECTS:wicked_subset_graphics_metal_obj>
        )
    elseif(WICKED_ENGINECONFIG_SUBSET_BACKEND EQUAL WICKED_SUBSET_BACKEND_DX12)
        if(NOT WIN32)
            message(FATAL_ERROR "EngineConfig requests DX12 backend (WI_ENGINECONFIG_SUBSET_BACKEND=1), but this platform is not Windows.")
        endif()
        if(NOT TARGET wicked_subset_graphics_dx12_obj)
            message(FATAL_ERROR "EngineConfig requests DX12 backend (WI_ENGINECONFIG_SUBSET_BACKEND=1), but WICKED_SUBSET_ENABLE_DX12 is OFF.")
        endif()
        target_sources(wicked_subset_visibility_pipeline_benchmark PRIVATE
            $<TARGET_OBJECTS:wicked_subset_graphics_dx12_obj>
        )
        target_link_libraries(wicked_subset_visibility_pipeline_benchmark PRIVATE d3d12 dxgi dxguid)
    elseif(WICKED_ENGINECONFIG_SUBSET_BACKEND EQUAL WICKED_SUBSET_BACKEND_VULKAN)
        if(NOT TARGET wicked_subset_graphics_vulkan_obj)
            message(FATAL_ERROR "EngineConfig requests Vulkan backend (WI_ENGINECONFIG_SUBSET_BACKEND=0), but WICKED_SUBSET_ENABLE_VULKAN is OFF.")
        endif()
        target_sources(wicked_subset_visibility_pipeline_benchmark PRIVATE
            $<TARGET_OBJECTS:wicked_subset_graphics_vulkan_obj>
            "${WICKED_SUBSET_ENGINE_ROOT}/Utility/spirv_reflect.c"
        )
    else()
        message(FATAL_ERROR "Unknown subset backend id: ${WICKED_ENGINECONFIG_SUBSET_BACKEND}")
    endif()

    if(APPLE)
        target_compile_options(wicked_subset_visibility_pipeline_benchmark PRIVATE
            "$<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>"
            "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>"
        )
    endif()

    target_link_libraries(wicked_subset_visibility_pipeline_benchmark PRIVATE SDL3::SDL3)

    if(APPLE)
        target_link_libraries(wicked_subset_visibility_pipeline_benchmark PRIVATE
            "-framework Cocoa"
            "-framework Foundation"
            "-framework Metal"
            "-framework MetalKit"
            "-framework MetalPerformanceShaders"
            "-framework QuartzCore"
            "-framework CoreGraphics"
            "-framework CoreVideo"
            "-framework IOKit"
        )
    elseif(UNIX)
        target_link_libraries(wicked_subset_visibility_pipeline_benchmark PRIVATE dl)
    endif()

    wicked_subset_copy_shadercompiler_runtime(wicked_subset_visibility_pipeline_benchmark)

    add_executable(wicked_subset_visibility_pipeline_benchmark_framebuffered ${WICKED_SUBSET_VISIBILITY_BENCHMARK_DEMO_SOURCES})
    wicked_subset_apply_common_flags(wicked_subset_visibility_pipeline_benchmark_framebuffered)

    target_compile_definitions(wicked_subset_visibility_pipeline_benchmark_framebuffered PRIVATE
        WICKED_SUBSET_VISIBILITY_BENCHMARK_SHADER_PATH="${WICKED_SUBSET_SHADER_DIR}/wicked_subset_visibility_pipeline_benchmark.hlsl"
        WICKED_SUBSET_ENGINE_SHADER_DIR="${WICKED_SUBSET_ENGINE_ROOT}/shaders"
        WICKED_SUBSET_BACKEND=${WICKED_ENGINECONFIG_SUBSET_BACKEND}
        WICKED_SUBSET_FRAME_BUFFERED=1
        WICKED_SUBSET_FRAME_SLOT_COUNT=16
    )

    wicked_subset_apply_mmgr_settings(wicked_subset_visibility_pipeline_benchmark_framebuffered)

    target_include_directories(wicked_subset_visibility_pipeline_benchmark_framebuffered PRIVATE
        "${WICKED_SUBSET_SRC_DIR}"
        "${WICKED_SUBSET_SHADER_DIR}"
        "${WICKED_SUBSET_ENGINE_ROOT}"
        "${WICKED_SUBSET_ENGINE_ROOT}/Utility/metal"
    )

    target_sources(wicked_subset_visibility_pipeline_benchmark_framebuffered PRIVATE
        $<TARGET_OBJECTS:wicked_subset_shadercompiler_obj>
    )
    wicked_subset_link_mmgr_runtime_object(wicked_subset_visibility_pipeline_benchmark_framebuffered)

    if(WICKED_ENGINECONFIG_SUBSET_BACKEND EQUAL WICKED_SUBSET_BACKEND_METAL)
        target_sources(wicked_subset_visibility_pipeline_benchmark_framebuffered PRIVATE
            $<TARGET_OBJECTS:wicked_subset_graphics_metal_obj>
        )
    elseif(WICKED_ENGINECONFIG_SUBSET_BACKEND EQUAL WICKED_SUBSET_BACKEND_DX12)
        target_sources(wicked_subset_visibility_pipeline_benchmark_framebuffered PRIVATE
            $<TARGET_OBJECTS:wicked_subset_graphics_dx12_obj>
        )
        target_link_libraries(wicked_subset_visibility_pipeline_benchmark_framebuffered PRIVATE d3d12 dxgi dxguid)
    elseif(WICKED_ENGINECONFIG_SUBSET_BACKEND EQUAL WICKED_SUBSET_BACKEND_VULKAN)
        target_sources(wicked_subset_visibility_pipeline_benchmark_framebuffered PRIVATE
            $<TARGET_OBJECTS:wicked_subset_graphics_vulkan_obj>
            "${WICKED_SUBSET_ENGINE_ROOT}/Utility/spirv_reflect.c"
        )
    endif()

    if(APPLE)
        target_compile_options(wicked_subset_visibility_pipeline_benchmark_framebuffered PRIVATE
            "$<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>"
            "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>"
        )
    endif()

    target_link_libraries(wicked_subset_visibility_pipeline_benchmark_framebuffered PRIVATE SDL3::SDL3)

    if(APPLE)
        target_link_libraries(wicked_subset_visibility_pipeline_benchmark_framebuffered PRIVATE
            "-framework Cocoa"
            "-framework Foundation"
            "-framework Metal"
            "-framework MetalKit"
            "-framework MetalPerformanceShaders"
            "-framework QuartzCore"
            "-framework CoreGraphics"
            "-framework CoreVideo"
            "-framework IOKit"
        )
    elseif(UNIX)
        target_link_libraries(wicked_subset_visibility_pipeline_benchmark_framebuffered PRIVATE dl)
    endif()

    wicked_subset_copy_shadercompiler_runtime(wicked_subset_visibility_pipeline_benchmark_framebuffered)
endif()

message(STATUS "Wicked graphics subset targets: ${WICKED_SUBSET_OBJECT_TARGETS}")
