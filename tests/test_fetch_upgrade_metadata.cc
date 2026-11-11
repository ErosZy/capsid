#include "protocol.h"
#include "channel_io.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace p = capsid::protocol;
namespace {
constexpr uint32_t fetch_caps = p::kCapDuplexChannel | p::kCapUpgrade | p::kCapChannelLayoutA2 |
    p::kCapChannelConfirm | p::kCapChannelCloseFinal | p::kCapFetchUpgrade;
void require(bool value, const char *message) {
    if (!value) {std::cerr << "FAIL: " << message << '\n'; std::exit(1);}
}
std::vector<uint8_t> hex(const std::string &value) {
    require(value.size() % 2 == 0, "literal even hex length");
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < value.size(); i += 2) {
        bytes.push_back(static_cast<uint8_t>(std::stoul(value.substr(i, 2), nullptr, 16)));
    }
    return bytes;
}
p::ChannelOpenPayload open_request() {
    p::ChannelOpenPayload value;
    value.dial_id = 1; value.owner_kind = p::kOwnerStandalone; value.max_message_bytes = 1024;
    value.url = "http://x/"; value.channel_kind = p::kChannelKindFetchUpgrade;
    value.http_request.redirect = p::kFetchRedirectManual;
    value.http_request.headers = {{"X", "y"}};
    return value;
}
p::ChannelAcceptPayload accept_response() {
    p::ChannelAcceptPayload value;
    value.dial_id = 1; value.channel_id = 2; value.accepted_max_message_bytes = 1024;
    value.has_http_response = true; value.http_response.status = 101;
    value.http_response.status_text = "Switching Protocols"; value.http_response.url = "http://x/";
    value.http_response.headers = {{"Set-Cookie", "a=1"}, {"Set-Cookie", "b=2"}};
    return value;
}
void rejects_open(const std::vector<uint8_t> &bytes, uint32_t caps = fetch_caps) {
    p::ChannelOpenPayload value; std::string error;
    require(!p::decode_channel_open(bytes.data(), bytes.size(), &value, &error, caps), "invalid OPEN rejected");
    require(error == "invalid channel payload", "OPEN exact payload error");
}
void rejects_accept(const std::vector<uint8_t> &bytes, uint32_t caps = fetch_caps) {
    p::ChannelAcceptPayload value; std::string error;
    require(!p::decode_channel_accept(bytes.data(), bytes.size(), &value, &error, caps), "invalid ACCEPT rejected");
    require(error == "invalid channel payload", "ACCEPT exact payload error");
}

