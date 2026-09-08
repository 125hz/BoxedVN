extern "C" {
#include "SoftFloat-3e/platform.h"
#include "SoftFloat-3e/softfloat.h"
FEXCORE_PRESERVE_ALL_ATTR extFloat80_t boxedvn_fex_reference_extF80_add(softfloat_state*,extFloat80_t,extFloat80_t);
FEXCORE_PRESERVE_ALL_ATTR extFloat80_t boxedvn_fex_reference_extF80_sub(softfloat_state*,extFloat80_t,extFloat80_t);
FEXCORE_PRESERVE_ALL_ATTR extFloat80_t boxedvn_fex_reference_extF80_mul(softfloat_state*,extFloat80_t,extFloat80_t);
}
#include <cstdio>
#include <cstdlib>
#include <random>
#include <chrono>

int main() {
    std::mt19937_64 rng(0x8077);
    uint64_t checked = 0;
    for (unsigned precision : {32u,64u,80u}) for (unsigned rounding : {0u,1u,2u,3u,4u}) {
        for (unsigned i=0;i<200000;++i) {
            uint64_t sa=rng()|1ull<<63, sb=rng()|1ull<<63;
            unsigned ea=1+rng()%32766, eb=1+rng()%32766;
            if (i%3) eb=unsigned(int(ea)+int(rng()%140)-70)&0x7fff;
            if (i%5==0) sb=sa;
            if (i%7==0) { sa=1ull<<63; sb=~0ull; }
            if (i%11==0) { sa&=~0x7ffull; sb&=~0x7ffull; }
            if (i%13==0) eb=ea;
            if (i%17==0) { ea=0;sa=1; }
            if (i%19==0) { ea=32767;sa=~0ull; }
            if (i%23==0) ea=16383;
            if (i%29==0) eb=16383;
            uint16_t xa=ea|((rng()&1)<<15), xb=eb|((rng()&1)<<15);
            extFloat80_t a{},b{};a.signif=sa;a.signExp=xa;b.signif=sb;b.signExp=xb;
            for(unsigned op=0;op<3;++op) {
                softfloat_state state{1,uint8_t(rounding),0,uint8_t(precision)};
                auto expected=op==0?boxedvn_fex_reference_extF80_add(&state,a,b):op==1?boxedvn_fex_reference_extF80_sub(&state,a,b):boxedvn_fex_reference_extF80_mul(&state,a,b);
                softfloat_state production{1,uint8_t(rounding),softfloat_flag_invalid,uint8_t(precision)};
                auto actual=op==0?extF80_add(&production,a,b):op==1?extF80_sub(&production,a,b):extF80_mul(&production,a,b);
                if(actual.signif!=expected.signif || actual.signExp!=expected.signExp ||
                   production.exceptionFlags!=(state.exceptionFlags|softfloat_flag_invalid)) {
                    std::printf("FAIL production wrapper p=%u r=%u op=%u\n",precision,rounding,op);return 1;
                }
                ++checked;
            }
        }
    }
    std::printf("PASS exact ext80 unity/reference results and flags: %llu\n",(unsigned long long)checked);
    // Deterministic ordinary physics-style magnitudes; timed calls include the
    // real preserve_all ABI. Not a device FPS claim.
    extFloat80_t inputs[2048]{};
    for(auto& v:inputs){v.signif=rng()|1ull<<63;v.signExp=uint16_t(16383+int(rng()%16)-8+((rng()&1)<<15));}
    volatile uint64_t checksum=0;
    for(unsigned op=0;op<3;++op) for(unsigned path=0;path<2;++path) {
        auto begin=std::chrono::steady_clock::now();
        softfloat_state state{1,0,0,80};
        for(unsigned n=0;n<3000000;++n){
            auto a=inputs[n&2047],b=inputs[(n*37+13)&2047];
            auto r=path ? (op==0?extF80_add(&state,a,b):op==1?extF80_sub(&state,a,b):extF80_mul(&state,a,b)) :
                (op==0?boxedvn_fex_reference_extF80_add(&state,a,b):op==1?boxedvn_fex_reference_extF80_sub(&state,a,b):boxedvn_fex_reference_extF80_mul(&state,a,b));
            checksum=checksum^r.signif;
        }
        double ns=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-begin).count()/3000000;
        std::printf("BENCH op=%u path=%s ns/call=%.2f\n",op,path?"unity":"reference",ns);
    }
}
