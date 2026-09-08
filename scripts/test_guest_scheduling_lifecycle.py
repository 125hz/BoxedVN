"""Compile production scheduling, helper-fault, and session-exit paths with fixtures.

GCD block syntax is adapted to C++ lambdas for the deterministic task queue.
The Apple rusage ABI is tested with Mach stubs; native Linux accounting is also
compiled and exercised. This does not execute ARM64 helper instructions on iOS.
"""
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


common = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <pthread.h>
#include <sys/resource.h>
#include "getrusagefairness.h"
#define U64 unsigned long long int
using S64=long long;using U32=uint32_t;
constexpr U64 K_EFAULT=14,K_EINVAL=22,K_EIO=5;
void klog_fmt(const char*,...) {}
#define BOXEDWINE_IOS
#define BOXEDWINE_MULTI_THREADED
#define BOXEDWINE_FEX64_BACKEND
namespace bvnFairness {
 std::atomic<uint64_t> throttleCount{0},throttleMicroseconds{0},getrusageCalls{0},schedYieldCalls{0};
}
namespace KSystem {uint64_t ticks=100;uint64_t getMicroCounter(){return ticks++;}
 uint64_t getSystemTimeAsMicroSeconds(){return 1234567;}}
struct KThread {unsigned id=11;GetrusageFairness schedYieldFairness;U64 voluntaryYieldCount64=0;
 U64 cachedThreadRusageAt=0,cachedThreadRusage[4]={};bool hasCachedThreadRusage=false;};
constexpr int K64_PAGE_SHIFT=12,K64_PAGE_WRITE=2,X64_SYS_getrusage=98,X64_SYS_clock_gettime=228;
struct Memory {std::array<U64,1024> words{};unsigned permissions[2]={3,3};bool committed[2]={true,true};
 U32 getPageFlags(U64 page){return page<2 ? permissions[page] : 0;}
 void* getCommittedPagePtr(U64 page){return page<2 && committed[page] ? words.data()+page*512 : nullptr;}
 void memsetGuest(U64 address,int value,U64 bytes){memset((char*)words.data()+address,value,bytes);}
 void writeq(U64 address,U64 value){words.at(address/8)=value;}};
