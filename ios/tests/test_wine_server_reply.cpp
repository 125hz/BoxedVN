#include "boxedvn_test.h"
#include "wine_server_reply.h"

#include <array>
#include <cstdint>
#include <cstring>

using namespace boxedvn;

// The device gets all the way through REQ_init_process_done: the client writes
// opcode 4 on the request descriptor, the server replies on the reply
// descriptor with error 0 and a PE entry point, and the process then exits 1
// without running a single Windows instruction. The socket ring kept only the
// first sixteen bytes of a message, which stops one field short of `suspend`,
// so that field was never visible.
//
// What is tested here is the recognition, not a transcript: nothing may know a
// pid or an entry address, because a decoder that recognises the exchange by
// the values one log happened to contain proves nothing about the next run.

namespace {

// Wine's protocol union is a fixed 64 bytes in both directions.
std::array<std::uint8_t, K_WINE_SERVER_MESSAGE_BYTES> emptyMessage() {
    std::array<std::uint8_t, K_WINE_SERVER_MESSAGE_BYTES> message {};
    message.fill(0);
    return message;
}

void putDword(std::uint8_t* message, unsigned offset, std::uint32_t value) {
    message[offset + 0] = (std::uint8_t)(value & 0xff);
    message[offset + 1] = (std::uint8_t)((value >> 8) & 0xff);
    message[offset + 2] = (std::uint8_t)((value >> 16) & 0xff);
    message[offset + 3] = (std::uint8_t)((value >> 24) & 0xff);
}

// A reply exactly as the device logged one: error 0, no variable-length data,
// and the image's entry point.
std::array<std::uint8_t, K_WINE_SERVER_MESSAGE_BYTES> admittedReply(
    std::uint64_t entry, std::int32_t suspend) {
    auto message = emptyMessage();
    putDword(message.data(), 0, 0);                              // error
    putDword(message.data(), 4, 0);                              // reply_size
    putDword(message.data(), 8, (std::uint32_t)(entry & 0xffffffffu));
    putDword(message.data(), 12, (std::uint32_t)(entry >> 32));
    putDword(message.data(), 16, (std::uint32_t)suspend);
    return message;
}

}  // namespace

BOXEDVN_TEST(wine_thread_termination_keeps_windows_exception_status) {
    auto message = emptyMessage();
    putDword(message.data(), 0, K_WINE_REQ_TERMINATE_THREAD);
    putDword(message.data(), 12, 0xfffffffe); // current Windows thread
    putDword(message.data(), 16, 0xc0000005); // access violation, not Linux exit(0)
    auto decoded = decodeWineTerminateThreadRequest(message.data(), message.size());
    CHECK(decoded.valid);
    CHECK_EQ(decoded.handle, 0xfffffffeu);
    CHECK_EQ(decoded.exitCode, 0xc0000005u);
    CHECK(!decodeWineTerminateThreadRequest(message.data(), 16).valid);
    CHECK(!decodeWineTerminateThreadRequest(nullptr, 64).valid);
    putDword(message.data(), 4, 4); // variable payload cannot be this request
    CHECK(!decodeWineTerminateThreadRequest(message.data(), message.size()).valid);
    putDword(message.data(), 4, 0);
    putDword(message.data(), 0, K_WINE_REQ_INIT_PROCESS_DONE);
    CHECK(!decodeWineTerminateThreadRequest(message.data(), message.size()).valid);
}

BOXEDVN_TEST(wine_server_request_opcode_needs_a_whole_message) {
    auto request = emptyMessage();
    putDword(request.data(), 0, K_WINE_REQ_INIT_PROCESS_DONE);

    CHECK_EQ(wineServerRequestOpcode(request.data(), request.size()),
             K_WINE_REQ_INIT_PROCESS_DONE);

    // A partial write is not a request. Treating a prefix as one would let a
    // short write name an exchange that never happened.
    CHECK_EQ(wineServerRequestOpcode(request.data(), 16), -1);
    CHECK_EQ(wineServerRequestOpcode(request.data(), 63), -1);
    CHECK_EQ(wineServerRequestOpcode(request.data(), 65), -1);
    CHECK_EQ(wineServerRequestOpcode(nullptr, request.size()), -1);
}

