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

# FEX_IOS_HOST is the ARM64EC module's define, not this build's. Setting it
# here compiles first: the iOS diagnostic buffers become declared. Then it
# fails, because the same define also enables VirtualQuery and
# MEMORY_BASIC_INFORMATION in Arm64.cpp, which exist only in a Windows PE
# build. So the host translator library is built without it, and the two
# diagnostic reporters that reference those buffers outside their guard are
# guarded by scripts/fex64-patches/fex-ios-host-diagnostics-guard.patch.
#
# That mismatch means the plain iOS host build is broken at the pinned commit -
# the reference port builds the ARM64EC target from it, and its own host
# archives predate these lines. Reported upstream rather than carried forever.
apply_patch() {
    local patch="${BOXEDVN_ROOT}/scripts/fex64-patches/$1"
    require_file "${patch}"
    if git -C "${SOURCE}" apply --reverse --check "${patch}" 2>/dev/null; then
        ok "patch already applied: $1"
        return 0
    fi
    git -C "${SOURCE}" apply --check "${patch}" || die "patch $1 no longer applies to FEX at $(git -C "${SOURCE}" rev-parse --short HEAD).

The pin moved or upstream fixed this. Re-cut the patch deliberately."
    git -C "${SOURCE}" apply "${patch}" || die "could not apply $1"
    ok "applied patch: $1"
}

apply_patch fex-ios-host-diagnostics-guard.patch
apply_patch fex-ios-caspal-diagnostic-host.patch
apply_patch fex-boxedwine-block-diagnostics.patch
apply_patch fex-apple-dual-map-cache-publish.patch
apply_patch fex-arm64-pair-immediate-mask.patch
apply_patch fex-arm64-context-indexed-unaligned-offset.patch
apply_patch fex-arm64-addsub-immediate-range.patch
apply_patch fex-boxedwine-ir-capture-arm.patch
apply_patch fex-boxedwine-low-address-alias.patch
# Depends on the alias patch above: its witness reports the host address a
# canonical guest stack slot resolves to, which only exists once the alias is
# published. Restores the missing null-host-target guard in the emitted L1
# lookup, so an indirect exit can never branch to host address zero.
apply_patch fex-boxedwine-null-exit-target.patch
# Depends on the null-exit patch above: it extends the same witness slot with a
# CALL history ring and an immediate read-back of every pushed return address,
# which is what separates a push that never landed from a slot zeroed later.
apply_patch fex-boxedwine-call-return-witness.patch
# Depends on the alias patch above: it asks the context whether the guest is
# hosted at an aliased address before deciding that a descriptor-table read
# would be translated as if it were guest memory.
apply_patch fex-boxedwine-longmode-segment-base.patch
# Test-only: an env-gated alias configuration for TestHarnessRunner. It
# changes no library code, and is applied here too so every checkout of
# the pin carries the same maintained set.
apply_patch fex-boxedwine-harness-alias.patch

# CMAKE_SYSTEM_PROCESSOR has to be stated. CMake leaves it empty when cross-
# compiling unless a toolchain file sets it, and FEX selects its entire
# architecture backend from that one string - so empty is not a missing
# optimisation, it is "Unsupported processor type" and no build at all.
#
# TUNE_CPU defaults to "native", which on this path runs a Python script over
# /proc/cpuinfo to pick an -mcpu. There is no /proc on macOS, and the build
# machine is not the target anyway: this runs on whatever iPhone installs it,
# and FEX detects host features at runtime regardless.
#
# ENABLE_LTO is on by default upstream and costs a great deal of link time for
# archives that Xcode is about to link again; ccache is off because CI caches
# the build directory instead; BUILD_TESTING pulls in Catch2 and unit tests
# that cannot run from here.
cmake -S "${SOURCE}" -B "${BUILD}" -G Ninja \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
    -DCMAKE_OSX_SYSROOT="${IPHONEOS_SDK}" \
    -DCMAKE_BUILD_TYPE="${CONFIGURATION}" \
    -DTUNE_CPU=none \
    -DBUILD_TESTING=OFF \
    -DBUILD_THUNKS=OFF \
    -DBUILD_FEXCONFIG=OFF \
    -DBUILD_FEX_LINUX_TESTS=OFF \
    -DENABLE_LTO=OFF \
    -DENABLE_CCACHE=OFF \
    -DENABLE_OFFLINE_TELEMETRY=OFF \
    || die "FEX: configure failed for iphoneos"

