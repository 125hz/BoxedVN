"""Compile the actual signal-time publisher; test dual aliases and races."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent
source = (repo / "ios/runtime/src/BVNFEXBackend.mm").read_text()
records = source[source.index("struct SwapSlot {"):source.index("gSwapBanks;")+len("gSwapBanks;")]
functions = source[source.index('extern "C" bool BVNFEXBackendSwapFault('):source.index('extern "C" bool BVNFEXBackendOwnsHostCodeAddress(')]
code = r'''
#include "fex_unaligned_swap.h"
#include "boxedvn/fex_code_segments.h"
#include <atomic>
#include <cstring>
#include <thread>
#include <vector>
#include <cassert>
#include <sys/mman.h>
#include <unistd.h>
boxedvn::FexCodeSegments gCodeSegments;
void sys_dcache_flush(void*,size_t){}
void sys_icache_invalidate(void*,size_t){}
'''+records+functions+r'''
int main(){
 int fd=memfd_create("swap",0);assert(fd>=0);assert(!ftruncate(fd,1<<20));
 auto rx=(uint64_t)mmap(nullptr,1<<20,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
 auto rw=(uint64_t)mmap(nullptr,1<<20,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
 assert(rx!=rw && gCodeSegments.append({rx,rw,1<<20}));
 gSwapBanks[0].rw=rw;gSwapBanks[0].rx.store(rx);
 const uint64_t site=rx+0x20000;*reinterpret_cast<uint32_t*>(site)=0xf8ea806a;
 // All contenders saw the old instruction before any published its branch.
 std::atomic<bool> go=false;std::vector<std::thread> threads;
 for(unsigned i=0;i<64;++i)threads.emplace_back([&]{
  while(!go.load())std::this_thread::yield();
  assert(BVNFEXBackendPatchUnalignedSwap(site,0xf8ea806a));
 });
 go=true;for(auto& t:threads)t.join();
 auto op=*reinterpret_cast<uint32_t*>(site);
 assert((op&0xfc000000)==0x14000000);
 assert(BVNFEXBackendPatchUnalignedSwap(site,op));
 assert(!BVNFEXBackendPatchUnalignedSwap(site+4,op)); // no unrelated branch accepted
 const int64_t delta=static_cast<int32_t>(op<<6)>>4;
 const uint64_t target=site+delta;
 auto& slot=gSwapBanks[0].records[(target-rx)/boxedvn::FexSwapThunk::slotBytes];
 uint64_t original=0,slow=0;bool stacked=false;
 assert(BVNFEXBackendSwapFault(target+slot.cas*4,&original,&slow,&stacked));
 assert(original==site && stacked);
 assert(*reinterpret_cast<uint32_t*>(slow)==0xf8ea806a);
 assert(BVNFEXBackendSwapFault(slow,&original,&slow,&stacked) && !stacked);
 assert(!BVNFEXBackendSwapFault(target,&original,&slow,&stacked));
 assert(!BVNFEXBackendPatchUnalignedSwap(site+4,0xd503201f));
 gSwapBanks[0].next=SwapBank::slots;
 assert(!BVNFEXBackendPatchUnalignedSwap(site+4,0xf8ea806a));
 assert(BVNFEXBackendPatchUnalignedSwap(site,op)); // full bank still recognizes a patch
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp)/"publish.cpp"
    binary = Path(tmp)/"publish"
    src.write_text(code)
    subprocess.run(["c++", "-std=c++17", "-pthread", "-I"+str(repo/"include"),
                    "-I"+str(repo/"ios/support/include"), str(src), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=20)
print("Atomic thunk dual-alias publication, fault lookup and race tests passed")
