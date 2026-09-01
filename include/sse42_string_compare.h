/*
 * BoxedWine - SSE4.2 packed string compare (PCMPESTRI/PCMPESTRM/PCMPISTRI/
 * PCMPISTRM) semantics, host independent.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 *
 * glibc's SSE4.2 strcmp, strcspn, strspn and strstr variants are selected by
 * IFUNC whenever CPUID reports SSE4.2, which the translated guest does. A
 * forked child runs in the 64-bit interpreter until it execs, and a device
 * run stopped there the first time such a child compared a string:
 *
 *   CPU64: unimpl opcode at RIP=... bytes=66 0f 3a 63 04 16 1a
 *
 * That is PCMPISTRI with the immediate glibc's strcmp uses. The four
 * instructions share one algorithm (Intel SDM vol. 2, "Packed Compare
 * Implicit/Explicit Length Strings"); it lives here so it can be unit
 * tested against the architectural definition rather than only on a device.
 */

#ifndef __SSE42_STRING_COMPARE_H__
#define __SSE42_STRING_COMPARE_H__

#include <cstdint>
#include <cstdlib>

namespace boxedvn {

struct Sse42StringResult {
    // IntRes2: one bit per element of the second operand.
    uint32_t mask = 0;
    // The index form's ECX: first/last set bit of `mask`, or the element
    // count when no bit is set.
    uint32_t index = 0;
    // The mask form's XMM0, as sixteen bytes.
    uint8_t xmm0[16] = {};
    bool cf = false; // IntRes2 != 0
    bool zf = false; // second operand shorter than the register
    bool sf = false; // first operand shorter than the register
    bool of = false; // IntRes2 bit 0
};

// Element count for the immediate's data-size field.
inline uint32_t sse42StringElementCount(uint8_t imm) {
    return (imm & 0x01) ? 8u : 16u;
}

// Length of an explicit operand: |value| saturated to the element count.
// `wide` selects the 64-bit register; otherwise the low 32 bits are the
// signed length.
inline uint32_t sse42StringExplicitLength(uint64_t value, bool wide, uint8_t imm) {
    const uint32_t count = sse42StringElementCount(imm);
    int64_t signedValue = wide ? (int64_t)value : (int64_t)(int32_t)(uint32_t)value;
    uint64_t magnitude;
    if (signedValue < 0) {
        magnitude = (uint64_t)(-(signedValue + 1)) + 1u;
    } else {
        magnitude = (uint64_t)signedValue;
    }
    return magnitude > count ? count : (uint32_t)magnitude;
}

// Length of an implicit operand: elements before the first zero element.
inline uint32_t sse42StringImplicitLength(const uint8_t* bytes, uint8_t imm) {
    const uint32_t count = sse42StringElementCount(imm);
    for (uint32_t i = 0; i < count; i++) {
        if (imm & 0x01) {
            if (bytes[i * 2] == 0 && bytes[i * 2 + 1] == 0) {
                return i;
            }
        } else if (bytes[i] == 0) {
            return i;
        }
    }
    return count;
}

namespace detail {

inline int64_t sse42Element(const uint8_t* bytes, uint32_t i, uint8_t imm) {
    if (imm & 0x01) {
        const uint16_t raw = (uint16_t)(bytes[i * 2] | (bytes[i * 2 + 1] << 8));
        return (imm & 0x02) ? (int64_t)(int16_t)raw : (int64_t)raw;
    }
    const uint8_t raw = bytes[i];
    return (imm & 0x02) ? (int64_t)(int8_t)raw : (int64_t)raw;
}

} // namespace detail

// The compare. `first` is the register operand (xmm1, "reg"), `second` the
// xmm2/m128 operand ("rm"). Lengths are element counts, already resolved.
inline Sse42StringResult sse42StringCompare(const uint8_t* first, uint32_t firstLength,
                                            const uint8_t* second, uint32_t secondLength,
                                            uint8_t imm) {
    Sse42StringResult result;
    const uint32_t count = sse42StringElementCount(imm);
    if (firstLength > count) firstLength = count;
    if (secondLength > count) secondLength = count;
    const uint32_t aggregation = (imm >> 2) & 0x03;

    uint32_t intRes1 = 0;
    for (uint32_t j = 0; j < count; j++) {
        bool bit = false;
        const bool bValid = j < secondLength;
        switch (aggregation) {
        case 0: // equal any: any element of first equals second[j]
            if (bValid) {
                for (uint32_t i = 0; i < firstLength; i++) {
                    if (detail::sse42Element(first, i, imm) == detail::sse42Element(second, j, imm)) {
                        bit = true;
                        break;
                    }
                }
            }
            break;
        case 1: // ranges: second[j] within any (first[2k], first[2k+1])
            if (bValid) {
                const int64_t b = detail::sse42Element(second, j, imm);
                for (uint32_t k = 0; k + 1 < firstLength; k += 2) {
                    const int64_t lo = detail::sse42Element(first, k, imm);
                    const int64_t hi = detail::sse42Element(first, k + 1, imm);
                    if (lo <= b && b <= hi) {
                        bit = true;
                        break;
                    }
                }
            }
            break;
        case 2: { // equal each: first[j] == second[j]; both invalid counts as equal
            const bool aValid = j < firstLength;
            if (aValid && bValid) {
                bit = detail::sse42Element(first, j, imm) == detail::sse42Element(second, j, imm);
            } else {
                bit = !aValid && !bValid;
            }
            break;
        }
        default: { // equal ordered: first is a substring of second starting at j
            bit = true;
            for (uint32_t i = 0; i < count; i++) {
                const bool aValid = i < firstLength;
                if (!aValid) {
                    break; // remaining first elements are invalid: forced true
                }
                const uint32_t jj = j + i;
                if (jj >= count || jj >= secondLength) {
                    bit = false; // first valid, second invalid: forced false
                    break;
                }
                if (detail::sse42Element(first, i, imm) != detail::sse42Element(second, jj, imm)) {
                    bit = false;
                    break;
                }
            }
            break;
        }
        }
        if (bit) {
            intRes1 |= 1u << j;
        }
    }

    const uint32_t allBits = count == 16 ? 0xFFFFu : 0xFFu;
    uint32_t intRes2 = intRes1;
    switch ((imm >> 4) & 0x03) {
    case 1: // negative polarity
        intRes2 = (~intRes1) & allBits;
        break;
    case 3: // masked negative: invert only the valid elements of second
        intRes2 = intRes1 ^ ((secondLength >= count) ? allBits : ((1u << secondLength) - 1u));
        break;
    default:
        break;
    }
    intRes2 &= allBits;

    result.mask = intRes2;
    if (intRes2 == 0) {
        result.index = count;
    } else if (imm & 0x40) {
        uint32_t last = 0;
        for (uint32_t j = 0; j < count; j++) {
            if (intRes2 & (1u << j)) last = j;
        }
        result.index = last;
    } else {
        uint32_t firstBit = 0;
        while (!(intRes2 & (1u << firstBit))) firstBit++;
        result.index = firstBit;
    }
    if (imm & 0x40) {
        // Byte or word mask.
        for (uint32_t j = 0; j < count; j++) {
            const bool set = (intRes2 & (1u << j)) != 0;
            if (count == 16) {
                result.xmm0[j] = set ? 0xFF : 0x00;
            } else {
                result.xmm0[j * 2] = set ? 0xFF : 0x00;
                result.xmm0[j * 2 + 1] = set ? 0xFF : 0x00;
            }
        }
    } else {
        result.xmm0[0] = (uint8_t)(intRes2 & 0xFF);
        result.xmm0[1] = (uint8_t)((intRes2 >> 8) & 0xFF);
    }
    result.cf = intRes2 != 0;
    result.zf = secondLength < count;
    result.sf = firstLength < count;
    result.of = (intRes2 & 1u) != 0;
    return result;
}

} // namespace boxedvn

#endif