# Build the static targets by name rather than everything. FEX declares both a
# static and a shared FEXCore, and "everything" includes the shared one, which
# has to resolve every symbol at link time - including the ones the ARM64EC
# module provides and this build does not have. An iOS application links
# archives, so the dylib is not merely unnecessary here, it is unbuildable and
# would fail every run.
#
# An unknown target is skipped rather than fatal: which support archives exist
# depends on options the fork resolves itself.
build_target() {
    local target="$1"
    local output
    if ! output="$(cmake --build "${BUILD}" -j "${JOBS}" --target "${target}" 2>&1)"; then
        if grep -qi "unknown target" <<<"${output}"; then
            warn "FEX: no target '${target}' in this configuration; skipping"
            return 0
        fi
        printf '%s\n' "${output}" >&2
        die "FEX: building '${target}' failed"
    fi
    printf '%s\n' "${output}"
    ok "FEX: built ${target}"
}

for target in FEXCore FEXCore_Base JemallocLibs; do
    build_target "${target}"
done

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

# FEXCore's public headers are not self-contained: LogManager.h includes
# <fmt/format.h>, and others reach for FEXHeaderUtils, CodeEmitter,
# unordered_dense and xxhash. Staging only FEXCore/include produces a set that
# compiles inside FEX's own build tree and nowhere else, so the whole reachable
# set is staged into one directory that a consumer can put on its search path.
mkdir -p "${STAGING}/include"
cp -R "${SOURCE}/FEXCore/include/." "${STAGING}/include/"

# Some of FEXCore's headers are generated into the build tree rather than
# checked in - Config.h includes ConfigValues.inl, which a script writes during
# the build - so the source tree alone yields a header set that cannot be
# included even though every file appears to be present.
if [[ -d "${BUILD}/include" ]]; then
    cp -R "${BUILD}/include/." "${STAGING}/include/"
fi
for headers in \
    "External/fmt/include" \
    "External/unordered_dense/include" \
    "External/xxhash" \
    "FEXHeaderUtils" \
    "CodeEmitter"; do
    if [[ -d "${SOURCE}/${headers}" ]]; then
        case "${headers}" in
            */include)
                # Already namespaced by a directory inside: fmt/, ankerl/.
                cp -R "${SOURCE}/${headers}/." "${STAGING}/include/" ;;
            External/xxhash)
                # A bare header at the top of its repository.
                cp "${SOURCE}/${headers}"/*.h "${STAGING}/include/" 2>/dev/null || true ;;
            *)
                # Included as <FEXHeaderUtils/...> and <CodeEmitter/...>, so the
                # directory itself has to survive.
                rm -rf "${STAGING}/include/${headers}"
                mkdir -p "${STAGING}/include/${headers}"
                ( cd "${SOURCE}/${headers}" && find . -name '*.h' -o -name '*.inl' ) \
                    | while IFS= read -r header; do
                        mkdir -p "${STAGING}/include/${headers}/$(dirname "${header}")"
                        cp "${SOURCE}/${headers}/${header}" \
                           "${STAGING}/include/${headers}/${header}"
                    done ;;
        esac
    else
        warn "no ${headers} to stage; a consumer may fail to include it"
    fi
done

[[ -f "${STAGING}/include/FEXCore/Config/ConfigValues.inl" ]] || die \
"FEX: the generated configuration header did not stage.
FEXCore/Config/Config.h includes it, so nothing can initialise FEX without it."

[[ -f "${STAGING}/include/fmt/format.h" ]] || die \
"FEX: fmt headers did not stage, and FEXCore's LogManager.h includes them.
Anything linking these archives would fail on the first include."

log "Staged ${found} archives in ${STAGING}/lib"
( cd "${STAGING}/lib" && ls -lh *.a | awk '{printf "  %-32s %s\n", $9, $5}' )

# One architecture check, because a static library built for the wrong target
# links happily and fails at the very end of a much longer job.
log "Architecture check"
arch="$(lipo -archs "${STAGING}/lib/libFEXCore.a" 2>/dev/null || echo unknown)"
[[ "${arch}" == "arm64" ]] || die \
"FEX: libFEXCore.a reports architecture '${arch}', expected arm64."
ok "libFEXCore.a is arm64"
