#!/usr/bin/env bash
# BoxedVN - build the fex64 application and package it as an unsigned IPA.
#
# Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
#
# Usage:
#   scripts/build-fex64-app.sh [--configuration Release|Debug]
#                              [--output-dir DIR] [--third-party-dir DIR]
#                              [--skip-fex]
#                              [--wine-core-dir DIR]
#                              [--wine-runtime-dir DIR] [--mythic-dir DIR]
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
WINE_CORE_DIR=""
WINE_RUNTIME_DIR=""
WINE_PE_DIR=""
MYTHIC_DIR=""

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
        --wine-core-dir)
            [[ $# -ge 2 ]] || die "--wine-core-dir needs a value"
            WINE_CORE_DIR="$2"; shift 2 ;;
        --wine-runtime-dir)
            [[ $# -ge 2 ]] || die "--wine-runtime-dir needs a value"
            WINE_RUNTIME_DIR="$2"; shift 2 ;;
        --wine-pe-dir)
            [[ $# -ge 2 ]] || die "--wine-pe-dir needs a value"
            WINE_PE_DIR="$2"; shift 2 ;;
        --mythic-dir)
            [[ $# -ge 2 ]] || die "--mythic-dir needs a value"
            MYTHIC_DIR="$2"; shift 2 ;;
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

WINE_ENABLED=0
WINE_RESOURCE_STAGING="${BOXEDVN_ROOT}/ios/fex/runtime"
if [[ -n "${WINE_CORE_DIR}${WINE_RUNTIME_DIR}${MYTHIC_DIR}" ]]; then
    [[ -n "${WINE_CORE_DIR}" && -n "${WINE_RUNTIME_DIR}" && -n "${MYTHIC_DIR}" ]] || die \
"Wine-enabled builds require --wine-core-dir, --wine-runtime-dir and --mythic-dir together."
    WINE_CORE_DIR="$(cd "${WINE_CORE_DIR}" && pwd)"
    WINE_RUNTIME_DIR="$(cd "${WINE_RUNTIME_DIR}" && pwd)"
    MYTHIC_DIR="$(cd "${MYTHIC_DIR}" && pwd)"
    require_file "${WINE_CORE_DIR}/lib/libntdll_unix.a"
    require_file "${WINE_CORE_DIR}/lib/libwineserver.a"
    require_file "${WINE_RUNTIME_DIR}/arm64ec-windows/ntdll.dll"
    require_file "${WINE_RUNTIME_DIR}/arm64ec-windows/fib-x64.exe"
    require_file "${WINE_RUNTIME_DIR}/arm64ec-windows/cube-x64.exe"
    require_file "${WINE_RUNTIME_DIR}/aarch64-windows/child-test.exe"
    # The desktop acceptance target runs this one, natively rather than
    # through translation, so a missing copy should fail the build here.
    require_file "${WINE_RUNTIME_DIR}/aarch64-windows/explorer.exe"
    require_file "${MYTHIC_DIR}/app/Mythic/arm64ec-windows/xtajit64.dll"
    require_file "${MYTHIC_DIR}/app/Mythic/prefix-template.tar.gz"
    [[ -d "${MYTHIC_DIR}/app/Mythic/nls" ]] || die "The pinned iOS integration has no NLS directory."

    # This path is generated only for Xcode's resource phase. Keep the large
    # runtime out of git and remove it when the build exits.
    rm -rf "${WINE_RESOURCE_STAGING}"
    mkdir -p "${WINE_RESOURCE_STAGING}"
    cp -R "${WINE_RUNTIME_DIR}/aarch64-windows" \
          "${WINE_RESOURCE_STAGING}/aarch64-windows"
    cp -R "${WINE_RUNTIME_DIR}/arm64ec-windows" \
          "${WINE_RESOURCE_STAGING}/arm64ec-windows"
    # Two ways to supply the emulator DLL. By default the prebuilt is patched
    # in place on the way into the bundle, because its allocator source was
    # force-pushed out of existence and cannot be rebuilt. Set
    # BOXEDVN_XTAJIT_DLL to stage a source-built one instead; that path needs
    # no patching because the defect is fixed in source, and it is staged
    # verbatim so the binary under test is exactly what CI produced.
    if [[ -n "${BOXEDVN_XTAJIT_DLL:-}" ]]; then
        require_file "${BOXEDVN_XTAJIT_DLL}"
        log "Staging the source-built emulator DLL"
        cp "${BOXEDVN_XTAJIT_DLL}" \
           "${WINE_RESOURCE_STAGING}/arm64ec-windows/xtajit64.dll"
        shasum -a 256 "${WINE_RESOURCE_STAGING}/arm64ec-windows/xtajit64.dll"
    else
        "${PYTHON:-python3}" "${BOXEDVN_ROOT}/scripts/patch-xtajit64.py" \
            "${MYTHIC_DIR}/app/Mythic/arm64ec-windows/xtajit64.dll" \
            "${WINE_RESOURCE_STAGING}/arm64ec-windows/xtajit64.dll" \
            || die "Patching the ARM64EC emulator DLL failed."
    fi
    # The integration is a game launcher, so its ARM64EC runtime ships a full
    # DLL set and the display driver but none of Wine's own programs. A desktop
    # needs explorer, and it has to be ARM64EC: the display driver is built
    # only for that architecture, so an ARM64 explorer can never load it while
    # an ARM64EC one runs beside the same wineios.drv the x64 path already
    # uses. Wine is configured here with --enable-archs=arm64ec,aarch64, so
    # that build already produced one; copy it in alongside rather than
    # replacing anything.
    # Wine builds every program for the native architecture only -- explorer,
    # cmd, wineboot are all aarch64 even when --enable-archs names arm64ec too,
    # because ARM64EC exists for DLLs that x64 code has to call, which Wine's
    # own programs are not. So a desktop cannot run in the ARM64EC world: it
    # has the display driver but will never have a program.
    #
    # It can run in the aarch64 world, which has explorer and a full DLL set
    # and lacks only a driver, because the integration ships wineios.drv built
    # for ARM64EC alone. Wine builds DLLs for both architectures, so stage the
    # aarch64 form of the driver next to the programs that can load it.
    if [[ -n "${WINE_PE_DIR}" ]]; then
        driver="$(find "${WINE_PE_DIR}" -name "wineios.drv" \
                  -path "*/aarch64-windows/*" -print -quit 2>/dev/null || true)"
        if [[ -n "${driver}" && -f "${driver}" ]]; then
            cp "${driver}" "${WINE_RESOURCE_STAGING}/aarch64-windows/wineios.drv"
            log "Staged the aarch64 wineios.drv from ${driver}"
        else
            warn "No aarch64 wineios.drv under ${WINE_PE_DIR}; the desktop has no display driver."
            warn "  wineios built, any architecture:"
            find "${WINE_PE_DIR}" -name "wineios.drv" 2>/dev/null \
                | head -10 | sed "s/^/    /" >&2 || true
        fi
    fi
    cp -R "${MYTHIC_DIR}/app/Mythic/nls" "${WINE_RESOURCE_STAGING}/nls"
    mkdir -p "${WINE_RESOURCE_STAGING}/prefix-template"
    tar -xzf "${MYTHIC_DIR}/app/Mythic/prefix-template.tar.gz" \
        -C "${WINE_RESOURCE_STAGING}/prefix-template" --strip-components=1
    trap 'rm -rf "${WINE_RESOURCE_STAGING}"' EXIT
    WINE_ENABLED=1
    log "Staged the native and ARM64EC Wine runtime resources"
fi

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
XCODE_WINE_SETTINGS=()
if [[ "${WINE_ENABLED}" -eq 1 ]]; then
    XCODE_WINE_SETTINGS+=(
        "BVN_WINE_LIBRARY_DIR=${WINE_CORE_DIR}/lib"
        "BVN_WINE_LDFLAGS=-lntdll_unix -lwineserver"
        'GCC_PREPROCESSOR_DEFINITIONS=$(inherited) BVN_WINE_BOOT_ENABLED=1'
    )
fi
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
    "${XCODE_WINE_SETTINGS[@]}" \
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