struct CPU64 {Memory* memory;KThread* thread;};
'''
scheduling = method("source/kernel/syscall64.cpp", "void kschedYield64(")
rusage = method("source/kernel/syscall64.cpp", "static U64 sys_getrusage64(")
polling = method("source/kernel/syscall64.cpp", "static U64 sys_clock_gettime64(")
polling += method("source/kernel/syscall64.cpp", "bool kpollingQuery64(")
apple = r'''
#define __APPLE__
struct time_value_t {int seconds,microseconds;};
struct thread_basic_info_data_t {time_value_t user_time,system_time;};
using mach_msg_type_number_t=unsigned;using thread_info_t=thread_basic_info_data_t*;
constexpr int THREAD_BASIC_INFO_COUNT=10,THREAD_BASIC_INFO=3,KERN_SUCCESS=0;
int pthread_mach_thread_np(pthread_t){return 7;}
int machResult=0,machCalls=0;
int thread_info(int port,int flavor,thread_info_t info,mach_msg_type_number_t* count){
 ++machCalls;
 assert(port==7 && flavor==THREAD_BASIC_INFO && *count==THREAD_BASIC_INFO_COUNT);
 *info={{12,3456},{2,7890}};return machResult;
}
'''
scheduling_tests = r'''
int main(){
 Memory m;KThread thread;CPU64 cpu{&m,&thread};m.words.fill(0xcccc);
 assert(sys_getrusage64(&cpu,7,8)==static_cast<U64>(-K_EINVAL));
 assert(m.words[1]==0xcccc);
 assert(sys_getrusage64(&cpu,1,0)==static_cast<U64>(-K_EFAULT));
 assert(sys_getrusage64(&cpu,1,8)==0);
 assert(m.words[0]==0xcccc && m.words[19]==0xcccc); // exact 144-byte ABI
#ifdef __APPLE__
 assert(m.words[1]==12 && m.words[2]==3456 && m.words[3]==2 && m.words[4]==7890);
 assert(m.words[17]==0 && m.words[18]==0);
 assert(machCalls==1);
 for(int i=0;i<100;i++)assert(sys_getrusage64(&cpu,1,8)==0);
 assert(machCalls==1); // burst polling performs only one Mach RPC
 KSystem::ticks+=1000;machResult=1;
 assert(sys_getrusage64(&cpu,1,8)==static_cast<U64>(-K_EIO));machResult=0;
#else
 volatile uint64_t busy=0;auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(25);
 while(std::chrono::steady_clock::now()<until)++busy;
 assert(sys_getrusage64(&cpu,1,8)==0 && (m.words[1] || m.words[2] || m.words[3] || m.words[4]));
#endif
 // No sleep on an ordinary yield; sustained polling gives a bounded handoff.
 kschedYield64(&cpu);assert(thread.voluntaryYieldCount64==0);
 for(int i=0;i<100;i++)kschedYield64(&cpu);
 assert(thread.voluntaryYieldCount64==1 && bvnFairness::throttleCount==1);
 for(int i=0;i<100;i++)kschedYield64(&cpu);
 assert(thread.voluntaryYieldCount64==1); // cannot turn into per-call sleeping
 KSystem::ticks+=4000;kschedYield64(&cpu);assert(thread.voluntaryYieldCount64==2);
 assert(sys_getrusage64(&cpu,1,8)==0);
#ifdef __APPLE__
 assert(m.words[17]==2 && m.words[18]==0); // Linux ru_nvcsw at byte 128
#endif
 KSystem::ticks+=10000;kschedYield64(&cpu);assert(thread.voluntaryYieldCount64==2);
 U64 result=999;
 assert(kpollingQuery64(&cpu,98,1,8,result) && result==0);
 assert(kpollingQuery64(&cpu,228,7,8,result) && m.words[1]==1 && m.words[2]==234567000);
 assert(!kpollingQuery64(&cpu,98,0,8,result)); // process accounting stays on full path
 assert(!kpollingQuery64(&cpu,228,99,8,result));
 assert(!kpollingQuery64(&cpu,0,1,8,result));
 assert(!kpollingQuery64(&cpu,98,1,0,result));
 assert(!kpollingQuery64(&cpu,98,1,~0ULL-20,result));
 m.permissions[1]=1;
 assert(!kpollingQuery64(&cpu,98,1,4088,result)); // straddles a read-only page
 m.permissions[1]=3;m.committed[1]=false;
 assert(!kpollingQuery64(&cpu,98,1,4088,result)); // demand commit may need full CPU state
 m.committed[1]=true;assert(kpollingQuery64(&cpu,98,1,4088,result));
}
'''

fault = method("ios/runtime/src/BVNFEXCPU64Adapter.mm",
               "if (signal == SIGBUS && siginfo->si_code == BUS_ADRALN && inOwnedFexCode &&")
fault_code = common + (repo / "include/fex_host_abort.h").read_text() + r'''
constexpr int SIGBUS=10,BUS_ADRALN=1;
struct Info {int si_code;};
struct Machine {struct {uint64_t __x[31]{},__pc=0;} __ss;struct {uint32_t __esr=0x92000061;} __es;};
bool BVNFEXBackendPatchUnalignedSwap(uint64_t,uint32_t){return false;}
struct Process {int id=10;};struct Adapter {void* fexThread=nullptr;Process* process;KThread* thread;};
bool expectedJit=false;int atomicCalls=0;
namespace FEXCore::ArchHelpers::Arm64 {
 enum class UnalignedHandlerType {HalfBarrier};
 std::optional<int> HandleUnalignedAccess(void*,UnalignedHandlerType,uint64_t pc,uint64_t*,bool jit){
  assert(jit==expectedJit);++atomicCalls;
  if(*(uint32_t*)pc==0xf8ea806a)return 4;return {};
 }
}
bool recover(Adapter* adapter,Machine* machine,int signal,Info* siginfo,
             bool inCodeBuffer,bool inOwnedFexCode,uint64_t hostPC){
 uint64_t faultAddress=0x7802881e2f,unalignedPC=hostPC;bool swapFault=false;
''' + fault + r'''
 return false;
}
int main(){
 uint32_t op=0xf8ea806a;auto pc=(uint64_t)&op;Process p;KThread t;Adapter a{nullptr,&p,&t};Machine m;Info info{BUS_ADRALN};
 assert(recover(&a,&m,SIGBUS,&info,false,true,pc) && m.__ss.__pc==pc+4); // generated helper
 expectedJit=true;assert(recover(&a,&m,SIGBUS,&info,true,true,pc)); // translated block
 int count=atomicCalls;
 assert(!recover(&a,&m,SIGBUS,&info,false,false,pc) && atomicCalls==count); // unrelated native code
 assert(!recover(&a,&m,11,&info,true,true,pc) && atomicCalls==count); // not an alignment fault
 info.si_code=2;assert(!recover(&a,&m,SIGBUS,&info,true,true,pc) && atomicCalls==count);
 info.si_code=BUS_ADRALN;op=0;assert(!recover(&a,&m,SIGBUS,&info,true,true,pc)); // unsupported opcode chains
}
'''

lifecycle = common + r'''
enum {BVNRuntimeStateRunning,BVNRuntimeStateStarting,BVNRuntimeStateStopping,BVNRuntimeStateStopped};
enum {BVNLogLevelInfo,QOS_CLASS_UTILITY,DISPATCH_TIME_NOW,NSEC_PER_MSEC=1000000,NSEC_PER_SEC=1000000000};
struct BVNGuestExitReport {bool valid=false;uint32_t pid=0,status=0;char missingModule[256]{};};
BVNGuestExitReport gLastGuestExit;int gState=BVNRuntimeStateRunning;
BVNGuestExitReport gLastChildExit;uint64_t gLastChildExitGeneration=0;
pthread_mutex_t gMutex=PTHREAD_MUTEX_INITIALIZER;
std::atomic<uint64_t> gLaunchGeneration{1},gExitWatchGeneration{UINT64_MAX};
int BVNRuntimeGetState(){return gState;}
int shutdowns=0;void BVNRuntimeRequestShutdown(){++shutdowns;gState=BVNRuntimeStateStopping;}
void BVNLogWrite(int,const char*,const char*){}
void copyString(char* dst,size_t n,const char* src){snprintf(dst,n,"%s",src);}
std::vector<std::function<void()>> tasks;
int dispatch_get_global_queue(int,int){return 0;}
uint64_t dispatch_time(int,uint64_t delay){return delay;}
void dispatch_after(uint64_t,int,std::function<void()> task){tasks.push_back(task);}
void dispatch_async(int,std::function<void()> task){tasks.push_back(task);}
void next(){assert(!tasks.empty());auto task=tasks.front();tasks.erase(tasks.begin());task();}
struct Command {std::string value;bool contains(const char* needle,bool insensitive){
 auto v=value;if(insensitive)std::transform(v.begin(),v.end(),v.begin(),[](unsigned char c){return std::tolower(c);});
 return v.find(needle)!=std::string::npos;}};
