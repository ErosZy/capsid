#include "channel_registry.h"
#include "protocol.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>

namespace {
namespace p = capsid::protocol;
using Side = capsid::ChannelSide;

void require(bool ok, const char *what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

// Each endpoint applies its own writes immediately. Reads are scheduled
// independently, preserving FIFO within each direction, never imposing a
// single global order on the two copies of the state machine.
struct Peers {
    capsid::ChannelRegistry registry[2];
    std::deque<p::Frame> outgoing[2];
    explicit Peers(uint32_t caps = 15, uint32_t max_channels = 128) {
        capsid::ChannelRegistry::Options options;
        options.max_channels = max_channels;
        registry[0] = capsid::ChannelRegistry(options);
        options.local_side = Side::Worker;
        registry[1] = capsid::ChannelRegistry(options);
        p::Frame hello;
        hello.version = p::kVersionV4;
        hello.type = p::kHello;
        hello.payload.resize(p::kHelloFixedPayloadSize);
        if (caps & p::kCapChannelLayoutA2)
            require(p::append_hello_a2_extension(&hello.payload, p::ChannelLimits()), "limits encode");
        p::append_u32(&hello.payload, caps);
        p::append_u32(&hello.payload, 0);
        send(Side::Host, hello);
        receive(Side::Worker);
        p::Frame ready;
        ready.version = p::kVersionV4;
        ready.type = p::kReady;
        p::append_u32(&ready.payload, caps);
        p::append_u32(&ready.payload, 0);
        send(Side::Worker, ready);
        receive(Side::Host);
    }
    capsid::ChannelRegistry::ApplyResult send(Side from, const p::Frame &frame) {
        std::vector<uint8_t> bytes;
        require(p::encode_versioned(p::kVersionV4, frame, &bytes), "wire encode");
        p::Parser parser(p::kVersionV4);
        require(parser.append(bytes.data(), bytes.size()), "wire append");
        p::Frame decoded;
        require(parser.next(&decoded) == p::kParseFrame, "wire parse");
        const auto result = registry[static_cast<int>(from)].apply(from, decoded);
        if (!result.accepted) std::fprintf(stderr, "send type %u: %s\n", frame.type, result.error.c_str());
        require(result.accepted, "sender accepts its write");
        outgoing[static_cast<int>(from)].push_back(decoded);
        return result;
    }
    capsid::ChannelRegistry::ApplyResult receive(Side to) {
        const int from = 1 - static_cast<int>(to);
        require(!outgoing[from].empty(), "read has queued frame");
        const auto result = registry[static_cast<int>(to)].apply(static_cast<Side>(from), outgoing[from].front());
        if (!result.accepted) std::fprintf(stderr, "receive type %u: %s\n", outgoing[from].front().type, result.error.c_str());
        require(result.accepted, "receiver accepts peer's ordered write");
        outgoing[from].pop_front();
        return result;
    }
    void dial(uint64_t id = 1) {
        p::ChannelOpenPayload dial;
        dial.dial_id = id;
        dial.owner_kind = p::kOwnerStandalone;
        dial.max_message_bytes = 1024 * 1024;
        dial.url = "ws://race.test/";
        p::Frame frame;
        frame.version = p::kVersionV4;
        frame.type = p::kChannelOpen;
        require(p::encode_channel_open(dial, &frame.payload), "OPEN encode");
        send(Side::Worker, frame);
        receive(Side::Host);
    }
    void accept(uint64_t dial_id = 1, uint32_t channel_id = 1) {
        p::ChannelAcceptPayload accept;
        accept.dial_id = dial_id;
        accept.channel_id = channel_id;
        accept.accepted_max_message_bytes = 1024 * 1024;
        p::Frame frame;
        frame.version = p::kVersionV4;
        frame.type = p::kChannelAccept;
        require(p::encode_channel_accept(accept, &frame.payload), "ACCEPT encode");
        send(Side::Host, frame);
    }
    void open() {
        dial();
        accept();
        receive(Side::Worker);
        if (registry[0].effective_capabilities() & p::kCapChannelConfirm) {
            p::ChannelAckPayload payload;
            payload.channel_id = 1;
            payload.dial_id = 1;
            p::Frame ack;
            ack.version = p::kVersionV4;
            ack.type = p::kChannelAck;
            require(p::encode_channel_ack(payload, &ack.payload), "result ACK encode");
            send(Side::Worker, ack);
            receive(Side::Host);
        }
    }
    void error(Side from, const p::Frame &f, const char *expected) {
        const auto result = registry[static_cast<int>(from)].apply(from, f);
        require(!result.accepted && result.error == expected, "illegal write fails with exact error");
    }
    void empty() {
        for (int side = 0; side < 2; ++side) {
            require(registry[side].live_count() == 0, "live channels reclaimed");
            require(registry[side].pending_dial_count() == 0, "correlations reclaimed");
            require(registry[side].reassembly_reserved() == 0, "assembly reservations reclaimed");
            require(outgoing[side].empty(), "transport drained");
        }
    }
};

p::Frame data(bool start, bool end) {
    p::Frame f;
    f.version = p::kVersionV4;
    f.type = p::kChannelData;
    f.flags = (start ? p::kFlagStart : 0) | (end ? p::kFlagEnd : 0);
    p::append_u32(&f.payload, 1);
    f.payload.push_back('a');
    return f;
}
p::Frame close() {
    p::ChannelClosePayload payload;
    payload.channel_id = 1;
    payload.close_kind = p::kCloseInitiated;
    payload.flags = p::kCloseFlagCodePresent;
    payload.code = 1000;
    p::Frame f;
    f.version = p::kVersionV4;
    f.type = p::kChannelClose;
    require(p::encode_channel_close(payload, &f.payload), "Close encode");
    return f;
}

p::Frame cancel(uint64_t dial_id = 0) {
    p::ChannelCancelPayload payload;
    payload.channel_id = dial_id ? 0 : 1;
    payload.dial_id = dial_id;
    p::Frame f;
    f.version = p::kVersionV4;
    f.type = p::kChannelCancel;
    require(p::encode_channel_cancel(payload, &f.payload), "CANCEL encode");
    return f;
}
p::Frame ack(uint32_t kind, uint64_t dial_id = 0, uint32_t channel_id = 1) {
    p::ChannelAckPayload payload;
    payload.channel_id = channel_id;
    payload.dial_id = dial_id;
    payload.kind = kind;
    p::Frame f;
    f.version = p::kVersionV4;
    f.type = p::kChannelAck;
    require(p::encode_channel_ack(payload, &f.payload), "ACK encode");
    return f;
}
p::Frame reject(uint64_t dial_id) {
    p::ChannelRejectPayload payload;
    payload.dial_id = dial_id;
    payload.reason_code = 8;
    p::Frame f;
    f.version = p::kVersionV4;
    f.type = p::kChannelReject;
    require(p::encode_channel_reject(payload, &f.payload), "REJECT encode");
    return f;
}

void close_crosses_fragmented_message() {
    Peers peers;
    peers.open();
    peers.send(Side::Worker, data(true, false));
    peers.receive(Side::Host);
    peers.send(Side::Worker, data(false, true));
    peers.send(Side::Host, close());
    peers.receive(Side::Worker);
    peers.receive(Side::Host);
    peers.send(Side::Worker, close());
    peers.receive(Side::Host);
    peers.empty();
}

void pending_cancel_crosses_result(bool accepted, bool cancel_first, bool read_result_first) {
    Peers peers(31);
    // Use a u64 id outside the channel-id space in every cancellation.
    const uint64_t dial_id = (uint64_t(1) << 32) + 7;
    peers.dial(dial_id);
    if (cancel_first) peers.send(Side::Worker, cancel(dial_id));
    if (accepted) peers.accept(dial_id);
    else peers.send(Side::Host, reject(dial_id));
    if (!cancel_first) peers.send(Side::Worker, cancel(dial_id));
    if (accepted) peers.error(Side::Host, data(true, true), "channel sequence");
    if (read_result_first) require(peers.receive(Side::Worker).discard, "cancelled result never opens JS socket");
    peers.receive(Side::Host);
    peers.send(Side::Host, ack(p::kAckCancel, dial_id, 0));
    if (!read_result_first) require(peers.receive(Side::Worker).discard, "queued result is drained without delivery");
    peers.receive(Side::Worker);
    peers.empty();
    peers.error(Side::Worker, ack(p::kAckDialResult, dial_id, accepted ? 1 : 0), "unknown channel");
    peers.error(Side::Host, ack(p::kAckCancel, dial_id, 0), "unknown channel");
}

void pending_cancel_before_result() {
    Peers peers(31);
    peers.dial();
    peers.send(Side::Worker, cancel(1));
    peers.receive(Side::Host);
    peers.error(Side::Host, reject(1), "channel sequence");
    peers.send(Side::Host, ack(p::kAckCancel, 1, 0));
    peers.receive(Side::Worker);
    peers.empty();
}

void normal_result_confirmation() {
    Peers peers(31);
    peers.dial();
    peers.accept();
    peers.error(Side::Host, data(true, true), "channel sequence");
    peers.error(Side::Worker, ack(p::kAckDialResult, 1, 9), "channel sequence");
    peers.receive(Side::Worker);
    peers.error(Side::Worker, data(true, true), "channel sequence");
    peers.error(Side::Worker, cancel(1), "channel sequence");
    peers.send(Side::Worker, ack(p::kAckDialResult, 1));
    peers.send(Side::Worker, data(true, true));
    peers.receive(Side::Host); // result ACK precedes every worker DATA
    peers.receive(Side::Host);
    peers.send(Side::Host, close());
    peers.receive(Side::Worker);
    peers.send(Side::Worker, close());
    peers.receive(Side::Host);
    peers.empty();
    peers.dial(2);
    peers.send(Side::Host, reject(2));
    peers.receive(Side::Worker);
    peers.send(Side::Worker, ack(p::kAckDialResult, 2, 0));
    peers.receive(Side::Host);
    peers.empty();
}

void assigned_cancel_crosses_data(Side initiator, bool transport) {
    Peers peers(31);
    peers.open();
    const Side other = initiator == Side::Host ? Side::Worker : Side::Host;
    peers.send(other, data(true, false));
    peers.receive(initiator);
    peers.send(other, data(false, true));
    p::Frame terminal = cancel();
    const uint32_t kind = transport ? p::kAckTransportClose : p::kAckCancel;
    if (transport) {
        terminal = close();
        terminal.payload[4] = p::kCloseTransport;
        terminal.payload[6] = uint8_t(1006 & 255);
        terminal.payload[7] = uint8_t(1006 >> 8);
    }
    peers.send(initiator, terminal);
    peers.receive(other);
    peers.error(other, data(true, true), "channel sequence");
    peers.send(other, ack(kind));
    require(peers.receive(initiator).discard, "queued DATA is drained after terminal marker");
    peers.receive(initiator);
    peers.empty();
}

void simultaneous_cancel() {
    Peers peers(31);
    peers.open();
    peers.send(Side::Host, data(true, true));
    peers.send(Side::Worker, data(true, true));
    peers.send(Side::Host, cancel());
    peers.send(Side::Worker, cancel());
    require(peers.receive(Side::Host).discard, "host drains crossed DATA");
    require(peers.receive(Side::Worker).discard, "worker drains crossed DATA");
    peers.receive(Side::Host);
    peers.receive(Side::Worker);
    peers.send(Side::Host, ack(p::kAckCancel));
    peers.send(Side::Worker, ack(p::kAckCancel));
    require(peers.registry[0].live_count() == 1 && peers.registry[1].live_count() == 1,
            "both endpoints retain correlation until their own cancellation is confirmed");
    peers.receive(Side::Host);
    peers.receive(Side::Worker);
    peers.empty();
}

void close_crosses_cancel() {
    Peers peers(31);
    peers.open();
    peers.send(Side::Host, close());
    peers.error(Side::Host, cancel(), "channel sequence"); // no terminal frames after own Close
    peers.send(Side::Worker, cancel());
    peers.receive(Side::Host);
    peers.receive(Side::Worker);
    peers.send(Side::Host, ack(p::kAckCancel));
    peers.receive(Side::Worker);
    peers.empty();
}

void final_close_is_atomic(Side initiator, bool peer_close_first) {
    // Independent literal wire values: capability bit 5 and close kind 3.
    // One marker carries the accepted Close and the terminal barrier, so a
    // crossed ordinary peer Close cannot retire the id before the terminal.
    p::ChannelClosePayload payload;
    payload.channel_id = 1;
    payload.close_kind = 3;
    payload.flags = p::kCloseFlagCodePresent;
    payload.code = 1000;
    payload.reason = "done";
    p::Frame final; final.version = 4; final.type = p::kChannelClose;
    require(p::encode_channel_close(payload, &final.payload), "atomic final Close encodes");
    p::ChannelClosePayload decoded;
    std::string error;
    require(p::decode_channel_close(final.payload.data(), final.payload.size(), &decoded, &error) &&
            decoded.close_kind == 3 && decoded.code == 1000 && decoded.reason == "done",
            "atomic final Close preserves sendable code and reason");
    Peers peers(63); peers.open();
    const Side other = initiator == Side::Host ? Side::Worker : Side::Host;
    if (peer_close_first) peers.send(other, close());
    peers.send(initiator, final);
    peers.error(initiator, final, "channel sequence");
    peers.error(initiator, data(true, true), "channel sequence");
    peers.receive(other);
    peers.error(other, close(), "channel sequence");
    peers.send(other, ack(p::kAckTransportClose));
    if (peer_close_first) peers.receive(initiator);
    peers.receive(initiator);
    peers.empty();
    Peers legacy(31); legacy.open();
    legacy.error(initiator, final, "channel frame without capability");
    Peers already_closed(63); already_closed.open();
    already_closed.send(initiator, close());
    already_closed.error(initiator, final, "channel sequence");
    payload.code = 1006;
    require(!p::encode_channel_close(payload, &final.payload), "final Close cannot put 1006 on the network");
    require(!p::capabilities_satisfy_closure(32 | 8 | 1), "final Close requires confirmation capability");
}

void half_close_keeps_control_stream() {
    Peers peers(31);
    peers.open();
    p::Frame half;
    half.version = p::kVersionV4;
    half.type = p::kChannelHalfClose;
    p::append_u32(&half.payload, 1);
    peers.send(Side::Worker, data(true, true));
    peers.send(Side::Worker, half);
    peers.send(Side::Host, half);
    peers.receive(Side::Host);
    p::Frame credit;
    credit.version = p::kVersionV4;
    credit.type = p::kChannelCredit;
    p::ChannelCreditPayload grant;
    grant.channel_id = 1;
    grant.credit = 1;
    require(p::encode_channel_credit(grant, &credit.payload), "CREDIT encode");
    peers.send(Side::Host, credit); // allowed after half-close, before full Close
    peers.receive(Side::Host);
    peers.receive(Side::Worker);
    peers.receive(Side::Worker);
    require(peers.registry[0].live_count() == 1 && peers.registry[1].live_count() == 1,
            "half-close ends DATA, not queued control frames");
    peers.send(Side::Host, close());
    peers.send(Side::Worker, close());
    peers.receive(Side::Host);
    peers.receive(Side::Worker);
    peers.empty();
}

void simultaneous_final_close() {
    Peers peers(63); peers.open();
    auto final = close(); final.payload[4] = 3;
    peers.send(Side::Host, final); peers.send(Side::Worker, final);
    peers.receive(Side::Host); peers.receive(Side::Worker);
    peers.send(Side::Host, ack(p::kAckTransportClose));
    peers.send(Side::Worker, ack(p::kAckTransportClose));
    require(peers.registry[0].live_count() == 1 && peers.registry[1].live_count() == 1,
            "simultaneous final Closes retain both outstanding terminal barriers");
    peers.receive(Side::Host); peers.receive(Side::Worker); peers.empty();
}

void deadline_and_unknown_guards() {
    Peers peers(31);
    peers.open();
    peers.error(Side::Worker, ack(p::kAckCancel), "channel sequence");
    peers.error(Side::Host, cancel(19), "channel open direction");
    peers.error(Side::Worker, cancel(19), "unknown channel");
    peers.send(Side::Worker, cancel());
    require(peers.registry[1].advance_time(4999), "confirmation deadline not early");
    require(!peers.registry[1].advance_time(1), "missing ACK expires IPC at configured bound");
    require(peers.registry[1].live_count() == 0 && peers.registry[1].reassembly_reserved() == 0,
            "expired IPC releases held state");
    peers.error(Side::Worker, data(true, true), "channel confirmation timeout");
    Peers legacy;
    legacy.open();
    legacy.error(Side::Worker, cancel(1), "channel frame without capability");
    legacy.error(Side::Worker, ack(p::kAckCancel), "channel frame without capability");
}

void rejected_results_are_bounded() {
    Peers peers(31, 1);
    // Repeat real two-peer lifecycles: no tombstone accumulation and ids
    // outside u32 remain independent of the host's channel high-water.
    for (uint64_t id = 1; id <= 1000; ++id) {
        peers.dial((uint64_t(1) << 32) + id);
        peers.send(Side::Host, reject((uint64_t(1) << 32) + id));
        peers.receive(Side::Worker);
        peers.send(Side::Worker, ack(p::kAckDialResult, (uint64_t(1) << 32) + id, 0));
        peers.receive(Side::Host);
        peers.empty();
    }
}

void crossed_result_cannot_extend_cancel_deadline() {
    for (bool accepted : {false, true}) {
        Peers peers(31);
        peers.dial();
        peers.send(Side::Worker, cancel(1));
        if (accepted) peers.accept();
        else peers.send(Side::Host, reject(1));
        require(peers.registry[1].advance_time(4999), "pending cancel approaches its original deadline");
        require(peers.receive(Side::Worker).discard, "late result remains suppressed");
        require(!peers.registry[1].advance_time(1), "crossed result cannot restart cancellation's bounded deadline");
        require(peers.registry[1].live_count() == 0 && peers.registry[1].pending_dial_count() == 0,
                "expired provisional result and correlation reclaimed together");
    }
}
} // namespace

int main() {
    close_crosses_fragmented_message();
    for (int accepted = 0; accepted < 2; ++accepted)
        for (int cancel_first = 0; cancel_first < 2; ++cancel_first)
            for (int read_first = 0; read_first < 2; ++read_first)
                pending_cancel_crosses_result(accepted, cancel_first, read_first);
    pending_cancel_before_result();
    normal_result_confirmation();
    for (int side = 0; side < 2; ++side)
        for (int transport = 0; transport < 2; ++transport)
            assigned_cancel_crosses_data(static_cast<Side>(side), transport);
    simultaneous_cancel();
    close_crosses_cancel();
    for (int side = 0; side < 2; ++side)
        for (bool crossed : {false, true})
            final_close_is_atomic(static_cast<Side>(side), crossed);
    half_close_keeps_control_stream();
    simultaneous_final_close();
    deadline_and_unknown_guards();
    rejected_results_are_bounded();
    crossed_result_cannot_extend_cancel_deadline();
    std::puts("two-peer channel races PASS");
}
