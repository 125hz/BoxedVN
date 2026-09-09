// Native playback for the shared BoxedWine OpenAL facade. No guest callbacks.
#define AL_LIBTYPE_STATIC
#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx.h>
#include "boxedwine_openal_bridge.h"
#include <cstring>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cmath>
#if defined(__APPLE__)
extern "C" bool bvnOpenALPrepareSession();
#endif

namespace {
template<class T> uint64_t pack(T v) { uint64_t r=0; static_assert(sizeof(v)<=8); std::memcpy(&r,&v,sizeof(v)); return r; }
template<class T> T unpack(uint64_t v) { T r; std::memcpy(&r,&v,sizeof(r)); return r; }
struct InvalidArgument {};
struct State {
    uint32_t next=1;
    ALenum error=AL_NO_ERROR;
    ALCenum alcError=ALC_NO_ERROR;
    bool outputEnabled=true;
    ALCcontext* current=nullptr;
    std::unordered_map<uint32_t,ALCdevice*> devices;
    std::unordered_map<uint32_t,ALCcontext*> contexts;
    std::unordered_map<ALCcontext*,ALfloat> gains;
    void listenerGain(ALfloat gain) {
        // Validate using OpenAL itself; do not hide invalid negative/NaN gain.
        if (!(gain>=0) || !std::isfinite(gain)) { alListenerf(AL_GAIN,gain); return; }
        gains[current]=gain;
        alListenerf(AL_GAIN,outputEnabled ? gain : 0.0f);
    }
    ALfloat listenerGain() { auto it=gains.find(current); return it==gains.end() ? 1.0f : it->second; }
    ALCdevice* device(uint64_t token) {
        if (!token) return nullptr;
        auto it=devices.find(uint32_t(token));
        if (token>UINT32_MAX || it==devices.end()) throw InvalidArgument{};
        return it->second;
    }
    ALCcontext* context(uint64_t token) {
        if (!token) return nullptr;
        auto it=contexts.find(uint32_t(token));
        if (token>UINT32_MAX || it==contexts.end()) throw InvalidArgument{};
        return it->second;
    }
    template<class T> uint64_t add(std::unordered_map<uint32_t,T*>& table,T* value) {
        if (!value) return 0;
        for (auto& entry:table) if(entry.second==value) return entry.first;
        if (!next) throw InvalidArgument{};
        auto id=next++; table.emplace(id,value); return id;
    }
    uint64_t remember(ALCdevice* p) { return add(devices,p); }
    uint64_t remember(ALCcontext* p) { return add(contexts,p); }
    void erase(uint64_t token,bool isDevice) {
        if(isDevice) devices.erase(uint32_t(token));
        else {
            auto it=contexts.find(uint32_t(token));
            if(it!=contexts.end()) gains.erase(it->second);
            if(it!=contexts.end() && current==it->second) current=nullptr;
            contexts.erase(uint32_t(token));
        }
    }
    ~State() {
        alcSetThreadContext(nullptr);
        for(auto& entry:contexts) alcDestroyContext(entry.second);
        for(auto& entry:devices) alcCloseDevice(entry.second);
    }
};
std::mutex bridgeMutex;
std::unordered_map<uint64_t,State> states;
// Advertise only APIs and format/state extensions transported by this facade.
constexpr const char* alExtensions="AL_EXT_FLOAT32 AL_EXT_DOUBLE AL_EXT_MCFORMATS AL_EXT_source_distance_model AL_SOFT_loop_points";
constexpr const char* alcExtensions="ALC_EXT_EFX ALC_ENUMERATION_EXT ALC_ENUMERATE_ALL_EXT";
bool extension(const char* list,const std::string& name) {
    if(name.empty() || name.find(' ')!=std::string::npos) return false;
    std::string haystack=" "+std::string(list)+" ";
    auto needle=" "+name+" ";
    auto lower=[](unsigned char c){return char(std::tolower(c));};
    std::transform(haystack.begin(),haystack.end(),haystack.begin(),lower);
    std::transform(needle.begin(),needle.end(),needle.begin(),lower);
    return haystack.find(needle)!=std::string::npos;
}
ALCdevice* openPlaybackDevice(const char* name) {
#if defined(__APPLE__)
    if(!bvnOpenALPrepareSession()) return nullptr;
#endif
    // Preserve the Windows router's legacy backend aliases on the native
    // CoreAudio backend. Arbitrary unknown device names still fail normally.
    if(name && extension("DirectSound3D DirectSound MMSYSTEM",name)) name=nullptr;
    if(name && (!std::strcmp(name,"Generic Software") || !std::strcmp(name,"Generic Hardware"))) name=nullptr;
    return alcOpenDevice(name);
}
size_t count(uint64_t n,size_t element) {
    if(n>INT32_MAX || n>SIZE_MAX/element || n*element>256*1024*1024) throw InvalidArgument{};
    return size_t(n)*element;
}
size_t effectVectorLength(ALuint effect,unsigned param) {
    ALint type=AL_EFFECT_NULL;
    alGetEffecti(effect,AL_EFFECT_TYPE,&type);
    return type==AL_EFFECT_EAXREVERB &&
        (param==AL_EAXREVERB_REFLECTIONS_PAN || param==AL_EAXREVERB_LATE_REVERB_PAN) ? 3 : 1;
}
size_t vectorLength(unsigned param) {
    switch(param) {
    case AL_ORIENTATION: return 6;
    case AL_POSITION: case AL_VELOCITY: case AL_DIRECTION: case AL_AUXILIARY_SEND_FILTER: return 3;
    case AL_LOOP_POINTS_SOFT: return 2;
    default: return 1;
    }
}
}

