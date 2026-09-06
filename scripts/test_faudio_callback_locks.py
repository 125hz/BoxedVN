"""Exercise the shipping FAudio callback blocks with competing API calls.

The worker holds an application lock, then attempts a buffer submission lock.
The render thread enters the actual source callback block and takes that same
application lock. Unpatched FAudio times out; patched FAudio lets both finish.
Only the backend and client are fakes, not the callback lock transitions.
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile


def callback_block(source, callback):
    call = source.index(f"voice->src.callback->{callback}(")
    # Find the enclosing block containing the source-lock release. The first
    # start and loop callbacks each have one such directly enclosing block.
    start = source.rfind("{", 0, call)
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    block = source[start:end]
    assert "FAudio_PlatformUnlockMutex(voice->audio->sourceLock)" in block
    return block


HARNESS = r'''
#include <mutex>
#include <thread>
#include <future>
#include <chrono>
#include <cstdio>
using Mutex=std::recursive_timed_mutex;
#define LOG_MUTEX_LOCK(a,b)
#define LOG_MUTEX_UNLOCK(a,b)
void FAudio_PlatformLockMutex(Mutex* m) { m->lock(); }
void FAudio_PlatformUnlockMutex(Mutex* m) { m->unlock(); }
struct Callback { void (*OnBufferStart)(Callback*,void*); void (*OnLoopEnd)(Callback*,void*); };
struct Audio { Mutex* sourceLock; };
struct Voice { struct { Mutex* bufferLock; Callback* callback; } src; Mutex* sendLock; Audio* audio; };
struct Buffer { void* pContext; };
Mutex application;
std::promise<void> requested;
void callback(Callback*,void*) { requested.set_value(); std::lock_guard<Mutex> hold(application); }
int main() {
    Mutex source,send,bufferMutex;
    Audio audio{&source}; Callback cb{callback,callback};
    Voice instance{{&bufferMutex,&cb},&send,&audio}; Voice* voice=&instance;
    Buffer bufferInstance{nullptr}; Buffer* buffer=&bufferInstance;
    std::promise<void> ready;
    bool completed=false;
    std::thread worker([&] {
        std::lock_guard<Mutex> app(application);
        ready.set_value(); requested.get_future().wait();
        // These are the locks acquired by voice APIs. Timeouts allow an
        // unpatched negative control to fail without hanging CI.
        if (!source.try_lock_for(std::chrono::milliseconds(500))) return;
        if (!send.try_lock_for(std::chrono::milliseconds(500))) { source.unlock(); return; }
        if (bufferMutex.try_lock_for(std::chrono::milliseconds(500))) {
            completed=true; bufferMutex.unlock();
        }
        send.unlock(); source.unlock();
    });
    ready.get_future().wait();
    source.lock(); send.lock(); bufferMutex.lock();
    @CALLBACK@
    bufferMutex.unlock(); send.unlock(); source.unlock();
    worker.join();
    if (!completed) { std::puts("callback held a voice lock across client code"); return 1; }
    return 0;
}
'''


def main():
    source = Path(sys.argv[1]).read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="boxedvn-audio-locks-") as tmp:
        cpp = Path(tmp) / "callback.cpp"
        exe = Path(tmp) / ("callback-test.exe" if os.name == "nt" else "callback-test")
        for callback in ("OnBufferStart", "OnLoopEnd"):
            block = callback_block(source, callback)
            cpp.write_text(HARNESS.replace("@CALLBACK@", block), encoding="utf-8")
            compiler = os.environ.get("CXX", "c++")
            if Path(compiler).name.lower() in ("cl", "cl.exe"):
                command = [compiler, "/nologo", "/std:c++17", "/EHsc", str(cpp), f"/Fe:{exe}", f"/Fo:{tmp}/callback.obj"]
            else:
                command = [compiler, "-std=c++17", "-pthread", str(cpp), "-o", str(exe)]
            subprocess.run(command, check=True)
            result = subprocess.run([str(exe)], timeout=5)
            expected = 1 if "--expect-lock-cycle" in sys.argv[2:] else 0
            if result.returncode != expected:
                raise RuntimeError(f"{callback}: expected exit {expected}, got {result.returncode}")
            print(f"FAudio {callback}: {'negative control reproduced lock cycle' if expected else 'competing API call completed'}", flush=True)


if __name__ == "__main__":
    main()
