#!/usr/bin/env bash
# BoxedVN - stage the x86-64 graphics probe and DXMT PE DLLs.
#
# The probe is BUILT from this repository's source (--probe), while the DLLs
# must come from build-dxmt-x64-pe.sh. Every copied PE is checked for the
# x86-64 machine type so an ARM64/ARM64EC resource cannot silently enter the
# bundle, and the probe is additionally checked for its stage markers.
#
# The probe used to be an opaque prebuilt executable copied out of the pinned
# integration checkout. Two device runs showed it exiting with status 1400 from
# one of three branches, each of which displays a message box before reading
# GetLastError -- so the status could not name the branch and the log said
# nothing more. Staging a binary this repository built, with markers it can be
# checked for, is what makes the failing stage recoverable.
#
# Usage:
#   scripts/stage-x64-graphics-assets.sh --probe PATH --dxmt DIR \
#       --output-dir DIR

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

PROBE=""; DXMT=""; OUTPUT_DIR=""; NATIVE_ARCHIVE=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --probe)      [[ $# -ge 2 ]] || die "--probe needs a value"; PROBE="$2"; shift 2 ;;
        --dxmt)       [[ $# -ge 2 ]] || die "--dxmt needs a value"; DXMT="$2"; shift 2 ;;
        --output-dir) [[ $# -ge 2 ]] || die "--output-dir needs a value"; OUTPUT_DIR="$2"; shift 2 ;;
        --native-archive) [[ $# -ge 2 ]] || die "--native-archive needs a value"; NATIVE_ARCHIVE="$2"; shift 2 ;;
        -h|--help) sed -n '2,22p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "Unknown argument '$1'. Run with --help." ;;
    esac
done
[[ -n "${PROBE}" ]] || die "--probe is required: stage the probe this repository builds, not a prebuilt binary."
[[ -f "${PROBE}" ]] || die "Built probe '${PROBE}' is missing. Run scripts/build-x64-graphics-probe.sh first."
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

verify_x64_pe "${PROBE}"
# The staged probe has to be able to say which stage it reached. Checking the
# marker strings here means a build that dropped them, or a stray prebuilt
# binary passed in by mistake, fails the job rather than shipping a probe whose
# device log is as mute as the one it replaced.
for marker in \
    'BOXEDVN_X64_CUBE_STAGE ' \
    'register-class' \
    'create-window' \
    'd3d11-create' \
    'present'; do
    grep -qa -- "${marker}" "${PROBE}" \
        || die "'${PROBE}' does not contain the '${marker}' stage marker. It is not the instrumented probe this repository builds."
done
cp "${PROBE}" "${OUTPUT_DIR}/boxedvn-d3d11-cube-x64.exe"

for dll in d3d11 dxgi d3d10core winemetal; do
    source_dll="${DXMT}/x86_64-windows/${dll}.dll"
    verify_x64_pe "${source_dll}"
    cp "${source_dll}" "${OUTPUT_DIR}/dxmt-x64/${dll}.dll"
done
if [[ -n "${NATIVE_ARCHIVE}" ]]; then
    cp "${NATIVE_ARCHIVE}" "${OUTPUT_DIR}/libdxmt_combined.a"
fi

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
python3 "${BOXEDVN_SCRIPT_DIR}/validate-dxmt-guest-abi.py" \
    --graphics-dir "${OUTPUT_DIR}"
ok "Staged x86-64 graphics resources: ${OUTPUT_DIR}"
