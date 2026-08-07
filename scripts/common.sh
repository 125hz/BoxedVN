#!/usr/bin/env bash
# BoxedVN - shared shell helpers.
#
# Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
#
# Sourced by every script in this directory.  Not executable on its own.

set -euo pipefail

BOXEDVN_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BOXEDVN_ROOT="$(cd "${BOXEDVN_SCRIPT_DIR}/.." && pwd)"
export BOXEDVN_ROOT

# Where third-party sources and builds live.  Never inside the source tree's
# tracked directories, and overridable so CI can cache it.
BOXEDVN_THIRD_PARTY="${BOXEDVN_THIRD_PARTY:-${BOXEDVN_ROOT}/third_party}"
export BOXEDVN_THIRD_PARTY

if [[ -t 1 ]]; then
    _bvn_bold=$'\033[1m'; _bvn_red=$'\033[31m'; _bvn_yellow=$'\033[33m'
    _bvn_green=$'\033[32m'; _bvn_reset=$'\033[0m'
else
    _bvn_bold=""; _bvn_red=""; _bvn_yellow=""; _bvn_green=""; _bvn_reset=""
fi

log()  { printf '%s==>%s %s\n' "${_bvn_bold}" "${_bvn_reset}" "$*"; }
ok()   { printf '%s  ok%s %s\n' "${_bvn_green}" "${_bvn_reset}" "$*"; }
warn() { printf '%swarn%s %s\n' "${_bvn_yellow}" "${_bvn_reset}" "$*" >&2; }

die() {
    printf '%serror%s %s\n' "${_bvn_red}" "${_bvn_reset}" "$*" >&2
    exit 1
}

require_command() {
    local command_name="$1"
    local hint="${2:-}"
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        if [[ -n "${hint}" ]]; then
            die "'${command_name}' is not on PATH. ${hint}"
        fi
        die "'${command_name}' is not on PATH."
    fi
}

require_file() {
    local path="$1"
    local hint="${2:-}"
    if [[ ! -f "${path}" ]]; then
        if [[ -n "${hint}" ]]; then
            die "Required file '${path}' is missing. ${hint}"
        fi
        die "Required file '${path}' is missing."
    fi
}

require_macos() {
    if [[ "$(uname -s)" != "Darwin" ]]; then
        die "This script must run on macOS; the iPhoneOS SDK and codesign are \
not available on $(uname -s)."
    fi
}

# Prints the versions of everything the build depends on, so a failed CI log is
# self-describing.
print_tool_versions() {
    log "Tool versions"
    printf '  uname          : %s\n' "$(uname -srm)"
    if [[ "$(uname -s)" == "Darwin" ]]; then
        printf '  macOS          : %s (%s)\n' \
            "$(sw_vers -productVersion)" "$(sw_vers -buildVersion)"
        printf '  host arch      : %s\n' "$(uname -m)"
        if command -v xcodebuild >/dev/null 2>&1; then
            printf '  Xcode          : %s\n' \
                "$(xcodebuild -version | tr '\n' ' ')"
            printf '  xcode-select   : %s\n' "$(xcode-select -p)"
        fi
        if command -v xcrun >/dev/null 2>&1; then
            printf '  iPhoneOS SDK   : %s\n' \
                "$(xcrun --sdk iphoneos --show-sdk-version 2>/dev/null || echo 'not installed')"
            printf '  macOS SDK      : %s\n' \
                "$(xcrun --sdk macosx --show-sdk-version 2>/dev/null || echo 'not installed')"
        fi
    fi
    printf '  cmake          : %s\n' "$(cmake --version | head -1)"
    if command -v ninja >/dev/null 2>&1; then
        printf '  ninja          : %s\n' "$(ninja --version)"
    else
        printf '  ninja          : not installed\n'
    fi
    printf '  git            : %s\n' "$(git --version)"
    if [[ -d "${BOXEDVN_ROOT}/.git" ]]; then
        printf '  BoxedVN commit : %s\n' \
            "$(git -C "${BOXEDVN_ROOT}" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    fi
}

# verify_sha256 <file> <expected>
verify_sha256() {
    local path="$1"
    local expected="$2"
    local actual
    actual="$(shasum -a 256 "${path}" | awk '{print $1}')"
    if [[ "${actual}" != "${expected}" ]]; then
        die "Checksum mismatch for '${path}'.
  expected SHA-256: ${expected}
  actual   SHA-256: ${actual}
The download is corrupt or the pinned artefact was replaced upstream. The file
has been left in place for inspection; delete it to retry."
    fi
    ok "SHA-256 verified: $(basename "${path}")"
}

# download_pinned <url> <destination> <sha256>
#
# Never re-downloads a file that already verifies, and never accepts a file
# whose checksum does not match the pin.
download_pinned() {
    local url="$1"
    local destination="$2"
    local sha256="$3"

    mkdir -p "$(dirname "${destination}")"

    if [[ -f "${destination}" ]]; then
        local actual
        actual="$(shasum -a 256 "${destination}" | awk '{print $1}')"
        if [[ "${actual}" == "${sha256}" ]]; then
            ok "cached: $(basename "${destination}")"
            return 0
        fi
        warn "cached $(basename "${destination}") has the wrong checksum; re-downloading"
        rm -f "${destination}"
    fi

    log "Downloading ${url}"
    require_command curl
    curl --fail --location --show-error --silent \
         --retry 3 --retry-delay 2 \
         --output "${destination}.partial" "${url}" \
        || die "Download failed: ${url}"
    mv "${destination}.partial" "${destination}"
    verify_sha256 "${destination}" "${sha256}"
}
