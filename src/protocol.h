#ifndef CAPSID_PROTOCOL_H
#define CAPSID_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace capsid {
namespace protocol {

static const uint32_t kMagic = 0x43545257u;
static const uint16_t kVersion = 3;
// FetchRPC v4 (docs/fetchrpc-v4.md): adds duplex channels and Upgrade
// negotiation on top of the v3 vocabulary. A connection speaks exactly
// one version; the parser pins its expectation at construction and
// rejects every other version deterministically.
static const uint16_t kVersionV4 = 4;
static const size_t kHeaderSize = 24;
static const size_t kHelloLegacyFixedPayloadSize = 106;
static const size_t kHelloFixedPayloadSize = 108;
static const uint32_t kMaxPayloadSize = 64u * 1024u;
static const uint32_t kMaxFixedBodySize = 4u * 1024u;
static const size_t kMaxBufferedBytes = 4u * 1024u * 1024u;
static const uint32_t kFlagStart = 1u;
static const uint32_t kFlagEnd = 2u;
static const uint32_t kFlagBundleName = 4u;
static const uint32_t kFlagTrustedBytecode = 8u;
// RequestHead flag: request has no body (GET/HEAD). The worker skips the
// initial request-direction credit and marks request_ended immediately,
// saving one frame per bodyless request.
static const uint32_t kFlagRequestEnd = 16u;
// ResponseHead flag: js_response_final knows a small complete body already
// covered by the request's response credit and records its exact byte length
// in the four bytes after the status code. Hosts reserve at most
// kMaxFixedBodySize and use one fixed-length HTTP write; larger, credit-bound,
// or streamed responses keep flags == 0 and the original pipelined sequence.
static const uint32_t kFlagResponseFixedBody = 1u;
// LOG flag: payload uses the authenticated Binding-log v1 envelope. User
// JavaScript cannot choose frame flags, so it cannot forge Binding metadata.
static const uint32_t kFlagBindingLog = 1u;
static const uint32_t kErrorFlagTimeout = 1u;
static const uint32_t kReadySandboxFeatureMask = (1u << 10) - 1u;
// ---- v4 flags (flag namespaces stay per frame type) ----
// kChannelOpen: worker->host outbound dial (host-assigned ids otherwise).
static const uint32_t kFlagChanOutbound = 1u;
// kChannelData: message payload is text (workerd WebSocket semantics).
static const uint32_t kFlagChanText = 4u;
// kRequestHead (v4): the request carries an accepted-pending Upgrade.
static const uint32_t kFlagRequestUpgrade = 32u;
// kResponseHead (v4): status 101 accept; payload carries channel_id.
static const uint32_t kFlagResponseUpgrade = 32u;
// Hello v4 fixed payload: u64 compat-id length (71), 71 id bytes,
// u32 capability bitset, u32 reserved (must be 0).
static const size_t kHelloV4FixedPayloadSize = 87;
static const uint32_t kCapDuplexChannel = 1u;
static const uint32_t kCapUpgrade = 2u;
static const uint32_t kCapHalfClose = 4u;
static const uint32_t kCapabilitiesKnownMask =
    kCapDuplexChannel | kCapUpgrade | kCapHalfClose;

enum FrameType {
    kHello = 1,
    kLoadBundle = 2,
    kReady = 3,
    kRequestHead = 4,
    kRequestBody = 5,
    kRequestEnd = 6,
    kResponseHead = 7,
    kResponseBody = 8,
    kResponseEnd = 9,
    kWindowUpdate = 10,
    kCancel = 11,
    kLog = 12,
    kError = 13,
    kShutdown = 14,
    kAudit = 15,
    kMemoryMetricsRequest = 16,
    kMemoryMetricsResponse = 17,
    // Binding v1 (§6): descriptor + source blob chunks between HELLO and
    // LOAD_BUNDLE. Chunking mirrors kLoadBundle (start/end flags).
    kLoadBinding = 18,
    // ---- FetchRPC v4 duplex channels (docs/fetchrpc-v4.md §3) ----
    // Host-assigned u32 channel ids; independent per-direction byte
    // credits; ownership fixed at open drives cleanup.
    kChannelOpen = 19,
    kChannelAccept = 20,
    kChannelReject = 21,
    kChannelData = 22,
    kChannelCredit = 23,
    kChannelHalfClose = 24,
    kChannelClose = 25,
    kChannelCancel = 26
};

struct Frame {
    uint16_t type;
    uint32_t flags;
    uint64_t request_id;
    std::vector<uint8_t> payload;
};

enum ParseResult {
    kParseNeedMore,
    kParseFrame,
    kParseError
};

class Parser {
public:
    // expected_version pins the wire version this parser accepts; the
    // default keeps the shipped v3 behavior byte-identical.
    explicit Parser(uint16_t expected_version = kVersion);

    bool append(const uint8_t *data, size_t size);
    ParseResult next(Frame *frame);
    ParseResult next(Frame *frame, std::vector<uint8_t> *payload);
    const std::string &error() const { return error_; }

private:
    std::vector<uint8_t> buffer_;
    size_t offset_;
    std::string error_;
    uint16_t expected_version_;
};

bool encode(const Frame &frame, std::vector<uint8_t> *output);
// Encodes with an explicit wire version (v4 channel frames). Type and
// flag validity are checked against that version.
bool encode_versioned(uint16_t version,
                      const Frame &frame,
                      std::vector<uint8_t> *output);
bool append_encoded(uint16_t type,
                    uint32_t flags,
                    uint64_t request_id,
                    const uint8_t *payload,
                    size_t payload_size,
                    std::vector<uint8_t> *output);

void append_u16(std::vector<uint8_t> *output, uint16_t value);
void append_u32(std::vector<uint8_t> *output, uint32_t value);
void append_u64(std::vector<uint8_t> *output, uint64_t value);
bool read_u16(const uint8_t **cursor, const uint8_t *end, uint16_t *value);
bool read_u32(const uint8_t **cursor, const uint8_t *end, uint32_t *value);
bool read_u64(const uint8_t **cursor, const uint8_t *end, uint64_t *value);

}  // namespace protocol
}  // namespace capsid

#endif
