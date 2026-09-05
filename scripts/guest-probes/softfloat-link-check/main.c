#include <stdio.h>
int check_fex_softfloat(void);
int check_fex_cpp_softfloat(void);
int check_legacy_softfloat(void);
int main(void) {
    const int legacy = check_legacy_softfloat();
    const int fex = check_fex_softfloat();
    const int cpp = check_fex_cpp_softfloat();
    printf("SoftFloat co-link: BoxedWine=%d FEX=%d FEX-C++=%d\n", legacy, fex, cpp);
    return legacy && fex && cpp ? 0 : 1;
}
