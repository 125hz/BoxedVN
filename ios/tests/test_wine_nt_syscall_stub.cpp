#include "boxedvn_test.h"
#include "wine_nt_syscall_stub.h"

#include <cstring>
#include <map>
#include <vector>

using namespace boxedvn;

// Wine's PE ntdll reaches its Unix side through one 32-byte thunk per NT
// service. While KUSER_SHARED_DATA's SystemCall flag is clear every thunk
// falls into a raw SYSCALL with a Windows NT ordinal in RAX, which is not a
// Linux syscall number.
//
// The bytes below are the ones actually packaged: ntdll.dll SHA-256
// 1a811cce0973b86d307ca19eae613345e55a1b0e285b381a159fbb4f899d7c2c, from
// wine64.zip SHA-256
// dbfa3a56f63ebfba12135c0dcc331e92e4cf2187389684f7c80fdc4a9446fef7. An
// earlier revision of the matcher guessed a shorter thunk with the dispatcher
// call directly on the jne target; nothing matched, and ordinals 154 and 227
// reached the Linux table. These tests exist so the layout is taken from the
// shipped module rather than from an assumption about it.

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

// The exact 32 bytes of the packaged thunk, laid out so the SYSCALL lands on
// `syscallAddress`. Only the mov-eax immediate differs between services.
std::vector<uint8_t> packagedWineThunk(uint32_t ordinal) {
    std::vector<uint8_t> bytes = {
        0x4C, 0x8B, 0xD1,                                // mov r10, rcx
        0xB8, 0x00, 0x00, 0x00, 0x00,                    // mov eax, ordinal
        0xF6, 0x04, 0x25, 0x08, 0x03, 0xFE, 0x7F, 0x01,  // test [7ffe0308],1
        0x75, 0x03,                                      // jne  syscall+3
        0x0F, 0x05,                                      // syscall
        0xC3,                                            // ret
        0xEB, 0x01,                                      // jmp  syscall+6
        0xC3,                                            // ret  (padding)
        0xFF, 0x14, 0x25, 0x00, 0x10, 0xFE, 0x7F,        // call [7ffe1000]
        0xC3,                                            // ret
    };
    bytes[4] = (uint8_t)(ordinal & 0xFF);
    bytes[5] = (uint8_t)((ordinal >> 8) & 0xFF);
    bytes[6] = (uint8_t)((ordinal >> 16) & 0xFF);
    bytes[7] = (uint8_t)((ordinal >> 24) & 0xFF);
    return bytes;
}

// The older layout with no bridge: the dispatcher call sits on the jne target.
std::vector<uint8_t> directWineThunk(uint32_t ordinal) {
    std::vector<uint8_t> bytes = {
        0x4C, 0x8B, 0xD1,
        0xB8, 0x00, 0x00, 0x00, 0x00,
        0xF6, 0x04, 0x25, 0x08, 0x03, 0xFE, 0x7F, 0x01,
        0x75, 0x03,
        0x0F, 0x05,
        0xC3,
        0xFF, 0x14, 0x25, 0x00, 0x10, 0xFE, 0x7F,        // call [7ffe1000]
        0xC3,
    };
    bytes[4] = (uint8_t)(ordinal & 0xFF);
    bytes[5] = (uint8_t)((ordinal >> 8) & 0xFF);
    bytes[6] = (uint8_t)((ordinal >> 16) & 0xFF);
    bytes[7] = (uint8_t)((ordinal >> 24) & 0xFF);
    return bytes;
}

// Module base 0x6fffffc50000; ordinal 154's stub is at RVA 0x54d10 and its
// SYSCALL at RVA 0x54d22. ksyscall64 reported 0x6fffffca4d24 because that
// diagnostic prints the post-SYSCALL RIP.
constexpr uint64_t kModuleBase = 0x6fffffc50000ULL;
constexpr uint64_t kOrdinal154Syscall = kModuleBase + 0x54d22ULL;
constexpr uint64_t kOrdinal227Syscall = kModuleBase + 0x55642ULL;
constexpr uint64_t kDispatcher = 0x7ffff7a12340ULL;

struct Fixture {
    FakeGuestMemory memory;
    uint64_t syscallAddress = 0;

    void install(uint32_t ordinal, uint64_t syscall) {
        syscallAddress = syscall;
        memory.write(syscall + (uint64_t)kWineNtStubFirstOffset,
                     packagedWineThunk(ordinal));
        memory.writeQword(K_WINE_KUSER_SYSCALL_DISPATCHER, kDispatcher);
    }

