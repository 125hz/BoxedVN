/* BoxedVN - guest getrusage fairness regression tests. GPLv2. */

#include "boxedvn_test.h"

#include "getrusagefairness.h"

BOXEDVN_TEST(getrusage_fairness_ignores_normal_calls) {
    GetrusageFairness fairness;
    CHECK(!fairness.observe(1000).throttle);
    CHECK(!fairness.observe(10000).throttle);
    CHECK(!fairness.observe(20000).throttle);
}

BOXEDVN_TEST(getrusage_fairness_activates_for_a_tight_spin) {
    GetrusageFairness fairness;
    std::uint64_t now = 1000;
    CHECK(!fairness.observe(now).throttle);
    bool sawActivation = false;
    for (std::uint32_t i = 0;
         i < GetrusageFairness::kActivationCallCount; ++i) {
        now += 10;
        const GetrusageFairnessDecision decision = fairness.observe(now);
        sawActivation = sawActivation || decision.activated;
    }
    CHECK(sawActivation);
    CHECK(fairness.observe(now + 10).throttle);
}

BOXEDVN_TEST(getrusage_fairness_recovers_after_the_spin_stops) {
    GetrusageFairness fairness;
    std::uint64_t now = 1000;
    fairness.observe(now);
    for (std::uint32_t i = 0;
         i < GetrusageFairness::kActivationCallCount; ++i) {
        now += 10;
        fairness.observe(now);
    }
    now += 10;
    CHECK(fairness.observe(now).throttle);
    now += GetrusageFairness::kRapidCallWindowUs + 1;
    CHECK(!fairness.observe(now).throttle);
    CHECK(!fairness.observe(now + 10).activated);
}
