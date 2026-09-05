#!/usr/bin/env bash
# BoxedVN - download and build pinned third-party dependencies.
#
# Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
#
# Usage:
#   scripts/fetch-dependencies.sh [--platform ios|ios-simulator|macos|all]
#                                 [--configuration Debug|Release]
#                                 [--third-party-dir DIR]
#                                 [--force]
#
# Everything downloaded is pinned by version, URL and SHA-256 in
# scripts/dependencies.lock.sh.  A checksum mismatch is fatal.

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
source "${BOXEDVN_SCRIPT_DIR}/dependencies.lock.sh"

PLATFORMS="ios"
CONFIGURATION="Release"
FORCE=0

usage() {
    sed -n '2,14p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --platform)
            [[ $# -ge 2 ]] || die "--platform needs a value"
            PLATFORMS="$2"; shift 2 ;;
        --configuration)
            [[ $# -ge 2 ]] || die "--configuration needs a value"
            CONFIGURATION="$2"; shift 2 ;;
        --third-party-dir)
            [[ $# -ge 2 ]] || die "--third-party-dir needs a value"
            BOXEDVN_THIRD_PARTY="$2"; shift 2 ;;
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

if [[ "${PLATFORMS}" == "all" ]]; then
    PLATFORMS="ios ios-simulator macos"
fi

require_macos
require_command cmake "Install it with 'brew install cmake' or from cmake.org."
require_command curl
require_command tar
require_command shasum
require_command xcrun "Install Xcode and run 'xcode-select --install'."

print_tool_versions

DOWNLOADS="${BOXEDVN_THIRD_PARTY}/downloads"
SOURCES="${BOXEDVN_THIRD_PARTY}/src"
PREFIXES="${BOXEDVN_THIRD_PARTY}/prefix"
mkdir -p "${DOWNLOADS}" "${SOURCES}" "${PREFIXES}"

# ---------------------------------------------------------------------------
# SDL2
# ---------------------------------------------------------------------------
SDL2_TARBALL="${DOWNLOADS}/SDL2-${BOXEDVN_SDL2_VERSION}.tar.gz"
SDL2_SOURCE="${SOURCES}/SDL2-${BOXEDVN_SDL2_VERSION}"

download_pinned "${BOXEDVN_SDL2_URL}" "${SDL2_TARBALL}" "${BOXEDVN_SDL2_SHA256}"

if [[ ! -d "${SDL2_SOURCE}" ]]; then
    log "Extracting SDL2 ${BOXEDVN_SDL2_VERSION}"
    tar -xzf "${SDL2_TARBALL}" -C "${SOURCES}"
    [[ -d "${SDL2_SOURCE}" ]] \
        || die "SDL2 tarball did not extract to the expected directory \
'${SDL2_SOURCE}'. The pinned archive layout may have changed."
fi
require_file "${SDL2_SOURCE}/CMakeLists.txt" \
    "The extracted SDL2 tree looks wrong; delete ${SOURCES} and retry."

# Restore only the patched file from the verified archive before applying our
# UIKit change. This also handles a cached tree carrying an older patch.
SDL2_PATCH="${BOXEDVN_SCRIPT_DIR}/sdl-patches/sdl2-bounded-uikit-pump.patch"
require_file "${SDL2_PATCH}" "The SDL UIKit event-pump patch is missing."
SDL2_PATCH_KEY="$(shasum -a 256 "${SDL2_PATCH}" | awk '{print $1}')"
tar -xzf "${SDL2_TARBALL}" -C "${SOURCES}" \
    "SDL2-${BOXEDVN_SDL2_VERSION}/src/video/uikit/SDL_uikitevents.m"
patch --batch --forward -d "${SDL2_SOURCE}" -p1 < "${SDL2_PATCH}" \
    || die "The SDL UIKit patch does not match the pinned source."
python3 "${BOXEDVN_SCRIPT_DIR}/test-sdl-uikit-pump.py" \
    --source "${SDL2_SOURCE}/src/video/uikit/SDL_uikitevents.m"

# build_sdl2 <platform>
build_sdl2() {
    local platform="$1"
    local prefix="${PREFIXES}/${platform}"
    local build_dir="${BOXEDVN_THIRD_PARTY}/build/sdl2-${platform}-${CONFIGURATION}"
    local -a args=()

    if [[ ${FORCE} -eq 1 ]]; then
        rm -rf "${build_dir}" "${prefix}"
    fi

    if [[ -f "${prefix}/lib/libSDL2.a" && ${FORCE} -eq 0 &&
          -f "${prefix}/share/boxedvn/sdl2-patch-key" &&
          "$(cat "${prefix}/share/boxedvn/sdl2-patch-key")" == "${SDL2_PATCH_KEY}" ]]; then
        ok "SDL2 for ${platform} already built at ${prefix}"
        return 0
    fi

    # The logs live beside the build directory, so its parent must exist even
    # before CMake creates the build directory itself.
    mkdir -p "$(dirname "${build_dir}")"

    case "${platform}" in
        ios)
            args+=(
                -DCMAKE_SYSTEM_NAME=iOS
                -DCMAKE_OSX_ARCHITECTURES=arm64
                -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0
                -DCMAKE_OSX_SYSROOT=iphoneos
            )
            ;;
        ios-simulator)
            # The simulator runs as a native process at the host's own
            # architecture (no translation for simulator binaries), so unlike
            # the device build this cannot default to arm64 - it must match
            # whatever Mac is running it.
            args+=(
                -DCMAKE_SYSTEM_NAME=iOS
                -DCMAKE_OSX_ARCHITECTURES="$(uname -m)"
                -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0
                -DCMAKE_OSX_SYSROOT=iphonesimulator
            )
            ;;
        macos)
            args+=(
                -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0
            )
            ;;
        *)
            die "Unknown platform '${platform}'. Use ios, ios-simulator or macos."
            ;;
    esac

    log "Configuring SDL2 ${BOXEDVN_SDL2_VERSION} for ${platform} (${CONFIGURATION})"
    cmake -S "${SDL2_SOURCE}" -B "${build_dir}" \
        -G "Unix Makefiles" \
        -DCMAKE_BUILD_TYPE="${CONFIGURATION}" \
        -DCMAKE_INSTALL_PREFIX="${prefix}" \
        -DSDL_SHARED=OFF \
        -DSDL_STATIC=ON \
        -DSDL_TEST=OFF \
        -DSDL_INSTALL_TESTS=OFF \
        "${args[@]}" \
        >"${build_dir}.configure.log" 2>&1 \
        || { tail -40 "${build_dir}.configure.log" >&2
             die "SDL2 configure failed for ${platform}. Full log: ${build_dir}.configure.log"; }

    log "Building SDL2 for ${platform}"
    cmake --build "${build_dir}" --parallel "$(sysctl -n hw.ncpu)" \
        >"${build_dir}.build.log" 2>&1 \
        || { tail -60 "${build_dir}.build.log" >&2
             die "SDL2 build failed for ${platform}. Full log: ${build_dir}.build.log"; }

    cmake --install "${build_dir}" >>"${build_dir}.build.log" 2>&1 \
        || { tail -40 "${build_dir}.build.log" >&2
             die "SDL2 install failed for ${platform}."; }

    require_file "${prefix}/lib/libSDL2.a" \
        "SDL2 installed but libSDL2.a is missing; the install layout changed."
    mkdir -p "${prefix}/share/boxedvn"
    printf '%s\n' "${SDL2_PATCH_KEY}" > "${prefix}/share/boxedvn/sdl2-patch-key"
    ok "SDL2 for ${platform} -> ${prefix}"
}

for platform in ${PLATFORMS}; do
    build_sdl2 "${platform}"
done

# ---------------------------------------------------------------------------
# MoltenVK (prebuilt static iOS arm64 XCFramework from Khronos)
# ---------------------------------------------------------------------------
MOLTENVK_ARCHIVE="${DOWNLOADS}/MoltenVK-ios-${BOXEDVN_MOLTENVK_VERSION}.tar"
MOLTENVK_SOURCE="${SOURCES}/MoltenVK-ios-${BOXEDVN_MOLTENVK_VERSION}"
MOLTENVK_LIBRARY="${MOLTENVK_SOURCE}/MoltenVK/static/MoltenVK.xcframework/ios-arm64/libMoltenVK.a"

download_pinned "${BOXEDVN_MOLTENVK_URL}" "${MOLTENVK_ARCHIVE}" \
                "${BOXEDVN_MOLTENVK_SHA256}"

if [[ ! -f "${MOLTENVK_LIBRARY}" ]]; then
    log "Extracting MoltenVK ${BOXEDVN_MOLTENVK_VERSION} for iOS"
    rm -rf "${MOLTENVK_SOURCE}"
    mkdir -p "${MOLTENVK_SOURCE}"
    tar -xf "${MOLTENVK_ARCHIVE}" -C "${MOLTENVK_SOURCE}" \
        --strip-components=1
fi
require_file "${MOLTENVK_LIBRARY}" \
    "The MoltenVK archive layout changed; expected its static iOS arm64 library."
ok "MoltenVK ${BOXEDVN_MOLTENVK_VERSION} -> ${MOLTENVK_SOURCE}"

# ---------------------------------------------------------------------------
# XcodeGen (only needed when generating the application shell)
# ---------------------------------------------------------------------------
XCODEGEN_ZIP="${DOWNLOADS}/xcodegen-${BOXEDVN_XCODEGEN_VERSION}.zip"
XCODEGEN_DIR="${BOXEDVN_THIRD_PARTY}/xcodegen-${BOXEDVN_XCODEGEN_VERSION}"

download_pinned "${BOXEDVN_XCODEGEN_URL}" "${XCODEGEN_ZIP}" \
                "${BOXEDVN_XCODEGEN_SHA256}"

if [[ ! -x "${XCODEGEN_DIR}/bin/xcodegen" ]]; then
    log "Extracting XcodeGen ${BOXEDVN_XCODEGEN_VERSION}"
    rm -rf "${XCODEGEN_DIR}"
    mkdir -p "${XCODEGEN_DIR}"
    require_command unzip
    unzip -q "${XCODEGEN_ZIP}" -d "${XCODEGEN_DIR}.tmp"
    mv "${XCODEGEN_DIR}.tmp/xcodegen"/* "${XCODEGEN_DIR}/"
    rm -rf "${XCODEGEN_DIR}.tmp"
    chmod +x "${XCODEGEN_DIR}/bin/xcodegen"
fi
[[ -x "${XCODEGEN_DIR}/bin/xcodegen" ]] \
    || die "XcodeGen did not extract to ${XCODEGEN_DIR}/bin/xcodegen."
ok "XcodeGen ${BOXEDVN_XCODEGEN_VERSION} -> ${XCODEGEN_DIR}/bin/xcodegen"

log "Dependencies ready under ${BOXEDVN_THIRD_PARTY}"
for platform in ${PLATFORMS}; do
    printf '  SDL2 (%s): %s\n' "${platform}" "${PREFIXES}/${platform}"
done
printf '  MoltenVK (ios): %s\n' "${MOLTENVK_SOURCE}"
