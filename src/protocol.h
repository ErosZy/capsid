#ifndef CAPSID_PROTOCOL_H
#define CAPSID_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <utility>
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
// Bootstrap mode for the worker side (amended §1.2): the first frame
// may be version 3 or 4 and locks the connection to that version;
// every later frame must match. Steady-state peers never mix.
static const uint16_t kVersionNegotiate = 0;
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
// Pre-A2 draft flag; A2 OPEN is worker->host and requires flags == 0.
static const uint32_t kFlagChanOutbound = 1u;
// kChannelData: message payload is text (workerd WebSocket semantics).
static const uint32_t kFlagChanText = 4u;
// kRequestHead (v4): the request carries an accepted-pending Upgrade.
static const uint32_t kFlagRequestUpgrade = 32u;
// kResponseHead (v4): status 101 accept; payload carries channel_id.
static const uint32_t kFlagResponseUpgrade = 32u;
// Hello v4 appends a capability tail to the (variable-length) v3
// startup-config payload: u32 capabilities + u32 reserved. Validation
// reads the last 8 bytes.
static const size_t kHelloV4TailSize = 8;
static const uint32_t kCapDuplexChannel = 1u;
static const uint32_t kCapUpgrade = 2u;
static const uint32_t kCapHalfClose = 4u;
// A2 amendment (v4.1): channel payloads §3.6 of the contract; every
// channel feature requires it, and announcing it requires
// CAP_DUPLEX_CHANNEL.
static const uint32_t kCapChannelLayoutA2 = 8u;
// v4.2: result and terminal acknowledgements make the two independent
// IPC directions safe to cancel. Requires DUPLEX + A2.
static const uint32_t kCapChannelConfirm = 16u;
// Atomic WebSocket Close followed by transport termination. Requires CONFIRM.
static const uint32_t kCapChannelCloseFinal = 32u;
// Optional HTTP metadata for outbound Upgrade fetch. Legacy OPEN/ACCEPT
// payloads remain exact; the extension requires UPGRADE + CONFIRM + CLOSE_FINAL.
static const uint32_t kCapFetchUpgrade = 64u;
// Optional retained incoming I/O owners after their transport has retired.
// Requires UPGRADE + CONFIRM; legacy peers never receive lifetime frames.
static const uint32_t kCapConnectionLifetime = 128u;
static const uint32_t kCapabilitiesKnownMask =
    kCapDuplexChannel | kCapUpgrade | kCapHalfClose | kCapChannelLayoutA2 |
    kCapChannelConfirm | kCapChannelCloseFinal | kCapFetchUpgrade | kCapConnectionLifetime;
// true iff the bitset satisfies the A2 dependency closure (§1.2).
bool capabilities_satisfy_closure(uint32_t capabilities);

// ---- A2 limits extension (contract §1.2.1) ----
// extension_size field value AND the bytes it counts (version..
// per_channel_queue_bytes); the size field itself adds 4, so the
// extension block totals kHelloA2ExtensionTotalSize.
static const uint32_t kHelloA2ExtensionBodySize = 48;
static const size_t kHelloA2ExtensionTotalSize = 52;
static const uint16_t kHelloA2ExtensionVersion = 1;
struct ChannelLimits {
    // Deployment defaults (implementation design §5); spawn options
    // and the Hello extension override within the frozen bounds.
    uint32_t max_channels = 128;
    uint64_t max_message_bytes = 1024u * 1024u;
    uint64_t reassembly_budget_bytes = 16u * 1024u * 1024u;
    uint64_t send_budget_bytes = 16u * 1024u * 1024u;
    uint32_t close_timeout_ms = 5000;
    uint32_t dial_timeout_ms = 10000;
    uint32_t idle_timeout_ms = 0;
    uint32_t per_channel_queue_bytes = 4u * 1024u * 1024u;
};
// Frozen bounds; any violation is "invalid hello extension".
bool validate_channel_limits(const ChannelLimits &limits);

// ---- A2 channel payloads (contract §3.2) ----
static const size_t kMaxUrlBytes = 2048;
static const size_t kMaxProtocolBytes = 256;
static const size_t kMaxProtocolCount = 64;
static const size_t kMaxDiagnosticBytes = 256;
static const size_t kMaxCloseReasonBytes = 123;
static const uint64_t kMinMessageBytes = 1024;
static const uint64_t kMaxMessageBytes = 64u * 1024u * 1024u;
static const uint64_t kInitialChannelCredit = 65536;

