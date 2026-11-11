#include "protocol.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace capsid {
namespace protocol {

namespace {

uint16_t load_u16(const uint8_t *input) {
    return static_cast<uint16_t>(input[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(input[1]) << 8);
}

uint32_t load_u32(const uint8_t *input) {
    return static_cast<uint32_t>(input[0]) |
           (static_cast<uint32_t>(input[1]) << 8) |
           (static_cast<uint32_t>(input[2]) << 16) |
           (static_cast<uint32_t>(input[3]) << 24);
}

uint64_t load_u64(const uint8_t *input) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(input[i]) << (i * 8);
    }
    return value;
}

bool valid_type(uint16_t type) {
    return type >= kHello && type <= kLoadBinding;
}

bool valid_type_versioned(uint16_t type, uint16_t version) {
    if (version == kVersionV4) {
        return type >= kHello && type <= kConnectionLifetime;
    }
    return valid_type(type);
}

// Hello v4 payload validation (docs/fetchrpc-v4.md §1.2, amended): the
// v3 startup-config payload (variable length) plus an 8-byte
// capability tail; only the tail is validated here - known capability
// bits only, reserved word zero.
bool valid_hello_v4_payload(const std::vector<uint8_t> &payload) {
    if (payload.size() < kHelloFixedPayloadSize + kHelloV4TailSize) {
        return false;
    }
    const uint8_t *cursor =
        payload.data() + payload.size() - kHelloV4TailSize;
    const uint8_t *end = payload.data() + payload.size();
    uint32_t capabilities = 0;
    uint32_t reserved = 0;
    if (!read_u32(&cursor, end, &capabilities) ||
        !read_u32(&cursor, end, &reserved)) {
        return false;
    }
    return (capabilities & ~kCapabilitiesKnownMask) == 0 && reserved == 0;
}

bool valid_flags(uint16_t type, uint32_t flags) {
    if (type == kLoadBundle) {
        return (flags & ~(kFlagStart |
                          kFlagEnd |
                          kFlagBundleName |
                          kFlagTrustedBytecode)) == 0 &&
               ((flags & kFlagBundleName) == 0 ||
                (flags & kFlagStart) != 0) &&
               ((flags & kFlagTrustedBytecode) == 0 ||
                (flags & kFlagStart) != 0);
    }
    if (type == kLoadBinding) {
        // The binding blob is opaque: only the chunk start/end flags apply.
        return (flags & ~(kFlagStart | kFlagEnd)) == 0;
    }
    if (type == kRequestHead) {
        // kFlagRequestEnd: request has no body.
        return (flags & ~kFlagRequestEnd) == 0;
    }
    if (type == kResponseHead) {
        return (flags & ~kFlagResponseFixedBody) == 0;
    }
    if (type == kError) {
        return (flags & ~kErrorFlagTimeout) == 0;
    }
    if (type == kReady) {
        return (flags & ~kReadySandboxFeatureMask) == 0;
    }
    if (type == kLog) {
        return (flags & ~kFlagBindingLog) == 0;
    }
    return flags == 0;
}

// v4 flag rules (docs/fetchrpc-v4.md §2-3): Upgrade flags ride the
// existing head frames, channel frames carry their own namespace.
bool valid_flags_versioned(uint16_t type, uint32_t flags, uint16_t version) {
    if (version != kVersionV4) {
        return valid_flags(type, flags);
    }
    switch (type) {
        case kRequestHead:
            return (flags & ~(kFlagRequestEnd | kFlagRequestUpgrade)) == 0;
        case kResponseHead:
            return (flags &
                    ~(kFlagResponseFixedBody | kFlagResponseUpgrade)) == 0;
        case kChannelOpen:
            return flags == 0;
        case kChannelData:
            return (flags & ~(kFlagStart | kFlagEnd | kFlagChanText)) == 0;
        default:
            return valid_flags(type, flags);
    }
}

}  // namespace

// Debug STL proxy allocation must propagate to the caller's error guard.
Parser::Parser(uint16_t expected_version)
    : buffer_(0), offset_(0), error_(""), expected_version_(expected_version) {}

bool Parser::append(const uint8_t *data, size_t size) {
    if (!error_.empty()) {
        return false;
    }
    if (size == 0) {
        return true;
    }
    if (!data) {
        error_ = "invalid parser input";
        return false;
    }
    const size_t buffered = buffer_.size() - offset_;
    if (size > kMaxBufferedBytes - buffered) {
        error_ = "frame buffer limit exceeded";
        return false;
    }
    if (offset_ != 0 &&
        (buffer_.size() + size > kMaxBufferedBytes ||
         offset_ >= buffer_.size() / 2)) {
        buffer_.erase(
            buffer_.begin(),
            buffer_.begin() + static_cast<ptrdiff_t>(offset_));
        offset_ = 0;
    }
    buffer_.insert(buffer_.end(), data, data + size);
    return true;
}

ParseResult Parser::next(Frame *frame) {
    return next(frame, frame ? &frame->payload : NULL);
}

