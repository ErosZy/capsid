// Channel registry conformance (GLM plan T06).
//
// The PRODUCTION registry (src/channel_registry.cc) must run the same
// frozen sequences as the T05 reference model (via the shared driver
// in fetchrpc_v4_sequences.h) and additionally satisfy the registry-
// specific guarantees: budget reserve-before-allocate, aggregate
// quotas, zero-credit control progress, teardown idempotence, and
// counters returning to zero after terminal transitions.
#include "channel_registry.h"
#include "fetchrpc_v4_sequences.h"
#include "protocol.h"

#include <jansson.h>

#include <cstdio>
#include <fstream>
#include <map>
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

// SequenceTarget adapter over the production registry.
class RegistryTarget : public fetchrpc_sequences::SequenceTarget {
public:
    void reset() override { registry_ = capsid::ChannelRegistry(); }
    std::string apply(const std::string &dir,
                      const capsid::protocol::Frame &frame) override {
        const capsid::ChannelSide side = dir == "h2w"
            ? capsid::ChannelSide::Host
            : capsid::ChannelSide::Worker;
        const capsid::ChannelRegistry::ApplyResult result =
            registry_.apply(side, frame);
        return result.accepted ? "ok" : "error:" + result.error;
    }

    capsid::ChannelRegistry registry_;
};

capsid::protocol::Frame parse_frame(
        const std::vector<uint8_t> &wire) {
    capsid::protocol::Parser parser(4);
    capsid::protocol::Frame frame;
    parser.append(wire.data(), wire.size());
    if (parser.next(&frame) != capsid::protocol::kParseFrame) {
        frame.type = 0;
    }
    return frame;
}

// negotiation shortcut for the unit tests
void negotiate_full(capsid::ChannelRegistry *registry, uint32_t capabilities = 15,
                    uint32_t max_channels = 128) {
    namespace p = capsid::protocol;
    const uint32_t caps = capabilities;
    capsid::protocol::Frame hello;
    hello.type = p::kHello;
    hello.version = p::kVersionV4;
    hello.payload =
        fetchrpc_sequences::canonical_startup_payload();
    p::ChannelLimits limits;
    limits.max_channels = max_channels;
    limits.max_message_bytes = 1024 * 1024;
    limits.reassembly_budget_bytes = 16 * 1024 * 1024;
    limits.send_budget_bytes = 16 * 1024 * 1024;
    limits.close_timeout_ms = 5000;
    limits.dial_timeout_ms = 10000;
    limits.idle_timeout_ms = 0;
    limits.per_channel_queue_bytes = 4 * 1024 * 1024;
    p::append_hello_a2_extension(&hello.payload, limits);
    p::append_u32(&hello.payload, caps);
    p::append_u32(&hello.payload, 0);
    registry->apply(capsid::ChannelSide::Host, hello);

    capsid::protocol::Frame ready;
    ready.type = p::kReady;
    ready.version = p::kVersionV4;
    ready.payload.assign(20, 0);
    p::append_u32(&ready.payload, caps);
    p::append_u32(&ready.payload, 0);
    registry->apply(capsid::ChannelSide::Worker, ready);
}

capsid::protocol::Frame data_frame(uint32_t channel,
                                   const std::vector<uint8_t> &message,
                                   uint32_t flags) {
    namespace p = capsid::protocol;
    capsid::protocol::Frame frame;
    frame.type = p::kChannelData;
    frame.version = p::kVersionV4;
    frame.flags = flags;
    p::append_u32(&frame.payload, channel);
    frame.payload.insert(frame.payload.end(), message.begin(),
                         message.end());
    return frame;
}

capsid::protocol::Frame credit_frame(uint32_t channel,
                                     uint64_t amount) {
    namespace p = capsid::protocol;
    p::ChannelCreditPayload grant;
    grant.channel_id = channel;
    grant.credit = amount;
    capsid::protocol::Frame frame;
    frame.type = p::kChannelCredit;
    frame.version = p::kVersionV4;
    p::encode_channel_credit(grant, &frame.payload);
    return frame;
}

