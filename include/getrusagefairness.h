/*
 * BoxedVN - cooperative fairness for pathological guest spin loops.
 * GPLv2; see license.txt.
 */

#ifndef __GETRUSAGE_FAIRNESS_H__
#define __GETRUSAGE_FAIRNESS_H__

#include <atomic>
#include <cstdint>

struct GetrusageFairnessDecision {
    bool throttle = false;
    bool activated = false;
    // True only the first time a given detector activates. Build 68's device
    // log carried 5,084 activation lines for a single thread, because the
    // detector flaps in and out of the active state; each one was a file write
    // on a hot syscall path.
    bool firstActivation = false;
};

// Guest spin loops defeat cooperative scheduling on this platform in two
// observed shapes.
//
// Wine worker pools use getrusage() as a tight spin-loop clock. Boxedwine
// services it entirely in-process, so nothing ever yields and several guest
// threads occupy every core.
//
// DXVK's worker threads spin on sched_yield() plus clock_gettime(). That looks
// like it should be safe, but Boxedwine implements sched_yield() as
// std::this_thread::yield(), and on Darwin that is only a hint: with other
// runnable threads at the same priority it commonly returns without
// descheduling. Eight spinning threads then starve the very command-stream
// thread they are waiting on, which is a livelock rather than slow progress.
//
// The same detector guards both: notice a pathological repeat rate, then make
// the call a real scheduling point.
//
// THE THROTTLE IS RATE-LIMITED IN WALL TIME, not applied per call, and that is
// the whole design. The first version slept 1 ms on *every* call once active,
// which is self-sustaining - a 1 ms sleep keeps the next call inside the 5 ms
// "rapid" window, so the thread stays throttled - and it converts a spinning
// thread into one that makes at most a thousand syscalls a second. The Fruit
// of Grisaia measured 0.4 presented frames per second at 0.65 host cores busy:
// not CPU-bound, sleeping. A scheduling point costs microseconds; spending a
// millisecond to buy one is what starved the guest.
//
// Bounding it by wall time instead caps the cost at roughly
// kThrottleSleepUs / kThrottleIntervalUs of the thread's time while still
// delivering a thousand real scheduling points a second, which is a thousand
// more than the livelock had.
//
// This detector is deliberately independent from sleeping and logging so its
// state transitions can be unit-tested on the host.
class GetrusageFairness {
public:
    GetrusageFairnessDecision observe(std::uint64_t nowMicroseconds) {
        GetrusageFairnessDecision decision;
        const bool rapid = lastCallMicroseconds != 0 &&
            nowMicroseconds >= lastCallMicroseconds &&
            nowMicroseconds - lastCallMicroseconds <= kRapidCallWindowUs;
        lastCallMicroseconds = nowMicroseconds;

        if (!rapid) {
            rapidCallCount = 0;
            active = false;
            return decision;
        }

        if (rapidCallCount < kActivationCallCount) {
            ++rapidCallCount;
        }
        if (!active && rapidCallCount >= kActivationCallCount) {
            active = true;
            decision.activated = true;
            decision.firstActivation = !hasEverActivated;
            hasEverActivated = true;
        }
        if (!active) {
            return decision;
        }

        // Rate limit in wall time. Throttling every call is what made the
        // mitigation more expensive than the problem.
        if (lastThrottleMicroseconds != 0 &&
            nowMicroseconds - lastThrottleMicroseconds < kThrottleIntervalUs) {
            return decision;
        }
        lastThrottleMicroseconds = nowMicroseconds;
        decision.throttle = true;
        return decision;
    }

    void reset() {
        lastCallMicroseconds = 0;
        lastThrottleMicroseconds = 0;
        rapidCallCount = 0;
        active = false;
        hasEverActivated = false;
    }

    static constexpr std::uint32_t kActivationCallCount = 32;
    static constexpr std::uint64_t kRapidCallWindowUs = 5000;
    // At most one scheduling point per millisecond of wall time...
    static constexpr std::uint64_t kThrottleIntervalUs = 1000;
    // ...and each one costs this much. Darwin will not sleep for less than
    // about 50 us, so this is close to the smallest real scheduling point
    // available, and it bounds the mitigation at roughly a tenth of the
    // thread's time.
    static constexpr std::uint64_t kThrottleSleepUs = 100;

private:
    std::uint64_t lastCallMicroseconds = 0;
    std::uint64_t lastThrottleMicroseconds = 0;
    std::uint32_t rapidCallCount = 0;
    bool active = false;
    bool hasEverActivated = false;
};

// How much time the fairness throttle has actually taken from the guest, so it
// can be reported next to the frame rate rather than inferred. Without this,
// "the mitigation is now the bottleneck" is a hypothesis; with it, it is a
// number in the same log line as the frames per second.
namespace bvnFairness {
extern std::atomic<std::uint64_t> throttleCount;
extern std::atomic<std::uint64_t> throttleMicroseconds;
}

#endif
