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

TOOLCHAIN_ROOT="$(dirname "${MINGW_ROOT}")"
LLVM_HOST="${TOOLCHAIN_ROOT}/llvm-darwin-x64"
 # DXMT's airconv Meson file includes ../../toolchains/llvm/include from the
 # source tree, so the Windows LLVM install must live at this exact path.
LLVM_WIN="${DXMT_SOURCE}/toolchains/llvm"
LLVM_HOST_BUILD="${TOOLCHAIN_ROOT}/llvm-darwin-x64-build"
LLVM_WIN_BUILD="${TOOLCHAIN_ROOT}/llvm-windows-x64-build"

build_llvm() {
    local kind="$1" build="$2" prefix="$3"
    if [[ ${FORCE} -eq 0 && -f "${prefix}/lib/libLLVMCore.a" ]]; then
        ok "LLVM ${kind}: cached"; return 0
    fi
    rm -rf "${build}" "${prefix}"; mkdir -p "${build}"
    local args=(
        -S "${LLVM_SOURCE}/llvm" -B "${build}" -G Ninja
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="${prefix}"
        -DLLVM_ENABLE_ASSERTIONS=OFF -DLLVM_ENABLE_ZSTD=OFF
        -DLLVM_TARGETS_TO_BUILD="" -DLLVM_BUILD_TOOLS=OFF
        -DLLVM_BUILD_TESTS=OFF -DLLVM_BUILD_EXAMPLES=OFF
        -DLLVM_ENABLE_PROJECTS=""
    )
    if [[ "${kind}" == "host" ]]; then
        args+=( -DCMAKE_OSX_ARCHITECTURES="$(uname -m)"
                -DLLVM_HOST_TRIPLE="$(uname -m)-apple-darwin" )
    else
        args+=( -DCMAKE_SYSTEM_NAME=Windows
                -DLLVM_HOST_TRIPLE=x86_64-w64-mingw32
                -DCMAKE_SYSROOT="${MINGW_ROOT}"
                -DCMAKE_C_COMPILER="${MINGW_BIN}/x86_64-w64-mingw32-gcc"
                -DCMAKE_CXX_COMPILER="${MINGW_BIN}/x86_64-w64-mingw32-g++" )
    fi
    cmake "${args[@]}"; cmake --build "${build}" --parallel "${JOBS}"
    cmake --install "${build}"
    [[ -f "${prefix}/lib/libLLVMCore.a" ]] || die "LLVM ${kind} produced no libLLVMCore.a."
    ok "LLVM ${kind}: ${prefix}"
}
build_llvm host "${LLVM_HOST_BUILD}" "${LLVM_HOST}"
build_llvm windows "${LLVM_WIN_BUILD}" "${LLVM_WIN}"

CROSS_FILE="${OUTPUT_DIR}/dxmt-win64.cross"
NATIVE_FILE="${OUTPUT_DIR}/dxmt-host.native"
cat > "${CROSS_FILE}" <<EOF
[binaries]
c = '${MINGW_BIN}/x86_64-w64-mingw32-clang'
cpp = '${MINGW_BIN}/x86_64-w64-mingw32-clang++'
ar = '${MINGW_BIN}/x86_64-w64-mingw32-ar'
strip = '${MINGW_BIN}/x86_64-w64-mingw32-strip'
windres = '${MINGW_BIN}/x86_64-w64-mingw32-windres'
[properties]
needs_exe_wrapper = true
[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF
cat > "${NATIVE_FILE}" <<EOF
[binaries]
c = 'clang'
cpp = 'clang++'
EOF

BUILD_DIR="${OUTPUT_DIR}/build"
if [[ ${FORCE} -eq 1 ]]; then rm -rf "${BUILD_DIR}"; fi
if [[ ! -f "${BUILD_DIR}/build.ninja" ]]; then
    meson setup "${BUILD_DIR}" "${DXMT_SOURCE}" \
        --cross-file "${CROSS_FILE}" --native-file "${NATIVE_FILE}" \
        --buildtype release -Dwine_builtin_dll=true \
        -Dnative_llvm_path="${LLVM_HOST}" -Dwine_install_path="${WINE_INSTALL}" \
        -Dbuild_airconv_for_windows=true -Ddxmt_native=false \
        -Denable_nvapi=false -Denable_nvngx=false
fi
meson compile -C "${BUILD_DIR}" -j "${JOBS}"

rm -rf "${OUTPUT_DIR}/x86_64-windows"; mkdir -p "${OUTPUT_DIR}/x86_64-windows"
for dll in d3d11 dxgi d3d10core winemetal; do
    candidate="$(find "${BUILD_DIR}/src" -type f -path "*/${dll}.dll" -print -quit)"
    [[ -f "${candidate}" ]] || die "DXMT build did not produce ${dll}.dll."
    cp "${candidate}" "${OUTPUT_DIR}/x86_64-windows/${dll}.dll"
    file "${candidate}" | grep -Eqi 'PE32\+.*x86-64' \
        || die "${candidate} is not an x86-64 PE image."
done
ok "DXMT x86-64 PE DLLs: ${OUTPUT_DIR}/x86_64-windows"