capsid::protocol::Frame open_frame(uint64_t dial,
                                   uint64_t max_message) {
    namespace p = capsid::protocol;
    p::ChannelOpenPayload open;
    open.channel_id = 0;
    open.dial_id = dial;
    open.owner_kind = p::kOwnerStandalone;
    open.max_message_bytes = max_message;
    open.url = "wss://unit.test/echo";
    capsid::protocol::Frame frame;
    frame.type = p::kChannelOpen;
    frame.version = p::kVersionV4;
    p::encode_channel_open(open, &frame.payload);
    return frame;
}

capsid::protocol::Frame accept_frame(uint64_t dial, uint32_t channel) {
    namespace p = capsid::protocol;
    p::ChannelAcceptPayload accept;
    accept.dial_id = dial;
    accept.channel_id = channel;
    accept.accepted_max_message_bytes = 1024 * 1024;
    capsid::protocol::Frame frame;
    frame.type = p::kChannelAccept;
    frame.version = p::kVersionV4;
    p::encode_channel_accept(accept, &frame.payload);
    return frame;
}

void test_ids_do_not_mix() {
    namespace p = capsid::protocol;
    capsid::ChannelRegistry registry;
    negotiate_full(&registry);
    // dial 100 resolves to channel 5; dial ids and channel ids are
    // separate namespaces (dial 5 must not collide with channel 5)
    require(registry.apply(capsid::ChannelSide::Worker,
                           open_frame(100, 1024 * 1024)).accepted,
            "dial 100 opens");
    require(registry.apply(capsid::ChannelSide::Host,
                           open_frame(5, 1024 * 1024)).error ==
                "channel open direction",
            "host cannot open");
    require(registry.apply(capsid::ChannelSide::Host,
                           accept_frame(100, 5)).accepted,
            "dial 100 accepted as channel 5");
    capsid::ChannelEntry entry;
    require(registry.find(5, &entry) && entry.dial_id == 100,
            "channel 5 carries dial 100");
    require(!registry.find(100, &entry),
            "dial id 100 is not a channel id");
}

void test_large_message_bounded_credit() {
    namespace p = capsid::protocol;
    capsid::ChannelRegistry registry;
    negotiate_full(&registry);
    registry.apply(capsid::ChannelSide::Worker,
                   open_frame(1, 1024 * 1024));
    registry.apply(capsid::ChannelSide::Host, accept_frame(1, 7));
    // a 128 KiB message completes although the initial credit window
    // is 64 KiB: fragments flow, the receiver replenishes mid-message
    std::vector<uint8_t> big = fetchrpc_sequences::filler_bytes(
        128u * 1024u);
    const size_t quarter = big.size() / 4;
    const uint32_t F = p::kFlagStart | p::kFlagChanText;
    require(registry.apply(capsid::ChannelSide::Worker,
                           data_frame(7,
                                      std::vector<uint8_t>(
                                          big.begin(),
                                          big.begin() + quarter),
                                      F)).accepted,
            "first quarter sent");
    // receiver grants another window mid-message
    require(registry.apply(capsid::ChannelSide::Host,
                           credit_frame(7, 65536)).accepted,
            "mid-message replenish");
    for (int part = 1; part < 4; ++part) {
        const uint32_t flags = part == 3
            ? (p::kFlagEnd | p::kFlagChanText)
            : p::kFlagChanText;
        require(registry.apply(
                    capsid::ChannelSide::Worker,
                    data_frame(7,
                               std::vector<uint8_t>(
                                   big.begin() + part * quarter,
                                   part == 3 ? big.end()
                                             : big.begin() +
                                                   (part + 1) * quarter),
                               flags)).accepted,
                "message part completes");
    }
    require(registry.messages_completed() == 1,
            "128 KiB message completed");
    require(registry.reassembly_reserved() == 0,
            "reassembly reservation released");
}