ParseResult Parser::next(Frame *frame, std::vector<uint8_t> *payload) {
    if (!frame || !payload || !error_.empty()) {
        return kParseError;
    }
    const size_t buffered = buffer_.size() - offset_;
    if (buffered < kHeaderSize) {
        return kParseNeedMore;
    }

    const uint8_t *header = &buffer_[offset_];
    if (load_u32(header) != kMagic) {
        error_ = "invalid frame magic";
        return kParseError;
    }
    const uint16_t wire_version = load_u16(header + 4);
    if (expected_version_ == kVersionNegotiate) {
        // Bootstrap (amended §1.2): the first frame locks the
        // connection's version; later frames must match.
        if (wire_version != kVersion && wire_version != kVersionV4) {
            error_ = "unsupported protocol version";
            return kParseError;
        }
        expected_version_ = wire_version;
    } else if (wire_version != expected_version_) {
        error_ = "unsupported protocol version";
        return kParseError;
    }

    const uint16_t type = load_u16(header + 6);
    if (!valid_type_versioned(type, expected_version_)) {
        error_ = "invalid frame type";
        return kParseError;
    }
    const uint32_t flags = load_u32(header + 8);
    if (!valid_flags_versioned(type, flags, expected_version_)) {
        error_ = "invalid frame flags";
        return kParseError;
    }
    if (expected_version_ == kVersionV4 && type >= kChannelOpen &&
            type <= kConnectionLifetime && load_u64(header + 12) != 0) {
        error_ = "invalid channel payload";
        return kParseError;
    }

    const uint32_t payload_size = load_u32(header + 20);
    if (payload_size > kMaxPayloadSize) {
        error_ = "frame payload limit exceeded";
        return kParseError;
    }
    const size_t frame_size = kHeaderSize + payload_size;
    if (buffered < frame_size) {
        return kParseNeedMore;
    }

    frame->type = type;
    frame->flags = flags;
    frame->request_id = load_u64(header + 12);
    frame->version = expected_version_;
    payload->assign(
        buffer_.begin() + static_cast<ptrdiff_t>(offset_ + kHeaderSize),
        buffer_.begin() + static_cast<ptrdiff_t>(offset_ + frame_size));
    if (payload != &frame->payload) {
        frame->payload.clear();
    }
    if (expected_version_ == kVersionV4 && type == kHello &&
        !valid_hello_v4_payload(*payload)) {
        error_ = "invalid hello capabilities";
        return kParseError;
    }
    offset_ += frame_size;
    if (offset_ == buffer_.size()) {
        buffer_.clear();
        offset_ = 0;
    }
    return kParseFrame;
}

bool encode(const Frame &frame, std::vector<uint8_t> *output) {
    if (!output) {
        return false;
    }
    output->clear();
    if (frame.payload.size() <= kMaxPayloadSize) {
        output->reserve(kHeaderSize + frame.payload.size());
    }
    return append_encoded(
        frame.type,
        frame.flags,
        frame.request_id,
        frame.payload.empty() ? NULL : &frame.payload[0],
        frame.payload.size(),
        output);
}

bool encode_versioned(uint16_t version,
                      const Frame &frame,
                      std::vector<uint8_t> *output) {
    if (!output || (version != kVersion && version != kVersionV4)) {
        return false;
    }
    if (!valid_type_versioned(frame.type, version) ||
        !valid_flags_versioned(frame.type, frame.flags, version)) {
        return false;
    }
    if (version == kVersionV4 && frame.type >= kChannelOpen &&
            frame.type <= kConnectionLifetime && frame.request_id != 0) return false;
    if (frame.payload.size() > kMaxPayloadSize ||
        (frame.payload.size() != 0 && frame.payload.empty())) {
        return false;
    }
    output->clear();
    output->reserve(kHeaderSize + frame.payload.size());
    append_u32(output, kMagic);
    append_u16(output, version);
    append_u16(output, frame.type);
    append_u32(output, frame.flags);
    append_u64(output, frame.request_id);
    append_u32(output, static_cast<uint32_t>(frame.payload.size()));
    if (!frame.payload.empty()) {
        output->insert(output->end(),
                       frame.payload.begin(),
                       frame.payload.end());
    }
    return true;
}

bool append_encoded(uint16_t type,
                    uint32_t flags,
                    uint64_t request_id,
                    const uint8_t *payload,
                    size_t payload_size,
                    std::vector<uint8_t> *output) {
    if (!output ||
        !valid_type(type) ||
        !valid_flags(type, flags) ||
        payload_size > kMaxPayloadSize ||
        (payload_size != 0 && !payload) ||
        output->size() >
            std::numeric_limits<size_t>::max() -
                kHeaderSize - payload_size) {
        return false;
    }
    append_u32(output, kMagic);
    append_u16(output, kVersion);
    append_u16(output, type);
    append_u32(output, flags);
    append_u64(output, request_id);
    append_u32(output, static_cast<uint32_t>(payload_size));
    if (payload_size != 0) {
        output->insert(output->end(), payload, payload + payload_size);
    }
    return true;
}

bool append_encoded_versioned(uint16_t version,
                              uint16_t type,
                              uint32_t flags,
                              uint64_t request_id,
                              const uint8_t *payload,
                              size_t payload_size,
                              std::vector<uint8_t> *output) {
    if (!output ||
        (version != kVersion && version != kVersionV4) ||
        !valid_type_versioned(type, version) ||
        !valid_flags_versioned(type, flags, version) ||
        payload_size > kMaxPayloadSize ||
        (payload_size != 0 && !payload) ||
        output->size() >
            std::numeric_limits<size_t>::max() -
                kHeaderSize - payload_size) {
        return false;
    }
    append_u32(output, kMagic);
    append_u16(output, version);
    append_u16(output, type);
    append_u32(output, flags);
    append_u64(output, request_id);
    append_u32(output, static_cast<uint32_t>(payload_size));
    if (payload_size != 0) {
        output->insert(
            output->end(), payload, payload + payload_size);
    }
    return true;
}

