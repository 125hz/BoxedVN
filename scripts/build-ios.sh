#!/usr/bin/env bash
# BoxedVN - build the iOS application.
#
# Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
#
# Usage:
#   scripts/build-ios.sh [--configuration Debug|Release]
#                        [--output-dir DIR]
#                        [--sign]
#                        [--skip-dependencies]
#                        [--no-bundled-rootfs]
#                        [--enable-fex64]
#                        [--wine64-runtime DIR|ARCHIVE.zip]
#                        [--wine64-runtime-manifest FILE]
#                        [--x64-graphics-runtime DIR]
#                        [--dxmt-native-archive FILE]
#
# --no-bundled-rootfs omits the ~160 MB root filesystem archive, producing a
# small IPA for fast sign/install iterations.  The app then asks the user to
# import a runtime ZIP once; that imported copy persists across reinstalls.
#
# Two stages:
#   1. CMake builds every C/C++/Objective-C++ target into one static archive.
#   2. XcodeGen regenerates the application project and xcodebuild links the
#      Swift shell against that archive.
#
# Code signing is off by default so the same command works on a CI runner with
# no Apple ID.  Pass --sign to let Xcode sign with whatever identity is
# configured locally.

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
source "${BOXEDVN_SCRIPT_DIR}/dependencies.lock.sh"

CONFIGURATION="Release"
OUTPUT_DIR=""
SIGN=0
SKIP_DEPENDENCIES=0
NO_BUNDLED_ROOTFS=0
ENABLE_FEX64=0
WINE64_RUNTIME_INPUT=""
WINE64_RUNTIME_MANIFEST=""
X64_GRAPHICS_RUNTIME_INPUT=""
DXMT_NATIVE_ARCHIVE=""

