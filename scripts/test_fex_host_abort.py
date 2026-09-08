"""Execute the production guest fault classifier against ARM read/write aborts."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent
source = (repo / "ios/runtime/src/BVNFEXCPU64Adapter.mm").read_text()
start = source.index("struct GuestMemoryFaultClass {")
end = source.index("\n// Read one guest qword", start)
classifier = source[start:end]
code = r'''
#include <cassert>
#include <cstdint>
#include <csignal>
#include "fex_host_abort.h"
constexpr int K_SIGSEGV=11,K_SIGBUS=7,K_SEGV_ACCERR=2,K_SEGV_MAPERR=1,K_BUS_ADRALN=1;
constexpr uint64_t K64_PAGE_MASK=4095,K64_PAGE_SIZE=4096,K64_PAGE_SHIFT=12;
constexpr unsigned K64_PAGE_READ=1,K64_PAGE_WRITE=2;
uint64_t k64GuestToHostAddress(uint64_t a){return a|0x7800000000ull;}
uint64_t k64HostToGuestAddress(uint64_t a){return a&~0x7800000000ull;}
unsigned hostSignalGuestNumber(int s){return s==SIGBUS?K_SIGBUS:K_SIGSEGV;}
unsigned hostSignalTrapNumber(int s){return s==SIGBUS?17:14;}
struct KMemory64 {
 bool mapped=false;unsigned flags=0;
 bool nativeIdentityMode(){return true;}
 bool nativeGuestRangeAllowed(uint64_t a,uint64_t){return a<0x100000000;}
 bool isPageMapped(uint64_t){return mapped;}
 unsigned getPageFlags(uint64_t){return flags;}
};
struct Process {KMemory64* memory64;};
struct BVNFEXCPU64Adapter {Process* process;};
'''+classifier+r'''
int main(){
 KMemory64 m; Process p{&m};BVNFEXCPU64Adapter a{&p};
 for(bool mapped:{false,true})for(unsigned flags=0;flags<4;++flags)for(bool write:{false,true}){
  m.mapped=mapped;m.flags=flags;
  auto result=classifyGuestMemoryFault(&a,SIGBUS,1,0x7801980380,write);
  assert(result.fromPageMap && result.address==0x1980380);
  const bool entitled=mapped&&(flags&(write?2:1));
  assert(result.signal==(entitled?K_SIGBUS:K_SIGSEGV));
  assert(result.trapNumber==(entitled?17:14));
  if(!entitled)assert(result.code==(mapped?K_SEGV_ACCERR:K_SEGV_MAPERR));
 }
 assert(boxedvn::armWriteAbort(0x9200004f));
 assert(!boxedvn::armAlignmentAbort(0x9200004f));
 assert(boxedvn::armAlignmentAbort(0x92000061));
 assert(!boxedvn::armWriteAbort(0x92000021));
 assert(!boxedvn::armAlignmentAbort(0));
 assert(boxedvn::guestPageFaultError(true,true)==7);
 assert(boxedvn::guestPageFaultError(false,false)==4);
}
'''
code = '#include <initializer_list>\n'+code
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp)/"abort.cpp"
    binary = Path(tmp)/"abort"
    src.write_text(code)
    subprocess.run(["c++", "-std=c++17", "-I"+str(repo/"include"), str(src), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("Guest protection/alignment classification passed")