uint16_t load_u16_at(const uint8_t *input) {
    return load_u16(input);
}

uint32_t load_u32_at(const uint8_t *input) {
    return load_u32(input);
}

void append_u16(std::vector<uint8_t> *output, uint16_t value) {
    output->push_back(static_cast<uint8_t>(value));
    output->push_back(static_cast<uint8_t>(value >> 8));
}

void append_u32(std::vector<uint8_t> *output, uint32_t value) {
    output->push_back(static_cast<uint8_t>(value));
    output->push_back(static_cast<uint8_t>(value >> 8));
    output->push_back(static_cast<uint8_t>(value >> 16));
    output->push_back(static_cast<uint8_t>(value >> 24));
}

void append_u64(std::vector<uint8_t> *output, uint64_t value) {
    for (size_t i = 0; i < 8; ++i) {
        output->push_back(static_cast<uint8_t>(value >> (i * 8)));
    }
}

bool read_u16(const uint8_t **cursor, const uint8_t *end, uint16_t *value) {
    if (!cursor || !*cursor || !value || end < *cursor || end - *cursor < 2) {
        return false;
    }
    *value = load_u16(*cursor);
    *cursor += 2;
    return true;
}

bool read_u32(const uint8_t **cursor, const uint8_t *end, uint32_t *value) {
    if (!cursor || !*cursor || !value || end < *cursor || end - *cursor < 4) {
        return false;
    }
    *value = load_u32(*cursor);
    *cursor += 4;
    return true;
}

bool read_u64(const uint8_t **cursor, const uint8_t *end, uint64_t *value) {
    if (!cursor || !*cursor || !value || end < *cursor || end - *cursor < 8) {
        return false;
    }
    *value = load_u64(*cursor);
    *cursor += 8;
    return true;
}


// ---- A2 amendment (v4.1): docs/fetchrpc-v4.md sections 1.2.1, 3.2, 3.6 ----

bool capabilities_satisfy_closure(uint32_t capabilities) {
    if ((capabilities & ~kCapabilitiesKnownMask) != 0) {
        return false;
    }
    const bool duplex = (capabilities & kCapDuplexChannel) != 0;
    const bool upgrade = (capabilities & kCapUpgrade) != 0;
    const bool half_close = (capabilities & kCapHalfClose) != 0;
    const bool a2 = (capabilities & kCapChannelLayoutA2) != 0;
    if ((upgrade || half_close || (capabilities & kCapChannelConfirm)) && (!duplex || !a2)) {
        return false;
    }
    if ((capabilities & kCapChannelCloseFinal) && !(capabilities & kCapChannelConfirm)) {
        return false;
    }
    const uint32_t fetch_dependencies = kCapUpgrade | kCapChannelConfirm | kCapChannelCloseFinal;
    if ((capabilities & kCapFetchUpgrade) && (capabilities & fetch_dependencies) != fetch_dependencies) {
        return false;
    }
    const uint32_t lifetime_dependencies = kCapUpgrade | kCapChannelConfirm;
    if ((capabilities & kCapConnectionLifetime) &&
            (capabilities & lifetime_dependencies) != lifetime_dependencies) return false;
    // DUPLEX requires A2; A2 without DUPLEX is equally malformed.
    if (duplex != a2) {
        return false;
    }
    return true;
}

namespace {

bool reject_payload(std::string *error, const char *message) {
    if (error) {
        *error = message;
    }
    return false;
}

bool read_byte(const uint8_t **cursor, const uint8_t *end, uint8_t *value) {
    if (!cursor || !*cursor || end < *cursor || end - *cursor < 1) {
        return false;
    }
    *value = **cursor;
    ++*cursor;
    return true;
}

bool read_string_u16(const uint8_t **cursor, const uint8_t *end,
                     size_t max_bytes, std::string *out) {
    uint16_t size = 0;
    if (!read_u16(cursor, end, &size) ||
            static_cast<size_t>(size) > max_bytes ||
            static_cast<size_t>(end - *cursor) < size) {
        return false;
    }
    out->assign(reinterpret_cast<const char *>(*cursor), size);
    *cursor += size;
    return true;
}

void append_string_u16(std::vector<uint8_t> *out, const std::string &value) {
    append_u16(out, static_cast<uint16_t>(value.size()));
    out->insert(out->end(), value.begin(), value.end());
}

bool fetch_upgrade_metadata_allowed(uint32_t capabilities) {
    return (capabilities & kCapFetchUpgrade) && capabilities_satisfy_closure(capabilities);
}

bool http_token(const std::string &value) {
    if (value.empty()) return false;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) continue;
        if (c && std::strchr("!#$%&'*+-.^_`|~", c)) continue;
        return false;
    }
    return true;
}

bool http_field_value(const std::string &value) {
    for (unsigned char c : value) {
        if ((c < 0x20 && c != '\t') || c == 0x7f) return false;
    }
    return true;
}

