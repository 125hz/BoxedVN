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

TOOLCHAIN_ROOT="$(dirname "${MINGW_ROOT}")"
 # DXMT's airconv Meson file includes ../../toolchains/llvm/include from the
 # source tree, so the Windows LLVM install must live at this exact path.
LLVM_WIN="${DXMT_SOURCE}/toolchains/llvm"
LLVM_WIN_BUILD="${TOOLCHAIN_ROOT}/llvm-windows-x64-build"

build_llvm() {
    local kind="$1" build="$2" prefix="$3"
    if [[ ${FORCE} -eq 0 && -f "${prefix}/lib/libLLVMCore.a" ]]; then
        ok "LLVM ${kind}: cached"; return 0
    fi
    if [[ ${FORCE} -eq 1 ]]; then
        rm -rf "${build}" "${prefix}"
    else
        rm -rf "${prefix}"
    fi
    mkdir -p "${build}"
    local args=(
        -S "${LLVM_SOURCE}/llvm" -B "${build}" -G Ninja
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="${prefix}"
        -DLLVM_ENABLE_ASSERTIONS=OFF -DLLVM_ENABLE_ZSTD=OFF
        -DLLVM_TARGETS_TO_BUILD="" -DLLVM_BUILD_TOOLS=OFF
        -DLLVM_BUILD_TESTS=OFF -DLLVM_INCLUDE_TESTS=OFF
        -DLLVM_BUILD_EXAMPLES=OFF -DLLVM_INCLUDE_EXAMPLES=OFF
        -DLLVM_BUILD_UTILS=OFF -DLLVM_INCLUDE_UTILS=OFF
        -DLLVM_INCLUDE_BENCHMARKS=OFF
        -DLLVM_ENABLE_PROJECTS=""
    )
    args+=( -DCMAKE_SYSTEM_NAME=Windows
            -DLLVM_HOST_TRIPLE=x86_64-w64-mingw32
            -DCMAKE_SYSROOT="${MINGW_ROOT}"
            -DCMAKE_C_COMPILER="${MINGW_BIN}/x86_64-w64-mingw32-gcc"
            -DCMAKE_CXX_COMPILER="${MINGW_BIN}/x86_64-w64-mingw32-g++" )
    if [[ -f "${build}/CMakeCache.txt" ]]; then
        ok "LLVM ${kind}: reconfiguring cached build tree"
    fi
    cmake "${args[@]}"
    cmake --build "${build}" --parallel "${JOBS}"
    cmake --install "${build}"
    [[ -f "${prefix}/lib/libLLVMCore.a" ]] || die "LLVM ${kind} produced no libLLVMCore.a."
    ok "LLVM ${kind}: ${prefix}"
}
build_llvm windows "${LLVM_WIN_BUILD}" "${LLVM_WIN}"

# This job produces only the guest PE DLLs. The pinned project otherwise also
# builds its macOS x86-64 airconv executable and Wine unixlib because the cross
# target is x86-64. Neither artifact enters the IPA: build-dxmt-ios-native.sh
# compiles the corresponding native half directly for iphoneos arm64. Disable
# those two host-only subdirectories so an Apple Silicon runner never attempts
# to link x86-64 helpers against ARM64 LLVM or the desktop Wine SDK.
AIRCONV_MESON="${DXMT_SOURCE}/src/airconv/meson.build"
WINEMETAL_MESON="${DXMT_SOURCE}/src/winemetal/meson.build"
require_file "${AIRCONV_MESON}"
require_file "${WINEMETAL_MESON}"
if grep -q "if cpu_family == 'x86_64' or dxmt_native" "${AIRCONV_MESON}"; then
    /usr/bin/sed -i '' \
        -e "s/if cpu_family == 'x86_64' or dxmt_native/if dxmt_native/" \
        "${AIRCONV_MESON}"
elif ! grep -q '^if dxmt_native$' "${AIRCONV_MESON}"; then
    die "Pinned DXMT airconv host-build condition changed."
