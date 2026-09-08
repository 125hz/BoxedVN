#include "fex_dispatch_exit.h"
#include "fex_unaligned_swap.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <sys/mman.h>
using namespace boxedvn;
static uint32_t* code;
static void wrapper() {
    // Save the host ABI, then load arbitrary values into ALL 31 registers.
    unsigned n=0;auto emit=[&](uint32_t op){code[n++]=op;};
    emit(0xd10203ff); // sub sp,sp,#128
    for(unsigned r=19;r<31;r+=2)
        emit(0xa9000000 | ((r-19)<<15) | ((r+1)<<10) | (31<<5) | r);
    emit(0xf90033e1); // str x1,[sp,#96] output pointer
    emit(0xaa0003f0); // mov x16,x0 input pointer
    emit(0xf9407e0f); // ldr x15,[x16,#248] flags
    emit(0xd51b420f); // msr nzcv,x15
    for(unsigned r=0;r<31;++r)if(r!=16)emit(0xf9400000 | (r<<10) | (16<<5) | r);
    emit(0xf9404210); // ldr x16,[x16,#128]
    emit(FexSwapThunk::branch(reinterpret_cast<uintptr_t>(code+n),reinterpret_cast<uintptr_t>(code+256)));
    n=128; // return from thunk, save everything before using scratch regs
    emit(0xd10403ff); // sub sp,sp,#256
    for(unsigned r=0;r<32;r+=2)emit(0xa9000000 | (r<<15) | ((r+1)<<10) | (31<<5) | r);
    emit(0xd53b4203);emit(0xf9007fe3); // nzcv -> saved[31]
    emit(0xf940b3e2); // ldr x2,[sp,#352] output pointer
    for(unsigned r=0;r<32;r+=2){
        emit(0xa9400000 | (r<<15) | (1<<10) | (31<<5));
        emit(0xa9000000 | (r<<15) | (1<<10) | (2<<5));
    }
    emit(0x910403ff); // add sp,sp,#256
    for(unsigned r=19;r<31;r+=2)
        emit(0xa9400000 | ((r-19)<<15) | ((r+1)<<10) | (31<<5) | r);
    emit(0x910203ff);emit(0xd65f03c0);
    assert(n<256);
}

int main() {
 code=(uint32_t*)mmap(nullptr,16384,7,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(code!=MAP_FAILED);wrapper();
 for(unsigned ro : {0u,16u,256u,32760u}) for(unsigned po : {8u,24u,512u,32752u}) {
  auto stub=fexDispatchExit(ro,po);assert(stub[0]);memcpy(code+256,stub.data(),sizeof(stub));
  __builtin___clear_cache((char*)code,(char*)(code+512));
  uint64_t frame[4096]{},record[3]{0,0x7ffeedccbbaa9988,0};
  frame[po/8]=(uint64_t)(code+128);
  for(unsigned flags=0;flags<16;++flags){
   uint64_t input[32],output[32]{};for(unsigned r=0;r<31;++r)input[r]=0xfedcba9876543210ull+r;
   input[28]=(uint64_t)frame;input[30]=(uint64_t)record;input[31]=uint64_t(flags)<<28;
   ((void(*)(uint64_t*,uint64_t*))code)(input,output);
   assert(frame[ro/8]==record[1]);
   for(unsigned r=0;r<32;++r)assert(output[r]==(r==0?record[1]:r==1?(uint64_t)(code+128):input[r]));
  }
 }
 assert(!fexDispatchExit(1,8)[0]);assert(!fexDispatchExit(0,32768)[0]);
 puts("PASS native dispatcher leaf: GPRs, NZCV, record RIP, frame offsets and target");
}