bool valid_http_headers(const std::vector<std::pair<std::string, std::string>> &headers) {
    if (headers.size() > kMaxChannelHttpHeaders) return false;
    size_t bytes = 2;
    for (const auto &header : headers) {
        if (!http_token(header.first) || !http_field_value(header.second) ||
                header.first.size() > UINT16_MAX || header.second.size() > UINT16_MAX) return false;
        const size_t charge = 4 + header.first.size() + header.second.size();
        if (charge > kMaxChannelHttpHeaderBytes - bytes) return false;
        bytes += charge;
    }
    return true;
}

void append_http_headers(std::vector<uint8_t> *out,
                         const std::vector<std::pair<std::string, std::string>> &headers) {
    append_u16(out, static_cast<uint16_t>(headers.size()));
    for (const auto &header : headers) {
        append_string_u16(out, header.first);
        append_string_u16(out, header.second);
    }
}

bool read_http_headers(const uint8_t **cursor, const uint8_t *end,
                       std::vector<std::pair<std::string, std::string>> *headers) {
    const uint8_t *begin = *cursor;
    uint16_t count = 0;
    if (!read_u16(cursor, end, &count) || count > kMaxChannelHttpHeaders) return false;
    headers->clear();
    for (uint16_t i = 0; i < count; ++i) {
        std::pair<std::string, std::string> field;
        if (!read_string_u16(cursor, end, kMaxChannelHttpHeaderBytes, &field.first) ||
                !read_string_u16(cursor, end, kMaxChannelHttpHeaderBytes, &field.second) ||
                static_cast<size_t>(*cursor - begin) > kMaxChannelHttpHeaderBytes ||
                !http_token(field.first) || !http_field_value(field.second)) return false;
        headers->push_back(std::move(field));
    }
    return true;
}

bool valid_http_response(const FetchUpgradeResponse &response) {
    return (response.status == 101 || (response.status >= 200 && response.status <= 599)) &&
        response.status_text.size() <= kMaxChannelHttpStatusTextBytes && http_field_value(response.status_text) &&
        !response.url.empty() && response.url.size() <= kMaxUrlBytes && http_field_value(response.url) &&
        valid_http_headers(response.headers);
}

}  // namespace

bool validate_channel_limits(const ChannelLimits &limits) {
    if (limits.max_channels < 1 || limits.max_channels > 1024) {
        return false;
    }
    if (limits.max_message_bytes < kMinMessageBytes ||
            limits.max_message_bytes > kMaxMessageBytes) {
        return false;
    }
    const uint64_t k1MiB = 1024u * 1024u;
    const uint64_t k256MiB = 256u * 1024u * 1024u;
    if (limits.reassembly_budget_bytes < k1MiB ||
            limits.reassembly_budget_bytes > k256MiB ||
            limits.send_budget_bytes < k1MiB ||
            limits.send_budget_bytes > k256MiB) {
        return false;
    }
    if (limits.close_timeout_ms < 100 || limits.close_timeout_ms > 600000 ||
            limits.dial_timeout_ms < 100 || limits.dial_timeout_ms > 600000 ||
            limits.idle_timeout_ms > 86400000u) {
        return false;
    }
    if (limits.per_channel_queue_bytes < kInitialChannelCredit ||
            limits.per_channel_queue_bytes > kMaxBufferedBytes) {
        return false;
    }
    return true;
}

bool append_hello_a2_extension(std::vector<uint8_t> *payload,
                               const ChannelLimits &limits) {
    if (!payload || !validate_channel_limits(limits)) {
        return false;
    }
    const size_t before = payload->size();
    append_u32(payload, kHelloA2ExtensionBodySize);
    append_u16(payload, kHelloA2ExtensionVersion);
    append_u16(payload, 0);
    append_u32(payload, limits.max_channels);
    append_u64(payload, limits.max_message_bytes);
    append_u64(payload, limits.reassembly_budget_bytes);
    append_u64(payload, limits.send_budget_bytes);
    append_u32(payload, limits.close_timeout_ms);
    append_u32(payload, limits.dial_timeout_ms);
    append_u32(payload, limits.idle_timeout_ms);
    append_u32(payload, limits.per_channel_queue_bytes);
    return payload->size() - before == kHelloA2ExtensionTotalSize;
}

bool decode_hello_a2_extension(const uint8_t *data, size_t size,
                               ChannelLimits *out, std::string *error) {
    if (!data || !out) {
        return reject_payload(error, "invalid hello extension");
    }
    const uint8_t *cursor = data;
    const uint8_t *end = data + size;
    uint32_t extension_size = 0;
    uint16_t extension_version = 0;
    uint16_t reserved = 0;
    if (!read_u32(&cursor, end, &extension_size) ||
            extension_size != kHelloA2ExtensionBodySize ||
            !read_u16(&cursor, end, &extension_version) ||
            extension_version != kHelloA2ExtensionVersion ||
            !read_u16(&cursor, end, &reserved) || reserved != 0) {
        return reject_payload(error, "invalid hello extension");
    }
    if (!read_u32(&cursor, end, &out->max_channels) ||
            !read_u64(&cursor, end, &out->max_message_bytes) ||
            !read_u64(&cursor, end, &out->reassembly_budget_bytes) ||
            !read_u64(&cursor, end, &out->send_budget_bytes) ||
            !read_u32(&cursor, end, &out->close_timeout_ms) ||
            !read_u32(&cursor, end, &out->dial_timeout_ms) ||
            !read_u32(&cursor, end, &out->idle_timeout_ms) ||
            !read_u32(&cursor, end, &out->per_channel_queue_bytes) ||
            cursor != end || !validate_channel_limits(*out)) {
        return reject_payload(error, "invalid hello extension");
    }
    return true;
}

