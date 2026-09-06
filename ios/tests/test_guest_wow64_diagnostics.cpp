#include "guest_wow64_diagnostics.h"
#include <map>
#include <cstring>
#include <cstdio>
#include <cstdlib>
static void check(bool v,const char* why) { if(!v) { std::puts(why); std::exit(1); } }
int main() {
    std::map<uint64_t,uint8_t> bytes;
    auto put=[&](uint64_t a,uint64_t v,unsigned n) { for(unsigned i=0;i<n;++i) bytes[a+i]=(v>>(8*i))&255; };
    auto read=[&](uint64_t a,void* out,size_t n) {
        for(size_t i=0;i<n;++i) { auto p=bytes.find(a+i); if(p==bytes.end()) return false; static_cast<uint8_t*>(out)[i]=p->second; }
        return true;
    };
    auto exec=[](uint32_t a) { return a>=0x400000 && a<0x410000; };
    constexpr uint64_t teb=0x7ffe0000, reserved=0x100000000;
    put(teb+0x30,teb,8); put(teb+0x1488,reserved,8); put(teb+0x180c,0x2000,4);
    put(teb+0x2018,teb+0x2000,4); put(teb+0x2004,0x140000,4); put(teb+0x2008,0x120000,4);
    put(reserved+2,0x14c,2); put(reserved+4,0x10007,4);
    put(reserved+4+0xb4,0x12ff80,4); put(reserved+4+0xb8,0x400123,4); put(reserved+4+0xc4,0x12ff00,4);
    put(0x12ff00,0x400234,4); put(0x12ff04,0x400234,4); put(0x12ff08,0x123456,4); put(0x12ff0c,0x400345,4);
    put(0x12ff80,0x12ffa0,4); put(0x12ff84,0x400678,4);
    put(0x12ffa0,0x12ff80,4); put(0x12ffa4,0x400789,4);
    boxedvn::Wow64WaitStack out;
    check(boxedvn::captureWow64WaitStack(teb,read,exec,out),"valid WoW64 context");
    check(out.count==2 && out.codeCandidates[1]==0x400345,"bounded executable candidates, deduplicated");
    check(out.frameCount==2 && out.frameReturns[0]==0x400678 && out.frameReturns[1]==0x400789,
          "frame chain ignores stale candidates and terminates a cycle");
    put(0x12ff80,0x140000,4);
    check(boxedvn::captureWow64WaitStack(teb,read,exec,out) && out.frameCount==1,
          "frame chain stops at stack ceiling");
    constexpr uint32_t peb=0x200000, ldr=0x201000, entry=0x202000, name=0x203000;
    put(teb+0x2030,peb,4); put(peb+0xc,ldr,4); put(ldr+0xc,entry,4);
    put(entry,ldr+0xc,4); put(entry+0x18,0x400000,4); put(entry+0x20,0x10000,4);
    put(entry+0x2c,16,2); put(entry+0x30,name,4);
    const char* dll="test.dll";
    for (unsigned i=0;i<8;++i) put(name+2*i,dll[i],2);
    unsigned modules=0;
    auto visit=[&](uint32_t b,uint32_t s,const char* n) {
        check(b==0x400000 && s==0x10000 && !std::strcmp(n,"test.dll"),"actual module range and UTF-16 name");
        ++modules;
    };
    boxedvn::visitWow64Modules(out.teb32,read,visit);
    check(modules==1,"loader list sentinel");
    put(entry,entry,4); modules=0;
    boxedvn::visitWow64Modules(out.teb32,read,visit);
    check(modules==1,"corrupt loader cycle bounded");
    put(entry+0x18,0xfffff000,4); modules=0;
    boxedvn::visitWow64Modules(out.teb32,read,visit);
    check(modules==0,"overflowing module range rejected");
    put(reserved+4+0xc4,0x110000,4);
    check(!boxedvn::captureWow64WaitStack(teb,read,exec,out),"reject stack outside TEB bounds");
    check(out.frameCount==0 && out.count==0,"invalid capture cannot retain stale output");
    put(reserved+4+0xc4,0x12ff00,4); put(reserved+2,0x8664,2);
    check(!boxedvn::captureWow64WaitStack(teb,read,exec,out),"reject non-i386 context");
    check(!boxedvn::captureWow64WaitStack(UINT64_MAX-8,read,exec,out),"reject overflow");
    std::puts("WoW64 wait diagnostic tests passed");
}
