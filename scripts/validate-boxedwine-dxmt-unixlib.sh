#!/usr/bin/env bash
set -euo pipefail

# Keep the guest veneer table length tied to the pinned native DXMT table.  A
# stale count would make a valid Wine unix-call index either fail spuriously or
# walk past the native function table.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
HEADER="${REPO_ROOT}/include/boxedwine_x64_hostcall.h"
NATIVE="${REPO_ROOT}/third_party/fex64/dxmt/src/winemetal/unix/winemetal_unix.c"

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[[ -f "${HEADER}" ]] || die "missing ${HEADER}"
[[ -f "${NATIVE}" ]] || die "missing pinned DXMT source ${NATIVE}"

expected="$(sed -n 's/^#define BOXEDWINE_X64_HOSTCALL_DXMT_UNIX_CALL_COUNT \([0-9][0-9]*\)U$/\1/p' "${HEADER}")"
[[ -n "${expected}" ]] || die "could not read the host-call count from ${HEADER}"

actual="$(awk '
    /^const void \*__wine_unix_call_funcs\[\] = \{/ { in_table=1; next }
    /^#ifndef DXMT_NATIVE/ { if (in_table) exit }
    in_table && $0 ~ /^[[:space:]]*(&|NULL)/ { count++ }
    END { print count + 0 }
' "${NATIVE}")"

[[ "${expected}" == "${actual}" ]] || die \
    "host-call count ${expected} does not match native DXMT table ${actual}"

printf 'DXMT unix-call ABI count: %s\n' "${actual}"