void test_aggregate_quota() {
    namespace p = capsid::protocol;
    capsid::ChannelRegistry::Options options;
    options.reassembly_budget_bytes = 64u * 1024u;  // tiny on purpose
    capsid::ChannelRegistry registry(options);
    negotiate_full(&registry);
    registry.apply(capsid::ChannelSide::Worker,
                   open_frame(1, 60u * 1024u));
    registry.apply(capsid::ChannelSide::Host, accept_frame(1, 7));
    registry.apply(capsid::ChannelSide::Worker,
                   open_frame(2, 60u * 1024u));
    registry.apply(capsid::ChannelSide::Host, accept_frame(2, 8));
    // 40 KiB open message on channel 7 (budget 64 KiB)
    const std::vector<uint8_t> half =
        fetchrpc_sequences::filler_bytes(40u * 1024u);
    require(registry.apply(capsid::ChannelSide::Worker,
                           data_frame(7, half,
                                      p::kFlagStart)).accepted,
            "channel 7 opens a 40 KiB message");
    require(registry.reassembly_reserved() == 40u * 1024u,
            "reservation counted");
    // channel 8 cannot push the aggregate over 64 KiB
    const capsid::ChannelRegistry::ApplyResult over =
        registry.apply(capsid::ChannelSide::Worker,
                       data_frame(8, half, p::kFlagStart));
    require(!over.accepted && over.error == "message size exceeded",
            "aggregate quota refuses the second open message");
    // and the rejection left NO partial state on channel 8
    capsid::ChannelEntry entry;
    require(registry.find(8, &entry) &&
                !entry.direction[1].message_open &&
                entry.direction[1].message_bytes == 0,
            "no partial state after quota refusal");
    require(registry.reassembly_reserved() == 40u * 1024u,
            "reservation unchanged by the refused frame");
    // zero credit does not stall CONTROL frames
    capsid::ChannelRegistry::ApplyResult control =
        registry.apply(capsid::ChannelSide::Host, credit_frame(7, 1));
    require(control.accepted, "credit still flows at quota");
    p::ChannelClosePayload close;
    close.channel_id = 7;
    close.close_kind = p::kCloseInitiated;
    close.flags = p::kCloseFlagCodePresent;
    close.code = 1000;
    capsid::protocol::Frame close_frame;
    close_frame.type = p::kChannelClose;
    close_frame.version = p::kVersionV4;
    p::encode_channel_close(close, &close_frame.payload);
    const capsid::ChannelRegistry::ApplyResult closed =
        registry.apply(capsid::ChannelSide::Host, close_frame);
    require(closed.accepted, "close progresses at quota");
    require(registry.reassembly_reserved() == 40u * 1024u,
            "first Close retains the opposite sender's unfinished assembly");
    require(registry.apply(capsid::ChannelSide::Worker, close_frame).accepted,
            "peer Close terminates the opposite direction");
    require(registry.reassembly_reserved() == 0,
            "peer Close returns the last assembly reservation");
}

void test_message_cap_without_budget() {
    // raising the message cap beyond the reassembly budget must not
    // silently accept an un-completable channel
    capsid::ChannelRegistry::Options options;
    options.reassembly_budget_bytes = 64u * 1024u;
    capsid::ChannelRegistry registry(options);
    negotiate_full(&registry);
    const capsid::ChannelRegistry::ApplyResult result =
        registry.apply(capsid::ChannelSide::Worker,
                       open_frame(9, 1024u * 1024u));
    require(result.accepted && result.dial_should_reject,
            "over-budget dial flagged for REJECT");
    require(result.dial_reject_reason == 8,
            "resource-limits reason");
    require(registry.live_count() == 0,
            "no state created for the refused dial");
}

