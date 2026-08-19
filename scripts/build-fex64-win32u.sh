#!/usr/bin/env bash
# BoxedVN - build Wine's win32u unix side for iphoneos.
#
# Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
#
# Usage:
#   scripts/build-fex64-win32u.sh [--third-party-dir DIR] [--jobs N] [--force]
#
# win32u is the window and message subsystem, and like ntdll and the graphics
# translator it is split in two: a PE half the guest loads, and a unix half
# that has to be native. The PE half already ships in the runtime. The unix
# half did not, and BVNWineRuntimeStubs.c answered its init with
# STATUS_NOT_SUPPORTED, which is the same shape of gap that made the graphics
# target branch through a null table until DXMT's unix side was built.
#
# What that costs is visible on device: the guest reaches its message loop,
# PeekMessage reports a message waiting, and the MSG handed back is entirely
# zero - no window, no message id - after which the guest branches through the
# address of its own stack buffer and dies.
#
# The integration carries iOS-adapted sources for the parts that cannot work
# unchanged - message, driver, class, defwnd, winstation, sysparams and a
# freetype wrapper - and this mirrors its build/win32u-unix/build.sh against
# our own tree layout.
#
# Produces: $BOXEDVN_THIRD_PARTY/fex64/win32u-ios/libwin32u_unix.a
#
# macOS with a full Xcode only. Run after scripts/build-fex64-wine.sh: the
# compile needs that tree's generated headers.

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

FORCE=0
JOBS=""

