#pragma once
#include <array>
#include <cstdint>

namespace boxedvn {
// Thread-local to an adapter. The memory-map generation retires all entries
// on mmap/munmap/mprotect; ordinary data writes do not change executable ranges.
class FexExecutableRangeCache {
    struct Entry { uint64_t first=0,last=0;uint32_t flags=0;bool valid=false; };
    std::array<Entry,8> entries{};
    uint64_t owner=0;
    uint32_t generation=0;
    unsigned next=0;
public:
    template<class PageFlags>
    bool query(uint64_t space, uint32_t epoch, uint64_t page, uint64_t maximumPage,
               uint32_t executable, uint32_t write, PageFlags flags,
               uint64_t& first, uint64_t& last, bool& writable) {
        if (owner!=space || generation!=epoch) {
            entries={};next=0;owner=space;generation=epoch;
        }
        for(const auto& e:entries)if(e.valid && page>=e.first && page<=e.last){
            first=e.first;last=e.last;writable=(e.flags&write)!=0;return true;
        }
        const uint32_t rights=flags(page)&(executable|write);
        if ((rights&executable)!=executable) return false;
        first=last=page;
        while(first && (flags(first-1)&(executable|write))==rights)--first;
        while(last<maximumPage && (flags(last+1)&(executable|write))==rights)++last;
        entries[next++%entries.size()]={first,last,rights,true};
        writable=(rights&write)!=0;
        return true;
    }
};
}
