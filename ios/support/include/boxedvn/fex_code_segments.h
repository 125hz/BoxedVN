#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace boxedvn {

// Entries are immutable after publication. Signal handlers can look up RX/RW
// pairs while the allocator publishes another segment, without taking a lock.
class FexCodeSegments final {
    static_assert(std::atomic<std::size_t>::is_always_lock_free);
public:
    static constexpr std::size_t maximum = 8;
    struct Segment {
        std::uintptr_t rx = 0;
        std::uintptr_t rw = 0;
        std::size_t size = 0;
    };

    bool append(Segment segment) noexcept {
        const auto count = size();
        if (count == maximum || !segment.rx || !segment.rw || !segment.size ||
            segment.rx + segment.size < segment.rx ||
            segment.rw + segment.size < segment.rw) return false;
        for (std::size_t i = 0; i < count; ++i) {
            const auto& old = entries_[i];
            if (segment.rx < old.rx + old.size && old.rx < segment.rx + segment.size)
                return false;
        }
        entries_[count] = segment;
        count_.store(count + 1, std::memory_order_release);
        return true;
    }

    std::size_t size() const noexcept { return count_.load(std::memory_order_acquire); }
    const Segment& at(std::size_t index) const noexcept { return entries_[index]; }
    std::size_t find(std::uintptr_t address) const noexcept {
        const auto count = size();
        for (std::size_t i = 0; i < count; ++i) {
            if (address >= entries_[i].rx && address - entries_[i].rx < entries_[i].size)
                return i;
        }
        return maximum;
    }
    std::uintptr_t writable(std::uintptr_t address) const noexcept {
        const auto index = find(address);
        return index == maximum ? 0 : entries_[index].rw + (address - entries_[index].rx);
    }

private:
    std::array<Segment, maximum> entries_ {};
    std::atomic<std::size_t> count_ {0};
};

} // namespace boxedvn
