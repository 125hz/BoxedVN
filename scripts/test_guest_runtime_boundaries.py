"""Compile production dispatch/signal methods with native boundary fixtures."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent


def method(file, signature):
    source = (repo / file).read_text()
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


code = r'''
#include <cassert>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <memory>
#include <unordered_map>
#include <cstring>
#include <array>
#include <vector>
#include <chrono>
using U64=unsigned long long; using U32=unsigned;
#define BOXEDWINE_GUEST_X64
#define BOXEDWINE_MULTI_THREADED
#define BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(m) std::lock_guard guard(m)
#define BOXEDWINE_CRITICAL_SECTION_WITH_CONDITION(c) std::lock_guard guard((c)->mutex)
#define BOXEDWINE_CONDITION_SIGNAL_ALL(c) (c)->cv.notify_all()
struct Condition {std::mutex mutex;std::condition_variable cv;};
using BOXEDWINE_CONDITION=std::shared_ptr<Condition>;
struct CPU64;
struct Process { U32 id=10; };
struct KThread {
 U32 id=11; Process storage; Process* process=&storage;
 U64 pendingSignals=0; std::mutex pendingSignalsMutex,waitingCondSync;
 CPU64* cpu64=nullptr; bool terminating=false,interruptibleWait64=false;
 BOXEDWINE_CONDITION waitingCond;
 void queuePendingSignal(U32, bool wakeWaiter=false);
};
constexpr U32 K_SA_RESTART=0x10000000;
constexpr int X64_RAX=0,X64_RCX=1;
struct Register {U64 u64=0;void setU64(U64 v){u64=v;}};
struct CPU64 {
 struct Action {bool installed=false;U64 handler=0,flags=0;};
 Action sigActions[65]; KThread* thread=nullptr;U64 sigMask=0,rip=0,syscallRip=0;
 Register reg[16];
 bool hasDeliverableSignal(); bool deliverPendingSignals(U64 restartSyscall=~0ULL);
};
int scalarYieldCalls=0;
void kschedYield64(CPU64*) {++scalarYieldCalls;}
int pollingCalls=0;
bool kpollingQuery64(CPU64*,U64 number,U64 first,U64 address,U64& result) {
 ++pollingCalls;if(number!=98 || first!=1 || address!=0x4000)return false;
 result=static_cast<U64>(-5);return true;
}
U64 capturedRip=0,capturedResult=0;
bool deliverSignalSync(CPU64* cpu,U32 sig) {
 auto a=cpu->sigActions[sig];if(!a.installed || a.handler<=1)return false;
 capturedRip=cpu->rip;capturedResult=cpu->reg[0].u64;
 cpu->rip=a.handler;return true;
}
using VkInstance=void*;using VkPhysicalDevice=void*;
constexpr auto VK_NULL_HANDLE=nullptr;
std::mutex gPhysicalDeviceMutex;
std::unordered_map<VkPhysicalDevice,VkInstance> gPhysicalDeviceInstances;
VkInstance gInstance=nullptr;
enum {VKB_GetPhysicalDeviceSurfaceSupportKHR,VKB_CreateDevice,
 VKB_EnumerateDeviceExtensionProperties,VKB_EnumerateDeviceLayerProperties,
 VKB_DestroyInstance,VKB_EnumeratePhysicalDevices,VKB_EnumeratePhysicalDeviceGroups,
 VKB_DestroySurfaceKHR};
const char* kCommandName[]={"vkGetPhysicalDeviceSurfaceSupportKHR","vkCreateDevice",
 "vkEnumerateDeviceExtensionProperties","vkEnumerateDeviceLayerProperties",
 "vkDestroyInstance","vkEnumeratePhysicalDevices","vkEnumeratePhysicalDeviceGroups","vkDestroySurfaceKHR"};
namespace FEXCore::Core {
 struct CpuStateFrame {void* Thread=nullptr;struct {U64 rip=0,fs_cached=0,gs_cached=0;U64 gregs[16]{};
  std::array<unsigned char,1024> vectors{};} State;};
}
struct FexThread {FEXCore::Core::CpuStateFrame* CurrentFrame=nullptr;};
constexpr U64 K64_NATIVE_GUEST_IMAGE_BASE=0x7a00000000ULL;
constexpr U64 K64_NATIVE_GUEST_INTERP_BASE=0x7f00000000ULL,K64_NATIVE_GUEST_HIGH_END=0x7f80000000ULL;
constexpr int BVNFEXCPU64AdapterActionContinue=0;
struct BVNFEXCPU64Adapter {FexThread* fexThread;KThread* thread;CPU64* cpu;int lastAction=99;};
constexpr U64 BOXEDWINE_X64_HOSTCALL_VULKAN_BRIDGE=0x7fff0003;
int recordingCalls=0;
bool vulkanBridge64RecordsCommands(U64 op){return op==123;}
U64 vulkanBridge64(CPU64*,U64 op,U64 address,U64 count) {
 assert(op==123 && address==0x8000 && count==3);++recordingCalls;return uint64_t(-7);
}
struct XWindow {};
using XWindowPtr=std::shared_ptr<XWindow>;
struct Surface {XWindowPtr window;bool presentation=true,presentationVisible=true,firstPresentObserved=false;};
std::mutex surfacesMutex;std::vector<Surface> surfaces;
struct KVulkdanSDLImpl {bool isPendingPresentationWindow(const XWindowPtr&);};
'''
for signature in ["bool CPU64::hasDeliverableSignal(", "bool CPU64::deliverPendingSignals("]:
    code += method("source/kernel/syscall64.cpp", signature)
code += method("source/kernel/kthread.cpp", "void KThread::queuePendingSignal(")
code += method("source/util/synchronization.cpp", "bool setThreadWaitingCondition(")
for signature in ["void rememberPhysicalDevices(", "VkInstance physicalDeviceInstance(",
                  "void forgetPhysicalDevices(", "VkInstance resolutionInstance("]:
    code += method("source/vulkan/vulkanbridge64.cpp", signature)
code += method("ios/runtime/src/BVNFEXCPU64Adapter.mm", "static bool handleScalarSyscall(")
code += method("ios/runtime/src/BVNFEXCPU64Adapter.mm", "static bool handlePollingQuery(")
code += method("ios/runtime/src/BVNFEXCPU64Adapter.mm", "static bool handleVulkanRecording(")
code += method("platform/sdl/kvulkanSDL.cpp", "bool KVulkdanSDLImpl::isPendingPresentationWindow(")
syscalls = (repo / "source/kernel/syscall64.cpp").read_text()
start = syscalls.index("case X64_SYS_getpid:", syscalls.index("case X64_SYS_getpid:"))
end = syscalls.index("break;", syscalls.index("case X64_SYS_gettid:", start)) + len("break;")
code += "enum {X64_SYS_getpid=39,X64_SYS_gettid=186}; U64 identity(CPU64* cpu,int call){U64 ret=0;switch(call){" + syscalls[start:end] + "}return ret;}"
code += (repo / "include/kdspaudio_math.h").read_text()
code += r'''
constexpr int K_EINVAL=22;
struct Audio {U32 capacity=22050,used=8000,fragment=4096;
 U32 getBufferCapacity(){return capacity;} U32 getBufferSize(){return used;}
 U32 getFragmentSize(){return fragment;}};
struct Arg {U32 words[4]{};void writed(U32 offset,U32 value){words[offset/4]=value;}};
struct DevDsp {Audio storage;Audio* audio=&storage;Arg arg;
 void ensureAudioOpen64(){} U32 bytesPerFrame(){return 4;}
 U32 space();};
U32 DevDsp::space(){ const char* op=nullptr; U32 value=0,result=0;
 switch(0x500C) {
'''
dsp = (repo / "source/kernel/devs/devdsp.cpp").read_text().split("U32 DevDsp::ioctl64(", 1)[1]
code += dsp[dsp.index("case 0x500C:"):dsp.index("case 0x500D:")]
code += r'''
 }return result ? result : value;}
int main() {
 auto window=std::make_shared<XWindow>(); KVulkdanSDLImpl presentation;
 assert(!presentation.isPendingPresentationWindow(window));
 surfaces.push_back({window,true,true,false});
 assert(presentation.isPendingPresentationWindow(window)); // capability probe
 surfaces.push_back({window,true,true,true});
 assert(!presentation.isPendingPresentationWindow(window)); // rendered surface wins
 surfaces.clear();surfaces.push_back({window,false,true,false});
 assert(!presentation.isPendingPresentationWindow(window)); // offscreen helper
 assert(KDspAudioMath::getDefaultFragmentSize(44100*8,48000,1024)==16384);
 assert(KDspAudioMath::getDefaultFragmentSize(44100*4,48000,1024)==8192);
 assert(KDspAudioMath::getDefaultFragmentSize(192000*32,48000,4096)==16384);
 assert(KDspAudioMath::getDefaultFragmentSize(352800,0,1024)==4096);
 assert(KDspAudioMath::getWriteCapacity(352800,16384,65536)==49152);
 assert(KDspAudioMath::getWriteCapacity(1,0xffffffff,65536)==65536);
 DevDsp dsp;
 assert(dsp.space()==12480); // capacity is 5 fragments; free bytes include the remainder
 assert(dsp.arg.words[0]==3 && dsp.arg.words[1]==5 && dsp.arg.words[3]==12480);
 dsp.storage.used-=4;assert(dsp.space()==12484); // one played frame advances one frame
 dsp.storage.used=0;assert(dsp.space()==20480); // never exceed Wine's reported capacity
 dsp.storage.used=50000;assert(dsp.space()==0); // no unsigned overflow
 auto a=(VkInstance)1,b=(VkInstance)2;
 VkPhysicalDevice da=(VkPhysicalDevice)3,db=(VkPhysicalDevice)4;
 rememberPhysicalDevices(a,&da,1);rememberPhysicalDevices(b,&db,1);gInstance=b;
 U64 args[4]{(U64)da};
 assert(resolutionInstance(VKB_GetPhysicalDeviceSurfaceSupportKHR,args)==a);
 assert(resolutionInstance(VKB_CreateDevice,args)==a);
 args[0]=(U64)db;assert(resolutionInstance(VKB_GetPhysicalDeviceSurfaceSupportKHR,args)==b);
 forgetPhysicalDevices(b);assert(physicalDeviceInstance(db)==nullptr);
 assert(physicalDeviceInstance(da)==a); // closing a probe must not lose the other instance
 rememberPhysicalDevices(a,nullptr,55); // count-only enumeration
 forgetPhysicalDevices(a);assert(physicalDeviceInstance(da)==nullptr);
 rememberPhysicalDevices(b,&da,1);assert(physicalDeviceInstance(da)==b); // recycled handle

 KThread t;CPU64 c; c.thread=&t; assert(identity(&c,39)==10 && identity(&c,186)==11); t.id=22; assert(identity(&c,39)==10 && identity(&c,186)==22);t.cpu64=&c;c.thread=&t;c.sigActions[10]={true,0x8000,K_SA_RESTART};
 c.rip=0x1002;c.syscallRip=0x1000;c.reg[0].setU64(-4ULL);
 t.queuePendingSignal(10,true);assert(c.hasDeliverableSignal());
 c.sigMask=1ULL<<9;assert(!c.hasDeliverableSignal());assert(!c.deliverPendingSignals());
 c.sigMask=0;assert(c.deliverPendingSignals(0));
 assert(capturedRip==0x1000 && capturedResult==0 && t.pendingSignals==0);
 c.rip=0x1002;c.reg[0].setU64(-4ULL);t.queuePendingSignal(10,true);
 assert(c.deliverPendingSignals(202));assert(capturedRip==0x1000 && capturedResult==202);
 c.sigActions[10].flags=0;c.rip=0x1002;c.reg[0].setU64(-4ULL);t.queuePendingSignal(10,true);
 assert(c.deliverPendingSignals(0));assert(capturedRip==0x1002 && capturedResult==-4ULL);
 c.sigActions[11]={true,1,K_SA_RESTART};t.queuePendingSignal(11);
 c.rip=0x1002;c.reg[0].setU64(-4ULL);assert(!c.hasDeliverableSignal());
 assert(!c.deliverPendingSignals(0));assert(c.rip==0x1002 && c.reg[0].u64==-4ULL);
 t.pendingSignals=0;

 auto condition=std::make_shared<Condition>();t.interruptibleWait64=true;
 t.queuePendingSignal(10,true); // signal before the waiter registers: must not park
 assert(!setThreadWaitingCondition(&t,condition));assert(!t.waitingCond);
 t.pendingSignals=0;
 // A legacy caller already holding the condition must queue without relocking.
 {std::unique_lock held(condition->mutex);t.waitingCond=condition;
  t.queuePendingSignal(10);assert(c.hasDeliverableSignal());}
 t.pendingSignals=0;t.waitingCond=nullptr;
 std::unique_lock lock(condition->mutex);
 assert(setThreadWaitingCondition(&t,condition));
 std::thread sender([&]{t.queuePendingSignal(10,true);}); // signal after registration
 assert(condition->cv.wait_for(lock,std::chrono::seconds(1),[&]{return c.hasDeliverableSignal();}));
 lock.unlock();sender.join();t.waitingCond=nullptr;t.pendingSignals=0;
 c.sigMask=1ULL<<9;t.queuePendingSignal(10,true);
 assert(setThreadWaitingCondition(&t,condition)); // masked signals do not interrupt
 t.waitingCond=nullptr;t.pendingSignals=0;c.sigMask=0;

 FEXCore::Core::CpuStateFrame frame;FexThread ft{&frame};frame.Thread=&ft;
 BVNFEXCPU64Adapter adapter{&ft,&t,&c};frame.State.rip=K64_NATIVE_GUEST_INTERP_BASE+0x100;
 frame.State.vectors.fill(0xa5);auto before=frame.State.vectors;
 frame.State.rip=0x7a4011a909ULL; // actual libc lane, below the ELF interpreter
 assert(handleScalarSyscall(&adapter,&frame,24,0,0));assert(frame.State.vectors==before);
 assert(scalarYieldCalls==1);
 assert(frame.State.rip==0x7a4011a90bULL);
 assert(frame.State.gregs[0]==0 && frame.State.gregs[1]==frame.State.rip);
 t.queuePendingSignal(10,true);assert(!handleScalarSyscall(&adapter,&frame,24,0,0));t.pendingSignals=0;
 frame.State.rip=0x140001000;assert(!handleScalarSyscall(&adapter,&frame,24,0,0)); // PE thunk
 frame.State.rip=K64_NATIVE_GUEST_INTERP_BASE+0x100;assert(!handleScalarSyscall(&adapter,&frame,0,0,0));
 frame.State.rip=0x7a4026164bULL;frame.State.fs_cached=11;frame.State.gs_cached=22;
 assert(handleScalarSyscall(&adapter,&frame,158,0x1002,0x1008ff6c0ULL));
 assert(frame.State.fs_cached==0x1008ff6c0ULL && frame.State.gs_cached==22);
 assert(frame.State.rip==0x7a4026164dULL && frame.State.vectors==before);
 assert(handleScalarSyscall(&adapter,&frame,158,0x1001,0x7ff00000));
 assert(frame.State.gs_cached==0x7ff00000 && frame.State.vectors==before);
 assert(!handleScalarSyscall(&adapter,&frame,158,0x1003,0x1000)); // GET_FS writes memory
 t.queuePendingSignal(10,true);
 assert(!handleScalarSyscall(&adapter,&frame,158,0x1002,1));
 assert(frame.State.fs_cached==0x1008ff6c0ULL);t.pendingSignals=0;
 frame.State.rip=0x7fff80001000ULL;
 assert(!handleScalarSyscall(&adapter,&frame,158,0x1002,1)); // PE top-down arena
 t.terminating=true;assert(!handleScalarSyscall(&adapter,&frame,24,0,0));
 t.terminating=false;frame.State.rip=0x7a40100000ULL;
 uint64_t recordingArgs[7]={BOXEDWINE_X64_HOSTCALL_VULKAN_BRIDGE,123,0x8000,3};uint64_t result=0;
 assert(handleVulkanRecording(&adapter,&frame,recordingArgs,result));
 assert(result==uint64_t(-7) && frame.State.gregs[0]==result && recordingCalls==1);
 assert(frame.State.rip==0x7a40100002ULL && frame.State.gregs[1]==frame.State.rip);
 assert(frame.State.vectors==before);
 recordingArgs[1]=124;assert(!handleVulkanRecording(&adapter,&frame,recordingArgs,result)); // submit/wait
 recordingArgs[1]=123;t.queuePendingSignal(10,true);
 assert(!handleVulkanRecording(&adapter,&frame,recordingArgs,result));t.pendingSignals=0;
 frame.State.rip=0x140001000;assert(!handleVulkanRecording(&adapter,&frame,recordingArgs,result));
 assert(recordingCalls==1);
 frame.State.rip=0x7a40100000ULL;
 uint64_t queryArgs[3]={98,1,0x4000};U64 queryResult=0;
 assert(handlePollingQuery(&adapter,&frame,queryArgs,queryResult));
 assert(queryResult==static_cast<U64>(-5) && frame.State.gregs[0]==queryResult);
 assert(frame.State.rip==0x7a40100002ULL && frame.State.gregs[1]==frame.State.rip);
 assert(frame.State.vectors==before && pollingCalls==1);
 t.queuePendingSignal(10,true);
 assert(!handlePollingQuery(&adapter,&frame,queryArgs,queryResult));t.pendingSignals=0;
 frame.State.rip=0x140001000;assert(!handlePollingQuery(&adapter,&frame,queryArgs,queryResult));
 frame.State.rip=0x7a40100000ULL;t.terminating=true;
 assert(!handlePollingQuery(&adapter,&frame,queryArgs,queryResult));t.terminating=false;
 frame.Thread=nullptr;assert(!handlePollingQuery(&adapter,&frame,queryArgs,queryResult));frame.Thread=&ft;
 assert(pollingCalls==1); // exclusions do not touch guest memory
 queryArgs[2]=0;assert(!handlePollingQuery(&adapter,&frame,queryArgs,queryResult));
 assert(frame.State.rip==0x7a40100000ULL && frame.State.vectors==before);
}
'''

with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "boundaries.cpp"
    binary = Path(tmp) / "boundaries"
    source.write_text(code)
    subprocess.run(["c++", "-std=c++20", "-pthread", str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
print("Guest runtime boundary fixtures passed")
