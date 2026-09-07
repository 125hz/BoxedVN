#pragma once
#include <cstdint>
#include <algorithm>

// OSS count_info describes consumed bytes and fragment transitions, not
// submitted bytes or queue length. Keep a 64-bit cursor across ABI wraparound.
class OssPlaybackPosition {
public:
    struct Result { uint32_t bytes, blocks, pointer; };
    Result query(uint64_t written, uint64_t queued, uint32_t fragment, uint32_t capacity) {
        played = std::max(played, written > queued ? written - queued : 0);
        const uint64_t blocks = fragment ? played / fragment : 0;
        Result result{static_cast<uint32_t>(played),
            static_cast<uint32_t>(blocks >= previousBlocks ? blocks - previousBlocks : 0),
            capacity ? static_cast<uint32_t>(played % capacity) : 0};
        previousBlocks = blocks;
        return result;
    }
    void reset() { played = previousBlocks = 0; }
private:
    uint64_t played = 0, previousBlocks = 0;
};