bool is_sendable_close_code(uint16_t code) {
    if (code < 1000 || code > 4999) {
        return false;
    }
    return code != 1004 && code != 1005 && code != 1006 && code != 1015;
}

bool encode_channel_open(const ChannelOpenPayload &in,
                         std::vector<uint8_t> *out, uint32_t capabilities) {
    if (!out || in.url.size() > kMaxUrlBytes ||
            in.protocols.size() > kMaxProtocolCount) {
        return false;
    }
    for (size_t i = 0; i < in.protocols.size(); ++i) {
        if (in.protocols[i].size() > kMaxProtocolBytes) {
            return false;
        }
    }
    if (in.max_message_bytes < kMinMessageBytes ||
            in.max_message_bytes > kMaxMessageBytes) {
        return false;
    }
    const bool fetch = in.channel_kind == kChannelKindFetchUpgrade;
    if (fetch ? (!fetch_upgrade_metadata_allowed(capabilities) || !in.protocols.empty() ||
                    in.http_request.redirect > kFetchRedirectError || !valid_http_headers(in.http_request.headers))
              : (in.channel_kind != kChannelKindWebSocket || !in.http_request.headers.empty() ||
                    in.http_request.redirect != kFetchRedirectFollow)) return false;
    out->clear();
    append_u32(out, in.channel_id);
    append_u64(out, in.dial_id);
    out->push_back(in.owner_kind);
    out->push_back(in.channel_kind);
    append_u16(out, 0);
    append_u64(out, in.owner_request_id);
    append_u64(out, in.max_message_bytes);
    append_u32(out, static_cast<uint32_t>(in.url.size()));
    out->insert(out->end(), in.url.begin(), in.url.end());
    append_u16(out, static_cast<uint16_t>(in.protocols.size()));
    for (size_t i = 0; i < in.protocols.size(); ++i) {
        append_string_u16(out, in.protocols[i]);
    }
    if (fetch) {
        append_u16(out, 1); // HTTP metadata version
        out->push_back(in.http_request.redirect);
        out->push_back(0); // reserved
        append_http_headers(out, in.http_request.headers);
    }
    return out->size() <= kMaxPayloadSize;
}

bool decode_channel_open(const uint8_t *data, size_t size,
                         ChannelOpenPayload *out, std::string *error, uint32_t capabilities) {
    if (!data || !out || size > kMaxPayloadSize) {
        return reject_payload(error, "invalid channel payload");
    }
    const uint8_t *cursor = data;
    const uint8_t *end = data + size;
    uint16_t reserved = 0;
    uint32_t url_size = 0;
    uint16_t protocol_count = 0;
    if (!read_u32(&cursor, end, &out->channel_id) ||
            !read_u64(&cursor, end, &out->dial_id) ||
            !read_byte(&cursor, end, &out->owner_kind) ||
            !read_byte(&cursor, end, &out->channel_kind) ||
            !read_u16(&cursor, end, &reserved) ||
            !read_u64(&cursor, end, &out->owner_request_id) ||
            !read_u64(&cursor, end, &out->max_message_bytes) ||
            !read_u32(&cursor, end, &url_size) ||
            url_size > kMaxUrlBytes ||
            static_cast<size_t>(end - cursor) < url_size) {
        return reject_payload(error, "invalid channel payload");
    }
    out->url.assign(reinterpret_cast<const char *>(cursor), url_size);
    cursor += url_size;
    if (!read_u16(&cursor, end, &protocol_count) ||
            protocol_count > kMaxProtocolCount) {
        return reject_payload(error, "invalid channel payload");
    }
    out->protocols.clear();
    for (uint16_t i = 0; i < protocol_count; ++i) {
        std::string protocol;
        if (!read_string_u16(&cursor, end, kMaxProtocolBytes, &protocol)) {
            return reject_payload(error, "invalid channel payload");
        }
        out->protocols.push_back(protocol);
    }
    out->http_request = FetchUpgradeRequest();
    if (out->channel_kind == kChannelKindFetchUpgrade) {
        uint16_t version = 0;
        uint8_t reserved_http = 0;
        if (!fetch_upgrade_metadata_allowed(capabilities) || !out->protocols.empty() ||
                !read_u16(&cursor, end, &version) || version != 1 ||
                !read_byte(&cursor, end, &out->http_request.redirect) || out->http_request.redirect > kFetchRedirectError ||
                !read_byte(&cursor, end, &reserved_http) || reserved_http != 0 ||
                !read_http_headers(&cursor, end, &out->http_request.headers)) {
            return reject_payload(error, "invalid channel payload");
        }
    }
    if (cursor != end || reserved != 0 ||
            out->owner_kind > kOwnerTailTask ||
            (out->channel_kind != kChannelKindWebSocket && out->channel_kind != kChannelKindFetchUpgrade) ||
            out->max_message_bytes < kMinMessageBytes ||
            out->max_message_bytes > kMaxMessageBytes) {
        return reject_payload(error, "invalid channel payload");
    }
    return true;
}