fi
if grep -q "if cpu_family == 'x86_64'" "${WINEMETAL_MESON}"; then
    /usr/bin/sed -i '' \
        -e "s/if cpu_family == 'x86_64'/if dxmt_native/" \
        "${WINEMETAL_MESON}"
elif ! grep -q '^if dxmt_native$' "${WINEMETAL_MESON}"; then
    die "Pinned DXMT winemetal host-build condition changed."
fi
ok "DXMT PE build: desktop-native helper targets excluded"

# The memory census keeps a POINTER to the WMT::Device it is handed
# (mem_census_set_device(&device) in Buffer's constructor, where `device` is
# that constructor's own argument) and dereferences it at report time, long
# after the frame is gone. On device the census read a stale stack slot, sent
# currentAllocatedSize to a guest address, and objc_msgSend took the process
# down about thirty frames in. Keep the handle by value instead: a handle is
# a plain integer and outlives any frame. Guarded so a pinned-source change
# that moves these lines fails the build rather than shipping the defect.
CENSUS_SOURCE="${DXMT_SOURCE}/src/dxmt/dxmt_mem_census.cpp"
require_file "${CENSUS_SOURCE}"
if grep -q 'static WMT::Device \*g_census_device;' "${CENSUS_SOURCE}"; then
    /usr/bin/sed -i '' \
        -e 's/static WMT::Device \*g_census_device;/static obj_handle_t g_census_device_handle; \/* BoxedVN: by value, the pointer was to a constructor argument *\//' \
        -e 's/mem_census_set_device(WMT::Device \*d) { g_census_device = d; }/mem_census_set_device(WMT::Device *d) { g_census_device_handle = d ? d->handle : NULL_OBJECT_HANDLE; }/' \
        -e 's/uint64_t metal = g_census_device ? g_census_device->currentAllocatedSize() : 0;/uint64_t metal = g_census_device_handle != NULL_OBJECT_HANDLE ? MTLDevice_currentAllocatedSize(g_census_device_handle) : 0;/' \
        "${CENSUS_SOURCE}"
    grep -q 'g_census_device_handle = d ? d->handle' "${CENSUS_SOURCE}" \
        || die "Pinned DXMT memory census setter changed; the by-value device patch did not apply."
    grep -q 'MTLDevice_currentAllocatedSize(g_census_device_handle)' "${CENSUS_SOURCE}" \
        || die "Pinned DXMT memory census report changed; the by-value device patch did not apply."
elif ! grep -q 'g_census_device_handle' "${CENSUS_SOURCE}"; then
    die "Pinned DXMT memory census no longer keeps a device pointer; re-audit mem_census_set_device."
fi
ok "DXMT PE build: memory census keeps the device handle by value"

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
    # DXMT_IOS: iOS Metal has no Managed storage mode; DXMT's winemetal.h
    # remaps Managed to Shared under this define, but its meson build only
    # sets it for the aarch64-windows target. This x86-64 build runs on the
    # same iOS device through FEX, and without the define the first buffer
    # the guest creates asserts inside Metal ("Invalid storageMode").
    meson setup "${BUILD_DIR}" "${DXMT_SOURCE}" \
        --cross-file "${CROSS_FILE}" --native-file "${NATIVE_FILE}" \
        --buildtype release -Dwine_builtin_dll=true \
        -Dwine_install_path="${WINE_INSTALL}" \
        -Dbuild_airconv_for_windows=true -Ddxmt_native=false \
        -Denable_nvapi=false -Denable_nvngx=false \
        -Dc_args=-DDXMT_IOS=1 -Dcpp_args=-DDXMT_IOS=1
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
python3 "${BOXEDVN_ROOT}/scripts/validate-dxmt-guest-abi.py" \
    --pe-dir "${OUTPUT_DIR}/x86_64-windows"
ok "DXMT x86-64 PE DLLs: ${OUTPUT_DIR}/x86_64-windows"
