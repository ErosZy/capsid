#include "protocol.h"

#include <algorithm>
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
    return type >= kHello && type <= kMemoryMetricsResponse;
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
    if (type == kError) {
        return (flags & ~kErrorFlagTimeout) == 0;
    }
    if (type == kReady) {
        return (flags & ~kReadySandboxFeatureMask) == 0;
    }
    return flags == 0;
}

}  // namespace

Parser::Parser() : offset_(0) {}

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
    if (load_u16(header + 4) != kVersion) {
        error_ = "unsupported protocol version";
        return kParseError;
    }

    const uint16_t type = load_u16(header + 6);
    if (!valid_type(type)) {
        error_ = "invalid frame type";
        return kParseError;
    }
    const uint32_t flags = load_u32(header + 8);
    if (!valid_flags(type, flags)) {
        error_ = "invalid frame flags";
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
    payload->assign(
        buffer_.begin() + static_cast<ptrdiff_t>(offset_ + kHeaderSize),
        buffer_.begin() + static_cast<ptrdiff_t>(offset_ + frame_size));
    if (payload != &frame->payload) {
        frame->payload.clear();
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

}  // namespace protocol
}  // namespace capsid