bool encode_channel_accept(const ChannelAcceptPayload &in,
                           std::vector<uint8_t> *out, uint32_t capabilities) {
    if (!out || in.selected_protocol.size() > kMaxProtocolBytes ||
            in.accepted_max_message_bytes < kMinMessageBytes ||
            in.accepted_max_message_bytes > kMaxMessageBytes) {
        return false;
    }
    if (in.has_http_response ? (!fetch_upgrade_metadata_allowed(capabilities) || !in.selected_protocol.empty() ||
            !valid_http_response(in.http_response))
        : (in.http_response.status != 0 || in.http_response.redirected || !in.http_response.status_text.empty() ||
            !in.http_response.url.empty() || !in.http_response.headers.empty())) return false;
    out->clear();
    append_u64(out, in.dial_id);
    append_u32(out, in.channel_id);
    append_u64(out, in.accepted_max_message_bytes);
    append_string_u16(out, in.selected_protocol);
    if (in.has_http_response) {
        append_u16(out, 1); // HTTP metadata version
        append_u16(out, in.http_response.status);
        out->push_back(in.http_response.redirected ? 1 : 0);
        out->push_back(0); // reserved
        append_string_u16(out, in.http_response.status_text);
        append_string_u16(out, in.http_response.url);
        append_http_headers(out, in.http_response.headers);
    }
    return out->size() <= kMaxPayloadSize;
}

bool decode_channel_accept(const uint8_t *data, size_t size,
                           ChannelAcceptPayload *out, std::string *error, uint32_t capabilities) {
    if (!data || !out || size > kMaxPayloadSize) {
        return reject_payload(error, "invalid channel payload");
    }
    const uint8_t *cursor = data;
    const uint8_t *end = data + size;
    if (!read_u64(&cursor, end, &out->dial_id) ||
            !read_u32(&cursor, end, &out->channel_id) ||
            !read_u64(&cursor, end, &out->accepted_max_message_bytes) ||
            !read_string_u16(&cursor, end, kMaxProtocolBytes,
                             &out->selected_protocol)) {
        return reject_payload(error, "invalid channel payload");
    }
    out->has_http_response = false;
    out->http_response = FetchUpgradeResponse();
    if (cursor != end) {
        uint16_t version = 0;
        uint8_t redirected = 0, reserved = 0;
        if (!fetch_upgrade_metadata_allowed(capabilities) || !out->selected_protocol.empty() ||
                !read_u16(&cursor, end, &version) || version != 1 ||
                !read_u16(&cursor, end, &out->http_response.status) ||
                !read_byte(&cursor, end, &redirected) || redirected > 1 ||
                !read_byte(&cursor, end, &reserved) || reserved != 0 ||
                !read_string_u16(&cursor, end, kMaxChannelHttpStatusTextBytes, &out->http_response.status_text) ||
                !read_string_u16(&cursor, end, kMaxUrlBytes, &out->http_response.url) ||
                !read_http_headers(&cursor, end, &out->http_response.headers)) {
            return reject_payload(error, "invalid channel payload");
        }
        out->http_response.redirected = redirected != 0;
        if (!valid_http_response(out->http_response)) return reject_payload(error, "invalid channel payload");
        out->has_http_response = true;
    }
    if (cursor != end || out->channel_id == 0 ||
            out->accepted_max_message_bytes < kMinMessageBytes ||
            out->accepted_max_message_bytes > kMaxMessageBytes) {
        return reject_payload(error, "invalid channel payload");
    }
    return true;
}

bool encode_channel_reject(const ChannelRejectPayload &in,
                           std::vector<uint8_t> *out) {
    if (!out || in.reason_code > 9 || in.diagnostic.size() > kMaxDiagnosticBytes) {
        return false;
    }
    out->clear();
    append_u64(out, in.dial_id);
    append_u32(out, in.channel_id);
    append_u32(out, in.reason_code);
    append_string_u16(out, in.diagnostic);
    return true;
}

bool decode_channel_reject(const uint8_t *data, size_t size,
                           ChannelRejectPayload *out, std::string *error) {
    if (!data || !out) {
        return reject_payload(error, "invalid channel payload");
    }
    const uint8_t *cursor = data;
    const uint8_t *end = data + size;
    if (!read_u64(&cursor, end, &out->dial_id) ||
            !read_u32(&cursor, end, &out->channel_id) ||
            !read_u32(&cursor, end, &out->reason_code) ||
            !read_string_u16(&cursor, end, kMaxDiagnosticBytes,
                             &out->diagnostic) ||
            cursor != end || out->reason_code > 9) {
        return reject_payload(error, "invalid channel payload");
    }
    return true;
}

bool encode_channel_credit(const ChannelCreditPayload &in,
                           std::vector<uint8_t> *out) {
    if (!out) {
        return false;
    }
    out->clear();
    append_u32(out, in.channel_id);
    append_u64(out, in.credit);
    return true;
}

