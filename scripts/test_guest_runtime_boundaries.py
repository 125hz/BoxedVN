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
struct KThread {
 U64 pendingSignals=0; std::mutex pendingSignalsMutex,waitingCondSync;
 CPU64* cpu64=nullptr; bool terminating=false,interruptibleWait64=false;
 BOXEDWINE_CONDITION waitingCond;
 void queuePendingSignal(U32);
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
 struct CpuStateFrame {void* Thread=nullptr;struct {U64 rip=0;U64 gregs[16]{};
  std::array<unsigned char,1024> vectors{};} State;};
}
struct FexThread {FEXCore::Core::CpuStateFrame* CurrentFrame=nullptr;};
constexpr U64 K64_NATIVE_GUEST_INTERP_BASE=0x7a00000000ULL,K64_NATIVE_GUEST_HIGH_END=0x7a80000000ULL;
constexpr int BVNFEXCPU64AdapterActionContinue=0;
struct BVNFEXCPU64Adapter {FexThread* fexThread;KThread* thread;CPU64* cpu;int lastAction=99;};
'''
for signature in ["bool CPU64::hasDeliverableSignal(", "bool CPU64::deliverPendingSignals("]:
    code += method("source/kernel/syscall64.cpp", signature)
code += method("source/kernel/kthread.cpp", "void KThread::queuePendingSignal(")
code += method("source/util/synchronization.cpp", "bool setThreadWaitingCondition(")
for signature in ["void rememberPhysicalDevices(", "VkInstance physicalDeviceInstance(",
                  "void forgetPhysicalDevices(", "VkInstance resolutionInstance("]:
    code += method("source/vulkan/vulkanbridge64.cpp", signature)
code += method("ios/runtime/src/BVNFEXCPU64Adapter.mm", "static bool handleScalarYield(")
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

 KThread t;CPU64 c;t.cpu64=&c;c.thread=&t;c.sigActions[10]={true,0x8000,K_SA_RESTART};
 c.rip=0x1002;c.syscallRip=0x1000;c.reg[0].setU64(-4ULL);
 t.queuePendingSignal(10);assert(c.hasDeliverableSignal());
 c.sigMask=1ULL<<9;assert(!c.hasDeliverableSignal());assert(!c.deliverPendingSignals());
 c.sigMask=0;assert(c.deliverPendingSignals(0));
 assert(capturedRip==0x1000 && capturedResult==0 && t.pendingSignals==0);
 c.rip=0x1002;c.reg[0].setU64(-4ULL);t.queuePendingSignal(10);
 assert(c.deliverPendingSignals(202));assert(capturedRip==0x1000 && capturedResult==202);
 c.sigActions[10].flags=0;c.rip=0x1002;c.reg[0].setU64(-4ULL);t.queuePendingSignal(10);
 assert(c.deliverPendingSignals(0));assert(capturedRip==0x1002 && capturedResult==-4ULL);
 c.sigActions[11]={true,1,K_SA_RESTART};t.queuePendingSignal(11);
 c.rip=0x1002;c.reg[0].setU64(-4ULL);assert(!c.hasDeliverableSignal());
 assert(!c.deliverPendingSignals(0));assert(c.rip==0x1002 && c.reg[0].u64==-4ULL);
 t.pendingSignals=0;

 auto condition=std::make_shared<Condition>();t.interruptibleWait64=true;
 t.queuePendingSignal(10); // signal before the waiter registers: must not park
 assert(!setThreadWaitingCondition(&t,condition));assert(!t.waitingCond);
 t.pendingSignals=0;
 std::unique_lock lock(condition->mutex);
 assert(setThreadWaitingCondition(&t,condition));
 std::thread sender([&]{t.queuePendingSignal(10);}); // signal after registration
 assert(condition->cv.wait_for(lock,std::chrono::seconds(1),[&]{return c.hasDeliverableSignal();}));
 lock.unlock();sender.join();t.waitingCond=nullptr;t.pendingSignals=0;
 c.sigMask=1ULL<<9;t.queuePendingSignal(10);
 assert(setThreadWaitingCondition(&t,condition)); // masked signals do not interrupt
 t.waitingCond=nullptr;t.pendingSignals=0;c.sigMask=0;

 FEXCore::Core::CpuStateFrame frame;FexThread ft{&frame};frame.Thread=&ft;
 BVNFEXCPU64Adapter adapter{&ft,&t,&c};frame.State.rip=K64_NATIVE_GUEST_INTERP_BASE+0x100;
 frame.State.vectors.fill(0xa5);auto before=frame.State.vectors;
 assert(handleScalarYield(&adapter,&frame,24));assert(frame.State.vectors==before);
 assert(frame.State.rip==K64_NATIVE_GUEST_INTERP_BASE+0x102);
 assert(frame.State.gregs[0]==0 && frame.State.gregs[1]==frame.State.rip);
 t.queuePendingSignal(10);assert(!handleScalarYield(&adapter,&frame,24));t.pendingSignals=0;
 frame.State.rip=0x140001000;assert(!handleScalarYield(&adapter,&frame,24)); // PE thunk
 frame.State.rip=K64_NATIVE_GUEST_INTERP_BASE+0x100;assert(!handleScalarYield(&adapter,&frame,0));
 t.terminating=true;assert(!handleScalarYield(&adapter,&frame,24));
}
'''

with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "boundaries.cpp"
    binary = Path(tmp) / "boundaries"
    source.write_text(code)
    subprocess.run(["c++", "-std=c++20", "-pthread", str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
print("Guest runtime boundary fixtures passed")