void test_teardown_idempotent_and_counters_zero() {
    namespace p = capsid::protocol;
    capsid::ChannelRegistry registry;
    negotiate_full(&registry);
    registry.apply(capsid::ChannelSide::Worker,
                   open_frame(1, 1024 * 1024));
    registry.apply(capsid::ChannelSide::Host, accept_frame(1, 7));
    const std::vector<uint8_t> message =
        fetchrpc_sequences::filler_bytes(4096);
    registry.apply(capsid::ChannelSide::Worker,
                   data_frame(7, message,
                              p::kFlagStart | p::kFlagEnd |
                                  p::kFlagChanText));
    require(registry.reassembly_reserved() == 0 &&
                registry.messages_completed() == 1,
            "message completed before teardown");
    registry.teardown_channel(7, 3);
    registry.teardown_channel(7, 3);   // repeat is safe
    registry.teardown_all(3);
    registry.teardown_all(3);          // repeat is safe
    require(registry.live_count() == 0 &&
                registry.reassembly_reserved() == 0,
            "teardown zeroes live state and reservations");
    // a wire frame for the torn-down id is still `unknown channel`,
    // never excused by the tombstone
    const capsid::ChannelRegistry::ApplyResult late =
        registry.apply(capsid::ChannelSide::Host,
                       credit_frame(7, 1));
    require(!late.accepted && late.error == "unknown channel",
            "tombstone does not excuse wire frames");
    // id reuse is refused after a teardown
    const capsid::ChannelRegistry::ApplyResult reuse =
        registry.apply(capsid::ChannelSide::Host,
                       accept_frame(1, 7));
    require(!reuse.accepted, "torn-down channel id cannot be reused");
}

void test_handshake_to_duplex_handover() {
    namespace p = capsid::protocol;
    capsid::ChannelRegistry registry;
    negotiate_full(&registry);
    capsid::protocol::Frame request;
    request.type = p::kRequestHead;
    request.version = p::kVersionV4;
    request.flags = p::kFlagRequestUpgrade;
    request.request_id = 91;
    p::append_u16(&request.payload, 3);
    request.payload.insert(request.payload.end(), {'G', 'E', 'T'});
    const std::string url = "https://unit.test/ws";
    p::append_u32(&request.payload, url.size());
    request.payload.insert(request.payload.end(), url.begin(), url.end());
    p::append_u16(&request.payload, 0);
    p::UpgradeTail tail;
    tail.channel_id = 12;
    tail.max_message_bytes = 1024 * 1024;
    p::encode_upgrade_tail(tail, &request.payload);
    require(registry.apply(capsid::ChannelSide::Host,
                           request).accepted,
            "upgrade reserves channel 12");
    capsid::ChannelEntry entry;
    require(registry.find(12, &entry) &&
                entry.owner_kind == p::kOwnerHandshake &&
                entry.phase == capsid::ChannelPhase::Pending,
            "pending carries the handshake owner");
    // data before the 101 commit is a sequencing violation
    const capsid::ChannelRegistry::ApplyResult early =
        registry.apply(capsid::ChannelSide::Worker,
                       data_frame(12,
                                  std::vector<uint8_t>(4, 0x61),
                                  p::kFlagStart | p::kFlagEnd |
                                      p::kFlagChanText));
    require(!early.accepted && early.error == "channel sequence",
            "no data before the 101 commit");
    capsid::protocol::Frame response;
    response.type = p::kResponseHead;
    response.version = p::kVersionV4;
    response.flags = p::kFlagResponseUpgrade;
    response.request_id = 91;
    p::append_u16(&response.payload, 101);
    p::append_u16(&response.payload, 0); // statusText length
    p::append_u16(&response.payload, 1);
    const std::string name = "sec-websocket-protocol", value = "chat";
    p::append_u16(&response.payload, name.size());
    response.payload.insert(response.payload.end(), name.begin(), name.end());
    p::append_u32(&response.payload, value.size());
    response.payload.insert(response.payload.end(), value.begin(), value.end());
    p::UpgradeTail answer;
    answer.channel_id = 12;
    answer.max_message_bytes = 512u * 1024u;
    p::encode_upgrade_tail(answer, &response.payload);
    response.request_id = 92;
    require(!registry.apply(capsid::ChannelSide::Worker, response).accepted,
            "upgrade cannot commit another request's channel");
    response.request_id = 91;
    require(registry.apply(capsid::ChannelSide::Worker,
                           response).accepted,
            "101 commits the upgrade");
    require(registry.find(12, &entry) &&
                entry.owner_kind == p::kOwnerDuplexConnection &&
                entry.phase == capsid::ChannelPhase::Open &&
                entry.max_message_bytes == 512u * 1024u,
            "handover to the duplex owner with the negotiated cap");
}

