#!/usr/bin/env bash
# BoxedVN - build the cross toolchains the fex64 stack needs.
#
# Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
#
# Usage:
#   scripts/build-fex64-toolchains.sh [--stage llvm-mingw|llvm-ios|freetype|all]
#                                     [--third-party-dir DIR]
#                                     [--jobs N]
#                                     [--force]
#
# These are the pieces that are slow, are needed by everything downstream, and
# change only when scripts/dependencies.fex64.lock.sh changes - which is what
# makes them worth caching in CI as a unit.
#
#   llvm-mingw  prebuilt x86_64-w64-mingw32 toolchain; builds DXMT's PE side.
#   llvm-ios    LLVM cross-built for iphoneos; DXMT's airconv translates DXBC
#               to Metal AIR through it. Two-stage, and the longest item in
#               the whole build.
#   freetype    static, for Wine's win32u unix side.
#
# Everything lands in $BOXEDVN_THIRD_PARTY/fex64/toolchains. Each stage writes
# a .stamp on success and is skipped when the stamp is present; --force
# rebuilds.
#
# macOS with a full Xcode only.

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
source "${BOXEDVN_SCRIPT_DIR}/dependencies.fex64.lock.sh"

STAGES="all"
FORCE=0
JOBS=""

usage() {
    sed -n '2,28p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --stage)
            [[ $# -ge 2 ]] || die "--stage needs a value"
            STAGES="$2"; shift 2 ;;
        --third-party-dir)
            [[ $# -ge 2 ]] || die "--third-party-dir needs a value"
            BOXEDVN_THIRD_PARTY="$2"; shift 2 ;;
        --jobs)
            [[ $# -ge 2 ]] || die "--jobs needs a value"
            JOBS="$2"; shift 2 ;;
        --force)
            FORCE=1; shift ;;
        -h|--help)
            usage; exit 0 ;;
        *)
            die "Unknown argument '$1'. Run with --help." ;;
    esac
done

case "${STAGES}" in
    all) STAGES="llvm-mingw freetype llvm-ios" ;;
    llvm-mingw|llvm-ios|freetype) ;;
    *) die "--stage must be llvm-mingw, llvm-ios, freetype or all, got '${STAGES}'." ;;
esac

require_macos
require_command cmake "Install it with 'brew install cmake'."
require_command ninja "Install it with 'brew install ninja'."
require_command git
require_command curl
require_command tar
require_command xcrun "Install Xcode and run 'xcode-select --install'."

[[ -n "${JOBS}" ]] || JOBS="$(sysctl -n hw.ncpu)"

FEX64_DIR="${BOXEDVN_THIRD_PARTY}/fex64"
TOOLCHAINS="${FEX64_DIR}/toolchains"
STAMPS="${TOOLCHAINS}/.stamps"
mkdir -p "${STAMPS}"

IPHONEOS_SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
IPHONEOS_MIN="17.0"

stage_done() {
    [[ "${FORCE}" -eq 0 && -f "${STAMPS}/$1.stamp" ]]
}

stage_stamp() {
    date -u +%Y-%m-%dT%H:%M:%SZ > "${STAMPS}/$1.stamp"
}

# --- llvm-mingw -------------------------------------------------------------

build_llvm_mingw() {
    local name="llvm-mingw-${BOXEDVN_LLVM_MINGW_VERSION}-ucrt-macos-universal"
    local destination="${TOOLCHAINS}/${name}"

    if stage_done llvm-mingw && [[ -x "${destination}/bin/aarch64-w64-mingw32-clang" ]]; then
        ok "llvm-mingw: cached"
        return 0
    fi

    log "llvm-mingw: downloading ${BOXEDVN_LLVM_MINGW_VERSION}"
    rm -rf "${destination}"
    mkdir -p "${TOOLCHAINS}"
    curl --fail --location --show-error --silent --retry 3 --retry-delay 2 \
        "${BOXEDVN_LLVM_MINGW_URL}" \
        | tar -xJ -C "${TOOLCHAINS}" \
        || die "llvm-mingw: download or extract failed"

    [[ -x "${destination}/bin/aarch64-w64-mingw32-clang" ]] \
        || die "llvm-mingw: extracted, but ${destination}/bin/aarch64-w64-mingw32-clang is missing.
The release layout changed; the pin needs revisiting."

    stage_stamp llvm-mingw
    ok "llvm-mingw: ${destination}"
}

