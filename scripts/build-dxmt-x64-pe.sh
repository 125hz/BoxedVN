#!/usr/bin/env bash
# BoxedVN - build DXMT's x86-64 Windows DLLs.
#
# This builds the PE half for the actual x86-64 guest ABI. ARM64 and ARM64EC
# images are rejected by the final architecture check. The native Metal half
# is built separately by the iOS build.
#
# Usage:
#   scripts/build-dxmt-x64-pe.sh --source DIR --toolchain DIR \
#       --wine-install DIR --llvm-source DIR --output-dir DIR

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

DXMT_SOURCE=""; MINGW_ROOT=""; WINE_INSTALL=""; LLVM_SOURCE=""
OUTPUT_DIR=""; JOBS=""; FORCE=0
usage() { sed -n '2,11p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }
while [[ $# -gt 0 ]]; do
    case "$1" in
        --source)       [[ $# -ge 2 ]] || die "--source needs a value"; DXMT_SOURCE="$2"; shift 2 ;;
        --toolchain)    [[ $# -ge 2 ]] || die "--toolchain needs a value"; MINGW_ROOT="$2"; shift 2 ;;
        --wine-install) [[ $# -ge 2 ]] || die "--wine-install needs a value"; WINE_INSTALL="$2"; shift 2 ;;
        --llvm-source)  [[ $# -ge 2 ]] || die "--llvm-source needs a value"; LLVM_SOURCE="$2"; shift 2 ;;
        --output-dir)   [[ $# -ge 2 ]] || die "--output-dir needs a value"; OUTPUT_DIR="$2"; shift 2 ;;
        --jobs)         [[ $# -ge 2 ]] || die "--jobs needs a value"; JOBS="$2"; shift 2 ;;
        --force)        FORCE=1; shift ;;
        -h|--help)      usage; exit 0 ;;
        *)              die "Unknown argument '$1'. Run with --help." ;;
    esac
done

require_macos
require_command cmake
require_command file
require_command meson "Install Meson with 'python3 -m pip install meson'."
require_command ninja
require_command python3
require_command xcrun
require_command xxd
[[ -n "${JOBS}" ]] || JOBS="$(sysctl -n hw.ncpu)"
[[ -n "${OUTPUT_DIR}" ]] || die "--output-dir is required."
for required in "${DXMT_SOURCE}" "${MINGW_ROOT}" "${WINE_INSTALL}" "${LLVM_SOURCE}"; do
    [[ -d "${required}" ]] || die "Required directory '${required}' is missing."
done
DXMT_SOURCE="$(cd "${DXMT_SOURCE}" && pwd)"
MINGW_ROOT="$(cd "${MINGW_ROOT}" && pwd)"
WINE_INSTALL="$(cd "${WINE_INSTALL}" && pwd)"
LLVM_SOURCE="$(cd "${LLVM_SOURCE}" && pwd)"
mkdir -p "${OUTPUT_DIR}"; OUTPUT_DIR="$(cd "${OUTPUT_DIR}" && pwd)"

MINGW_BIN="${MINGW_ROOT}/bin"
for tool in x86_64-w64-mingw32-clang x86_64-w64-mingw32-clang++ \
            x86_64-w64-mingw32-ar x86_64-w64-mingw32-strip \
            x86_64-w64-mingw32-windres; do
    [[ -x "${MINGW_BIN}/${tool}" ]] || die "Missing llvm-mingw tool '${MINGW_BIN}/${tool}'."
done

# --- pinned-source patches --------------------------------------------------
#
# DXMT is built from a pinned commit, and the changes this port needs to that
# source live as unified diffs in scripts/dxmt-patches rather than as commits
# in the checkout: the pin stays readable, and a patch the pin outgrows fails
# the build instead of rotting silently. Both DXMT builds apply the whole
# directory. They compile different halves from separate trees - this one the
# guest PE DLLs, build-dxmt-ios-native.sh the native Metal side - and the two
# halves agree on where a mapped buffer's memory lives, so they must not be
# patched differently.
#
# Idempotent: a tree that already carries a patch reverses it cleanly, which
# is what a restored CI cache looks like.
apply_dxmt_patches() {
    local tree="$1" patch name
    require_command git
    for patch in "${BOXEDVN_ROOT}"/scripts/d3d9-metal-patches/*.patch; do
        [[ -f "${patch}" ]] || die "No DXMT patches found in scripts/dxmt-patches."
        name="$(basename "${patch}")"
        if git -C "${tree}" apply --reverse --check "${patch}" 2>/dev/null; then
            ok "dxmt patch already applied: ${name}"
            continue
        fi
        git -C "${tree}" apply --check "${patch}" \
            || die "DXMT patch ${name} no longer applies to the pinned source in
${tree}. The pin moved or upstream fixed this. Re-cut the patch deliberately."
        git -C "${tree}" apply "${patch}" || die "Could not apply ${name}."
        ok "dxmt patch applied: ${name}"
    done
}
apply_dxmt_patches "${DXMT_SOURCE}"

# All shader compilation runs natively on iPhoneOS. No Windows LLVM copy.
for arch in x86_64 i686; do
family=x86_64; install_arch=x86_64
if [[ "${arch}" == i686 ]]; then family=x86; install_arch=i386; fi
CROSS_FILE="${OUTPUT_DIR}/dxmt-${arch}.cross"
NATIVE_FILE="${OUTPUT_DIR}/dxmt-${arch}.native"
cat > "${CROSS_FILE}" <<EOF
[binaries]
c = '${MINGW_BIN}/${arch}-w64-mingw32-clang'
cpp = '${MINGW_BIN}/${arch}-w64-mingw32-clang++'
ar = '${MINGW_BIN}/${arch}-w64-mingw32-ar'
strip = '${MINGW_BIN}/${arch}-w64-mingw32-strip'
windres = '${MINGW_BIN}/${arch}-w64-mingw32-windres'
[properties]
needs_exe_wrapper = true
[host_machine]
system = 'windows'
cpu_family = '${family}'
cpu = '${arch}'
endian = 'little'
EOF
cat > "${NATIVE_FILE}" <<EOF
[binaries]
c = 'clang'
cpp = 'clang++'
EOF

BUILD_DIR="${OUTPUT_DIR}/build-${arch}"
if [[ ${FORCE} -eq 1 ]]; then rm -rf "${BUILD_DIR}"; fi
if [[ ! -f "${BUILD_DIR}/build.ninja" ]]; then
    # DXMT_IOS: iOS Metal has no Managed storage mode; DXMT's winemetal.h
    # remaps Managed to Shared under this define, but its meson build only
    # sets it for the aarch64-windows target. This x86-64 build runs on the
    # same iOS device through FEX, and without the define the first buffer
    # the guest creates asserts inside Metal ("Invalid storageMode").
    meson setup "${BUILD_DIR}" "${DXMT_SOURCE}" \
        --cross-file "${CROSS_FILE}" --native-file "${NATIVE_FILE}" \
        --buildtype release -Dwine_builtin_dll=true \
        -Dwine_install_path="${WINE_INSTALL}" \
        -Dbuild_airconv_for_windows=false -Ddxmt_native=false \
        -Denable_nvapi=false -Denable_nvngx=false \
        -Dc_args=-DDXMT_IOS=1 -Dcpp_args=-DDXMT_IOS=1
fi
meson compile -C "${BUILD_DIR}" -j "${JOBS}"

rm -rf "${OUTPUT_DIR}/${install_arch}-windows"; mkdir -p "${OUTPUT_DIR}/${install_arch}-windows"
for dll in d3d11 dxgi d3d10core winemetal d3d9; do
    candidate="$(find "${BUILD_DIR}/src" -type f -path "*/${dll}.dll" -print -quit)"
    [[ -f "${candidate}" ]] || die "DXMT build did not produce ${dll}.dll."
    cp "${candidate}" "${OUTPUT_DIR}/${install_arch}-windows/${dll}.dll"
    if [[ "${arch}" == x86_64 ]]; then
        file "${candidate}" | grep -Eqi 'PE32\+.*x86-64' || die "Invalid PE64 ${candidate}"
    else
        file "${candidate}" | grep -Eqi 'PE32 .*Intel 80386' || die "Invalid PE32 ${candidate}"
    fi
done
if [[ "${arch}" == x86_64 ]]; then
    python3 "${BOXEDVN_ROOT}/scripts/validate-dxmt-guest-abi.py" --pe-dir "${OUTPUT_DIR}/x86_64-windows"
fi
done
ok "DXMT D3D9/11 PE32 and PE64 modules built"
