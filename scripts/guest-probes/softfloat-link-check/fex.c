#include "platform.h"
#include "softfloat.h"
int check_fex_softfloat(void) {
    struct softfloat_state state = {0};
    state.roundingPrecision = 64;
    const extFloat80_t eleven = i32_to_extF80(11);
    const float64_t converted = extF80_to_f64(&state, eleven);
    const float64_t tiny = {1};
    const float32_t scale = {0x5f800000};
    const extFloat80_t product = extF80_mul(&state, f64_to_extF80(&state, tiny),
                                           f32_to_extF80(&state, scale));
    return converted.v == 0x4026000000000000ULL &&
           extF80_to_f64(&state, product).v == 0x00d0000000000000ULL &&
           extF80_to_i32(&state, eleven, softfloat_round_near_even, 0) == 11;
}
