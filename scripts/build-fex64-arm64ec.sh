#!/usr/bin/env bash
# BoxedVN - build the ARM64EC emulator DLL from pinned source.
#
# Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
#
# Usage:
#   scripts/build-fex64-arm64ec.sh [--third-party-dir DIR] [--jobs N]
#                                  [--configuration Release|RelWithDebInfo]
#                                  [--force]
#
# Produces libarm64ecfex.dll, the module the runtime loads as xtajit64.dll.
#
# Why this exists
# ---------------
# The integration ships that DLL prebuilt, and the allocator revision it was
# built from was force-pushed out of its repository, so the allocator could only
# be studied through disassembly and repaired by patching instructions. That
# bought one fix per device cycle against a component that corrupts its own
# metadata in several different ways. Building it from source turns the same
# work into reading C.
#
# The pinned fork already carries an iOS-host path: Source/Windows/ARM64EC has
# IosJitAlias.cpp and its CMakeLists selects different link rules under
# FEX_IOS_HOST_BUILD, because the upstream -nostdlib link fails on a macOS host.
# So this uses the toolchain the rest of the fex64 stack already downloads
# rather than introducing a second one.
#
# Not wired into the app build by default. build-fex64-app.sh keeps using the
# prebuilt until a source-built DLL has been proven on device.

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

CONFIGURATION="RelWithDebInfo"
FORCE=0
JOBS=""

