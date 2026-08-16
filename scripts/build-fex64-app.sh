#!/usr/bin/env bash
# BoxedVN - build the fex64 application and package it as an unsigned IPA.
#
# Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
#
# Usage:
#   scripts/build-fex64-app.sh [--configuration Release|Debug]
#                              [--output-dir DIR] [--third-party-dir DIR]
#                              [--skip-fex]
#
# Stage A2 of docs/ARCHITECTURE_FEX64.md. Builds FEX if its archives are not
# already present, generates the Xcode project, links the BoxedVNFex target
# against those archives, and produces an unsigned IPA a sideloader can sign.
#
# The linking is the point. Two of FEX's diagnostic buffers are defined in the
# ARM64EC module rather than in FEXCore, so whether an application can link
# these archives at all is an open question until this job answers it.
#
# macOS with a full Xcode only.

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
source "${BOXEDVN_SCRIPT_DIR}/dependencies.lock.sh"

CONFIGURATION="Release"
OUTPUT_DIR=""
SKIP_FEX=0

usage() {
    sed -n '2,19p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --configuration)
            [[ $# -ge 2 ]] || die "--configuration needs a value"
            CONFIGURATION="$2"; shift 2 ;;
        --output-dir)
            [[ $# -ge 2 ]] || die "--output-dir needs a value"
            OUTPUT_DIR="$2"; shift 2 ;;
        --third-party-dir)
            [[ $# -ge 2 ]] || die "--third-party-dir needs a value"
            BOXEDVN_THIRD_PARTY="$2"; shift 2 ;;
        --skip-fex)
            SKIP_FEX=1; shift ;;
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
require_command xcodebuild "Install Xcode from the App Store."

[[ -n "${OUTPUT_DIR}" ]] || OUTPUT_DIR="${BOXEDVN_ROOT}/build/fex64-${CONFIGURATION}"
mkdir -p "${OUTPUT_DIR}"

FEX_STAGING="${BOXEDVN_THIRD_PARTY}/fex64/fex-ios"

# The archive alone is not evidence of a usable staging directory. A cached
# one from before the headers were complete has libFEXCore.a in it and nothing
# that can include it, and then this job fails on a compile error that looks
# like a source problem. Check one file from each half.
fex_staging_is_complete() {
    [[ -f "${FEX_STAGING}/lib/libFEXCore.a" ]] &&
    [[ -f "${FEX_STAGING}/include/FEXCore/Config/ConfigValues.inl" ]] &&
    [[ -f "${FEX_STAGING}/include/fmt/format.h" ]]
}

if [[ "${SKIP_FEX}" -eq 0 ]] && ! fex_staging_is_complete; then
    if [[ -d "${FEX_STAGING}" ]]; then
        warn "the staged FEX is incomplete - probably cached from an older \
staging step - so it is being rebuilt"
        rm -rf "${FEX_STAGING}"
    else
        log "FEX archives are missing; building them first"
    fi
    "${BOXEDVN_SCRIPT_DIR}/build-fex64-fex.sh" --configuration "${CONFIGURATION}"
fi

fex_staging_is_complete || die \
"The staged FEX at '${FEX_STAGING}' is incomplete.
Run scripts/build-fex64-fex.sh, or drop --skip-fex."

XCODEGEN="${BOXEDVN_THIRD_PARTY}/xcodegen-${BOXEDVN_XCODEGEN_VERSION}/bin/xcodegen"
[[ -x "${XCODEGEN}" ]] \
    || die "XcodeGen is missing at '${XCODEGEN}'. Run scripts/fetch-dependencies.sh."

print_tool_versions

BUILD_REVISION="$(git -C "${BOXEDVN_ROOT}" rev-parse --short HEAD 2>/dev/null || echo unknown)"

log "Generating the Xcode project"
( cd "${BOXEDVN_ROOT}/ios" && "${XCODEGEN}" generate --spec project.yml --project . --quiet )

XCODE_PROJECT="${BOXEDVN_ROOT}/ios/BoxedVN.xcodeproj"
DERIVED_DATA="${OUTPUT_DIR}/DerivedData"
BUILD_LOG="${OUTPUT_DIR}/xcodebuild.log"

log "Building BoxedVNFex.app for generic iOS device"
set +e
xcodebuild \
    -project "${XCODE_PROJECT}" \
    -scheme BoxedVNFex \
    -configuration "${CONFIGURATION}" \
    -destination 'generic/platform=iOS' \
    -derivedDataPath "${DERIVED_DATA}" \
    BVN_FEX_LIBRARY_DIR="${FEX_STAGING}/lib" \
    BVN_FEX_INCLUDE_DIR="${FEX_STAGING}/include" \
    BVN_BUILD_REVISION="${BUILD_REVISION}" \
    CODE_SIGN_IDENTITY="" CODE_SIGNING_REQUIRED=NO CODE_SIGNING_ALLOWED=NO \
    build \
    >"${BUILD_LOG}" 2>&1
status=$?
set -e

if [[ ${status} -ne 0 ]]; then
    # Undefined symbols are the expected failure here and they scroll past in a
    # wall of Swift progress, so surface them before the generic tail.
    if grep -q "Undefined symbols" "${BUILD_LOG}"; then
        warn "Undefined symbols at link time:"
        sed -n '/Undefined symbols/,/ld: symbol/p' "${BUILD_LOG}" | head -40 >&2
    fi
    tail -60 "${BUILD_LOG}" >&2
    die "xcodebuild failed with status ${status}. Full log: ${BUILD_LOG}"
fi

APP_PATH="${DERIVED_DATA}/Build/Products/${CONFIGURATION}-iphoneos/BoxedVNFex.app"
[[ -d "${APP_PATH}" ]] || die \
"xcodebuild reported success but '${APP_PATH}' does not exist."

if codesign --display "${APP_PATH}" >/dev/null 2>&1; then
    die "The app carries a code signature; refusing to label it unsigned."
fi

log "Linked binary"
otool -L "${APP_PATH}/BoxedVNFex" | sed 's/^/  /' | head -20
ls -lh "${APP_PATH}/BoxedVNFex" | awk '{printf "  binary %s\n", $5}'

"${BOXEDVN_SCRIPT_DIR}/package-ipa.sh" \
    --app "${APP_PATH}" \
    --output-dir "${OUTPUT_DIR}/artifacts" \
    --name BoxedVNFex.ipa

ok "Unsigned IPA at ${OUTPUT_DIR}/artifacts/BoxedVNFex.ipa"
