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
#                              [--wine-runtime-dir DIR] [--wine-pe-dir DIR]
#                              [--mythic-dir DIR]
#                              [--dxmt-lib PATH] [--win32u-lib PATH]
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
DXMT_LIB=""
WIN32U_LIB=""

usage() {
    sed -n '2,22p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
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
        --dxmt-lib)
            [[ $# -ge 2 ]] || die "--dxmt-lib needs a value"
            DXMT_LIB="$2"; shift 2 ;;
        --win32u-lib)
            [[ $# -ge 2 ]] || die "--win32u-lib needs a value"
            WIN32U_LIB="$2"; shift 2 ;;
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

    # The integration's prebuilt runtime is intentionally smaller than a full
    # Wine installation. Add the compatibility DLLs that ordinary Windows
    # programs may import before their entry point runs from the pinned Wine
    # tree the combined build has already produced. Keep this list explicit:
    # copying every built DLL would substantially grow the IPA and could
    # replace integration-tested binaries with a different build wholesale.
    if [[ -n "${WINE_PE_DIR}" ]]; then
        [[ -d "${WINE_PE_DIR}" ]] || die \
            "The Wine PE build directory '${WINE_PE_DIR}' does not exist."
        WINE_PE_DIR="$(cd "${WINE_PE_DIR}" && pwd)"
        for runtime_dll in xinput1_4.dll xinput9_1_0.dll; do
            runtime_source="${WINE_PE_DIR}/${runtime_dll}"
            [[ -f "${runtime_source}" ]] || die \
"The x86-64 Wine compatibility build at '${WINE_PE_DIR}' did not produce ${runtime_dll}.
The bundled runtime needs this system compatibility DLL before programs that
import it can enter their main function."
            cp "${runtime_source}" \
               "${WINE_RESOURCE_STAGING}/arm64ec-windows/${runtime_dll}"
            log "Staged x86-64 Wine compatibility DLL ${runtime_dll}"
        done
    fi

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

    # The 32-bit guest side, in two halves that are staged independently and
    # checked together by the app at start.
    #
    # Both are optional here on purpose. A build that carries neither is every
    # build before this one and runs x86-64 exactly as it did; a build that
    # carries one and not the other is a mistake worth naming, but not one
    # worth refusing an otherwise working IPA over, so the app reports which
    # half is missing rather than the packaging step failing here.
    #
    # xtajit.dll is FEX's libwow64fex.dll under the name the loader asks for,
    # the same substitution that stages libarm64ecfex.dll as xtajit64.dll. It
    # goes into the ARM64EC set because that is what becomes system32, and the
    # CPU backend is loaded by the 64-bit loader rather than by the 32-bit
    # world it serves. The i386 DLLs go into their own directory, which the
    # app links into syswow64: their names collide with the 64-bit set by
    # design, and mixing the two would break both.
    if [[ -n "${BOXEDVN_WOW64_DLL:-}" ]]; then
        require_file "${BOXEDVN_WOW64_DLL}"
        log "Staging the WoW64 emulator DLL as xtajit.dll"
        cp "${BOXEDVN_WOW64_DLL}" \
           "${WINE_RESOURCE_STAGING}/arm64ec-windows/xtajit.dll"
        shasum -a 256 "${WINE_RESOURCE_STAGING}/arm64ec-windows/xtajit.dll"
    fi
    if [[ -d "${WINE_RUNTIME_DIR}/i386-windows" ]]; then
        cp -R "${WINE_RUNTIME_DIR}/i386-windows" \
              "${WINE_RESOURCE_STAGING}/i386-windows"
        i386_staged="$(find "${WINE_RESOURCE_STAGING}/i386-windows" -type f | wc -l | tr -d ' ')"
        log "Staged ${i386_staged} i386 PE files for the 32-bit guest side"
    fi
    # The integration is a game launcher, so its ARM64EC runtime ships a full
    # DLL set and the display driver but none of Wine's own programs. A desktop
    # needs explorer, and it has to be ARM64EC: the display driver is built
    # only for that architecture, so an ARM64 explorer can never load it while
    # an ARM64EC one runs beside the same wineios.drv the x64 path already
    # uses. Wine is configured here with --enable-archs=arm64ec,aarch64,i386,
    # so that build already produced one; copy it in alongside rather than
    # replacing anything.
    # No display driver is staged, because none exists for iOS. wineios.drv is
    # the CoreAudio/CoreMIDI driver, not a display one; the fork carries only
    # the usual winex11, winemac, winewayland and wineandroid, none of which
    # run here. The integration reaches the screen through DXMT instead, whose
    # unix side resolves macdrv_* to a CAMetalLayer the app owns -- a surface
    # for D3D11 output, not a window manager. It is a game launcher and never
    # needed a desktop.
    #
    # So the desktop target loads Wine's window stack and then stops for want
    # of a driver. Reaching a visible desktop means writing one, or teaching
    # winemac.drv to speak UIKit instead of Cocoa.
    cp -R "${MYTHIC_DIR}/app/Mythic/nls" "${WINE_RESOURCE_STAGING}/nls"
    mkdir -p "${WINE_RESOURCE_STAGING}/prefix-template"
    tar -xzf "${MYTHIC_DIR}/app/Mythic/prefix-template.tar.gz" \
        -C "${WINE_RESOURCE_STAGING}/prefix-template" --strip-components=1

    # Point the x86 CPU backend at the emulator that can actually run here.
    #
    # Wine asks Software\Microsoft\Wow64\x86 which DLL executes a 32-bit
    # guest's instructions, and the pinned template answers "wow64cpu.dll" --
    # correct for an x86-64 host, where 32-bit code runs on the hardware and
    # the backend only has to switch modes. On ARM64 there is nothing to
    # switch to, so the answer has to name an emulator: xtajit.dll, which is
    # where libwow64fex.dll was staged above. The template already lists both
    # xtajit and xtajit64 in KnownDLLs, so the name is the one Wine expects;
    # only this value was left at the x86-64 default.
    #
    # Rewritten only when the backend is actually present. A prefix naming a
    # DLL that was never staged fails further from the cause than one still
    # naming the x86-64 default, and the app checks for the file itself
    # before it lets a 32-bit program start.
    if [[ -f "${WINE_RESOURCE_STAGING}/arm64ec-windows/xtajit.dll" ]]; then
        registry="${WINE_RESOURCE_STAGING}/prefix-template/system.reg"
        require_file "${registry}"
        # Scoped to the one key: "wow64cpu.dll" also appears in KnownDLLs,
        # where it is a name-to-file mapping rather than a choice of backend
        # and has to stay as it is.
        #
        # The key is matched with . rather than by spelling its separators.
        # Registry paths in system.reg are written with doubled backslashes,
        # and how many a backslash has to become to survive both awk's regex
        # lexer and the shell quoting around it is exactly the kind of detail
        # that silently matches nothing -- which here would mean shipping a
        # prefix that still names the x86-64 backend. The anchors and the
        # trailing "] " keep it precise without depending on that count.
        awk '
            /^\[Software.*Microsoft.*Wow64.*x86\] / { inkey = 1 }
            inkey && /^@="wow64cpu\.dll"$/ { print "@=\"xtajit.dll\""; inkey = 0; next }
            /^\[/ && !/^\[Software.*Microsoft.*Wow64.*x86\] / { inkey = 0 }
            { print }
        ' "${registry}" > "${registry}.wow64" || die "Rewriting the WoW64 CPU backend failed."
        grep -q '^@="xtajit\.dll"$' "${registry}.wow64" || die \
"The prefix template's Software\Microsoft\Wow64\x86 key did not carry the
expected wow64cpu.dll default, so the CPU backend was not redirected. The
pinned template has changed shape; re-read it before assuming 32-bit works."
        mv "${registry}.wow64" "${registry}"
        log "Prefix template: x86 CPU backend set to xtajit.dll"
    fi
    trap 'rm -rf "${WINE_RESOURCE_STAGING}"' EXIT
    WINE_ENABLED=1
    log "Staged the native and ARM64EC Wine runtime resources"
fi

# DXMT is only reachable through Wine, so an archive supplied without a Wine
# runtime would link in dead weight and quietly drop the stub that the rest of
# the build still needs.
if [[ -n "${DXMT_LIB}" ]]; then
    [[ "${WINE_ENABLED}" -eq 1 ]] || die "--dxmt-lib needs a Wine-enabled build; pass --wine-core-dir, --wine-runtime-dir and --mythic-dir too."
    require_file "${DXMT_LIB}" "Build it with scripts/build-fex64-dxmt.sh."
    DXMT_LIB="$(cd "$(dirname "${DXMT_LIB}")" && pwd)/$(basename "${DXMT_LIB}")"
fi

# win32u's unix side and the wineserver both define a handful of the same
# global names, and both are linked into this one binary because the server
# runs in-process. On a static link the two land in a single namespace and the
# linker silently picks one, so a win32u call site can end up in server code
# with unrelated arguments. That is not hypothetical: get_virtual_screen_rect
# resolved to server/window.c's three-argument version and the first NtGdi
# syscall the guest ever made faulted reading 0x48 off a dpi value of zero.
#
# scripts/build-fex64-win32u.sh renames the ten that exist today. This re-
# derives the set from the archives actually being linked and fails if it has
# grown, because the failure mode of missing one is a wild jump on some later
# device run rather than anything visible here.
#
# Read to files and compared with comm rather than piped into grep: these
# objects are compiled -fvisibility=hidden, so the definitions are Mach-O
# private externs that plain `nm` prints and `nm -g` does not.
check_win32u_server_collisions() {
    local work="${OUTPUT_DIR}/symbol-collisions"
    rm -rf "${work}"
    mkdir -p "${work}"

    local nm_ok=1

    # `nm -m` rather than plain `nm` because the three spellings have to be
    # told apart. A file-static and a hidden-visibility global are both
    # "lowercase t" to plain nm, but only the second one can collide across
    # translation units - and these objects are built -fvisibility=hidden, so
    # nearly everything is the second one. `nm -m` spells it out:
    #
    #   (__TEXT,__text) external _foo
    #   (__TEXT,__text) non-external (was a private external) _foo
    #   (__TEXT,__text) non-external _foo                      <- file static
    #
    # Take the first two, drop the third, and drop (undefined) entirely.
    defined_globals() {
        xcrun -sdk iphoneos nm -m "$1" 2>/dev/null | awk '
            /\(undefined\)/                   { next }
            /was a private external/          { print $NF; next }
            !/non-external/ && / external /   { print $NF }
        ' | sed 's/^_//' | sort -u
    }

    defined_globals "${WIN32U_LIB}" > "${work}/win32u.txt" || nm_ok=0
    defined_globals "${WINE_CORE_DIR}/lib/libwineserver.a" > "${work}/server.txt" || nm_ok=0

    if [[ "${nm_ok}" -eq 0 || ! -s "${work}/win32u.txt" || ! -s "${work}/server.txt" ]]; then
        warn "symbol-collision check skipped: could not read both symbol tables"
        return 0
    fi

    comm -12 "${work}/win32u.txt" "${work}/server.txt" > "${work}/shared.txt"

    # Anything already renamed carries the win32u_ prefix and cannot collide.
    local count
    count="$(grep -c . "${work}/shared.txt" || true)"
    if [[ "${count}" -gt 0 ]]; then
        warn "win32u and the wineserver both define ${count} symbol(s):"
        sed 's/^/    /' "${work}/shared.txt" >&2
        die "win32u/wineserver symbol collision.

Add each name above to WIN32U_COLLISIONS in scripts/build-fex64-win32u.sh, which
renames win32u's copy at compile time. Leaving one in place lets the linker bind
a win32u call to the server function of the same name, which does not fail here
- it fails on device, as a wild jump, whenever that call site is first reached."
    fi

    ok "win32u shares no global names with the wineserver"
}

# The window subsystem is reachable only through Wine for the same reason.
if [[ -n "${WIN32U_LIB}" ]]; then
    [[ "${WINE_ENABLED}" -eq 1 ]] || die "--win32u-lib needs a Wine-enabled build."
    require_file "${WIN32U_LIB}" "Build it with scripts/build-fex64-win32u.sh."
    WIN32U_LIB="$(cd "$(dirname "${WIN32U_LIB}")" && pwd)/$(basename "${WIN32U_LIB}")"
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
    wine_definitions='$(inherited) BVN_WINE_BOOT_ENABLED=1'
    dxmt_ldflags=""
    if [[ -n "${DXMT_LIB}" ]]; then
        # Linked by absolute path rather than -l: the archive lives outside
        # every library search path and carries LLVM inside it, so there is
        # nothing to resolve by name. Defining BVN_WINE_DXMT_ENABLED is what
        # removes the null unix-call stub in BVNWineRuntimeStubs.c; the two
        # must move together or the stub silently wins the link.
        dxmt_ldflags="${DXMT_LIB}"
        wine_definitions="${wine_definitions} BVN_WINE_DXMT_ENABLED=1"
        log "Linking DXMT's unix side ($(wc -c < "${DXMT_LIB}" | tr -d ' ') bytes)"
    else
        warn "no --dxmt-lib: the graphics target will branch to zero on its first unix call, because winemetal.dll resolves through the null stub table"
    fi
    if [[ -n "${WIN32U_LIB}" ]]; then
        # Same pairing rule as the graphics archive: the define is what
        # removes the stub, so it travels with the library replacing it.
        dxmt_ldflags="${dxmt_ldflags} ${WIN32U_LIB}"
        wine_definitions="${wine_definitions} BVN_WINE_WIN32U_ENABLED=1"
        log "Linking win32u's unix side ($(wc -c < "${WIN32U_LIB}" | tr -d ' ') bytes)"
        check_win32u_server_collisions
    else
        warn "no --win32u-lib: the window subsystem keeps answering STATUS_NOT_SUPPORTED, so a guest message loop gets a zeroed MSG"
    fi
    XCODE_WINE_SETTINGS+=(
        "BVN_WINE_LIBRARY_DIR=${WINE_CORE_DIR}/lib"
        "BVN_WINE_LDFLAGS=-lntdll_unix -lwineserver"
        "BVN_DXMT_LDFLAGS=${dxmt_ldflags}"
        "GCC_PREPROCESSOR_DEFINITIONS=${wine_definitions}"
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

# DXMT finds the display shim with dlsym(RTLD_DEFAULT, "macdrv_functions"),
# which the linker cannot see, so under -dead_strip the symbol is a candidate
# for removal however it is declared. The source marks it used, but that is an
# assumption about the toolchain rather than a fact about this binary, and when
# it fails DXMT reports only that Wine lacks the symbols it needs and shows a
# white screen. Verify by content instead.
if ! xcrun dyld_info -exports "${APP_PATH}/BoxedVNFex" 2>/dev/null \
        | grep -q "macdrv_functions"; then
    die "The built app does not export macdrv_functions.

DXMT resolves the display shim by name at runtime, so without it every D3D11
swapchain fails and the graphics target renders nothing. Check that
IOSDisplayShim.m is in the target and that its __attribute__((used)) survived
the link."
fi
ok "macdrv_functions is exported"

"${BOXEDVN_SCRIPT_DIR}/package-ipa.sh" \
    --app "${APP_PATH}" \
    --output-dir "${OUTPUT_DIR}/artifacts" \
    --name BoxedVNFex.ipa

ok "Unsigned IPA at ${OUTPUT_DIR}/artifacts/BoxedVNFex.ipa"