void test_first_id_and_pending_dial_collision() {
    capsid::ChannelRegistry registry;
    negotiate_full(&registry);
    require(registry.apply(capsid::ChannelSide::Worker, open_frame(1, 1024 * 1024)).accepted,
            "first dial opens");
    require(registry.apply(capsid::ChannelSide::Host, accept_frame(1, 1)).accepted,
            "first dial can receive channel id 1");
    require(registry.apply(capsid::ChannelSide::Worker, open_frame(2, 1024 * 1024)).accepted,
            "second dial opens without overwriting channel 1");
    require(registry.apply(capsid::ChannelSide::Host, accept_frame(2, 2)).accepted,
            "second dial can receive channel id 2");
    auto invalid = data_frame(1, std::vector<uint8_t>(1, 'a'),
        capsid::protocol::kFlagStart | capsid::protocol::kFlagEnd);
    invalid.request_id = 88;
    require(registry.apply(capsid::ChannelSide::Worker, invalid).error == "invalid channel payload",
            "channel payload id cannot be mixed with the HTTP header namespace");
    capsid::ChannelEntry entry;
    require(registry.find(1, &entry) && entry.dial_id == 1,
            "first channel preserves its correlation");
    require(registry.find(2, &entry) && entry.dial_id == 2,
            "second channel preserves its correlation");
    require(registry.live_count() == 2 && registry.pending_dial_count() == 0,
            "completed dials leave no correlation slots");
}

void test_terminal_data_and_reclamation() {
    namespace p = capsid::protocol;
    capsid::ChannelRegistry registry;
    negotiate_full(&registry);
    registry.apply(capsid::ChannelSide::Worker, open_frame(1, 1024 * 1024));
    registry.apply(capsid::ChannelSide::Host, accept_frame(1, 1));
    p::ChannelClosePayload close;
    close.channel_id = 1;
    close.close_kind = p::kCloseInitiated;
    close.flags = p::kCloseFlagCodePresent;
    close.code = 1000;
    p::Frame f;
    f.version = p::kVersionV4;
    f.type = p::kChannelClose;
    p::encode_channel_close(close, &f.payload);
    require(registry.apply(capsid::ChannelSide::Host, f).accepted, "host closes");
    require(registry.apply(capsid::ChannelSide::Worker,
            data_frame(1, std::vector<uint8_t>(1, 'a'), p::kFlagStart | p::kFlagEnd)).accepted,
            "opposite-side queued data can drain before its sender closes");
    require(registry.apply(capsid::ChannelSide::Host,
            data_frame(1, std::vector<uint8_t>(1, 'a'), p::kFlagStart | p::kFlagEnd)).error ==
            "channel sequence", "the sender cannot produce DATA after its own Close");
    require(registry.apply(capsid::ChannelSide::Worker, f).accepted, "peer Close completes");
    require(registry.live_count() == 0 && registry.reassembly_reserved() == 0,
            "close handshake reclaims channel state");
    // A long-lived IPC connection must not accumulate one map entry per close.
    for (uint32_t id = 2; id < 2002; ++id) {
        require(registry.apply(capsid::ChannelSide::Worker, open_frame(id, 1024 * 1024)).accepted,
                "repeated dial admitted");
        require(registry.apply(capsid::ChannelSide::Host, accept_frame(id, id)).accepted,
                "repeated accept admitted");
        close.channel_id = id;
        close.close_kind = p::kCloseTransport;
        close.code = 1006;
        p::encode_channel_close(close, &f.payload);
        require(registry.apply(capsid::ChannelSide::Host, f).accepted,
                "transport observation ends channel immediately");
        require(registry.live_count() == 0 && registry.pending_dial_count() == 0,
                "terminal records reclaimed on every iteration");
    }
    require(registry.apply(capsid::ChannelSide::Worker, open_frame(9000, 1024 * 1024)).accepted,
            "dial after many closes");
    require(registry.apply(capsid::ChannelSide::Host, accept_frame(9000, 1)).error ==
            "channel sequence", "old id still cannot be reused after reclamation");
}

