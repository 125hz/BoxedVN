# BoxedVN - iOS/iPadOS port of Boxedwine
# Copyright (C) 2012-2026  The BoxedWine Team
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.  See license.txt.
#
# ---------------------------------------------------------------------------
# This CMake graph builds the *native code* of BoxedVN:
#
#   boxedwine_core    - the upstream Boxedwine emulator, unmodified apart from
#                       a handful of narrow TARGET_OS_IPHONE guards.
#   boxedvn_support   - host-independent BoxedVN logic (PE architecture
#                       detection, safe ZIP extraction, game manifests).
#   boxedvn_runtime   - the C ABI the Apple frontend calls into.
#   boxedvn_tests     - ctest suite for boxedvn_support (runs on the host).
#
# The SwiftUI application shell is generated separately by XcodeGen from
# ios/project.yml and links the static libraries produced here.  See
# docs/BUILD_IOS.md for the rationale.
# ---------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.24)

project(BoxedVN
    VERSION 0.1.0
    DESCRIPTION "iOS/iPadOS port of Boxedwine"
    LANGUAGES C CXX
)

if(APPLE)
    enable_language(OBJC)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(cmake/BoxedwineSources.cmake)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(BOXEDVN_BUILD_TESTS      "Build the host-independent unit tests"        ON)
option(BOXEDVN_BUILD_CORE       "Build the Boxedwine emulator core"            ON)
option(BOXEDVN_BUILD_RUNTIME    "Build the Objective-C++ runtime bridge (iOS)"  ON)
# A runnable macOS host for the emulator. This exists to debug the guest stack
# on the development machine, where lldb can attach and a rebuild is seconds
# rather than a sign-and-install cycle. It is not a shipping product: the Swift
# frontend, the StikDebug JIT handshake and the UIKit presentation path are all
# iOS-only. Note also that an Intel Mac exercises Boxedwine's x64 JIT, so an
# ARM64 codegen defect will not reproduce here.
option(BOXEDVN_BUILD_MACOS_HOST "Build a runnable macOS emulator host"          OFF)
option(BOXEDVN_ENABLE_UI        "Build Boxedwine's built-in Dear ImGui launcher" OFF)
option(BOXEDVN_ENABLE_MIDI      "Build CoreMIDI support"                       OFF)

set(BOXEDVN_SDL2_ROOT "" CACHE PATH
    "Root of a prebuilt SDL2 for the target platform (contains include/ and lib/)")
set(BOXEDVN_MOLTENVK_ROOT "" CACHE PATH
    "Root of Khronos's extracted MoltenVK iOS package")

# ---------------------------------------------------------------------------
# Platform identification
# ---------------------------------------------------------------------------
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(BOXEDVN_TARGET_IOS TRUE)
else()
    set(BOXEDVN_TARGET_IOS FALSE)
endif()

message(STATUS "BoxedVN: system      = ${CMAKE_SYSTEM_NAME}")
message(STATUS "BoxedVN: osx archs   = ${CMAKE_OSX_ARCHITECTURES}")
message(STATUS "BoxedVN: build type  = ${CMAKE_BUILD_TYPE}")

# ---------------------------------------------------------------------------
# zlib + minizip.  Built here rather than in the core block because
# boxedvn_support uses minizip for game imports and must be buildable (and
# testable) without SDL2 or the emulator.
# ---------------------------------------------------------------------------
add_library(boxedwine_zlib STATIC ${BOXEDWINE_ZLIB} ${BOXEDWINE_MINIZIP})
target_include_directories(boxedwine_zlib PUBLIC
    "${BOXEDWINE_ROOT}/lib/zlib"
    "${BOXEDWINE_ROOT}/lib/zlib/contrib/minizip"
)
target_compile_definitions(boxedwine_zlib PRIVATE HAVE_UNISTD_H=1 HAVE_STDARG_H=1)
# The vendored zlib predates the Apple SDK change that makes <sys/cdefs.h> pull
# in <TargetConditionals.h>.  Its zutil.h sees TARGET_OS_MAC, assumes classic
# Mac OS, and does `#ifndef fdopen / #define fdopen(fd,mode) NULL`, which then
# mangles the real fdopen() declaration in <stdio.h>.  Predefining fdopen as an
# identity macro takes that branch out without patching the vendored source.
target_compile_definitions(boxedwine_zlib PRIVATE fdopen=fdopen)
target_compile_options(boxedwine_zlib PRIVATE -w)

# ---------------------------------------------------------------------------
# boxedvn_support : host-independent logic, no Boxedwine headers, fully tested
# ---------------------------------------------------------------------------
add_subdirectory(ios/support)