    void installDirect(uint32_t ordinal, uint64_t syscall) {
        syscallAddress = syscall;
        memory.write(syscall + (uint64_t)kWineNtStubFirstOffset,
                     directWineThunk(ordinal));
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

    uint8_t peek(int offset) {
        uint8_t byte = 0;
        FakeGuestMemory::read(&memory,
                              syscallAddress + (uint64_t)(int64_t)offset,
                              &byte, 1);
        return byte;
    }
};

} // namespace

BOXEDVN_TEST(wine_nt_stub_matches_the_packaged_ordinal_154_bytes) {
    // The exact bytes and the exact live address from the device.
    Fixture fixture;
    fixture.install(154, kOrdinal154Syscall);
    CHECK(kOrdinal154Syscall == 0x6fffffca4d22ULL);
    // The mov-eax immediate for 154 is 0x9a.
    CHECK(fixture.peek(-14) == 0x9A);

    WineNtSyscallStub stub;
    CHECK(fixture.match(154, stub));
    CHECK(stub.ntOrdinal == 154);
    CHECK(stub.syscallAddress == 0x6fffffca4d22ULL);
    // Resume on the jne target, which is what the device log must report.
    CHECK(stub.indirectPath == 0x6fffffca4d25ULL);
    CHECK(stub.indirectPath == stub.syscallAddress + 3);
    // The eb 01 bridge hops the padding ret and reaches the dispatcher call.
    CHECK(stub.dispatcherCall == stub.syscallAddress + 6);
    CHECK(stub.dispatcher == kDispatcher);
}

BOXEDVN_TEST(wine_nt_stub_matches_the_packaged_ordinal_227_bytes) {
    // The ordinal that looped. Only the immediate differs: 0xe3.
    Fixture fixture;
    fixture.install(227, kOrdinal227Syscall);
    CHECK(kOrdinal227Syscall == 0x6fffffca5642ULL);
    CHECK(fixture.peek(-14) == 0xE3);

    WineNtSyscallStub stub;
    CHECK(fixture.match(227, stub));
    CHECK(stub.ntOrdinal == 227);
    CHECK(stub.syscallAddress == 0x6fffffca5642ULL);
    CHECK(stub.indirectPath == 0x6fffffca5645ULL);
    CHECK(stub.dispatcherCall == stub.syscallAddress + 6);
    // Both thunks come from one generated table: 73 ordinals apart at Wine's
    // 32-byte stride.
    CHECK(kOrdinal227Syscall - kOrdinal154Syscall == (227 - 154) * 32);
}

BOXEDVN_TEST(wine_nt_stub_layout_constants_describe_the_packaged_thunk) {
    CHECK(kWineNtStubFirstOffset == -18);
    CHECK(kWineNtStubLastOffset == 13);
    CHECK(kWineNtStubLength == 32);
    CHECK(kWineNtStubLength ==
          (unsigned)(kWineNtStubLastOffset - kWineNtStubFirstOffset + 1));
    CHECK(packagedWineThunk(154).size() == kWineNtStubLength);
}