struct UserProcess {bool terminated=false,system=false;Command commandLine;
 bool isSystemProcess(){return system;}};
namespace KSystem {
 std::array<std::shared_ptr<UserProcess>,4> processes;
 std::vector<U32> getProcessIdsWithThreads(){return {0,1,2,3};}
 auto getProcess(U32 id){return processes[id];}
}
'''
lifecycle += method("source/kernel/syscall64.cpp", 'extern "C" uint32_t BVNRuntimeLiveUserProcess(')
for signature in ['extern "C" void BVNRuntimeNoteChildProcessExited(', "static void finishLaunchedProcessExit(", 'extern "C" void BVNRuntimeNoteLaunchedProcessExited(']:
    lifecycle += method("ios/runtime/src/BVNRuntime.mm", signature).replace("^{", "[=]{")
lifecycle += r'''
int main(){
 using namespace KSystem;
 processes[0]=std::make_shared<UserProcess>(UserProcess{false,false,{"wine64 Launcher.EXE"}});
 processes[1]=std::make_shared<UserProcess>(UserProcess{false,false,{"wine64 Program.EXE"}});
 processes[2]=std::make_shared<UserProcess>(UserProcess{false,true,{"wine64 services.exe"}});
 BVNRuntimeNoteLaunchedProcessExited(0,0,"");BVNRuntimeNoteLaunchedProcessExited(0,0,"");
 assert(tasks.size()==1);next();assert(shutdowns==0 && !gLastGuestExit.valid && tasks.size()==1);
 next();assert(shutdowns==0); // child still loading: keep input and presentation alive
 BVNRuntimeNoteChildProcessExited(1,0xc000001d,"child.dll");
 processes[1]->terminated=true;next();assert(shutdowns==1 && gLastGuestExit.valid);
 assert(gLastGuestExit.pid==1 && gLastGuestExit.status==0xc000001d);
 // Scheduled quit retries from the old generation cannot stop another session.
 ++gLaunchGeneration;gState=BVNRuntimeStateRunning;gLastGuestExit={};
 while(!tasks.empty())next();assert(shutdowns==1);
 processes[1]->terminated=false;
 BVNRuntimeNoteLaunchedProcessExited(0,0,"");next();assert(!gLastGuestExit.valid);
 ++gLaunchGeneration;next();assert(tasks.empty() && shutdowns==1); // stale live-child poll
 processes[1]->terminated=true;
 BVNRuntimeNoteLaunchedProcessExited(0,7,"missing.dll");next();
 assert(shutdowns==2 && gLastGuestExit.status==7); // helpers alone do not retain session
 assert(std::string(gLastGuestExit.missingModule)=="missing.dll");
}
'''

audio_queue = common + r'''
namespace KSystem {bool soundEnabled=true;}
U32 hostQueued=0;U32 SDL_GetQueuedAudioSize(int){return hostQueued;}
struct Audio {
 int deviceId=1;bool sameFormat=false;std::vector<uint8_t> audioBuffer;
 struct {uint64_t bufferedInputBytes(){return 4096;}} converter;
 void drainNoSoundAudioBuffer(){} U32 bytesPerSecondWant(){return 44100*8;}
 U32 bytesPerSecondGot(){return 48000*8;}
'''
audio_queue += method("platform/sdl/kdspaudio.cpp", "U32 getQueuedAudioSizeWant(")
audio_queue += r'''
};
int main(){
 Audio audio;
 assert(audio.getQueuedAudioSizeWant()==4096);
 assert(audio.getQueuedAudioSizeWant(false)==0); // retained filter samples cannot play
 hostQueued=4800*8;
 assert(audio.getQueuedAudioSizeWant(false)==4410*8);
 assert(audio.getQueuedAudioSizeWant()==4410*8+4096); // position accounting unchanged
 audio.sameFormat=true;assert(audio.getQueuedAudioSizeWant()==4410*8);
 audio.deviceId=0;assert(audio.getQueuedAudioSizeWant()==0);
 KSystem::soundEnabled=false;audio.audioBuffer.resize(1234);
 assert(audio.getQueuedAudioSizeWant(false)==1234);
}
'''

playback = r'''
#include "ossplaybackposition.h"
#include <cassert>
int main() {
 OssPlaybackPosition p;
 auto r=p.query(8192,4096,1024,4096);
 assert(r.bytes==4096 && r.blocks==4 && r.pointer==0);
 r=p.query(8192,2048,1024,4096);assert(r.bytes==6144 && r.blocks==2 && r.pointer==2048);
 r=p.query(8192,2048,1024,4096);assert(r.blocks==0);
 r=p.query(8192,3000,1024,4096);assert(r.bytes==6144 && r.blocks==0);
 r=p.query((1ull<<32)+100,0,1024,4096);assert(r.bytes==100 && r.pointer==100);
 p.reset();r=p.query(100,200,1024,4096);assert(!r.bytes && !r.blocks && !r.pointer);
 r=p.query(100,0,0,0);assert(r.bytes==100 && !r.blocks && !r.pointer);
}
'''

with tempfile.TemporaryDirectory() as tmp:
    for name, code in [("rusage_linux", common+scheduling+rusage+polling+scheduling_tests),
                       ("rusage_apple", common+apple+scheduling+rusage+polling+scheduling_tests),
                       ("fault", fault_code), ("lifecycle", lifecycle),
                       ("audio_queue", audio_queue), ("oss_position", playback)]:
        src = Path(tmp) / (name + ".cpp")
        exe = Path(tmp) / name
        src.write_text(code)
        subprocess.run(["c++", "-std=c++17", "-pthread", "-I"+str(repo/"include"), str(src), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
        print(name + " passed")