using Side = capsid::ChannelSide;
p::Frame frame(uint8_t type) {p::Frame value; value.version = p::kVersionV4; value.type = type; return value;}
p::Frame request_frame(bool fetch = true) {
    auto value = open_request();
    if (!fetch) {value.channel_kind = p::kChannelKindWebSocket; value.http_request = {};}
    auto out = frame(p::kChannelOpen);
    require(p::encode_channel_open(value, &out.payload, fetch_caps), "test OPEN encodes"); return out;
}
p::Frame response_frame(uint16_t status = 101, bool metadata = true) {
    auto value = accept_response(); value.http_response.status = status;
    if (!metadata) {value.has_http_response = false; value.http_response = {};}
    auto out = frame(p::kChannelAccept);
    require(p::encode_channel_accept(value, &out.payload, fetch_caps), "test ACCEPT encodes"); return out;
}
p::Frame ack_frame(uint32_t kind, uint32_t channel, uint64_t dial = 0) {
    p::ChannelAckPayload value; value.kind = kind; value.channel_id = channel; value.dial_id = dial;
    auto out = frame(p::kChannelAck); require(p::encode_channel_ack(value, &out.payload), "test ACK encodes"); return out;
}
p::Frame data_frame(size_t bytes = 1, bool text = false, uint16_t flags = p::kFlagStart | p::kFlagEnd) {
    auto out = frame(p::kChannelData); out.flags = flags | (text ? p::kFlagChanText : 0);
    p::append_u32(&out.payload, 2); out.payload.resize(4 + bytes, 42); return out;
}
p::Frame credit_frame(uint64_t bytes) {
    p::ChannelCreditPayload value; value.channel_id = 2; value.credit = bytes;
    auto out = frame(p::kChannelCredit); require(p::encode_channel_credit(value, &out.payload), "test CREDIT encodes"); return out;
}
p::Frame close_frame(uint8_t kind = p::kCloseFinal, uint16_t code = 0) {
    p::ChannelClosePayload value; value.channel_id = 2; value.close_kind = kind; value.code = code;
    value.flags = code ? p::kCloseFlagCodePresent : 0;
    auto out = frame(p::kChannelClose); require(p::encode_channel_close(value, &out.payload), "test CLOSE encodes"); return out;
}
struct Peers {
    capsid::ChannelRegistry registry[2];
    Peers(uint32_t host = fetch_caps, uint32_t worker = fetch_caps) {
        capsid::ChannelRegistry::Options options; options.local_side = Side::Worker;
        registry[1] = capsid::ChannelRegistry(options);
        auto hello = frame(p::kHello); hello.payload.resize(p::kHelloFixedPayloadSize);
        require(p::append_hello_a2_extension(&hello.payload, p::ChannelLimits()), "test A2 limits");
        p::append_u32(&hello.payload, host); p::append_u32(&hello.payload, 0); both(Side::Host, hello);
        auto ready = frame(p::kReady); p::append_u32(&ready.payload, worker); p::append_u32(&ready.payload, 0); both(Side::Worker, ready);
    }
    void both(Side from, const p::Frame &input) {
        // Exercise actual frame encoding/parsing in addition to the registry.
        std::vector<uint8_t> bytes; require(p::encode_versioned(p::kVersionV4, input, &bytes), "test wire encode");
        p::Parser parser(p::kVersionV4); require(parser.append(bytes.data(), bytes.size()), "test wire append");
        p::Frame parsed; require(parser.next(&parsed) == p::kParseFrame, "test wire parse");
        for (auto &endpoint : registry) {
            const auto result = endpoint.apply(from, parsed);
            if (!result.accepted) std::cerr << "type " << unsigned(parsed.type) << ": " << result.error << '\n';
            require(result.accepted, "both peers accept valid frame");
        }
    }
    void reject(Side from, const p::Frame &input, const char *error) {
        for (auto &endpoint : registry) {
            const auto live = endpoint.live_count(), pending = endpoint.pending_dial_count();
            const auto reserved = endpoint.reassembly_reserved(), messages = endpoint.messages_completed();
            capsid::ChannelEntry before; const bool exists = endpoint.find(2, &before);
            const auto result = endpoint.apply(from, input);
            require(!result.accepted && result.error == error, "both peers reject with exact error");
            require(endpoint.live_count() == live && endpoint.pending_dial_count() == pending &&
                endpoint.reassembly_reserved() == reserved && endpoint.messages_completed() == messages, "rejection preserves reservations/correlation");
            if (exists) {capsid::ChannelEntry after; require(endpoint.find(2, &after) && before.phase == after.phase &&
                before.direction[0].credit == after.direction[0].credit && before.direction[1].credit == after.direction[1].credit &&
                before.direction[0].message_open == after.direction[0].message_open, "rejection preserves phase/credit/assembly");}
        }
    }
    void opened(uint16_t status) {
        both(Side::Worker, request_frame()); both(Side::Host, response_frame(status));
        reject(Side::Host, data_frame(), "channel sequence");
        both(Side::Worker, ack_frame(p::kAckDialResult, 2, 1));
        for (auto &endpoint : registry) {capsid::ChannelEntry entry; require(endpoint.find(2, &entry) && entry.fetch_upgrade &&
            entry.http_status == status && entry.channel_kind == (status == 101 ? p::kChannelKindWebSocket : p::kChannelKindHttpBody), "accepted kind follows actual HTTP status");}
    }
    void empty() {for (const auto &endpoint : registry) require(endpoint.live_count() == 0 && endpoint.pending_dial_count() == 0 &&
        endpoint.reassembly_reserved() == 0, "terminal ACK releases channel/correlation/reservations");}
};
void registry_tests() {
    for (bool missing_on_host : {false, true}) {
        Peers peers(missing_on_host ? fetch_caps & ~p::kCapFetchUpgrade : fetch_caps,
                    missing_on_host ? fetch_caps : fetch_caps & ~p::kCapFetchUpgrade);
        peers.reject(Side::Worker, request_frame(), "invalid channel payload"); peers.empty();
    }
    for (bool fetch : {false, true}) {
        Peers peers; peers.both(Side::Worker, request_frame(fetch));
        peers.reject(Side::Host, response_frame(101, !fetch), "invalid channel payload");
        peers.both(Side::Host, response_frame(101, fetch)); peers.both(Side::Worker, ack_frame(p::kAckDialResult, 2, 1));
        peers.both(Side::Worker, data_frame(1, true)); peers.both(Side::Host, data_frame(1, true));
    }
    {
        Peers peers; peers.reject(Side::Host, request_frame(), "channel open direction");
        auto forged = request_frame(); forged.payload[13] = p::kChannelKindHttpBody;
        peers.reject(Side::Worker, forged, "invalid channel payload"); peers.empty();
    }
    for (uint16_t status : {uint16_t(200), uint16_t(302), uint16_t(409), uint16_t(599)}) {
        Peers peers; peers.opened(status);
        peers.reject(Side::Worker, data_frame(), "channel sequence");
        peers.reject(Side::Host, data_frame(1, true), "channel sequence");
        peers.reject(Side::Host, credit_frame(1), "channel sequence");
        peers.reject(Side::Worker, close_frame(), "channel sequence");
        peers.reject(Side::Host, close_frame(p::kCloseInitiated), "channel sequence");
        peers.reject(Side::Host, close_frame(p::kCloseFinal, 1000), "channel sequence");
        for (size_t i = 0; i < 64; ++i) peers.both(Side::Host, data_frame(1024));
        peers.reject(Side::Host, data_frame(), "channel credit underflow");
        peers.both(Side::Worker, credit_frame(1024)); peers.both(Side::Host, data_frame(1024));
        peers.both(Side::Host, close_frame());
        peers.reject(Side::Host, data_frame(), "channel sequence");
        peers.both(Side::Worker, ack_frame(p::kAckTransportClose, 2)); peers.empty();
    }
    for (uint16_t status : {uint16_t(204), uint16_t(205), uint16_t(304)}) {
        Peers peers; peers.opened(status); peers.reject(Side::Host, data_frame(0), "channel sequence");
        peers.both(Side::Host, close_frame()); peers.both(Side::Worker, ack_frame(p::kAckTransportClose, 2)); peers.empty();
    }
    {
        Peers peers; peers.opened(409); peers.both(Side::Host, data_frame(1, false, p::kFlagStart));
        peers.reject(Side::Host, close_frame(), "channel sequence");
        peers.both(Side::Host, data_frame(1, false, p::kFlagEnd));
        peers.both(Side::Host, close_frame()); peers.both(Side::Worker, ack_frame(p::kAckTransportClose, 2)); peers.empty();
    }
    {
        Peers peers(fetch_caps | p::kCapHalfClose, fetch_caps | p::kCapHalfClose); peers.opened(409);
        auto half = frame(p::kChannelHalfClose); p::append_u32(&half.payload, 2);
        peers.reject(Side::Worker, half, "channel sequence"); peers.reject(Side::Host, half, "channel sequence");
        peers.both(Side::Host, close_frame(p::kCloseTransport, 1006));
        peers.both(Side::Worker, ack_frame(p::kAckTransportClose, 2)); peers.empty();
    }
    // ACCEPT and cancellation can cross on independent FIFO streams. Metadata
    // must not revive the cancelled endpoint or leave an assigned body channel.
    for (uint16_t status : {uint16_t(101), uint16_t(409)}) {
        Peers peers; peers.both(Side::Worker, request_frame());
        require(peers.registry[0].apply(Side::Host, response_frame(status)).accepted, "host provisional result");
        p::ChannelCancelPayload value; value.dial_id = 1;
        auto cancel = frame(p::kChannelCancel); require(p::encode_channel_cancel(value, &cancel.payload), "test CANCEL encodes");
        peers.both(Side::Worker, cancel);
        const auto result = peers.registry[1].apply(Side::Host, response_frame(status));
        require(result.accepted && result.discard, "crossed metadata result is drained without delivery");
        peers.both(Side::Host, ack_frame(p::kAckCancel, 0, 1)); peers.empty();
    }
    // Refuse invalid producers before queue allocation, preserving owned quota.
    for (Side local : {Side::Host, Side::Worker}) {
        capsid::ChannelRegistry::Options options; options.local_side = local; capsid::ChannelIO io(options);
        const auto deliver = [&](Side from, const p::Frame &input) {return from == local ? io.commit_local(input) : io.receive(input);};
        auto hello = frame(p::kHello); hello.payload.resize(p::kHelloFixedPayloadSize);
        require(p::append_hello_a2_extension(&hello.payload, p::ChannelLimits()), "IO test limits");
        p::append_u32(&hello.payload, fetch_caps); p::append_u32(&hello.payload, 0); require(deliver(Side::Host, hello).accepted, "IO HELLO");
        auto ready = frame(p::kReady); p::append_u32(&ready.payload, fetch_caps); p::append_u32(&ready.payload, 0);
        require(deliver(Side::Worker, ready).accepted && deliver(Side::Worker, request_frame()).accepted &&
            deliver(Side::Host, response_frame(409)).accepted && deliver(Side::Worker, ack_frame(p::kAckDialResult, 2, 1)).accepted, "IO body opens");
        std::string error; const uint8_t byte = 7;
        require(!io.queue_message(2, &byte, 1, true, &error) && error == "channel sequence" && io.send_reserved() == 0, "body rejects text before allocation");
        require(io.queue_message(2, &byte, 1, false, &error) == (local == Side::Host), "body queue enforces producer direction");
        if (local == Side::Worker) require(io.send_reserved() == 0, "invalid producer retains no quota");
        io.teardown_all(p::kCancelShutdown); require(io.send_reserved() == 0 && io.receive_reserved() == 0, "IO teardown returns all owned quota");
    }
}
}

