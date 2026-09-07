/* Copyright (C) 2026 The BoxedWine Team. GPL-2.0-or-later. */
#pragma once

#include <SDL.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

// Keep the resampler's phase and filter history across OSS writes. Converting
// each packet independently inserts a discontinuity at every packet boundary.
class SdlAudioConverter {
public:
    ~SdlAudioConverter() { reset(); }
    SdlAudioConverter() = default;
    SdlAudioConverter(const SdlAudioConverter&) = delete;
    SdlAudioConverter& operator=(const SdlAudioConverter&) = delete;

    void reset() {
        if (stream) SDL_FreeAudioStream(stream);
        stream = nullptr;
        inputFrames = representedInputFrames = 0;
        pendingBytes.store(0, std::memory_order_relaxed);
    }
    bool open(const SDL_AudioSpec& in, const SDL_AudioSpec& out) {
        reset();
        inputRate = in.freq; outputRate = out.freq;
        inputFrameSize = in.channels * SDL_AUDIO_BITSIZE(in.format) / 8;
        outputFrameSize = out.channels * SDL_AUDIO_BITSIZE(out.format) / 8;
        silenceFrame.assign(inputFrameSize, in.format == AUDIO_U8 ? 0x80 : 0);
        if (!SDL_AUDIO_ISSIGNED(in.format) && SDL_AUDIO_BITSIZE(in.format) == 16) {
            for (unsigned channel = 0; channel < in.channels; ++channel)
                silenceFrame[channel * 2 + (SDL_AUDIO_ISBIGENDIAN(in.format) ? 0 : 1)] = 0x80;
        }
        stream = SDL_NewAudioStream(in.format, in.channels, in.freq,
                                    out.format, out.channels, out.freq);
        return stream != nullptr;
    }
    bool put(const void* data, unsigned bytes) {
        if (SDL_AudioStreamPut(stream, data, bytes) < 0) return false;
        inputFrames += bytes / inputFrameSize;
        publishPending();
        return true;
    }
    int get(std::vector<uint8_t>& output) {
        int available = SDL_AudioStreamAvailable(stream);
        if (available <= 0) return available;
        output.resize(available);
        int result = SDL_AudioStreamGet(stream, output.data(), available);
        if (result > 0) {
            // SDL2 rounds each output block down to whole frames. Reverse that
            // rounding per block; cumulative rate conversion would accumulate
            // the discarded fractions as imaginary queued input over time.
            uint64_t frames = result / outputFrameSize;
            representedInputFrames += (frames * inputRate + outputRate - 1) / outputRate;
            representedInputFrames = std::min(inputFrames, representedInputFrames);
            publishPending();
        }
        return result;
    }
    bool finish() {
        // SDL2 only flushes retained filter samples when its staging buffer is
        // nonempty. A final silent frame also drains a block-aligned stream.
        if (inputRate != outputRate && SDL_AudioStreamPut(stream, silenceFrame.data(), inputFrameSize) < 0) return false;
        return SDL_AudioStreamFlush(stream) == 0;
    }
    uint64_t bufferedInputBytes() const {
        // Include input retained for the resampling filter, even when SDL has
        // no output ready yet. OSS must not report that input as played.
        return pendingBytes.load(std::memory_order_relaxed);
    }
private:
    void publishPending() {
        pendingBytes.store((inputFrames - representedInputFrames) * inputFrameSize, std::memory_order_relaxed);
    }
    std::atomic<uint64_t> pendingBytes{0};
    SDL_AudioStream* stream = nullptr;
    std::vector<uint8_t> silenceFrame;
    uint64_t inputFrames = 0, representedInputFrames = 0;
    unsigned inputRate = 1, outputRate = 1, inputFrameSize = 1, outputFrameSize = 1;
};
