#!/usr/bin/env bash
# BoxedVN - stage the pinned x86-64 graphics probe and DXMT PE DLLs.
#
# The probe is taken from the pinned integration checkout, while the DLLs must
# come from build-dxmt-x64-pe.sh. Every copied PE is checked for the x86-64
# machine type so an ARM64/ARM64EC resource cannot silently enter the bundle.
#
# Usage:
#   scripts/stage-x64-graphics-assets.sh --mythic DIR --dxmt DIR \
#       --output-dir DIR

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

MYTHIC=""; DXMT=""; OUTPUT_DIR=""; NATIVE_ARCHIVE=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --mythic)     [[ $# -ge 2 ]] || die "--mythic needs a value"; MYTHIC="$2"; shift 2 ;;
        --dxmt)       [[ $# -ge 2 ]] || die "--dxmt needs a value"; DXMT="$2"; shift 2 ;;
        --output-dir) [[ $# -ge 2 ]] || die "--output-dir needs a value"; OUTPUT_DIR="$2"; shift 2 ;;
        --native-archive) [[ $# -ge 2 ]] || die "--native-archive needs a value"; NATIVE_ARCHIVE="$2"; shift 2 ;;
        -h|--help) sed -n '2,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "Unknown argument '$1'. Run with --help." ;;
    esac
done
[[ -d "${MYTHIC}" ]] || die "Pinned integration checkout '${MYTHIC}' is missing."
[[ -d "${DXMT}" ]] || die "DXMT PE output '${DXMT}' is missing."
[[ -n "${OUTPUT_DIR}" ]] || die "--output-dir is required."
if [[ -n "${NATIVE_ARCHIVE}" ]]; then
    [[ -f "${NATIVE_ARCHIVE}" ]] || die "Native DXMT archive '${NATIVE_ARCHIVE}' is missing."
fi
require_command file
require_command python3
mkdir -p "${OUTPUT_DIR}/dxmt-x64"
OUTPUT_DIR="$(cd "${OUTPUT_DIR}" && pwd)"

verify_x64_pe() {
    local path="$1"
    [[ -f "${path}" ]] || die "Missing required PE '${path}'."
    file "${path}" | grep -Eqi 'PE32\+.*x86-64' \
        || die "'${path}' is not an x86-64 PE image."
}

PROBE="${MYTHIC}/app/Mythic/arm64ec-windows/cube-x64.exe"
verify_x64_pe "${PROBE}"
cp "${PROBE}" "${OUTPUT_DIR}/boxedvn-d3d11-cube-x64.exe"

for dll in d3d11 dxgi d3d10core winemetal; do
    source_dll="${DXMT}/x86_64-windows/${dll}.dll"
    verify_x64_pe "${source_dll}"
    cp "${source_dll}" "${OUTPUT_DIR}/dxmt-x64/${dll}.dll"
done
if [[ -n "${NATIVE_ARCHIVE}" ]]; then
    cp "${NATIVE_ARCHIVE}" "${OUTPUT_DIR}/libdxmt_combined.a"
fi

python3 "${BOXEDVN_SCRIPT_DIR}/validate-dxmt-guest-abi.py" \
    --pe-dir "${OUTPUT_DIR}/dxmt-x64" \
    --probe "${OUTPUT_DIR}/boxedvn-d3d11-cube-x64.exe"

sha256_file() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        sha256sum "$1" | awk '{print $1}'
    fi
}
{
    printf '%s\n' '# BoxedVN x86-64 graphics resources manifest v1.'
    printf '%s\n' 'format=boxedvn-x64-graphics-v1'
    printf '%s\n' 'probe=boxedvn-d3d11-cube-x64.exe'
    printf 'probe_sha256=%s\n' "$(sha256_file "${OUTPUT_DIR}/boxedvn-d3d11-cube-x64.exe")"
    for dll in d3d11 dxgi d3d10core winemetal; do
        printf '%s_sha256=%s\n' "${dll}" \
            "$(sha256_file "${OUTPUT_DIR}/dxmt-x64/${dll}.dll")"
    done
    if [[ -n "${NATIVE_ARCHIVE}" ]]; then
        printf 'native_archive=libdxmt_combined.a\n'
        printf 'native_archive_sha256=%s\n' \
            "$(sha256_file "${OUTPUT_DIR}/libdxmt_combined.a")"
    fi
} > "${OUTPUT_DIR}/x64-graphics.manifest"
ok "Staged x86-64 graphics resources: ${OUTPUT_DIR}"
