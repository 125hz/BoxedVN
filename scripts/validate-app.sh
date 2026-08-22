#!/usr/bin/env bash
# BoxedVN - validate a built .app bundle.
#
# Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
#
# Usage: scripts/validate-app.sh PATH/TO/BoxedVN.app
#
# Checks the things that are cheap to get wrong and expensive to discover on
# the device: wrong architecture, a macOS binary that happens to have built, a
# malformed Info.plist, or a missing executable.

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

[[ $# -eq 1 ]] || die "usage: $0 PATH/TO/BoxedVN.app"
APP_PATH="$1"

[[ -d "${APP_PATH}" ]] || die "'${APP_PATH}' is not a directory."

EXECUTABLE="${APP_PATH}/BoxedVN"
INFO_PLIST="${APP_PATH}/Info.plist"

log "Validating $(basename "${APP_PATH}")"

# --- executable -------------------------------------------------------------
require_file "${EXECUTABLE}" \
    "The app bundle has no BoxedVN executable; the link step produced nothing."

if [[ ! -x "${EXECUTABLE}" ]]; then
    die "'${EXECUTABLE}' is not executable."
fi

architectures="$(lipo -archs "${EXECUTABLE}" 2>/dev/null || true)"
if [[ "${architectures}" != *arm64* ]]; then
    die "The executable has no arm64 slice (found: '${architectures}').
BoxedVN only supports physical ARM64 devices."
fi
ok "architectures: ${architectures}"

# --- built for iOS, not macOS ----------------------------------------------
# LC_BUILD_VERSION platform 2 is IOS; 1 is MACOS, 7 is IOSSIMULATOR.
platform="$(otool -l "${EXECUTABLE}" \
    | awk '/LC_BUILD_VERSION/{found=1} found && /platform/{print $2; exit}')"
case "${platform}" in
    2)
        ok "platform: iOS (LC_BUILD_VERSION platform 2)"
        ;;
    7)
        die "The executable was built for the iOS Simulator (platform 7), not a
device. Check the -destination and SUPPORTED_PLATFORMS settings."
        ;;
    1)
        die "The executable was built for macOS (platform 1), not iOS.
Check CMAKE_SYSTEM_NAME and the xcodebuild destination."
        ;;
    *)
        die "Could not determine the target platform from LC_BUILD_VERSION
(got '${platform}'). Refusing to package something unidentified."
        ;;
esac

minimum="$(otool -l "${EXECUTABLE}" \
    | awk '/LC_BUILD_VERSION/{found=1} found && /minos/{print $2; exit}')"
ok "minimum iOS version: ${minimum:-unknown}"

# DXMT locates the host display table with dlsym(RTLD_DEFAULT, ...). A source
# declaration is not evidence that Release linking kept it in the export trie,
# so reject an IPA that could execute D3D11 but has nowhere to present it.
exports="$(xcrun dyld_info -exports "${EXECUTABLE}" 2>/dev/null || true)"
if [[ "${exports}" != *"macdrv_functions"* ]]; then
    die "The app does not export macdrv_functions. DXMT resolves this table by
name at runtime; check BVNDXMTDisplay.m and the force-link setting."
fi
ok "DXMT host display table exported"

# The host-call dispatcher references this table directly; it does not resolve
# it with dlsym. Verify retention in the unstripped Mach-O symbol table rather
# than requiring an unnecessary dynamic export.
has_dxmt_unix_table=0
if xcrun nm "${EXECUTABLE}" 2>/dev/null | awk '
    $NF == "_dxmt_winemetal_unix_call_funcs" { found = 1 }
    END { exit(found ? 0 : 1) }
'; then
    has_dxmt_unix_table=1
fi
if [[ "${BOXEDVN_REQUIRE_DXMT_NATIVE:-0}" == "1" && \
      ${has_dxmt_unix_table} -ne 1 ]]; then
    die "The x86-64 build does not retain dxmt_winemetal_unix_call_funcs.
Check the native DXMT archive and its force-link setting."
fi
if [[ ${has_dxmt_unix_table} -eq 1 ]]; then
    ok "DXMT native unix-call table retained"
elif [[ "${BOXEDVN_REQUIRE_DXMT_NATIVE:-0}" != "1" ]]; then
    warn "No native DXMT unix-call table in this non-x86-64 build"
fi

# --- Info.plist -------------------------------------------------------------
require_file "${INFO_PLIST}" "The app bundle has no Info.plist."

if ! plutil -lint "${INFO_PLIST}" >/dev/null 2>&1; then
    plutil -lint "${INFO_PLIST}" >&2 || true
    die "Info.plist is malformed."
fi

for key in CFBundleIdentifier CFBundleExecutable CFBundleName \
           CFBundleShortVersionString CFBundleVersion MinimumOSVersion; do
    value="$(/usr/libexec/PlistBuddy -c "Print :${key}" "${INFO_PLIST}" 2>/dev/null || true)"
    if [[ -z "${value}" ]]; then
        die "Info.plist is missing the required key '${key}'."
    fi
    printf '  %-28s %s\n' "${key}" "${value}"
done

bundle_executable="$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "${INFO_PLIST}")"
if [[ ! -f "${APP_PATH}/${bundle_executable}" ]]; then
    die "Info.plist names CFBundleExecutable '${bundle_executable}', but no such
file exists in the bundle."
fi

# --- root filesystem, when the build was told to bundle one -----------------
if [[ "${BOXEDVN_EXPECT_ROOTFS:-0}" == "1" ]]; then
    if ! find "${APP_PATH}" -name 'boxedwine.zip' -print -quit | grep -q .; then
        die "BOXEDVN_EXPECT_ROOTFS=1 but no boxedwine.zip is in the bundle.
Run scripts/fetch-rootfs.sh before building."
    fi
    ok "bundled root filesystem present"
else
    if find "${APP_PATH}" -name 'boxedwine.zip' -print -quit | grep -q .; then
        ok "bundled root filesystem present"
    else
        warn "No root filesystem is bundled. The app will ask the user to import
one; see scripts/fetch-rootfs.sh."
    fi
fi

printf '  %-28s %s\n' "bundle size" \
    "$(du -sh "${APP_PATH}" | awk '{print $1}')"

ok "validation passed"