usage() {
    sed -n '2,28p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --third-party-dir)
            [[ $# -ge 2 ]] || die "--third-party-dir needs a value"
            BOXEDVN_THIRD_PARTY="$2"; shift 2 ;;
        --jobs)
            [[ $# -ge 2 ]] || die "--jobs needs a value"
            JOBS="$2"; shift 2 ;;
        --force)
            FORCE=1; shift ;;
        -h|--help)
            usage; exit 0 ;;
        *)
            die "Unknown argument '$1'. Run with --help." ;;
    esac
done

require_macos
require_command xcrun "Install Xcode and run 'xcode-select --install'."
require_command ar

[[ -n "${JOBS}" ]] || JOBS="$(sysctl -n hw.ncpu)"

FEX64_DIR="${BOXEDVN_THIRD_PARTY}/fex64"
TOOLCHAINS="${FEX64_DIR}/toolchains"
WINE_SOURCE="${FEX64_DIR}/wine"
WINE_BUILD="${WINE_SOURCE}/build-macos"
INTEGRATION="${FEX64_DIR}/mythic"
WIN32U_IOS="${INTEGRATION}/build/win32u-unix"
NTDLL_SHIMS="${INTEGRATION}/build/ntdll-unix/shims"
FREETYPE="${TOOLCHAINS}/freetype-ios"
OUTPUT_DIR="${FEX64_DIR}/win32u-ios"
OBJ_DIR="${OUTPUT_DIR}/obj"
LIB="${OUTPUT_DIR}/libwin32u_unix.a"
STAMP="${OUTPUT_DIR}/.stamp"

# Cache check first, for the same reason the graphics build does it: a restored
# archive needs none of the inputs below.
if [[ "${FORCE}" -eq 0 && -f "${STAMP}" && -f "${LIB}" ]]; then
    ok "win32u: cached ($(wc -c < "${LIB}" | tr -d ' ') bytes)"
    exit 0
fi

require_file "${WINE_BUILD}/include/config.h" \
    "Run scripts/build-fex64-wine.sh first; this compile needs its generated headers."
require_file "${WIN32U_IOS}/message_ios.c" \
    "The pinned integration has no win32u iOS sources; check the mythic pin."
require_file "${WIN32U_IOS}/config_ios.h"
require_file "${NTDLL_SHIMS}/wine_ios_exit.h"

SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
IPHONEOS_MIN="17.0"

rm -rf "${OBJ_DIR}" "${LIB}" "${STAMP}"
mkdir -p "${OBJ_DIR}"

# Wine generates its IDL headers on demand, as prerequisites of whichever DLL
# needs them, and the macOS tree here is built with `make include` only - for
# its generated headers, never for a DLL. So nothing ever asks for these, and
# the first build failed on every unit that reaches ntuser_private.h, which
# pulls shlobj.h -> ole2.h -> objbase.h -> combaseapi.h -> objidlbase.h.
#
# Ask for them by name. Done here rather than in build-fex64-wine.sh on
# purpose: that script's hash is the Wine cache key, and changing it would
# throw away a tree that takes twenty minutes to rebuild in order to add a
# step that takes seconds.
if [[ ! -f "${WINE_BUILD}/include/objidlbase.h" ]]; then
    require_command make
    idl_targets=()
    for idl in "${WINE_SOURCE}"/include/*.idl; do
        [[ -e "${idl}" ]] || continue
        idl_targets+=("include/$(basename "${idl}" .idl).h")
    done

    if [[ "${#idl_targets[@]}" -gt 0 ]]; then
        log "win32u: generating ${#idl_targets[@]} Wine IDL headers"
        # -k because not every .idl yields a header - some describe only a type
        # library - and a miss there is not a failure of this step.
        make -C "${WINE_BUILD}" -k -j "${JOBS}" "${idl_targets[@]}" \
            > "${OUTPUT_DIR}/idl-headers.log" 2>&1 || true
    fi

    require_file "${WINE_BUILD}/include/objidlbase.h" \
        "Wine did not generate its IDL headers. See ${OUTPUT_DIR}/idl-headers.log."
    ok "win32u: Wine IDL headers present"
fi

FAILED_FILES=()

# Serial, matching the other iphoneos compiles here: macOS ships bash 3.2 and
# `wait -n` is not available in it.
compile_one() {
    local source="$1" name="$2"
    local status=0
    shift 2

    printf '  %-28s ' "${name}"
    xcrun -sdk iphoneos clang \
        -arch arm64 -isysroot "${SDK}" "-miphoneos-version-min=${IPHONEOS_MIN}" \
        -O2 -fPIC -fvisibility=hidden -fno-stack-protector -fno-strict-aliasing \
        -Wno-implicit-function-declaration -Wno-int-conversion \
        -include "${WIN32U_IOS}/config_ios.h" \
        -include "${NTDLL_SHIMS}/wine_ios_exit.h" \
        -I"${WIN32U_IOS}" \
        -I"${NTDLL_SHIMS}" \
        -I"${WINE_BUILD}/dlls/win32u" -I"${WINE_SOURCE}/dlls/win32u" \
        -I"${WINE_BUILD}/include" -I"${WINE_SOURCE}/include" \
        -D__WINESRC__ -D_WIN32U_ \
        -D_ACRTIMP= -DWINBASEAPI= \
        -DSYSTEMDLLPATH=\"\" \
        -DWINE_UNIX_LIB -DWINE_IOS=1 \
        -D__wine_unix_lib_init=win32u_unix_lib_init \
        -USONAME_LIBFREETYPE -USONAME_LIBFONTCONFIG \
        -USONAME_LIBEGL -USONAME_LIBVULKAN -USONAME_LIBGNUTLS \
        -UHAVE_FT2BUILD_H \
        "$@" \
        -c "${source}" -o "${OBJ_DIR}/${name}.o" \
        2>"${OBJ_DIR}/${name}.err" || status=$?

    if [[ "${status}" -eq 0 ]]; then
        echo ok
    else
        echo FAILED
        FAILED_FILES+=("${name}")
    fi
}

log "win32u unix side -> iphoneos"

# Every translation unit except main.c, which is the PE-side entry and lives in
# win32u.dll. dibdrv sources are prefixed: several of their basenames collide
# with the ones a directory up.
for source in "${WINE_SOURCE}"/dlls/win32u/*.c "${WINE_SOURCE}"/dlls/win32u/dibdrv/*.c; do
    [[ -e "${source}" ]] || continue
    name="$(basename "${source}" .c)"
    [[ "${name}" == "main" ]] && continue

    case "${source}" in
        */dibdrv/*) name="dibdrv_${name}" ;;
    esac

    # Per-file iOS overrides, the same pattern the ntdll compile uses.
    case "${name}" in
        class|winstation|sysparams|defwnd|driver|message)
            compile_one "${WIN32U_IOS}/${name}_ios.c" "${name}"
            continue ;;
        freetype)
            # The wrapper defines HAVE_FT2BUILD_H itself; config_ios.h's undef
            # governs every other unit.
            compile_one "${WIN32U_IOS}/freetype_ios.c" "freetype" \
                -I"${FREETYPE}/include/freetype2" -I"${FREETYPE}/include"
            continue ;;
    esac

    compile_one "${source}" "${name}"
done

if [[ "${#FAILED_FILES[@]}" -gt 0 ]]; then
    warn "win32u: ${#FAILED_FILES[@]} translation unit(s) failed to compile"
    for name in "${FAILED_FILES[@]}"; do
        echo "--- ${name} ---" >&2
        head -25 "${OBJ_DIR}/${name}.err" >&2
    done
    die "win32u: compile failed; full output is in ${OBJ_DIR}/*.err"
fi

ok "win32u: $(find "${OBJ_DIR}" -name '*.o' | wc -l | tr -d ' ') objects"

log "win32u: archiving"
ar rcs "${LIB}" "${OBJ_DIR}"/*.o || die "win32u: ar failed"

# Fold freetype in so the application link needs no separate entry for it.
if [[ -f "${FREETYPE}/lib/libfreetype.a" ]]; then
    xcrun -sdk iphoneos libtool -static -o "${LIB}" \
        "${LIB}" "${FREETYPE}/lib/libfreetype.a" \
        || die "win32u: could not merge libfreetype.a"
    ok "win32u: merged libfreetype.a"
else
    warn "win32u: no libfreetype.a at ${FREETYPE}/lib - fonts will be disabled"
fi

require_file "${LIB}"

# The point of the archive is this symbol: it is what the stub in
# BVNWineRuntimeStubs.c answers with STATUS_NOT_SUPPORTED today, and the two
# have to change together or the stub silently wins the link.
if ! xcrun -sdk iphoneos nm -g "${OBJ_DIR}/message.o" > "${OBJ_DIR}/message.symbols" 2>/dev/null; then
    warn "win32u: could not read symbols from message.o"
fi
if ! xcrun -sdk iphoneos nm -g "${LIB}" 2>/dev/null | grep -q "win32u_unix_lib_init"; then
    die "win32u: built ${LIB}, but it does not define win32u_unix_lib_init.
That symbol is the whole point: without it the stub keeps answering
STATUS_NOT_SUPPORTED and the message queue stays broken."
fi

date -u +%Y-%m-%dT%H:%M:%SZ > "${STAMP}"
ok "win32u: ${LIB} ($(wc -c < "${LIB}" | tr -d ' ') bytes)"