bool decode_channel_credit(const uint8_t *data, size_t size,
                           ChannelCreditPayload *out, std::string *error) {
    if (!data || !out) {
        return reject_payload(error, "invalid channel payload");
    }
    const uint8_t *cursor = data;
    const uint8_t *end = data + size;
    if (!read_u32(&cursor, end, &out->channel_id) ||
            !read_u64(&cursor, end, &out->credit) || cursor != end) {
        return reject_payload(error, "invalid channel payload");
    }
    return true;
}

bool encode_channel_close(const ChannelClosePayload &in,
                          std::vector<uint8_t> *out) {
    if (!out || in.reason.size() > kMaxCloseReasonBytes) {
        return false;
    }
    const bool code_present = (in.flags & kCloseFlagCodePresent) != 0;
    if (!code_present && in.close_kind != kCloseTransport && !in.reason.empty()) return false;
    if ((in.flags & ~kCloseFlagCodePresent) != 0 ||
            (code_present && in.code == 0) ||
            (!code_present && in.code != 0) ||
            in.close_kind > kCloseFinal) {
        return false;
    }
    if (code_present) {
        if (in.close_kind == kCloseTransport) {
            if (in.code != 1006 && in.code != 1015) {
                return false;
            }
        } else if (!is_sendable_close_code(in.code)) {
            return false;
        }
    }
    out->clear();
    append_u32(out, in.channel_id);
    out->push_back(in.close_kind);
    out->push_back(in.flags);
    append_u16(out, in.code);
    append_u16(out, static_cast<uint16_t>(in.reason.size()));
    out->insert(out->end(), in.reason.begin(), in.reason.end());
    return true;
}

bool decode_channel_close(const uint8_t *data, size_t size,
                          ChannelClosePayload *out, std::string *error) {
    if (!data || !out) {
        return reject_payload(error, "invalid channel payload");
    }
    const uint8_t *cursor = data;
    const uint8_t *end = data + size;
    uint16_t reason_size = 0;
    if (!read_u32(&cursor, end, &out->channel_id) ||
            !read_byte(&cursor, end, &out->close_kind) ||
            !read_byte(&cursor, end, &out->flags) ||
            !read_u16(&cursor, end, &out->code) ||
            !read_u16(&cursor, end, &reason_size) ||
            reason_size > kMaxCloseReasonBytes ||
            static_cast<size_t>(end - cursor) < reason_size) {
        return reject_payload(error, "invalid channel payload");
    }
    out->reason.assign(reinterpret_cast<const char *>(cursor),
                       reason_size);
    cursor += reason_size;
    if (cursor != end) {
        return reject_payload(error, "invalid channel payload");
    }
    if ((out->flags & ~kCloseFlagCodePresent) != 0 ||
            out->close_kind > kCloseFinal) {
        return reject_payload(error, "invalid close code");
    }
    const bool code_present = (out->flags & kCloseFlagCodePresent) != 0;
    if ((code_present && out->code == 0) ||
            (!code_present && out->code != 0)) {
        return reject_payload(error, "invalid close code");
    }
    if (!code_present && out->close_kind != kCloseTransport && !out->reason.empty()) {
        return reject_payload(error, "invalid channel payload");
    }
    if (code_present) {
        if (out->close_kind == kCloseTransport) {
            if (out->code != 1006 && out->code != 1015) {
                return reject_payload(error, "invalid close code");
            }
        } else if (!is_sendable_close_code(out->code)) {
            return reject_payload(error, "invalid close code");
        }
    }
    return true;
}

bool encode_channel_cancel(const ChannelCancelPayload &in,
                           std::vector<uint8_t> *out) {
    if (!out || ((in.channel_id == 0) == (in.dial_id == 0)) ||
            in.terminal_reason > kCancelProtocolViolation) {
        return false;
    }
    out->clear();
    append_u32(out, in.channel_id);
    append_u32(out, in.terminal_reason);
    if (in.channel_id == 0) append_u64(out, in.dial_id);
    return true;
}

bool decode_channel_cancel(const uint8_t *data, size_t size,
                           ChannelCancelPayload *out, std::string *error) {
    if (!data || !out) {
        return reject_payload(error, "invalid channel payload");
    }
    const uint8_t *cursor = data;
    const uint8_t *end = data + size;
    out->dial_id = 0;
    if (!read_u32(&cursor, end, &out->channel_id) ||
            !read_u32(&cursor, end, &out->terminal_reason) ||
            (out->channel_id == 0 && !read_u64(&cursor, end, &out->dial_id)) ||
            ((out->channel_id == 0) == (out->dial_id == 0)) ||
            out->terminal_reason > kCancelProtocolViolation ||
            cursor != end) {
        return reject_payload(error, "invalid channel payload");
    }
    return true;
}

bool encode_channel_ack(const ChannelAckPayload &in, std::vector<uint8_t> *out) {
    if (!out || in.kind > kAckTransportClose ||
            (in.kind == kAckDialResult && in.dial_id == 0) ||
            (in.kind == kAckCancel && ((in.channel_id == 0) == (in.dial_id == 0))) ||
            (in.kind == kAckTransportClose && (in.channel_id == 0 || in.dial_id != 0)))
        return false;
    out->clear();
    append_u32(out, in.channel_id);
    append_u64(out, in.dial_id);
    append_u32(out, in.kind);
    return true;
}

