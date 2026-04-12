cmake_minimum_required(VERSION 3.19)

# enable "codegen" target if supported by CMake
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.31)
    cmake_policy(SET CMP0171 NEW)
    set(codegen CODEGEN)
endif()

if (${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR})
    message(FATAL_ERROR
        "In-source builds are not supported!\n"
        "Run `git clean -d -f` to clean up the files CMake has created (stash "
        "your changes first, if you have made any), then run `cmake -B build "
        "<other_options>` followed by `cmake --build build --parallel`"
    )
endif()

project(WickedEngine)

include(CheckIPOSupported)

set(WICKED_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(WICKED_ENGINE_CONFIG_HEADER "${WICKED_ROOT_DIR}/WickedEngine/EngineConfig.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${WICKED_ENGINE_CONFIG_HEADER}")

function(wicked_read_engine_config_bool define_name out_var default_value)
    set(_value "${default_value}")
    if(EXISTS "${WICKED_ENGINE_CONFIG_HEADER}")
        file(STRINGS "${WICKED_ENGINE_CONFIG_HEADER}" _matches REGEX "^[ \t]*#define[ \t]+${define_name}[ \t]+[01][ \t]*$")
        if(_matches)
            list(GET _matches 0 _line)
            string(REGEX REPLACE "^[ \t]*#define[ \t]+${define_name}[ \t]+([01])[ \t]*$" "\\1" _value "${_line}")
        endif()
    endif()
    set(${out_var} "${_value}" PARENT_SCOPE)
endfunction()

function(wicked_read_engine_config_int define_name out_var default_value)
    set(_value "${default_value}")
    if(EXISTS "${WICKED_ENGINE_CONFIG_HEADER}")
        file(STRINGS "${WICKED_ENGINE_CONFIG_HEADER}" _matches REGEX "^[ \t]*#define[ \t]+${define_name}[ \t]+-?[0-9]+[ \t]*$")
        if(_matches)
            list(GET _matches 0 _line)
            string(REGEX REPLACE "^[ \t]*#define[ \t]+${define_name}[ \t]+(-?[0-9]+)[ \t]*$" "\\1" _value "${_line}")
        endif()
    endif()
    set(${out_var} "${_value}" PARENT_SCOPE)
endfunction()

wicked_read_engine_config_bool("WI_ENGINECONFIG_ENABLE_MMGR" WICKED_ENGINECONFIG_ENABLE_MMGR 0)
wicked_read_engine_config_bool("WI_ENGINECONFIG_SUBSET_USE_DX12" WICKED_ENGINECONFIG_SUBSET_USE_DX12 0)
set(WICKED_ENGINECONFIG_SUBSET_BACKEND_DEFAULT 0)
if(APPLE)
    set(WICKED_ENGINECONFIG_SUBSET_BACKEND_DEFAULT 2)
elseif(WIN32 AND WICKED_ENGINECONFIG_SUBSET_USE_DX12)
    set(WICKED_ENGINECONFIG_SUBSET_BACKEND_DEFAULT 1)
endif()
if(NOT DEFINED WICKED_ENGINECONFIG_SUBSET_BACKEND)
    wicked_read_engine_config_int("WI_ENGINECONFIG_SUBSET_BACKEND" WICKED_ENGINECONFIG_SUBSET_BACKEND "${WICKED_ENGINECONFIG_SUBSET_BACKEND_DEFAULT}")
endif()
if(WICKED_ENGINECONFIG_SUBSET_BACKEND EQUAL -1)
    set(WICKED_ENGINECONFIG_SUBSET_BACKEND "${WICKED_ENGINECONFIG_SUBSET_BACKEND_DEFAULT}")
endif()
if(NOT WICKED_ENGINECONFIG_SUBSET_BACKEND MATCHES "^[012]$")
    message(FATAL_ERROR "EngineConfig value WI_ENGINECONFIG_SUBSET_BACKEND must be 0 (Vulkan), 1 (DX12), or 2 (Metal).")
endif()
set(WICKED_ENGINECONFIG_SUBSET_BACKEND_NAME "Vulkan")
if(WICKED_ENGINECONFIG_SUBSET_BACKEND EQUAL 1)
    set(WICKED_ENGINECONFIG_SUBSET_BACKEND_NAME "DX12")
elseif(WICKED_ENGINECONFIG_SUBSET_BACKEND EQUAL 2)
    set(WICKED_ENGINECONFIG_SUBSET_BACKEND_NAME "Metal")
endif()

option(WICKED_DYNAMIC_LIBRARY "Build WickedEngine as a dynamic library" OFF)
option(WICKED_PIC "Build WickedEngine as position-independent code" WICKED_DYNAMIC_LIBRARY)
option(USE_LIBCXX "Link WickedEngine to llvm libc++ library - only available with the Clang compiler" OFF)
option(WICKED_BUILD_FULL_ENGINE "Build full WickedEngine library/editor/samples" OFF)
option(WICKED_BUILD_GRAPHICS_SUBSET "Build graphics device + shader compiler subset targets" ON)
option(WICKED_EDITOR "Build WickedEngine editor" ON)
option(WICKED_TESTS "Build WickedEngine tests" ON)
option(WICKED_IMGUI_EXAMPLE "Build WickedEngine imgui example" ON)
option(WICKED_ENABLE_IPO "Enable IPO/LTO in non-debug builds" NO)
option(WICKED_EMBED_SHADERS "Embed shaders into the library" NO)
option(WICKED_ENABLE_RTTI "Enable RTTI" NO)
option(WICKED_PLATFORM_QNX "Enable Vulkan QNX policy branch stubs" OFF)
option(WICKED_PLATFORM_SWITCH "Enable Vulkan Switch policy branch stubs" OFF)
option(WICKED_PLATFORM_SWITCH2 "Enable Vulkan Switch2 policy branch stubs" OFF)
if(UNIX)
    option(WICKED_ENABLE_ASAN "Enable AddressSanitizer in debug builds" OFF)
    option(WICKED_ENABLE_UBSAN "Enable UndefinedBehaviourSanitizer in debug builds" OFF)
    option(WICKED_ENABLE_TSAN "Enable ThreadSanitizer in debug builds" OFF)
    option(WICKED_ENABLE_SAN_ALWAYS "Enable the selected sanitizers in all builds, not just debug" OFF)
endif()
if(UNIX)
    option(WICKED_LINUX_TEMPLATE "Build WickedEngine Linux template" ON)
elseif(WIN32)
    option(WICKED_WINDOWS_TEMPLATE "Build WickedEngine Windows template" ON)
endif()

# Vulkan platform matrix compile definitions:
# These feed wiGraphicsDevice_Vulkan.h policy macros and can be overridden for
# stub branch compile coverage with WICKED_PLATFORM_QNX/SWITCH/SWITCH2.
set(WICKED_PLATFORM_STUB_OVERRIDE_COUNT 0)
if(WICKED_PLATFORM_QNX)
    math(EXPR WICKED_PLATFORM_STUB_OVERRIDE_COUNT "${WICKED_PLATFORM_STUB_OVERRIDE_COUNT} + 1")
endif()
if(WICKED_PLATFORM_SWITCH)
    math(EXPR WICKED_PLATFORM_STUB_OVERRIDE_COUNT "${WICKED_PLATFORM_STUB_OVERRIDE_COUNT} + 1")
endif()
if(WICKED_PLATFORM_SWITCH2)
    math(EXPR WICKED_PLATFORM_STUB_OVERRIDE_COUNT "${WICKED_PLATFORM_STUB_OVERRIDE_COUNT} + 1")
endif()
if(WICKED_PLATFORM_STUB_OVERRIDE_COUNT GREATER 1)
    message(FATAL_ERROR
        "Only one of WICKED_PLATFORM_QNX/WICKED_PLATFORM_SWITCH/WICKED_PLATFORM_SWITCH2 can be ON at a time."
    )
endif()

set(WICKED_VULKAN_PLATFORM_WIN32_VALUE 0)
set(WICKED_VULKAN_PLATFORM_LINUX_VALUE 0)
set(WICKED_VULKAN_PLATFORM_BSD_VALUE 0)
set(WICKED_VULKAN_PLATFORM_ANDROID_VALUE 0)
set(WICKED_VULKAN_PLATFORM_APPLE_VALUE 0)
set(WICKED_VULKAN_PLATFORM_QNX_VALUE 0)
set(WICKED_VULKAN_PLATFORM_SWITCH_VALUE 0)
set(WICKED_VULKAN_PLATFORM_SWITCH2_VALUE 0)

if(WICKED_PLATFORM_STUB_OVERRIDE_COUNT GREATER 0)
    if(WICKED_PLATFORM_QNX)
        set(WICKED_VULKAN_PLATFORM_QNX_VALUE 1)
    elseif(WICKED_PLATFORM_SWITCH)
        set(WICKED_VULKAN_PLATFORM_SWITCH_VALUE 1)
    elseif(WICKED_PLATFORM_SWITCH2)
        set(WICKED_VULKAN_PLATFORM_SWITCH2_VALUE 1)
    endif()
else()
    if(WIN32)
        set(WICKED_VULKAN_PLATFORM_WIN32_VALUE 1)
    endif()
    if(ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "Android")
        set(WICKED_VULKAN_PLATFORM_ANDROID_VALUE 1)
    endif()
    if(APPLE)
        set(WICKED_VULKAN_PLATFORM_APPLE_VALUE 1)
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(WICKED_VULKAN_PLATFORM_LINUX_VALUE 1)
    endif()
    if(CMAKE_SYSTEM_NAME MATCHES "^(FreeBSD|NetBSD|OpenBSD|DragonFly)$")
        set(WICKED_VULKAN_PLATFORM_BSD_VALUE 1)
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "QNX")
        set(WICKED_VULKAN_PLATFORM_QNX_VALUE 1)
    endif()
