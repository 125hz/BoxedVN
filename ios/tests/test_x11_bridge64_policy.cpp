/* BoxedVN - admission policy for the x86-64 guest X11 bridge call. GPLv2. */

#include "boxedvn_test.h"

#include "x11_bridge64_policy.h"

#include <cstring>
#include <map>
#include <set>
#include <string>

namespace {

// A page table with a few readable and writable pages.
class FakeProbe : public boxedvn::X11Bridge64PageProbe {
public:
    std::set<uint64_t> readable;
    std::set<uint64_t> writable;
    bool accessible(uint64_t address, bool write) const override {
        const uint64_t page = address & ~0xFFFULL;
        if (!readable.count(page)) {
            return false;
        }
        return !write || writable.count(page);
    }
    void map(uint64_t page, bool write) {
        readable.insert(page);
        if (write) {
            writable.insert(page);
        }
    }
};

struct FakeMemory {
    std::map<uint64_t, uint64_t> words;
    void read(uint64_t address, uint64_t* out, uint64_t count) const {
        for (uint64_t i = 0; i < count; i++) {
            auto it = words.find(address + i * 8);
            out[i] = it == words.end() ? 0 : it->second;
        }
    }
};

}  // namespace

BOXEDVN_TEST(x11_bridge64_private_number_does_not_collide) {
    CHECK(BOXEDWINE_X64_HOSTCALL_X11_BRIDGE != 0x7fff0001ULL);
    CHECK(BOXEDWINE_X64_HOSTCALL_X11_BRIDGE > 0x7fff0000ULL);
    CHECK_EQ(BOXEDWINE_X64_X11_MAX_ARGS, 16U);
}

BOXEDVN_TEST(x11_bridge64_known_implemented_and_invalid_operations_differ) {
    CHECK(boxedvn::x11Bridge64KnownOp(BOXEDWINE_X64_X11_OP_INIT_THREADS));
    CHECK(boxedvn::x11Bridge64KnownOp(BOXEDWINE_X64_X11_OP_CREATE_WINDOW));
    CHECK(boxedvn::x11Bridge64KnownOp(BOXEDWINE_X64_X11_OP_COUNT - 1));
    CHECK(!boxedvn::x11Bridge64KnownOp(BOXEDWINE_X64_X11_OP_COUNT));
    CHECK(!boxedvn::x11Bridge64KnownOp(0xFFFFFFFFFFFFFFFFULL));
    CHECK_EQ(std::string(boxedvn::x11Bridge64OpName(BOXEDWINE_X64_X11_OP_OPEN_DISPLAY)),
             std::string("open-display"));
    CHECK_EQ(std::string(boxedvn::x11Bridge64OpName(BOXEDWINE_X64_X11_OP_MAP_WINDOW)),
             std::string("map-window"));
    CHECK(boxedvn::x11Bridge64OpName(BOXEDWINE_X64_X11_OP_COUNT) == nullptr);
    // Every table entry names itself, and no two share a name.
    std::set<std::string> names;
    for (uint64_t op = 0; op < BOXEDWINE_X64_X11_OP_COUNT; op++) {
        const char* name = boxedvn::x11Bridge64OpName(op);
        CHECK(name != nullptr);
        if (name) {
            CHECK(names.insert(name).second);
        }
    }
    // The error codes are distinct from each other and from every Xlib
    // Status/XID a successful call can return.
    CHECK(BOXEDWINE_X64_X11_E_BADOP < 0);
    CHECK(BOXEDWINE_X64_X11_E_BUFFER < 0);
    CHECK(BOXEDWINE_X64_X11_E_FAULT < 0);
    CHECK(BOXEDWINE_X64_X11_E_ARGS < 0);
    CHECK(BOXEDWINE_X64_X11_E_UNIMPL < 0);
    CHECK(BOXEDWINE_X64_X11_E_DISPLAY < 0);
    CHECK(BOXEDWINE_X64_X11_E_BADOP != BOXEDWINE_X64_X11_E_FAULT);
    CHECK(BOXEDWINE_X64_X11_E_BUFFER != BOXEDWINE_X64_X11_E_UNIMPL);
}

