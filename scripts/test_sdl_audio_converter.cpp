// Signal-level regression for the production OSS converter; link SDL2.
#include "sdl_audio_converter.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

int main() {
    SDL_AudioSpec in{}, out{};
    in.format = out.format = AUDIO_F32SYS;
    in.channels = out.channels = 2;
    in.freq = 44100; out.freq = 48000;
    for (int packet : {441, 1024, 2048}) {
        SdlAudioConverter converter;
        assert(converter.open(in, out));
        std::vector<float> input(44100 * 2), result;
        std::vector<uint8_t> output;
        for (int i = 0; i < 44100; ++i)
        {
            input[i * 2] = .5f;
            input[i * 2 + 1] = .5f * std::sin(i * 440.0 * 6.283185307179586 / 44100);
        }
        uint64_t submitted = 0;
        auto drain = [&] {
            int count = converter.get(output);
            assert(count >= 0 && count % 8 == 0);
            size_t start = result.size(); result.resize(start + count / 4);
            if (count) std::memcpy(result.data() + start, output.data(), count);
        };
        for (int offset = 0; offset < 44100; offset += packet) {
            int frames = std::min(packet, 44100 - offset);
            assert(converter.put(input.data() + offset * 2, frames * 8));
            submitted += frames * 8;
            drain();
            // Filter lookahead must remain in the guest's pending queue.
            assert(converter.bufferedInputBytes() <= 16384);
            assert(converter.bufferedInputBytes() <= submitted);
#ifdef BOXEDVN_LOW_LATENCY_RESAMPLER
            // Short Wine buffers must not lose 512 frames to filter lookahead.
            if (offset > 4096) assert(converter.bufferedInputBytes() <= 16 * 8);
#endif
        }
        assert(converter.finish()); drain();
        // SDL2 rounds per internal resampling block; allow <1ms over 1s.
        assert(std::abs((int)result.size() / 2 - 48000) < 48);
        double squared = 0; float maxStep = 0;
        for (size_t i = 100; i + 100 < result.size() / 2; ++i) {
            double reference = .5;
            squared += std::pow(result[i * 2] - reference, 2);
            maxStep = std::max(maxStep, std::abs(result[i * 2 + 1] - result[(i - 1) * 2 + 1]));
        }
        double rms = std::sqrt(squared / (result.size() / 2 - 200));
        printf("packet=%d frames=%zu rms=%g max_step=%g pending=%llu\n", packet,
               result.size() / 2, rms, maxStep, (unsigned long long)converter.bufferedInputBytes());
        assert(rms < .005 && maxStep < .06);
        assert(converter.bufferedInputBytes() < 16);
        converter.reset(); assert(converter.bufferedInputBytes() == 0);
        in.freq = out.freq;
        assert(converter.open(in, out));
        assert(converter.put(input.data(), 800)); drain();
        assert(converter.bufferedInputBytes() == 0);
        in.freq = 44100;
    }
    // Pending input must not grow with session duration from rate-rounding.
    for (int rate : {22050, 44100, 96000}) {
        in.freq = rate;
        SdlAudioConverter converter;
        assert(converter.open(in, out));
        std::vector<float> silence(2048 * 2);
        std::vector<uint8_t> output;
        for (int i = 0; i < rate * 30 / 2048; ++i) {
            assert(converter.put(silence.data(), silence.size() * sizeof(float)));
            assert(converter.get(output) >= 0);
            assert(converter.bufferedInputBytes() <= 16384);
        }
        printf("rate=%d pending=%llu\n", rate, (unsigned long long)converter.bufferedInputBytes());
    }
}
