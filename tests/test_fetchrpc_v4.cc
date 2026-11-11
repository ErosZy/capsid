// FetchRPC v4 conformance harness (contract stage + A2 amendment v4.1).
//
// Contract: docs/fetchrpc-v4.md (FROZEN 2026-10-11 + A2 2026-10-12).
// Golden vectors: tests/fixtures/fetchrpc-v4/vectors.json
// (schema_version 2, frozen with the contract).
//
// This harness enforces, with a real JSON parser (vendored jansson):
//   * strict schema: missing files, wrong schema versions, malformed
//     entries, and unexecuted sequences all fail the run;
//   * the v3 boundary: the shipped v3 parser rejects every
//     v4-versioned vector with the exact contract error string;
//   * v4 grammar: every ok vector parses, its payload decodes through
//     the matching A2 decoder, and re-encodes byte-identically; every
//     error vector fails with the exact contract error string;
//   * HELLO vectors through the REAL startup decoder (decode_hello):
//     carriage and A2 shapes decode (limits included), every negative
//     is rejected with its contract string;
//   * every sequence executes against an independent reference state
//     model (test-local): negotiation/closure, upgrade commit/release,
//     dial association, credits, fragmentation, close kinds, capability
//     gating. The production registry (T06) must repeat the same
//     sequences and does not substitute for this model.
#include "fetchrpc_v4_sequences.h"
#include "ipc_validation.h"
#include "protocol.h"

#include <jansson.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void require(bool condition, const std::string &message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

