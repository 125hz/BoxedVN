# BoxedVN - iOS/iPadOS port of Boxedwine
# Copyright (C) 2012-2026  The BoxedWine Team
#
# This file is part of a GPLv2 project.  See license.txt at the repository root.
#
# ---------------------------------------------------------------------------
# Source list for the Boxedwine emulator core.
#
# This list is derived from the two authoritative upstream build definitions at
# commit 379bf2414a67fc6509d506a6eefdf6ffa7ebf82d:
#
#   * project/linux/makefile
#       - glob of lib/asmjit/asmjit/**.cpp
#       - glob of source/**.cpp
#       - platform/linux/*.cpp, platform/sdl/*.cpp
#       - lib/pugixml/src/*.cpp, lib/glew/src/glew.cpp, imgui, minizip
#       - lib/softfloat/source/*.c and .../8086-SSE/*.c
#
#   * project/mac-xcode/Boxedwine/Boxedwine.xcodeproj
#       - `source`, `include` and `asmjit` are PBXFileSystemSynchronizedRootGroups
#         with NO exception sets, i.e. every .cpp underneath them is compiled.
#       - explicit files add platform/mac/*.cpp + macOpenGL.mm and zlib built
#         from lib/zlib rather than the system libz.
#
# The macOS target is the closest relative of the iOS target, so this file
# follows the macOS file set and diverges only where an API does not exist on
# iOS (documented inline).
# ---------------------------------------------------------------------------

