#!/usr/bin/env bash
# BoxedVN - fetch pinned inputs needed to build DXMT x86-64 PE DLLs.
#
# Source repositories are checked out by commit. Binary SDKs are verified by
# SHA-256 before extraction and are used only as build inputs.
#
# Usage:
#   scripts/prepare-dxmt-x64-dependencies.sh [--third-party-dir DIR] [--force]

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
source "${BOXEDVN_SCRIPT_DIR}/dependencies.fex64.lock.sh"

FORCE=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --third-party-dir) [[ $# -ge 2 ]] || die "--third-party-dir needs a value"; BOXEDVN_THIRD_PARTY="$2"; shift 2 ;;
        --force) FORCE=1; shift ;;
        -h|--help) sed -n '2,9p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "Unknown argument '$1'. Run with --help." ;;
    esac
done
require_command git
require_command curl
require_command tar
require_command shasum

FEX64_DIR="${BOXEDVN_THIRD_PARTY}/fex64"
TOOLCHAINS="${FEX64_DIR}/toolchains"
mkdir -p "${TOOLCHAINS}"

fetch_args=(--third-party-dir "${BOXEDVN_THIRD_PARTY}" --component dxmt)
[[ ${FORCE} -eq 1 ]] && fetch_args+=(--force)
bash "${BOXEDVN_SCRIPT_DIR}/fetch-fex64-dependencies.sh" "${fetch_args[@]}"

fetch_archive() {
    local url="$1" path="$2" expected="$3" actual
    if [[ -f "${path}" ]]; then
        actual="$(shasum -a 256 "${path}" | awk '{print $1}')"
        [[ "${actual}" == "${expected}" ]] && { ok "cached: $(basename "${path}")"; return 0; }
        warn "Checksum mismatch for ${path}; downloading again."
        rm -f "${path}"
    fi
    curl --fail --location --show-error --silent --retry 3 --retry-delay 2 \
        --output "${path}.partial" "${url}" || die "Download failed: ${url}"
    actual="$(shasum -a 256 "${path}.partial" | awk '{print $1}')"
    [[ "${actual}" == "${expected}" ]] \
        || die "Checksum mismatch for ${url}: expected ${expected}, got ${actual}."
    mv "${path}.partial" "${path}"
    ok "verified: $(basename "${path}")"
}

MINGW_NAME="llvm-mingw-${BOXEDVN_LLVM_MINGW_VERSION}-ucrt-macos-universal"
MINGW_ROOT="${TOOLCHAINS}/${MINGW_NAME}"
MINGW_ARCHIVE="${TOOLCHAINS}/${MINGW_NAME}.tar.xz"
if [[ ${FORCE} -eq 1 || ! -x "${MINGW_ROOT}/bin/x86_64-w64-mingw32-clang" ]]; then
    fetch_archive "${BOXEDVN_LLVM_MINGW_URL}" "${MINGW_ARCHIVE}" \
        "${BOXEDVN_LLVM_MINGW_SHA256}"
    rm -rf "${MINGW_ROOT}"; tar -xJf "${MINGW_ARCHIVE}" -C "${TOOLCHAINS}"
fi
[[ -x "${MINGW_ROOT}/bin/x86_64-w64-mingw32-clang" ]] \
    || die "llvm-mingw archive did not contain the x86-64 compiler."

WINE_ARCHIVE="${TOOLCHAINS}/wine-${BOXEDVN_DXMT_WINE_SDK_VERSION}.tar.gz"
WINE_ROOT="${TOOLCHAINS}/wine-sdk"
if [[ ${FORCE} -eq 1 || ! -x "${WINE_ROOT}/bin/winebuild" ]]; then
    fetch_archive "${BOXEDVN_DXMT_WINE_SDK_URL}" "${WINE_ARCHIVE}" \
        "${BOXEDVN_DXMT_WINE_SDK_SHA256}"
    rm -rf "${WINE_ROOT}"; mkdir -p "${WINE_ROOT}"
    tar -xzf "${WINE_ARCHIVE}" -C "${WINE_ROOT}"
fi
[[ -x "${WINE_ROOT}/bin/winebuild" ]] \
    || die "Wine SDK archive did not contain bin/winebuild."

LLVM_ROOT="${FEX64_DIR}/llvm-project"
if [[ ${FORCE} -eq 1 ]]; then rm -rf "${LLVM_ROOT}"; fi
if [[ ! -d "${LLVM_ROOT}/llvm/include/llvm" ]]; then
    git clone --depth 1 --branch "${BOXEDVN_LLVM_TAG}" \
        "${BOXEDVN_LLVM_REPOSITORY}" "${LLVM_ROOT}" \
        || die "Could not fetch LLVM ${BOXEDVN_LLVM_TAG}."
fi
[[ -f "${LLVM_ROOT}/llvm/include/llvm/IR/Module.h" ]] \
    || die "LLVM checkout is incomplete."

ok "DXMT x86-64 build inputs are ready"