endif()

set(WICKED_VULKAN_PLATFORM_COMPILE_DEFINITIONS
    WICKED_VULKAN_PLATFORM_WIN32=${WICKED_VULKAN_PLATFORM_WIN32_VALUE}
    WICKED_VULKAN_PLATFORM_LINUX=${WICKED_VULKAN_PLATFORM_LINUX_VALUE}
    WICKED_VULKAN_PLATFORM_BSD=${WICKED_VULKAN_PLATFORM_BSD_VALUE}
    WICKED_VULKAN_PLATFORM_ANDROID=${WICKED_VULKAN_PLATFORM_ANDROID_VALUE}
    WICKED_VULKAN_PLATFORM_APPLE=${WICKED_VULKAN_PLATFORM_APPLE_VALUE}
    WICKED_VULKAN_PLATFORM_QNX=${WICKED_VULKAN_PLATFORM_QNX_VALUE}
    WICKED_VULKAN_PLATFORM_SWITCH=${WICKED_VULKAN_PLATFORM_SWITCH_VALUE}
    WICKED_VULKAN_PLATFORM_SWITCH2=${WICKED_VULKAN_PLATFORM_SWITCH2_VALUE}
)
message(STATUS
    "Vulkan platform policy: win32=${WICKED_VULKAN_PLATFORM_WIN32_VALUE}, "
    "linux=${WICKED_VULKAN_PLATFORM_LINUX_VALUE}, bsd=${WICKED_VULKAN_PLATFORM_BSD_VALUE}, "
    "android=${WICKED_VULKAN_PLATFORM_ANDROID_VALUE}, apple=${WICKED_VULKAN_PLATFORM_APPLE_VALUE}, "
    "qnx=${WICKED_VULKAN_PLATFORM_QNX_VALUE}, switch=${WICKED_VULKAN_PLATFORM_SWITCH_VALUE}, "
    "switch2=${WICKED_VULKAN_PLATFORM_SWITCH2_VALUE}"
)

