#include "ws_codec.h"

#include <cstring>

namespace capsid {
namespace host {
namespace ws {
namespace {

// ---- SHA-1 (RFC 3174), fixed-size, no external dependency ----

struct Sha1 {
    uint32_t h[5];
    uint64_t total = 0;
    uint8_t block[64];
    size_t used = 0;

    Sha1() {
        h[0] = 0x67452301u;
        h[1] = 0xEFCDAB89u;
        h[2] = 0x98BADCFEu;
        h[3] = 0x10325476u;
        h[4] = 0xC3D2E1F0u;
    }

    static uint32_t rol(uint32_t value, int bits) {
        return (value << bits) | (value >> (32 - bits));
    }

    void process(const uint8_t chunk[64]) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(chunk[i * 4]) << 24) |
                   (static_cast<uint32_t>(chunk[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(chunk[i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(chunk[i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f;
            uint32_t k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const uint32_t temp = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = temp;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    void update(const uint8_t *data, size_t size) {
        total += size;
        while (size > 0) {
            const size_t take = 64 - used < size ? 64 - used : size;
            std::memcpy(block + used, data, take);
            used += take;
            data += take;
            size -= take;
            if (used == 64) {
                process(block);
                used = 0;
            }
        }
    }

    void digest(uint8_t out[20]) {
        const uint64_t bits = total * 8;
        const uint8_t pad = 0x80;
        update(&pad, 1);
        const uint8_t zero = 0;
        while (used != 56) {
            update(&zero, 1);
        }
        uint8_t length_bytes[8];
        for (int i = 0; i < 8; ++i) {
            length_bytes[i] = static_cast<uint8_t>(bits >> (56 - i * 8));
        }
        update(length_bytes, 8);
        for (int i = 0; i < 5; ++i) {
            out[i * 4] = static_cast<uint8_t>(h[i] >> 24);
            out[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
            out[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
            out[i * 4 + 3] = static_cast<uint8_t>(h[i]);
        }
    }
};

const char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const uint8_t *data, size_t size) {
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    size_t i = 0;
    while (i + 3 <= size) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8) |
                           static_cast<uint32_t>(data[i + 2]);
        out.push_back(kBase64Alphabet[(v >> 18) & 63]);
        out.push_back(kBase64Alphabet[(v >> 12) & 63]);
        out.push_back(kBase64Alphabet[(v >> 6) & 63]);
        out.push_back(kBase64Alphabet[v & 63]);
        i += 3;
    }
    const size_t rest = size - i;
    if (rest == 1) {
        const uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        out.push_back(kBase64Alphabet[(v >> 18) & 63]);
        out.push_back(kBase64Alphabet[(v >> 12) & 63]);
        out.push_back('=');
        out.push_back('=');
    } else if (rest == 2) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8);
        out.push_back(kBase64Alphabet[(v >> 18) & 63]);
        out.push_back(kBase64Alphabet[(v >> 12) & 63]);
        out.push_back(kBase64Alphabet[(v >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

bool is_control(uint8_t opcode) {
    return opcode >= 0x8;
}

bool valid_opcode(uint8_t opcode) {
    return opcode == kContinuation || opcode == kText || opcode == kBinary ||
           opcode == kClose || opcode == kPing || opcode == kPong;
}

void append_u16_be(std::vector<uint8_t> *out, uint16_t value) {
    out->push_back(static_cast<uint8_t>(value >> 8));
    out->push_back(static_cast<uint8_t>(value));
}

void append_u64_be(std::vector<uint8_t> *out, uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        out->push_back(static_cast<uint8_t>(value >> (i * 8)));
    }
}

}  // namespace

std::string compute_accept_key(const std::string &client_key) {
    static const char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    Sha1 sha;
    sha.update(reinterpret_cast<const uint8_t *>(client_key.data()),
               client_key.size());
    sha.update(reinterpret_cast<const uint8_t *>(kGuid), sizeof(kGuid) - 1);
    uint8_t digest[20];
    sha.digest(digest);
    return base64_encode(digest, sizeof(digest));
}

bool validate_client_key(const std::string &key) {
    if (key.size() != 24 || key[22] != '=' || key[23] != '=') {
        return false;
    }
    for (size_t i = 0; i < 22; ++i) {
        const char c = key[i];
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '+' || c == '/';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool encode(const Frame &frame,
            Role role,
            uint32_t mask,
            std::vector<uint8_t> *output) {
    if (!output || !valid_opcode(frame.opcode)) {
        return false;
    }
    if (is_control(frame.opcode)) {
        if (!frame.fin) {
            return false;  // fragmented control frame
        }
        if (frame.payload.size() > 125) {
            return false;  // control payload limit
        }
    }
    if (role == kClient && mask == 0) {
        return false;  // client frames must be masked
    }
    if (role == kServer && mask != 0) {
        return false;  // server frames must not be masked
    }
    output->clear();
    output->reserve(2 + 8 + frame.payload.size());
    uint8_t head0 = static_cast<uint8_t>(frame.fin ? 0x80 : 0x00) |
                    (frame.opcode & 0x0F);
    output->push_back(head0);
    const bool masked = role == kClient;
    uint8_t head1 = masked ? 0x80 : 0x00;
    if (frame.payload.size() <= 125) {
        head1 |= static_cast<uint8_t>(frame.payload.size());
        output->push_back(head1);
    } else if (frame.payload.size() <= 0xFFFF) {
        head1 |= 126;
        output->push_back(head1);
        append_u16_be(output, static_cast<uint16_t>(frame.payload.size()));
    } else {
        head1 |= 127;
        output->push_back(head1);
        append_u64_be(output, frame.payload.size());
    }
    if (masked) {
        output->push_back(static_cast<uint8_t>(mask >> 24));
        output->push_back(static_cast<uint8_t>(mask >> 16));
        output->push_back(static_cast<uint8_t>(mask >> 8));
        output->push_back(static_cast<uint8_t>(mask));
        size_t i = 0;
        for (const uint8_t byte : frame.payload) {
            const size_t k = i % 4;
            const uint8_t mask_byte =
                static_cast<uint8_t>(mask >> (24 - k * 8));
            output->push_back(byte ^ mask_byte);
            ++i;
        }
    } else {
        output->insert(output->end(),
                       frame.payload.begin(),
                       frame.payload.end());
    }
    return true;
}

Parser::Parser(Role role)
    : role_(role), offset_(0) {}

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
    buffer_.insert(buffer_.end(), data, data + size);
    if (buffer_.size() - offset_ > 64u * 1024u * 1024u) {
        error_ = "frame buffer limit exceeded";
        return false;
    }
    return true;
}

ParseResult Parser::next(Frame *frame) {
    if (!frame || !error_.empty()) {
        return kParseError;
    }
    const size_t buffered = buffer_.size() - offset_;
    if (buffered < 2) {
        return kParseNeedMore;
    }
    const uint8_t *header = &buffer_[offset_];
    const bool fin = (header[0] & 0x80) != 0;
    if ((header[0] & 0x70) != 0) {
        error_ = "reserved bits set";
        return kParseError;
    }
    const uint8_t opcode = header[0] & 0x0F;
    if (!valid_opcode(opcode)) {
        error_ = "reserved opcode";
        return kParseError;
    }
    if (is_control(opcode) && !fin) {
        error_ = "fragmented control frame";
        return kParseError;
    }
    const bool masked = (header[1] & 0x80) != 0;
    if (role_ == kServer && !masked) {
        error_ = "client frame not masked";
        return kParseError;
    }
    if (role_ == kClient && masked) {
        error_ = "server frame must not be masked";
        return kParseError;
    }
    uint64_t length = header[1] & 0x7F;
    size_t header_size = 2;
    if (length == 126) {
        if (buffered < 4) {
            return kParseNeedMore;
        }
        length = (static_cast<uint64_t>(header[2]) << 8) | header[3];
        header_size = 4;
    } else if (length == 127) {
        if (buffered < 10) {
            return kParseNeedMore;
        }
        length = 0;
        for (int i = 2; i < 10; ++i) {
            length = (length << 8) | header[i];
        }
        header_size = 10;
    }
    if (is_control(opcode) && length > 125) {
        error_ = "control frame payload limit";
        return kParseError;
    }
    if (length > 64u * 1024u * 1024u) {
        error_ = "frame payload limit exceeded";
        return kParseError;
    }
    const size_t mask_size = masked ? 4 : 0;
    const size_t frame_size = header_size + mask_size +
                              static_cast<size_t>(length);
    if (buffered < frame_size) {
        return kParseNeedMore;
    }
    frame->fin = fin;
    frame->opcode = opcode;
    frame->payload.assign(
        buffer_.begin() + static_cast<ptrdiff_t>(offset_ + header_size +
                                                 mask_size),
        buffer_.begin() + static_cast<ptrdiff_t>(offset_ + frame_size));
    if (masked) {
        const uint8_t *mask_bytes = header + header_size;
        for (size_t i = 0; i < frame->payload.size(); ++i) {
            frame->payload[i] ^= mask_bytes[i % 4];
        }
    }
    offset_ += frame_size;
    if (offset_ == buffer_.size()) {
        buffer_.clear();
        offset_ = 0;
    }
    return kParseFrame;
}

}  // namespace ws
}  // namespace host
}  // namespace capsid
