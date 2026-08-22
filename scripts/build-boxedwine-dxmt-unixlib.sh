#!/usr/bin/env bash
set -euo pipefail

# Build the x86-64 ELF unix side used by Wine's winemetal PE thunks when the
# guest runs inside BoxedWine.  The implementation is only a syscall veneer;
# the native DXMT table is linked into the iOS host.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SOURCE="${REPO_ROOT}/source/dxmt/boxedwine_dxmt_unixlib.c"
OUTPUT="${1:-${REPO_ROOT}/build/guest-dxmt/winemetal.so}"
CC="${CC:-x86_64-linux-gnu-gcc}"

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[[ -f "${SOURCE}" ]] || die "missing ${SOURCE}"
command -v "${CC}" >/dev/null 2>&1 || die "${CC} is not installed"
command -v python3 >/dev/null 2>&1 || die "python3 is not installed"

mkdir -p "$(dirname "${OUTPUT}")"

"${CC}" \
    -shared -fPIC -fvisibility=hidden \
    -nostdlib -nodefaultlibs -nostartfiles \
    -Wl,--no-undefined -Wl,-soname,winemetal.so \
    -I"${REPO_ROOT}/include" \
    "${SOURCE}" -o "${OUTPUT}"

python3 "${REPO_ROOT}/scripts/validate-dxmt-guest-abi.py" \
    --unixlib "${OUTPUT}"

printf 'built %s\n' "${OUTPUT}"
