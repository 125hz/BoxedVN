/* BoxedVN - guest getrusage fairness regression tests. GPLv2. */

#include "boxedvn_test.h"

#include "getrusagefairness.h"

BOXEDVN_TEST(getrusage_fairness_ignores_normal_calls) {
    GetrusageFairness fairness;
    CHECK(!fairness.observe(1000).throttle);
    CHECK(!fairness.observe(10000).throttle);
    CHECK(!fairness.observe(20000).throttle);
}

// Drives a tight spin until the detector activates and returns the time of the
// activating call.
static std::uint64_t spinUntilActive(GetrusageFairness& fairness,
                                     std::uint64_t start) {
    std::uint64_t now = start;
    fairness.observe(now);
    for (std::uint32_t i = 0;
         i < GetrusageFairness::kActivationCallCount; ++i) {
        now += 10;
        if (fairness.observe(now).activated) {
            return now;
        }
    }
    return 0;
}

BOXEDVN_TEST(getrusage_fairness_activates_for_a_tight_spin) {
    GetrusageFairness fairness;
    const std::uint64_t activatedAt = spinUntilActive(fairness, 1000);
    CHECK(activatedAt != 0);
}

BOXEDVN_TEST(getrusage_fairness_throttle_is_rate_limited_in_wall_time) {
    // The first version slept 1 ms on every call once active, which is
    // self-sustaining - the sleep keeps the next call inside the rapid window -
    // and reduced the guest to about a thousand syscalls a second. On device
    // that showed as 0.4 presented frames/sec at 0.65 host cores busy: not
    // CPU-bound, sleeping. A spinning thread needs scheduling points, not a
    // duty cycle, so no more than one may be handed out per interval.
    GetrusageFairness fairness;
    const std::uint64_t activatedAt = spinUntilActive(fairness, 1000);
    CHECK(activatedAt != 0);

    // Keep spinning hard for one whole interval: exactly one throttle is due,
    // the one that came with activation.
    std::uint64_t now = activatedAt;
    std::uint32_t throttles = 0;
    while (now + 10 < activatedAt + GetrusageFairness::kThrottleIntervalUs) {
        now += 10;
        if (fairness.observe(now).throttle) {
            ++throttles;
        }
    }
    CHECK(throttles == 0);

    // Once the interval has elapsed, exactly one more.
    now = activatedAt + GetrusageFairness::kThrottleIntervalUs;
    CHECK(fairness.observe(now).throttle);
    CHECK(!fairness.observe(now + 10).throttle);

    // And the cost it implies stays a small fraction of wall time.
    CHECK(GetrusageFairness::kThrottleSleepUs * 4 <
          GetrusageFairness::kThrottleIntervalUs);
}

BOXEDVN_TEST(getrusage_fairness_recovers_after_the_spin_stops) {
    GetrusageFairness fairness;
    const std::uint64_t activatedAt = spinUntilActive(fairness, 1000);
    CHECK(activatedAt != 0);

    const std::uint64_t quiet =
        activatedAt + GetrusageFairness::kRapidCallWindowUs + 1;
    CHECK(!fairness.observe(quiet).throttle);
    CHECK(!fairness.observe(quiet + 10).activated);
}

BOXEDVN_TEST(getrusage_fairness_logs_only_the_first_activation) {
    // Build 68's device log carried 5,084 activation lines for one thread,
    // because the detector flaps in and out of the active state and each
    // transition wrote to the session file from a hot syscall path.
    GetrusageFairness fairness;
    std::uint64_t now = spinUntilActive(fairness, 1000);
    CHECK(now != 0);

    // Let it lapse and re-activate.
    now += GetrusageFairness::kRapidCallWindowUs + 1;
    fairness.observe(now);
    bool sawActivation = false;
    bool sawFirstActivation = false;
    for (std::uint32_t i = 0;
         i < GetrusageFairness::kActivationCallCount; ++i) {
        now += 10;
        const GetrusageFairnessDecision decision = fairness.observe(now);
        sawActivation = sawActivation || decision.activated;
        sawFirstActivation = sawFirstActivation || decision.firstActivation;
    }
    CHECK(sawActivation);
    CHECK(!sawFirstActivation);
}
