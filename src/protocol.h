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
static const size_t kHeaderSize = 24;
static const size_t kHelloLegacyFixedPayloadSize = 106;
static const size_t kHelloFixedPayloadSize = 108;
static const uint32_t kMaxPayloadSize = 64u * 1024u;
static const size_t kMaxBufferedBytes = 4u * 1024u * 1024u;
static const uint32_t kFlagStart = 1u;
static const uint32_t kFlagEnd = 2u;
static const uint32_t kFlagBundleName = 4u;
static const uint32_t kFlagTrustedBytecode = 8u;
// RequestHead flag: request has no body (GET/HEAD). The worker skips the
// initial request-direction credit and marks request_ended immediately,
// saving one frame per bodyless request.
static const uint32_t kFlagRequestEnd = 16u;
static const uint32_t kErrorFlagTimeout = 1u;
static const uint32_t kReadySandboxFeatureMask = (1u << 10) - 1u;

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
    kMemoryMetricsResponse = 17
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
    Parser();

    bool append(const uint8_t *data, size_t size);
    ParseResult next(Frame *frame);
    ParseResult next(Frame *frame, std::vector<uint8_t> *payload);
    // Returns a payload view into parser-owned storage. The view remains
    // valid until the next non-const Parser call. This lets synchronous
    // consumers take their one required ownership snapshot directly,
    // without first copying into an intermediate vector.
    ParseResult next_view(Frame *frame,
                          const uint8_t **payload_data,
                          size_t *payload_size_out);
    const std::string &error() const { return error_; }

private:
    void release_view();

    std::vector<uint8_t> buffer_;
    size_t offset_;
    bool view_active_;
    std::string error_;
};

bool encode(const Frame &frame, std::vector<uint8_t> *output);
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