extern "C" int bvnOpenALInvoke(uint64_t generation,uint32_t operation,BvnOpenALPacket* packet,BvnOpenALMap map,void* opaque,int outputEnabled) {
    if(!packet || packet->abi!=BOXEDWINE_OPENAL_ABI || !map) return -1;
    std::scoped_lock lock(bridgeMutex);
    // A guest can migrate between host threads. Do not leave a native TLS
    // reference on an old worker after its guest context has been retired.
    struct ClearThreadContext { ~ClearThreadContext() { alcSetThreadContext(nullptr); } } clearThreadContext;
    auto& state=states[generation];
    auto& p=*packet;
    p.result=0;
    auto pointer=[&](uint64_t address,size_t bytes,bool writable)->void* {
        if(!bytes) return nullptr;
        void* result=address ? map(opaque,address,bytes,writable) : nullptr;
        if(!result) throw InvalidArgument{};
        return result;
    };
    auto readString=[&](uint64_t address) {
        std::string result;
        if(!address) return result;
        for(unsigned i=0;i<1024;++i) {
            char value=*static_cast<const char*>(pointer(address+i,1,false));
            if(!value) return result;
            result+=value;
        }
        throw InvalidArgument{};
    };
    auto readAttributes=[&](uint64_t address) {
        std::vector<ALCint> result;
        if(!address) return result;
        for(unsigned i=0;i<128;++i) {
            ALCint value; std::memcpy(&value,pointer(address+i*4,4,false),4);
            result.push_back(value);
            if(!(i&1) && !value) return result;
        }
        throw InvalidArgument{};
    };
    auto writeString=[&](const char* text,bool multiple) {
        if(!text) return;
        size_t size=std::strlen(text)+1;
        if(multiple) {
            while(text[size]) size+=std::strlen(text+size)+1;
            ++size;
        }
        if(size>p.args[7]) throw InvalidArgument{};
        std::memcpy(pointer(p.args[6],size,true),text,size);
        p.result=1;
    };
    try {
        // OpenAL's ordinary current context is process-local in the guest.
        // Native TLS keeps helper processes from inheriting another guest's AL.
        alcSetThreadContext(state.current);
        if(state.outputEnabled!=bool(outputEnabled)) {
            state.outputEnabled=bool(outputEnabled);
            for(auto& entry:state.contexts) {
                alcSetThreadContext(entry.second);
                auto it=state.gains.find(entry.second);
                alListenerf(AL_GAIN,outputEnabled ? (it==state.gains.end() ? 1.0f : it->second) : 0.0f);
            }
            alcSetThreadContext(state.current);
        }
        switch(operation) {
#include "openal_dispatch.inc"
        default: return -1;
        }
    } catch(const InvalidArgument&) {
        state.error=AL_INVALID_VALUE;
        state.alcError=ALC_INVALID_VALUE;
        return -1;
    } catch(const std::bad_alloc&) {
        state.error=AL_OUT_OF_MEMORY;
        state.alcError=ALC_OUT_OF_MEMORY;
        return -1;
    }
    return 0;
}

extern "C" void bvnOpenALRetire(uint64_t generation) {
    std::scoped_lock lock(bridgeMutex);
    states.erase(generation);
}
