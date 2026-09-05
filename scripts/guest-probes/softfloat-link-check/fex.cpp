// FEXCore includes the C library this way. Verify C++ callers receive the
// same linker aliases as the C definitions, including the state parameter.
extern "C" {
#define check_fex_softfloat check_fex_cpp_softfloat
#include "fex.c"
}
