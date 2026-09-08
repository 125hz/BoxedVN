// Forced into FEX translation units only; BoxedWine keeps its own VM policies.
#pragma once
#if defined(__APPLE__)
#include <sys/mman.h>
#ifdef __cplusplus
extern "C" {
#endif
int boxedvn_fex_madvise(void*, size_t, int);
#ifdef __cplusplus
}
#endif
#define madvise boxedvn_fex_madvise
#endif
