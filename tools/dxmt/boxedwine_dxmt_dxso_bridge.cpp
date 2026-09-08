/* BoxedWine DXSO host-call bridge. GPL-2.0-or-later.
 * Dispatches native and WoW64 D3D9 shader parameters to the native compiler.
 * Guest pointers are translated; opaque compiler handles stay host values. */
#include <cstring>
#include <cstdint>
#include <cstddef>
#include "airconv_public.h"
#define _Static_assert static_assert
#include "airconv_thunks.h"
#undef _Static_assert
#include "boxedwine_dxmt_guest_pointer.h"

namespace {
union Node {
    DXSO_SHADER_COMPILATION_ARGUMENT_DATA header;
    DXSO_SHADER_IA_INPUT_LAYOUT_DATA layout;
    DXSO_SHADER_PSO_PIXEL_SHADER_DATA pixel;
    DXSO_SHADER_PS_SAMPLER_LAYOUT_DATA samplers;
    DXSO_SHADER_PS_POINT_SPRITE_DATA sprite;
    DXSO_SHADER_VS_POINT_SIZE_DATA point;
    DXSO_SHADER_PS_FOG_DATA fog;
    DXSO_SHADER_FFP_KEY_DATA ffp;
};
template<typename T> T read(const void* p, size_t offset) {
    T value; std::memcpy(&value, static_cast<const uint8_t*>(p)+offset, sizeof(value)); return value;
}
template<typename T> T* guest(uint64_t address) {
    return reinterpret_cast<T*>(boxedwine_dxmt_host_pointer(address));
}
bool copyArguments(uint64_t address, bool wow64, Node (&nodes)[16], DXSO_SHADER_COMPILATION_ARGUMENT_DATA*& head) {
    head = nullptr;
    uint64_t visited[16]{};
    unsigned count = 0;
    while (address) {
        if (count == 16) return false;
        for (unsigned i=0; i<count; ++i) if (visited[i] == address) return false;
        visited[count] = address;
        const uint8_t* source = guest<const uint8_t>(address);
        uint64_t next = wow64 ? read<uint32_t>(source,0) : read<uint64_t>(source,0);
        const size_t typeOffset = wow64 ? 4 : 8;
        uint32_t type = read<uint32_t>(source,typeOffset);
        Node& node = nodes[count];
        std::memset(&node,0,sizeof(node));
        size_t payload = 0;
        switch (type) {
        case DXSO_SHADER_IA_INPUT_LAYOUT: {
            auto& layout = node.layout;
            layout.index_buffer_format = static_cast<DXSO_INDEX_BUFFER_FORMAT>(read<uint32_t>(source,typeOffset+4));
            layout.slot_mask = read<uint32_t>(source,typeOffset+8);
            layout.num_elements = read<uint32_t>(source,typeOffset+12);
            const uint64_t elements = wow64 ? read<uint32_t>(source,20) : read<uint64_t>(source,24);
            if (layout.num_elements > 64 || (layout.num_elements && !elements)) return false;
            layout.elements = guest<DXSO_IA_INPUT_ELEMENT>(elements);
            layout.position_transformed = read<uint32_t>(source,wow64 ? 24 : 32);
            layout.vs_float_const_count = read<uint32_t>(source,wow64 ? 28 : 36);
            break;
        }
        case DXSO_SHADER_PSO_PIXEL_SHADER: payload = 5*sizeof(uint32_t); break;
        case DXSO_SHADER_PS_SAMPLER_LAYOUT: payload = 16; break;
        case DXSO_SHADER_PS_POINT_SPRITE: case DXSO_SHADER_VS_POINT_SIZE: break;
        case DXSO_SHADER_PS_FOG: payload = 2*sizeof(uint32_t); break;
        case DXSO_SHADER_FFP_KEY:
            payload = offsetof(DXSO_SHADER_FFP_KEY_DATA,emit_sample_mask)+4-
                      offsetof(DXSO_SHADER_FFP_KEY_DATA,kind); break;
        default: return false; // unknown layouts must never be silently truncated
        }
        if (payload) std::memcpy(reinterpret_cast<uint8_t*>(&node)+12,source+typeOffset+4,payload);
        node.header.type = static_cast<DXSO_SHADER_COMPILATION_ARGUMENT_TYPE>(type);
        if (count) nodes[count-1].header.next = &node;
        else head = &node.header;
        ++count; address = next;
    }
    return true;
}
}

