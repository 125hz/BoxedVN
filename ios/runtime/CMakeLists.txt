# boxedvn_runtime: the narrow C ABI between the Apple frontend and the
# Boxedwine C++ core.  Swift only ever sees BVNRuntime.h.

if(NOT BOXEDVN_TARGET_IOS)
    # The runtime bridge is UIKit/SDL-lifecycle specific.  On macOS the core is
    # built for compile validation only.
    return()
endif()

add_library(boxedvn_runtime STATIC
    src/BVNRuntime.mm
    src/BVNLaunchArguments.cpp
    src/BVNLog.mm
    src/BVNJIT.mm
    src/BVNMemory.mm
    src/BVNExecMemory.cpp
    src/BVNRangeAllocator.cpp
    src/BVNPaths.mm
    src/BVNImport.mm
    src/BVNMacPlatformShim.mm
    src/BVNMain.mm
    src/BVNAppDelegate.mm
)

# Read BOXEDWINE_VERSION_STR straight out of the upstream header so the value
# the UI reports can never drift from the core it is linked against.
file(READ "${BOXEDWINE_ROOT}/include/boxedwine.h" _boxedwine_header)
string(REGEX MATCH "#define BOXEDWINE_VERSION_STR \"([^\"]+)\""
       _version_match "${_boxedwine_header}")
if(NOT CMAKE_MATCH_1)
    message(FATAL_ERROR
        "Could not read BOXEDWINE_VERSION_STR from include/boxedwine.h. "
        "The upstream header layout changed.")
endif()
set(BOXEDWINE_VERSION_STRING "${CMAKE_MATCH_1}")
message(STATUS "BoxedVN: Boxedwine core version = ${BOXEDWINE_VERSION_STRING}")

target_compile_definitions(boxedvn_runtime PRIVATE
    BVN_BOXEDWINE_VERSION="${BOXEDWINE_VERSION_STRING}"
    # BVNJIT.mm reports whether Boxedwine's ARM64 JIT is compiled into the
    # core this binary links against.  It cannot detect this by including
    # boxedwine.h itself - BOXEDWINE_JIT is a macro defined *inside* that
    # header (derived from BOXEDWINE_MAC_JIT + TARGET_CPU_ARM64), invisible to
    # any translation unit that does not include it, and pulling that whole
    # header into a small ObjC++ bridge file is not worth doing for one flag.
    # This target only ever builds for the iOS ARM64 device target (see the
    # early return() above), where boxedwine_core unconditionally has
    # BOXEDWINE_JIT on, so the value here is not a guess.
    BOXEDWINE_JIT
)

target_include_directories(boxedvn_runtime PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(boxedvn_runtime PUBLIC
    boxedwine_core
    boxedvn_support
)

find_library(UIKIT_FRAMEWORK UIKit REQUIRED)
find_library(AVFAUDIO_FRAMEWORK AVFoundation REQUIRED)
find_library(GAMECONTROLLER_FRAMEWORK GameController REQUIRED)
find_library(METAL_FRAMEWORK Metal REQUIRED)
find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
find_library(COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
find_library(COREMOTION_FRAMEWORK CoreMotion REQUIRED)
find_library(COREHAPTICS_FRAMEWORK CoreHaptics REQUIRED)
find_library(OPENGLES_FRAMEWORK OpenGLES REQUIRED)
find_library(IMAGEIO_FRAMEWORK ImageIO REQUIRED)
find_library(IOSURFACE_FRAMEWORK IOSurface REQUIRED)
find_library(UNIFORMTYPE_FRAMEWORK UniformTypeIdentifiers REQUIRED)
find_library(SECURITY_FRAMEWORK Security REQUIRED)

target_link_libraries(boxedvn_runtime PUBLIC
    ${UIKIT_FRAMEWORK}
    ${AVFAUDIO_FRAMEWORK}
    ${GAMECONTROLLER_FRAMEWORK}
    ${METAL_FRAMEWORK}
    ${QUARTZCORE_FRAMEWORK}
    ${COREGRAPHICS_FRAMEWORK}
    ${COREMOTION_FRAMEWORK}
    ${COREHAPTICS_FRAMEWORK}
    ${OPENGLES_FRAMEWORK}
    ${IMAGEIO_FRAMEWORK}
    ${IOSURFACE_FRAMEWORK}
    ${UNIFORMTYPE_FRAMEWORK}
    ${SECURITY_FRAMEWORK}
)

target_compile_options(boxedvn_runtime PRIVATE -Wall -Wextra -Wno-unused-parameter)

# CMake does not enable ARC on its own.  The runtime bridge is written for ARC;
# the Boxedwine core it links against is plain C++ and unaffected.
target_compile_options(boxedvn_runtime PRIVATE
    $<$<COMPILE_LANGUAGE:OBJC,OBJCXX>:-fobjc-arc>)

# ---------------------------------------------------------------------------
# Bundle every static library the app shell needs into one archive, so the
# XcodeGen project has a single link input that never needs updating when the
# CMake target graph changes.
# ---------------------------------------------------------------------------
set(BOXEDVN_MERGED_LIB "${CMAKE_BINARY_DIR}/libboxedvn.a")

add_custom_command(
    OUTPUT "${BOXEDVN_MERGED_LIB}"
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/merge-static-libs.sh"
            "${BOXEDVN_MERGED_LIB}"
            $<TARGET_FILE:boxedvn_runtime>
            $<TARGET_FILE:boxedwine_core>
            $<TARGET_FILE:boxedvn_support>
            $<TARGET_FILE:boxedwine_zlib>
            $<TARGET_FILE:boxedwine_softfloat>
            $<TARGET_FILE:boxedwine_asmjit>
            $<TARGET_FILE:boxedwine_pugixml>
            "${BOXEDVN_SDL2_LIBRARY}"
            "${BOXEDVN_MOLTENVK_LIBRARY}"
    DEPENDS boxedvn_runtime boxedwine_core boxedvn_support
            boxedwine_zlib boxedwine_softfloat boxedwine_asmjit
            boxedwine_pugixml
    COMMENT "Merging BoxedVN static libraries into libboxedvn.a"
    VERBATIM
)

add_custom_target(boxedvn_merged ALL DEPENDS "${BOXEDVN_MERGED_LIB}")
