#include "platform.h"
#include "softfloat.h"
int check_legacy_softfloat(void) {
    const extFloat80_t eleven = i32_to_extF80(11);
    return extF80_to_f64(eleven).v == 0x4026000000000000ULL;
}
