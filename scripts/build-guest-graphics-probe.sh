#!/usr/bin/env bash
# Build the dependency-free IA-32 Direct3D acceptance probe.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUTPUT="${1:-${REPO_ROOT}/build/guest-probes/boxedvn-d3d9-cube.exe}"
CXX="${BOXEDVN_GUEST_CXX:-i686-w64-mingw32-g++}"
OBJDUMP="${BOXEDVN_GUEST_OBJDUMP:-i686-w64-mingw32-objdump}"

if ! command -v "${CXX}" >/dev/null 2>&1; then
    printf 'error: %s is required to build the IA-32 graphics probe.\n' "${CXX}" >&2
    printf 'On Debian/Ubuntu, install g++-mingw-w64-i686.\n' >&2
    exit 1
fi
if ! command -v "${OBJDUMP}" >/dev/null 2>&1; then
    printf 'error: %s is required to validate the graphics probe.\n' "${OBJDUMP}" >&2
    exit 1
fi

mkdir -p "$(dirname "${OUTPUT}")"
"${CXX}" \
    -std=c++17 -Os -Wall -Wextra -Werror \
    -static-libgcc -static-libstdc++ -mwindows \
    "${REPO_ROOT}/tools/guest-probes/d3d9_cube.cpp" \
    -o "${OUTPUT}" -ld3d9

if ! "${OBJDUMP}" -f "${OUTPUT}" | grep -q 'file format pei-i386'; then
    printf 'error: %s is not an IA-32 PE executable.\n' "${OUTPUT}" >&2
    exit 1
fi

if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "${OUTPUT}" > "${OUTPUT}.sha256"
else
    shasum -a 256 "${OUTPUT}" > "${OUTPUT}.sha256"
fi

printf 'Built IA-32 Direct3D probe: %s\n' "${OUTPUT}"
cat "${OUTPUT}.sha256"
