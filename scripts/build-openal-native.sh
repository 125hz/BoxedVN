#!/usr/bin/env bash
# Build the shared ARM64 mixer and both Windows API facades from one API list.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
mingw="${1:?llvm-mingw directory required}"
pe_output="${2:?PE output directory required}"
native_output="${3:?native archive output required}"
source_dir="${root}/third_party/openal-soft"
build_dir="${root}/build/openal-ios"
pin=dc7d7054a5b4f3bec1dc23a42fd616a0847af948 # OpenAL Soft 1.24.3, LGPL-2.0-or-later
if [[ ! -d "${source_dir}/.git" ]]; then
    git clone --depth 1 --branch 1.24.3 https://github.com/kcat/openal-soft.git "${source_dir}"
fi
[[ "$(git -C "${source_dir}" rev-parse HEAD)" == "${pin}" ]] || { echo 'OpenAL source pin mismatch' >&2; exit 1; }
mkdir -p "${build_dir}/generated"
python3 "${root}/scripts/generate-openal-bridge.py" --source "${source_dir}" --output "${build_dir}/generated"
for arch in i686 x86_64; do
    output_arch=i386; [[ "${arch}" == x86_64 ]] && output_arch=x86_64
    mkdir -p "${pe_output}/${output_arch}-windows"
    "${mingw}/bin/${arch}-w64-mingw32-clang++" -std=c++17 -O2 -shared -static -static-libgcc -static-libstdc++ \
        -I"${root}/include" -I"${source_dir}/include" \
        "${build_dir}/generated/openal_guest.cpp" "${build_dir}/generated/openal32.def" \
        -o "${pe_output}/${output_arch}-windows/openal32.dll"
    # Same 32-byte DOS-header marker written by winebuild --builtin. It makes
    # Wine's builtin-first override select this projected architecture module.
    python3 - "${pe_output}/${output_arch}-windows/openal32.dll" <<'PY'
from pathlib import Path
import struct,sys
p=Path(sys.argv[1]); data=bytearray(p.read_bytes())
assert data[:2]==b'MZ' and struct.unpack_from('<I',data,60)[0]>=96
data[64:96]=b'Wine builtin DLL'.ljust(32,b'\0')
p.write_bytes(data)
PY
done
sdk="$(xcrun --sdk iphoneos --show-sdk-path)"
cmake -S "${source_dir}" -B "${build_dir}/library" -G Ninja \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="${sdk}" -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 -DCMAKE_BUILD_TYPE=Release -DLIBTYPE=STATIC \
    -DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF -DALSOFT_TESTS=OFF -DALSOFT_INSTALL=OFF \
    -DALSOFT_EAX=OFF -DALSOFT_BACKEND_COREAUDIO=ON -DALSOFT_REQUIRE_COREAUDIO=ON -DALSOFT_BACKEND_WAVE=OFF
cmake --build "${build_dir}/library" --target OpenAL -j 4
xcrun --sdk iphoneos clang++ -std=c++20 -O3 -arch arm64 -isysroot "${sdk}" -miphoneos-version-min=16.0 \
    -I"${root}/include" -I"${source_dir}/include" -I"${build_dir}/generated" \
    -c "${root}/tools/openal/openal_native.cpp" -o "${build_dir}/openal_native.o"
xcrun --sdk iphoneos clang++ -std=c++17 -O2 -fobjc-arc -arch arm64 -isysroot "${sdk}" -miphoneos-version-min=16.0 \
    -c "${root}/tools/openal/openal_session.mm" -o "${build_dir}/openal_session.o"
mkdir -p "$(dirname "${native_output}")"
xcrun --sdk iphoneos libtool -static -o "${native_output}" \
    "${build_dir}/openal_native.o" "${build_dir}/openal_session.o" "${build_dir}/library/libopenal.a"
echo 'Native OpenAL mixer and PE32/PE64 facades built'
