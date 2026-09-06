"""Compile the actual fstat64 implementation against a tiny guest-file fixture."""
import os, shutil, subprocess, tempfile
from pathlib import Path
from test_x64_wine_debug_channels import function_body
root=Path(__file__).resolve().parent.parent
source=(root/"source/kernel/syscall64.cpp").read_text()
fixture=r"""
#include <cstdint>
#include <cstring>
#include <memory>
#include <cassert>
using U8=uint8_t; using U32=uint32_t; using U64=uint64_t; using FD=int;
constexpr int K_ENOSYS=38, K_EFAULT=14;
struct KMemory64 { U8 bytes[160]{}; void memcpyToGuest(U64 a,const void*p,U64 n){memcpy(bytes+a,p,n);} };
struct Node { U64 id=9,rdev=0; U64 lastModified(){return 1788724800123ULL;} U32 getMode(){return 0100644;} };
struct Open { std::shared_ptr<Node> node=std::make_shared<Node>(); U64 length(){return 2590208;} };
struct Object { virtual ~Object()=default; };
struct KFile:Object { std::shared_ptr<Open> openFile=std::make_shared<Open>(); };
struct Descriptor { std::shared_ptr<Object> kobject=std::make_shared<KFile>(); };
using KFileDescriptorPtr=std::shared_ptr<Descriptor>;
struct Process { KFileDescriptorPtr getFileDescriptor(FD fd){return fd==3?std::make_shared<Descriptor>():nullptr;} };
struct Thread { std::shared_ptr<Process> process=std::make_shared<Process>(); };
struct CPU64 { Thread* thread; KMemory64* memory; };
void reportWineDebugStderrIdentity(Process*,const char*,int,U32,U64){}
"""
fixture+=function_body(source,"static void writeStatBuf64(")+"\n"
fixture+=function_body(source,"static U64 sys_fstat64(")+r"""
int main(){
    KMemory64 memory; Thread thread; CPU64 cpu{&thread,&memory};
    assert(sys_fstat64(&cpu,3,8)==0);
    U64 size=0,mtime=0;
    memcpy(&size,memory.bytes+8+48,8); memcpy(&mtime,memory.bytes+8+88,8);
    assert(size==2590208); assert(mtime==1788724800);
    // The previous millisecond value exceeds Windows SYSTEMTIME's range;
    // a CRT fstat then fails and Mono treats the assembly length as zero.
    assert(mtime < 910670256000ULL);
    assert((int64_t)sys_fstat64(&cpu,9,8)==-9);
}
"""
with tempfile.TemporaryDirectory(prefix="boxedvn-stat-") as tmp:
    tmp=Path(tmp); cpp=tmp/"stat.cpp"; exe=tmp/"stat-test"
    cpp.write_text(fixture)
    subprocess.run([os.environ.get("CXX","g++"),"-std=c++17",str(cpp),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print("fstat64 reports Linux seconds, preserves size, and rejects invalid descriptors")