extern "C" uint32_t boxedwine_dxmt_dxso_call(uint32_t call, void* params, bool wow64) {
    constexpr uint32_t invalid = 0xc000000d;
    if (!params) return invalid;
    // The outer block is already translated by the host-call dispatcher.
    // Handles are native opaque 64-bit values even for WoW64; addresses aren't.
    switch (call) {
    case unix_dxso_initialize: {
        uint64_t bytes, code, output;
        int* result;
        if (wow64) {
            auto* p = static_cast<dxso_initialize_params32*>(params);
            code=p->bytecode; bytes=p->bytecode_size; output=p->shader; result=&p->ret;
        } else {
            auto* p = static_cast<dxso_initialize_params*>(params);
            code=reinterpret_cast<uintptr_t>(p->bytecode); bytes=p->bytecode_size;
            output=reinterpret_cast<uintptr_t>(p->shader); result=&p->ret;
        }
        if (!code || !output || bytes < 4 || bytes > 16*1024*1024) return invalid;
        *guest<dxso_shader_t>(output) = nullptr;
        *result = DXSOInitialize(guest<const void>(code), bytes, guest<dxso_shader_t>(output));
        return 0;
    }
    case unix_dxso_compile: {
        dxso_shader_t shader;
        uint64_t args,name,output; int* result;
        if (wow64) {
            auto* p = static_cast<dxso_compile_params32*>(params);
            shader=p->shader; args=p->args; name=p->func_name; output=p->bitcode; result=&p->ret;
        } else {
            auto* p = static_cast<dxso_compile_params*>(params);
            shader=p->shader; args=reinterpret_cast<uintptr_t>(p->args);
            name=reinterpret_cast<uintptr_t>(p->func_name); output=reinterpret_cast<uintptr_t>(p->bitcode); result=&p->ret;
        }
        if (!name || !output) return invalid;
        *guest<dxso_bitcode_t>(output) = nullptr;
        Node nodes[16]; DXSO_SHADER_COMPILATION_ARGUMENT_DATA* chain;
        if (!copyArguments(args,wow64,nodes,chain)) { *result=-1; return invalid; }
        *result=DXSOCompile(shader,chain,guest<const char>(name),guest<dxso_bitcode_t>(output));
        return 0;
    }
    case unix_dxso_get_compiled_bitcode: {
        dxso_bitcode_t bitcode; uint64_t output;
        if (wow64) {
            auto* p=static_cast<dxso_get_compiled_bitcode_params32*>(params);
            bitcode=p->bitcode; output=p->data_out;
        } else {
            auto* p=static_cast<dxso_get_compiled_bitcode_params*>(params);
            bitcode=p->bitcode; output=reinterpret_cast<uintptr_t>(p->data_out);
        }
        if (!output) return invalid;
        auto* data=guest<SM50_COMPILED_BITCODE>(output);
        DXSOGetCompiledBitcode(bitcode,data);
        data->Data=reinterpret_cast<void*>(boxedwine_dxmt_tag_host_pointer(reinterpret_cast<uintptr_t>(data->Data)));
        return 0;
    }
    case unix_dxso_destroy: DXSODestroy(static_cast<dxso_destroy_params*>(params)->shader); return 0;
    case unix_dxso_destroy_bitcode: DXSODestroyBitcode(static_cast<dxso_destroy_bitcode_params*>(params)->bitcode); return 0;
    default: return invalid;
    }
}
