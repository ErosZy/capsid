// Independent wire construction follows RFC 6455 sections 5.2-5.5:
// https://www.rfc-editor.org/rfc/rfc6455.html
// Text payload validation follows the project's declared workerd profile;
// this target only constructs ASCII text, and retains strict Close reasons.
#include "host/ws_codec.h"
#include "host/ws_stream.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
namespace ws = capsid::host::ws;
using Bytes = std::vector<uint8_t>;
void invariant(bool value, int line) {
    if (!value) { std::fprintf(stderr, "WS_INVARIANT line=%d\n", line); std::abort(); }
}
#define check(value) invariant((value), __LINE__)

struct Packet {
    bool fin;
    uint8_t opcode;
    Bytes payload;
};

Bytes wire(const Packet& p, bool masked, uint32_t key) {
    Bytes result{static_cast<uint8_t>((p.fin ? 128 : 0) | p.opcode)};
    const uint8_t mask = masked ? 128 : 0;
    if (p.payload.size() < 126) result.push_back(mask | static_cast<uint8_t>(p.payload.size()));
    else {
        const unsigned count = p.payload.size() <= 65535 ? 2 : 8;
        result.push_back(mask | (count == 2 ? 126 : 127));
        const uint64_t length = p.payload.size();
        for (unsigned n = count; n; --n) result.push_back(static_cast<uint8_t>(length >> ((n - 1) * 8)));
    }
    uint8_t key_bytes[4];
    for (unsigned n = 0; n < 4; ++n) key_bytes[n] = static_cast<uint8_t>(key >> (24 - n * 8));
    if (masked) result.insert(result.end(), key_bytes, key_bytes + 4);
    for (size_t i = 0; i < p.payload.size(); ++i)
        result.push_back(p.payload[i] ^ (masked ? key_bytes[i % 4] : 0));
    return result;
}

struct Control {
    uint8_t opcode;
    uint16_t code;
    size_t at;
    Bytes bytes;
    bool operator==(const Control& other) const {
        return opcode == other.opcode && code == other.code && at == other.at && bytes == other.bytes;
    }
};
struct Observed {
    Bytes data;
    std::vector<Control> controls;
    unsigned starts = 0, ends = 0;
    bool active = false, text = false, error = false;
    uint16_t code = 0;
};

Observed stream(const Bytes& bytes, ws::Role role, uint64_t limit, bool fragmented) {
    ws::StreamDecoder decoder(limit, role);
    Observed out;
    size_t offset = 0, steps = 0;
    // Each iteration consumes input, emits a bounded item, or advances the
    // six-step credit schedule. The bound also permits zero-length frames.
    while (steps < 8 * bytes.size() + 128) {
        size_t count = bytes.size() - offset;
        if (fragmented && count) {
            const size_t chunk = offset < 14 ? 1 + offset % 3 :
                257 + static_cast<size_t>(bytes[offset]) * 29;
            count = std::min(count, chunk);
        }
        const uint64_t credits[] = {0, 1, 31, 16384, 16384, 65536};
        const uint64_t credit = fragmented ? credits[steps % 6] : 65536;
        ++steps;
        size_t used = 123;
        ws::StreamDecoder::Item item;
        const auto result = decoder.consume(count ? bytes.data() + offset : nullptr,
                                            count, &used, credit, &item);
        check(used <= count);
        offset += used;
        if (result == ws::StreamDecoder::Error) {
            out.error = true; out.code = decoder.error_code();
            check(out.code == 1002 || out.code == 1007 || out.code == 1009);
            const std::string diagnostic = decoder.error();
            check(!diagnostic.empty());
            used = 777;
            check(decoder.consume(nullptr, 0, &used, 0, &item) == ws::StreamDecoder::Error);
            check(used == 0 && decoder.error_code() == out.code && decoder.error() == diagnostic);
            return out;
        }
        if (result == ws::StreamDecoder::ItemReady) {
            if (item.opcode >= 8) {
                check(item.payload.size() <= 125);
                out.controls.push_back({item.opcode, item.code, out.data.size(), item.payload});
            } else {
                check(item.opcode == 1 || item.opcode == 2);
                check(item.payload.size() <= 16384 && item.payload.size() <= credit);
                if (item.start) { check(!out.active); out.active = true; out.text = item.text; ++out.starts; }
                check(out.active && item.text == out.text);
                out.data.insert(out.data.end(), item.payload.begin(), item.payload.end());
                if (item.end) { out.active = false; ++out.ends; }
            }
            continue;
        }
        check(result == ws::StreamDecoder::NeedMore);
        if (offset == bytes.size()) return out;
        check(used != 0 || credit == 0);
    }
    check(false);
    return out;
}

