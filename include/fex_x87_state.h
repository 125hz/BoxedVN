/* Pinned FEX CPUState x87 flags to architectural FSW. GPLv2. */
#pragma once
#include <cstdint>

namespace boxedvn {
inline uint16_t fexX87Status(const uint8_t* flags) {
    uint16_t status = 0;
    for (unsigned i = 0; i < 11; ++i) status |= uint16_t(flags[32 + i] & 1) << i;
    status |= uint16_t(flags[43] & 7) << 11;
    status |= uint16_t(flags[46] & 1) << 14;
    status |= uint16_t(flags[47] & 1) << 15;
    return status;
}
inline void setFexX87Status(uint8_t* flags, uint16_t status) {
    for (unsigned i = 0; i < 11; ++i) flags[32 + i] = (status >> i) & 1;
    flags[43] = (status >> 11) & 7;
    flags[46] = (status >> 14) & 1;
    flags[47] = (status >> 15) & 1;
}
} // namespace boxedvn
