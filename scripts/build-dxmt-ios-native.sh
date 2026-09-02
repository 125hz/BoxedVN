#!/usr/bin/env bash
# BoxedVN - build DXMT's unix side for iphoneos.
#
# Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
#
# Usage:
#   scripts/build-dxmt-ios-native.sh [--third-party-dir DIR] [--jobs N] [--force]
#
# DXMT ships in two halves. The PE half is built separately for the x86-64
# guest by build-dxmt-x64-pe.sh. This script builds the Metal and
# shader-translation half that runs as native iphoneos arm64 into one static
# archive the application links.
#
# Without it, the reserved BoxedWine host-call dispatcher has no native
# winemetal implementation and must reject every guest graphics call. Keeping
# this archive as an explicit build input also preserves the ordinary build,
# where FEX and DXMT remain absent rather than becoming hidden dependencies.
#
# Produces: $BOXEDVN_THIRD_PARTY/fex64/dxmt-ios/libdxmt_combined.a
#
# macOS with a full Xcode and the Metal toolchain only.

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
source "${BOXEDVN_SCRIPT_DIR}/dependencies.fex64.lock.sh"

FORCE=0
JOBS=""

usage() {
    sed -n '2,23p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
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
require_command xxd

[[ -n "${JOBS}" ]] || JOBS="$(sysctl -n hw.ncpu)"

FEX64_DIR="${BOXEDVN_THIRD_PARTY}/fex64"
TOOLCHAINS="${FEX64_DIR}/toolchains"
DXMT_ROOT="${FEX64_DIR}/dxmt"
DXMT_SRC="${DXMT_ROOT}/src"
LLVM_SRC="${FEX64_DIR}/llvm-project/llvm"
LLVM_BUILD="${TOOLCHAINS}/llvm-ios-build"
OUTPUT_DIR="${FEX64_DIR}/dxmt-ios"
OBJ_DIR="${OUTPUT_DIR}/obj"
SHADER_DIR="${OUTPUT_DIR}/shader-headers"
COMBINED_LIB="${OUTPUT_DIR}/libdxmt_combined.a"
STAMP="${OUTPUT_DIR}/.stamp"

# The cache check comes before the input checks deliberately. A restored
# archive needs neither the DXMT source nor LLVM, and LLVM is the longest build
# in the project - demanding it here would make a cache hit cost an hour.
if [[ "${FORCE}" -eq 0 && -f "${STAMP}" && -f "${COMBINED_LIB}" ]]; then
    ok "dxmt: cached ($(wc -c < "${COMBINED_LIB}" | tr -d ' ') bytes)"
    exit 0
fi

require_file "${DXMT_SRC}/winemetal/unix/winemetal_unix.c" \
    "Fetch the pinned DXMT source first:
  scripts/fetch-fex64-dependencies.sh --component dxmt"
require_file "${LLVM_BUILD}/lib/libLLVMCore.a" \
    "airconv translates DXBC through LLVM. Build it first:
  scripts/build-fex64-toolchains.sh --stage llvm-ios"
require_file "${LLVM_SRC}/include/llvm-c/Core.h" \
    "The LLVM checkout is incomplete; airconv includes its headers directly."

SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
IPHONEOS_MIN="17.0"

rm -rf "${OBJ_DIR}" "${SHADER_DIR}" "${COMBINED_LIB}" "${STAMP}"
mkdir -p "${OBJ_DIR}" "${SHADER_DIR}"

# --- generated shader headers -----------------------------------------------
#
# airconv_context.cpp includes air_msad.h, air_samplepos.h and
# air_tessellation.h, none of which are in the source tree: upstream generates
# them from .metal sources through the meson build we do not run for this half.
# Each shader becomes AIR bitcode and then a C array.
#
# The target triple stays air64-apple-macos14.0, matching upstream. That looks
# wrong in an iphoneos archive and is not: these are LLVM bitcode modules
# airconv links into the shaders it generates at runtime, not code this binary
# executes, and it is the combination the integration ships and runs on device.
# Retargeting them changes shader translation, so it is not done here in
# passing.
build_shader_headers() {
    local shader source

    if ! xcrun -sdk macosx metal --version >/dev/null 2>&1; then
        die "The Metal compiler is unavailable, so airconv's shader headers
cannot be generated. Install it with:
  xcodebuild -downloadComponent MetalToolchain"
    fi

    for shader in air_msad air_samplepos air_tessellation; do
        source="${DXMT_SRC}/airconv/shaders/${shader}.metal"
        require_file "${source}"
        xcrun -sdk macosx metal -o "${SHADER_DIR}/${shader}.air" \
            -c "${source}" -std=metal3.1 --target=air64-apple-macos14.0 \
            || die "dxmt: could not compile ${shader}.metal to AIR"
        # -n names the array after the shader, which is what the #include
        # expects. It needs xxd from vim 9 or newer.
        ( cd "${SHADER_DIR}" \
          && xxd -n "${shader}" -i "${shader}.air" "${shader}.h" ) \
            || die "dxmt: could not hexdump ${shader}.air"
        require_file "${SHADER_DIR}/${shader}.h"
    done
    ok "dxmt: 3 shader headers"
}

build_shader_headers

# --- compile ----------------------------------------------------------------

COMMON_FLAGS=(-arch arm64 -isysroot "${SDK}"
              "-miphoneos-version-min=${IPHONEOS_MIN}"
              -DDXMT_NATIVE=1 -fblocks -O2)
INCLUDES=(-I"${DXMT_ROOT}/include" -I"${DXMT_ROOT}/libs"
          -I"${DXMT_SRC}/winemetal" -I"${DXMT_SRC}/airconv")
INCLUDES_DIRECTX=(-I"${DXMT_ROOT}/include/native/directx"
                  -I"${DXMT_ROOT}/include/native/windows")
INCLUDES_SHADERS=(-I"${SHADER_DIR}")
LLVM_INCLUDES=(-I"${LLVM_BUILD}/include" -I"${LLVM_SRC}/include")
AIRCONV_DEFS=(-D_FILE_OFFSET_BITS=64 -D__STDC_CONSTANT_MACROS
              -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS)

FAILED_FILES=()

# Each translation unit writes its own .err so a failure names the file instead
# of interleaving with the rest. Compilation is serial deliberately: macOS
# still ships bash 3.2, which has no `wait -n`, and every other script here
# stays inside that dialect. The LLVM toolchain build dominates this stage's
# wall time regardless.
compile_one() {
    local kind="$1" source="$2" name="$3"
    local status=0

    require_file "${source}"
    printf '  %-32s ' "${name}"

    case "${kind}" in
        objc)
            xcrun -sdk iphoneos clang "${COMMON_FLAGS[@]}" -x objective-c \
                "${INCLUDES[@]}" \
                -c "${source}" -o "${OBJ_DIR}/${name}.o" \
                2>"${OBJ_DIR}/${name}.err" || status=$? ;;
        cxx)
            xcrun -sdk iphoneos clang++ "${COMMON_FLAGS[@]}" \
                -std=c++20 -fno-exceptions -fno-rtti \
                "${INCLUDES[@]}" "${INCLUDES_DIRECTX[@]}" \
                "${INCLUDES_SHADERS[@]}" "${LLVM_INCLUDES[@]}" \
                "${AIRCONV_DEFS[@]}" \
                -c "${source}" -o "${OBJ_DIR}/${name}.o" \
                2>"${OBJ_DIR}/${name}.err" || status=$? ;;
        cxx-exceptions)
            # ShaderBinary.cpp throws, so -fno-exceptions cannot apply here.
            xcrun -sdk iphoneos clang++ "${COMMON_FLAGS[@]}" \
                -std=c++20 -fno-rtti \
                "${INCLUDES[@]}" "${INCLUDES_DIRECTX[@]}" \
                "${AIRCONV_DEFS[@]}" \
                -c "${source}" -o "${OBJ_DIR}/${name}.o" \
                2>"${OBJ_DIR}/${name}.err" || status=$? ;;
        *)
            die "compile_one: unknown kind '${kind}'" ;;
    esac

    if [[ "${status}" -eq 0 ]]; then
        echo ok
    else
        echo FAILED
        FAILED_FILES+=("${name}")
    fi
}

