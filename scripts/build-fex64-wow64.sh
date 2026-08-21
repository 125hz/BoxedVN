#!/usr/bin/env bash
# BoxedVN - build the WoW64 emulator DLL from pinned source.
#
# Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
#
# Usage:
#   scripts/build-fex64-wow64.sh [--third-party-dir DIR] [--jobs N]
#                                [--configuration Release|RelWithDebInfo]
#                                [--force]
#
# Produces libwow64fex.dll, the module Wine loads as the x86 CPU backend when
# it starts a 32-bit guest.
#
# Why this exists
# ---------------
# The fex64 stack is x86-64 by construction and older visual novels are i386.
# Wine's WoW64 runs an i386 PE side against the same 64-bit unix side this
# port already builds, and asks a CPU backend DLL to execute the guest's
# instructions through exactly the BTCpu interface libarm64ecfex.dll already
# implements for x86-64. FEX ships that backend: Source/Windows/WOW64.
#
# The difference from the ARM64EC build is one word. That one is compiled for
# arm64ec, which makes FEX's CMake set ARCHITECTURE_arm64ec and descend into
# Source/Windows/ARM64EC. This one is compiled for plain aarch64 -- host code
# with no EC mangling, because nothing here has to be callable from x86-64 --
# which sets only ARCHITECTURE_arm64 and descends into Source/Windows/WOW64
# instead. Same tree, same toolchain, same pin.
#
# The 32-bit guest's address space is the constraint that decides whether any
# of this runs, and it is not decided here: an i386 guest's pointers are 32
# bits, so a 64-bit Mach-O's default 4 GiB __PAGEZERO covers the whole of it.
# The app target is linked with -pagezero_size to leave that range free, and
# the FEX bridge reports what it actually got at startup ("low address space:"
# in the session log). Read that line before trusting anything built here.

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

CONFIGURATION="RelWithDebInfo"
FORCE=0
JOBS=""

