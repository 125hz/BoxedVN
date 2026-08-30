#include "boxedvn_test.h"
#include "wine_nt_syscall_stub.h"

#include <cstring>
#include <map>
#include <vector>

using namespace boxedvn;

// Wine's PE ntdll reaches its Unix side through one 32-byte thunk per NT
// service. While KUSER_SHARED_DATA's SystemCall flag is clear every thunk
// falls into a raw SYSCALL with a Windows NT ordinal in RAX, which is not a
// Linux syscall number. The device log shows what happens then: NT 227
// returning -ENOSYS to a caller that retried it 3,215,735 times.
//
// Recognising the thunk is what makes the redirect safe, so these tests are
// about exactly one question: what may and may not be mistaken for one.

namespace {

// A synthetic guest address space, so the matcher can be driven without a
// KMemory64 and without any assumption about where Wine maps ntdll.
class FakeGuestMemory {
public:
    void write(uint64_t address, const std::vector<uint8_t>& bytes) {
        for (size_t index = 0; index < bytes.size(); ++index) {
            bytes_[address + index] = bytes[index];
        }
    }

    void writeQword(uint64_t address, uint64_t value) {
        std::vector<uint8_t> encoded(8);
        for (int index = 0; index < 8; ++index) {
            encoded[index] = (uint8_t)((value >> (index * 8)) & 0xFF);
        }
        write(address, encoded);
    }

    void unmap(uint64_t address, unsigned length) {
        for (unsigned index = 0; index < length; ++index) {
            bytes_.erase(address + index);
        }
    }

    static bool read(void* context, uint64_t address, uint8_t* out,
                     unsigned length) {
        auto* memory = static_cast<FakeGuestMemory*>(context);
        for (unsigned index = 0; index < length; ++index) {
            auto found = memory->bytes_.find(address + index);
            if (found == memory->bytes_.end()) {
                return false;
            }
            out[index] = found->second;
        }
        return true;
    }

private:
    std::map<uint64_t, uint8_t> bytes_;
};

// The exact thunk winebuild emits, laid out so the SYSCALL lands on
// `syscallAddress`.
std::vector<uint8_t> wineThunk(uint32_t ordinal) {
    std::vector<uint8_t> bytes = {
        0x4C, 0x8B, 0xD1,                                // mov r10, rcx
        0xB8, 0x00, 0x00, 0x00, 0x00,                    // mov eax, ordinal
        0xF6, 0x04, 0x25, 0x08, 0x03, 0xFE, 0x7F, 0x01,  // test [7ffe0308],1
        0x75, 0x03,                                      // jne .indirect
        0x0F, 0x05,                                      // syscall
        0xC3,                                            // ret
        0xFF, 0x14, 0x25, 0x00, 0x10, 0xFE, 0x7F,        // call [7ffe1000]
        0xC3,                                            // ret
    };
    bytes[4] = (uint8_t)(ordinal & 0xFF);
    bytes[5] = (uint8_t)((ordinal >> 8) & 0xFF);
    bytes[6] = (uint8_t)((ordinal >> 16) & 0xFF);
    bytes[7] = (uint8_t)((ordinal >> 24) & 0xFF);
    return bytes;
}

// The address the observed device log reported for NT 154, and the dispatcher
// Wine had already published.
constexpr uint64_t kObservedSyscall = 0x6fffffca4d22ULL;
constexpr uint64_t kDispatcher = 0x7ffff7a12340ULL;

struct Fixture {
    FakeGuestMemory memory;
    uint64_t syscallAddress = 0;

    void install(uint32_t ordinal, uint64_t syscall) {
        syscallAddress = syscall;
        memory.write(syscall + (uint64_t)kWineNtStubFirstOffset,
                     wineThunk(ordinal));
        memory.writeQword(K_WINE_KUSER_SYSCALL_DISPATCHER, kDispatcher);
    }

    bool match(uint64_t rax, WineNtSyscallStub& stub) {
        return matchWineNtSyscallStub(&FakeGuestMemory::read, &memory,
                                      syscallAddress, rax, stub);
    }

    // Overwrite one byte of the installed thunk, at an offset relative to the
    // SYSCALL instruction.
    void poke(int offset, uint8_t value) {
        memory.write(syscallAddress + (uint64_t)(int64_t)offset, {value});
    }
};

} // namespace

