#!/usr/bin/env bash
# BoxedVN - build the native iPhoneOS half of the minimal Wine runtime.
#
# Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
#
# Usage:
#   scripts/build-fex64-wine-ios-core.sh [--third-party-dir DIR] [--jobs N]
#
# This deliberately stops before win32u, audio, networking and DXMT. Its one
# job is to turn the already-proven Wine trees plus the pinned iOS integration
# sources into the two native archives needed to start wineserver and ntdll.
# That makes a headless wineboot attempt possible without making graphics or
# the long LLVM toolchain prerequisites of the first boot.

set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

JOBS=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --third-party-dir)
            [[ $# -ge 2 ]] || die "--third-party-dir needs a value"
            BOXEDVN_THIRD_PARTY="$2"; shift 2 ;;
        --jobs)
            [[ $# -ge 2 ]] || die "--jobs needs a value"
            JOBS="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,14p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) die "Unknown argument '$1'. Run with --help." ;;
    esac
done

require_macos
require_command xcrun
require_command ar

[[ -n "${JOBS}" ]] || JOBS="$(sysctl -n hw.ncpu)"

FEX64_DIR="${BOXEDVN_THIRD_PARTY}/fex64"
WINE_SOURCE="${FEX64_DIR}/wine"
WINE_BUILD="${WINE_SOURCE}/build-macos"
INTEGRATION="${FEX64_DIR}/mythic"
NTDLL_IOS="${INTEGRATION}/build/ntdll-unix"
SERVER_IOS="${INTEGRATION}/build/wineserver"
JIT_AUTHORITY_PATCH="${BOXEDVN_ROOT}/patches/mythic-jit-pool-authority.patch"
CHECKED_CALL_PATCH="${BOXEDVN_ROOT}/patches/mythic-arm64ec-checked-call-recovery.patch"
DEAD_VA_PATCH="${BOXEDVN_ROOT}/patches/mythic-dead-va-window.patch"
EC_THUNK_PATCH="${BOXEDVN_ROOT}/patches/mythic-ec-import-thunk.patch"
RSP_WINDOW_PATCH="${BOXEDVN_ROOT}/patches/mythic-rsp-window-diagnostic.patch"
OUTPUT="${FEX64_DIR}/wine-ios-core"
OBJECTS="${FEX64_DIR}/wine-ios-core-objects"
SDK="$(xcrun --sdk iphoneos --show-sdk-path)"

require_file "${WINE_BUILD}/include/config.h" \
    "Run scripts/build-fex64-wine.sh first; the iOS compile needs its generated headers."
require_file "${NTDLL_IOS}/virtual_ios.c" \
    "Fetch the pinned Mythic integration sources before building the iOS Wine core."
require_file "${SERVER_IOS}/main_ios.c"
require_file "${JIT_AUTHORITY_PATCH}"
require_file "${CHECKED_CALL_PATCH}"
require_file "${DEAD_VA_PATCH}"
require_file "${EC_THUNK_PATCH}"
require_file "${RSP_WINDOW_PATCH}"

# The third argument selects the matching mode. The first two patches carry no
# context and need --unidiff-zero, which switches off the context check; the
# cost of that is a patch already applied can look appliable again and land a
# second copy. A patch generated with context is applied with git's normal
# matching instead, which is stricter, tolerates line drift from whatever was
# applied before it, and tells "already applied" from "fresh" reliably.
apply_integration_patch() {
    local patch="$1"
    local description="$2"
    local mode="${3---unidiff-zero}"
    if git -C "${INTEGRATION}" apply ${mode} --check "${patch}"; then
        log "Applying ${description}"
        git -C "${INTEGRATION}" apply ${mode} "${patch}"
    elif git -C "${INTEGRATION}" apply ${mode} --reverse --check "${patch}"; then
        log "${description} is already applied"
    else
        die "The pinned integration source no longer accepts ${patch}; refresh the patch explicitly."
    fi
}

apply_integration_patch "${JIT_AUTHORITY_PATCH}" "JIT-pool data ownership fix"
apply_integration_patch "${CHECKED_CALL_PATCH}" "ARM64EC checked-call recovery"
# Applied last, and generated against the two above, so it is the one that has
# to survive their line numbers.
apply_integration_patch "${DEAD_VA_PATCH}" "dead VA window short-circuit" ""
apply_integration_patch "${EC_THUNK_PATCH}" "ARM64EC import-thunk routing" ""
apply_integration_patch "${RSP_WINDOW_PATCH}" "guest stack-window diagnostic" ""

rm -rf "${OUTPUT}" "${OBJECTS}"
mkdir -p "${OUTPUT}/lib" "${OBJECTS}/ntdll" "${OBJECTS}/wineserver"

FAILED=0

compile_ntdll() {
    local source="$1"
    local name="$2"
    printf '  %-28s ' "${name}"
    if xcrun --sdk iphoneos clang \
        -arch arm64 -isysroot "${SDK}" -miphoneos-version-min=17.0 \
        -O2 -fPIC -fvisibility=hidden -fno-stack-protector -fno-strict-aliasing \
        -Wno-implicit-function-declaration -Wno-int-conversion \
        -include "${WINE_BUILD}/include/config.h" \
        -include "${NTDLL_IOS}/shims/wine_ios_exit.h" \
        -I"${NTDLL_IOS}/shims" \
        -I"${WINE_BUILD}/dlls/ntdll" \
        -I"${WINE_SOURCE}/dlls/ntdll" \
        -I"${WINE_SOURCE}/dlls/ntdll/unix" \
        -I"${WINE_BUILD}/include" -I"${WINE_SOURCE}/include" \
        -D__WINESRC__ -DLTC_NO_PROTOTYPES -DLTC_SOURCE -D_NTSYSTEM_ \
        -D_ACRTIMP= -DWINBASEAPI= \
        -DBINDIR=\"/usr/local/bin\" -DLIBDIR=\"/usr/local/lib\" \
        -DDATADIR=\"/usr/local/share\" -DSYSTEMDLLPATH=\"\" \
        -DWINE_UNIX_LIB -DWINE_IOS=1 \
        -Dget_thread_context=ntdll_get_thread_context \
        -Dset_thread_context=ntdll_set_thread_context \
        -c "${source}" -o "${OBJECTS}/ntdll/${name}.o" \
        2>"${OBJECTS}/ntdll/${name}.err"; then
        echo OK
    else
        echo FAILED
        tail -30 "${OBJECTS}/ntdll/${name}.err" >&2
        FAILED=$((FAILED + 1))
    fi
}

log "Wine ntdll unix side -> iphoneos"
for source in "${WINE_SOURCE}"/dlls/ntdll/unix/*.c; do
    name="$(basename "${source}" .c)"
    case "${name}" in
        loader|process|server|env|virtual|signal_arm64|thread)
            compile_ntdll "${NTDLL_IOS}/${name}_ios.c" "${name}" ;;
        cdrom)
            compile_ntdll "${NTDLL_IOS}/cdrom_stub.c" "${name}" ;;
        *)
            compile_ntdll "${source}" "${name}" ;;
    esac
done

# The null driver is self-contained and keeps Wine's audio entry points
# resolvable before a real iOS audio backend is brought over.
compile_ntdll "${NTDLL_IOS}/audio_null_ios.c" "audio_null_ios"

[[ "${FAILED}" -eq 0 ]] || die "${FAILED} ntdll iPhoneOS translation units failed"
ar rcs "${OUTPUT}/lib/libntdll_unix.a" "${OBJECTS}/ntdll"/*.o

SERVER_FLAGS=(
    -arch arm64 -isysroot "${SDK}" -miphoneos-version-min=17.0 -O2
    -I"${WINE_SOURCE}/include" -I"${WINE_SOURCE}/include/wine"
    -I"${WINE_BUILD}/include" -I"${SERVER_IOS}" -I"${WINE_SOURCE}/server"
    -I"${NTDLL_IOS}/shims"
    -include "${SERVER_IOS}/config_ios.h"
    -include stdarg.h
    -include "${SERVER_IOS}/unicode_fix.h"
    -include "${SERVER_IOS}/wineserver_ios_kill.h"
    -DBINDIR=\"/usr/local/bin\" -DDATADIR=\"/usr/local/share\"
    -D__WINESRC__ -DWINE_IOS=1 -Dmain=wineserver_main
    -Wno-implicit-function-declaration -Wno-int-conversion
)

compile_server() {
    local source="$1"
    local name="$2"
    printf '  %-28s ' "${name}"
    if xcrun --sdk iphoneos clang "${SERVER_FLAGS[@]}" \
        -c "${source}" -o "${OBJECTS}/wineserver/${name}.o" \
        2>"${OBJECTS}/wineserver/${name}.err"; then
        echo OK
    else
        echo FAILED
        tail -30 "${OBJECTS}/wineserver/${name}.err" >&2
        FAILED=$((FAILED + 1))
    fi
}

log "Wine server -> iphoneos"
for source in "${WINE_SOURCE}"/server/*.c; do
    name="$(basename "${source}" .c)"
    case "${name}" in
        request|main|mach|unicode|fd|mapping|queue)
            compile_server "${SERVER_IOS}/${name}_ios.c" "${name}" ;;
        window)
            compile_server "${SERVER_IOS}/window_ios.c" "${name}" ;;
        *)
            compile_server "${source}" "${name}" ;;
    esac
done
compile_server "${SERVER_IOS}/wine_log_ios.c" "wine_log_ios"

# This wrapper must be compiled without the header that replaces kill(), or
# its own implementation would expand recursively.
printf '  %-28s ' "wineserver_ios_kill"
if xcrun --sdk iphoneos clang \
    -arch arm64 -isysroot "${SDK}" -miphoneos-version-min=17.0 -O2 \
    -I"${SERVER_IOS}" -DWINE_IOS=1 -Wno-implicit-function-declaration \
    -c "${SERVER_IOS}/wineserver_ios_kill.c" \
    -o "${OBJECTS}/wineserver/wineserver_ios_kill.o" \
    2>"${OBJECTS}/wineserver/wineserver_ios_kill.err"; then
    echo OK
else
    echo FAILED
    tail -30 "${OBJECTS}/wineserver/wineserver_ios_kill.err" >&2
    FAILED=$((FAILED + 1))
fi

[[ "${FAILED}" -eq 0 ]] || die "${FAILED} Wine core iPhoneOS translation units failed"
ar rcs "${OUTPUT}/lib/libwineserver.a" "${OBJECTS}/wineserver"/*.o

for archive in libntdll_unix.a libwineserver.a; do
    require_file "${OUTPUT}/lib/${archive}"
    members="$(ar t "${OUTPUT}/lib/${archive}" | wc -l | tr -d ' ')"
    bytes="$(wc -c < "${OUTPUT}/lib/${archive}" | tr -d ' ')"
    [[ "${members}" -gt 0 && "${bytes}" -gt 0 ]] || die "${archive} is empty"
    ok "${archive}: ${members} objects, ${bytes} bytes"
done

log "Wine iPhoneOS core staged in ${OUTPUT}"