usage() {
    sed -n '2,34p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --third-party-dir)
            [[ $# -ge 2 ]] || die "--third-party-dir needs a value"
            BOXEDVN_THIRD_PARTY="$2"; shift 2 ;;
        --jobs)
            [[ $# -ge 2 ]] || die "--jobs needs a value"
            JOBS="$2"; shift 2 ;;
        --configuration)
            [[ $# -ge 2 ]] || die "--configuration needs a value"
            CONFIGURATION="$2"; shift 2 ;;
        --force) FORCE=1; shift ;;
        --help|-h) usage; exit 0 ;;
        *) die "Unknown argument '$1'. Run with --help." ;;
    esac
done

require_command cmake
require_command ninja

[[ -n "${JOBS}" ]] || JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

source "${BOXEDVN_SCRIPT_DIR}/dependencies.fex64.lock.sh"

FEX64_DIR="${BOXEDVN_THIRD_PARTY}/fex64"
FEX_SOURCE="${FEX64_DIR}/fex"
BUILD="${FEX64_DIR}/fex-build-wow64"
OUTPUT="${FEX64_DIR}/wow64-fex"

TOOLCHAIN="${FEX64_DIR}/toolchains/llvm-mingw-${BOXEDVN_LLVM_MINGW_VERSION}-ucrt-macos-universal"
TRIPLE="aarch64-w64-mingw32"

require_file "${FEX_SOURCE}/CMakeLists.txt" \
    "Fetch the pinned FEX source first: scripts/fetch-fex64-dependencies.sh --component fex"
require_file "${FEX_SOURCE}/Source/Windows/WOW64/CMakeLists.txt" \
    "The pinned FEX tree has no WOW64 module; check the pin."
require_file "${FEX_SOURCE}/External/rpmalloc/rpmalloc/rpmalloc.c" \
    "The rpmalloc submodule is not populated. fetch-fex64-dependencies.sh places it
at the override commit because the recorded gitlink is unreachable."

[[ -x "${TOOLCHAIN}/bin/${TRIPLE}-clang" ]] || die \
"${TOOLCHAIN}/bin/${TRIPLE}-clang is missing.

Run scripts/build-fex64-toolchains.sh --stage llvm-mingw first."

export PATH="${TOOLCHAIN}/bin:${PATH}"

# The upstream link for this target is hand-rolled the same way the ARM64EC
# one was, and that is where the macOS-host build breaks. The patch adds the
# same FEX_IOS_HOST_BUILD fall-through the fork already carries next door.
# Idempotent: already-applied is fine, because the two emulator builds share
# one source tree and either may run first.
LINK_PATCH="${BOXEDVN_ROOT}/patches/fex-wow64-ios-host-link.patch"
require_file "${LINK_PATCH}"
if git -C "${FEX_SOURCE}" apply --check "${LINK_PATCH}" 2>/dev/null; then
    log "Applying the WOW64 iOS-host link rules"
    git -C "${FEX_SOURCE}" apply "${LINK_PATCH}" \
        || die "The WOW64 iOS-host link rules failed to apply."
elif git -C "${FEX_SOURCE}" apply --reverse --check "${LINK_PATCH}" 2>/dev/null; then
    log "The WOW64 iOS-host link rules are already applied"
else
    die "The pinned FEX source no longer accepts ${LINK_PATCH}; refresh it explicitly."
fi

if [[ "${FORCE}" == "1" ]]; then
    rm -rf "${BUILD}"
fi

log "WoW64 emulator DLL <- ${FEX_SOURCE}"
log "  toolchain ${TOOLCHAIN}"
log "  triple    ${TRIPLE} (plain aarch64: ARCHITECTURE_arm64 without arm64ec)"

# A separate build directory from the ARM64EC one, deliberately. The two
# differ in CMAKE_SYSTEM_PROCESSOR, which is what selects the architecture and
# therefore which of Source/Windows' two modules gets built; sharing a cache
# between them would configure one and build the other.
#
# The rpmalloc span-size and commit-mode patches are applied by the ARM64EC
# script, not by this one, because both emulator DLLs are built from one
# source tree and applying them twice is neither needed nor safe. CI runs that
# script first for exactly this reason.
#
# SPAN_SIZE is passed here all the same, and has to match the value that build
# uses. This DLL links the same FEXCore and the same allocator, and runs in
# the same address space under the same 64 GiB ceiling: rpmalloc reserves size
# plus alignment, so its stock 256 MiB span costs a 512 MiB reservation per
# request and runs the process out of reservable space. Two emulator DLLs
# disagreeing about that would be a difference that only shows up as an
# address-space exhaustion in whichever one happened to allocate second.
cmake -S "${FEX_SOURCE}" -B "${BUILD}" -G Ninja \
    -DCMAKE_BUILD_TYPE="${CONFIGURATION}" \
    -DCMAKE_TOOLCHAIN_FILE="${FEX_SOURCE}/Data/CMake/toolchain_mingw.cmake" \
    -DMINGW_TRIPLE="${TRIPLE}" \
    -DFEX_IOS_HOST_BUILD=True \
    -DCMAKE_C_FLAGS="-DFEX_IOS_HOST -DSPAN_SIZE=67108864" \
    -DCMAKE_CXX_FLAGS="-DFEX_IOS_HOST" \
    -DCMAKE_ASM_FLAGS="-DFEX_IOS_HOST" \
    -DENABLE_LTO=False \
    -DBUILD_TESTING=False \
    -DBUILD_THUNKS=False \
    -DENABLE_JEMALLOC_GLIBC_ALLOC=False \
    -DENABLE_OFFLINE_TELEMETRY=False \
    -DTUNE_CPU=none \
    || die "CMake configuration for ${TRIPLE} failed."

cmake --build "${BUILD}" -j "${JOBS}" --target wow64fex \
    || die "Building the wow64fex target failed."

DLL="$(find "${BUILD}" -name 'libwow64fex.dll' -print -quit)"
[[ -n "${DLL}" ]] || die "The build reported success but produced no libwow64fex.dll."

mkdir -p "${OUTPUT}"
cp "${DLL}" "${OUTPUT}/libwow64fex.dll"

log "WoW64 emulator DLL: ${OUTPUT}/libwow64fex.dll"
log "  size   $(wc -c < "${OUTPUT}/libwow64fex.dll") bytes"
log "  sha256 $(shasum -a 256 "${OUTPUT}/libwow64fex.dll" | cut -d' ' -f1)"

# Same reason as the ARM64EC map: a device fault in here reports an offset and
# nothing else, and without a map each one costs a build round trip to guess
# at. Addresses are relative to the preferred image base, not the JIT-pool
# copy the loader reports.
if nm -C --defined-only --numeric-sort "${DLL}" > "${OUTPUT}/libwow64fex.symbols" 2>/dev/null &&
   [[ -s "${OUTPUT}/libwow64fex.symbols" ]]; then
    log "  symbols $(wc -l < "${OUTPUT}/libwow64fex.symbols") entries -> libwow64fex.symbols"
else
    rm -f "${OUTPUT}/libwow64fex.symbols"
    warn "could not emit a symbol map; device fault offsets will not resolve"
fi

ok "wow64fex built from source"
