/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "boxedwine.h"

#ifdef __TEST

#include "../cpu/testCPU.h"
#include "../../x11/x11.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace {

constexpr U32 TEST_FUTEX_ADDRESS = TEST_HEAP_ADDRESS + 0x980;
constexpr U32 TEST_FUTEX_WAIT = 0;
constexpr U32 TEST_FUTEX_WAKE = 1;
constexpr U32 TEST_FUTEX_PRIVATE_FLAG = 128;
constexpr U32 TEST_FUTEX_VALUE = 0x12345678;

bool waitForFutexWorker(const std::atomic<bool>& done, U32 timeoutMs) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done.load(std::memory_order_acquire);
}

} // namespace

void testTerminatingThreadDoesNotEnterFutexWait() {
#ifdef BOXEDWINE_MULTI_THREADED
    TestContext& context = testContext();
    KThread* mainThread = context.thread;
    KThread* waiter = context.process->createThread();
    std::atomic<bool> done(false);
    std::atomic<U32> result(0);

    context.memory->writed(TEST_FUTEX_ADDRESS, TEST_FUTEX_VALUE);
    waiter->terminating = true;

    std::thread worker([waiter, &done, &result]() {
        KThread::setCurrentThread(waiter);
        result.store(waiter->futex(TEST_FUTEX_ADDRESS, TEST_FUTEX_WAIT | TEST_FUTEX_PRIVATE_FLAG, TEST_FUTEX_VALUE, 0, 0, 0, false), std::memory_order_release);
        done.store(true, std::memory_order_release);
        KThread::setCurrentThread(nullptr);
    });

    if (!waitForFutexWorker(done, 100)) {
        KThread::setCurrentThread(mainThread);
        mainThread->futex(TEST_FUTEX_ADDRESS, TEST_FUTEX_WAKE | TEST_FUTEX_PRIVATE_FLAG, 1, 0, 0, 0, false);
        waitForFutexWorker(done, 1000);
        testFail("terminating thread entered futex wait before returning EINTR");
    }

    worker.join();
    KThread::setCurrentThread(mainThread);

    const U32 expected = -K_EINTR;
    if (result.load(std::memory_order_acquire) != expected) {
        testFail("terminating futex wait expected %d (0x%X), got %d (0x%X)", (S32)expected, expected, (S32)result.load(std::memory_order_acquire), result.load(std::memory_order_acquire));
    }

    context.process->deleteThread(waiter);
#endif
}

void testX11RasterOperationsAndPlaneMask() {
    constexpr U32 source = 0x5a0ff00f;
    constexpr U32 destination = 0xa55aa55a;
    const U32 expected[] = {
        0,
        source & destination,
        source & ~destination,
        source,
        ~source & destination,
        destination,
        source ^ destination,
        source | destination,
        ~(source | destination),
        ~(source ^ destination),
        ~destination,
        source | ~destination,
        ~source,
        ~source | destination,
        ~(source & destination),
        0xffffffff,
    };

    for (S32 function = GXclear; function <= GXset; ++function) {
        const U32 actual = x11ApplyRasterOperation(
            source, destination, function, 0xffffffff);
        if (actual != expected[function]) {
            testFail("X11 raster function %d expected %08X, got %08X",
                     function, expected[function], actual);
        }
    }

    constexpr U32 planeMask = 0x00ff00ff;
    const U32 fullAnd = source & destination;
    const U32 maskedAnd = (destination & ~planeMask) |
        (fullAnd & planeMask);
    const U32 actualAnd = x11ApplyRasterOperation(
        source, destination, GXand, planeMask);
    if (actualAnd != maskedAnd) {
        testFail("X11 GXand plane mask expected %08X, got %08X",
                 maskedAnd, actualAnd);
    }
}

#endif
