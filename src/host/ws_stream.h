#ifndef CAPSID_HOST_WS_STREAM_H
#define CAPSID_HOST_WS_STREAM_H
#include "host/ws_codec.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace capsid {
namespace host {
namespace ws {
// Strict incremental server decoder. Only a header, a 125-byte control
// payload and one <=16 KiB delivery chunk are ever owned; a 64 MiB frame
// never causes a 64 MiB frame allocation. UTF-8 state spans continuation
// frames and IPC fragments. The caller pauses reads when credit is zero.
class StreamDecoder {
public:
    struct Item {
        uint8_t opcode = kBinary;
        bool start = false, end = false, text = false;
        uint16_t code = 0;
        std::vector<uint8_t> payload;
    };
    enum Result { NeedMore, ItemReady, Error };
    explicit StreamDecoder(uint64_t max_message, Role role = kServer)
        : max_message_(max_message), role_(role) {}
    Result consume(const uint8_t *bytes, size_t size, size_t *used,
                   uint64_t credit, Item *item);
    uint16_t error_code() const { return error_code_; }
    const std::string &error() const { return error_; }
private:
    Result fail(uint16_t code, const char *error);
    bool utf8(const uint8_t *bytes, size_t size, bool final);
    uint64_t max_message_, message_bytes_ = 0, remaining_ = 0, offset_ = 0;
    Role role_;
    std::array<uint8_t, 14> header_{};
    size_t header_size_ = 0, header_needed_ = 2;
    std::array<uint8_t, 125> control_{};
    size_t control_size_ = 0;
    uint8_t opcode_ = 0;
    bool fin_ = false, in_payload_ = false, message_open_ = false;
    bool text_ = false, start_ = false, closed_ = false;
    unsigned utf8_remaining_ = 0;
    uint8_t utf8_min_ = 0x80, utf8_max_ = 0xbf;
    uint16_t error_code_ = 1002;
    std::string error_;
};
}  // namespace ws
}  // namespace host
}  // namespace capsid
#endif
