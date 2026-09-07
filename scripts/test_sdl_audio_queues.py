"""Compile the pinned SDL default-device admission code with device-list stubs.

CoreAudio playback and interruption handling require an iOS device. This test
covers the admission failure seen in logs, including unchanged capture limits.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--source", type=Path, required=True)
args = parser.parse_args()
audio = (args.source / "src/audio/SDL_audio.c").read_text()
core = (args.source / "src/audio/coreaudio/SDL_coreaudio.m").read_text()
start = audio.index("    if ((iscapture) && (current_audio.impl.OnlyHasDefaultCaptureDevice))")
end = audio.index("    } else if (devname != NULL)", start)
admission = audio[start:end] + "    }\n"
assert core.count("impl->AllowsMultipleDefaultOutputDevices = SDL_TRUE;") == 1
init = core[core.index("static SDL_bool COREAUDIO_Init("):]
assert init.index("#else") < init.index("AllowsMultipleDefaultOutputDevices") < init.index("#endif")
assert "!open_playback_devices && !open_capture_devices && session_active" in core

code = r'''
#include <assert.h>
#include <string.h>
#include <stddef.h>
#define SDL_arraysize(a) (sizeof(a)/sizeof(a[0]))
#define SDL_strcmp strcmp
#define DEFAULT_INPUT_DEVNAME "input"
#define DEFAULT_OUTPUT_DEVNAME "output"
int locked;
void SDL_LockMutex(int unused) { assert(!locked); locked=1; }
void SDL_UnlockMutex(int unused) { assert(locked); locked=0; }
int SDL_SetError(const char* unused) { return -1; }
struct { struct { int OnlyHasDefaultCaptureDevice,OnlyHasDefaultOutputDevice,
                     AllowsMultipleDefaultOutputDevices; } impl; int detectionLock; } current_audio;
struct Device { int iscapture; } output={0},capture={1};
struct Device* open_devices[4];
int admit(const char* devname,int iscapture) { int i;
'''
code += admission + 'return 1; }\n'
code += r'''
int main(void) {
 current_audio.impl.OnlyHasDefaultCaptureDevice=1;
 current_audio.impl.OnlyHasDefaultOutputDevice=1;
 assert(admit(NULL,0));
 open_devices[0]=&output;
 assert(!admit(NULL,0)); /* reproduce unpatched default-output failure */
 current_audio.impl.AllowsMultipleDefaultOutputDevices=1;
 assert(admit(NULL,0)); assert(admit("output",0));
 open_devices[1]=&output;assert(admit(NULL,0));
 assert(!admit("unknown",0));
 assert(admit(NULL,1));open_devices[2]=&capture;
 assert(!admit(NULL,1)); /* capture stays exclusive */
 assert(!admit("unknown",1));
 open_devices[0]=NULL;assert(admit(NULL,0));
 assert(open_devices[1]==&output && open_devices[2]==&capture);
 current_audio.impl.AllowsMultipleDefaultOutputDevices=0;
 assert(!admit(NULL,0)); /* other backends retain their policy */
 assert(!locked);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "admission.c"
    executable = Path(tmp) / "admission"
    source.write_text(code)
    subprocess.run(["cc", str(source), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
print("SDL playback admission, capture exclusivity and lock balance passed")