# --- freetype ---------------------------------------------------------------

build_freetype() {
    local source="${FEX64_DIR}/freetype"
    local build="${TOOLCHAINS}/freetype-build"
    local prefix="${TOOLCHAINS}/freetype-ios"

    if stage_done freetype && [[ -f "${prefix}/lib/libfreetype.a" ]]; then
        ok "freetype: cached"
        return 0
    fi

    if [[ ! -d "${source}/.git" ]]; then
        log "freetype: cloning ${BOXEDVN_FREETYPE_TAG}"
        git clone --depth 1 --branch "${BOXEDVN_FREETYPE_TAG}" \
            "${BOXEDVN_FREETYPE_REPOSITORY}" "${source}" \
            || die "freetype: clone failed"
    fi

    # Every optional dependency is off: the fonts this consumes are the plain
    # TrueType files Wine ships, and each extra dependency is another library
    # to cross-build for iphoneos for no gain.
    log "freetype: configuring"
    rm -rf "${build}"
    cmake -S "${source}" -B "${build}" -G Ninja \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="${IPHONEOS_MIN}" \
        -DCMAKE_OSX_SYSROOT="${IPHONEOS_SDK}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${prefix}" \
        -DBUILD_SHARED_LIBS=OFF \
        -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON \
        -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON \
        -DCMAKE_C_FLAGS="-fno-stack-protector" \
        || die "freetype: configure failed"

    log "freetype: building"
    cmake --build "${build}" -j "${JOBS}" || die "freetype: build failed"
    cmake --install "${build}" || die "freetype: install failed"

    [[ -f "${prefix}/lib/libfreetype.a" ]] \
        || die "freetype: built, but ${prefix}/lib/libfreetype.a is missing"

    stage_stamp freetype
    ok "freetype: ${prefix}/lib/libfreetype.a"
}

# --- LLVM for iphoneos ------------------------------------------------------