set(WICKED_SUBSET_ENABLE_MMGR ${WICKED_ENGINECONFIG_ENABLE_MMGR})
set(WICKED_FULL_ENABLE_MMGR ${WICKED_ENGINECONFIG_ENABLE_MMGR})
message(STATUS "EngineConfig source: ${WICKED_ENGINE_CONFIG_HEADER}")
message(STATUS "EngineConfig values: WI_ENGINECONFIG_ENABLE_MMGR=${WICKED_ENGINECONFIG_ENABLE_MMGR}, WI_ENGINECONFIG_SUBSET_USE_DX12=${WICKED_ENGINECONFIG_SUBSET_USE_DX12}, WI_ENGINECONFIG_SUBSET_BACKEND=${WICKED_ENGINECONFIG_SUBSET_BACKEND} (${WICKED_ENGINECONFIG_SUBSET_BACKEND_NAME})")

if (WICKED_BUILD_GRAPHICS_SUBSET AND NOT WICKED_BUILD_FULL_ENGINE)
    set(symlink_default OFF)
elseif (CMAKE_HOST_WIN32)
    set(symlink_default OFF)
else()
    set(symlink_default ON)
endif()
option(WICKED_ENABLE_SYMLINKS "Prefer symlinking over copying directories" ${symlink_default})

