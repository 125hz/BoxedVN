"""Exercise the real DXSO bridge with guest aliases and compiler boundary stubs."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

p=argparse.ArgumentParser()
p.add_argument('--source',type=Path,required=True)
a=p.parse_args()
root=Path(__file__).resolve().parent.parent
fixture=r'''
#include <cassert>
#include <sys/mman.h>
#include "boxedwine_dxmt_dxso_bridge.cpp"
static int calls=0, destroyed=0;
static unsigned expectedType=0;
static uint8_t bitcodeBytes[8];
extern "C" int DXSOInitialize(const void* code,size_t bytes,dxso_shader_t* shader) {
    assert(*static_cast<const uint32_t*>(code)==0xfffe0300 && bytes==8);
    *shader=reinterpret_cast<void*>(0x12340000); ++calls; return 0;
}
extern "C" void DXSODestroy(dxso_shader_t shader) {assert(shader==reinterpret_cast<void*>(0x12340000));++destroyed;}
extern "C" void DXSODestroyBitcode(dxso_bitcode_t code) {assert(code==reinterpret_cast<void*>(0x56780000));++destroyed;}
extern "C" void DXSOGetCompiledBitcode(dxso_bitcode_t code,SM50_COMPILED_BITCODE* data) {
    assert(code==reinterpret_cast<void*>(0x56780000)); data->Data=bitcodeBytes; data->Size=8;
}
extern "C" int DXSOCompile(dxso_shader_t shader,DXSO_SHADER_COMPILATION_ARGUMENT_DATA* chain,const char* name,dxso_bitcode_t* code) {
    assert(shader==reinterpret_cast<void*>(0x12340000));assert(!strcmp(name,"main"));
    assert(chain && chain->type==expectedType);
    if (expectedType==DXSO_SHADER_IA_INPUT_LAYOUT) {
        auto* layout=reinterpret_cast<DXSO_SHADER_IA_INPUT_LAYOUT_DATA*>(chain);
        assert(layout->slot_mask==3 && layout->num_elements==1);
        assert(layout->elements->reg==7 && layout->position_transformed==1 && layout->vs_float_const_count==256);
        assert(chain->next && static_cast<DXSO_SHADER_COMPILATION_ARGUMENT_DATA*>(chain->next)->type==DXSO_SHADER_PS_FOG);
        auto* fog=static_cast<DXSO_SHADER_PS_FOG_DATA*>(chain->next);
        assert(fog->mode==2 && fog->coord_is_w==1 && !fog->next);
    } else {
        auto* ffp=reinterpret_cast<DXSO_SHADER_FFP_KEY_DATA*>(chain);
        assert(ffp->kind==1 && ffp->stages[7][2]==0xabc && ffp->emit_sample_mask==1 && !ffp->next);
    }
    *code=reinterpret_cast<void*>(0x56780000); ++calls; return 0;
}
int main() {
    constexpr uintptr_t low=0x100000, alias=0x7800100000;
    auto* mapped=static_cast<uint8_t*>(mmap(reinterpret_cast<void*>(alias),65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0));
    assert(mapped==reinterpret_cast<void*>(alias));
    auto raw=[&](size_t offset){return low+offset;};
    *reinterpret_cast<uint32_t*>(mapped)=0xfffe0300;
    strcpy(reinterpret_cast<char*>(mapped+32),"main");
    auto* element=reinterpret_cast<DXSO_IA_INPUT_ELEMENT*>(mapped+64);element->reg=7;
    for(bool wow64:{false,true}) {
        dxso_initialize_params init{reinterpret_cast<void*>(raw(0)),8,reinterpret_cast<dxso_shader_t*>(raw(100)),99};
        dxso_initialize_params32 init32{uint32_t(raw(0)),8,uint32_t(raw(100)),99};
        assert(boxedwine_dxmt_dxso_call(unix_dxso_initialize,wow64 ? static_cast<void*>(&init32):&init,wow64)==0);
        auto shader=*reinterpret_cast<dxso_shader_t*>(mapped+100);
        assert((wow64?init32.ret:init.ret)==0);
        DXSO_SHADER_IA_INPUT_LAYOUT_DATA layout{};
        layout.next=reinterpret_cast<void*>(raw(512)); layout.type=DXSO_SHADER_IA_INPUT_LAYOUT;
        layout.slot_mask=3;layout.num_elements=1;layout.elements=reinterpret_cast<DXSO_IA_INPUT_ELEMENT*>(raw(64));
        layout.position_transformed=1;layout.vs_float_const_count=256;
        DXSO_SHADER_PS_FOG_DATA fog{};fog.type=DXSO_SHADER_PS_FOG;fog.mode=2;fog.coord_is_w=1;
        if(wow64) {
            uint32_t l[]={uint32_t(raw(512)),1,0,3,1,uint32_t(raw(64)),1,256};
            uint32_t f[]={0,7,2,1}; memcpy(mapped+256,l,sizeof(l));memcpy(mapped+512,f,sizeof(f));
        } else {memcpy(mapped+256,&layout,sizeof(layout));memcpy(mapped+512,&fog,sizeof(fog));}
        dxso_compile_params c{shader,reinterpret_cast<DXSO_SHADER_COMPILATION_ARGUMENT_DATA*>(raw(256)),reinterpret_cast<const char*>(raw(32)),reinterpret_cast<dxso_bitcode_t*>(raw(112)),99};
        dxso_compile_params32 c32{shader,uint32_t(raw(256)),uint32_t(raw(32)),uint32_t(raw(112)),99};
        auto* params=wow64?static_cast<void*>(&c32):&c;
        expectedType=DXSO_SHADER_IA_INPUT_LAYOUT;
        assert(boxedwine_dxmt_dxso_call(unix_dxso_compile,params,wow64)==0);
        auto code=*reinterpret_cast<dxso_bitcode_t*>(mapped+112);
        dxso_get_compiled_bitcode_params b{code,reinterpret_cast<SM50_COMPILED_BITCODE*>(raw(128))};
        dxso_get_compiled_bitcode_params32 b32{code,uint32_t(raw(128))};
        assert(!boxedwine_dxmt_dxso_call(unix_dxso_get_compiled_bitcode,wow64?static_cast<void*>(&b32):&b,wow64));
        auto* data=reinterpret_cast<SM50_COMPILED_BITCODE*>(mapped+128);
        assert(data->Size==8 && boxedwine_dxmt_host_pointer(reinterpret_cast<uintptr_t>(data->Data))==reinterpret_cast<uintptr_t>(bitcodeBytes));
        // Reject cycles and unknown node layouts without invoking the compiler.
        auto before=calls;
        if(wow64)*reinterpret_cast<uint32_t*>(mapped+512)=raw(256);
        else *reinterpret_cast<uint64_t*>(mapped+512)=raw(256);
        assert(boxedwine_dxmt_dxso_call(unix_dxso_compile,params,wow64)==0xc000000d && calls==before);
        DXSO_SHADER_FFP_KEY_DATA ffp{};ffp.type=DXSO_SHADER_FFP_KEY;ffp.kind=1;ffp.stages[7][2]=0xabc;ffp.emit_sample_mask=1;
        memset(mapped+256,0,512);
        if(wow64) {
            *reinterpret_cast<uint32_t*>(mapped+260)=DXSO_SHADER_FFP_KEY;
            memcpy(mapped+264,reinterpret_cast<uint8_t*>(&ffp)+12,offsetof(DXSO_SHADER_FFP_KEY_DATA,emit_sample_mask)+4-12);
        } else memcpy(mapped+256,&ffp,sizeof(ffp));
        expectedType=DXSO_SHADER_FFP_KEY;
        assert(!boxedwine_dxmt_dxso_call(unix_dxso_compile,params,wow64));
        *reinterpret_cast<uint32_t*>(mapped+256+(wow64?4:8))=0xdead;
        before=calls;assert(boxedwine_dxmt_dxso_call(unix_dxso_compile,params,wow64)==0xc000000d && calls==before);
        dxso_destroy_params d{shader};dxso_destroy_bitcode_params db{code};
        assert(!boxedwine_dxmt_dxso_call(unix_dxso_destroy,&d,wow64));
        assert(!boxedwine_dxmt_dxso_call(unix_dxso_destroy_bitcode,&db,wow64));
    }
    assert(calls==6 && destroyed==4);
    assert(boxedwine_dxmt_dxso_call(0,mapped,false)==0xc000000d);
    munmap(mapped,65536);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    source=Path(tmp,'test.cpp');source.write_text(fixture)
    binary=Path(tmp,'test')
    subprocess.run([os.environ.get('CXX','clang++'),'-std=c++20','-DDXMT_NATIVE=1',
        '-I'+str(root/'include'),'-I'+str(root/'tools/dxmt'),
        '-I'+str(a.source/'src/airconv'),'-I'+str(a.source/'src/winemetal'),
        str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('DXSO native/WoW64 alias, argument-chain, handle and bitcode tests passed')