build_llvm_ios() {
    local source="${FEX64_DIR}/llvm-project"
    local host_build="${TOOLCHAINS}/llvm-host-build"
    local ios_build="${TOOLCHAINS}/llvm-ios-build"

    if stage_done llvm-ios && [[ -f "${ios_build}/lib/libLLVMCore.a" ]]; then
        ok "llvm-ios: cached"
        return 0
    fi

    if [[ ! -d "${source}/.git" ]]; then
        log "llvm: cloning ${BOXEDVN_LLVM_TAG} (large)"
        git clone --depth 1 --branch "${BOXEDVN_LLVM_TAG}" \
            "${BOXEDVN_LLVM_REPOSITORY}" "${source}" \
            || die "llvm: clone failed"
    fi

    # Apple's linker has no --gc-sections; it spells that -dead_strip. LLVM
    # decides between them from CMAKE_SYSTEM_NAME, and its test only knows
    # "Darwin", so an iOS build hands Apple ld a GNU flag and fails to link.
    #
    # Both the system-name test and the flag itself are rewritten. Patching
    # only the test left a second site that emits the flag unconditionally,
    # which is what stopped the first attempt at linking llvm-tblgen.
    local add_llvm="${source}/llvm/cmake/modules/AddLLVM.cmake"
    require_file "${add_llvm}" "The LLVM checkout is incomplete."
    /usr/bin/sed -i ''         -e 's/CMAKE_SYSTEM_NAME MATCHES "Darwin"/CMAKE_SYSTEM_NAME MATCHES "Darwin|iOS"/g'         -e 's/-Wl,--gc-sections/-Wl,-dead_strip/g'         "${add_llvm}" || die "llvm: could not patch AddLLVM.cmake"

    if grep -q -- '--gc-sections' "${add_llvm}"; then
        die "llvm: AddLLVM.cmake still emits --gc-sections after patching.
Apple's linker rejects it outright, so the build would fail at its first link."
    fi
    ok "llvm: AddLLVM.cmake uses -dead_strip"

    # LLVMHello is a demo pass plugin - MODULE, BUILDTREE_ONLY, wanted by
    # nothing here - and it is the only loadable module this configuration
    # builds, since tools, utils and examples are all off. It is also the one
    # target that fails.
    #
    # Its CMakeLists sets LLVM_EXPORTED_SYMBOL_FILE, and
    # add_llvm_symbol_exports picks a format per platform with
    # `if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")`. That is the same class of
    # bug as --gc-sections above, and it slips past the same fix: the sed
    # pattern has no brace, so the dereferenced spelling never matches, iOS
    # falls through to the final else(), and LLVM generates a Windows .def and
    # hands it to Apple's ld, which says "unknown file type in LLVMHello.def"
    # at target 1656 of 1674 - after every library airconv actually needs has
    # already linked.
    #
    # Drop the subdirectory instead of teaching that test about iOS. Widening
    # the platform test changes every Darwin-gated branch in AddLLVM.cmake to
    # make a plugin we then throw away link correctly; removing one
    # unconditional add_subdirectory cannot affect a library we keep.
    local transforms="${source}/llvm/lib/Transforms/CMakeLists.txt"
    require_file "${transforms}" "The LLVM checkout is incomplete."
    /usr/bin/sed -i '' -e '/^add_subdirectory(Hello)$/d' "${transforms}" \
        || die "llvm: could not drop the Hello plugin"

    if grep -q 'add_subdirectory(Hello)' "${transforms}"; then
        die "llvm: lib/Transforms still builds the Hello plugin.
It is the only loadable module in this configuration and it cannot link for
iOS, so the build would fail after every library we need has been built."
    fi
    ok "llvm: Hello plugin excluded"

    # Stage 1. llvm-tblgen has to run on the build machine, so it is built for
    # macOS and handed to the iOS configure. Nothing else from this tree is
    # used.
    if [[ ! -x "${host_build}/bin/llvm-tblgen" ]]; then
        log "llvm: stage 1, host llvm-tblgen"
        cmake -S "${source}/llvm" -B "${host_build}" -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DLLVM_TARGETS_TO_BUILD="" \
            -DLLVM_INCLUDE_TESTS=OFF \
            -DLLVM_INCLUDE_BENCHMARKS=OFF \
            -DLLVM_INCLUDE_EXAMPLES=OFF \
            || die "llvm: host configure failed"
        cmake --build "${host_build}" -j "${JOBS}" --target llvm-tblgen \
            || die "llvm: host llvm-tblgen build failed"
    else
        ok "llvm: stage 1 cached"
    fi

    # Stage 2. airconv needs LLVM's IR and bitcode libraries, not a compiler
    # for any particular machine, so no backend targets are built. That is
    # most of LLVM's build time avoided.
    log "llvm: stage 2, iphoneos libraries"
    cmake -S "${source}/llvm" -B "${ios_build}" -G Ninja \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="${IPHONEOS_MIN}" \
        -DCMAKE_OSX_SYSROOT="${IPHONEOS_SDK}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DLLVM_TABLEGEN="${host_build}/bin/llvm-tblgen" \
        -DLLVM_TARGETS_TO_BUILD="" \
        -DLLVM_BUILD_TOOLS=OFF \
        -DLLVM_BUILD_UTILS=OFF \
        -DLLVM_INCLUDE_TOOLS=OFF \
        -DLLVM_INCLUDE_UTILS=OFF \
        -DLLVM_INCLUDE_TESTS=OFF \
        -DLLVM_INCLUDE_BENCHMARKS=OFF \
        -DLLVM_INCLUDE_EXAMPLES=OFF \
        -DLLVM_ENABLE_ZLIB=OFF \
        -DLLVM_ENABLE_ZSTD=OFF \
        -DLLVM_ENABLE_LIBXML2=OFF \
        -DLLVM_ENABLE_TERMINFO=OFF \
        -DLLVM_ENABLE_PIC=ON \
        || die "llvm: iphoneos configure failed"

    log "llvm: stage 2 building with ${JOBS} jobs (this is the long one)"
    cmake --build "${ios_build}" -j "${JOBS}" || die "llvm: iphoneos build failed"

    [[ -f "${ios_build}/lib/libLLVMCore.a" ]] \
        || die "llvm: built, but ${ios_build}/lib/libLLVMCore.a is missing"

    stage_stamp llvm-ios
    ok "llvm-ios: ${ios_build}"
}

log "fex64 toolchains -> ${TOOLCHAINS}"
print_tool_versions

for stage in ${STAGES}; do
    case "${stage}" in
        llvm-mingw) build_llvm_mingw ;;
        freetype)   build_freetype ;;
        llvm-ios)   build_llvm_ios ;;
    esac
done

log "Toolchain contents"
du -sh "${TOOLCHAINS}"/* 2>/dev/null | sed 's/^/  /' || true
