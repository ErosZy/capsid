#ifndef CAPSID_HOST_WS_CODEC_H
#define CAPSID_HOST_WS_CODEC_H

// RFC 6455 WebSocket framing for the host data plane (FetchRPC v4,
// docs/fetchrpc-v4.md decision D1): the host terminates every
// WebSocket — answering upgrade handshakes, framing inbound client
// connections, and dialing outbound ones under the egress policy.
//
// The codec is deliberately self-contained (SHA-1 and base64 included,
// both covered by RFC test vectors in tests/test_ws_codec.cc) so the
// host gains no new link dependencies.
#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace capsid {
namespace host {
namespace ws {

enum Opcode : uint8_t {
    kContinuation = 0x0,
    kText = 0x1,
    kBinary = 0x2,
    kClose = 0x8,
    kPing = 0x9,
    kPong = 0xA,
};

enum Role {
    // The server receives masked frames and sends unmasked ones; the
    // client is the mirror image (RFC 6455 §5.1).
    kServer,
    kClient,
};

enum ParseResult {
    kParseNeedMore,
    kParseFrame,
    kParseError,
};

struct Frame {
    bool fin = true;
    uint8_t opcode = kText;
    std::vector<uint8_t> payload;
};

// RFC 6455 §1.3: SHA-1(client-key + GUID), base64-encoded.
std::string compute_accept_key(const std::string &client_key);

// A client key is 16 random bytes, base64-encoded (24 chars).
bool validate_client_key(const std::string &key);

// Encodes one frame. Client frames are masked with the supplied mask
// (the caller owns randomness); server frames must pass mask == 0.
// Control frames over 125 bytes and fragmented control frames are
// rejected (RFC 6455 §5.5).
bool encode(const Frame &frame,
            Role role,
            uint32_t mask,
            std::vector<uint8_t> *output);

class Parser {
public:
    explicit Parser(Role role);

    bool append(const uint8_t *data, size_t size);
    ParseResult next(Frame *frame);
    const std::string &error() const { return error_; }

private:
    Role role_;
    std::vector<uint8_t> buffer_;
    size_t offset_;
    std::string error_;
};

}  // namespace ws
}  // namespace host
}  // namespace capsid

#endif
