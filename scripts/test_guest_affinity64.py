"""Exercise the production Linux affinity and Vulkan surface teardown paths."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent
source = (repo / "source/kernel/syscall64.cpp").read_text()
body = source.split("static U64 sys_sched_getaffinity64(", 1)[1].split("\n}\n", 1)[0]
vulkan = (repo / "source/vulkan/vulkanbridge64.cpp").read_text()
destroy = vulkan.split("S64 dispatchCommand(", 1)[1].split("    case VKB_DestroySurfaceKHR:\n", 1)[1].split("    case ", 1)[0]
code = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#define U64 unsigned long long int
using U32=uint32_t;
constexpr U64 K_EFAULT=14,K_EINVAL=22;
namespace Platform {U32 count=6;U32 getCpuCount(){return count;}}
namespace KSystem {U32 cpuAffinityCountForApp=0;}
struct Memory {std::array<U64,8> words;void writeq(U64 a,U64 v){words.at(a/8)=v;}};
struct CPU64 {Memory* memory;};
static U64 sys_sched_getaffinity64(''' + body + r'''
}
using VkInstance=void*;using VkSurfaceKHR=U64;
using PFN_vkDestroySurfaceKHR=void(*)(VkInstance,VkSurfaceKHR,void*);
int stage=0;
void nativeDestroy(VkInstance instance,VkSurfaceKHR surface,void*) {
 assert(instance==(void*)7 && surface==42 && stage==0);stage=1;
}
struct Vulkan {void destroyVulkanSurface(void* surface){assert(surface==(void*)42 && stage==1);stage=2;}};
namespace KNativeSystem {Vulkan vk;Vulkan* getVulkan(){return &vk;}}
int teardown() {
 auto raw=nativeDestroy;
 auto H=[](int){return (void*)7;};auto A=[](int){return static_cast<U64>(42);};
''' + destroy + r'''
}
int main(){
 Memory m;m.words.fill(0xcccc);CPU64 cpu{&m};
 assert(sys_sched_getaffinity64(&cpu,0,24,8)==8);
 assert(m.words[1]==63 && m.words[2]==0xcccc);
 KSystem::cpuAffinityCountForApp=2;
 assert(sys_sched_getaffinity64(&cpu,0,8,8)==8 && m.words[1]==3);
 KSystem::cpuAffinityCountForApp=0;Platform::count=65;
 assert(sys_sched_getaffinity64(&cpu,0,8,8)==static_cast<U64>(-K_EINVAL));
 assert(sys_sched_getaffinity64(&cpu,0,16,8)==16);
 assert(m.words[1]==~static_cast<U64>(0) && m.words[2]==1 && m.words[3]==0xcccc);
 Platform::count=0;
 assert(sys_sched_getaffinity64(&cpu,0,8,8)==8 && m.words[1]==1);
 assert(sys_sched_getaffinity64(&cpu,0,7,8)==static_cast<U64>(-K_EINVAL));
 assert(sys_sched_getaffinity64(&cpu,0,8,0)==static_cast<U64>(-K_EFAULT));
 assert(teardown()==0 && stage==2);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / "test.cpp"
    exe = Path(tmp) / "test"
    src.write_text(code)
    subprocess.run(["c++", "-std=c++17", str(src), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("Production affinity and surface lifetime fixtures passed")
