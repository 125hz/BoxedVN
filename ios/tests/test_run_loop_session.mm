// Exercise the real CoreFoundation/main-dispatch interaction on macOS.
#include "BVNRunLoopSession.h"
#include <cstdio>
#include <pthread.h>

int main() {
    __block bool done = false;
    __block bool deliveredWhileRunning = false;
    __block bool mainThread = false;
    __block bool returnedFromLaunch = false;
    __block bool enteredAfterLaunch = false;
    dispatch_async(dispatch_get_main_queue(), ^{
        BVNEnqueueRunLoopSession(^{
            enteredAfterLaunch = returnedFromLaunch;
            mainThread = pthread_main_np();
            __block bool sessionRunning = true;
            dispatch_async(dispatch_get_main_queue(), ^{
                deliveredWhileRunning = sessionRunning;
            });
            const auto deadline = CFAbsoluteTimeGetCurrent() + 1.0;
            while (!deliveredWhileRunning && CFAbsoluteTimeGetCurrent() < deadline) {
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, true);
            }
            sessionRunning = false;
            done = true;
        });
        returnedFromLaunch = true;
    });
    const auto deadline = CFAbsoluteTimeGetCurrent() + 3.0;
    while (!done && CFAbsoluteTimeGetCurrent() < deadline) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
    }
    if (!done || !mainThread || !enteredAfterLaunch || !deliveredWhileRunning) {
        std::fprintf(stderr, "session entry failed: done=%d main=%d deferred=%d main_queue=%d\n",
                     done, mainThread, enteredAfterLaunch, deliveredWhileRunning);
        return 1;
    }
    std::puts("PASS: SDL-style session pumps main-queue work before session exit");
}
