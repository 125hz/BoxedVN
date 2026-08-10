/*
 * BoxedVN - how long the guest spends inside the host's present path.
 * GPLv2; see license.txt.
 */

#ifndef __BVN_HOST_PRESENT_H__
#define __BVN_HOST_PRESENT_H__

#include <atomic>
#include <cstdint>

// Build 72's automatic thread snapshot answered the question that four builds
// of guessing had not: during a Grisaia stall every other guest thread is
// parked on KUnixSocketObject::lockCond - the X11 socket - behind one thread
// that is inside host Vulkan call 170, vkQueuePresentKHR, in a Mach trap,
// burning 73-143 microseconds of CPU per *second*.
//
// That identifies the blocked call but not how long each one blocks, and the
// difference matters: a present that takes 30 ms is a slow GPU, one that takes
// 2.5 seconds is a drawable that the compositor is not handing back. These
// counters make the next log say which, without another round trip.
//
// MoltenVK acquires the CAMetalDrawable lazily, at present time rather than at
// vkAcquireNextImageKHR, so both are timed - if the wait ever moves to acquire
// the numbers will show that too.
namespace bvnHostPresent {
extern std::atomic<std::uint64_t> presentCalls;
extern std::atomic<std::uint64_t> presentMicroseconds;
extern std::atomic<std::uint64_t> presentWorstMicroseconds;
extern std::atomic<std::uint64_t> acquireCalls;
extern std::atomic<std::uint64_t> acquireMicroseconds;
extern std::atomic<std::uint64_t> acquireWorstMicroseconds;

void recordPresent(std::uint64_t microseconds);
void recordAcquire(std::uint64_t microseconds);
}

#endif