BOXEDVN_TEST(wine_init_process_done_reply_carries_entry_and_suspend) {
    const auto message = admittedReply(0x00000001400013d0ull, 0);
    const WineInitProcessDoneReply reply =
        decodeWineInitProcessDoneReply(message.data(), message.size());

    CHECK(reply.valid);
    CHECK_EQ(reply.error, 0u);
    CHECK_EQ(reply.replySize, 0u);
    CHECK_EQ(reply.entry, 0x00000001400013d0ull);
    CHECK_EQ(reply.suspend, 0);
    CHECK(wineInitProcessDoneAdmitted(reply));
}

BOXEDVN_TEST(wine_init_process_done_reply_decodes_a_suspended_start) {
    // A non-zero suspend means the server admitted the process but told it to
    // wait. That is a completely different failure from the one on the device
    // and the field the sixteen-byte ring could never show, so it has to
    // survive decoding rather than being assumed zero.
    const auto message = admittedReply(0x0000000140001000ull, 1);
    const WineInitProcessDoneReply reply =
        decodeWineInitProcessDoneReply(message.data(), message.size());

    CHECK_EQ(reply.suspend, 1);
    CHECK(wineInitProcessDoneAdmitted(reply));
}

BOXEDVN_TEST(wine_init_process_done_reply_needs_the_suspend_field) {
    const auto message = admittedReply(0x0000000140001000ull, 0);

    // Sixteen bytes is what the socket ring kept: enough for the header and
    // the entry, one field short of suspend. Decoding it would invent a zero.
    CHECK(!decodeWineInitProcessDoneReply(message.data(), 16).valid);
    CHECK(!decodeWineInitProcessDoneReply(message.data(), 19).valid);
    CHECK(decodeWineInitProcessDoneReply(message.data(), 20).valid);
    CHECK(!decodeWineInitProcessDoneReply(nullptr, 64).valid);
}

BOXEDVN_TEST(wine_init_process_done_refusals_are_not_admissions) {
    // The server refusing, and the server admitting with no entry point, are
    // both real outcomes. Arming a trace over either would be watching a
    // handoff that is not going to happen.
    auto refused = admittedReply(0x0000000140001000ull, 0);
    putDword(refused.data(), 0, 0xc0000022u);  // STATUS_ACCESS_DENIED
    CHECK(!wineInitProcessDoneAdmitted(
        decodeWineInitProcessDoneReply(refused.data(), refused.size())));

    const auto noEntry = admittedReply(0, 0);
    CHECK(!wineInitProcessDoneAdmitted(
        decodeWineInitProcessDoneReply(noEntry.data(), noEntry.size())));

    CHECK(!wineInitProcessDoneAdmitted(WineInitProcessDoneReply {}));
}

BOXEDVN_TEST(wine_server_exchange_is_recognised_without_a_pid_or_an_entry) {
    // The whole point of keying on the opcode: a second process running the
    // same exchange, with a different entry point, is recognised identically.
    auto request = emptyMessage();
    putDword(request.data(), 0, K_WINE_REQ_INIT_PROCESS_DONE);
    CHECK_EQ(wineServerRequestOpcode(request.data(), request.size()),
             K_WINE_REQ_INIT_PROCESS_DONE);

    for (std::uint64_t entry : {0x0000000140001000ull, 0x0000000180002240ull,
                                0x00007ff800000000ull}) {
        const auto message = admittedReply(entry, 0);
        const WineInitProcessDoneReply reply =
            decodeWineInitProcessDoneReply(message.data(), message.size());
        CHECK_EQ(reply.entry, entry);
        CHECK(wineInitProcessDoneAdmitted(reply));
    }

    // A different exchange on the same descriptors must not be mistaken for
    // this one.
    putDword(request.data(), 0, K_WINE_REQ_INIT_PROCESS_DONE + 1);
    CHECK(wineServerRequestOpcode(request.data(), request.size()) !=
          K_WINE_REQ_INIT_PROCESS_DONE);
}
