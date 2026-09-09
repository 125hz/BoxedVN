#pragma once
#include <stdint.h>
#include <stddef.h>
#define BOXEDWINE_OPENAL_HOSTCALL 0x7fff0004u
#define BOXEDWINE_OPENAL_ABI 1u
typedef struct BvnOpenALPacket {
    uint64_t abi;
    uint64_t args[8];
    uint64_t result;
} BvnOpenALPacket;
typedef void* (*BvnOpenALMap)(void*, uint64_t, size_t, int);
#ifdef __cplusplus
extern "C" {
#endif
int bvnOpenALInvoke(uint64_t generation, uint32_t operation, BvnOpenALPacket*, BvnOpenALMap, void*, int outputEnabled);
void bvnOpenALRetire(uint64_t generation);
#ifdef __cplusplus
}
#endif
