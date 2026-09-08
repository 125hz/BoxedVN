#!/usr/bin/env bash
# Test the pinned, patched converter itself, including waveform continuity.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
source "${root}/scripts/dependencies.lock.sh"
work="${root}/build/sdl-resampler-test"
mkdir -p "${work}"
curl --fail --location --retry 3 "${BOXEDVN_SDL2_URL}" -o "${work}/SDL2.tar.gz"
printf '%s  %s\n' "${BOXEDVN_SDL2_SHA256}" "${work}/SDL2.tar.gz" | sha256sum --check
tar -xzf "${work}/SDL2.tar.gz" -C "${work}"
sdl="${work}/SDL2-${BOXEDVN_SDL2_VERSION}"
patch --batch --forward -d "${sdl}" -p1 < "${root}/scripts/sdl-patches/sdl2-resampler-lookahead.patch"
cmake -S "${sdl}" -B "${work}/host" -DCMAKE_BUILD_TYPE=Release \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF -DSDL_LIBSAMPLERATE=OFF \
    -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_ALSA=OFF \
    -DSDL_PULSEAUDIO=OFF -DSDL_PIPEWIRE=OFF -DSDL_JACK=OFF
cmake --build "${work}/host" --parallel 4 --target SDL2-static
c++ -std=c++17 -DBOXEDVN_LOW_LATENCY_RESAMPLER -I"${root}/include" \
    -I"${sdl}/include" -I"${work}/host/include" \
    "${root}/scripts/test_sdl_audio_converter.cpp" "${work}/host/libSDL2.a" \
    -pthread -ldl -lm -o "${work}/test"
"${work}/test"
