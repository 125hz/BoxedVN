"""Generate a bounded OpenAL 1.1 / EFX bridge from the pinned public headers.

The PE facade contains no mixer. Only scalar bit patterns, guest pointers and
opaque integer tokens cross the private BoxedWine ABI; native function pointers
and native ALC pointers never escape into a Windows program.
"""
import argparse
from pathlib import Path
import re

parser = argparse.ArgumentParser()
parser.add_argument("--source", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
api = []
for header in ("al.h", "alc.h", "efx.h"):
    text = (args.source / "include/AL" / header).read_text()
    for ret, name, params in re.findall(r"^ALC?_API\s+(.+?)\s+ALC?_APIENTRY\s+(\w+)\(([^;]+?)\)\s+ALC?_API_NOEXCEPT;", text, re.M):
        if name.startswith("alcCapture"):
            continue  # Playback only; no microphone capture implementation.
        parameters = []
        for p in ([] if params.strip() == "void" else params.split(",")):
            match = re.fullmatch(r"\s*(.+?)(\w+)\s*", p)
            if not match:
                raise ValueError(p)
            parameters.append((match[1].strip(), match[2]))
        api.append((name, ret.strip(), parameters))
api.sort()
assert len({a[0] for a in api}) == len(api)
assert 100 <= len(api) <= 160, len(api)
out = args.output
out.mkdir(parents=True, exist_ok=True)
guest = ['''// Generated from pinned OpenAL headers. See generate-openal-bridge.py.
#define AL_LIBTYPE_STATIC
#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/efx.h>
#include <windows.h>
#include <string.h>
#include "boxedwine_openal_bridge.h"
template<class T> static uint64_t pack(T v) { uint64_t r=0; static_assert(sizeof(v)<=8); memcpy(&r,&v,sizeof(v)); return r; }
template<class T> static T unpack(uint64_t v) { T r; memcpy(&r,&v,sizeof(r)); return r; }
static void invoke(unsigned op,BvnOpenALPacket& p) {
    uintptr_t result=BOXEDWINE_OPENAL_HOSTCALL;
    // FEX's WoW64 context uses the Linux64 register ABI in both code segments.
    // Its Linux decoder accepts INT80 only in the 32-bit segment.
#if defined(__x86_64__)
    __asm__ volatile("syscall" : "+a"(result) : "D"(uintptr_t(op)), "S"(uintptr_t(&p)) : "rcx", "r11", "memory", "cc");
#else
    __asm__ volatile("int $0x80" : "+a"(result) : "D"(uintptr_t(op)), "S"(uintptr_t(&p)) : "memory", "cc");
#endif
    if (result) p.result=0;
}
static thread_local char strings[32][4096];
''']
native = []
export = ["LIBRARY openal32", "EXPORTS"]

def scalar(typ, index):
    return f"unpack<{typ}>(p.args[{index}])"

def pointer_size(name, params, index):
    typ, _ = params[index]
    base = typ.replace("const", "").replace("*", "").strip()
    size = "1" if base in ("ALvoid", "void") else f"sizeof({base})"
    names = [p[1] for p in params]
    if name == "alBufferData":
        return "count(p.args[3],1)"
    if name == "alcGetIntegerv":
        return "count(p.args[2],sizeof(ALCint))"
    if "n" in names:
        return f"count(p.args[{names.index('n')}],{size})"
    if "nb" in names:
        return f"count(p.args[{names.index('nb')}],{size})"
    if name.endswith(("fv", "iv")) and "param" in names:
        pi = names.index("param")
        if 'Effect' in name and 'Slot' not in name:
            return f"effectVectorLength(ALuint(p.args[0]),unsigned(p.args[{pi}]))*{size}"
        return f"vectorLength(unsigned(p.args[{pi}]))*{size}"
    return size

for opcode, (name, ret, params) in enumerate(api, 1):
    export.append("    " + name)
    declaration = ", ".join(t+" "+n for t,n in params) or "void"
    # EFX functions are also exported: callers normally obtain these by name.
    # The .def supplies exports. Adding dllexport after a prototype has already
    # been used by GetProcAddress is rejected by the LLVM-MinGW compiler.
    guest.append(f'extern "C" {ret} AL_APIENTRY {name}({declaration}) noexcept {{')
    if name.endswith("GetProcAddress"):
        pname = params[-1][1]
        for _,var in params[:-1]:
            guest.append(f"    (void){var};")
        guest.append(f"    if (!{pname}) return nullptr;")
        for fn, _, _ in api:
            guest.append(f'    if (!strcmp({pname},"{fn}")) return reinterpret_cast<void*>(&{fn});')
        guest.append("    return nullptr;\n}")
        continue
    guest.append("    BvnOpenALPacket p{}; p.abi=BOXEDWINE_OPENAL_ABI;")
    for i,(typ,var) in enumerate(params):
        guest.append(f"    p.args[{i}]=pack({var});")
    if name.endswith("GetString"):
        pi = next(i for i,p in enumerate(params) if p[1] == "param")
        guest.append(f"    char* buffer=strings[unsigned(p.args[{pi}])&31]; buffer[0]=0;")
        guest.append("    p.args[6]=pack(buffer); p.args[7]=4096;")
    guest.append(f"    invoke({opcode},p);")
    if ret != "void":
        if name.endswith("GetString"):
            guest.append("    return p.result ? buffer : nullptr;")
        else:
            guest.append(f"    return unpack<{ret}>(p.result);")
    guest.append("}")

    native.append(f"case {opcode}: {{ // {name}")
    callargs = []
    for i,(typ,var) in enumerate(params):
        if typ in ("ALCdevice *", "ALCdevice*", "ALCcontext *", "ALCcontext*"):
            kind = "device" if "device" in typ else "context"
            callargs.append(f"state.{kind}(p.args[{i}])")
        elif "*" in typ:
            if "char" in typ:
                native.append(f"    auto s{i}=readString(p.args[{i}]);")
                callargs.append(f"p.args[{i}] ? s{i}.c_str() : nullptr")
            elif name == "alcCreateContext":
                native.append(f"    auto attributes=readAttributes(p.args[{i}]);")
                callargs.append("attributes.empty() ? nullptr : attributes.data()")
            else:
                size = pointer_size(name,params,i)
                callargs.append(f"static_cast<{typ}>(pointer(p.args[{i}],{size},{str('const' not in typ).lower()}))")
        else:
            callargs.append(scalar(typ,i))
    call = f"{name}({', '.join(callargs)})"
    if name == "alcOpenDevice":
        call = f"openPlaybackDevice({', '.join(callargs)})"
    if name == "alcMakeContextCurrent":
        native.append("    auto context=state.context(p.args[0]); p.result=alcSetThreadContext(context);")
        native.append("    if(p.result) state.current=context;")
        native.append("    if(p.result && context && !state.outputEnabled) alListenerf(AL_GAIN,0.0f);")
    elif name == "alcGetCurrentContext":
        native.append("    p.result=state.remember(state.current);")
    elif name == "alGetError":
        native.append("    p.result=state.error ? state.error : alGetError(); state.error=AL_NO_ERROR;")
    elif name == "alcGetError":
        native.append(f"    p.result=state.alcError ? state.alcError : {call}; state.alcError=ALC_NO_ERROR;")
    elif name in ("alListenerf", "alListenerfv"):
        gain = callargs[1] if name == "alListenerf" else f"*({callargs[1]})"
        native.append(f"    if(p.args[0]==AL_GAIN) state.listenerGain({gain}); else {call};")
    elif name in ("alGetListenerf", "alGetListenerfv"):
        native.append(f"    if(p.args[0]==AL_GAIN) *({callargs[1]})=state.listenerGain(); else {call};")
    elif name in ("alIsExtensionPresent", "alcIsExtensionPresent"):
        ext = "alcExtensions" if name.startswith("alc") else "alExtensions"
        native.append(f"    p.result=extension({ext},s{len(params)-1});")
    elif name.endswith("GetString"):
        pi = next(i for i,p in enumerate(params) if p[1] == "param")
        ext = "alcExtensions" if name.startswith("alc") else "alExtensions"
        param = "ALC_EXTENSIONS" if name.startswith("alc") else "AL_EXTENSIONS"
        native.append(f"    auto text=p.args[{pi}]=={param} ? {ext} : {call};")
        # NULL-device enumeration is a double-NUL list; ordinary strings are single-NUL.
        multi = "!p.args[0] && (p.args[1]==ALC_DEVICE_SPECIFIER || p.args[1]==ALC_ALL_DEVICES_SPECIFIER)" if name.startswith("alc") else "false"
        native.append(f"    writeString(text,({multi}));")
    elif "ALCdevice" in ret or "ALCcontext" in ret:
        native.append(f"    p.result=state.remember({call});")
        if name == "alcOpenDevice":
            native.append('    if(p.result) std::fprintf(stderr,"BOXEDWINE_OPENAL_NATIVE device=open mixer=native\\n");')
    elif name in ("alcCloseDevice", "alcDestroyContext"):
        if name == "alcDestroyContext":
            native.append("    if(state.context(p.args[0])==state.current) alcSetThreadContext(nullptr);")
        native.append(f"    {'p.result=' if ret!='void' else ''}{call};")
        native.append(f"    {'if (p.result) ' if ret!='void' else ''}state.erase(p.args[0],{str(name=='alcCloseDevice').lower()});")
    elif ret == "void":
        native.append(f"    {call};")
    else:
        native.append(f"    p.result=pack({call});")
    native.append("    break;\n}")

(out/"openal_guest.cpp").write_text("\n".join(guest)+"\n")
(out/"openal_dispatch.inc").write_text("\n".join(native)+"\n")
(out/"openal32.def").write_text("\n".join(export)+"\n")
(out/"openal_ops.txt").write_text("\n".join(f"{i} {a[0]}" for i,a in enumerate(api,1))+"\n")
(out/"openal_ops.h").write_text("#pragma once\nenum {\n"+"\n".join(f"OP_{a[0]}={i}," for i,a in enumerate(api,1))+"\n};\n")
print(f"Generated {len(api)} OpenAL playback/EFX entry points")
