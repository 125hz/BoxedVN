#include <utility>
#include <initializer_list>
#include <cassert>
#include <cstdint>
#include <cstddef>
// Exercise the Apple-only inline helper without requiring an Apple SDK.
#define __APPLE__ 1
#include <FEXCore/Utils/DualMap.h>
#undef __APPLE__
unsigned char rx1[64], rw1[64], rx2[64], rw2[64], ordinary[64];
int64_t lookup(const void* p) {
  auto a=reinterpret_cast<uintptr_t>(p);
  for(auto pair : {std::pair<unsigned char*,unsigned char*>{rx1,rw1},{rx2,rw2}}) {
    auto r=reinterpret_cast<uintptr_t>(pair.first);
    if(a>=r && a-r<64) return int64_t(reinterpret_cast<uintptr_t>(pair.second))-int64_t(r);
  }
  return 0;
}
namespace FEXCore::DualMap { int64_t WriteOffset=0; int64_t (*WriteOffsetLookup)(const void*)=lookup; }
int main() {
  *FEXCore::DualMap::WriteAddr(rx1+3)=0x12;
  *FEXCore::DualMap::WriteAddr(rx2+7)=0x34;
  *FEXCore::DualMap::WriteAddr(ordinary+9)=0x56;
  assert(rx1[3]==0 && rw1[3]==0x12 && rx2[7]==0 && rw2[7]==0x34 && ordinary[9]==0x56);
  assert(FEXCore::DualMap::WriteAddr(static_cast<void*>(rx2+63))==rw2+63);
}
