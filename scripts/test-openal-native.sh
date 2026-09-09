#!/usr/bin/env bash
# Native output and ABI boundary tests; loopback never opens host speakers.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
source_dir="${1:-${root}/build/openal-test-source}"
build_dir="${2:-${root}/build/openal-test}"
if [[ ! -d "${source_dir}/.git" ]]; then
    git clone --depth 1 --branch 1.24.3 https://github.com/kcat/openal-soft.git "${source_dir}"
fi
[[ "$(git -C "${source_dir}" rev-parse HEAD)" == dc7d7054a5b4f3bec1dc23a42fd616a0847af948 ]]
python3 "${root}/scripts/generate-openal-bridge.py" --source "${source_dir}" --output "${build_dir}/generated"
cmake -S "${source_dir}" -B "${build_dir}/library" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DLIBTYPE=STATIC -DALSOFT_BACKEND_WAVE=OFF -DALSOFT_BACKEND_NULL=OFF -DALSOFT_BACKEND_COREAUDIO=OFF \
    -DALSOFT_BACKEND_ALSA=OFF -DALSOFT_BACKEND_PULSEAUDIO=OFF -DALSOFT_BACKEND_PIPEWIRE=OFF \
    -DALSOFT_BACKEND_JACK=OFF -DALSOFT_BACKEND_OSS=OFF -DALSOFT_BACKEND_SNDIO=OFF \
    -DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF -DALSOFT_TESTS=OFF -DALSOFT_INSTALL=OFF -DALSOFT_EAX=OFF
cmake --build "${build_dir}/library" --target OpenAL -j 4
extra=(-pthread)
[[ "$(uname -s)" == Linux ]] && extra+=(-ldl)
"${CXX:-c++}" -std=c++20 -O2 -Wall -Wextra -Werror \
    -I"${root}/include" -I"${source_dir}/include" -I"${build_dir}/generated" \
    "${root}/scripts/tests/openal_native.cpp" "${build_dir}/library/libopenal.a" \
    "${extra[@]}" -o "${build_dir}/test-openal"
"${build_dir}/test-openal"