# check that IPO is supported when turned on
if(WICKED_ENABLE_IPO)
    check_ipo_supported()
endif()

if(WICKED_ENABLE_SYMLINKS)
    # check for symlink support (on windows it requires admin or developer mode)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E create_symlink CMakeLists.txt ${CMAKE_CURRENT_BINARY_DIR}/symlink-test
        ERROR_QUIET
        RESULT_VARIABLE symlink_check_result
    )
    if(symlink_check_result EQUAL 0)
        file(REMOVE ${CMAKE_BINARY_DIR}/symlink-test)
        set(COPY_OR_SYMLINK_DIR_CMD create_symlink)
    else()
        message(STATUS "Symlinks are not supported, falling back to directory copy")
        if(CMAKE_VERSION VERSION_LESS "3.26.0")
            set(COPY_OR_SYMLINK_DIR_CMD copy_directory)
        else()
            set(COPY_OR_SYMLINK_DIR_CMD copy_directory_if_different)
        endif()
    endif()
else()
    if(CMAKE_VERSION VERSION_LESS "3.26.0")
        set(COPY_OR_SYMLINK_DIR_CMD copy_directory)
    else()
        set(COPY_OR_SYMLINK_DIR_CMD copy_directory_if_different)
    endif()
endif()


# Configure CMake global variables
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ${WICKED_PIC})

# Use solution folders to organize projects
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(WICKED_ENABLE_ASAN AND WICKED_ENABLE_TSAN)
    message(FATAL_ERROR "WICKED_ENABLE_ASAN and WICKED_ENABLE_TSAN are mutually exclusive!")
endif()

set(WICKED_MMGR_COVERAGE_MODE "disabled")
if(WICKED_BUILD_GRAPHICS_SUBSET AND WICKED_SUBSET_ENABLE_MMGR)
    set(WICKED_MMGR_COVERAGE_MODE "subset")
endif()
if(WICKED_BUILD_FULL_ENGINE AND WICKED_FULL_ENABLE_MMGR)
    if(WICKED_MMGR_COVERAGE_MODE STREQUAL "subset")
        set(WICKED_MMGR_COVERAGE_MODE "subset+full")
    else()
        set(WICKED_MMGR_COVERAGE_MODE "full")
    endif()
endif()

if(WICKED_SUBSET_ENABLE_MMGR AND NOT WICKED_BUILD_GRAPHICS_SUBSET)
    message(STATUS "MMGR subset mode requested, but WICKED_BUILD_GRAPHICS_SUBSET=OFF; subset MMGR integration is inactive.")
endif()
if(WICKED_FULL_ENABLE_MMGR AND NOT WICKED_BUILD_FULL_ENGINE)
    message(STATUS "MMGR full-engine mode requested, but WICKED_BUILD_FULL_ENGINE=OFF; full-engine MMGR integration is inactive.")
endif()
message(STATUS "MMGR status: subset=${WICKED_SUBSET_ENABLE_MMGR}, full=${WICKED_FULL_ENABLE_MMGR}, coverage=${WICKED_MMGR_COVERAGE_MODE}")

if(WICKED_ENABLE_ASAN)
    set(SANITIZE_OPTIONS "-fsanitize=address")
endif()

if(WICKED_ENABLE_TSAN)
    set(SANITIZE_OPTIONS "${SANITIZE_OPTIONS} -fsanitize=thread")
endif()

if(WICKED_ENABLE_UBSAN)
    set(SANITIZE_OPTIONS "${SANITIZE_OPTIONS} -fsanitize=undefined")
endif()

if(WICKED_ENABLE_SAN_ALWAYS)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SANITIZE_OPTIONS}")
    add_link_options(${SANITIZE_LINK_OPTIONS})
else()
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} ${SANITIZE_OPTIONS}")
    add_link_options($<$<CONFIG:Debug>:${SANITIZE_LINK_OPTIONS}>)
endif()

if (WIN32)
    set(PLATFORM "Windows")
    add_compile_definitions(WIN32=1)
    add_compile_definitions(_HAS_EXCEPTIONS=0)
