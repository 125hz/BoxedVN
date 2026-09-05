#!/usr/bin/env bash
# Rebuild the existing WoW64 DXVK modules with allocation-safe worker reports.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
source "${SCRIPT_DIR}/dependencies.dxvk.lock.sh"
SOURCE="${REPO_ROOT}/build/dxvk-source"
OUTPUT="${REPO_ROOT}/build/dxvk-i386"

if [[ ! -d "${SOURCE}/.git" ]]; then
    git init "${SOURCE}"
    git -C "${SOURCE}" remote add origin "${BOXEDVN_DXVK_REPOSITORY}"
fi
git -C "${SOURCE}" fetch --depth 1 origin "${BOXEDVN_DXVK_REVISION}"
git -C "${SOURCE}" fetch --depth 1 origin refs/tags/v2.5.2:refs/tags/v2.5.2
git -C "${SOURCE}" checkout --detach "${BOXEDVN_DXVK_REVISION}"
test "$(git -C "${SOURCE}" rev-parse HEAD)" = "${BOXEDVN_DXVK_REVISION}"
git -C "${SOURCE}" submodule update --init --recursive --depth 1
for patch in dxvk-2.5.2-moltenvk.patch dxvk-2.5.2-worker-errors.patch dxvk-2.5.2-allocation-errors.patch dxvk-2.5.2-null-vertex-buffer.patch; do
    path="${REPO_ROOT}/third_party/patches/${patch}"
    if ! git -C "${SOURCE}" apply --reverse --check --unidiff-zero "${path}" 2>/dev/null; then
        git -C "${SOURCE}" apply --check --unidiff-zero "${path}"
        git -C "${SOURCE}" apply --unidiff-zero "${path}"
    fi
done

# Explicit POSIX compiler: the win32 GCC flavour does not implement the C++
# mutex/condition_variable interfaces used by DXVK. Runtime libraries remain
# statically linked by DXVK's upstream Meson configuration.
sed -e "s/i686-w64-mingw32-gcc'/i686-w64-mingw32-gcc-posix'/" \
    -e "s/i686-w64-mingw32-g++'/i686-w64-mingw32-g++-posix'/" \
    "${SOURCE}/build-win32.txt" > "${SOURCE}/boxedvn-win32.txt"
setup_args=()
if [[ -f "${SOURCE}/build-boxedvn/meson-private/coredata.dat" ]]; then
    setup_args=(--reconfigure)
fi
meson setup "${setup_args[@]}" "${SOURCE}/build-boxedvn" "${SOURCE}" \
    --cross-file "${SOURCE}/boxedvn-win32.txt" --buildtype release \
    -Denable_d3d8=false -Dbuild_id=true
meson compile -C "${SOURCE}/build-boxedvn" -j "${BOXEDVN_DXVK_JOBS:-4}"
mkdir -p "${OUTPUT}"
for module in d3d9 d3d11 dxgi d3d10core; do
    dll="$(find "${SOURCE}/build-boxedvn/src" -name "${module}.dll" -print -quit)"
    test -s "${dll}"
    cp "${dll}" "${OUTPUT}/${module}.dll"
    i686-w64-mingw32-strip --strip-debug "${OUTPUT}/${module}.dll"
done
{
    printf 'revision=%s\n' "${BOXEDVN_DXVK_REVISION}"
    printf 'compiler=%s\n' "$(i686-w64-mingw32-g++-posix -dumpfullversion)"
    sha256sum "${REPO_ROOT}"/third_party/patches/dxvk-2.5.2-*.patch
    sha256sum "${OUTPUT}"/*.dll
} > "${OUTPUT}/build-manifest.txt"
python3 "${SCRIPT_DIR}/validate-dxvk-i386.py" "${OUTPUT}"
