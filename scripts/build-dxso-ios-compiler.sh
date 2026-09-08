#!/usr/bin/env bash
# Build the D3D9 shader compiler and WoW64 bridge for iPhoneOS. This produces
# a port-development library, never a replacement for the shipping DXMT.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
source_dir="${1:?pinned D3D9 source directory required}"
fex_dir="${2:-${root}/third_party/fex64}"
output="${root}/build/dxso-ios"
pin="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["commit"])' "${root}/scripts/dependencies.d3d9-metal.lock.json")"
[[ "$(git -C "${source_dir}" rev-parse HEAD)" == "${pin}" ]] || { echo 'D3D9 source pin mismatch' >&2; exit 1; }
[[ -z "$(git -C "${source_dir}" status --porcelain --untracked-files=no)" ]] || { echo 'D3D9 source must be pristine' >&2; exit 1; }
sdk="$(xcrun --sdk iphoneos --show-sdk-path)"
llvm_build="${fex_dir}/toolchains/llvm-ios-build"
llvm_source="${fex_dir}/llvm-project/llvm"
mkdir -p "${output}/obj" "${output}/shaders"
flags=(-arch arm64 -isysroot "${sdk}" -miphoneos-version-min=17.0 -O2
       -DDXMT_NATIVE=1 -DDXMT_IOS=1 -fblocks -std=c++20 -fno-rtti
       -I"${source_dir}/include" -I"${source_dir}/libs"
       -I"${source_dir}/include/native/directx" -I"${source_dir}/include/native/windows"
       -I"${source_dir}/src/airconv" -I"${source_dir}/src/winemetal"
       -I"${llvm_build}/include" -I"${llvm_source}/include"
       -I"${output}/shaders" -I"${root}/include")
for shader in air_msad air_samplepos air_tessellation; do
    xcrun -sdk macosx metal -c "${source_dir}/src/airconv/shaders/${shader}.metal" \
        -o "${output}/shaders/${shader}.air" -std=metal3.1 --target=air64-apple-macos14.0
    xxd -n "${shader}" -i "${output}/shaders/${shader}.air" "${output}/shaders/${shader}.h"
done
objects=()
while IFS= read -r source_file; do
    object="${output}/obj/${source_file//\//_}.o"
    xcrun -sdk iphoneos clang++ "${flags[@]}" -fno-exceptions \
        -c "${source_dir}/src/airconv/${source_file}" -o "${object}"
    objects+=("${object}")
done < <(python3 - "${source_dir}" <<'PY'
import pathlib,re,sys
source=pathlib.Path(sys.argv[1], 'src/airconv/meson.build').read_text()
block=source.split('airconv_src = files([',1)[1].split('])',1)[0]
files=re.findall(r"'([^']+\.cpp)'",block)
assert 'dxso_compile.cpp' in files and 'ffp_compile.cpp' in files
print('\n'.join(files))
PY
)
for source_file in BlobContainer DXBCUtils ShaderBinary; do
    object="${output}/obj/${source_file}.o"
    xcrun -sdk iphoneos clang++ "${flags[@]}" -c "${source_dir}/libs/DXBCParser/${source_file}.cpp" -o "${object}"
    objects+=("${object}")
done
bridge="${output}/obj/boxedwine_dxmt_dxso_bridge.o"
xcrun -sdk iphoneos clang++ "${flags[@]}" -c "${root}/tools/dxmt/boxedwine_dxmt_dxso_bridge.cpp" -o "${bridge}"
objects+=("${bridge}")
xcrun -sdk iphoneos libtool -static -o "${output}/libdxso_compiler.a" "${objects[@]}"
# Resolve the whole compiler against the same LLVM used on iOS. Compiling
# individual objects alone would miss missing codegen operations and symbols.
xcrun -sdk iphoneos clang++ "${flags[@]}" -dynamiclib "${objects[@]}" \
    "${llvm_build}"/lib/*.a -lz -lcurses -framework Foundation \
    -o "${output}/dxso-link-check.dylib"
xcrun nm -gU "${output}/dxso-link-check.dylib" > "${output}/symbols.txt"
for symbol in DXSOInitialize DXSOCompile DXSOGetCompiledBitcode boxedwine_dxmt_dxso_call; do
    grep -q " _${symbol}$" "${output}/symbols.txt"
done
printf 'Source: %s\nTarget: arm64-apple-ios17\nRuntime enabled: no\n' "${pin}" > "${output}/manifest.txt"
echo 'D3D9 shader compiler and guest bridge compiled and linked for iPhoneOS.'