void test_message_limit_is_negotiated_before_budget_gate() {
    capsid::ChannelRegistry registry;
    negotiate_full(&registry);
    require(registry.apply(capsid::ChannelSide::Worker,
                          open_frame(1, 64u * 1024u * 1024u)).accepted,
            "large requested cap can negotiate down to configured cap");
    require(registry.apply(capsid::ChannelSide::Host, accept_frame(1, 1)).accepted,
            "negotiated-down cap fits the aggregate budget");
    capsid::ChannelEntry entry;
    require(registry.find(1, &entry) && entry.max_message_bytes == 1024 * 1024,
            "configured message cap remains authoritative");
}

void test_resource_rejection_can_be_acknowledged() {
    namespace p = capsid::protocol;
    capsid::ChannelRegistry::Options options;
    options.reassembly_budget_bytes = 64 * 1024;
    capsid::ChannelRegistry registry(options);
    negotiate_full(&registry);
    const auto opened = registry.apply(capsid::ChannelSide::Worker, open_frame(1, 1024 * 1024));
    require(opened.accepted && opened.dial_should_reject, "resource refusal recorded");
    p::ChannelRejectPayload reject;
    reject.dial_id = 1;
    reject.reason_code = 8;
    p::Frame f;
    f.version = p::kVersionV4;
    f.type = p::kChannelReject;
    p::encode_channel_reject(reject, &f.payload);
    require(registry.apply(capsid::ChannelSide::Host, f).accepted,
            "host can answer a resource-refused dial with REJECT");
    require(registry.pending_dial_count() == 0, "resource refusal correlation released");
    require(registry.apply(capsid::ChannelSide::Worker,
                          open_frame(1, 1024 * 1024)).error == "channel sequence",
            "completed dial ids cannot be reused by late accepts");
}

void test_tail_owner_refused() {
    namespace p = capsid::protocol;
    capsid::ChannelRegistry registry;
    negotiate_full(&registry);
    p::ChannelOpenPayload open;
    open.channel_id = 0;
    open.dial_id = 3;
    open.owner_kind = p::kOwnerTailTask;
    open.max_message_bytes = 1024 * 1024;
    open.url = "wss://tail.test/";
    capsid::protocol::Frame frame;
    frame.type = p::kChannelOpen;
    frame.version = p::kVersionV4;
    p::encode_channel_open(open, &frame.payload);
    const capsid::ChannelRegistry::ApplyResult result =
        registry.apply(capsid::ChannelSide::Worker, frame);
    require(result.accepted && result.dial_should_reject,
            "tail owner refused in v0.3.0");
    require(registry.live_count() == 0,
            "no channel created for a tail owner");
}

