#!/usr/bin/env bash
# BoxedVN - build FEX for iphoneos as static libraries.
#
# Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
#
# Usage:
#   scripts/build-fex64-fex.sh [--third-party-dir DIR] [--jobs N]
#                              [--configuration Release|Debug] [--force]
#
# This is ladder A: proving the translator builds and runs needs none of the
# graphics or Windows work. Nothing here depends on LLVM, llvm-mingw, FreeType,
# GnuTLS or a Wine tree.
#
# The fork already knows about iOS - its top-level CMakeLists accepts
# CMAKE_SYSTEM_NAME "iOS", and on an Apple target it disables both Linux
# allocators and skips Source/ entirely, so no FEXLoader, no thunks, no
# binfmt_misc. What is left is FEXCore and the handful of support libraries the
# application links.
#
# macOS with a full Xcode only.

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

CONFIGURATION="Release"
FORCE=0
JOBS=""

usage() {
    sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
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
        --force)
            FORCE=1; shift ;;
        -h|--help)
            usage; exit 0 ;;
        *)
            die "Unknown argument '$1'. Run with --help." ;;
    esac
done

case "${CONFIGURATION}" in
    Debug|Release) ;;
    *) die "--configuration must be Debug or Release, got '${CONFIGURATION}'." ;;
esac

require_macos
require_command cmake "Install it with 'brew install cmake'."
require_command ninja "Install it with 'brew install ninja'."
require_command xcrun "Install Xcode and run 'xcode-select --install'."

[[ -n "${JOBS}" ]] || JOBS="$(sysctl -n hw.ncpu)"

FEX64_DIR="${BOXEDVN_THIRD_PARTY}/fex64"
SOURCE="${FEX64_DIR}/fex"
BUILD="${FEX64_DIR}/fex-build-ios"
STAGING="${FEX64_DIR}/fex-ios"

if [[ ! -d "${SOURCE}/FEXCore" ]]; then
    log "FEX sources are missing; fetching"
    "${BOXEDVN_SCRIPT_DIR}/fetch-fex64-dependencies.sh" --component fex \
        || die "could not fetch FEX"
fi

[[ "${FORCE}" -eq 1 ]] && rm -rf "${BUILD}"

IPHONEOS_SDK="$(xcrun --sdk iphoneos --show-sdk-path)"

print_tool_versions
log "FEX -> ${BUILD}"
log "source at $(git -C "${SOURCE}" rev-parse HEAD 2>/dev/null || echo unknown)"

# ENABLE_LTO is on by default upstream and costs a great deal of link time for
# static libraries that are about to be linked again by Xcode; ccache is off
# because CI caches the build directory instead. BUILD_TESTING pulls in Catch2
# and the unit tests, none of which can run on a device from here.
cmake -S "${SOURCE}" -B "${BUILD}" -G Ninja \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
    -DCMAKE_OSX_SYSROOT="${IPHONEOS_SDK}" \
    -DCMAKE_BUILD_TYPE="${CONFIGURATION}" \
    -DBUILD_TESTING=OFF \
    -DBUILD_THUNKS=OFF \
    -DBUILD_FEXCONFIG=OFF \
    -DBUILD_FEX_LINUX_TESTS=OFF \
    -DENABLE_LTO=OFF \
    -DENABLE_CCACHE=OFF \
    -DENABLE_OFFLINE_TELEMETRY=OFF \
    || die "FEX: configure failed for iphoneos"

cmake --build "${BUILD}" -j "${JOBS}" || die "FEX: build failed"

# Collect whatever archives the build produced rather than asserting a list.
# The set depends on options the fork resolves itself - the Linux allocators
# switch themselves off on Apple, for one - and a hardcoded list would turn a
# correct build into a failure.
rm -rf "${STAGING}"
mkdir -p "${STAGING}/lib"
found=0
while IFS= read -r archive; do
    cp "${archive}" "${STAGING}/lib/"
    found=$((found + 1))
done < <(find "${BUILD}" -name '*.a' -type f)

[[ "${found}" -gt 0 ]] || die "FEX: the build produced no static libraries.
Something configured but built nothing; check the ninja output above."

# FEXCore is the one that actually matters: everything else is a support
# library it links against, and a build that produced only those has not built
# the translator.
[[ -f "${STAGING}/lib/libFEXCore.a" ]] || die \
"FEX: built ${found} archives but no libFEXCore.a.

That is the translator itself, so this is a configuration failure rather than
a partial build. Archives that were produced:
$(ls "${STAGING}/lib" | sed 's/^/  /')"

mkdir -p "${STAGING}/include"
cp -R "${SOURCE}/FEXCore/include/." "${STAGING}/include/"

log "Staged ${found} archives in ${STAGING}/lib"
( cd "${STAGING}/lib" && ls -lh *.a | awk '{printf "  %-32s %s\n", $9, $5}' )

# One architecture check, because a static library built for the wrong target
# links happily and fails at the very end of a much longer job.
log "Architecture check"
arch="$(lipo -archs "${STAGING}/lib/libFEXCore.a" 2>/dev/null || echo unknown)"
[[ "${arch}" == "arm64" ]] || die \
"FEX: libFEXCore.a reports architecture '${arch}', expected arm64."
ok "libFEXCore.a is arm64"
