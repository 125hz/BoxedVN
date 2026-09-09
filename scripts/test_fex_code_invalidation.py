"""Exercise the production FEX frontend invalidation callback with cache models."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent
text = (repo / "ios/runtime/src/BVNFEXBackend.mm").read_text()
signature = "void invalidateLiveExecutableRange(FEXCore::Core::InternalThreadState* thread,"
start = text.index(signature, text.index("bool mapGuestProbe()"))
end = text.index("{", start) + 1
depth = 1
while depth:
    depth += (text[end] == "{") - (text[end] == "}")
    end += 1
callback = text[start:end]
fixture = r'''
#include <cassert>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
struct Context;
namespace FEXCore::Core {
struct InternalThreadState { Context* CTX; int cachedCode=1; };
}
using Thread=FEXCore::Core::InternalThreadState;
struct CheckedMutex {
 bool held=false;
 void lock(){assert(!held);held=true;}
 void unlock(){assert(held);held=false;}
};
struct Context {
 CheckedMutex mutex;
 int sharedCode=1, invalidations=0;
 std::vector<Thread*> cleared;
 CheckedMutex& GetCodeInvalidationMutex(){return mutex;}
 void InvalidateCodeBuffersCodeRange(uint64_t start,uint64_t length){
  assert(mutex.held && start==0x10000 && length==8);
  sharedCode=0;++invalidations;
 }
 void InvalidateThreadCachedCodeRange(Thread* thread,uint64_t start,uint64_t length){
  assert(mutex.held && sharedCode==0 && start==0x10000 && length==8);
  assert(thread->CTX==this);thread->cachedCode=0;cleared.push_back(thread);
 }
 int execute(Thread& thread,int memoryCode){
  // A private-cache miss must not resurrect a stale shared translation.
  if(!thread.cachedCode){if(!sharedCode)sharedCode=memoryCode;thread.cachedCode=sharedCode;}
  return thread.cachedCode;
 }
};
struct Bundle {std::unique_ptr<Context> context=std::make_unique<Context>();};
std::unique_ptr<Bundle> gProbeContext;
std::mutex gLiveMutex;
std::unordered_map<Thread*,Context*> gLiveThreadContexts;
'''
fixture += callback + r'''
int main(){
 Context live,other,retired;
 Thread writer{&live},reader{&live},unrelated{&other},oldEpoch{&retired};
 gLiveThreadContexts={{&writer,&live},{&reader,&live},{&unrelated,&other},{&oldEpoch,&retired}};
 assert(live.execute(reader,1)==1);
 invalidateLiveExecutableRange(&writer,0x10000,8);
 assert(live.invalidations==1 && live.cleared.size()==2);
 assert(live.execute(writer,2)==2 && live.execute(reader,2)==2);
 assert(other.invalidations==0 && unrelated.cachedCode==1);
 assert(retired.invalidations==0 && oldEpoch.cachedCode==1);
 // In-flight callbacks from retired exec epochs still use their own context.
 invalidateLiveExecutableRange(&oldEpoch,0x10000,8);
 assert(retired.execute(oldEpoch,3)==3 && retired.cleared.size()==1);
 assert(live.invalidations==1);
 gProbeContext=std::make_unique<Bundle>();
 Thread probe{gProbeContext->context.get()},unknown{&other};
 invalidateLiveExecutableRange(&unknown,0x10000,8);
 assert(gProbeContext->context->invalidations==0);
 invalidateLiveExecutableRange(&probe,0x10000,8);
 assert(gProbeContext->context->execute(probe,4)==4);
 invalidateLiveExecutableRange(nullptr,0x10000,8);
 invalidateLiveExecutableRange(&writer,0x10000,0);
 invalidateLiveExecutableRange(&writer,UINT64_MAX-3,8);
 assert(live.invalidations==1);
 assert(!live.mutex.held && !retired.mutex.held && !gProbeContext->context->mutex.held);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    source, binary = Path(tmp) / "invalidation.cpp", Path(tmp) / "invalidation"
    source.write_text(fixture)
    subprocess.run(["c++", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
print("PASS FEX invalidation: shared cache, sibling threads, epochs, lock ownership and probe isolation")
