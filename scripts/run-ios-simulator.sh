#!/usr/bin/env bash
# BoxedVN - build and run the app in the iOS Simulator, for debugging only.
#
# Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
#
# Usage: scripts/run-ios-simulator.sh [--udid UDID] [--skip-dependencies]
#
# This is NOT the shipping configuration - see scripts/build-ios.sh for that.
# Boxedwine's ARM64 JIT does not exist on x86_64/simulator, so no guest
# session will ever start here: KNativeAudio, the JIT probe, and any launch
# request all behave accordingly and say so.
#
# What this IS useful for: everything that happens before a guest starts -
# app delegate / UIApplicationMain lifecycle, the SDLUIKitDelegate subclass,
# SwiftUI window creation, the library UI, import. That is a real class of
# bugs, independent of device-only JIT/emulation behaviour, and it is much
# faster to iterate on here than by round-tripping through sideloading.
#
# Builds for the Mac's own architecture, matching how Simulator binaries work
# (no translation): x86_64 on an Intel Mac, arm64 on Apple Silicon.

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
source "${BOXEDVN_SCRIPT_DIR}/dependencies.lock.sh"

UDID=""
SKIP_DEPENDENCIES=0

usage() {
    sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --udid)               [[ $# -ge 2 ]] || die "--udid needs a value"
                              UDID="$2"; shift 2 ;;
        --skip-dependencies)  SKIP_DEPENDENCIES=1; shift ;;
        -h|--help)            usage; exit 0 ;;
        *)                    die "Unknown argument '$1'. Run with --help." ;;
    esac
done

require_macos
require_command cmake
require_command ninja
require_command xcodebuild
require_command xcrun

HOST_ARCH="$(uname -m)"
print_tool_versions
log "Simulator target architecture: ${HOST_ARCH} (matches this Mac; no translation)"

if [[ -z "${UDID}" ]]; then
    UDID="$(xcrun simctl list devices booted | grep -m1 -oE '[0-9A-F]{8}-([0-9A-F]{4}-){3}[0-9A-F]{12}' || true)"
    [[ -n "${UDID}" ]] || die "No simulator is booted and --udid was not given.
Boot one first, e.g.:
  xcrun simctl boot \"iPhone 17\""
    log "Using booted simulator ${UDID}"
fi

SDL2_PREFIX="${BOXEDVN_THIRD_PARTY}/prefix/ios-simulator"

if [[ ${SKIP_DEPENDENCIES} -eq 0 ]]; then
    "${BOXEDVN_SCRIPT_DIR}/fetch-dependencies.sh" \
        --platform ios-simulator --configuration Release
fi
require_file "${SDL2_PREFIX}/lib/libSDL2.a" \
    "Run scripts/fetch-dependencies.sh --platform ios-simulator first."

CMAKE_BUILD_DIR="${BOXEDVN_ROOT}/build/ios-simulator"

log "Configuring the native build for ${HOST_ARCH}"
cmake -S "${BOXEDVN_ROOT}" -B "${CMAKE_BUILD_DIR}" -G Ninja \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_ARCHITECTURES="${HOST_ARCH}" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
    -DCMAKE_OSX_SYSROOT=iphonesimulator \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBOXEDVN_BUILD_TESTS=OFF \
    -DBOXEDVN_SDL2_ROOT="${SDL2_PREFIX}" \
    || die "CMake configure failed."

log "Building the native targets"
cmake --build "${CMAKE_BUILD_DIR}" --parallel "$(sysctl -n hw.ncpu)" \
    || die "CMake build failed."

MERGED_LIBRARY="${CMAKE_BUILD_DIR}/libboxedvn.a"
require_file "${MERGED_LIBRARY}" "The merged static library was not produced."
ok "libboxedvn.a: $(lipo -info "${MERGED_LIBRARY}")"

log "Generating the simulator application project"
require_file "${BOXEDVN_ROOT}/ios/project-simulator.yml" \
    "ios/project-simulator.yml is missing."
XCODEGEN="${BOXEDVN_THIRD_PARTY}/xcodegen-${BOXEDVN_XCODEGEN_VERSION}/bin/xcodegen"
require_file "${XCODEGEN}" "Run scripts/fetch-dependencies.sh first."
(
    cd "${BOXEDVN_ROOT}/ios"
    "${XCODEGEN}" generate --spec project-simulator.yml --project . --quiet
) || die "XcodeGen failed."

OUTPUT_DIR="${BOXEDVN_ROOT}/build/ios-simulator-app"
mkdir -p "${OUTPUT_DIR}"

log "Building BoxedVN.app for the simulator"
xcodebuild \
    -project "${BOXEDVN_ROOT}/ios/BoxedVN.xcodeproj" \
    -scheme BoxedVN \
    -configuration Debug \
    -destination "platform=iOS Simulator,id=${UDID}" \
    -derivedDataPath "${OUTPUT_DIR}/DerivedData" \
    BVN_LIBRARY_DIR="${CMAKE_BUILD_DIR}" \
    BVN_INCLUDE_DIR="${BOXEDVN_ROOT}/ios/runtime/include" \
    build >"${OUTPUT_DIR}/xcodebuild.log" 2>&1 \
    || { tail -60 "${OUTPUT_DIR}/xcodebuild.log" >&2
         die "xcodebuild failed. Full log: ${OUTPUT_DIR}/xcodebuild.log"; }

APP_PATH="${OUTPUT_DIR}/DerivedData/Build/Products/Debug-iphonesimulator/BoxedVN.app"
require_file "${APP_PATH}/BoxedVN" "The simulator build did not produce an executable."

log "Installing and launching on ${UDID}"
xcrun simctl terminate "${UDID}" org.boxedwine.boxedvn >/dev/null 2>&1 || true
xcrun simctl install "${UDID}" "${APP_PATH}" || die "simctl install failed."
xcrun simctl launch "${UDID}" org.boxedwine.boxedvn || die "simctl launch failed."

ok "Running. App bundle: ${APP_PATH}"
printf '\nSession logs land under:\n'
printf '  ~/Library/Developer/CoreSimulator/Devices/%s/data/Containers/Data/Application/<container>/Documents/Logs/\n' "${UDID}"
