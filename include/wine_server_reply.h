/*
 * BoxedWine - decoding the Wine-server reply that hands a process its entry.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 *
 * Wine's client and server exchange fixed-size 64-byte protocol unions over a
 * request socket and a reply socket. The last exchange before a process starts
 * running Windows code is REQ_init_process_done: the client writes opcode 4,
 * and the server replies with the PE entry point it admitted.
 *
 * The device log shows that exchange completing -- opcode 4 written on the
 * request fd, 64 bytes read back on the reply fd carrying error 0 and entry
 * 0x00000001400013d0 -- and the process then exiting 1 without ever reaching
 * Windows code. The socket ring only kept the first sixteen bytes of a
 * message, which stops one field short of `suspend`, so the reply is decoded
 * here from the full buffer instead.
 *
 *   struct reply_header { unsigned int error; unsigned int reply_size; };
 *   struct init_process_done_reply {
 *       struct reply_header __header;   // 0
 *       client_ptr_t         entry;     // 8
 *       int                  suspend;   // 16
 *   };
 *
 * Nothing here knows a pid or an entry address: the exchange is recognised by
 * the opcode the process last wrote and by the size of the reply.
 */

#ifndef __WINE_SERVER_REPLY_H__
#define __WINE_SERVER_REPLY_H__

#include <cstdint>

// Wine's protocol union is this size in both directions.
#define K_WINE_SERVER_MESSAGE_BYTES 64

// REQ_init_process_done in Wine 9.0's generated request list. The opcode is
// the first field of the request header.
#define K_WINE_REQ_INIT_PROCESS_DONE 4

#if defined(__cplusplus)

namespace boxedvn {

struct WineInitProcessDoneReply {
    bool valid = false;
    std::uint32_t error = 0;
    std::uint32_t replySize = 0;
    std::uint64_t entry = 0;
    std::int32_t suspend = 0;
};

// The opcode a client request carries, or a negative value when the buffer is
// not a whole protocol message.
inline int wineServerRequestOpcode(const std::uint8_t* message,
                                   std::uint64_t length) noexcept {
    if (message == nullptr || length != K_WINE_SERVER_MESSAGE_BYTES) {
        return -1;
    }
    return (int)((std::uint32_t)message[0] |
                 ((std::uint32_t)message[1] << 8) |
                 ((std::uint32_t)message[2] << 16) |
                 ((std::uint32_t)message[3] << 24));
}

// Decode the reply to REQ_init_process_done. `length` must be the whole
// message: a sixteen-byte prefix stops one field short of `suspend`, which is
// exactly what the socket ring could not show.
inline WineInitProcessDoneReply decodeWineInitProcessDoneReply(
    const std::uint8_t* message, std::uint64_t length) noexcept {
    WineInitProcessDoneReply reply;
    if (message == nullptr || length < 20) {
        return reply;
    }
    auto dword = [message](unsigned offset) -> std::uint32_t {
        return (std::uint32_t)message[offset] |
               ((std::uint32_t)message[offset + 1] << 8) |
               ((std::uint32_t)message[offset + 2] << 16) |
               ((std::uint32_t)message[offset + 3] << 24);
    };
    reply.error = dword(0);
    reply.replySize = dword(4);
    reply.entry = (std::uint64_t)dword(8) | ((std::uint64_t)dword(12) << 32);
    reply.suspend = (std::int32_t)dword(16);
    reply.valid = true;
    return reply;
}

// True when this reply admitted the process: no error, and an entry point to
// jump to. A zero entry would mean the server refused, which is a different
// failure from the one the device shows.
inline bool wineInitProcessDoneAdmitted(
    const WineInitProcessDoneReply& reply) noexcept {
    return reply.valid && reply.error == 0 && reply.entry != 0;
}

} // namespace boxedvn

#endif // __cplusplus

#endif
