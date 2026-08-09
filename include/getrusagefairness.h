/*
 * BoxedVN - cooperative fairness for pathological guest getrusage polling.
 * GPLv2; see license.txt.
 */

#ifndef __GETRUSAGE_FAIRNESS_H__
#define __GETRUSAGE_FAIRNESS_H__

#include <cstdint>

struct GetrusageFairnessDecision {
    bool throttle = false;
    bool activated = false;
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
        }
        decision.throttle = active;
        return decision;
    }

    void reset() {
        lastCallMicroseconds = 0;
        rapidCallCount = 0;
        active = false;
    }

    static constexpr std::uint32_t kActivationCallCount = 32;
    static constexpr std::uint64_t kRapidCallWindowUs = 5000;

private:
    std::uint64_t lastCallMicroseconds = 0;
    std::uint32_t rapidCallCount = 0;
    bool active = false;
};

#endif
