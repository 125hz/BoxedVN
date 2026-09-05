/* BoxedVN main-thread session entry. GPLv2; see license.txt. */
#pragma once

#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>

// A long-lived SDL loop must not occupy the serial main dispatch queue.
// CFRunLoop suppresses reentrant main-queue delivery from a GCD callback,
// even when SDL pumps the run loop. Enter from a run-loop block instead.
static inline void BVNEnqueueRunLoopSession(dispatch_block_t session) {
    CFRunLoopRef loop = CFRunLoopGetMain();
    CFRunLoopPerformBlock(loop, kCFRunLoopCommonModes, session);
    CFRunLoopWakeUp(loop);
}