enum ChannelOwnerKind {
    kOwnerHandshake = 0,
    kOwnerDuplexConnection = 1,
    kOwnerRequest = 2,
    kOwnerResponseStream = 3,
    kOwnerStandalone = 4,
    kOwnerTailTask = 5
};
enum ChannelKindCode {
    kChannelKindWebSocket = 0,
    kChannelKindFetchUpgrade = 1,
    // Registry-only result of a non-101 Upgrade fetch. Never legal in OPEN.
    kChannelKindHttpBody = 2
};

static const size_t kMaxChannelHttpHeaders = 128;
static const size_t kMaxChannelHttpHeaderBytes = 48u * 1024u;
static const size_t kMaxChannelHttpStatusTextBytes = 1024;
enum FetchUpgradeRedirect { kFetchRedirectFollow = 0, kFetchRedirectManual = 1, kFetchRedirectError = 2 };
// Header names are HTTP tokens; values are raw ByteString octets. Preserve
// order and duplicates, including separate Set-Cookie fields. HTTP bodies
// travel in credit-controlled DATA after ACCEPT, never in this metadata.
struct FetchUpgradeRequest {
    uint8_t redirect = kFetchRedirectFollow;
    std::vector<std::pair<std::string, std::string>> headers;
};
struct FetchUpgradeResponse {
    uint16_t status = 0;
    bool redirected = false;
    std::string status_text;
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
};
enum ChannelCloseKind {
    kCloseInitiated = 0,
    kCloseObserved = 1,
    kCloseTransport = 2,
    kCloseFinal = 3
};
static const uint8_t kCloseFlagCodePresent = 1;
enum ChannelCancelReason {
    kCancelLocal = 0,
    kCancelDisconnect = 1,
    kCancelTimeout = 2,
    kCancelShutdown = 3,
    kCancelResourceLimits = 4,
    kCancelProtocolViolation = 5
};

struct ChannelOpenPayload {
    uint32_t channel_id = 0;    // 0: dial request
    uint64_t dial_id = 0;       // nonzero for dials
    uint8_t owner_kind = 0;
    uint8_t channel_kind = 0;
    uint64_t owner_request_id = 0;
    uint64_t max_message_bytes = 0;
    std::string url;
    std::vector<std::string> protocols;
    FetchUpgradeRequest http_request;
};
struct ChannelAcceptPayload {
    uint64_t dial_id = 0;
    uint32_t channel_id = 0;    // assigned, nonzero
    uint64_t accepted_max_message_bytes = 0;
    std::string selected_protocol;    // may be empty
    bool has_http_response = false;
    FetchUpgradeResponse http_response;
};
struct ChannelRejectPayload {
    uint64_t dial_id = 0;
    uint32_t channel_id = 0;    // 0 until assigned
    uint32_t reason_code = 0;
    std::string diagnostic;
};
struct ChannelCreditPayload {
    uint32_t channel_id = 0;
    uint64_t credit = 0;
};
struct ChannelClosePayload {
    uint32_t channel_id = 0;
    uint8_t close_kind = 0;
    uint8_t flags = 0;
    uint16_t code = 0;
    std::string reason;
};
struct ChannelCancelPayload {
    uint32_t channel_id = 0;
    uint32_t terminal_reason = 0;
    // Only the negotiated 16-byte pending-dial form carries this field.
    // Assigned-channel CANCEL keeps its original 8-byte encoding.
    uint64_t dial_id = 0;
};
enum ChannelAckKind {
    kAckDialResult = 0,
    kAckCancel = 1,
    kAckTransportClose = 2
};
struct ChannelAckPayload {
    uint32_t channel_id = 0;
    uint64_t dial_id = 0;
    uint32_t kind = kAckDialResult;
};
struct ConnectionLifetimePayload {
    uint32_t channel_id = 0;
    uint32_t retained = 0;
};
// Upgrade tails ride RequestHead/ResponseHead payloads (contract §2).
struct UpgradeTail {
    uint32_t channel_id = 0;
    uint64_t max_message_bytes = 0;
};

// Encode/decode pairs. Decoders are strict: any grammar violation
// (contract §3.2 bounds, §3.6 close matrix) returns false and sets
// *error to the exact contract string.
bool encode_channel_open(const ChannelOpenPayload &in,
                         std::vector<uint8_t> *out, uint32_t capabilities = 0);
bool decode_channel_open(const uint8_t *data, size_t size,
                         ChannelOpenPayload *out, std::string *error, uint32_t capabilities = 0);
