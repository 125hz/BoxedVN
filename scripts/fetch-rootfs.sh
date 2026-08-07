#!/usr/bin/env bash
# BoxedVN - download a pinned Boxedwine root filesystem.
#
# Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
#
# Usage:
#   scripts/fetch-rootfs.sh [--id wine10|wine11] [--bundle] [--output-dir DIR]
#   scripts/fetch-rootfs.sh --list
#
# Boxedwine runs a real 32-bit Wine inside an emulated Linux kernel and cannot
# run anything without a root filesystem.  BoxedVN does not redistribute one;
# this downloads an upstream Boxedwine build pinned by exact URL and SHA-256 in
# scripts/dependencies.lock.sh.  A checksum mismatch is fatal.
#
# --bundle additionally copies the archive into ios/app/Bundled/ so that
# scripts/build-ios.sh puts it inside the .app.  That makes the IPA several
# hundred megabytes.  Without --bundle the archive stays under third_party/ and
# is imported on the device through Settings -> Import root filesystem ZIP.
#
# The contents of these archives have not been reviewed for redistribution.
# Do not ship a bundled root filesystem in a public release until they have.

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
source "${BOXEDVN_SCRIPT_DIR}/dependencies.lock.sh"

ROOTFS_ID="${BOXEDVN_ROOTFS_DEFAULT}"
BUNDLE=0
OUTPUT_DIR=""
LIST=0

usage() {
    sed -n '2,22p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --id)         [[ $# -ge 2 ]] || die "--id needs a value"
                      ROOTFS_ID="$2"; shift 2 ;;
        --bundle)     BUNDLE=1; shift ;;
        --output-dir) [[ $# -ge 2 ]] || die "--output-dir needs a value"
                      OUTPUT_DIR="$2"; shift 2 ;;
        --list)       LIST=1; shift ;;
        -h|--help)    usage; exit 0 ;;
        *)            die "Unknown argument '$1'. Run with --help." ;;
    esac
done

# ---------------------------------------------------------------------------
# Catalogue
# ---------------------------------------------------------------------------
lookup_entry() {
    local wanted="$1"
    local entry
    for entry in "${BOXEDVN_ROOTFS_ENTRIES[@]}"; do
        if [[ "${entry%%|*}" == "${wanted}" ]]; then
            printf '%s' "${entry}"
            return 0
        fi
    done
    return 1
}

if [[ ${LIST} -eq 1 ]]; then
    printf 'Pinned root filesystems:\n\n'
    for entry in "${BOXEDVN_ROOTFS_ENTRIES[@]}"; do
        IFS='|' read -r id url sha256 bytes description <<<"${entry}"
        marker=" "
        [[ "${id}" == "${BOXEDVN_ROOTFS_DEFAULT}" ]] && marker="*"
        printf '%s %-8s %6s MB  %s\n' "${marker}" "${id}" \
            "$(( bytes / 1024 / 1024 ))" "${description}"
        printf '           url    %s\n' "${url}"
        printf '           sha256 %s\n\n' "${sha256}"
    done
    printf '* = default\n'
    exit 0
fi

entry="$(lookup_entry "${ROOTFS_ID}")" || {
    available="$(printf '%s\n' "${BOXEDVN_ROOTFS_ENTRIES[@]}" | cut -d'|' -f1 | tr '\n' ' ')"
    die "Unknown root filesystem id '${ROOTFS_ID}'.
Available: ${available}
Run '$0 --list' for details."
}

IFS='|' read -r id url sha256 expected_bytes description <<<"${entry}"

require_command curl
require_command shasum

print_tool_versions

OUTPUT_DIR="${OUTPUT_DIR:-${BOXEDVN_THIRD_PARTY}/rootfs}"
mkdir -p "${OUTPUT_DIR}"
ARCHIVE="${OUTPUT_DIR}/${id}.zip"

log "Root filesystem: ${description}"
printf '  id       : %s\n' "${id}"
printf '  size     : %s MB\n' "$(( expected_bytes / 1024 / 1024 ))"
printf '  url      : %s\n' "${url}"
printf '  sha256   : %s\n' "${sha256}"
printf '  target   : %s\n\n' "${ARCHIVE}"

download_pinned "${url}" "${ARCHIVE}" "${sha256}"

actual_bytes="$(stat -f%z "${ARCHIVE}" 2>/dev/null || stat -c%s "${ARCHIVE}")"
if [[ "${actual_bytes}" != "${expected_bytes}" ]]; then
    die "'${ARCHIVE}' is ${actual_bytes} bytes but the pin says ${expected_bytes}.
The checksum matched, so this is a bug in the pin, not a bad download."
fi

# A root filesystem that is not a readable ZIP would fail much later, inside
# the emulator, with a far worse error message.
if command -v unzip >/dev/null 2>&1; then
    if ! unzip -tqq "${ARCHIVE}" >/dev/null 2>&1; then
        die "'${ARCHIVE}' passed its checksum but is not a readable ZIP archive."
    fi
    ok "archive integrity verified"
fi

# ---------------------------------------------------------------------------
# Optional: place it in the app bundle
# ---------------------------------------------------------------------------
BUNDLED_DIR="${BOXEDVN_ROOT}/ios/app/Bundled"

if [[ ${BUNDLE} -eq 1 ]]; then
    log "Copying into the app bundle sources"
    mkdir -p "${BUNDLED_DIR}"
    # The runtime looks for a resource named boxedwine.zip; see
    # BVNPathBundledRootFilesystemZip in ios/runtime/src/BVNPaths.mm.
    cp "${ARCHIVE}" "${BUNDLED_DIR}/boxedwine.zip"
    ok "ios/app/Bundled/boxedwine.zip ($(du -h "${BUNDLED_DIR}/boxedwine.zip" | awk '{print $1}'))"
    printf '\n'
    warn "The IPA will now be several hundred megabytes.
The contents of this archive have not been reviewed for redistribution; do not
publish a release built this way. See docs/BUILD_IOS.md."
    printf '\n'
    printf 'Build with:\n'
    printf '  BOXEDVN_EXPECT_ROOTFS=1 ./scripts/build-ios.sh\n'
else
    printf '\n'
    log "Not bundled (default)"
    printf 'Copy this file to the device through Files, then use\n'
    printf '  Settings -> Import root filesystem ZIP...\n'
    printf 'inside BoxedVN:\n'
    printf '  %s\n' "${ARCHIVE}"
    printf '\nOr re-run with --bundle to build it into the app.\n'
fi
