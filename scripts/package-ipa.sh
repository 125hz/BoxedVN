#!/usr/bin/env bash
# BoxedVN - package a built .app into an unsigned IPA.
#
# Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
#
# Usage:
#   scripts/package-ipa.sh --app PATH/TO/BoxedVN.app [--output-dir DIR]
#                          [--name BoxedVN-unsigned.ipa]
#
# Produces Payload/BoxedVN.app inside a ZIP named *.ipa, plus a SHA-256
# checksum, and then unzips it again as a smoke test.  The result is meant to
# be signed afterwards by SideStore, Sideloadly, AltStore or similar.

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

APP_PATH=""
OUTPUT_DIR=""
IPA_NAME="BoxedVN-unsigned.ipa"

usage() {
    sed -n '2,13p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --app)        [[ $# -ge 2 ]] || die "--app needs a value"
                      APP_PATH="$2"; shift 2 ;;
        --output-dir) [[ $# -ge 2 ]] || die "--output-dir needs a value"
                      OUTPUT_DIR="$2"; shift 2 ;;
        --name)       [[ $# -ge 2 ]] || die "--name needs a value"
                      IPA_NAME="$2"; shift 2 ;;
        -h|--help)    usage; exit 0 ;;
        *)            die "Unknown argument '$1'. Run with --help." ;;
    esac
done

[[ -n "${APP_PATH}" ]] || die "--app is required. Run with --help."
[[ -d "${APP_PATH}" ]] || die "'${APP_PATH}' is not a directory."

OUTPUT_DIR="${OUTPUT_DIR:-$(dirname "${APP_PATH}")}"
mkdir -p "${OUTPUT_DIR}"
OUTPUT_DIR="$(cd "${OUTPUT_DIR}" && pwd)"

require_macos
require_command ditto
require_command shasum
require_command unzip
require_command codesign

# Never package something that failed validation.
"${BOXEDVN_SCRIPT_DIR}/validate-app.sh" "${APP_PATH}"

STAGING="$(mktemp -d "${TMPDIR:-/tmp}/boxedvn-ipa.XXXXXX")"
trap 'rm -rf "${STAGING}"' EXIT

log "Staging Payload/$(basename "${APP_PATH}")"
mkdir -p "${STAGING}/Payload"
# ditto rather than cp: it preserves symlinks, resource forks and permissions
# the way the App Store tooling expects.
ditto "${APP_PATH}" "${STAGING}/Payload/$(basename "${APP_PATH}")"

IPA_PATH="${OUTPUT_DIR}/${IPA_NAME}"
rm -f "${IPA_PATH}" "${IPA_PATH}.sha256"

log "Creating ${IPA_NAME}"
# --keepParent is what puts the Payload/ prefix inside the archive, which is
# exactly what makes a ZIP an IPA.
(
    cd "${STAGING}"
    ditto -c -k --sequesterRsrc --keepParent "Payload" "${IPA_PATH}"
) || die "Could not create the IPA."

[[ -f "${IPA_PATH}" ]] || die "The IPA was not created at '${IPA_PATH}'."

# ---------------------------------------------------------------------------
# Checksum
# ---------------------------------------------------------------------------
(
    cd "${OUTPUT_DIR}"
    shasum -a 256 "${IPA_NAME}" > "${IPA_NAME}.sha256"
)
ok "SHA-256: $(awk '{print $1}' "${IPA_PATH}.sha256")"

# ---------------------------------------------------------------------------
# Smoke test: unzip it again and confirm the bundle survived
# ---------------------------------------------------------------------------
log "Smoke testing the IPA"
SMOKE="$(mktemp -d "${TMPDIR:-/tmp}/boxedvn-smoke.XXXXXX")"
trap 'rm -rf "${STAGING}" "${SMOKE}"' EXIT

unzip -q "${IPA_PATH}" -d "${SMOKE}" \
    || die "The produced IPA could not be unzipped."

APP_NAME="$(basename "${APP_PATH}")"
UNPACKED="${SMOKE}/Payload/${APP_NAME}"

[[ -d "${UNPACKED}" ]] \
    || die "The IPA does not contain Payload/${APP_NAME}.
Contents:
$(cd "${SMOKE}" && find . -maxdepth 2 | head -20)"

# The executable is named after the bundle, not after this project: the fex64
# branch packages BoxedVNFex.app with the same script. Read it from the bundle
# rather than assuming, so this check keeps testing what it is for - that the
# archive round trip preserved an executable - instead of the product name.
EXECUTABLE_NAME="${APP_NAME%.app}"
[[ -f "${UNPACKED}/${EXECUTABLE_NAME}" ]] \
    || die "The unpacked bundle has no ${EXECUTABLE_NAME} executable.
Contents:
$(cd "${UNPACKED}" && find . -maxdepth 1 | head -20)"

[[ -f "${UNPACKED}/Info.plist" ]] \
    || die "The unpacked bundle has no Info.plist."

# Re-validate the unpacked copy: this catches anything the archive step
# mangled, which a checksum of the archive cannot.
"${BOXEDVN_SCRIPT_DIR}/validate-app.sh" "${UNPACKED}" >/dev/null \
    || die "The unpacked bundle failed validation."

ok "smoke test passed"

SIGNING_DESCRIPTION="unsigned"
if codesign --display "${UNPACKED}" >/dev/null 2>&1; then
    SIGNING_DESCRIPTION="ad-hoc entitlement template"
fi

printf '\n'
log "Packaged"
printf '  ipa      : %s (%s)\n' "${IPA_PATH}" \
    "$(du -h "${IPA_PATH}" | awk '{print $1}')"
printf '  checksum : %s\n' "${IPA_PATH}.sha256"
printf '  signing  : %s\n' "${SIGNING_DESCRIPTION}"
printf '\n'
if [[ "${SIGNING_DESCRIPTION}" == "unsigned" ]]; then
    printf 'This IPA is unsigned. Sign it with SideStore, Sideloadly, AltStore or\n'
    printf 'an equivalent tool before installing.\n'
else
    printf 'This IPA has only an ad-hoc entitlement-template signature. Replace it\n'
    printf 'with a provisioning-profile-backed signature using your sideloader.\n'
fi
printf 'Signing does NOT enable JIT:\n'
printf 'on iOS 26/27, assign StikDebug universal.js to BoxedVN, launch through\n'
printf 'StikDebug, and keep the script active while the guest runs.\n'
