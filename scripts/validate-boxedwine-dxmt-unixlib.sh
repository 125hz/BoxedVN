#!/usr/bin/env bash
set -euo pipefail

# Keep the guest veneer table length tied to the pinned native DXMT table.  A
# stale count would make a valid Wine unix-call index either fail spuriously or
# walk past the native function table.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
HEADER="${BOXEDVN_DXMT_ABI_HEADER:-${REPO_ROOT}/include/boxedwine_x64_hostcall.h}"
NATIVE="${BOXEDVN_DXMT_ABI_NATIVE:-${REPO_ROOT}/third_party/fex64/dxmt/src/winemetal/unix/winemetal_unix.c}"

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

table_entries="$(awk '
    /^const void \*__wine_unix_call_funcs\[\] = \{/ { in_table=1; next }
    /^#ifndef DXMT_NATIVE/ { if (in_table) exit }
    in_table && $0 ~ /^[[:space:]]*(&[[:alnum:]_]+|NULL),/ {
        entry=$0
        sub(/^[[:space:]]*/, "", entry)
        sub(/,$/, "", entry)
        sub(/^&/, "", entry)
        printf "%u\t%s\n", ordinal++, entry
    }
' "${NATIVE}")"

check_entry() {
    local index="$1"
    local expected_name="$2"
    local actual_name
    actual_name="$(printf '%s\n' "${table_entries}" \
        | awk -F '\t' -v wanted="${index}" '$1 == wanted { print $2; exit }')"
    [[ "${actual_name}" == "${expected_name}" ]] || die \
        "DXMT unix-call index ${index} is '${actual_name:-missing}', expected '${expected_name}'"
}

# These entries cross the BoxedWine/DXMT boundary during surface creation and
# presentation. A table reorder with the same total count would otherwise call
# an unrelated native function with a valid-looking argument block.
check_entry 47 _MTLCommandBuffer_presentDrawable
check_entry 48 _MTLCommandBuffer_presentDrawableAfterMinimumDuration
check_entry 67 _MetalLayer_nextDrawable
check_entry 70 _MetalLayer_setProps
check_entry 71 _MetalLayer_getProps
check_entry 72 _CreateMetalViewFromHWND

printf 'DXMT unix-call ABI count: %s; display and present indices verified\n' "${actual}"