if(BOXEDVN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(ios/tests)
endif()

if(NOT BOXEDVN_BUILD_CORE)
    message(STATUS "BoxedVN: emulator core disabled (BOXEDVN_BUILD_CORE=OFF)")
    return()
endif()

# ---------------------------------------------------------------------------
# SDL2 discovery
# ---------------------------------------------------------------------------
if(BOXEDVN_SDL2_ROOT)
    set(BOXEDVN_SDL2_INCLUDE_DIR "${BOXEDVN_SDL2_ROOT}/include/SDL2")
    if(NOT EXISTS "${BOXEDVN_SDL2_INCLUDE_DIR}/SDL.h")
        # Some SDL install layouts put headers directly under include/.
        set(BOXEDVN_SDL2_INCLUDE_DIR "${BOXEDVN_SDL2_ROOT}/include")
    endif()
    if(NOT EXISTS "${BOXEDVN_SDL2_INCLUDE_DIR}/SDL.h")
        message(FATAL_ERROR
            "BOXEDVN_SDL2_ROOT=${BOXEDVN_SDL2_ROOT} does not contain SDL.h.\n"
            "Run scripts/fetch-dependencies.sh to build a pinned SDL2 first.")
    endif()
    # NO_CMAKE_FIND_ROOT_PATH: when cross-compiling for iOS, CMake otherwise
    # rewrites the search path into the iPhoneOS sysroot and cannot see our
    # own prefix.
    find_library(BOXEDVN_SDL2_LIBRARY
        NAMES SDL2 libSDL2.a
        PATHS "${BOXEDVN_SDL2_ROOT}/lib"
        NO_DEFAULT_PATH
        NO_CMAKE_FIND_ROOT_PATH
        REQUIRED
    )
    message(STATUS "BoxedVN: SDL2 include = ${BOXEDVN_SDL2_INCLUDE_DIR}")
    message(STATUS "BoxedVN: SDL2 library = ${BOXEDVN_SDL2_LIBRARY}")
else()
    message(FATAL_ERROR
        "BOXEDVN_SDL2_ROOT is not set.\n"
        "Run scripts/fetch-dependencies.sh (it pins and builds SDL2), then pass\n"
        "  -DBOXEDVN_SDL2_ROOT=<path>\n"
        "or use one of the presets in CMakePresets.json.")
endif()

# ---------------------------------------------------------------------------
# Third-party static libraries built with their own (looser) warning settings
# ---------------------------------------------------------------------------
add_library(boxedwine_softfloat STATIC ${BOXEDWINE_SOFTFLOAT})
target_include_directories(boxedwine_softfloat PUBLIC ${BOXEDWINE_SOFTFLOAT_INCLUDE_DIRS})
target_compile_options(boxedwine_softfloat PRIVATE -w)

add_library(boxedwine_asmjit STATIC ${BOXEDWINE_ASMJIT_CPP})
target_include_directories(boxedwine_asmjit PUBLIC "${BOXEDWINE_ROOT}/lib/asmjit")
target_compile_definitions(boxedwine_asmjit PUBLIC ASMJIT_STATIC)
target_compile_options(boxedwine_asmjit PRIVATE -w)

if(BOXEDVN_TARGET_IOS)
    # asmjit's virtmem.cpp calls sys_icache_invalidate() on every __APPLE__
    # target but only includes <libkern/OSCacheControl.h> under TARGET_OS_OSX,
    # so it does not compile for iOS.  Force-including the header fixes it
    # without patching the vendored third-party source.
    target_compile_options(boxedwine_asmjit PRIVATE
        -include libkern/OSCacheControl.h)
endif()

add_library(boxedwine_pugixml STATIC ${BOXEDWINE_PUGIXML})
target_include_directories(boxedwine_pugixml PUBLIC "${BOXEDWINE_ROOT}/lib/pugixml/src")
target_compile_options(boxedwine_pugixml PRIVATE -w)

# ---------------------------------------------------------------------------
# boxedwine_core
# ---------------------------------------------------------------------------
set(BOXEDWINE_CORE_SOURCES
    ${BOXEDWINE_SOURCE_CPP}
    ${BOXEDWINE_PLATFORM_POSIX}
    ${BOXEDWINE_PLATFORM_SDL}
)

if(NOT BOXEDVN_TARGET_IOS)
    list(APPEND BOXEDWINE_CORE_SOURCES ${BOXEDWINE_PLATFORM_MACOS_ONLY})
    if(BOXEDVN_ENABLE_MIDI)
        list(APPEND BOXEDWINE_CORE_SOURCES ${BOXEDWINE_PLATFORM_MACOS_MIDI})
    endif()
elseif(BOXEDVN_ENABLE_MIDI)
    message(FATAL_ERROR
        "BOXEDVN_ENABLE_MIDI is not supported on iOS: platform/mac/coremidi.cpp "
        "depends on the macOS-only DLS synth AudioUnit.")
endif()

if(BOXEDVN_ENABLE_UI)
    list(APPEND BOXEDWINE_CORE_SOURCES ${BOXEDWINE_IMGUI} ${BOXEDWINE_GLEW})
else()
    # source/ui/** is the Dear ImGui launcher; with BOXEDWINE_DISABLE_UI the
    # emulator is driven entirely by startup arguments, which is what the
    # BoxedVN SwiftUI frontend does.
    list(FILTER BOXEDWINE_CORE_SOURCES EXCLUDE REGEX "/source/ui/")
    # networkutils.cpp is libcurl's only consumer and is referenced solely by
    # source/ui (the container downloader).  Dropping it removes the libcurl
    # dependency entirely, which matters on iOS where there is no system
    # libcurl to link against.
    list(FILTER BOXEDWINE_CORE_SOURCES EXCLUDE REGEX "/source/util/networkutils\\.cpp$")
endif()

if(NOT BOXEDVN_TARGET_IOS)
    list(APPEND BOXEDWINE_CORE_SOURCES ${BOXEDWINE_PLATFORM_MACOS_ONLY})
endif()

add_library(boxedwine_core STATIC ${BOXEDWINE_CORE_SOURCES})

target_include_directories(boxedwine_core PUBLIC
    ${BOXEDWINE_INCLUDE_DIRS}
    "${BOXEDVN_SDL2_INCLUDE_DIR}"
)

if(BOXEDVN_TARGET_IOS)
    # platform/linux/platform.cpp asks BVNExecMemory for JIT memory on iOS
    # rather than calling mmap itself; see the comment there.  Only the header
    # is needed at compile time - everything ends up in one merged archive, so
    # the symbol resolves at link time without a target-level dependency (the
    # same arrangement BVNMacPlatformShim.mm already relies on).
    target_include_directories(boxedwine_core PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/ios/runtime/include"
    )

    if(NOT BOXEDVN_MOLTENVK_ROOT)
        message(FATAL_ERROR
            "BOXEDVN_MOLTENVK_ROOT is not set. Run fetch-dependencies.sh first.")
    endif()
    set(BOXEDVN_MOLTENVK_LIBRARY
        "${BOXEDVN_MOLTENVK_ROOT}/MoltenVK/static/MoltenVK.xcframework/ios-arm64/libMoltenVK.a")
    if(NOT EXISTS "${BOXEDVN_MOLTENVK_LIBRARY}")
        message(FATAL_ERROR
            "MoltenVK static iOS library not found at ${BOXEDVN_MOLTENVK_LIBRARY}")
    endif()
    message(STATUS "BoxedVN: MoltenVK    = ${BOXEDVN_MOLTENVK_LIBRARY}")
endif()

# Flags mirrored from the macOS Release configuration of the upstream Xcode
# project (see cmake/BoxedwineSources.cmake for provenance).
target_compile_definitions(boxedwine_core PUBLIC
    SIMDE_NO_NATIVE
    BOXEDWINE_64
    BOXEDWINE_ZLIB
    SDL2=1
    BOXEDWINE_POSIX
    BOXEDWINE_RECORDER
    BOXEDWINE_MAC_JIT
)

if(NOT BOXEDVN_TARGET_IOS)
    # Wine's CoreAudio driver.  On iOS the SDL audio backend is used instead;
    # see cmake/BoxedwineSources.cmake.
    target_compile_definitions(boxedwine_core PUBLIC
        BOXEDWINE_CORE_AUDIO
        __MACOSX_CORE__
    )
endif()

if(BOXEDVN_ENABLE_MIDI)
    target_compile_definitions(boxedwine_core PUBLIC BOXEDWINE_MIDI)
endif()

if(NOT BOXEDVN_ENABLE_UI)
    target_compile_definitions(boxedwine_core PUBLIC BOXEDWINE_DISABLE_UI)
endif()

if(BOXEDVN_TARGET_IOS)
    # BoxedVN supplies main() from ios/runtime so that SDL's UIKit application
    # delegate can be subclassed; suppress the one in platform/sdl.
    target_compile_definitions(boxedwine_core PUBLIC BOXEDWINE_EXTERNAL_MAIN)
    target_compile_definitions(boxedwine_core PUBLIC BOXEDWINE_IOS)
    target_compile_definitions(boxedwine_core PUBLIC BOXEDWINE_VULKAN)
elseif(BOXEDVN_BUILD_MACOS_HOST)
    # The macOS host needs the same generated Vulkan bridge as the device so
    # that DXVK reaches MoltenVK by the identical path. The bundled macOS
    # MoltenVK is universal (x86_64 + arm64), unlike the iOS package which is
    # arm64-only - which is also why the iOS Simulator cannot run this stack.
    target_compile_definitions(boxedwine_core PUBLIC BOXEDWINE_VULKAN)
endif()

target_compile_options(boxedwine_core PRIVATE
    -Wall
    -Werror=switch
    -Wno-invalid-offsetof
    -Wno-delete-incomplete
    -Wno-unused-result
    -Wno-unknown-pragmas
    -Wno-unused-local-typedefs
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-unused-but-set-variable
    -Wno-deprecated-declarations
)

target_link_libraries(boxedwine_core PUBLIC
    boxedwine_zlib
    boxedwine_softfloat
    boxedwine_asmjit
    boxedwine_pugixml
    "${BOXEDVN_SDL2_LIBRARY}"
)
if(BOXEDVN_TARGET_IOS)
    target_link_libraries(boxedwine_core PUBLIC "${BOXEDVN_MOLTENVK_LIBRARY}")
endif()

find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox REQUIRED)
find_library(COREAUDIO_FRAMEWORK CoreAudio REQUIRED)
find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
target_link_libraries(boxedwine_core PUBLIC
    ${AUDIOTOOLBOX_FRAMEWORK}
    ${COREAUDIO_FRAMEWORK}
    ${FOUNDATION_FRAMEWORK}
)

