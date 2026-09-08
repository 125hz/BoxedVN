#include "fex_host_discard.h"
#include <cassert>
#include <cstdio>
#include <unistd.h>
int main() {
    const size_t page=sysconf(_SC_PAGESIZE);
    auto* data=(unsigned char*)mmap(nullptr,page*3,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(data!=MAP_FAILED);
    for(unsigned round=0;round<40;++round) {
        memset(data,0x5a,page*3);
        assert(!boxedvn::discardFexHostPages(data+page,page,MADV_DONTNEED));
        for(size_t i=0;i<page*3;++i) assert(data[i]==(i>=page&&i<page*2?0:0x5a));
    }
    // The shared cache's whole allocation must also return zero on repeated use.
    assert(!boxedvn::discardFexHostPages(data,page*3,MADV_DONTNEED));
    for(size_t i=0;i<page*3;++i) assert(!data[i]);
    assert(boxedvn::discardFexHostPages(data+1,page,MADV_DONTNEED)==-1);
    munmap(data,page*3);
    FILE* file=tmpfile();assert(file);unsigned char bytes[256];memset(bytes,0xa5,sizeof(bytes));
    for(size_t i=0;i<page;i+=sizeof(bytes)) assert(fwrite(bytes,1,sizeof(bytes),file)==sizeof(bytes));
    fflush(file);
    data=(unsigned char*)mmap(nullptr,page,PROT_READ,MAP_PRIVATE,fileno(file),0);assert(data!=MAP_FAILED);
    assert(!boxedvn::discardFexHostPages(data,page,MADV_DONTNEED));
    for(size_t i=0;i<page;++i)assert(data[i]==0xa5);
    munmap(data,page);fclose(file);
    puts("PASS host discard: repeated zero-fill, subranges, alignment and file-backed exclusion");
}