elseif (UNIX)
    if (WICKED_BUILD_GRAPHICS_SUBSET AND NOT WICKED_BUILD_FULL_ENGINE)
        set(PLATFORM "SDL3")
    else()
        set(PLATFORM "SDL2")
    endif()
    add_compile_definitions(_GLIBCXX_USE_CXX11_ABI=1)
endif()

if (MSVC)
    add_compile_options(
        /W3 # warning level 3
        /MP # multi-processor compilation
        /EHsc- # exceptions disabled
        $<$<AND:$<NOT:$<BOOL:${ubsan_active}>>,$<NOT:$<BOOL:${WICKED_ENABLE_RTTI}>>>:/GR-> # runtime type information disabled
        $<$<CONFIG:Release>:/GS-> # security check disabled in Release
    )
else()
    set(ubsan_active
         "$<AND:$<BOOL:${WICKED_ENABLE_UBSAN}>,$<OR:$<CONFIG:Debug>,$<BOOL:${WICKED_ENABLE_SAN_ALWAYS}>>>"
    )
    # Common compiler options and warning level for CLANG and GCC:
    add_compile_options(
        -Wall # warning level: all

        # some warnings are disabled to better match MSVC warning level 3:
        -Wno-unused-variable
        -Wno-unused-function
        -Wno-unused-but-set-variable
        -Wno-sign-compare

        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions> # exceptions disabled
        $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<NOT:$<BOOL:${ubsan_active}>>,$<NOT:$<BOOL:${WICKED_ENABLE_RTTI}>>>:-fno-rtti> # runtime type information disabled

        # security checks disabled in Release:
        $<$<CONFIG:Release>:-fno-stack-protector>
        $<$<CONFIG:Release>:-fcf-protection=none>
        $<$<AND:$<CONFIG:Release>,$<NOT:$<PLATFORM_ID:Windows>>>:-fno-stack-clash-protection> # not supported on Windows
        $<$<CONFIG:Release>:-fno-stack-check>
        $<$<AND:$<CONFIG:Release>,$<NOT:$<PLATFORM_ID:Windows>>>:-fno-asynchronous-unwind-tables>  # seems to crash LUA evaluation on Windows
    )
endif()

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # CLANG specific compile options:
    add_compile_options(
        -fdeclspec
        -fms-extensions

        -Wno-nullability-completeness
        -Wno-unused-private-field

        # Increase Ccache hit rate, note that you also need to set config
        # options in your Ccache config
        # see https://ccache.dev/manual/latest.html#_precompiled_headers
        "SHELL:-Xclang -fno-pch-timestamp"
    )
    if (USE_LIBCXX)
        add_compile_options(
            $<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>
        )
        add_link_options(
            -stdlib=libc++
        )
    endif()
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC specific compile options:
    add_compile_options(
        -Wno-strict-aliasing
    )
endif()

if (WICKED_BUILD_GRAPHICS_SUBSET)
    message(STATUS "Configuring Wicked graphics subset (MMGR subset=${WICKED_SUBSET_ENABLE_MMGR}, coverage=${WICKED_MMGR_COVERAGE_MODE})")
    add_subdirectory(Samples/WickedGraphicsSubset)
endif()

if (WICKED_BUILD_FULL_ENGINE)
    add_subdirectory(WickedEngine)
    add_custom_target(Content
       COMMAND ${CMAKE_COMMAND} -E ${COPY_OR_SYMLINK_DIR_CMD} ${WICKED_ROOT_DIR}/Content ${CMAKE_CURRENT_BINARY_DIR}/Content
       COMMENT "$<IF:$<BOOL:${WICKED_ENABLE_SYMLINKS}>,Symlinking,Copying> Content directory"
       VERBATIM
    )

    if (WICKED_EDITOR)
        add_subdirectory(Editor)
    endif()

    if (WICKED_TESTS)
        add_subdirectory(Samples/Tests)
    endif()

    if (WICKED_IMGUI_EXAMPLE)
        add_subdirectory(Samples/Example_ImGui)
        add_subdirectory(Samples/Example_ImGui_Docking)
    endif()

    if (WICKED_LINUX_TEMPLATE)
        add_subdirectory(Samples/Template_Linux)
    endif()

    if (WICKED_WINDOWS_TEMPLATE)
        add_subdirectory(Samples/Template_Windows)
    endif()
endif()