BOXEDVN_TEST(wine_nt_stub_is_recognised_at_the_observed_high_address) {
    // The address the old implementation could not see: it gated on ntdll's
    // 0x170000000 image base, and this Wine maps its thunks near
    // 0x6fffffca0000.
    Fixture fixture;
    fixture.install(154, kObservedSyscall);

    WineNtSyscallStub stub;
    CHECK(fixture.match(154, stub));
    CHECK(stub.ntOrdinal == 154);
    CHECK(stub.syscallAddress == kObservedSyscall);
    // The jne targets the `call [0x7ffe1000]`, three bytes past the SYSCALL.
    CHECK(stub.indirectPath == kObservedSyscall + 3);
    CHECK(stub.dispatcher == kDispatcher);
    CHECK(kObservedSyscall < 0x170000000ULL ||
          kObservedSyscall >= 0x170400000ULL);
}

BOXEDVN_TEST(wine_nt_stub_is_recognised_wherever_ntdll_is_mapped) {
    // The point of matching on shape: the address is not part of the contract.
    const uint64_t addresses[] = {
        0x170001234ULL, 0x6fffffca5642ULL, 0x7ffe80000000ULL, 0x40000ULL,
    };
    for (uint64_t address : addresses) {
        Fixture fixture;
        fixture.install(227, address);
        WineNtSyscallStub stub;
        CHECK(fixture.match(227, stub));
        CHECK(stub.ntOrdinal == 227);
        CHECK(stub.indirectPath == address + 3);
    }
}

BOXEDVN_TEST(wine_nt_stub_second_observed_ordinal_is_recognised) {
    // NT 227 at 0x6fffffca5644-2 is the one that looped. Its distance from the
    // NT 154 thunk is 73 ordinals at Wine's 32-byte thunk stride, which is
    // what confirms both are entries in the same generated table.
    Fixture fixture;
    fixture.install(227, 0x6fffffca5642ULL);
    WineNtSyscallStub stub;
    CHECK(fixture.match(227, stub));
    CHECK(stub.ntOrdinal == 227);
    CHECK(0x6fffffca5642ULL - kObservedSyscall == (227 - 154) * 32);
}

BOXEDVN_TEST(genuine_linux_syscall_227_is_not_redirected) {
    // A real clock_settime from glibc: the SYSCALL is there, the surrounding
    // bytes are not Wine's thunk. This must reach the Linux dispatcher.
    FakeGuestMemory memory;
    const uint64_t address = 0x7ffff7c91234ULL;
    // Ordinary glibc syscall sequence: mov eax,imm32 / syscall / cmp / jae.
    memory.write(address - 18, {
        0x48, 0x89, 0xF7,              // mov rdi, rsi
        0xB8, 0xE3, 0x00, 0x00, 0x00,  // mov eax, 227
        0x48, 0x89, 0xD6, 0x48, 0x89, 0xCA, 0x4D, 0x89,
        0xC2, 0x4D,
        0x0F, 0x05,                    // syscall
        0x48, 0x3D, 0x01, 0xF0, 0xFF, 0xFF, 0x73, 0x01,
        0xC3, 0x0F,
    });
    memory.writeQword(K_WINE_KUSER_SYSCALL_DISPATCHER, kDispatcher);

    WineNtSyscallStub stub;
    CHECK(!matchWineNtSyscallStub(&FakeGuestMemory::read, &memory, address,
                                  227, stub));
}

BOXEDVN_TEST(wine_nt_stub_requires_the_ordinal_to_match_rax) {
    // A thunk that is present but not the one being executed -- a jump landed
    // elsewhere, or RAX was set by something else. Redirecting on a mismatched
    // ordinal would send Wine's dispatcher to the wrong service.
    Fixture fixture;
    fixture.install(154, kObservedSyscall);
    WineNtSyscallStub stub;
    CHECK(!fixture.match(155, stub));
    CHECK(!fixture.match(0, stub));
    // An NT ordinal is loaded by `mov eax`, so it cannot have high bits.
    CHECK(!fixture.match(0x100000000ULL | 154, stub));
    CHECK(fixture.match(154, stub));
}

BOXEDVN_TEST(wine_nt_stub_rejects_a_single_wrong_opcode_byte) {
    // Every byte of the shape is load-bearing. Walk the whole thunk and
    // corrupt one byte at a time; nothing may still match.
    for (int offset = kWineNtStubFirstOffset; offset <= kWineNtStubLastOffset;
         ++offset) {
        Fixture fixture;
        fixture.install(154, kObservedSyscall);
        WineNtSyscallStub reference;
        CHECK(fixture.match(154, reference));

        uint8_t original[1] = {};
        FakeGuestMemory::read(&fixture.memory,
                              kObservedSyscall + (uint64_t)(int64_t)offset,
                              original, 1);
        fixture.poke(offset, (uint8_t)(original[0] ^ 0xFF));
        WineNtSyscallStub stub;
        CHECK(!fixture.match(154, stub));
    }
}