log "dxmt: compiling"

# The unix side reads guest pointers directly. Under BoxedWine the guest is
# dereferenced through one address rule (include/boxedwine_dxmt_guest_pointer.h)
# that is not the identity for Wine's thread stacks, where every unix-call
# parameter block and its stack-resident descriptors live; a device run had
# every DXMT call refused for exactly that reason. The two unix sources are
# rewritten beside the originals so each guest pointer read is translated,
# and the translation helper is force-included into those translation units.
# The originals are never modified; a pinned-source change that moves a
# dereference site fails the rewrite rather than shipping an untranslated read.
require_command python3
python3 "${BOXEDVN_ROOT}/scripts/rewrite-dxmt-guest-pointers.py" \
    "${DXMT_SRC}/winemetal/unix/winemetal_unix.c" \
    "${DXMT_SRC}/winemetal/unix/cache.c" \
    || die "dxmt: could not rewrite the unix side's guest pointer reads"
GUEST_POINTER_HEADER="${BOXEDVN_ROOT}/include/boxedwine_dxmt_guest_pointer.h"
require_file "${GUEST_POINTER_HEADER}"
COMMON_FLAGS+=(-include "${GUEST_POINTER_HEADER}")

compile_one objc "${DXMT_SRC}/winemetal/unix/winemetal_unix.boxedwine.c" winemetal_unix
compile_one objc "${DXMT_SRC}/winemetal/unix/cache.boxedwine.c" cache