void test_retained_incoming_lifetime() {
    namespace p=capsid::protocol;using capsid::ChannelSide;
    capsid::ChannelRegistry::Options options;options.max_channels=1;
    capsid::ChannelRegistry registry(options);negotiate_full(&registry,155,1);
    auto incoming=[&](capsid::ChannelRegistry &target,uint32_t id,bool commit=true) {
        p::Frame request;request.version=4;request.type=p::kRequestHead;request.flags=p::kFlagRequestUpgrade;request.request_id=100+id;
        p::append_u16(&request.payload,3);request.payload.insert(request.payload.end(),{'G','E','T'});
        const std::string url="https://unit.test/ws";p::append_u32(&request.payload,url.size());request.payload.insert(request.payload.end(),url.begin(),url.end());p::append_u16(&request.payload,0);
        p::UpgradeTail tail;tail.channel_id=id;tail.max_message_bytes=1024*1024;p::encode_upgrade_tail(tail,&request.payload);
        require(target.apply(ChannelSide::Host,request).accepted,"retained lifetime incoming reservation");
        if(commit){p::Frame response;response.version=4;response.type=p::kResponseHead;response.flags=p::kFlagResponseUpgrade;response.request_id=request.request_id;
            p::append_u16(&response.payload,101);p::append_u16(&response.payload,0);p::append_u16(&response.payload,0);p::encode_upgrade_tail(tail,&response.payload);
            require(target.apply(ChannelSide::Worker,response).accepted,"retained lifetime committed incoming");}
    };
    auto state=[](uint32_t id,uint32_t retained) {p::Frame frame;frame.version=4;frame.type=p::kConnectionLifetime;p::append_u32(&frame.payload,id);p::append_u32(&frame.payload,retained);return frame;};
    auto cancel=[](uint32_t id,uint32_t reason=p::kCancelDisconnect) {p::Frame frame;frame.version=4;frame.type=p::kChannelCancel;p::ChannelCancelPayload value;value.channel_id=id;value.terminal_reason=reason;p::encode_channel_cancel(value,&frame.payload);return frame;};
    auto ack=[](uint32_t id) {p::Frame frame;frame.version=4;frame.type=p::kChannelAck;p::ChannelAckPayload value;value.channel_id=id;value.kind=p::kAckCancel;p::encode_channel_ack(value,&frame.payload);return frame;};
    incoming(registry,1);
    require(!registry.apply(ChannelSide::Worker,state(1,1)).accepted && registry.retained_owner_count()==0,"no lifetime hold before actual transport terminal");
    require(registry.apply(ChannelSide::Host,cancel(1)).accepted,"incoming EOF cancellation");
    auto bad=state(1,1);bad.flags=1;require(!registry.apply(ChannelSide::Worker,bad).accepted,"lifetime flags rejected by registry");
    bad=state(1,1);bad.payload.push_back(0);require(!registry.apply(ChannelSide::Worker,bad).accepted,"lifetime trailing bytes rejected");
    require(!registry.apply(ChannelSide::Host,state(1,1)).accepted,"host cannot retain incoming runtime owner");
    require(registry.apply(ChannelSide::Worker,state(1,1)).accepted && registry.retained_owner_count()==1,"runtime retains before ACK");
    require(!registry.apply(ChannelSide::Worker,state(1,1)).accepted && registry.retained_owner_count()==1,"duplicate retain is transactional");
    require(registry.apply(ChannelSide::Worker,ack(1)).accepted && registry.live_count()==0 && registry.retained_owner_count()==1,"ACK returns wire slot while generation owner stays retained");
    incoming(registry,2);require(registry.apply(ChannelSide::Host,cancel(2)).accepted,"reuse wire capacity independently");
    require(registry.apply(ChannelSide::Worker,state(2,1)).accepted && registry.apply(ChannelSide::Worker,ack(2)).accepted,"second retained owner at bounded protocol allowance");
    incoming(registry,3);require(registry.apply(ChannelSide::Host,cancel(3)).accepted,"third terminal");
    require(!registry.apply(ChannelSide::Worker,state(3,1)).accepted && registry.retained_owner_count()==2,"retained owner quota is independent and finite");
    require(!registry.apply(ChannelSide::Worker,state(99,0)).accepted && !registry.apply(ChannelSide::Host,state(1,0)).accepted,"unknown and foreign releases refused");
    require(registry.apply(ChannelSide::Worker,state(1,0)).accepted && registry.retained_owner_count()==1,"release retired channel without re-creating transport");
    require(!registry.apply(ChannelSide::Worker,state(1,0)).accepted,"duplicate release refused");
    require(registry.apply(ChannelSide::Worker,state(3,1)).accepted && registry.apply(ChannelSide::Worker,ack(3)).accepted,"retained quota reused after actual release");
    registry.teardown_all(p::kCancelShutdown);require(registry.retained_owner_count()==0 && registry.live_count()==0,"explicit teardown clears retained state");
    capsid::ChannelRegistry legacy(options);negotiate_full(&legacy,31,1);incoming(legacy,1);require(legacy.apply(ChannelSide::Host,cancel(1)).accepted,"legacy incoming cancel");
    require(!legacy.apply(ChannelSide::Worker,state(1,1)).accepted,"no lifetime records without opt-in");
    capsid::ChannelRegistry failure(options);negotiate_full(&failure,155,1);incoming(failure,1);
    require(failure.apply(ChannelSide::Host,cancel(1,p::kCancelResourceLimits)).accepted && !failure.apply(ChannelSide::Worker,state(1,1)).accepted,"resource failure cannot acquire retained lifetime");
    capsid::ChannelRegistry handshake(options);negotiate_full(&handshake,155,1);incoming(handshake,1,false);
    require(handshake.apply(ChannelSide::Host,cancel(1)).accepted && !handshake.apply(ChannelSide::Worker,state(1,1)).accepted,"uncommitted handshake cannot acquire retained lifetime");
    capsid::ChannelRegistry outgoing(options);negotiate_full(&outgoing,155,1);
    require(outgoing.apply(ChannelSide::Worker,open_frame(1,1024*1024)).accepted && outgoing.apply(ChannelSide::Host,accept_frame(1,1)).accepted,"outgoing result fixture");
    p::ChannelAckPayload result;result.channel_id=1;result.dial_id=1;result.kind=p::kAckDialResult;p::Frame commit;commit.version=4;commit.type=p::kChannelAck;p::encode_channel_ack(result,&commit.payload);
    require(outgoing.apply(ChannelSide::Worker,commit).accepted && outgoing.apply(ChannelSide::Host,cancel(1)).accepted,"outgoing transport terminal");
    require(!outgoing.apply(ChannelSide::Worker,state(1,1)).accepted,"outgoing endpoint cannot acquire incoming lifetime");
}
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
    json_t *root =
        json_loads(buffer.str().c_str(), JSON_REJECT_DUPLICATES, &json_error);
    if (!root) {
        std::fprintf(stderr,
                     "FAIL: vectors JSON parse error %s:%d %s\n",
                     path, json_error.line, json_error.text);
        return 2;
    }
    const json_t *schema_version =
        json_object_get(root, "schema_version");
    require(json_is_integer(schema_version) &&
                json_integer_value(schema_version) == 2,
            "schema_version is exactly 2");
    const json_t *frames = json_object_get(root, "frames");
    const json_t *sequences = json_object_get(root, "sequences");
    require(json_is_array(frames), "frames array present");

    std::map<std::string, std::vector<uint8_t>> frame_wire;
    size_t index;
    json_t *value;
    json_array_foreach(frames, index, value) {
        const std::string name =
            json_string_value(json_object_get(value, "name"));
        frame_wire[name] = unhex(
            json_string_value(json_object_get(value, "hex")));
    }
    // the production registry replays the SAME frozen sequences
    {
        RegistryTarget target;
        const std::set<std::string> executed =
            fetchrpc_sequences::run_sequences(sequences, frame_wire,
                                              &target, &failures);
        require(executed.size() == json_array_size(sequences),
                "registry executed every sequence");
    }

    test_first_id_and_pending_dial_collision();
    test_terminal_data_and_reclamation();
    test_resource_rejection_can_be_acknowledged();
    test_message_limit_is_negotiated_before_budget_gate();
    test_ids_do_not_mix();
    test_large_message_bounded_credit();
    test_aggregate_quota();
    test_message_cap_without_budget();
    test_teardown_idempotent_and_counters_zero();
    test_handshake_to_duplex_handover();
    test_tail_owner_refused();
    test_retained_incoming_lifetime();

    json_decref(root);
    if (failures > 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("channel registry: frozen sequences + budget "
                "guarantees PASS\n");
    return 0;
}
