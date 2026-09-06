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
    boxedvn::Wow64WaitStack out;
    check(boxedvn::captureWow64WaitStack(teb,read,exec,out),"valid WoW64 context");
    check(out.count==2 && out.codeCandidates[1]==0x400345,"bounded executable candidates, deduplicated");
    put(reserved+4+0xc4,0x110000,4);
    check(!boxedvn::captureWow64WaitStack(teb,read,exec,out),"reject stack outside TEB bounds");
    put(reserved+4+0xc4,0x12ff00,4); put(reserved+2,0x8664,2);
    check(!boxedvn::captureWow64WaitStack(teb,read,exec,out),"reject non-i386 context");
    check(!boxedvn::captureWow64WaitStack(UINT64_MAX-8,read,exec,out),"reject overflow");
    std::puts("WoW64 wait diagnostic tests passed");
}