bool encode_channel_accept(const ChannelAcceptPayload &in,
                           std::vector<uint8_t> *out, uint32_t capabilities = 0);
bool decode_channel_accept(const uint8_t *data, size_t size,
                           ChannelAcceptPayload *out, std::string *error, uint32_t capabilities = 0);
bool encode_channel_reject(const ChannelRejectPayload &in,
                           std::vector<uint8_t> *out);
bool decode_channel_reject(const uint8_t *data, size_t size,
                           ChannelRejectPayload *out, std::string *error);
bool encode_channel_credit(const ChannelCreditPayload &in,
                           std::vector<uint8_t> *out);
bool decode_channel_credit(const uint8_t *data, size_t size,
                           ChannelCreditPayload *out, std::string *error);
bool encode_channel_close(const ChannelClosePayload &in,
                          std::vector<uint8_t> *out);
bool decode_channel_close(const uint8_t *data, size_t size,
                          ChannelClosePayload *out, std::string *error);
bool encode_channel_cancel(const ChannelCancelPayload &in,
                           std::vector<uint8_t> *out);
bool decode_channel_cancel(const uint8_t *data, size_t size,
                           ChannelCancelPayload *out, std::string *error);
bool encode_channel_ack(const ChannelAckPayload &in, std::vector<uint8_t> *out);
bool decode_channel_ack(const uint8_t *data, size_t size,
                        ChannelAckPayload *out, std::string *error);
bool encode_connection_lifetime(const ConnectionLifetimePayload &in, std::vector<uint8_t> *out);
bool decode_connection_lifetime(const uint8_t *data, size_t size,
                                ConnectionLifetimePayload *out, std::string *error);
// APPEND semantics: the tail follows existing payload bytes (e.g. a
// ResponseHead status prefix); start from an empty vector for a
// standalone tail.
bool encode_upgrade_tail(const UpgradeTail &in,
                         std::vector<uint8_t> *out);
bool decode_upgrade_tail(const uint8_t *data, size_t size,
                         UpgradeTail *out, std::string *error);
// DATA prefix: first payload bytes are the u32 channel id.
bool decode_channel_data_id(const uint8_t *data, size_t size,
                            uint32_t *channel_id, std::string *error);
// A2 limits extension append/decode relative to a composed Hello
// payload (startup bytes already present; tail appended after).
bool append_hello_a2_extension(std::vector<uint8_t> *payload,
                               const ChannelLimits &limits);
bool decode_hello_a2_extension(const uint8_t *data, size_t size,
                               ChannelLimits *out, std::string *error);
// RFC 6455 §7.4.1 sendable close code set shared by kinds 0/1.
bool is_sendable_close_code(uint16_t code);

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
    kChannelCancel = 26,
    kChannelAck = 27,
    kConnectionLifetime = 28
};

struct Frame {
    uint16_t type = 0;
    uint32_t flags = 0;
    uint64_t request_id = 0;
    // Wire version this frame arrived with (filled by Parser::next;
    // zero from encode-only callers).
    uint16_t version = 0;
    // Explicit zero count keeps Debug STL allocation throwable and the
    // frame aggregate empty, including builds without guaranteed elision.
    std::vector<uint8_t> payload{0, std::allocator<uint8_t>{}};
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
// Append-semantics twin with an explicit wire version (v4 channel
// frames); preserves any prefix already in output.
bool append_encoded_versioned(uint16_t version,
                              uint16_t type,
                              uint32_t flags,
                              uint64_t request_id,
                              const uint8_t *payload,
                              size_t payload_size,
                              std::vector<uint8_t> *output);

// Direct little-endian loads at an address (bounds belong to the
// caller); used by tail readers that index from a frame payload end.
uint16_t load_u16_at(const uint8_t *input);
uint32_t load_u32_at(const uint8_t *input);
// Validate the complete v3 head prefix before reading the appended A2
// tail. This prevents tail-only fixtures from standing in for real heads.
bool decode_upgrade_head(const Frame &frame, UpgradeTail *tail,
                         std::string *error);
void append_u16(std::vector<uint8_t> *output, uint16_t value);
void append_u32(std::vector<uint8_t> *output, uint32_t value);
void append_u64(std::vector<uint8_t> *output, uint64_t value);
bool read_u16(const uint8_t **cursor, const uint8_t *end, uint16_t *value);
bool read_u32(const uint8_t **cursor, const uint8_t *end, uint32_t *value);
bool read_u64(const uint8_t **cursor, const uint8_t *end, uint64_t *value);

}  // namespace protocol
}  // namespace capsid

#endif