if(BOXEDVN_ENABLE_MIDI)
    find_library(COREMIDI_FRAMEWORK CoreMIDI REQUIRED)
    target_link_libraries(boxedwine_core PUBLIC ${COREMIDI_FRAMEWORK})
endif()

# ---------------------------------------------------------------------------
# boxedvn_runtime : the narrow C ABI between Swift and the emulator
# ---------------------------------------------------------------------------
if(BOXEDVN_BUILD_RUNTIME)
    add_subdirectory(ios/runtime)
endif()

# ---------------------------------------------------------------------------
# boxedvn_macos : development-only host so the guest can be debugged locally
# ---------------------------------------------------------------------------
if(BOXEDVN_BUILD_MACOS_HOST)
    if(BOXEDVN_TARGET_IOS)
        message(FATAL_ERROR "BOXEDVN_BUILD_MACOS_HOST cannot be combined with an iOS build.")
    endif()
    set(BOXEDVN_MACOS_HOST_STUB "${CMAKE_CURRENT_BINARY_DIR}/boxedvn_macos_host.cpp")
    file(WRITE "${BOXEDVN_MACOS_HOST_STUB}"
"// Generated. main() lives in platform/sdl/knativesystem.cpp; this file only\n"
"// gives the executable a translation unit so that the linker pulls the core\n"
"// archive in, with -force_load below because main() is otherwise unreferenced.\n")
    # platform/linux/platform.cpp calls three Cocoa helpers under __MACH__.
    # Upstream defines them in its Xcode project behind a Swift bridging
    # header, and the device shim is written against UIKit, so neither builds
    # here. The host gets its own minimal versions.
    add_executable(boxedvn_macos
        "${BOXEDVN_MACOS_HOST_STUB}"
        "${CMAKE_CURRENT_SOURCE_DIR}/ios/support/src/macos_host_platform.mm")
    target_link_libraries(boxedvn_macos PRIVATE
        -Wl,-force_load $<TARGET_FILE:boxedwine_core>
        boxedwine_core)
    find_library(BOXEDVN_MACOS_MOLTENVK MoltenVK
        PATHS "${CMAKE_CURRENT_SOURCE_DIR}/lib/mac/vulkan/lib" NO_DEFAULT_PATH)
    if(NOT BOXEDVN_MACOS_MOLTENVK)
        message(FATAL_ERROR
            "BOXEDVN_BUILD_MACOS_HOST needs lib/mac/vulkan/lib/libMoltenVK.dylib.")
    endif()
    target_link_libraries(boxedvn_macos PRIVATE "${BOXEDVN_MACOS_MOLTENVK}")
    # SDL2's Cocoa video/input/haptic backends. The iOS build pulls the UIKit
    # equivalents in through the app target, so these are only needed here.
    # OpenGL is needed by platform/mac/pixelformat.cpp and macOpenGL.mm, which
    # are part of the non-iOS core sources.
    foreach(framework Cocoa CoreGraphics Carbon IOKit ForceFeedback
                      GameController CoreHaptics Metal QuartzCore CoreVideo
                      OpenGL CoreMIDI AudioUnit UniformTypeIdentifiers)
        find_library(BOXEDVN_MACOS_FW_${framework} ${framework})
        if(BOXEDVN_MACOS_FW_${framework})
            target_link_libraries(boxedvn_macos PRIVATE
                "${BOXEDVN_MACOS_FW_${framework}}")
        endif()
    endforeach()
    message(STATUS "BoxedVN: macOS host enabled -> boxedvn_macos")
endif()