BOXEDVN_TEST(wine_nt_stub_rejects_the_wrong_kuser_addresses) {
    // The SystemCall flag and the dispatcher pointer are decoded from the
    // instructions' own displacement fields, not assumed. A thunk that tests
    // some other address is not Wine's.
    Fixture fixture;
    fixture.install(154, kObservedSyscall);
    fixture.poke(-7, 0x09);  // test [0x7ffe0309], 1
    WineNtSyscallStub stub;
    CHECK(!fixture.match(154, stub));

    Fixture second;
    second.install(154, kObservedSyscall);
    second.poke(6, 0x08);  // call [0x7ffe1008]
    CHECK(!second.match(154, stub));
}

BOXEDVN_TEST(wine_nt_stub_rejects_a_branch_that_skips_the_indirect_call) {
    // The jne must land exactly on the `call [0x7ffe1000]`. A different
    // displacement means this is not the control flow the redirect assumes,
    // and resuming at syscall+3 would be a jump into the middle of something.
    for (uint8_t displacement : {(uint8_t)0x00, (uint8_t)0x02, (uint8_t)0x04,
                                 (uint8_t)0x0A, (uint8_t)0xFD}) {
        Fixture fixture;
        fixture.install(154, kObservedSyscall);
        fixture.poke(-1, displacement);
        WineNtSyscallStub stub;
        CHECK(!fixture.match(154, stub));
    }
}

BOXEDVN_TEST(wine_nt_stub_rejects_an_unpublished_dispatcher) {
    // Wine has not wired its dispatcher yet. There is nothing to redirect to.
    Fixture fixture;
    fixture.install(154, kObservedSyscall);
    fixture.memory.writeQword(K_WINE_KUSER_SYSCALL_DISPATCHER, 0);
    WineNtSyscallStub stub;
    CHECK(!fixture.match(154, stub));

    fixture.memory.writeQword(K_WINE_KUSER_SYSCALL_DISPATCHER, ~(uint64_t)0);
    CHECK(!fixture.match(154, stub));

    fixture.memory.writeQword(K_WINE_KUSER_SYSCALL_DISPATCHER, kDispatcher);
    CHECK(fixture.match(154, stub));
}

BOXEDVN_TEST(wine_nt_stub_rejects_unreadable_memory) {
    // A partially mapped region must not look like a thunk. KMemory64 reports
    // an absent page as zero bytes, which is why the reader refuses the range
    // rather than reading it.
    Fixture fixture;
    fixture.install(154, kObservedSyscall);
    WineNtSyscallStub stub;
    CHECK(fixture.match(154, stub));

    // The byte before the thunk's first instruction is outside it, so removing
    // the thunk's own first byte is what has to be refused.
    fixture.memory.unmap(kObservedSyscall + (uint64_t)kWineNtStubFirstOffset, 1);
    CHECK(!fixture.match(154, stub));

    Fixture noKuser;
    noKuser.install(154, kObservedSyscall);
    noKuser.memory.unmap(K_WINE_KUSER_SYSCALL_DISPATCHER, 8);
    CHECK(!noKuser.match(154, stub));
}

BOXEDVN_TEST(wine_nt_stub_rejects_a_null_reader_and_a_low_address) {
    WineNtSyscallStub stub;
    CHECK(!matchWineNtSyscallStub(nullptr, nullptr, kObservedSyscall, 154,
                                  stub));
    // A SYSCALL in the first 18 bytes of the address space cannot have a
    // thunk in front of it; the read must not wrap.
    FakeGuestMemory memory;
    memory.writeQword(K_WINE_KUSER_SYSCALL_DISPATCHER, kDispatcher);
    CHECK(!matchWineNtSyscallStub(&FakeGuestMemory::read, &memory, 4, 154,
                                  stub));
}

BOXEDVN_TEST(wine_nt_stub_constants_name_kuser_shared_data) {
    CHECK(K_WINE_KUSER_SYSTEM_CALL_FLAG == 0x7ffe0308ULL);
    CHECK(K_WINE_KUSER_SYSCALL_DISPATCHER == 0x7ffe1000ULL);
    // KUSER_SHARED_DATA is one 64 KiB region at 0x7ffe0000; both live in it.
    CHECK(K_WINE_KUSER_SYSTEM_CALL_FLAG >= 0x7ffe0000ULL);
    CHECK(K_WINE_KUSER_SYSCALL_DISPATCHER < 0x7ffe0000ULL + 0x10000ULL);
    CHECK(kWineNtStubLength ==
          (unsigned)(kWineNtStubLastOffset - kWineNtStubFirstOffset + 1));
}