bool decode_channel_ack(const uint8_t *data, size_t size,
                        ChannelAckPayload *out, std::string *error) {
    if (!data || !out || size != 16) return reject_payload(error, "invalid channel payload");
    const uint8_t *cursor = data;
    const uint8_t *end = data + size;
    if (!read_u32(&cursor, end, &out->channel_id) ||
            !read_u64(&cursor, end, &out->dial_id) ||
            !read_u32(&cursor, end, &out->kind) ||
            out->kind > kAckTransportClose ||
            (out->kind == kAckDialResult && out->dial_id == 0) ||
            (out->kind == kAckCancel && ((out->channel_id == 0) == (out->dial_id == 0))) ||
            (out->kind == kAckTransportClose && (out->channel_id == 0 || out->dial_id != 0)))
        return reject_payload(error, "invalid channel payload");
    return true;
}

bool encode_connection_lifetime(const ConnectionLifetimePayload &in, std::vector<uint8_t> *out) {
    if (!out || !in.channel_id || in.retained > 1) return false;
    out->clear();append_u32(out, in.channel_id);append_u32(out, in.retained);
    return true;
}

bool decode_connection_lifetime(const uint8_t *data, size_t size,
                                ConnectionLifetimePayload *out, std::string *error) {
    if (!data || !out || size != 8) return reject_payload(error, "invalid channel payload");
    const uint8_t *cursor=data;const uint8_t *end=data+size;
    if (!read_u32(&cursor,end,&out->channel_id) || !read_u32(&cursor,end,&out->retained) ||
            !out->channel_id || out->retained>1) return reject_payload(error, "invalid channel payload");
    return true;
}

bool encode_upgrade_tail(const UpgradeTail &in,
                         std::vector<uint8_t> *out) {
    // APPEND semantics: upgrade tails follow the status prefix on
    // ResponseHead (and the head fields on RequestHead); callers
    // composing standalone tails start from an empty vector.
    if (!out || in.max_message_bytes < kMinMessageBytes ||
            in.max_message_bytes > kMaxMessageBytes) {
        return false;
    }
    append_u32(out, in.channel_id);
    append_u64(out, in.max_message_bytes);
    return true;
}

bool decode_upgrade_tail(const uint8_t *data, size_t size,
                         UpgradeTail *out, std::string *error) {
    if (!data || !out) {
        return reject_payload(error, "invalid channel payload");
    }
    const uint8_t *cursor = data;
    const uint8_t *end = data + size;
    if (!read_u32(&cursor, end, &out->channel_id) ||
            !read_u64(&cursor, end, &out->max_message_bytes) ||
            cursor != end ||
            out->channel_id == 0 ||
            out->max_message_bytes < kMinMessageBytes ||
            out->max_message_bytes > kMaxMessageBytes) {
        return reject_payload(error, "invalid channel payload");
    }
    return true;
}

bool decode_channel_data_id(const uint8_t *data, size_t size,
                            uint32_t *channel_id, std::string *error) {
    if (!data || !channel_id || size < 4) {
        return reject_payload(error, "invalid channel payload");
    }
    const uint8_t *cursor = data;
    const uint8_t *end = data + size;
    if (!read_u32(&cursor, end, channel_id)) {
        return reject_payload(error, "invalid channel payload");
    }
    return true;
}

bool decode_upgrade_head(const Frame &frame, UpgradeTail *tail,
                         std::string *error) {
    if (frame.version != kVersionV4 || frame.request_id == 0 ||
            frame.payload.size() < 12) {
        return reject_payload(error, "invalid channel payload");
    }
    const uint8_t *cursor = frame.payload.data();
    const uint8_t *end = cursor + frame.payload.size() - 12;
    const auto skip16 = [&](bool nonempty) -> bool {
        uint16_t length = 0;
        if (!read_u16(&cursor, end, &length) || (nonempty && length == 0) ||
                static_cast<size_t>(end - cursor) < length) return false;
        cursor += length;
        return true;
    };
    const auto skip32 = [&](bool nonempty) -> bool {
        uint32_t length = 0;
        if (!read_u32(&cursor, end, &length) || (nonempty && length == 0) ||
                static_cast<size_t>(end - cursor) < length) return false;
        cursor += length;
        return true;
    };
    if (frame.type == kRequestHead) {
        if ((frame.flags & kFlagRequestUpgrade) == 0 ||
                !skip16(true) || !skip32(true)) {
            return reject_payload(error, "invalid channel payload");
        }
    } else if (frame.type == kResponseHead) {
        uint16_t status = 0;
        if ((frame.flags & kFlagResponseUpgrade) == 0 ||
                (frame.flags & kFlagResponseFixedBody) != 0 ||
                !read_u16(&cursor, end, &status) || status != 101) {
            return reject_payload(error, "invalid upgrade response");
        }
        if (!skip16(false)) return reject_payload(error, "invalid channel payload");
    } else {
        return reject_payload(error, "invalid channel payload");
    }
    uint16_t count = 0;
    if (!read_u16(&cursor, end, &count)) {
        return reject_payload(error, "invalid channel payload");
    }
    for (uint16_t i = 0; i < count; ++i) {
        if (!skip16(true) || !skip32(false)) {
            return reject_payload(error, "invalid channel payload");
        }
    }
    if (cursor != end) return reject_payload(error, "invalid channel payload");
    return decode_upgrade_tail(end, 12, tail, error);
}

}  // namespace protocol
}  // namespace capsid
