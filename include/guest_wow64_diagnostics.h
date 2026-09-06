#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

namespace boxedvn {
struct Wow64WaitStack {
    uint32_t eip=0, esp=0, ebp=0, teb32=0;
    std::array<uint32_t,24> codeCandidates{};
    std::array<uint32_t,32> frameReturns{};
    unsigned count=0, frameCount=0;
};
// Wine's x86-64 wow64cpu keeps WOW64_CPURESERVED in TEB64 TLS slot 1.
// Validate the machine, context and TEB32 stack bounds before scanning. These
// are diagnostic candidates, not an unwound backtrace or a guest-state repair.
template<class Read, class Executable>
bool captureWow64WaitStack(uint64_t teb, Read read, Executable executable,
                           Wow64WaitStack& out) {
    out = {};
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
    out.teb32=static_cast<uint32_t>(teb32);
    // EBP chains omit frames compiled without frame pointers, but avoid
    // mistaking stale temporary stack contents for active callers.
    uint64_t frame=out.ebp;
    while (frame>=out.esp && frame>=low && frame+8<=high && !(frame&3) &&
           out.frameCount<out.frameReturns.size()) {
        uint32_t next=0, ret=0;
        if (!read(frame,&next,4) || !read(frame+4,&ret,4) || !executable(ret)) break;
        out.frameReturns[out.frameCount++]=ret;
        if (next<=frame) break;
        frame=next;
    }
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

// Wine's i386 PEB loader list supplies actual relocated module bases. This is
// a bounded best-effort snapshot: never take a guest loader lock while logging
// a lock timeout, and never interpret its contents as host pointers.
template<class Read, class Visit>
void visitWow64Modules(uint32_t teb32, Read read, Visit visit) {
    uint32_t peb=0, ldr=0, link=0;
    if (!teb32 || teb32>UINT32_MAX-0x34 || !read(uint64_t(teb32)+0x30,&peb,4) ||
        !peb || peb>UINT32_MAX-0x10 || !read(uint64_t(peb)+0x0c,&ldr,4) ||
        !ldr || ldr>UINT32_MAX-0x14 || !read(uint64_t(ldr)+0x0c,&link,4)) return;
    const uint32_t head=ldr+0x0c;
    std::array<uint32_t,256> seen{};
    unsigned count=0;
    while (link && link!=head && count<seen.size()) {
        if (link>UINT32_MAX-0x34) return;
        for (unsigned i=0;i<count;++i) if (seen[i]==link) return;
        seen[count++]=link;
        uint32_t next=0, base=0, size=0, nameAddress=0;
        uint16_t nameBytes=0;
        if (!read(link,&next,4) || !read(uint64_t(link)+0x18,&base,4) ||
            !read(uint64_t(link)+0x20,&size,4) || !read(uint64_t(link)+0x2c,&nameBytes,2) ||
            !read(uint64_t(link)+0x30,&nameAddress,4)) return;
        if (base && size && uint64_t(base)+size<=uint64_t(UINT32_MAX)+1) {
            char name[96]={};
            if (!(nameBytes&1) && nameAddress && uint64_t(nameAddress)+nameBytes<=uint64_t(UINT32_MAX)+1) {
                const unsigned chars=std::min<unsigned>(nameBytes/2,sizeof(name)-1);
                for (unsigned i=0;i<chars;++i) {
                    uint16_t c=0;
                    if (!read(uint64_t(nameAddress)+2*i,&c,2)) break;
                    name[i]=(c>=32 && c<127) ? static_cast<char>(c) : '?';
                }
            }
            visit(base,size,name);
        }
        link=next;
    }
}
}