BOXEDVN_TEST(x11_bridge64_zero_through_fifteen_arguments_arrive_intact) {
    FakeProbe probe;
    probe.map(0x7a4000000000ULL, true);
    FakeMemory memory;
    const uint64_t base = 0x7a4000000100ULL;
    for (uint64_t count = 0; count <= 16; count++) {
        for (uint64_t i = 0; i < count; i++) {
            // High bits set so a truncation to 32 bits would be visible.
            memory.words[base + i * 8] = 0x8000000000000000ULL | (i << 40) | (0xABCDULL + i);
        }
        const boxedvn::X11Bridge64Call call = boxedvn::x11Bridge64Decode(
            probe, BOXEDWINE_X64_X11_OP_CREATE_WINDOW, count ? base : 0, count,
            [&memory](uint64_t address, uint64_t* out, uint64_t n) {
                memory.read(address, out, n);
            });
        CHECK(call.admission == boxedvn::X11Bridge64Admission::Admitted);
        CHECK_EQ(call.count, count);
        for (uint64_t i = 0; i < count; i++) {
            CHECK_EQ(call.args[i], 0x8000000000000000ULL | (i << 40) | (0xABCDULL + i));
        }
        for (uint64_t i = count; i < BOXEDWINE_X64_X11_MAX_ARGS; i++) {
            CHECK_EQ(call.args[i], 0ULL);
        }
    }
    // Seventeen is one too many.
    const boxedvn::X11Bridge64Call tooMany = boxedvn::x11Bridge64Decode(
        probe, BOXEDWINE_X64_X11_OP_CREATE_WINDOW, base, 17,
        [&memory](uint64_t address, uint64_t* out, uint64_t n) {
            memory.read(address, out, n);
        });
    CHECK(tooMany.admission == boxedvn::X11Bridge64Admission::BadArgumentCount);
    CHECK_EQ(boxedvn::x11Bridge64AdmissionResult(tooMany.admission),
             (int64_t)BOXEDWINE_X64_X11_E_ARGS);
}

BOXEDVN_TEST(x11_bridge64_refuses_an_unknown_operation_before_touching_memory) {
    FakeProbe probe;
    bool touched = false;
    const boxedvn::X11Bridge64Call call = boxedvn::x11Bridge64Decode(
        probe, BOXEDWINE_X64_X11_OP_COUNT + 7, 0x7a4000000000ULL, 3,
        [&touched](uint64_t, uint64_t*, uint64_t) { touched = true; });
    CHECK(call.admission == boxedvn::X11Bridge64Admission::UnknownOperation);
    CHECK(!touched);
    CHECK_EQ(boxedvn::x11Bridge64AdmissionResult(call.admission),
             (int64_t)BOXEDWINE_X64_X11_E_BADOP);
}

BOXEDVN_TEST(x11_bridge64_invalid_guest_pointers_fail_safely) {
    FakeProbe probe;
    probe.map(0x7a4000000000ULL, true);   // readable + writable
    probe.map(0x7a4000001000ULL, false);  // readable only
    bool touched = false;
    auto reader = [&touched](uint64_t, uint64_t*, uint64_t) { touched = true; };

    // A null array with a non-zero count.
    CHECK(boxedvn::x11Bridge64Decode(probe, 0, 0, 2, reader).admission ==
          boxedvn::X11Bridge64Admission::ArgumentArrayFault);
    // A non-canonical address.
    CHECK(boxedvn::x11Bridge64Decode(probe, 0, 0xFFFF800000000000ULL, 1, reader).admission ==
          boxedvn::X11Bridge64Admission::ArgumentArrayFault);
    // An unmapped page.
    CHECK(boxedvn::x11Bridge64Decode(probe, 0, 0x7a4000009000ULL, 1, reader).admission ==
          boxedvn::X11Bridge64Admission::ArgumentArrayFault);
    // A read-only page: the array is written back, so it must be writable.
    CHECK(boxedvn::x11Bridge64Decode(probe, 0, 0x7a4000001000ULL, 1, reader).admission ==
          boxedvn::X11Bridge64Admission::ArgumentArrayFault);
    // A range that starts on a good page but runs into a bad one.
    CHECK(boxedvn::x11Bridge64Decode(probe, 0, 0x7a4000000FF8ULL, 2, reader).admission ==
          boxedvn::X11Bridge64Admission::ArgumentArrayFault);
    CHECK(!touched);
    // Wrap-around near the top of the canonical range.
    CHECK(!boxedvn::x11Bridge64RangeAccessible(probe, 0x00007FFFFFFFFFF0ULL, 0x100, false));
    // The good page, read and written.
    CHECK(boxedvn::x11Bridge64RangeAccessible(probe, 0x7a4000000010ULL, 64, true));
    CHECK(boxedvn::x11Bridge64RangeAccessible(probe, 0x7a4000001010ULL, 64, false));
    CHECK(!boxedvn::x11Bridge64RangeAccessible(probe, 0x7a4000001010ULL, 64, true));
    // A zero-length range never faults; a null pointer with a length does.
    CHECK(boxedvn::x11Bridge64RangeAccessible(probe, 0, 0, true));
    CHECK(!boxedvn::x11Bridge64RangeAccessible(probe, 0, 1, false));
}