for cpp in airconv_context air_type air_signature air_operations \
           dxbc_converter dxbc_converter_gs dxbc_converter_ts \
           dxbc_converter_basicblock dxbc_converter_cfg \
           dxbc_instructions dxbc_signature metallib_writer; do
    compile_one cxx "${DXMT_SRC}/airconv/${cpp}.cpp" "${cpp}"
done

compile_one cxx "${DXMT_SRC}/airconv/nt/air_builder.cpp" air_builder
compile_one cxx "${DXMT_SRC}/airconv/nt/dxbc_converter_base.cpp" dxbc_converter_base
compile_one cxx "${DXMT_SRC}/airconv/transforms/lower_16bit_texread.cpp" lower_16bit_texread

for cpp in BlobContainer DXBCUtils ShaderBinary; do
    compile_one cxx-exceptions "${DXMT_ROOT}/libs/DXBCParser/${cpp}.cpp" "dxbc_${cpp}"
done

if [[ "${#FAILED_FILES[@]}" -gt 0 ]]; then
    warn "dxmt: ${#FAILED_FILES[@]} translation unit(s) failed to compile"
    for name in "${FAILED_FILES[@]}"; do
        echo "--- ${name} ---" >&2
        head -25 "${OBJ_DIR}/${name}.err" >&2
    done
    die "dxmt: compile failed; full output is in ${OBJ_DIR}/*.err"
fi

ok "dxmt: $(find "${OBJ_DIR}" -name '*.o' | wc -l | tr -d ' ') objects"

# --- archive ----------------------------------------------------------------
#
# airconv references LLVM throughout, so the LLVM iphoneos libraries are folded
# into the same archive rather than left for the application to list in
# dependency order.
log "dxmt: archiving"
xcrun -sdk iphoneos libtool -static -o "${COMBINED_LIB}" \
    "${OBJ_DIR}"/*.o "${LLVM_BUILD}"/lib/*.a \
    || die "dxmt: libtool could not combine the archive"

require_file "${COMBINED_LIB}"

# This symbol is the table winemetal.dll reaches through. Verify it by content
# rather than trusting that the compile produced it, the same way the app build
# checks its own display exports.
#
# Read the one object that defines it, into a file, and grep the file. Doing
# this as `nm archive | grep -q` instead is actively wrong here: grep -q exits
# at the first match and closes the pipe while nm is still streaming the
# million-odd symbols LLVM contributes, nm dies on SIGPIPE, and common.sh's
# `set -o pipefail` turns that into a failed check - so the gate fires
# precisely when the symbol IS present. It did, on the first green build.
SYMBOLS="${OBJ_DIR}/winemetal_unix.symbols"
xcrun -sdk iphoneos nm -g "${OBJ_DIR}/winemetal_unix.o" > "${SYMBOLS}" \
    || die "dxmt: could not read the symbol table of winemetal_unix.o"

if ! grep -q "dxmt_winemetal_unix_call_funcs" "${SYMBOLS}"; then
    warn "dxmt: winemetal_unix.o defines no renamed unix-call table"
    die "dxmt: built ${COMBINED_LIB}, but winemetal_unix.o does not define
dxmt_winemetal_unix_call_funcs. The rename is guarded by TARGET_OS_IOS, so a
miss here means the iOS branch was not taken. Do not package an archive that
does not export this table."
fi

# ... and that the archive actually carries that object. Written to a file for
# the same reason.
MEMBERS="${OUTPUT_DIR}/members.txt"
ar -t "${COMBINED_LIB}" > "${MEMBERS}" \
    || die "dxmt: could not list the members of ${COMBINED_LIB}"

if ! grep -qx "winemetal_unix.o" "${MEMBERS}"; then
    die "dxmt: ${COMBINED_LIB} does not contain winemetal_unix.o, so the table
it defines cannot reach the link."
fi
ok "dxmt: unix-call table present"

date -u +%Y-%m-%dT%H:%M:%SZ > "${STAMP}"
ok "dxmt: ${COMBINED_LIB} ($(wc -c < "${COMBINED_LIB}" | tr -d ' ') bytes)"
