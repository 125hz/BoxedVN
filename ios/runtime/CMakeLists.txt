# boxedvn_runtime: the narrow C ABI between the Apple frontend and the
# Boxedwine C++ core.  Swift only ever sees BVNRuntime.h.

if(NOT BOXEDVN_TARGET_IOS)
    # The runtime bridge is UIKit/SDL-lifecycle specific.  On macOS the core is
    # built for compile validation only.
    return()
endif()

add_library(boxedvn_runtime STATIC
    src/BVNRuntime.mm
    src/BVNLog.mm
    src/BVNJIT.mm
    src/BVNPaths.mm
    src/BVNMacPlatformShim.mm
    src/BVNMain.mm
    src/BVNAppDelegate.mm
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
find_library(UNIFORMTYPE_FRAMEWORK UniformTypeIdentifiers REQUIRED)

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
    ${UNIFORMTYPE_FRAMEWORK}
)

target_compile_options(boxedvn_runtime PRIVATE -Wall -Wextra -Wno-unused-parameter)

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
    DEPENDS boxedvn_runtime boxedwine_core boxedvn_support
            boxedwine_zlib boxedwine_softfloat boxedwine_asmjit
            boxedwine_pugixml
    COMMENT "Merging BoxedVN static libraries into libboxedvn.a"
    VERBATIM
)

add_custom_target(boxedvn_merged ALL DEPENDS "${BOXEDVN_MERGED_LIB}")