usage() {
    sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
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
BUILD="${FEX64_DIR}/fex-build-arm64ec"
OUTPUT="${FEX64_DIR}/arm64ec-fex"

TOOLCHAIN="${FEX64_DIR}/toolchains/llvm-mingw-${BOXEDVN_LLVM_MINGW_VERSION}-ucrt-macos-universal"
TRIPLE="arm64ec-w64-mingw32"

require_file "${FEX_SOURCE}/CMakeLists.txt" \
    "Fetch the pinned FEX source first: scripts/fetch-fex64-dependencies.sh --component fex"
require_file "${FEX_SOURCE}/Source/Windows/ARM64EC/CMakeLists.txt" \
    "The pinned FEX tree has no ARM64EC module; check the pin."
require_file "${FEX_SOURCE}/External/rpmalloc/rpmalloc/rpmalloc.c" \
    "The rpmalloc submodule is not populated. fetch-fex64-dependencies.sh places it
at the override commit because the recorded gitlink is unreachable."

[[ -x "${TOOLCHAIN}/bin/${TRIPLE}-clang" ]] || die \
"${TOOLCHAIN}/bin/${TRIPLE}-clang is missing.

Run scripts/build-fex64-toolchains.sh --stage llvm-mingw first. If that ran and
the binary still is not there, this llvm-mingw release has no ARM64EC target and
the pin in scripts/dependencies.fex64.lock.sh needs raising deliberately."

export PATH="${TOOLCHAIN}/bin:${PATH}"

# The fork calls one diagnostic hook that only existed in the allocator
# revision it records, which is unreachable. Supply a stub that reports "no
# snapshot" so the reporter skips it. Idempotent: already-applied is fine.
STUB_PATCH="${BOXEDVN_ROOT}/patches/fex-arm64ec-rpmalloc-diagnostic-stub.patch"
require_file "${STUB_PATCH}"
if git -C "${FEX_SOURCE}" apply --unidiff-zero --check "${STUB_PATCH}" 2>/dev/null; then
    log "Applying the allocator diagnostic stub"
    git -C "${FEX_SOURCE}" apply --unidiff-zero "${STUB_PATCH}" \
        || die "The allocator diagnostic stub failed to apply."
elif git -C "${FEX_SOURCE}" apply --unidiff-zero --reverse --check "${STUB_PATCH}" 2>/dev/null; then
    log "The allocator diagnostic stub is already applied"
else
    die "The pinned FEX source no longer accepts ${STUB_PATCH}; refresh it explicitly."
fi

# Name the caller of the oversized reserves. Two 512MB reserve-only requests
# arrive per thread, one percent of each is ever committed, and on iOS they run
# the process out of reservable address space at about 6GB - after which the
# NULL is stored through and the process wedges. The wine side cannot see past
# its own allocator frames, so the probe goes here, where the symbols are ours
# and the build already publishes a map to resolve them against.
#
# Carries context rather than zero context lines, so it survives the stub patch
# above landing in a different file. Idempotent the same way.
PROBE_PATCH="${BOXEDVN_ROOT}/patches/fex-bigres-caller-probe.patch"
require_file "${PROBE_PATCH}"
if git -C "${FEX_SOURCE}" apply --check "${PROBE_PATCH}" 2>/dev/null; then
    log "Applying the oversized-reserve caller probe"
    git -C "${FEX_SOURCE}" apply "${PROBE_PATCH}"         || die "The oversized-reserve caller probe failed to apply."
elif git -C "${FEX_SOURCE}" apply --reverse --check "${PROBE_PATCH}" 2>/dev/null; then
    log "The oversized-reserve caller probe is already applied"
else
    die "The pinned FEX source no longer accepts ${PROBE_PATCH}; refresh it explicitly."
fi

# rpmalloc reserves size + alignment, so its 256MB span costs a 512MB
# reservation every time. Two arrive per thread, one percent of each is ever
# committed, and on iOS the process runs out of reservable address space at
# about 6GB - after which the allocation fails and the NULL is stored through.
# Make the constant overridable, then use the smallest span compatible with
# this pinned allocator's 64MB large-page class.
#
# Applied to the submodule, not to FEX: rpmalloc is its own repository, so
# git -C the FEX tree cannot reach inside it.
SPAN_PATCH="${BOXEDVN_ROOT}/patches/rpmalloc-span-size-override.patch"
COMMIT_MODE_PATCH="${BOXEDVN_ROOT}/patches/rpmalloc-runtime-commit-mode.patch"
RPMALLOC_SOURCE="${FEX_SOURCE}/External/rpmalloc"
require_file "${SPAN_PATCH}"
if git -C "${RPMALLOC_SOURCE}" apply --check "${SPAN_PATCH}" 2>/dev/null; then
    log "Applying the rpmalloc span-size override"
    git -C "${RPMALLOC_SOURCE}" apply "${SPAN_PATCH}" || die "The rpmalloc span-size override failed to apply."
elif git -C "${RPMALLOC_SOURCE}" apply --reverse --check "${SPAN_PATCH}" 2>/dev/null; then
    log "The rpmalloc span-size override is already applied"
else
    die "The pinned rpmalloc no longer accepts ${SPAN_PATCH}; refresh it explicitly."
fi

# With decommit compiled in, rpmalloc reserves spans first and commits their
# first page separately. Its runtime disable_decommit switch used to suppress
# that separate commit without changing the reserve-only mapping, leaving the
# span header inaccessible. Make the initial mapping match the runtime mode.
require_file "${COMMIT_MODE_PATCH}"
if git -C "${RPMALLOC_SOURCE}" apply --check "${COMMIT_MODE_PATCH}" 2>/dev/null; then
    log "Applying the rpmalloc runtime commit-mode fix"
    git -C "${RPMALLOC_SOURCE}" apply "${COMMIT_MODE_PATCH}" || die "The rpmalloc runtime commit-mode fix failed to apply."
elif git -C "${RPMALLOC_SOURCE}" apply --reverse --check "${COMMIT_MODE_PATCH}" 2>/dev/null; then
    log "The rpmalloc runtime commit-mode fix is already applied"
else
    die "The pinned rpmalloc no longer accepts ${COMMIT_MODE_PATCH}; refresh it explicitly."
fi

if [[ "${FORCE}" == "1" ]]; then
    rm -rf "${BUILD}"
fi

log "ARM64EC emulator DLL <- ${FEX_SOURCE}"
log "  toolchain ${TOOLCHAIN}"
log "  rpmalloc  $(git -C "${FEX_SOURCE}/External/rpmalloc" rev-parse HEAD 2>/dev/null || echo unknown)"

# ENABLE_LTO off because the upstream mingw CI does the same and it keeps link
# times sane. BUILD_TESTING off: nothing here runs on the host. TUNE_CPU none
# so the build does not tune for the macOS host it is cross-compiling on.
# FEX_IOS_HOST_BUILD selects the link rules the fork added for this host.
#
# FEX_IOS_HOST is a separate thing: a compile define no CMake file in the fork
# ever sets, yet 24 sources depend on it, and in Core.cpp it guards the
# declarations of iOS log buffers whose uses a hundred lines later are not
# guarded at all. The prebuilt plainly had it, since the shipped DLL carries
# all of that iOS behaviour, so the author passed it in the environment. Pass
# it explicitly rather than depending on an undocumented local setting.
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

cmake --build "${BUILD}" -j "${JOBS}" --target arm64ecfex \
    || die "Building the arm64ecfex target failed."

DLL="$(find "${BUILD}" -name 'libarm64ecfex.dll' -print -quit)"
[[ -n "${DLL}" ]] || die "The build reported success but produced no libarm64ecfex.dll."

mkdir -p "${OUTPUT}"
cp "${DLL}" "${OUTPUT}/libarm64ecfex.dll"

log "ARM64EC emulator DLL: ${OUTPUT}/libarm64ecfex.dll"
log "  size   $(wc -c < "${OUTPUT}/libarm64ecfex.dll") bytes"
log "  sha256 $(shasum -a 256 "${OUTPUT}/libarm64ecfex.dll" | cut -d' ' -f1)"

# Emit a sorted symbol map beside the DLL.
#
# Device faults report an offset into this module and nothing else, and every
# one of them costs a build round trip to guess at. The wedge this was added
# for is a `blr x11` at libarm64ecfex+0x198774 that branched into an x86-64
# import thunk instead of routing the call through the emulator; naming that
# call site is the difference between fixing it in the source we now build and
# working around it in the fault handler.
#
# Addresses here are file/preferred-image relative, so subtract the module base
# reported by the loader, not the JIT-pool copy address.
if nm -C --defined-only --numeric-sort "${DLL}" > "${OUTPUT}/libarm64ecfex.symbols" 2>/dev/null &&
   [[ -s "${OUTPUT}/libarm64ecfex.symbols" ]]; then
    log "  symbols $(wc -l < "${OUTPUT}/libarm64ecfex.symbols") entries -> libarm64ecfex.symbols"
else
    # Not fatal: the DLL is what the build exists to produce, and a missing map
    # only costs the next fault an extra round trip.
    rm -f "${OUTPUT}/libarm64ecfex.symbols"
    warn "could not emit a symbol map; device fault offsets will not resolve"
fi

ok "arm64ecfex built from source"