int main() {
    // Immutable wire examples, written from the specified field widths and
    // little-endian layout rather than generated by the production encoder.
    const auto open_bytes = hex("000000000100000000000000040100000000000000000000000400000000000009000000687474703a2f2f782f0000010001000100010058010079");
    const auto accept_bytes = hex("010000000000000002000000000400000000000000000100650000001300537769746368696e672050726f746f636f6c730900687474703a2f2f782f02000a005365742d436f6f6b69650300613d310a005365742d436f6f6b69650300623d32");
    require(p::capabilities_satisfy_closure(fetch_caps), "complete fetch capability closure");
    for (auto dependency : {p::kCapDuplexChannel, p::kCapUpgrade, p::kCapChannelLayoutA2,
            p::kCapChannelConfirm, p::kCapChannelCloseFinal}) {
        require(!p::capabilities_satisfy_closure(fetch_caps & ~dependency), "every fetch dependency required");
    }
    require(p::capabilities_satisfy_closure(fetch_caps | p::kCapConnectionLifetime), "allocated lifetime capability preserves fetch closure");
    require(!p::capabilities_satisfy_closure(fetch_caps | (1u << 8)), "unallocated capability remains invalid");
    std::vector<uint8_t> encoded; std::string error;
    require(p::encode_channel_open(open_request(), &encoded, fetch_caps) && encoded == open_bytes, "OPEN exact independent bytes");
    require(p::encode_channel_accept(accept_response(), &encoded, fetch_caps) && encoded == accept_bytes, "ACCEPT exact independent bytes");
    p::ChannelOpenPayload request;
    p::ChannelAcceptPayload response;
    require(p::decode_channel_open(open_bytes.data(), open_bytes.size(), &request, &error, fetch_caps), "OPEN literal decodes");
    require(request.channel_kind == p::kChannelKindFetchUpgrade && request.http_request.redirect == p::kFetchRedirectManual &&
        request.http_request.headers == open_request().http_request.headers, "OPEN metadata fields");
    require(p::decode_channel_accept(accept_bytes.data(), accept_bytes.size(), &response, &error, fetch_caps), "ACCEPT literal decodes");
    require(response.has_http_response && response.http_response.status == 101 && response.http_response.headers == accept_response().http_response.headers,
        "response duplicate fields retained");
    rejects_open(open_bytes, 0); rejects_accept(accept_bytes, 0);
    rejects_open(open_bytes, fetch_caps & ~p::kCapFetchUpgrade); rejects_accept(accept_bytes, fetch_caps & ~p::kCapFetchUpgrade);
    require(!p::encode_channel_open(open_request(), &encoded), "legacy encoder does not emit new OPEN");
    require(!p::encode_channel_accept(accept_response(), &encoded), "legacy encoder does not emit metadata");
    for (size_t size = 0; size < open_bytes.size(); ++size) rejects_open({open_bytes.begin(), open_bytes.begin() + size});
    for (size_t size = 0; size < accept_bytes.size(); ++size) {
        // Exactly the old22-byte ACCEPT is legal at the codec layer. The
        // registry must reject it when the matching dial requires metadata.
        if (size != 22) rejects_accept({accept_bytes.begin(), accept_bytes.begin() + size});
    }
    auto legacy_accept = std::vector<uint8_t>(accept_bytes.begin(), accept_bytes.begin() + 22);
    require(p::decode_channel_accept(legacy_accept.data(), legacy_accept.size(), &response, &error, fetch_caps) && !response.has_http_response &&
        response.http_response.status == 0 && response.http_response.headers.empty(), "reuse clears decoded response metadata");
    require(p::encode_channel_accept(response, &encoded) && encoded == legacy_accept, "legacy ACCEPT stays byte exact");
    auto ordinary = open_request(); ordinary.channel_kind = p::kChannelKindWebSocket; ordinary.http_request = {};
    require(p::encode_channel_open(ordinary, &encoded), "legacy OPEN encodes without metadata capability");
    require(p::decode_channel_open(encoded.data(), encoded.size(), &request, &error, fetch_caps) && request.http_request.headers.empty() &&
        request.http_request.redirect == p::kFetchRedirectFollow, "reuse clears decoded request metadata");
    for (const auto &name : {std::string(), std::string("bad name"), std::string("bad:field"), std::string("x\r\ny"), std::string(1, '\x80')}) {
        auto value = open_request(); value.http_request.headers = {{name, "value"}};
        require(!p::encode_channel_open(value, &encoded, fetch_caps), "invalid HTTP field name rejected");
    }
    for (unsigned int byte : {0u, 1u, 10u, 13u, 31u, 127u}) {
        auto value = open_request(); value.http_request.headers[0].second.assign(1, static_cast<char>(byte));
        require(!p::encode_channel_open(value, &encoded, fetch_caps), "invalid HTTP value control rejected");
    }
    auto octets = open_request(); octets.http_request.headers[0].second = std::string("\xff\tZ", 3);
    require(p::encode_channel_open(octets, &encoded, fetch_caps) &&
        p::decode_channel_open(encoded.data(), encoded.size(), &request, &error, fetch_caps) && request.http_request.headers == octets.http_request.headers,
        "ByteString octets and horizontal tab retained without UTF-8 conversion");
    auto too_many = open_request(); too_many.http_request.headers.assign(129, {"x", "y"});
    require(!p::encode_channel_open(too_many, &encoded, fetch_caps), "header count bound");
    auto boundary = open_request(); boundary.http_request.headers = {{"x", std::string(p::kMaxChannelHttpHeaderBytes - 7, 'a')}};
    require(p::encode_channel_open(boundary, &encoded, fetch_caps), "exact header byte bound admitted");
    boundary.http_request.headers[0].second.push_back('a');
    require(!p::encode_channel_open(boundary, &encoded, fetch_caps), "one excess header byte rejected");
    for (unsigned int status : {0u, 100u, 102u, 199u, 600u, 65535u}) {
        auto value = accept_response(); value.http_response.status = static_cast<uint16_t>(status);
        require(!p::encode_channel_accept(value, &encoded, fetch_caps), "invalid final HTTP status rejected");
    }
    for (unsigned int status : {200u, 204u, 302u, 409u, 599u}) {
        auto value = accept_response(); value.http_response.status = static_cast<uint16_t>(status);
        require(p::encode_channel_accept(value, &encoded, fetch_caps), "ordinary HTTP response status retained");
    }
    for (size_t offset : {size_t(47), size_t(48), size_t(50)}) {
        auto value = open_bytes; value[offset] = 255; rejects_open(value);
    }
    for (size_t offset : {size_t(22), size_t(23), size_t(26), size_t(27)}) {
        auto value = accept_bytes; value[offset] = 255; rejects_accept(value);
    }
    auto extra = open_bytes; extra.push_back(0); rejects_open(extra);
    extra = accept_bytes; extra.push_back(0); rejects_accept(extra);
    auto bad_name = open_bytes; bad_name[55] = ':'; rejects_open(bad_name);
    auto bad_value = open_bytes; bad_value[58] = '\n'; rejects_open(bad_value);
    auto interim_status = accept_bytes; interim_status[24] = 100; rejects_accept(interim_status);
    auto missing_kind = open_request(); missing_kind.channel_kind = p::kChannelKindWebSocket;
    require(!p::encode_channel_open(missing_kind, &encoded, fetch_caps), "legacy OPEN cannot silently lose HTTP fields");
    auto missing_flag = accept_response(); missing_flag.has_http_response = false;
    require(!p::encode_channel_accept(missing_flag, &encoded, fetch_caps), "ACCEPT cannot silently lose HTTP metadata");
    registry_tests();
    std::cout << "PASS: negotiated Upgrade fetch metadata, exact legacy bytes, duplicate headers, bounds, ACK/cancel races and one-way body credit\n";
}
