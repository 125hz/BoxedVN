#include "fex_unaligned_swap.h"
#include "fex_host_abort.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#include <algorithm>
#include <sys/mman.h>
#include <signal.h>
#include <ucontext.h>

using namespace boxedvn;
static uint32_t* code;
static uint64_t input[32], output[32];
static FexSwapThunk thunk;
static uint64_t slowFaults = 0, protectionFaults = 0;
static void fault(int sig, siginfo_t* info, void* raw) {
    auto& m = static_cast<ucontext_t*>(raw)->uc_mcontext;
    auto* pc = reinterpret_cast<uint32_t*>(m.pc);
    auto* entry = code + 256;
    if (sig == SIGBUS && pc == entry + thunk.fallback) {
        // Independent slow fixture: execute the unaligned exchange once and
        // resume after the original opcode, as the production FEX handler does.
        unsigned rn=(*pc>>5)&31, rs=(*pc>>16)&31, rt=*pc&31;
        uint64_t old, value=rs==31?0:m.regs[rs];
        memcpy(&old, reinterpret_cast<void*>(m.regs[rn]),8);
        memcpy(reinterpret_cast<void*>(m.regs[rn]),&value,8);
        if(rt!=31)m.regs[rt]=old;
        m.pc+=4;++slowFaults;return;
    }
    if (sig == SIGSEGV && (pc == entry + thunk.load || pc == entry + thunk.cas)) {
        uint64_t saved[16];memcpy(saved,reinterpret_cast<void*>(m.sp),sizeof(saved));
        for(unsigned i=0;i<12;++i)m.regs[i]=saved[i];
        m.pstate=(m.pstate&0x0fffffff)|(saved[12]&0xf0000000);
        m.sp+=sizeof(saved);
        for(unsigned i=0;i<31;++i)assert(m.regs[i]==input[i]);
        assert((m.pstate&0xf0000000)==input[31]);
        ++protectionFaults;
        // Model delivery to Wine: leave the faulting operation unexecuted.
        m.pc=reinterpret_cast<uint64_t>(code+128);return;
    }
    fprintf(stderr,"unexpected signal %d pc=%p addr=%p\n",sig,pc,info->si_addr);
    _Exit(2);
}

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
static void compile(unsigned rn,unsigned rs,unsigned rt) {
    thunk={};assert(thunk.build(0xf8e08000 | (rs<<16) | (rn<<5) | rt,
        reinterpret_cast<uintptr_t>(code+256),reinterpret_cast<uintptr_t>(code+128)));
    memcpy(code+256,thunk.words.data(),thunk.count*4);
    __builtin___clear_cache(reinterpret_cast<char*>(code),reinterpret_cast<char*>(code+512));
}
int main() {
    static_assert(armWriteAbort(0x9200004f));
    static_assert(!armAlignmentAbort(0x9200004f));
    static_assert(armAlignmentAbort(0x92000061));
    static_assert(guestPageFaultError(true,true)==7);
    assert(FexSwapThunk::accepts(0xf8ea806a));
    assert(!FexSwapThunk::accepts(0xb8ea806a));
    assert(!FexSwapThunk::branch(0x10000000,0x18000000));
    code=static_cast<uint32_t*>(mmap(nullptr,16384,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    assert(code!=MAP_FAILED);wrapper();
    struct sigaction sa{};sa.sa_sigaction=fault;sa.sa_flags=SA_SIGINFO;
    sigaction(SIGBUS,&sa,nullptr);sigaction(SIGSEGV,&sa,nullptr);
    alignas(16) unsigned char bytes[48],expected[48];
    unsigned cases=0;
    for(unsigned rn=0;rn<31;++rn)for(unsigned rs=0;rs<32;++rs)for(unsigned rt=0;rt<32;++rt){
        compile(rn,rs,rt);
        for(unsigned offset=0;offset<16;++offset){
            for(unsigned i=0;i<32;++i)input[i]=0x1122334455667700ull+i;
            input[31]=0xa0000000; input[rn]=reinterpret_cast<uintptr_t>(bytes+offset);
            memset(bytes,0x5a,sizeof(bytes));memcpy(expected,bytes,sizeof(bytes));
            const uint64_t value=rs==31?0:input[rs];
            memcpy(expected+offset,&value,8);
            reinterpret_cast<void(*)(uint64_t*,uint64_t*)>(code)(input,output);
            for(unsigned i=0;i<32;++i)assert(output[i]==(i==rt && rt!=31?0x5a5a5a5a5a5a5a5aull:input[i]));
            assert(!memcmp(bytes,expected,sizeof(bytes)));++cases;
        }
    }
    // A page revoked after patching must reconstruct the complete original
    // context at the LDP/CASP, including address/source/destination overlap.
    void* ro=mmap(nullptr,4096,PROT_READ,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    for(unsigned r: {0u,4u,10u,18u,29u,30u}){
        compile(r,r,r);input[r]=reinterpret_cast<uintptr_t>(ro)+3;
        reinterpret_cast<void(*)(uint64_t*,uint64_t*)>(code)(input,output);
        assert(!memcmp(input,output,sizeof(input)));
    }
    // Simultaneous exchanges must return every value exactly once, while an
    // independent adjacent-byte atomic must not be overwritten by the CAS.
    auto* parallelCode=code+1024;
    auto* done=code+1280;*done=0xd65f03c0; // ret
    FexSwapThunk parallel;
    assert(parallel.build(0xf8e18000, reinterpret_cast<uintptr_t>(parallelCode),
                          reinterpret_cast<uintptr_t>(done))); // x1 -> [x0], old -> x0
    memcpy(parallelCode,parallel.words.data(),parallel.count*4);
    __builtin___clear_cache(reinterpret_cast<char*>(parallelCode),reinterpret_cast<char*>(done+1));
    auto exchange=reinterpret_cast<uint64_t(*)(void*,uint64_t)>(parallelCode);
    alignas(16) unsigned char shared[16]{};
    constexpr unsigned workers=4, iterations=10000;
    std::vector<uint64_t> results(workers*iterations+1);
    std::vector<std::thread> threads;
    for(unsigned t=0;t<workers;++t)threads.emplace_back([&,t]{
        for(unsigned i=0;i<iterations;++i){
            const unsigned index=t*iterations+i;
            results[index]=exchange(shared+3,index+1);
        }
    });
    threads.emplace_back([&]{for(unsigned i=0;i<iterations;++i)__atomic_fetch_xor(shared+15,1,__ATOMIC_SEQ_CST);});
    for(auto& t:threads)t.join();
    memcpy(&results.back(),shared+3,8);
    std::sort(results.begin(),results.end());
    for(unsigned i=0;i<results.size();++i)assert(results[i]==i);
    assert(shared[15]==0 && shared[0]==0 && shared[11]==0);
    printf("unaligned swap: %u register/alignment cases; %llu split fallbacks; %llu protection recoveries\n",
           cases,(unsigned long long)slowFaults,(unsigned long long)protectionFaults);
    puts("unaligned swap: 40000 contended exchanges and adjacent-byte atomic passed");
}