std::vector<uint8_t> unhex(const std::string &hex) {
    std::vector<uint8_t> out;
    if (hex.size() % 2 != 0) {
        return out;
    }
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = std::stoi(hex.substr(i, 1), nullptr, 16);
        const int lo = std::stoi(hex.substr(i + 1, 1), nullptr, 16);
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

std::string hex_of(const std::vector<uint8_t> &bytes) {
    static const char *kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (size_t i = 0; i < bytes.size(); ++i) {
        out.push_back(kDigits[bytes[i] >> 4]);
        out.push_back(kDigits[bytes[i] & 0xf]);
    }
    return out;
}

// ---------------------------------------------------------------------
// Reference state model (independent of the production registry)
// ---------------------------------------------------------------------

namespace model {

enum Side { kHost = 0, kWorker = 1 };

struct Channel {
    std::string state;  // pending|dialing|open|rejected|closing|closed|cancelled
    uint64_t dial_id = 0;
    uint64_t message_cap = 0;
    uint64_t message_bytes_so_far[2] = {0, 0};
    bool message_open[2] = {false, false};
    bool message_text[2] = {false, false};
    uint64_t credit[2] = {65536, 65536};
    bool half_closed[2] = {false, false};   // by Side
    bool close_sent[2] = {false, false};    // by Side
};

struct Reference {
    uint32_t announced[2] = {0, 0};          // Hello/READY bitsets
    uint32_t effective = 0;
    bool hello_seen = false;
    bool ready_seen = false;
    uint16_t wire_version = 0;
    uint64_t next_dial = 1;
    std::map<uint32_t, Channel> channels;
    std::map<uint64_t, Channel> pending_dials;
    uint64_t last_dial_id = 0;

    static uint32_t caps_mask() {
        namespace p = capsid::protocol;
        return p::kCapabilitiesKnownMask;
    }

    std::string apply(Side from, const capsid::protocol::Frame &frame,
                      std::string *detail) {
        namespace p = capsid::protocol;
        // bootstrap: first frame must be HELLO from the host
        if (!hello_seen) {
            if (frame.version == 3) {
                return "unsupported protocol version";
            }
            if (frame.type != p::kHello) {
                return "first frame must be HELLO";
            }
            return hello(frame, detail);
        }
        // version lock: every later frame must match the connection
        if (frame.version != wire_version) {
            return "unsupported protocol version";
        }
        if (frame.type == p::kHello) {
            return "duplicate HELLO";
        }
        if (!ready_seen) {
            if (frame.type != p::kReady) {
                return "expected READY";
            }
            return ready(frame, detail);
        }
        switch (frame.type) {
        case p::kReady:
            return "duplicate READY";
        case p::kChannelOpen:
            return channel_open(from, frame, detail);
        case p::kChannelAccept:
            return channel_accept(from, frame, detail);
        case p::kChannelReject:
            return channel_reject(from, frame, detail);
        case p::kChannelData:
            return channel_data(from, frame, detail);
        case p::kChannelCredit:
            return channel_credit(from, frame, detail);
        case p::kChannelHalfClose:
            return channel_half_close(from, frame, detail);
        case p::kChannelClose:
            return channel_close(from, frame, detail);
        case p::kChannelCancel:
            return channel_cancel(from, frame, detail);
        case p::kRequestHead:
            return upgrade_request(from, frame, detail);
        case p::kResponseHead:
            return upgrade_response(from, frame, detail);
        default:
            return "ok";  // non-channel v4 frames (carriage) pass through
        }
    }

    std::string hello(const capsid::protocol::Frame &frame,
                      std::string *) {
        namespace p = capsid::protocol;
        if (frame.payload.size() < p::kHelloV4TailSize) {
            return "invalid hello capabilities";
        }
        const uint8_t *tail =
            frame.payload.data() + frame.payload.size() -
            p::kHelloV4TailSize;
        const uint32_t caps = p::load_u32_at(tail);
        const uint32_t reserved = p::load_u32_at(tail + 4);
        (void)reserved;
        // closure + unknown bits are validated by the real decoder in
        // the harness; the model re-checks to stay self-contained
        if (!p::capabilities_satisfy_closure(caps)) {
            return "invalid hello capabilities";
        }
        announced[kHost] = caps;
        hello_seen = true;
        wire_version = frame.version;
        return "ok";
    }

    std::string ready(const capsid::protocol::Frame &frame,
                      std::string *) {
        namespace p = capsid::protocol;
        if (frame.payload.size() < p::kHelloV4TailSize) {
            return "invalid hello capabilities";
        }
        const uint8_t *tail =
            frame.payload.data() + frame.payload.size() -
            p::kHelloV4TailSize;
        const uint32_t caps = p::load_u32_at(tail);
        if (!p::capabilities_satisfy_closure(caps)) {
            return "invalid hello capabilities";
        }
        announced[kWorker] = caps;
        effective = announced[kHost] & announced[kWorker];
        ready_seen = true;
        return "ok";
    }

    bool require_cap(uint32_t bit, std::string *error) {
        if ((effective & bit) == 0) {
            if (error) {
                *error = "channel frame without capability";
            }
            return false;
        }
        return true;
    }

    Channel *live(uint32_t id, std::string *error) {
        std::map<uint32_t, Channel>::iterator it = channels.find(id);
        if (it == channels.end()) {
            if (error) {
                *error = "unknown channel";
            }
            return nullptr;
        }
        if (it->second.state == "closed" ||
                it->second.state == "cancelled" ||
                it->second.state == "rejected") {
            if (error) {
                *error = "unknown channel";
            }
            return nullptr;
        }
        return &it->second;
    }

    std::string channel_open(Side from, const capsid::protocol::Frame &f,
                             std::string *) {
        namespace p = capsid::protocol;
        if (!require_cap(p::kCapDuplexChannel, &last_error)) {
            return last_error;
        }
        if (from != kWorker) {
            return "channel open direction";
        }
        p::ChannelOpenPayload open;
        if (!p::decode_channel_open(f.payload.data(), f.payload.size(),
                                    &open, &last_error)) {
            return "invalid channel payload";
        }
        if (open.channel_id != 0 || open.dial_id == 0) {
            return "channel open direction";
        }
        if (open.dial_id <= last_dial_id) {
            return "channel sequence";
        }
        Channel channel;
        channel.state = "dialing";
        channel.dial_id = open.dial_id;
        channel.message_cap = open.max_message_bytes;
        pending_dials.emplace(open.dial_id, channel);
        last_dial_id = open.dial_id;
        return "ok";
    }

    std::string channel_accept(Side from, const capsid::protocol::Frame &f,
                               std::string *) {
        namespace p = capsid::protocol;
        if (!require_cap(p::kCapDuplexChannel, &last_error)) {
            return last_error;
        }
        if (from != kHost) {
            return "channel open direction";
        }
        p::ChannelAcceptPayload accept;
        if (!p::decode_channel_accept(f.payload.data(), f.payload.size(),
                                      &accept, &last_error)) {
            return "invalid channel payload";
        }
        auto it = pending_dials.find(accept.dial_id);
        if (it == pending_dials.end()) {
            return "unknown channel";
        }
        Channel channel = it->second;
        if (channel.state != "dialing") {
            return "unknown channel";
        }
        channel.state = "open";
        channel.message_cap = std::min(channel.message_cap,
                                       accept.accepted_max_message_bytes);
        channels[accept.channel_id] = channel;
        pending_dials.erase(it);
        return "ok";
    }

    std::string channel_reject(Side from,
                               const capsid::protocol::Frame &f,
                               std::string *) {
        namespace p = capsid::protocol;
        if (!require_cap(p::kCapDuplexChannel, &last_error)) {
            return last_error;
        }
        if (from != kHost) {
            return "channel open direction";
        }
        p::ChannelRejectPayload reject;
        if (!p::decode_channel_reject(f.payload.data(), f.payload.size(),
                                      &reject, &last_error)) {
            return "invalid channel payload";
        }
        auto it = pending_dials.find(reject.dial_id);
        if (it == pending_dials.end() ||
                it->second.state != "dialing") {
            return "unknown channel";
        }
        pending_dials.erase(it);
        return "ok";
    }

    std::string channel_data(Side from, const capsid::protocol::Frame &f,
                             std::string *) {
        namespace p = capsid::protocol;
        if (!require_cap(p::kCapDuplexChannel, &last_error)) {
            return last_error;
        }
        uint32_t id = 0;
        if (!p::decode_channel_data_id(f.payload.data(), f.payload.size(),
                                       &id, &last_error)) {
            return "invalid channel payload";
        }
        Channel *channel = live(id, &last_error);
        if (channel == nullptr) {
            return last_error;
        }
        if (channel->state != "open") {
            return "channel sequence";
        }
        if (channel->close_sent[from] || channel->half_closed[from]) {
            return "channel sequence";
        }
        const size_t message_bytes = f.payload.size() - 4;
        const bool start = (f.flags & p::kFlagStart) != 0;
        const bool end = (f.flags & p::kFlagEnd) != 0;
        const bool text = (f.flags & p::kFlagChanText) != 0;
        // fragmentation and cap checks precede the credit check so a
        // cap violation is reported even when credit would also fail
        if (!start && !channel->message_open[from]) {
            return "fragmentation";
        }
        if (start && channel->message_open[from]) {
            return "fragmentation";
        }
        const uint64_t accumulated =
            (start ? 0 : channel->message_bytes_so_far[from]) + message_bytes;
        if (accumulated > channel->message_cap) {
            return "message size exceeded";
        }
        if (!start && text != channel->message_text[from]) return "fragmentation";
        if (channel->credit[from] < message_bytes) return "channel credit underflow";
        if (start) {
            channel->message_open[from] = true;
            channel->message_text[from] = text;
        }
        channel->message_bytes_so_far[from] = accumulated;
        if (end) {
            channel->message_open[from] = false;
        }
        channel->credit[from] -= message_bytes;
        return "ok";
    }

    std::string channel_credit(Side from,
                               const capsid::protocol::Frame &f,
                               std::string *) {
        namespace p = capsid::protocol;
        if (!require_cap(p::kCapDuplexChannel, &last_error)) {
            return last_error;
        }
        p::ChannelCreditPayload grant;
        if (!p::decode_channel_credit(f.payload.data(), f.payload.size(),
                                      &grant, &last_error)) {
            return "invalid channel payload";
        }
        Channel *channel = live(grant.channel_id, &last_error);
        if (channel == nullptr) {
            return last_error;
        }
        // credit flows TO the peer
        const Side beneficiary = from == kHost ? kWorker : kHost;
        if (channel->credit[beneficiary] >
            UINT64_MAX - grant.credit) {
            return "channel credit overflow";
        }
        channel->credit[beneficiary] += grant.credit;
        return "ok";
    }

    std::string channel_half_close(Side from,
                                   const capsid::protocol::Frame &f,
                                   std::string *) {
        namespace p = capsid::protocol;
        if (!require_cap(p::kCapHalfClose, &last_error)) {
            return last_error;
        }
        uint32_t id = 0;
        if (!p::decode_channel_data_id(f.payload.data(), f.payload.size(),
                                       &id, &last_error) ||
                f.payload.size() != 4) {
            return "invalid channel payload";
        }
        Channel *channel = live(id, &last_error);
        if (channel == nullptr) {
            return last_error;
        }
        if (channel->half_closed[from]) {
            return "channel sequence";
        }
        channel->half_closed[from] = true;
        if (channel->half_closed[kHost] && channel->half_closed[kWorker]) {
            channel->state = "closed";
        }
        return "ok";
    }

    std::string channel_close(Side from,
                              const capsid::protocol::Frame &f,
                              std::string *) {
        namespace p = capsid::protocol;
        if (!require_cap(p::kCapDuplexChannel, &last_error)) {
            return last_error;
        }
        p::ChannelClosePayload close;
        if (!p::decode_channel_close(f.payload.data(), f.payload.size(),
                                     &close, &last_error)) {
            return last_error;  // exact close-code matrix string
        }
        Channel *channel = live(close.channel_id, &last_error);
        if (channel == nullptr) {
            return last_error;
        }
        if (channel->close_sent[from]) {
            return "channel sequence";
        }
        channel->close_sent[from] = true;
        if (channel->close_sent[kHost] && channel->close_sent[kWorker]) {
            channel->state = "closed";
        } else {
            channel->state = "closing";
        }
        return "ok";
    }

    std::string channel_cancel(Side from,
                               const capsid::protocol::Frame &f,
                               std::string *) {
        namespace p = capsid::protocol;
        if (!require_cap(p::kCapDuplexChannel, &last_error)) {
            return last_error;
        }
        p::ChannelCancelPayload cancel;
        if (!p::decode_channel_cancel(f.payload.data(), f.payload.size(),
                                      &cancel, &last_error)) {
            return "invalid channel payload";
        }
        Channel *channel = live(cancel.channel_id, &last_error);
        if (channel == nullptr) {
            return last_error;
        }
        channel->state = "cancelled";
        return "ok";
    }

    std::string upgrade_request(Side from,
                                const capsid::protocol::Frame &f,
                                std::string *) {
        namespace p = capsid::protocol;
        if (!require_cap(p::kCapUpgrade, &last_error)) {
            return last_error;
        }
        if (from != kHost) {
            return "invalid upgrade response";
        }
        if ((f.flags & p::kFlagRequestUpgrade) == 0) {
            return "ok";  // normal request, not upgrade traffic
        }
        p::UpgradeTail tail;
        if (!p::decode_upgrade_head(f, &tail, &last_error)) {
            return "invalid channel payload";
        }
        if (channels.count(tail.channel_id) != 0) {
            return "channel sequence";
        }
        Channel channel;
        channel.state = "pending";
        channel.message_cap = tail.max_message_bytes;
        channels[tail.channel_id] = channel;
        last_upgrade_pending = tail.channel_id;
        return "ok";
    }

    std::string upgrade_response(Side from,
                                 const capsid::protocol::Frame &f,
                                 std::string *) {
        namespace p = capsid::protocol;
        if (!require_cap(p::kCapUpgrade, &last_error)) {
            return last_error;
        }
        if (from != kWorker) {
            return "invalid upgrade response";
        }
        const bool upgrade_flag =
            (f.flags & p::kFlagResponseUpgrade) != 0;
        const bool fixed_body =
            (f.flags & p::kFlagResponseFixedBody) != 0;
        uint32_t status = 0;
        if (f.payload.size() < 4) {
            return "invalid channel payload";
        }
        status = p::load_u16_at(f.payload.data());
        if (upgrade_flag && fixed_body) {
            return "invalid upgrade response";
        }
        if (!upgrade_flag) {
            if (status == 101) {
                return "invalid upgrade response";
            }
            // non-101 to a pending upgrade releases the reservation
            if (last_upgrade_pending != 0) {
                std::map<uint32_t, Channel>::iterator it =
                    channels.find(last_upgrade_pending);
                if (it != channels.end() &&
                        it->second.state == "pending") {
                    channels.erase(it);
                }
                last_upgrade_pending = 0;
            }
            return "ok";
        }
        if (status != 101) {
            return "invalid upgrade response";
        }
        p::UpgradeTail tail;
        if (!p::decode_upgrade_tail(f.payload.data() + f.payload.size() - 12,
                                    12,
                                    &tail, &last_error)) {
            return "invalid channel payload";
        }
        std::map<uint32_t, Channel>::iterator it =
            channels.find(tail.channel_id);
        if (it == channels.end() || it->second.state != "pending") {
            return "unknown channel";
        }
        it->second.state = "open";
        it->second.message_cap =
            std::min(it->second.message_cap, tail.max_message_bytes);
        return "ok";
    }

    uint32_t last_upgrade_pending = 0;
    std::string last_error;
};

}  // namespace model

// A2 payload decode dispatcher: routes by the vector's decode tag,
// decodes strictly, and (for round-trip parity) re-encodes. Returns
// false with the exact contract error on rejection.
bool decode_a2(const std::string &decode,
               const capsid::protocol::Frame &frame,
               std::string *error) {
    namespace p = capsid::protocol;
    const uint8_t *data = frame.payload.data();
    size_t size = frame.payload.size();
    if (decode == "upgrade-tail") {
        p::UpgradeTail head_tail;
        if (!p::decode_upgrade_head(frame, &head_tail, error)) return false;
        if (size < 12) {
            if (error) *error = "invalid channel payload";
            return false;
        }
        data += size - 12;
        size = 12;
    }
    if (decode == "open") {
        p::ChannelOpenPayload out;
        if (!p::decode_channel_open(data, size, &out, error)) {
            return false;
        }
        std::vector<uint8_t> re;
        return p::encode_channel_open(out, &re) &&
               re.size() == size &&
               std::equal(re.begin(), re.end(), data);
    }
    if (decode == "accept") {
        p::ChannelAcceptPayload out;
        if (!p::decode_channel_accept(data, size, &out, error)) {
            return false;
        }
        std::vector<uint8_t> re;
        return p::encode_channel_accept(out, &re) &&
               re.size() == size &&
               std::equal(re.begin(), re.end(), data);
    }
    if (decode == "reject") {
        p::ChannelRejectPayload out;
        if (!p::decode_channel_reject(data, size, &out, error)) {
            return false;
        }
        std::vector<uint8_t> re;
        return p::encode_channel_reject(out, &re) &&
               re.size() == size &&
               std::equal(re.begin(), re.end(), data);
    }
    if (decode == "credit") {
        p::ChannelCreditPayload out;
        if (!p::decode_channel_credit(data, size, &out, error)) {
            return false;
        }
        std::vector<uint8_t> re;
        return p::encode_channel_credit(out, &re) &&
               re.size() == size &&
               std::equal(re.begin(), re.end(), data);
    }
    if (decode == "halfclose") {
        uint32_t id = 0;
        return p::decode_channel_data_id(data, size, &id, error) &&
               size == 4;
    }
    if (decode == "close") {
        p::ChannelClosePayload out;
        if (!p::decode_channel_close(data, size, &out, error)) {
            return false;
        }
        std::vector<uint8_t> re;
        return p::encode_channel_close(out, &re) &&
               re.size() == size &&
               std::equal(re.begin(), re.end(), data);
    }
    if (decode == "cancel") {
        p::ChannelCancelPayload out;
        if (!p::decode_channel_cancel(data, size, &out, error)) {
            return false;
        }
        std::vector<uint8_t> re;
        return p::encode_channel_cancel(out, &re) &&
               re.size() == size &&
               std::equal(re.begin(), re.end(), data);
    }
    if (decode == "ack") {
        p::ChannelAckPayload out;
        if (!p::decode_channel_ack(data, size, &out, error)) return false;
        std::vector<uint8_t> re;
        return p::encode_channel_ack(out, &re) && re.size() == size &&
            std::equal(re.begin(), re.end(), data);
    }
    if (decode == "upgrade-tail") {
        p::UpgradeTail out;
        if (!p::decode_upgrade_tail(data, size, &out, error)) {
            return false;
        }
        std::vector<uint8_t> re;
        return p::encode_upgrade_tail(out, &re) &&
               re.size() == size &&
               std::equal(re.begin(), re.end(), data);
    }
    if (decode == "data") {
        uint32_t id = 0;
        return p::decode_channel_data_id(data, size, &id, error);
    }
    if (decode == "hello-tail" || decode == "none") {
        return true;  // handled elsewhere (real decoder)
    }
    if (decode == "ready-tail") {
        // reference READY-tail rule (contract §1.2): at least 8 tail
        // bytes, known bits, zero reserved, closure satisfied; the
        // production client-side READY parse lands in T07 and must
        // repeat these vectors
        if (frame.payload.size() < 8) {
            if (error) {
                *error = "invalid hello capabilities";
            }
            return false;
        }
        const uint8_t *tail =
            frame.payload.data() + frame.payload.size() - 8;
        const uint32_t caps = p::load_u32_at(tail);
        const uint32_t reserved = p::load_u32_at(tail + 4);
        if (reserved != 0 ||
                (caps & ~p::kCapabilitiesKnownMask) != 0 ||
                !p::capabilities_satisfy_closure(caps)) {
            if (error) {
                *error = "invalid hello capabilities";
            }
            return false;
        }
        return true;
    }
    if (error) {
        *error = "unknown decode tag: " + decode;
    }
    return false;
}

// SequenceTarget adapter over the reference model.
class ReferenceTarget : public fetchrpc_sequences::SequenceTarget {
public:
    void reset() override { reference_ = model::Reference(); }
    std::string apply(const std::string &dir,
                      const capsid::protocol::Frame &frame) override {
        const std::string result = reference_.apply(
            dir == "h2w" ? model::kHost : model::kWorker,
            frame, nullptr);
        return result == "ok" ? result : "error:" + result;
    }

    model::Reference reference_;
};

}  // namespace

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : nullptr;
    if (!path) {
        std::fprintf(stderr, "usage: %s <vectors.json>\n", argv[0]);
        return 2;
    }
    std::ifstream in(path);
    if (!in.good()) {
        std::fprintf(stderr, "FAIL: vectors file missing: %s\n", path);
        return 2;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    json_error_t json_error;
    json_t *root = json_loads(buffer.str().c_str(), JSON_REJECT_DUPLICATES, &json_error);
    if (!root) {
        std::fprintf(stderr, "FAIL: vectors JSON parse error %s:%d %s\n",
                     path, json_error.line, json_error.text);
        return 2;
    }

    // strict schema
    const json_t *schema_version =
        json_object_get(root, "schema_version");
    require(json_is_integer(schema_version) &&
                json_integer_value(schema_version) == 2,
            "schema_version is exactly 2");
    const json_t *frames = json_object_get(root, "frames");
    const json_t *sequences = json_object_get(root, "sequences");
    require(json_is_array(frames) && json_array_size(frames) >= 40,
            "frames array present");
    require(json_is_array(sequences) && json_array_size(sequences) >= 20,
            "sequences array present");

    std::map<std::string, std::vector<uint8_t>> frame_wire;
    std::map<std::string, std::string> frame_expect;

    size_t index;
    json_t *value;
    json_array_foreach(frames, index, value) {
        const std::string name =
            json_string_value(json_object_get(value, "name"));
        const std::string hex =
            json_string_value(json_object_get(value, "hex"));
        const std::string decode =
            json_string_value(json_object_get(value, "decode"));
        const std::string expect =
            json_string_value(json_object_get(value, "expect"));
        const bool a2_limits =
            json_is_true(json_object_get(value, "a2_limits"));
        require(!name.empty() && !hex.empty() && !expect.empty(),
                "frame entry complete: " + name);
        const std::vector<uint8_t> wire = unhex(hex);
        require(!wire.empty(), "hex decodes: " + name);
        require(frame_wire.count(name) == 0,
                "frame names unique: " + name);
        frame_wire[name] = wire;
        frame_expect[name] = expect;

        // v3 boundary (frozen forever): every v4 vector is rejected
        // by the shipped v3 parser with the exact version string.
        capsid::protocol::Parser v3;
        capsid::protocol::Frame frame;
        require(v3.append(wire.data(), wire.size()),
                "v3 parser buffers: " + name);
        require(v3.next(&frame) == capsid::protocol::kParseError,
                "v3 parser rejects v4 frame: " + name);
        require(v3.error() == "unsupported protocol version",
                "v3 rejection names the version: " + name);

        if (expect == "v4-parser-ok") {
            capsid::protocol::Parser v4(4);
            capsid::protocol::Frame parsed;
            require(v4.append(wire.data(), wire.size()),
                    "v4 parser buffers: " + name);
            require(v4.next(&parsed) == capsid::protocol::kParseFrame,
                    "v4 parser accepts: " + name);
            std::vector<uint8_t> re;
            require(capsid::protocol::encode_versioned(4, parsed, &re),
                    "v4 encode: " + name);
            require(re == wire, "v4 round trip identical: " + name);
            std::string payload_error;
            require(decode_a2(decode, parsed, &payload_error),
                    "positive payload decoder accepts: " + name +
                        " (error: " + payload_error + ")");
        } else if (expect.rfind("v4-parser-error:", 0) == 0) {
            const std::string want =
                expect.substr(std::string("v4-parser-error:").size());
            // Hello-family vectors exercise the REAL startup decoder;
            // the parser may also reject first (unknown bits) - either
            // layer may fire, but the string is the contract's
            if (decode == "hello-tail") {
                capsid::protocol::Parser v4(4);
                capsid::protocol::Frame parsed;
                require(v4.append(wire.data(), wire.size()), "buffers");
                capsid::protocol::ParseResult result = v4.next(&parsed);
                std::string error = v4.error();
                bool rejected = result == capsid::protocol::kParseError;
                if (!rejected) {
                    capsid::WorkerStartupConfig config;
                    rejected = !capsid::decode_hello(parsed, &config,
                                                     &error);
                }
                require(rejected,
                        "parser or startup decoder rejects: " + name);
                require(error == want,
                        "hello rejection exact: " + name +
                            " (got: " + error + ")");
            } else {
                capsid::protocol::Parser v4(4);
                capsid::protocol::Frame parsed;
                require(v4.append(wire.data(), wire.size()), "buffers");
                capsid::protocol::ParseResult result =
                    v4.next(&parsed);
                if (result == capsid::protocol::kParseError) {
                    require(v4.error() == want,
                            "parser error exact: " + name +
                                " (got: " + v4.error() + ")");
                } else {
                    // grammar passed; the A2 payload decoder must
                    // reject with the exact contract string
                    std::string error;
                    require(!decode_a2(decode, parsed, &error),
                            "payload decoder rejects: " + name);
                    require(error == want,
                            "payload error exact: " + name +
                                " (got: " + error + ")");
                }
            }
        }

        // positive HELLOs through the real startup decoder
        if (decode == "hello-tail" && expect == "v4-parser-ok") {
            capsid::protocol::Parser v4(4);
            capsid::protocol::Frame parsed;
            v4.append(wire.data(), wire.size());
            require(v4.next(&parsed) == capsid::protocol::kParseFrame,
                    "hello parses: " + name);
            capsid::WorkerStartupConfig config;
            std::string error;
            require(capsid::decode_hello(parsed, &config, &error),
                    "real startup decoder accepts: " + name +
                        " (error: " + error + ")");
            if (a2_limits) {
                require(config.channel_limits_present,
                        "A2 limits decoded: " + name);
                require(config.channel_limits.max_channels == 128,
                        "A2 limits carry values: " + name);
            }
        }
    }
    // ---- sequences through the reference model ----
    {
        ReferenceTarget target;
        const std::set<std::string> executed =
            fetchrpc_sequences::run_sequences(sequences, frame_wire,
                                              &target, &failures);
        require(executed.size() == json_array_size(sequences),
                "every sequence executed");
    }

    json_decref(root);
    if (failures > 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("fetchrpc-v4 A2 vectors + reference sequences: PASS\n");
    return 0;
}
