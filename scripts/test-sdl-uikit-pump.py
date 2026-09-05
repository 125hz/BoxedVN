#!/usr/bin/env python3
"""Exercise the patched SDL pump with always-ready, idle and slow sources."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--source", type=Path, required=True)
args = parser.parse_args()
source = args.source.read_text()
start = source.index("void UIKit_PumpEvents(_THIS)")
end = source.index("\n#ifdef ENABLE_GCKEYBOARD", start)
pump = source[start:end]
compiler = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc")
if not compiler:
    raise SystemExit("A host C compiler is required for the SDL pump check")
harness = r'''
#include <assert.h>
#include <stdint.h>
#define _THIS void
#define TRUE 1
typedef double CFTimeInterval;
typedef int SInt32;
typedef uint64_t Uint64;
typedef int CFStringRef;
enum { kCFRunLoopDefaultMode, UITrackingRunLoopMode, kCFRunLoopRunHandledSource = 4 };
static int UIKit_EventPumpEnabled = 1;
static unsigned calls[2];
static Uint64 ticks, step;
static int ready = 1;
static Uint64 SDL_GetTicks64(void) { return ticks; }
static SInt32 CFRunLoopRunInMode(int mode, double seconds, int once) {
    assert(seconds > 0 && once);
    ++calls[mode];
    ticks += step;
    return ready ? kCFRunLoopRunHandledSource : 0;
}
'''
harness += pump
harness += r'''
int main(void) {
    /* A source that never drains and a clock that has not ticked. */
    UIKit_PumpEvents();
    assert(calls[0] == 32 && calls[1] == 32);
    calls[0] = calls[1] = 0; step = 1;
    UIKit_PumpEvents();
    assert(calls[0] == 2 && calls[1] == 2);
    /* One slow callback must not prevent the other mode being serviced. */
    calls[0] = calls[1] = 0; step = 40;
    UIKit_PumpEvents();
    assert(calls[0] == 1 && calls[1] == 1);
    calls[0] = calls[1] = 0; ready = 0; step = 0;
    UIKit_PumpEvents();
    assert(calls[0] == 1 && calls[1] == 1);
    calls[0] = calls[1] = 0; UIKit_EventPumpEnabled = 0;
    UIKit_PumpEvents();
    assert(calls[0] == 0 && calls[1] == 0);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="boxedvn-sdl-pump-") as directory:
    root = Path(directory)
    c = root / "pump.c"
    exe = root / ("pump.exe" if os.name == "nt" else "pump")
    c.write_text(harness)
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)
print("SDL UIKit pump: continuous, timed, slow, idle and disabled checks passed")
