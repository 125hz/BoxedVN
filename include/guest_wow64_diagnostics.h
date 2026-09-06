#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

namespace boxedvn {
struct Wow64WaitStack {
    uint32_t eip=0, esp=0, ebp=0;
    std::array<uint32_t,24> codeCandidates{};
    unsigned count=0;
};
// Wine's x86-64 wow64cpu keeps WOW64_CPURESERVED in TEB64 TLS slot 1.
// Validate the machine, context and TEB32 stack bounds before scanning. These
// are diagnostic candidates, not an unwound backtrace or a guest-state repair.
template<class Read, class Executable>
bool captureWow64WaitStack(uint64_t teb, Read read, Executable executable,
                           Wow64WaitStack& out) {
    uint64_t self=0, reserved=0;
    int32_t wowOffset=0;
    if (!teb || teb>UINT64_MAX-0x1810 || !read(teb+0x30,&self,8) || self!=teb ||
        !read(teb+0x1488,&reserved,8) || !reserved || reserved>UINT64_MAX-0xcc ||
        !read(teb+0x180c,&wowOffset,4) || wowOffset<=0) return false;
    if (teb>UINT64_MAX-static_cast<uint32_t>(wowOffset)) return false;
    const uint64_t teb32=teb+static_cast<uint32_t>(wowOffset);
    if (teb32>UINT32_MAX-0x20) return false;
    uint16_t machine=0;
    uint32_t flags=0, high=0, low=0, self32=0;
    const uint64_t context=reserved+4;
    if (!read(reserved+2,&machine,2) || machine!=0x14c ||
        !read(context,&flags,4) || !(flags&0x10000) ||
        !read(teb32+0x18,&self32,4) || self32!=teb32 ||
        !read(teb32+4,&high,4) || !read(teb32+8,&low,4) || low>=high ||
        !read(context+0xb4,&out.ebp,4) || !read(context+0xb8,&out.eip,4) ||
        !read(context+0xc4,&out.esp,4) || out.esp<low || out.esp>=high) return false;
    out.count=0;
    const uint64_t end=std::min<uint64_t>(high,static_cast<uint64_t>(out.esp)+4096);
    for (uint64_t p=out.esp;p+4<=end && out.count<out.codeCandidates.size();p+=4) {
        uint32_t candidate=0;
        if (!read(p,&candidate,4)) break;
        if (candidate<0x10000 || !executable(candidate)) continue;
        bool duplicate=false;
        for (unsigned i=0;i<out.count;++i) if(out.codeCandidates[i]==candidate) duplicate=true;
        if(!duplicate) out.codeCandidates[out.count++]=candidate;
    }
    return true;
}
}
