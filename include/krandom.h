/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef __KRANDOM_H__
#define __KRANDOM_H__

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <random>

namespace KRandom {

enum class Source {
    System,
    Fallback,
    Invalid,
};

inline bool allZero(const std::uint8_t* bytes, std::size_t length) {
    for (std::size_t i = 0; i < length; ++i) {
        if (bytes[i] != 0) return false;
    }
    return true;
}

// Fill a host buffer with startup/runtime entropy. std::random_device maps to
// the platform entropy service on the supported hosts. The fallback exists so
// a host entropy failure cannot turn an otherwise valid guest launch into a
// crash or advertise an all-zero AT_RANDOM value to the dynamic loader.
inline Source fill(void* destination, std::size_t length) {
    if (length == 0) return Source::System;
    if (!destination) return Source::Invalid;

    auto* bytes = static_cast<std::uint8_t*>(destination);
    bool systemFilled = false;
    try {
        static std::mutex entropyMutex;
        static std::random_device entropy;
        std::lock_guard<std::mutex> lock(entropyMutex);
        std::size_t offset = 0;
        while (offset < length) {
            const std::random_device::result_type value = entropy();
            for (std::size_t byte = 0;
                 byte < sizeof(value) && offset < length;
                 ++byte, ++offset) {
                bytes[offset] = static_cast<std::uint8_t>(value >> (byte * 8));
            }
        }
        systemFilled = !allZero(bytes, length);
    } catch (...) {
        systemFilled = false;
    }
    if (systemFilled) return Source::System;

    // SplitMix64 is not presented as a cryptographic source. It is a
    // process-unique emergency fallback seeded from time, destination address,
    // and a monotonic counter, and its use is surfaced to the caller for a
    // bounded diagnostic.
    static std::atomic<std::uint64_t> fallbackCounter{0};
    std::uint64_t state = static_cast<std::uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    state ^= static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(destination));
    state ^= fallbackCounter.fetch_add(0x9E3779B97F4A7C15ULL,
                                       std::memory_order_relaxed);
    state ^= 0xD1B54A32D192ED03ULL;

    std::size_t offset = 0;
    while (offset < length) {
        state += 0x9E3779B97F4A7C15ULL;
        std::uint64_t value = state;
        value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
        value ^= value >> 31;
        for (std::size_t byte = 0;
             byte < sizeof(value) && offset < length;
             ++byte, ++offset) {
            bytes[offset] = static_cast<std::uint8_t>(value >> (byte * 8));
        }
    }
    if (allZero(bytes, length)) bytes[0] = 1;
    return Source::Fallback;
}

} // namespace KRandom

#endif