BOXEDVN_TEST(wine_nt_stub_bridge_resolves_to_the_dispatcher_call) {
    // The whole reason resuming at +3 is correct: Wine's own branch goes
    // there, and the two bytes there carry execution to the call at +6.
    Fixture fixture;
    fixture.install(154, kOrdinal154Syscall);
    CHECK(fixture.peek(3) == 0xEB);
    CHECK(fixture.peek(4) == 0x01);
    CHECK(fixture.peek(5) == 0xC3);
    CHECK(fixture.peek(6) == 0xFF);
    CHECK(fixture.peek(13) == 0xC3);

    WineNtSyscallStub stub;
    CHECK(fixture.match(154, stub));
    // jmp rel8 ends at +5 and displaces by 1.
    CHECK(stub.syscallAddress + 5 + 1 == stub.dispatcherCall);
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

BOXEDVN_TEST(wine_nt_stub_rejects_a_corrupt_bridge_opcode) {
    // Without the eb the byte at +3 selects a different tail, or none.
    for (uint8_t opcode : {(uint8_t)0x00, (uint8_t)0x90, (uint8_t)0xC3,
                           (uint8_t)0xE9, (uint8_t)0xEA}) {
        Fixture fixture;
        fixture.install(154, kOrdinal154Syscall);
        fixture.poke(3, opcode);
        WineNtSyscallStub stub;
        CHECK(!fixture.match(154, stub));
    }
}

BOXEDVN_TEST(wine_nt_stub_rejects_a_corrupt_bridge_displacement) {
    // The bridge must resolve exactly to the dispatcher call. Anything else
    // means resuming at +3 would land somewhere this code has not validated.
    for (uint8_t displacement : {(uint8_t)0x00, (uint8_t)0x02, (uint8_t)0x03,
                                 (uint8_t)0xFF, (uint8_t)0x7F}) {
        Fixture fixture;
        fixture.install(154, kOrdinal154Syscall);
        fixture.poke(4, displacement);
        WineNtSyscallStub stub;
        CHECK(!fixture.match(154, stub));
    }
}

BOXEDVN_TEST(wine_nt_stub_rejects_a_corrupt_padding_ret) {
    Fixture fixture;
    fixture.install(154, kOrdinal154Syscall);
    fixture.poke(5, 0x90);
    WineNtSyscallStub stub;
    CHECK(!fixture.match(154, stub));
}

BOXEDVN_TEST(wine_nt_stub_rejects_a_corrupt_dispatcher_call) {
    for (int offset : {6, 7, 8, 13}) {
        Fixture fixture;
        fixture.install(154, kOrdinal154Syscall);
        fixture.poke(offset, 0x90);
        WineNtSyscallStub stub;
        CHECK(!fixture.match(154, stub));
    }
}

BOXEDVN_TEST(wine_nt_stub_rejects_every_single_byte_corruption_of_the_layout) {
    // Every byte of the packaged 32-byte thunk is load-bearing. Walk the whole
    // layout and corrupt one byte at a time; nothing may still match.
    for (int offset = kWineNtStubFirstOffset; offset <= kWineNtStubLastOffset;
         ++offset) {
        Fixture fixture;
        fixture.install(154, kOrdinal154Syscall);
        WineNtSyscallStub reference;
        CHECK(fixture.match(154, reference));

        fixture.poke(offset, (uint8_t)(fixture.peek(offset) ^ 0xFF));
        WineNtSyscallStub stub;
        CHECK(!fixture.match(154, stub));
    }
}

BOXEDVN_TEST(genuine_linux_syscall_227_is_not_redirected) {
    // A real clock_settime from glibc: the SYSCALL is there, the surrounding
    // bytes are not Wine's thunk. This must reach the Linux dispatcher.
    FakeGuestMemory memory;
    const uint64_t address = 0x7ffff7c91234ULL;
    memory.write(address - 18, {
        0x48, 0x89, 0xF7,              // mov rdi, rsi
        0xB8, 0xE3, 0x00, 0x00, 0x00,  // mov eax, 227
        0x48, 0x89, 0xD6, 0x48, 0x89, 0xCA, 0x4D, 0x89,
        0xC2, 0x4D,
        0x0F, 0x05,                    // syscall
        0x48, 0x3D, 0x01, 0xF0, 0xFF, 0xFF, 0x73, 0x01,
        0xC3, 0x0F, 0x1F, 0x00,
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
    fixture.install(154, kOrdinal154Syscall);
    WineNtSyscallStub stub;
    CHECK(!fixture.match(155, stub));
    CHECK(!fixture.match(227, stub));
    CHECK(!fixture.match(0, stub));
    // An NT ordinal is loaded by `mov eax`, so it cannot have high bits.
    CHECK(!fixture.match(0x100000000ULL | 154, stub));
    CHECK(fixture.match(154, stub));
}

BOXEDVN_TEST(wine_nt_stub_rejects_the_wrong_kuser_addresses) {
    // The SystemCall flag and the dispatcher pointer are decoded from the
    // instructions' own displacement fields, not assumed. A thunk that tests
    // some other address is not Wine's.
    Fixture flag;
    flag.install(154, kOrdinal154Syscall);
    flag.poke(-7, 0x09);  // test [0x7ffe0309], 1
    WineNtSyscallStub stub;
    CHECK(!flag.match(154, stub));

    Fixture call;
    call.install(154, kOrdinal154Syscall);
    call.poke(9, 0x08);  // call [0x7ffe1008]
    CHECK(!call.match(154, stub));
}

BOXEDVN_TEST(wine_nt_stub_rejects_a_branch_that_misses_the_bridge) {
    // The jne must land exactly on the bridge. A different displacement means
    // this is not the control flow the redirect assumes.
    for (uint8_t displacement : {(uint8_t)0x00, (uint8_t)0x02, (uint8_t)0x04,
                                 (uint8_t)0x06, (uint8_t)0xFD}) {
        Fixture fixture;
        fixture.install(154, kOrdinal154Syscall);
        fixture.poke(-1, displacement);
        WineNtSyscallStub stub;
        CHECK(!fixture.match(154, stub));
    }
}

BOXEDVN_TEST(wine_nt_stub_rejects_an_unpublished_dispatcher) {
    // Wine has not wired its dispatcher yet. There is nothing to redirect to.
    Fixture fixture;
    fixture.install(154, kOrdinal154Syscall);
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
    Fixture head;
    head.install(154, kOrdinal154Syscall);
    WineNtSyscallStub stub;
    CHECK(head.match(154, stub));
    head.memory.unmap(kOrdinal154Syscall + (uint64_t)kWineNtStubFirstOffset, 1);
    CHECK(!head.match(154, stub));

    // The tail is read separately, so its absence has to be refused too.
    Fixture tail;
    tail.install(154, kOrdinal154Syscall);
    tail.memory.unmap(kOrdinal154Syscall + 13, 1);
    CHECK(!tail.match(154, stub));

    Fixture noKuser;
    noKuser.install(154, kOrdinal154Syscall);
    noKuser.memory.unmap(K_WINE_KUSER_SYSCALL_DISPATCHER, 8);
    CHECK(!noKuser.match(154, stub));
}

BOXEDVN_TEST(wine_nt_stub_rejects_a_null_reader_and_a_low_address) {
    WineNtSyscallStub stub;
    CHECK(!matchWineNtSyscallStub(nullptr, nullptr, kOrdinal154Syscall, 154,
                                  stub));
    // A SYSCALL in the first 18 bytes of the address space cannot have a
    // thunk in front of it; the read must not wrap.
    FakeGuestMemory memory;
    memory.writeQword(K_WINE_KUSER_SYSCALL_DISPATCHER, kDispatcher);
    CHECK(!matchWineNtSyscallStub(&FakeGuestMemory::read, &memory, 4, 154,
                                  stub));
}

BOXEDVN_TEST(wine_nt_stub_still_accepts_the_direct_layout_completely) {
    // Kept because it costs nothing and is validated to the same depth, not
    // because anything currently ships it. Here the dispatcher call is the
    // jne target itself.
    Fixture fixture;
    fixture.installDirect(154, kOrdinal154Syscall);
    WineNtSyscallStub stub;
    CHECK(fixture.match(154, stub));
    CHECK(stub.ntOrdinal == 154);
    CHECK(stub.indirectPath == kOrdinal154Syscall + 3);
    CHECK(stub.dispatcherCall == kOrdinal154Syscall + 3);

    // And every byte of that layout is load-bearing too.
    for (int offset = kWineNtStubFirstOffset; offset <= 10; ++offset) {
        Fixture corrupt;
        corrupt.installDirect(154, kOrdinal154Syscall);
        corrupt.poke(offset, (uint8_t)(corrupt.peek(offset) ^ 0xFF));
        WineNtSyscallStub rejected;
        CHECK(!corrupt.match(154, rejected));
    }
}

BOXEDVN_TEST(wine_nt_stub_rejects_an_unknown_tail) {
    // Neither a bridge nor a direct call: the matcher must not guess.
    Fixture fixture;
    fixture.install(154, kOrdinal154Syscall);
    fixture.poke(3, 0xE8);  // call rel32
    WineNtSyscallStub stub;
    CHECK(!fixture.match(154, stub));
}

BOXEDVN_TEST(wine_nt_stub_constants_name_kuser_shared_data) {
    CHECK(K_WINE_KUSER_SYSTEM_CALL_FLAG == 0x7ffe0308ULL);
    CHECK(K_WINE_KUSER_SYSCALL_DISPATCHER == 0x7ffe1000ULL);
    // KUSER_SHARED_DATA is one 64 KiB region at 0x7ffe0000; both live in it.
    CHECK(K_WINE_KUSER_SYSTEM_CALL_FLAG >= 0x7ffe0000ULL);
    CHECK(K_WINE_KUSER_SYSCALL_DISPATCHER < 0x7ffe0000ULL + 0x10000ULL);
}