usage() {
    sed -n '2,24p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --configuration) [[ $# -ge 2 ]] || die "--configuration needs a value"
                         CONFIGURATION="$2"; shift 2 ;;
        --output-dir)    [[ $# -ge 2 ]] || die "--output-dir needs a value"
                         OUTPUT_DIR="$2"; shift 2 ;;
        --sign)          SIGN=1; shift ;;
        --skip-dependencies) SKIP_DEPENDENCIES=1; shift ;;
        --no-bundled-rootfs) NO_BUNDLED_ROOTFS=1; shift ;;
        --enable-fex64) ENABLE_FEX64=1; shift ;;
        --wine64-runtime) [[ $# -ge 2 ]] || die "--wine64-runtime needs a value"
                           WINE64_RUNTIME_INPUT="$2"; shift 2 ;;
        --wine64-runtime-manifest) [[ $# -ge 2 ]] || die "--wine64-runtime-manifest needs a value"
                                   WINE64_RUNTIME_MANIFEST="$2"; shift 2 ;;
        --x64-graphics-runtime) [[ $# -ge 2 ]] || die "--x64-graphics-runtime needs a value"
                                X64_GRAPHICS_RUNTIME_INPUT="$2"; shift 2 ;;
        --dxmt-native-archive) [[ $# -ge 2 ]] || die "--dxmt-native-archive needs a value"
                               DXMT_NATIVE_ARCHIVE="$2"; shift 2 ;;
        -h|--help)       usage; exit 0 ;;
        *)               die "Unknown argument '$1'. Run with --help." ;;
    esac
done

case "${CONFIGURATION}" in
    Debug|Release) ;;
    *) die "--configuration must be Debug or Release, got '${CONFIGURATION}'." ;;
esac

OUTPUT_DIR="${OUTPUT_DIR:-${BOXEDVN_ROOT}/build/ios-${CONFIGURATION}}"
mkdir -p "${OUTPUT_DIR}"
OUTPUT_DIR="$(cd "${OUTPUT_DIR}" && pwd)"

if [[ -n "${WINE64_RUNTIME_INPUT}" && ${ENABLE_FEX64} -ne 1 ]]; then
    die "--wine64-runtime requires --enable-fex64. The existing iOS runtime has
no 64-bit guest loader when the optional FEX64 backend is disabled."
fi
if [[ -n "${WINE64_RUNTIME_MANIFEST}" && -z "${WINE64_RUNTIME_INPUT}" ]]; then
    die "--wine64-runtime-manifest requires --wine64-runtime."
fi
if [[ -n "${X64_GRAPHICS_RUNTIME_INPUT}" && ${ENABLE_FEX64} -ne 1 ]]; then
    die "--x64-graphics-runtime requires --enable-fex64."
fi
if [[ -n "${X64_GRAPHICS_RUNTIME_INPUT}" && -z "${WINE64_RUNTIME_INPUT}" ]]; then
    die "--x64-graphics-runtime requires --wine64-runtime."
fi
if [[ -n "${X64_GRAPHICS_RUNTIME_INPUT}" ]]; then
    [[ -d "${X64_GRAPHICS_RUNTIME_INPUT}" ]] || die \
        "x64 graphics resources must be supplied as a directory."
    require_file "${X64_GRAPHICS_RUNTIME_INPUT}/boxedvn-d3d11-cube-x64.exe"
    require_file "${X64_GRAPHICS_RUNTIME_INPUT}/x64-graphics.manifest"
    require_file "${X64_GRAPHICS_RUNTIME_INPUT}/libdxmt_combined.a"
    [[ -d "${X64_GRAPHICS_RUNTIME_INPUT}/dxmt-x64" ]] || die \
        "x64 graphics resources are missing dxmt-x64/."
    for dxmt_dll in d3d11 dxgi d3d10core winemetal; do
        require_file "${X64_GRAPHICS_RUNTIME_INPUT}/dxmt-x64/${dxmt_dll}.dll"
    done
    require_command python3
    python3 "${BOXEDVN_SCRIPT_DIR}/validate-dxmt-guest-abi.py" \
        --graphics-dir "${X64_GRAPHICS_RUNTIME_INPUT}"
    if [[ -z "${DXMT_NATIVE_ARCHIVE}" ]]; then
        DXMT_NATIVE_ARCHIVE="${X64_GRAPHICS_RUNTIME_INPUT}/libdxmt_combined.a"
    fi
fi
if [[ -n "${DXMT_NATIVE_ARCHIVE}" ]]; then
    [[ ${ENABLE_FEX64} -eq 1 ]] || die "--dxmt-native-archive requires --enable-fex64."
    require_file "${DXMT_NATIVE_ARCHIVE}" \
        "Build the native iPhoneOS DXMT archive before this step."
fi

require_macos
require_command cmake "Install with 'brew install cmake'."
require_command ninja "Install with 'brew install ninja'."
require_command xcodebuild "Install Xcode from the App Store."
require_command xcrun

require_file "${BOXEDVN_ROOT}/CMakeLists.txt" \
    "Run this from a complete clone of the repository."
require_file "${BOXEDVN_ROOT}/ios/project.yml" \
    "The XcodeGen specification is missing."

print_tool_versions

if ! xcrun --sdk iphoneos --show-sdk-path >/dev/null 2>&1; then
    die "The iPhoneOS SDK is not installed.
Check 'xcode-select -p' points at a full Xcode (not the Command Line Tools) and
that the iOS platform is installed:
  sudo xcode-select -s /Applications/Xcode.app
  xcodebuild -downloadPlatform iOS"
fi

# ---------------------------------------------------------------------------
# 1. Dependencies
# ---------------------------------------------------------------------------
SDL2_PREFIX="${BOXEDVN_THIRD_PARTY}/prefix/ios"
MOLTENVK_ROOT="${BOXEDVN_THIRD_PARTY}/src/MoltenVK-ios-${BOXEDVN_MOLTENVK_VERSION}"
XCODEGEN="${BOXEDVN_THIRD_PARTY}/xcodegen-${BOXEDVN_XCODEGEN_VERSION}/bin/xcodegen"
FEX64_PREFIX="${BOXEDVN_THIRD_PARTY}/fex64/fex-ios"

if [[ ${SKIP_DEPENDENCIES} -eq 0 ]]; then
    log "Fetching dependencies"
    "${BOXEDVN_SCRIPT_DIR}/fetch-dependencies.sh" \
        --platform ios --configuration Release
fi

require_file "${SDL2_PREFIX}/lib/libSDL2.a" \
    "Run scripts/fetch-dependencies.sh --platform ios first."
require_file "${MOLTENVK_ROOT}/MoltenVK/static/MoltenVK.xcframework/ios-arm64/libMoltenVK.a" \
    "Run scripts/fetch-dependencies.sh --platform ios first."
[[ -x "${XCODEGEN}" ]] \
    || die "XcodeGen is missing at '${XCODEGEN}'. Run scripts/fetch-dependencies.sh."

# ---------------------------------------------------------------------------
# 2. CMake: the emulator, the support library and the bridge
# ---------------------------------------------------------------------------
CMAKE_BUILD_DIR="${OUTPUT_DIR}/cmake"

cmake_options=(
    -DCMAKE_SYSTEM_NAME=iOS
    -DCMAKE_OSX_ARCHITECTURES=arm64
    -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0
    -DCMAKE_OSX_SYSROOT=iphoneos
    -DCMAKE_BUILD_TYPE="${CONFIGURATION}"
    -DBOXEDVN_BUILD_TESTS=OFF
    -DBOXEDVN_SDL2_ROOT="${SDL2_PREFIX}"
    -DBOXEDVN_MOLTENVK_ROOT="${MOLTENVK_ROOT}"
)
if [[ ${ENABLE_FEX64} -eq 1 ]]; then
    require_file "${FEX64_PREFIX}/lib/libFEXCore.a" \
        "Run scripts/build-fex64-fex.sh before enabling the optional FEX backend."
    require_file "${BOXEDVN_ROOT}/build/guest-probes/boxedvn-fex64-kernel-probe" \
        "Run scripts/build-guest-fex64-probe.sh before enabling the optional FEX backend."
    strings "${BOXEDVN_ROOT}/build/guest-probes/boxedvn-fex64-kernel-probe" | \
        grep -Fq 'BoxedWine FEX64 SSE2/REP STOS/call-ret PASS' || {
        echo "error: the bundled FEX correctness probe is stale; rebuild it" >&2
        exit 1
    }
    cmake_options+=(
        -DBOXEDVN_ENABLE_FEX64=ON
        -DBOXEDVN_ENABLE_GUEST_X64=ON
        -DBOXEDVN_FEX64_ROOT="${FEX64_PREFIX}"
    )
    [[ -n "${DXMT_NATIVE_ARCHIVE}" ]] && cmake_options+=(
        -DBOXEDVN_DXMT_NATIVE_ARCHIVE="${DXMT_NATIVE_ARCHIVE}"
    )
    log "Optional BoxedWine x86-64 guest ABI and FEX translator enabled"
fi

WINE64_PREPARED_DIR=""
if [[ -n "${WINE64_RUNTIME_INPUT}" ]]; then
    WINE64_PREPARED_DIR="${OUTPUT_DIR}/wine64-runtime-validated"
    log "Validating CI-produced BoxedWine64 Wine64 layers"
    wine64_prepare_args=(
        --input "${WINE64_RUNTIME_INPUT}"
        --output-dir "${WINE64_PREPARED_DIR}"
    )
    [[ -n "${WINE64_RUNTIME_MANIFEST}" ]] \
        && wine64_prepare_args+=(--manifest "${WINE64_RUNTIME_MANIFEST}")
    bash "${BOXEDVN_SCRIPT_DIR}/prepare-wine64-runtime.sh" "${wine64_prepare_args[@]}"
    ok "Wine64 layers validated; they will be staged as optional app resources"
fi

log "Configuring the native build (${CONFIGURATION})"
cmake -S "${BOXEDVN_ROOT}" -B "${CMAKE_BUILD_DIR}" -G Ninja \
    "${cmake_options[@]}" \
    || die "CMake configure failed."

log "Building the native targets"
cmake --build "${CMAKE_BUILD_DIR}" --parallel "$(sysctl -n hw.ncpu)" \
    || die "CMake build failed."

MERGED_LIBRARY="${CMAKE_BUILD_DIR}/libboxedvn.a"
require_file "${MERGED_LIBRARY}" \
    "The merged static library was not produced; check the build log above."

if ! lipo -info "${MERGED_LIBRARY}" | grep -q arm64; then
    die "'${MERGED_LIBRARY}' does not contain an arm64 slice:
$(lipo -info "${MERGED_LIBRARY}")"
fi
ok "libboxedvn.a: $(lipo -info "${MERGED_LIBRARY}")"

# ---------------------------------------------------------------------------
# 3. XcodeGen + xcodebuild: the Swift application shell
# ---------------------------------------------------------------------------
# ios/app/Bundled is an optional resource directory, so a slim build only has
# to make the archive invisible for the duration of the build. The app already
# falls back to a user-imported archive when no bundled one is present
# (Storage.activeRootFilesystem), which is what makes ~6 MB iterations possible
# on device: sign and install the shell, import the runtime ZIP once, and every
# later build reuses it. The stash is restored unconditionally on exit so an
# interrupted build can never lose a 160 MB download.
BUNDLED_ROOTFS="${BOXEDVN_ROOT}/ios/app/Bundled/boxedwine.zip"
STASHED_ROOTFS=""
WINE64_BUNDLE_DIR="${BOXEDVN_ROOT}/ios/app/Bundled/boxedwine64-runtime"
STASHED_WINE64_BUNDLE=""
CREATED_WINE64_BUNDLE=0
CREATED_BUNDLED_DIR=0
restore_bundled_resources() {
    if [[ -n "${STASHED_ROOTFS}" && -f "${STASHED_ROOTFS}" ]]; then
        mv "${STASHED_ROOTFS}" "${BUNDLED_ROOTFS}"
        STASHED_ROOTFS=""
    fi
    if [[ -n "${STASHED_WINE64_BUNDLE}" && -d "${STASHED_WINE64_BUNDLE}" ]]; then
        if [[ -d "${WINE64_BUNDLE_DIR}" ]]; then
            # This is the exact temporary staging directory created below.
            # Remove it before restoring the developer's original bundle;
            # otherwise mv nests the stash inside the staged directory.
            rm -rf "${WINE64_BUNDLE_DIR}"
        fi
        mv "${STASHED_WINE64_BUNDLE}" "${WINE64_BUNDLE_DIR}"
        STASHED_WINE64_BUNDLE=""
        CREATED_WINE64_BUNDLE=0
    elif [[ ${CREATED_WINE64_BUNDLE} -eq 1 && -d "${WINE64_BUNDLE_DIR}" ]]; then
        # This exact directory was created by this invocation; do not leave
        # runtime material in the source tree after a build.
        rm -rf "${WINE64_BUNDLE_DIR}"
        CREATED_WINE64_BUNDLE=0
    fi
    if [[ ${CREATED_BUNDLED_DIR} -eq 1 && -d "$(dirname "${WINE64_BUNDLE_DIR}")" ]]; then
        rmdir "$(dirname "${WINE64_BUNDLE_DIR}")" 2>/dev/null || true
        CREATED_BUNDLED_DIR=0
    fi
}
trap restore_bundled_resources EXIT
if [[ ${NO_BUNDLED_ROOTFS} -eq 1 && -f "${BUNDLED_ROOTFS}" ]]; then
    STASHED_ROOTFS="${BOXEDVN_ROOT}/build/boxedwine.zip.stashed"
    mkdir -p "$(dirname "${STASHED_ROOTFS}")"
    mv "${BUNDLED_ROOTFS}" "${STASHED_ROOTFS}"
    log "Building without the bundled root filesystem; the app will require an imported runtime ZIP"
fi

if [[ -n "${WINE64_PREPARED_DIR}" ]]; then
    if [[ -e "${WINE64_BUNDLE_DIR}" ]]; then
        [[ -d "${WINE64_BUNDLE_DIR}" ]] \
            || die "Refusing to replace non-directory '${WINE64_BUNDLE_DIR}'."
        STASHED_WINE64_BUNDLE="${OUTPUT_DIR}/boxedwine64-runtime.stashed"
        [[ ! -e "${STASHED_WINE64_BUNDLE}" ]] \
            || die "Refusing to overwrite existing '${STASHED_WINE64_BUNDLE}'."
        mv "${WINE64_BUNDLE_DIR}" "${STASHED_WINE64_BUNDLE}"
    fi
    if [[ ! -d "$(dirname "${WINE64_BUNDLE_DIR}")" ]]; then
        mkdir -p "$(dirname "${WINE64_BUNDLE_DIR}")"
        CREATED_BUNDLED_DIR=1
    fi
    mkdir -p "${WINE64_BUNDLE_DIR}"
    CREATED_WINE64_BUNDLE=1
    cp "${WINE64_PREPARED_DIR}/glibc-rootfs64.zip" \
       "${WINE64_BUNDLE_DIR}/glibc-rootfs64.zip"
    cp "${WINE64_PREPARED_DIR}/wine64.zip" \
       "${WINE64_BUNDLE_DIR}/wine64.zip"
    cp "${WINE64_PREPARED_DIR}/wine64-runtime.manifest" \
       "${WINE64_BUNDLE_DIR}/wine64-runtime.manifest"
    log "Staged optional BoxedWine64 runtime resources in the app bundle"
fi

if [[ -n "${X64_GRAPHICS_RUNTIME_INPUT}" ]]; then
    if [[ ! -d "${WINE64_BUNDLE_DIR}" ]]; then
        mkdir -p "${WINE64_BUNDLE_DIR}"
        CREATED_WINE64_BUNDLE=1
    fi
    cp "${X64_GRAPHICS_RUNTIME_INPUT}/boxedvn-d3d11-cube-x64.exe" \
       "${WINE64_BUNDLE_DIR}/boxedvn-d3d11-cube-x64.exe"
    rm -rf "${WINE64_BUNDLE_DIR}/dxmt-x64"
    cp -R "${X64_GRAPHICS_RUNTIME_INPUT}/dxmt-x64" \
          "${WINE64_BUNDLE_DIR}/dxmt-x64"
    cp "${X64_GRAPHICS_RUNTIME_INPUT}/x64-graphics.manifest" \
       "${WINE64_BUNDLE_DIR}/x64-graphics.manifest"
    log "Staged validated x86-64 graphics probe and DXMT PE resources"
fi

log "Generating the application project"
(
    cd "${BOXEDVN_ROOT}/ios"
    "${XCODEGEN}" generate --spec project.yml --project . --quiet
) || die "XcodeGen failed."

# Stamp the build with the commit it came from.  A sideloaded IPA has no
# update channel, so "which build am I running?" is otherwise unanswerable from
# the device.  A working tree with uncommitted changes is marked +dirty: the
# commit alone would be a lie about what is in the binary.
BUILD_REVISION="unknown"
if git -C "${BOXEDVN_ROOT}" rev-parse --git-dir >/dev/null 2>&1; then
    BUILD_REVISION="$(git -C "${BOXEDVN_ROOT}" rev-parse --short=8 HEAD)"
    if ! git -C "${BOXEDVN_ROOT}" diff --quiet HEAD 2>/dev/null; then
        BUILD_REVISION="${BUILD_REVISION}+dirty"
    fi
fi
log "Build revision: ${BUILD_REVISION}"

XCODE_PROJECT="${BOXEDVN_ROOT}/ios/BoxedVN.xcodeproj"
[[ -d "${XCODE_PROJECT}" ]] || die "XcodeGen did not produce ${XCODE_PROJECT}."

DERIVED_DATA="${OUTPUT_DIR}/DerivedData"
BUILD_LOG="${OUTPUT_DIR}/xcodebuild.log"
mkdir -p "${OUTPUT_DIR}"

signing_args=(
    CODE_SIGNING_ALLOWED=NO
    CODE_SIGNING_REQUIRED=NO
    CODE_SIGN_IDENTITY=""
    CODE_SIGN_ENTITLEMENTS=""
    DEVELOPMENT_TEAM=""
)
if [[ ${SIGN} -eq 1 ]]; then
    log "Code signing enabled (using the locally configured identity)"
    signing_args=()
fi

DXMT_FORCE_LINK=""
if [[ -n "${DXMT_NATIVE_ARCHIVE}" ]]; then
    DXMT_FORCE_LINK="-Wl,-u,_dxmt_winemetal_unix_call_funcs"
fi

log "Building BoxedVN.app for generic iOS device"
set +e
xcodebuild \
    -project "${XCODE_PROJECT}" \
    -scheme BoxedVN \
    -configuration "${CONFIGURATION}" \
    -destination 'generic/platform=iOS' \
    -derivedDataPath "${DERIVED_DATA}" \
    BVN_LIBRARY_DIR="${CMAKE_BUILD_DIR}" \
    BVN_INCLUDE_DIR="${BOXEDVN_ROOT}/ios/runtime/include" \
    BVN_BUILD_REVISION="${BUILD_REVISION}" \
    BVN_DXMT_FORCE_LINK="${DXMT_FORCE_LINK}" \
    "${signing_args[@]}" \
    build \
    >"${BUILD_LOG}" 2>&1
xcodebuild_status=$?
set -e

if [[ ${xcodebuild_status} -ne 0 ]]; then
    echo "--- last 60 lines of ${BUILD_LOG} ---" >&2
    tail -60 "${BUILD_LOG}" >&2
    die "xcodebuild failed with status ${xcodebuild_status}. Full log: ${BUILD_LOG}"
fi

APP_PATH="${DERIVED_DATA}/Build/Products/${CONFIGURATION}-iphoneos/BoxedVN.app"
if [[ ! -d "${APP_PATH}" ]]; then
    die "xcodebuild reported success but '${APP_PATH}' does not exist.
Search ${BUILD_LOG} for 'BoxedVN.app' to see where it was written."
fi

ok "Built ${APP_PATH}"

# ---------------------------------------------------------------------------
# 4. Validate the product before anyone tries to install it
# ---------------------------------------------------------------------------
if [[ -n "${DXMT_NATIVE_ARCHIVE}" ]]; then
    export BOXEDVN_REQUIRE_DXMT_NATIVE=1
fi
if [[ ${ENABLE_FEX64} -eq 1 ]]; then
    export BOXEDVN_REQUIRE_FEX_PROBE=1
fi
"${BOXEDVN_SCRIPT_DIR}/validate-app.sh" "${APP_PATH}"
if [[ ${ENABLE_FEX64} -eq 1 ]]; then
    cmp "${BOXEDVN_ROOT}/build/guest-probes/boxedvn-fex64-kernel-probe" \
        "${APP_PATH}/boxedvn-fex64-kernel-probe" \
        || die "The bundled FEX correctness probe differs from the validated CI input."
    ok "bundled FEX correctness probe matches the validated input"
fi

printf '\n'
log "Build complete"
printf '  app        : %s\n' "${APP_PATH}"
printf '  build log  : %s\n' "${BUILD_LOG}"
printf '  next step  : scripts/package-ipa.sh --app "%s"\n' "${APP_PATH}"
