// Exercise actual A2 state transitions, rejected-frame rollback and teardown.
// This target does not exercise the WebSocket wire parser or real sockets.
#include "channel_registry.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <vector>

namespace {
namespace p = capsid::protocol;
using Side = capsid::ChannelSide;
using Registry = capsid::ChannelRegistry;

void invariant(bool value, int line) {
    if (!value) { std::fprintf(stderr, "INVARIANT line=%d\n", line); std::abort(); }
}
#define check(value) invariant((value), __LINE__)

void entry(std::vector<uint64_t>& out, const capsid::ChannelEntry& e) {
    out.insert(out.end(), {e.channel_id, e.dial_id, e.owner_kind, e.channel_kind,
        e.fetch_upgrade, e.http_status, e.owner_request_id, e.max_message_bytes,
        static_cast<uint64_t>(e.phase), e.authority.binding, e.authority.binding_id,
        e.authority_registered});
    for (const auto& d : e.direction) out.insert(out.end(), {d.credit,
        d.half_closed, d.close_sent, d.terminal_sent, d.terminal_kind,
        d.terminal_reason, d.terminal_acked, d.terminal_ack_sent,
        d.message_open, d.message_text, d.message_bytes});
}

std::vector<uint64_t> snapshot(const Registry& r) {
    std::vector<uint64_t> out{r.live_count(), r.retained_owner_count(),
        r.reassembly_reserved(), r.effective_capabilities(), r.wire_version(),
        r.negotiation_complete(), r.messages_completed(), r.bytes_reassembled(),
        r.pending_dial_count()};
    for (uint32_t id = 0; id < 16; ++id) {
        capsid::ChannelEntry e;
        const bool found = r.find(id, &e);
        out.push_back(found);
        if (found) entry(out, e);
        bool cancelled = false, unresolved = false;
        const bool pending = r.pending_dial(id, &e, &cancelled, &unresolved);
        out.insert(out.end(), {pending, cancelled, unresolved});
        if (pending) entry(out, e);
        uint32_t reason = 0;
        const bool reject = r.dial_must_reject(id, &reason);
        out.push_back(reject);
        if (reject) out.push_back(reason);
    }
    return out;
}

std::vector<uint8_t> hello() {
    std::vector<uint8_t> v;
    p::append_u32(&v, 7);
    p::append_u64(&v, 64u * 1024u * 1024u);
    p::append_u64(&v, 0); p::append_u32(&v, 64);
    p::append_u64(&v, 5000); p::append_u32(&v, 1024 * 1024);
    p::append_u32(&v, 4); p::append_u32(&v, 1024);
    p::append_u32(&v, 64 * 1024); p::append_u32(&v, 4 * 1024 * 1024);
    v.push_back(0); p::append_u32(&v, 0); p::append_u32(&v, 0);
    p::append_u16(&v, 0); p::append_u64(&v, 0); p::append_u64(&v, 0);
    p::append_u32(&v, 0); p::append_u32(&v, 0); v.push_back(0);
    p::append_u32(&v, 0); p::append_u16(&v, 0); p::append_u16(&v, 0);
    p::append_u16(&v, 0); p::append_u32(&v, 0); p::append_u32(&v, 0);
    p::append_u16(&v, 0);
    p::ChannelLimits limits;
    limits.max_channels = 4; limits.max_message_bytes = 1024;
    limits.reassembly_budget_bytes = 1024 * 1024; limits.send_budget_bytes = 1024 * 1024;
    limits.per_channel_queue_bytes = 65536;
    limits.close_timeout_ms = 100; limits.dial_timeout_ms = 100;
    check(p::append_hello_a2_extension(&v, limits));
    p::append_u32(&v, p::kCapabilitiesKnownMask); p::append_u32(&v, 0);
    return v;
}

p::Frame frame(uint16_t type) {
    p::Frame f;
    f.version = p::kVersionV4; f.type = type;
    return f;
}

p::Frame incoming_request(uint32_t id, uint64_t request) {
    auto f = frame(p::kRequestHead); f.flags = p::kFlagRequestUpgrade;
    f.request_id = request;
    p::append_u16(&f.payload, 3);
    f.payload.insert(f.payload.end(), {'G', 'E', 'T'});
    const std::string url = "https://fuzz.test/ws";
    p::append_u32(&f.payload, static_cast<uint32_t>(url.size()));
    f.payload.insert(f.payload.end(), url.begin(), url.end());
    p::append_u16(&f.payload, 0);
    p::UpgradeTail tail; tail.channel_id = id; tail.max_message_bytes = 1024;
    p::encode_upgrade_tail(tail, &f.payload);
    return f;
}

p::Frame incoming_response(uint32_t id, uint64_t request) {
    auto f = frame(p::kResponseHead); f.flags = p::kFlagResponseUpgrade;
    f.request_id = request;
    p::append_u16(&f.payload, 101); p::append_u16(&f.payload, 0);
    p::append_u16(&f.payload, 0);  // header count after empty status text
    p::UpgradeTail tail; tail.channel_id = id; tail.max_message_bytes = 1024;
    p::encode_upgrade_tail(tail, &f.payload);
    return f;
}

void warm(Registry& r, bool commit_incoming) {
    auto f = frame(p::kHello); f.payload = hello();
    check(r.apply(Side::Host, f).accepted);
    f = frame(p::kReady); f.payload.assign(20, 0);
    p::append_u32(&f.payload, p::kCapabilitiesKnownMask); p::append_u32(&f.payload, 0);
    check(r.apply(Side::Worker, f).accepted);
    p::ChannelOpenPayload open;
    open.dial_id = 1; open.owner_kind = p::kOwnerStandalone;
    open.channel_kind = p::kChannelKindWebSocket;
    open.max_message_bytes = 1024; open.url = "wss://fuzz.test/";
    f = frame(p::kChannelOpen); check(p::encode_channel_open(open, &f.payload, p::kCapabilitiesKnownMask));
    check(r.apply(Side::Worker, f).accepted);
    p::ChannelAcceptPayload accept;
    accept.dial_id = 1; accept.channel_id = 1; accept.accepted_max_message_bytes = 1024;
    f = frame(p::kChannelAccept); check(p::encode_channel_accept(accept, &f.payload, p::kCapabilitiesKnownMask));
    check(r.apply(Side::Host, f).accepted);
    p::ChannelAckPayload ack;
    ack.dial_id = 1; ack.channel_id = 1; ack.kind = p::kAckDialResult;
    f = frame(p::kChannelAck); check(p::encode_channel_ack(ack, &f.payload));
    check(r.apply(Side::Worker, f).accepted);
    check(r.live_count() == 1);
    check(r.apply(Side::Host, incoming_request(2, 2)).accepted);
    if (commit_incoming) check(r.apply(Side::Worker, incoming_response(2, 2)).accepted);
    check(r.live_count() == 2);
}

p::Frame operation(const uint8_t* d, size_t n) {
    const uint32_t id = d[1] % 16;
    const uint64_t dial = d[2] % 16;
    p::Frame f;
    switch (d[0] % 12) {
    case 0: {
        f = frame(p::kChannelOpen);
        p::ChannelOpenPayload v;
        v.dial_id = dial; v.channel_id = id;
        v.owner_kind = d[3] % 5; v.channel_kind = d[4] % 3;
        v.owner_request_id = d[5] % 16;
        v.max_message_bytes = d[6] ? d[6] * 1024 : 1024;
        v.url = "wss://fuzz.test/";
        p::encode_channel_open(v, &f.payload, p::kCapabilitiesKnownMask); break;
    }
    case 1: {
        f = frame(p::kChannelAccept); p::ChannelAcceptPayload v;
        v.channel_id = id; v.dial_id = dial;
        v.accepted_max_message_bytes = d[6] ? d[6] * 1024 : 1024;
        p::encode_channel_accept(v, &f.payload, p::kCapabilitiesKnownMask); break;
    }
    case 2: {
        f = frame(p::kChannelReject); p::ChannelRejectPayload v;
        v.channel_id = id; v.dial_id = dial; v.reason_code = d[3];
        p::encode_channel_reject(v, &f.payload); break;
    }
    case 3: {
        f = frame(p::kChannelData); p::append_u32(&f.payload, id);
        f.flags = d[3] & 15;
        f.payload.insert(f.payload.end(), d + 8, d + n);
        if (d[6]) f.payload.insert(f.payload.end(), static_cast<size_t>(d[6]) * 64, d[5]); break;
    }
    case 4: {
        f = frame(p::kChannelCredit); p::ChannelCreditPayload v;
        v.channel_id = id;
        v.credit = d[3] & 1 ? std::numeric_limits<uint64_t>::max() : d[4];
        p::encode_channel_credit(v, &f.payload); break;
    }
    case 5: f = frame(p::kChannelHalfClose); p::append_u32(&f.payload, id); break;
    case 6: {
        f = frame(p::kChannelClose); p::ChannelClosePayload v;
        v.channel_id = id; v.close_kind = d[3] % 5;
        v.flags = d[4] & 1; v.code = v.flags ? 1000 : 0;
        p::encode_channel_close(v, &f.payload); break;
    }
    case 7: {
        f = frame(p::kChannelCancel); p::ChannelCancelPayload v;
        v.channel_id = id; v.dial_id = id ? 0 : dial; v.terminal_reason = d[3] % 7;
        p::encode_channel_cancel(v, &f.payload); break;
    }
    case 8: {
        f = frame(p::kChannelAck); p::ChannelAckPayload v;
        v.channel_id = id; v.dial_id = dial; v.kind = d[3] % 4;
        p::encode_channel_ack(v, &f.payload); break;
    }
    case 9: {
        f = frame(p::kConnectionLifetime); p::ConnectionLifetimePayload v;
        v.channel_id = id; v.retained = d[3] % 3;
        p::encode_connection_lifetime(v, &f.payload); break;
    }
    case 10: f = incoming_request(id, dial); break;
    default: f = incoming_response(id, dial); break;
    }
    if (d[7] & 1) f.flags ^= 0x80000000u;
    if (d[7] & 2) f.version = p::kVersion;
    if (d[7] & 4) f.request_id = d[5] + 1;
    if ((d[7] & 8) && !f.payload.empty()) f.payload.resize(d[6] % f.payload.size());
    return f;
}
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    Registry::Options options;
    options.local_side = size && (data[0] & 1) ? Side::Worker : Side::Host;
    options.max_channels = 4; options.max_message_bytes = 1024;
    options.per_channel_queue_bytes = 65536;
    options.reassembly_budget_bytes = 1024 * 1024; options.send_budget_bytes = 1024 * 1024;
    options.close_timeout_ms = 100; options.dial_timeout_ms = 100;
    Registry registry(options);
    warm(registry, size && (data[0] & 2));
    // Bound per-input work independently of libFuzzer's input-length limit.
    for (size_t offset = 0, count = 0; offset + 8 <= size && count < 64; ++count) {
        const size_t n = std::min<size_t>(32, size - offset);
        const uint8_t* d = data + offset;
        if (d[0] % 16 == 10) registry.advance_time(static_cast<uint64_t>(d[3]) * 100);
        else if (d[0] % 16 == 11) registry.teardown_channel(d[1] % 16, d[3] % 6);
        else if (d[0] % 16 == 12) registry.teardown_handshake(d[2] % 16);
        else if (d[0] % 16 == 13) {
            capsid::ChannelAuthority authority;
            authority.binding = d[3] & 1; authority.binding_id = d[4];
            registry.register_authority(d[1] % 16, authority);
        } else {
            const auto before = snapshot(registry);
            const auto result = registry.apply(d[4] & 1 ? Side::Worker : Side::Host,
                                               operation(d, n));
            if (!result.accepted) check(snapshot(registry) == before);
        }
        check(registry.live_count() <= options.max_channels);
        check(registry.reassembly_reserved() <= options.reassembly_budget_bytes);
        offset += n;
    }
    registry.teardown_all(p::kCancelShutdown);
    check(!registry.live_count() && !registry.pending_dial_count() &&
          !registry.retained_owner_count() && !registry.reassembly_reserved());
    const auto closed = snapshot(registry);
    registry.teardown_all(p::kCancelShutdown);
    check(snapshot(registry) == closed);
    return 0;
}