set(BOXEDWINE_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

# --- Emulator core: everything under source/, minus the unit-test harness -----
# source/test/** is only compiled into the __TEST target upstream and defines a
# competing main(); it is excluded here and built separately if ever needed.
file(GLOB_RECURSE BOXEDWINE_SOURCE_CPP CONFIGURE_DEPENDS
    "${BOXEDWINE_ROOT}/source/*.cpp"
)
list(FILTER BOXEDWINE_SOURCE_CPP EXCLUDE REGEX "/source/test/")
# source/sdl/emscripten is WASM-only (emscripten.h); never built for Apple.
list(FILTER BOXEDWINE_SOURCE_CPP EXCLUDE REGEX "/source/sdl/emscripten/")
# The WASM JIT backend is Emscripten-only.
list(FILTER BOXEDWINE_SOURCE_CPP EXCLUDE REGEX "/source/emulation/cpu/wasm/")

# --- asmjit (the ARM64 / x64 assembler used by the JIT) ----------------------
file(GLOB_RECURSE BOXEDWINE_ASMJIT_CPP CONFIGURE_DEPENDS
    "${BOXEDWINE_ROOT}/lib/asmjit/asmjit/*.cpp"
)

# --- Platform layer ----------------------------------------------------------
# platform/linux is misnamed upstream: it is the POSIX layer and is compiled by
# the macOS target too (it is full of __MACH__ conditionals).
set(BOXEDWINE_PLATFORM_POSIX
    "${BOXEDWINE_ROOT}/platform/linux/platform.cpp"
    "${BOXEDWINE_ROOT}/platform/linux/platformOpenGL.cpp"
    "${BOXEDWINE_ROOT}/platform/linux/platformThreads.cpp"
    "${BOXEDWINE_ROOT}/platform/linux/platformThreads-armv8.cpp"
    "${BOXEDWINE_ROOT}/platform/linux/platformThreads-x64.cpp"
)

file(GLOB BOXEDWINE_PLATFORM_SDL CONFIGURE_DEPENDS
    "${BOXEDWINE_ROOT}/platform/sdl/*.cpp"
)

# Apple-specific platform files.  audiounit.cpp / knativecoreaudio.cpp use
# AudioToolbox+CoreAudio and coremidi.cpp uses CoreMIDI -- all three frameworks
# exist on iOS.  macOpenGL.mm is NSOpenGL/AppKit and is macOS-only; it is added
# by the caller, not here.
set(BOXEDWINE_PLATFORM_APPLE_COMMON
    "${BOXEDWINE_ROOT}/platform/mac/audiounit.cpp"
    "${BOXEDWINE_ROOT}/platform/mac/knativecoreaudio.cpp"
    "${BOXEDWINE_ROOT}/platform/mac/pixelformat.cpp"
)

set(BOXEDWINE_PLATFORM_APPLE_MIDI
    "${BOXEDWINE_ROOT}/platform/mac/coremidi.cpp"
)

set(BOXEDWINE_PLATFORM_MACOS_ONLY
    "${BOXEDWINE_ROOT}/platform/mac/macOpenGL.mm"
)

# --- Third-party sources compiled directly into the core ---------------------
file(GLOB BOXEDWINE_PUGIXML CONFIGURE_DEPENDS "${BOXEDWINE_ROOT}/lib/pugixml/src/*.cpp")

set(BOXEDWINE_ZLIB
    "${BOXEDWINE_ROOT}/lib/zlib/adler32.c"
    "${BOXEDWINE_ROOT}/lib/zlib/compress.c"
    "${BOXEDWINE_ROOT}/lib/zlib/crc32.c"
    "${BOXEDWINE_ROOT}/lib/zlib/deflate.c"
    "${BOXEDWINE_ROOT}/lib/zlib/gzclose.c"
    "${BOXEDWINE_ROOT}/lib/zlib/gzlib.c"
    "${BOXEDWINE_ROOT}/lib/zlib/gzread.c"
    "${BOXEDWINE_ROOT}/lib/zlib/gzwrite.c"
    "${BOXEDWINE_ROOT}/lib/zlib/infback.c"
    "${BOXEDWINE_ROOT}/lib/zlib/inffast.c"
    "${BOXEDWINE_ROOT}/lib/zlib/inflate.c"
    "${BOXEDWINE_ROOT}/lib/zlib/inftrees.c"
    "${BOXEDWINE_ROOT}/lib/zlib/trees.c"
    "${BOXEDWINE_ROOT}/lib/zlib/uncompr.c"
    "${BOXEDWINE_ROOT}/lib/zlib/zutil.c"
)

set(BOXEDWINE_MINIZIP
    "${BOXEDWINE_ROOT}/lib/zlib/contrib/minizip/ioapi.c"
    "${BOXEDWINE_ROOT}/lib/zlib/contrib/minizip/mztools.c"
    "${BOXEDWINE_ROOT}/lib/zlib/contrib/minizip/unzip.c"
    "${BOXEDWINE_ROOT}/lib/zlib/contrib/minizip/zip.c"
)

# softfloat supplies the 80-bit x87 emulation.
file(GLOB BOXEDWINE_SOFTFLOAT CONFIGURE_DEPENDS
    "${BOXEDWINE_ROOT}/lib/softfloat/source/*.c"
    "${BOXEDWINE_ROOT}/lib/softfloat/source/8086-SSE/*.c"
)

set(BOXEDWINE_GLEW "${BOXEDWINE_ROOT}/lib/glew/src/glew.cpp")

set(BOXEDWINE_IMGUI
    "${BOXEDWINE_ROOT}/lib/imgui/imgui.cpp"
    "${BOXEDWINE_ROOT}/lib/imgui/imgui_draw.cpp"
    "${BOXEDWINE_ROOT}/lib/imgui/imgui_widgets.cpp"
    "${BOXEDWINE_ROOT}/lib/imgui/examples/imgui_impl_sdl.cpp"
    "${BOXEDWINE_ROOT}/lib/imgui/examples/imgui_impl_opengl3.cpp"
    "${BOXEDWINE_ROOT}/lib/imgui/addon/imguitinyfiledialogs.cpp"
)

# --- Include directories shared by every Boxedwine translation unit ----------
set(BOXEDWINE_INCLUDE_DIRS
    "${BOXEDWINE_ROOT}/include"
    "${BOXEDWINE_ROOT}/lib/asmjit"
    "${BOXEDWINE_ROOT}/lib/simde"
    "${BOXEDWINE_ROOT}/lib/pugixml/src"
    "${BOXEDWINE_ROOT}/lib/zlib"
    "${BOXEDWINE_ROOT}/lib/zlib/contrib/minizip"
    "${BOXEDWINE_ROOT}/lib/glew/include"
    "${BOXEDWINE_ROOT}/lib/imgui"
    "${BOXEDWINE_ROOT}/source"
    "${BOXEDWINE_ROOT}/platform/sdl"
    "${BOXEDWINE_ROOT}/platform/mac"
)

set(BOXEDWINE_SOFTFLOAT_INCLUDE_DIRS
    "${BOXEDWINE_ROOT}/lib/softfloat/source/include"
    "${BOXEDWINE_ROOT}/lib/softfloat/source"
    "${BOXEDWINE_ROOT}/lib/softfloat/source/8086-SSE"
)