void parser(const Bytes& bytes, ws::Role role, const std::vector<Packet>* expected, bool chunks) {
    ws::Parser decoder(role);
    size_t offset = 0, observed = 0;
    while (offset < bytes.size()) {
        const size_t length = std::min(bytes.size() - offset,
            chunks ? (offset < 14 ? 1 + offset % 3 : 257 + static_cast<size_t>(bytes[offset]) * 29) : bytes.size());
        if (!decoder.append(bytes.data() + offset, length)) { check(expected == nullptr); return; }
        offset += length;
        for (;;) {
            ws::Frame f;
            const auto result = decoder.next(&f);
            if (result == ws::kParseNeedMore) break;
            if (result == ws::kParseError) {
                check(expected == nullptr && !decoder.error().empty());
                check(!decoder.append(nullptr, 0));
                return;
            }
            check(result == ws::kParseFrame);
            if (expected) {
                check(observed < expected->size());
                const Packet& want = (*expected)[observed];
                check(f.fin == want.fin && f.opcode == want.opcode && f.payload == want.payload);
            }
            ++observed;
        }
        check(decoder.storage_bytes() <= bytes.size());
    }
    if (expected) check(observed == expected->size());
}

void valid(const uint8_t* data, size_t size, bool masked, bool text, bool split) {
    Bytes payload;
    if (size) payload.assign(data, data + size);
    if (text) for (auto& b : payload) b &= 127;
    const uint32_t key = size >= 4 ?
        uint32_t(data[0]) << 24 | uint32_t(data[1]) << 16 | uint32_t(data[2]) << 8 | data[3] : 0x37fa213d;
    std::vector<Packet> packets;
    const size_t middle = split ? payload.size() / 2 : payload.size();
    packets.push_back({!split, static_cast<uint8_t>(text ? 1 : 2), Bytes(payload.begin(), payload.begin() + middle)});
    if (split) {
        packets.push_back({true, 9, {'p', 'i', 'n', 'g'}});
        packets.push_back({true, 0, Bytes(payload.begin() + middle, payload.end())});
    }
    packets.push_back({true, 8, {3, 232, 'o', 'k'}});
    Bytes encoded;
    for (const Packet& p : packets) {
        const Bytes reference = wire(p, masked, key);
        ws::Frame input; input.fin = p.fin; input.opcode = p.opcode; input.payload = p.payload;
        Bytes actual{99};
        check(ws::encode(input, masked ? ws::kClient : ws::kServer, masked ? key : 0, &actual));
        check(actual == reference);
        encoded.insert(encoded.end(), reference.begin(), reference.end());
    }
    const auto role = masked ? ws::kServer : ws::kClient;
    const uint64_t limit = std::max<size_t>(1, size);
    const Observed a = stream(encoded, role, limit, false);
    const Observed b = stream(encoded, role, limit, true);
    check(!a.error && !b.error && !a.active && !b.active);
    check(a.data == payload && b.data == payload && a.starts == 1 && b.starts == 1 && a.ends == 1 && b.ends == 1);
    check(a.controls == b.controls && a.controls.size() == (split ? 2 : 1));
    size_t index = 0;
    if (split) {
        check(a.controls[0].opcode == 9 && a.controls[0].at == middle &&
              a.controls[0].bytes == Bytes({'p', 'i', 'n', 'g'}));
        ++index;
    }
    check(a.controls[index].opcode == 8 && a.controls[index].code == 1000 &&
          a.controls[index].at == payload.size() && a.controls[index].bytes == Bytes({3, 232, 'o', 'k'}));
    parser(encoded, role, &packets, false);
    parser(encoded, role, &packets, true);
}

void invalid() {
    const std::vector<std::pair<Bytes, uint16_t>> cases{
        {{0xc2, 0}, 1002}, {{0x83, 0}, 1002}, {{0x09, 0}, 1002},
        {{0x89, 126, 0, 126}, 1002}, {{0x82, 126, 0, 1, 0}, 1002},
        {{0x82, 127, 0x80, 0, 0, 0, 0, 0, 0, 0}, 1002},
        {{0x82, 127, 0, 0, 0, 0, 0, 0, 1, 0}, 1002},
        {{0x80, 0}, 1002}, {{0x88, 1, 0}, 1002},
        {{0x88, 2, 3, 237}, 1002}, {{0x88, 3, 3, 232, 0xff}, 1007},
        {{0x88, 0, 0x82, 0}, 1002}, {{0x82, 2, 'a', 'b'}, 1009},
    };
    for (const auto& c : cases) {
        const auto a = stream(c.first, ws::kClient, 1, false);
        const auto b = stream(c.first, ws::kClient, 1, true);
        check(a.error && b.error && a.code == c.second && b.code == c.second);
    }
    check(stream({0x82, 0}, ws::kServer, 1, true).code == 1002);
    check(stream({0x82, 0x80, 0, 0, 0, 0}, ws::kClient, 1, true).code == 1002);
}
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    Bytes raw;
    if (size) raw.assign(data, data + size);
    for (const auto role : {ws::kClient, ws::kServer}) {
        const uint64_t limits[] = {0, 1, 125, 1024, 65536, UINT64_C(1) << 32};
        const uint64_t limit = limits[size ? data[0] % 6 : 0];
        const auto a = stream(raw, role, limit, false);
        const auto b = stream(raw, role, limit, true);
        check(a.error == b.error);
        if (a.error) check(a.code == b.code);
        else check(a.data == b.data && a.controls == b.controls && a.starts == b.starts && a.ends == b.ends && a.active == b.active);
        parser(raw, role, nullptr, false);
        parser(raw, role, nullptr, true);
    }
    valid(data, size, false, false, false);
    valid(data, size, true, false, false);
    valid(data, size, false, true, true);
    valid(data, size, true, true, true);
    invalid();
    return 0;
}
