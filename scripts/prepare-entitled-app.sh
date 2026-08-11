#!/usr/bin/env bash
# Add an ad-hoc signature carrying BoxedVN's requested entitlements to a copy
# of an unsigned .app. The result is still not installable as-is: the user's
# sideloader must replace this signature with one backed by their provisioning
# profile, after GetMoreRAM has enabled the App ID capability.

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

APP_PATH=""
OUTPUT_PATH=""
ENTITLEMENTS="${BOXEDVN_ROOT}/ios/app/BoxedVN.entitlements"

usage() {
    printf '%s\n' \
        "usage: scripts/prepare-entitled-app.sh --app PATH --output PATH" \
        "       [--entitlements PATH]"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --app)          [[ $# -ge 2 ]] || die "--app needs a value"
                        APP_PATH="$2"; shift 2 ;;
        --output)       [[ $# -ge 2 ]] || die "--output needs a value"
                        OUTPUT_PATH="$2"; shift 2 ;;
        --entitlements) [[ $# -ge 2 ]] || die "--entitlements needs a value"
                        ENTITLEMENTS="$2"; shift 2 ;;
        -h|--help)      usage; exit 0 ;;
        *)              die "Unknown argument '$1'. Run with --help." ;;
    esac
done

[[ -n "${APP_PATH}" ]] || die "--app is required."
[[ -n "${OUTPUT_PATH}" ]] || die "--output is required."
require_macos
require_command codesign
require_command ditto
require_command plutil
require_file "${ENTITLEMENTS}" "BoxedVN's entitlement request is missing."
[[ -d "${APP_PATH}" ]] || die "'${APP_PATH}' is not an app directory."

"${BOXEDVN_SCRIPT_DIR}/validate-app.sh" "${APP_PATH}"

if [[ -e "${OUTPUT_PATH}" ]]; then
    die "Refusing to overwrite existing output '${OUTPUT_PATH}'."
fi
mkdir -p "$(dirname "${OUTPUT_PATH}")"
ditto "${APP_PATH}" "${OUTPUT_PATH}"

log "Embedding the requested entitlements in an ad-hoc signature"
codesign --force --sign - --timestamp=none \
    --entitlements "${ENTITLEMENTS}" "${OUTPUT_PATH}"
codesign --verify --strict "${OUTPUT_PATH}"

EXTRACTED="$(mktemp "${TMPDIR:-/tmp}/boxedvn-entitlements.XXXXXX.plist")"
trap 'rm -f "${EXTRACTED}"' EXIT
codesign --display --entitlements :- "${OUTPUT_PATH}" \
    >"${EXTRACTED}" 2>/dev/null

for key in get-task-allow com.apple.developer.kernel.increased-memory-limit; do
    value="$(/usr/libexec/PlistBuddy -c "Print :${key}" \
        "${EXTRACTED}" 2>/dev/null || true)"
    [[ "${value}" == "true" ]] \
        || die "The ad-hoc signature is missing '${key}'."
done

ok "Ad-hoc entitlement template prepared at ${OUTPUT_PATH}"
printf '%s\n' \
    "This copy is not the unsigned build and cannot be installed as-is." \
    "Re-sign it with the same Apple ID/App ID configured in GetMoreRAM."
