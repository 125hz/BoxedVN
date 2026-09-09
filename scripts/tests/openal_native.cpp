// Exercise the actual native dispatcher using a deterministic loopback device.
#define AL_LIBTYPE_STATIC
#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#if defined(__APPLE__)
extern "C" bool bvnOpenALPrepareSession() { return true; }
#endif
static ALCdevice* testOpenDevice(const char*) { return alcLoopbackOpenDeviceSOFT(nullptr); }
#define alcOpenDevice testOpenDevice
#include "../../tools/openal/openal_native.cpp"
#undef alcOpenDevice
#include "openal_ops.h"
#include <cassert>
#include <cmath>
#include <chrono>
#include <array>

struct Guest {
    std::array<unsigned char,32768> memory{};
    static void* map(void* value,uint64_t address,size_t bytes,int) {
        auto& guest=*static_cast<Guest*>(value);
        if(address<0x1000 || address-0x1000>=guest.memory.size() || bytes>guest.memory.size()-(address-0x1000)) return nullptr;
        return guest.memory.data()+address-0x1000;
    }
    uint64_t call(uint32_t op,std::initializer_list<uint64_t> arguments={},uint64_t generation=1) {
        BvnOpenALPacket packet{}; packet.abi=BOXEDWINE_OPENAL_ABI;
        std::copy(arguments.begin(),arguments.end(),packet.args);
        assert(bvnOpenALInvoke(generation,op,&packet,map,this,1)==0);
        return packet.result;
    }
    template<class T> T& at(uint64_t address) { return *static_cast<T*>(map(this,address,sizeof(T),1)); }
};
int main() {
    Guest guest;
    auto device=guest.call(OP_alcOpenDevice);
    assert(device && device<=UINT32_MAX);
    const ALCint attributes[]={ALC_FORMAT_CHANNELS_SOFT,ALC_STEREO_SOFT,ALC_FORMAT_TYPE_SOFT,ALC_FLOAT_SOFT,ALC_FREQUENCY,48000,0};
    std::memcpy(Guest::map(&guest,0x1000,sizeof(attributes),1),attributes,sizeof(attributes));
    auto context=guest.call(OP_alcCreateContext,{device,0x1000});
    assert(context && context!=device && context<=UINT32_MAX);
    assert(guest.call(OP_alcMakeContextCurrent,{context}));
    assert(guest.call(OP_alcGetCurrentContext)==context);
    assert(guest.call(OP_alcGetContextsDevice,{context})==device);
    guest.call(OP_alGenBuffers,{1,0x1100});
    auto buffer=guest.at<ALuint>(0x1100);
    assert(buffer);
    for(unsigned n=0;n<1024;++n) guest.at<int16_t>(0x2000+n*2)=int16_t(std::sin(n*0.05)*12000);
    guest.call(OP_alBufferData,{buffer,AL_FORMAT_MONO16,0x2000,2048,48000});
    guest.call(OP_alGenSources,{64,0x1200});
    for(unsigned n=0;n<64;++n) {
        auto source=guest.at<ALuint>(0x1200+n*4);
        guest.call(OP_alSourcei,{source,AL_BUFFER,buffer});
        guest.call(OP_alSourcei,{source,AL_LOOPING,AL_TRUE});
        guest.call(OP_alSourcef,{source,AL_GAIN,pack(0.01f)});
        guest.call(OP_alSourcePlay,{source});
    }
    auto source=guest.at<ALuint>(0x1200);
    guest.call(OP_alGetSourcei,{source,AL_SOURCE_STATE,0x1104});
    assert(guest.at<ALint>(0x1104)==AL_PLAYING);
    guest.call(OP_alGenEffects,{1,0x1108});
    auto effect=guest.at<ALuint>(0x1108);
    guest.call(OP_alEffecti,{effect,AL_EFFECT_TYPE,AL_EFFECT_REVERB});
    guest.call(OP_alGenAuxiliaryEffectSlots,{1,0x110c});
    auto slot=guest.at<ALuint>(0x110c);
    guest.call(OP_alAuxiliaryEffectSloti,{slot,AL_EFFECTSLOT_EFFECT,effect});
    guest.call(OP_alSource3i,{source,AL_AUXILIARY_SEND_FILTER,slot,0,AL_FILTER_NULL});
    assert(guest.call(OP_alGetError)==AL_NO_ERROR);
    std::array<float,512> output{};
    auto* nativeDevice=states.at(1).device(device);
    const auto begin=std::chrono::steady_clock::now();
    double peak=0;
    for(unsigned frame=0;frame<1000;++frame) {
        alcRenderSamplesSOFT(nativeDevice,output.data(),256);
        for(float value:output) { assert(std::isfinite(value)); peak=std::max(peak,std::abs(double(value))); }
    }
    assert(peak>0.001 && peak<=1);
    std::printf("NATIVE_AUDIO_BENCH 64 looping sources + reverb: 5.33 seconds mixed in %.3f seconds, peak=%.4f\n",
        std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count(),peak);
    assert(guest.call(OP_alGetString,{AL_EXTENSIONS,0,0,0,0,0,0x4000,4096}));
    const char* extensions=static_cast<char*>(Guest::map(&guest,0x4000,4096,0));
    assert(std::strstr(extensions,"AL_EXT_FLOAT32") && !std::strstr(extensions,"callback"));
    BvnOpenALPacket invalid{}; invalid.abi=BOXEDWINE_OPENAL_ABI;
    invalid.args[0]=buffer; invalid.args[1]=AL_FORMAT_MONO16; invalid.args[2]=0xfffffff0; invalid.args[3]=2048; invalid.args[4]=48000;
    assert(bvnOpenALInvoke(1,OP_alBufferData,&invalid,Guest::map,&guest,1)!=0);
    assert(guest.call(OP_alGetError)==AL_INVALID_VALUE);
    invalid={}; invalid.abi=BOXEDWINE_OPENAL_ABI; invalid.args[0]=device;
    assert(bvnOpenALInvoke(2,OP_alcCloseDevice,&invalid,Guest::map,&guest,1)!=0); // foreign generation
    // Muting preserves guest-visible listener gain and changes actual output.
    guest.call(OP_alListenerf,{AL_GAIN,pack(0.5f)});
    BvnOpenALPacket muted{}; muted.abi=BOXEDWINE_OPENAL_ABI;
    muted.args[0]=AL_GAIN; muted.args[1]=0x1110;
    assert(bvnOpenALInvoke(1,OP_alGetListenerf,&muted,Guest::map,&guest,0)==0);
    assert(guest.at<float>(0x1110)==0.5f);
    assert(alcGetThreadContext()==nullptr);
    alcSetThreadContext(states.at(1).current);
    ALfloat nativeGain=-1; alGetListenerf(AL_GAIN,&nativeGain); assert(nativeGain==0);
    alcSetThreadContext(nullptr);
    guest.call(OP_alGetListenerf,{AL_GAIN,0x1110});
    alcSetThreadContext(states.at(1).current);
    alGetListenerf(AL_GAIN,&nativeGain); assert(nativeGain==0.5f);
    alcSetThreadContext(nullptr);
    guest.call(OP_alcMakeContextCurrent,{0});
    guest.call(OP_alcDestroyContext,{context});
    assert(guest.call(OP_alcCloseDevice,{device}));
    bvnOpenALRetire(1); bvnOpenALRetire(2);
    assert(states.empty());
    std::puts("PASS native OpenAL buffers, mixing, EFX, handles, pointer bounds and retirement");
}
