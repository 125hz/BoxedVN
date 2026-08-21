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

# Named after the bundle, not after this project: the fex64 branch validates
# BoxedVNFex.app with the same script.
APP_BASENAME="$(basename "${APP_PATH}")"
EXECUTABLE="${APP_PATH}/${APP_BASENAME%.app}"
INFO_PLIST="${APP_PATH}/Info.plist"

log "Validating $(basename "${APP_PATH}")"

# --- executable -------------------------------------------------------------
require_file "${EXECUTABLE}" \
    "The app bundle has no ${APP_BASENAME%.app} executable; the link step produced nothing."

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

# --- __PAGEZERO, when the build asked for a smaller one ---------------------
#
# A 64-bit Mach-O reserves the low 4 GiB as __PAGEZERO unless it is linked with
# -pagezero_size. The fex64 app asks for one page instead, because that low
# range is the entire address space of a 32-bit Windows guest and WoW64 has
# nowhere to put one without it.
#
# Report it either way. A dropped or ignored linker flag looks exactly like an
# iOS kernel refusing the low range once the app is running -- the app's own
# "low address space:" line cannot tell those apart -- and concluding the wrong
# one costs a device cycle and points the next fix at the wrong layer.
pagezero="$(otool -l "${EXECUTABLE}" 2>/dev/null     | awk '/__PAGEZERO/{found=1} found && /vmsize/{print $2; exit}')"
if [[ -z "${pagezero}" ]]; then
    warn "No __PAGEZERO segment found. Unusual, but not fatal on its own."
elif [[ "${pagezero}" == "0x0000000100000000" ]]; then
    # The current, expected state: iOS refused to launch a build linked with a
    # 16 KiB page-zero, so the flag is not passed. Stated rather than warned
    # about, because it is what every build looks like today.
    ok "__PAGEZERO ${pagezero} (stock 4 GiB; 32-bit guests unreachable)"
else
    ok "__PAGEZERO ${pagezero} (reduced; the low range is free for a 32-bit guest)"
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
