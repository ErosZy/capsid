#include "host/ws_stream.h"
#include "protocol.h"
#include <algorithm>
#include <limits>

namespace capsid {
namespace host {
namespace ws {
StreamDecoder::Result StreamDecoder::fail(uint16_t code, const char *error) {
    error_code_ = code; error_ = error; return Error;
}
bool StreamDecoder::utf8(const uint8_t *bytes, size_t size, bool final) {
    for (size_t i=0; i<size; ++i) {
        const uint8_t c = bytes[i];
        if (utf8_remaining_) {
            if (c < utf8_min_ || c > utf8_max_) return false;
            --utf8_remaining_; utf8_min_ = 0x80; utf8_max_ = 0xbf;
        } else if (c <= 0x7f) continue;
        else if (c >= 0xc2 && c <= 0xdf) utf8_remaining_ = 1;
        else if (c >= 0xe0 && c <= 0xef) {
            utf8_remaining_ = 2;
            if (c == 0xe0) utf8_min_ = 0xa0;
            if (c == 0xed) utf8_max_ = 0x9f;
        } else if (c >= 0xf0 && c <= 0xf4) {
            utf8_remaining_ = 3;
            if (c == 0xf0) utf8_min_ = 0x90;
            if (c == 0xf4) utf8_max_ = 0x8f;
        } else return false;
    }
    return !final || utf8_remaining_ == 0;
}
StreamDecoder::Result StreamDecoder::consume(const uint8_t *bytes, size_t size,
                                             size_t *used, uint64_t credit, Item *item) {
    *used = 0;
    if (!error_.empty()) return Error;
    for (;;) {
        if (!in_payload_) {
            while (header_size_ < header_needed_) {
                if (*used == size) return NeedMore;
                header_[header_size_++] = bytes[(*used)++];
                if (header_size_ == 2) {
                    opcode_ = header_[0] & 15; fin_ = (header_[0] & 0x80) != 0;
                    const unsigned length = header_[1] & 127;
                    if ((header_[0] & 0x70) || bool(header_[1] & 0x80) != (role_ == kServer) ||
                        (opcode_ != kContinuation && opcode_ != kText && opcode_ != kBinary &&
                         opcode_ != kClose && opcode_ != kPing && opcode_ != kPong)) return fail(1002, "invalid RFC frame header");
                    if (opcode_ >= 8 && (!fin_ || length > 125)) return fail(1002, "invalid control frame");
                    header_needed_ = 2 + (length == 126 ? 2 : length == 127 ? 8 : 0) + (role_ == kServer ? 4 : 0);
                }
            }
            const unsigned short_length = header_[1] & 127;
            uint64_t length = short_length;
            if (short_length >= 126) {
                length = 0;
                const size_t count = short_length == 126 ? 2 : 8;
                for (size_t i=0; i<count; ++i) length = (length << 8) | header_[2+i];
                if ((short_length == 126 && length < 126) ||
                    (short_length == 127 && (length < 65536 || (header_[2] & 0x80)))) return fail(1002, "noncanonical frame length");
            }
            if (closed_) return fail(1002, "frame after Close");
            if (opcode_ < 8) {
                if (opcode_ == kContinuation) {
                    if (!message_open_) return fail(1002, "continuation without message");
                    start_ = false;
                } else {
                    if (message_open_) return fail(1002, "new frame inside fragmented message");
                    message_open_ = true; message_bytes_ = 0; utf8_remaining_ = 0;
                    text_ = opcode_ == kText; start_ = true;
                }
                if (length > max_message_ || message_bytes_ > max_message_ - length) return fail(1009, "message size exceeded");
                message_bytes_ += length;
            }
            remaining_ = length; offset_ = 0; control_size_ = 0; in_payload_ = true;
        }
        if (opcode_ >= 8) {
            while (remaining_ && *used < size) {
                control_[control_size_++] = bytes[(*used)++] ^ (role_ == kServer ? header_[header_needed_ - 4 + offset_ % 4] : 0);
                ++offset_; --remaining_;
            }
            if (remaining_) return NeedMore;
            item->opcode = opcode_; item->start = item->end = false;
            item->payload.assign(control_.begin(), control_.begin() + control_size_);
            item->code = 0;
            if (opcode_ == kClose) {
                if (control_size_ == 1) return fail(1002, "invalid Close payload");
                if (control_size_ >= 2) {
                    item->code = uint16_t(control_[0]) << 8 | control_[1];
                    if (!protocol::is_sendable_close_code(item->code)) return fail(1002, "invalid Close code");
                    // Close reason has an independent UTF-8 validator.
                    StreamDecoder reason(123);
                    if (!reason.utf8(control_.data()+2, control_size_-2, true)) return fail(1007, "invalid Close reason UTF-8");
                }
                closed_ = true;
            }
            in_payload_ = false; header_size_ = 0; header_needed_ = 2;
            return ItemReady;
        }
        const size_t amount = static_cast<size_t>(std::min<uint64_t>(
            std::min<uint64_t>(remaining_, credit), std::min<size_t>(size - *used, 16384)));
        if (remaining_ && !amount) return NeedMore;
        item->payload.resize(amount);
        for (size_t i=0; i<amount; ++i) item->payload[i] = bytes[(*used)++] ^ (role_ == kServer ? header_[header_needed_ - 4 + (offset_+i)%4] : 0);
        const bool end = fin_ && remaining_ == amount;
        // Message payloads are delivered as-is: workerd does not
        // validate text UTF-8 on the wire; the worker's TextDecoder
        // surfaces invalid bytes lossily as U+FFFD.
        item->opcode = text_ ? kText : ws::kBinary;
        item->text = text_; item->start = start_; item->end = end; item->code = 0;
        start_ = false; remaining_ -= amount; offset_ += amount;
        if (!remaining_) { in_payload_ = false; header_size_ = 0; header_needed_ = 2; }
        if (end) { message_open_ = false; message_bytes_ = 0; }
        return ItemReady;
    }
}
}  // namespace ws
}  // namespace host
}  // namespace capsid
