#include "channel_io.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif

#ifndef CAPSID_CHANNEL_IO_NO_FAULT_SWEEP
#define CAPSID_CHANNEL_IO_NO_FAULT_SWEEP 0
#endif

// Fail individual native allocations, including payloads and queue nodes.
// The toggle stays outside setup/verification so each failure proves that
// a refused receive/send leaves both wire credit and owned quota intact.
static long allocation_countdown = -1;
static bool observe_allocations = false;
static std::size_t largest_allocation = 0;
void *operator new(std::size_t size) {
    if (observe_allocations && size > largest_allocation) largest_allocation = size;
    if (!CAPSID_CHANNEL_IO_NO_FAULT_SWEEP && allocation_countdown == 0) throw std::bad_alloc();
    if (!CAPSID_CHANNEL_IO_NO_FAULT_SWEEP && allocation_countdown > 0) --allocation_countdown;
    if (void *ptr = std::malloc(size ? size : 1)) return ptr;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *ptr) noexcept { std::free(ptr); }
void operator delete[](void *ptr) noexcept { std::free(ptr); }
#if __cplusplus >= 201402L
void operator delete(void *ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete[](void *ptr, std::size_t) noexcept { std::free(ptr); }
#endif

namespace {
namespace p = capsid::protocol;
using Side = capsid::ChannelSide;
void require(bool value, const char *what) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
capsid::ChannelRegistry::Options options(Side side, uint64_t budget = 16 * 1024 * 1024,
                                        uint64_t cap = 1024 * 1024, uint64_t queue = 4 * 1024 * 1024) {
    capsid::ChannelRegistry::Options o;
    o.local_side = side;
    o.reassembly_budget_bytes = budget;
    o.send_budget_bytes = budget;
    o.max_message_bytes = cap;
    o.per_channel_queue_bytes = queue;
    return o;
}
struct Pair {
    capsid::ChannelIO host, worker;
    explicit Pair(uint64_t budget = 16 * 1024 * 1024, uint64_t cap = 1024 * 1024,
                  uint64_t queue = 4 * 1024 * 1024, uint32_t capabilities = 31)
        : host(options(Side::Host, budget, cap, queue)), worker(options(Side::Worker, budget, cap, queue)) {
        p::Frame hello;
        hello.version = 4; hello.type = p::kHello;
        hello.payload.resize(p::kHelloFixedPayloadSize);
        require(p::append_hello_a2_extension(&hello.payload, p::ChannelLimits()), "limits");
        p::append_u32(&hello.payload, capabilities); p::append_u32(&hello.payload, 0);
        require(host.commit_local(hello).accepted && worker.receive(hello).accepted, "HELLO");
        p::Frame ready;
        ready.version = 4; ready.type = p::kReady;
        p::append_u32(&ready.payload, capabilities); p::append_u32(&ready.payload, 0);
        require(worker.commit_local(ready).accepted && host.receive(ready).accepted, "READY");
        open(1);
    }
    void open(uint32_t id) {
        p::Frame f; f.version = 4; f.type = p::kChannelOpen;
        p::ChannelOpenPayload payload;
        payload.dial_id = id; payload.owner_kind = p::kOwnerStandalone;
        payload.max_message_bytes = 1024 * 1024; payload.url = "ws://owned.test/";
        require(p::encode_channel_open(payload, &f.payload), "OPEN encode");
        require(worker.commit_local(f).accepted && host.receive(f).accepted, "OPEN");
        f.type = p::kChannelAccept;
        p::ChannelAcceptPayload accept;
        accept.dial_id = id; accept.channel_id = id; accept.accepted_max_message_bytes = payload.max_message_bytes;
        require(p::encode_channel_accept(accept, &f.payload), "ACCEPT encode");
        require(host.commit_local(f).accepted && worker.receive(f).accepted, "ACCEPT");
        f.type = p::kChannelAck;
        p::ChannelAckPayload ack; ack.channel_id = id; ack.dial_id = id;
        require(p::encode_channel_ack(ack, &f.payload), "ACK encode");
        require(worker.commit_local(f).accepted && host.receive(f).accepted, "result ACK");
    }
};
p::Frame data(size_t count, bool start, bool end) {
    p::Frame f; f.version = 4; f.type = p::kChannelData;
    f.flags = (start ? p::kFlagStart : 0) | (end ? p::kFlagEnd : 0);
    p::append_u32(&f.payload, 1); f.payload.resize(4 + count, 'x'); return f;
}
void owned_large_message_and_lease() {
    Pair pair;
    std::vector<uint8_t> source(150000);
    for (size_t i = 0; i < source.size(); ++i) source[i] = uint8_t(i % 251);
    const auto expected = source;
    std::string error;
    require(pair.worker.queue_message(1, source.data(), source.size(), false, &error), "large message queues synchronously");
    std::fill(source.begin(), source.end(), 0); // mutation cannot affect the owned copy
    const auto reserved = pair.worker.send_reserved();
    p::Frame fragment;
    size_t fragments = 0;
    while (pair.worker.next_fragment(&fragment)) {
        p::Frame retry;
        require(pair.worker.next_fragment(&retry) && retry.payload == fragment.payload && retry.flags == fragment.flags,
                "WOULD_BLOCK peek leaves bytes, cursor and credit untouched");
        require(pair.worker.commit_fragment(fragment, &error), "writer commits exact fragment");
        require(pair.host.receive(fragment).accepted, "receiver accepts owned fragment");
        if (!(fragment.flags & p::kFlagEnd)) require(pair.worker.send_reserved() == reserved, "partial send retains allocation charge");
        p::Frame grant; grant.version = 4; grant.type = p::kChannelCredit;
        p::ChannelCreditPayload credit; credit.channel_id = 1; credit.credit = fragment.payload.size() - 4;
        require(p::encode_channel_credit(credit, &grant.payload), "credit encode");
        require(pair.host.commit_local(grant).accepted && pair.worker.receive(grant).accepted,
                "admitted chunks replenish credit before the complete message");
        ++fragments;
    }
    require(fragments >= 3 && pair.worker.send_reserved() == 0, "message exceeds initial window and releases send storage");
    auto message = pair.host.take_message();
    require(message && message->size() == expected.size() && message->origin().owner_kind == p::kOwnerStandalone,
            "immutable message retains its independent connection origin");
    size_t offset = 0;
    for (const auto &chunk : message->chunks()) {
        require(offset + chunk.size() <= expected.size() &&
                std::memcmp(chunk.data(), expected.data() + offset, chunk.size()) == 0, "owned bytes are exact");
        offset += chunk.size();
    }
    require(offset == expected.size(), "all copied bytes checked");
    pair.host.teardown_all(0);
    require(pair.host.receive_reserved() >= expected.size(), "dispatch lease stays charged after dequeue and teardown");
    message.reset();
    require(pair.host.receive_reserved() == 0, "releasing the final owner returns the receive budget");
}
void native_forwarding_preserves_leases_and_retries() {
    Pair pair;
    pair.open(2);
    capsid::ChannelAuthority origin; origin.binding = true; origin.binding_id = 42;
    require(pair.worker.register_authority(1, origin), "forward source authority registers");
    const size_t size = 150000;
    std::vector<uint8_t> expected(size);
    for (size_t index = 0; index < size; ++index) expected[index] = uint8_t(index % 251);
    for (size_t offset = 0; offset < size;) {
        const size_t count = std::min(size_t(17000), size - offset);
        auto fragment = data(0, offset == 0, offset + count == size);
        fragment.payload.insert(fragment.payload.end(), expected.begin() + offset, expected.begin() + offset + count);
        require(pair.host.commit_local(fragment).accepted && pair.worker.receive(fragment).accepted, "native source chunks cross several output fragment boundaries");
        p::ChannelCreditPayload credit; credit.channel_id = 1; credit.credit = count;
        p::Frame grant; grant.version = 4; grant.type = p::kChannelCredit;
        require(p::encode_channel_credit(credit, &grant.payload) && pair.worker.commit_local(grant).accepted && pair.host.receive(grant).accepted, "source can exceed the initial credit window");
        offset += count;
    }
    auto message = pair.worker.take_message();
    require(message && message->size() == size && message->origin().authority.binding_id == 42, "source lease has its original immutable authority");
    const uint64_t received = pair.worker.receive_reserved();
    std::weak_ptr<const capsid::ChannelMessage> lease = message;
    std::string error;
    bool queued = false;
    largest_allocation = 0; observe_allocations = true;
    for (long fail = 0; fail < 64; ++fail) {
        allocation_countdown = fail;
        try { queued = pair.worker.forward_message(2, message, &error); }
        catch (const std::bad_alloc &) {}
        allocation_countdown = -1;
        if (queued) break;
        require(!pair.worker.has_pending_send(2) && !pair.worker.send_reserved() && pair.worker.receive_reserved() == received,
                "forward allocation failure preserves both quota banks and the source lease");
    }
    observe_allocations = false;
    require(queued, "forward admission succeeds after individual allocation failures");
    require(largest_allocation < size, "native handover allocates metadata without a full payload copy");
    message.reset();
    require(!lease.expired() && pair.worker.receive_reserved() == received, "queued forward owns the original receive storage");
    p::Frame fragment;
    size_t fragments = 0;
    while (pair.worker.next_fragment(&fragment)) {
        require(p::load_u32_at(fragment.payload.data()) == 2 && !(fragment.flags & p::kFlagChanText), "forward preserves binary type on its own output channel");
        require(pair.worker.commit_fragment(fragment, &error) && pair.host.receive(fragment).accepted, "forward commits exact leased chunks");
        p::ChannelCreditPayload credit; credit.channel_id = 2; credit.credit = fragment.payload.size() - 4;
        p::Frame grant; grant.version = 4; grant.type = p::kChannelCredit;
        require(p::encode_channel_credit(credit, &grant.payload) && pair.host.commit_local(grant).accepted && pair.worker.receive(grant).accepted, "forward responds to actual output credit");
        ++fragments;
    }
    require(fragments >= 3 && lease.expired() && !pair.worker.receive_reserved() && !pair.worker.send_reserved(), "final output fragment releases source and output leases");
    auto output = pair.host.take_message();
    require(output && output->size() == size && !output->text(), "forward assembles one complete binary message");
    size_t offset = 0;
    for (const auto &chunk : output->chunks()) {
        require(std::memcmp(chunk.data(), expected.data() + offset, chunk.size()) == 0, "forward checks every byte across input chunk boundaries");
        offset += chunk.size();
    }
    require(offset == size, "forwarded bytes have their exact original length");

    Pair pressure(1600, 1024, 1600); pressure.open(2);
    auto input = data(400, true, true);
    require(pressure.host.commit_local(input).accepted && pressure.worker.receive(input).accepted, "bounded native forwarding source");
    auto held = pressure.worker.take_message();
    const bool admitted = pressure.worker.forward_message(2, held, &error);
    require(admitted, "bounded native forwarding admitted");
    const uint64_t sending = pressure.worker.send_reserved(), receiving = pressure.worker.receive_reserved();
    require(!pressure.worker.forward_message(2, held, &error) && error == "channel send budget exceeded" &&
            pressure.worker.send_reserved() == sending && pressure.worker.receive_reserved() == receiving, "busy output preserves the input lease for retry");
    require(pressure.worker.next_fragment(&fragment) && pressure.worker.commit_fragment(fragment, &error) && pressure.host.receive(fragment).accepted, "busy output actually drains");
    pressure.host.take_message().reset();
    require(pressure.worker.forward_message(2, held, &error), "the same message retries after output capacity returns");
    pressure.worker.teardown_all(0); held.reset();
    require(!pressure.worker.send_reserved() && !pressure.worker.receive_reserved(), "cancellation drops retained proxy leases and quota");
    std::puts(CAPSID_CHANNEL_IO_NO_FAULT_SWEEP
        ? "native forwarding Debug iterators: exact payload, immutable authority, pressure retry and cancellation PASS"
        : "native forwarding: exact large payload, immutable authority, allocation rollback, pressure retry and cancellation PASS");
}

void quota_refusal_is_atomic_and_empty_messages_are_bounded() {
    Pair pair(1600, 1024);
    auto too_big = pair.host.receive(data(900, true, false));
    require(!too_big.accepted && too_big.error == "channel receive budget exceeded", "reserve includes metadata before allocation");
    capsid::ChannelEntry entry;
    pair.host.registry().find(1, &entry);
    require(entry.direction[1].credit == 65536 && pair.host.receive_reserved() == 0, "refusal spends no credit or memory");
    require(pair.host.receive(data(400, true, false)).accepted, "first small fragment admitted");
    const auto held = pair.host.receive_reserved();
    require(!pair.host.receive(data(500, false, true)).accepted && pair.host.receive_reserved() == held,
            "continuation refuses without changing existing assembly");
    require(pair.host.receive(data(100, false, true)).accepted, "valid retry completes same assembly");
    auto lease = pair.host.take_message();
    require(lease && lease->size() == 500, "partial rejection never contaminates completed message");
    require(!pair.host.receive(data(0, true, true)).accepted, "dequeued lease still limits the next receive");
    lease.reset();
    for (int i = 0; i < 2; ++i) require(pair.host.receive(data(0, true, true)).accepted, "empty message has bounded metadata charge");
    require(!pair.host.receive(data(0, true, true)).accepted && pair.host.queued_messages() == 2,
            "unbounded zero-byte queue rejected");
    pair.host.teardown_all(0);
    require(pair.host.receive_reserved() == 0, "teardown releases assembly and completion queues");
    const uint8_t bytes[500] = {};
    std::string error;
    require(pair.worker.queue_message(1, bytes, sizeof(bytes), true, &error), "bounded send admitted");
    const auto used = pair.worker.send_reserved();
    require(!pair.worker.queue_message(1, bytes, sizeof(bytes), true, &error) && error == "channel send budget exceeded" &&
            pair.worker.send_reserved() == used, "send limit refuses before copying or consuming state");
    pair.worker.teardown_all(0);
    require(pair.worker.send_reserved() == 0, "teardown releases copied pending sends");
}
void cancelled_data_drains_without_allocation() {
    Pair pair;
    auto start = data(200, true, false);
    require(pair.worker.commit_local(start).accepted && pair.host.receive(start).accepted, "message starts");
    const auto tail = data(200, false, true);
    require(pair.worker.commit_local(tail).accepted, "tail queued before learning CANCEL");
    p::Frame cancel; cancel.version = 4; cancel.type = p::kChannelCancel;
    p::ChannelCancelPayload c; c.channel_id = 1; p::encode_channel_cancel(c, &cancel.payload);
    require(pair.host.commit_local(cancel).accepted && pair.host.receive_reserved() == 0, "local cancel releases assembly storage");
    const auto crossed = pair.host.receive(tail);
    require(crossed.accepted && crossed.discard && pair.host.receive_reserved() == 0 && !pair.host.take_message(),
            "late DATA validates without allocation or callback revival");
    require(pair.worker.receive(cancel).accepted, "peer observes cancellation");
    p::Frame ack; ack.version = 4; ack.type = p::kChannelAck;
    p::ChannelAckPayload a; a.channel_id = 1; a.kind = p::kAckCancel; p::encode_channel_ack(a, &ack.payload);
    require(pair.worker.commit_local(ack).accepted && pair.host.receive(ack).accepted, "terminal ACK");
    require(pair.worker.registry().live_count() == 0 && pair.host.registry().live_count() == 0,
            "no endpoint or queue survives the cancellation barrier");
}
void per_channel_queue_and_fairness() {
    Pair pair;
    pair.open(2);
    std::vector<uint8_t> bytes(70000, 'f');
    std::string error;
    require(pair.worker.queue_message(1, bytes.data(), bytes.size(), true, &error) &&
            pair.worker.queue_message(2, bytes.data(), bytes.size(), false, &error), "two sends queue");
    p::Frame first, second;
    require(pair.worker.next_fragment(&first) && pair.worker.commit_fragment(first, &error), "first channel makes progress");
    require(pair.worker.next_fragment(&second) && pair.worker.commit_fragment(second, &error), "second channel makes progress");
    require(p::load_u32_at(first.payload.data()) == 1 && p::load_u32_at(second.payload.data()) == 2,
            "one channel cannot monopolize the fragment pump");
    Pair limited(16 * 1024 * 1024, 1024, 750);
    require(!limited.worker.queue_message(1, bytes.data(), 500, false, &error) &&
            error == "channel send budget exceeded" && limited.worker.send_reserved() == 0,
            "per-channel queue ceiling applies even with unused aggregate quota");
    require(!limited.host.receive(data(500, true, true)).accepted && limited.host.receive_reserved() == 0,
            "per-channel receive queue has the same bound");
    pair.worker.teardown_all(0);
    require(pair.worker.send_reserved() == 0, "unfinished fragments reclaim owned storage");
}

#if !CAPSID_CHANNEL_IO_NO_FAULT_SWEEP
void allocation_failures_preserve_state() {
    unsigned receive_failures = 0, send_failures = 0, control_failures = 0;
    for (long fail_at = 0; fail_at < 60; ++fail_at) {
        Pair pair;
        require(pair.host.receive(data(400, true, false)).accepted, "OOM setup fragment");
        const auto held = pair.host.receive_reserved();
        auto tail = data(100, false, true);
        allocation_countdown = fail_at;
        bool failed = false;
        try { require(pair.host.receive(tail).accepted, "receive succeeds without OOM"); }
        catch (const std::bad_alloc &) { failed = true; }
        allocation_countdown = -1;
        if (failed) {
            ++receive_failures;
            capsid::ChannelEntry entry;
            pair.host.registry().find(1, &entry);
            require(pair.host.receive_reserved() == held && pair.host.queued_messages() == 0 &&
                    entry.direction[1].credit == 65536 - 400 && entry.direction[1].message_bytes == 400,
                    "every receive allocation failure rolls back quota, queue and credit");
            require(pair.host.receive(tail).accepted, "retry same fragment after OOM");
        }
        auto lease = pair.host.take_message();
        require(lease && lease->size() == 500, "OOM retry preserves exact assembly length");
        size_t observed=0;
        for (const auto &chunk:lease->chunks()) {
            for (size_t i=0;i<chunk.size();++i) require(chunk.data()[i]=='x',"OOM retry preserves every owned payload byte");
            observed+=chunk.size();
        }
        require(observed==500,"OOM retry owns the complete payload");
        lease.reset();
        require(pair.host.receive_reserved() == 0, "OOM lifecycle releases receive storage");
        const uint8_t bytes[500] = {};
        std::string error;
        allocation_countdown = fail_at;
        failed = false;
        try { require(pair.worker.queue_message(1, bytes, sizeof(bytes), false, &error), "send succeeds without OOM"); }
        catch (const std::bad_alloc &) { failed = true; }
        allocation_countdown = -1;
        if (failed) {
            ++send_failures;
            p::Frame peek;
            require(pair.worker.send_reserved() == 0 && !pair.worker.next_fragment(&peek),
                    "every send allocation failure rolls back owned quota and queue");
            require(pair.worker.queue_message(1, bytes, sizeof(bytes), false, &error), "retry send after OOM");
        }
        pair.worker.teardown_all(0);
        require(pair.worker.send_reserved() == 0, "OOM lifecycle releases send storage");
        Pair cancelled;
        require(cancelled.worker.queue_message(1, bytes, sizeof(bytes), false, &error), "OOM control setup");
        const auto send_held = cancelled.worker.send_reserved();
        p::Frame cancel; cancel.version = 4; cancel.type = p::kChannelCancel;
        p::ChannelCancelPayload payload; payload.channel_id = 1;
        p::encode_channel_cancel(payload, &cancel.payload);
        failed = false;
        allocation_countdown = fail_at;
        try { require(cancelled.worker.commit_local(cancel).accepted, "control succeeds without OOM"); }
        catch (const std::bad_alloc &) { failed = true; }
        allocation_countdown = -1;
        if (failed) {
            ++control_failures;
            capsid::ChannelEntry entry;
            cancelled.worker.registry().find(1, &entry);
            require(entry.phase == capsid::ChannelPhase::Open && cancelled.worker.send_reserved() == send_held,
                    "control allocation failure preserves phase and copied sends");
            require(cancelled.worker.commit_local(cancel).accepted, "control retry after allocation failure");
        }
        require(cancelled.worker.send_reserved() == 0, "committed control releases stopped producer");
    }
    std::printf("native allocation faults: receive=%u send=%u control=%u\n",
                receive_failures, send_failures, control_failures);
    // Instrumented and ordinary builds can use different allocation counts.
    // Every actual fault in the 60-position sweep must pass the rollback
    // and byte checks above; every operation must really encounter a fault.
    require(receive_failures > 0 && send_failures > 0 && control_failures > 0,
            "fault injection actually reaches receive, send and control allocations");
}

#endif
void half_close_releases_only_the_local_producer() {
    Pair pair;
    const uint8_t byte = 'h';
    std::string error;
    require(pair.worker.queue_message(1, &byte, 1, true, &error), "half-close setup send queue");
    p::Frame half; half.version = 4; half.type = p::kChannelHalfClose;
    p::append_u32(&half.payload, 1);
    require(pair.worker.commit_local(half).accepted && pair.host.receive(half).accepted,
            "half-close ends the local producer");
    p::Frame peek;
    require(pair.worker.send_reserved() == 0 && !pair.worker.next_fragment(&peek),
            "stopped producer keeps no unsent payload allocation");
    require(pair.host.commit_local(data(1, true, true)).accepted && pair.worker.receive(data(1, true, true)).accepted,
            "half-close leaves the receiving direction alive");
    auto message = pair.worker.take_message();
    require(message && message->size() == 1, "peer message still reaches independent receive owner");
}
bool blocked_writer(const p::Frame &,void *) { return false; }
bool accepted_writer(const p::Frame &,void *) { return true; }
void writer_transaction_quarantine_and_close_receive() {
    Pair pair(4096,1024,4096);
    std::string error; const uint8_t byte='q';
    require(pair.worker.queue_message(1,&byte,1,false,&error),"writer test queues copied message");
    const auto reserved=pair.worker.send_reserved(); capsid::ChannelEntry before,after;
    pair.worker.registry().find(1,&before);
    require(!pair.worker.transmit_fragment(blocked_writer,nullptr,&error) && pair.worker.send_reserved()==reserved,"blocked writer preserves charged send");
    pair.worker.registry().find(1,&after);
    require(before.direction[1].credit==after.direction[1].credit,"blocked writer consumes no credit");
    require(pair.worker.transmit_fragment(accepted_writer,nullptr,&error) && !pair.worker.send_reserved(),"successful writer commits and releases final allocation");
    capsid::ChannelAuthority origin;origin.binding=true;origin.binding_id=42;
    require(pair.host.register_authority(1,origin) && !pair.host.register_authority(1,capsid::ChannelAuthority()),"authority registers once and cannot widen");
    auto start=data(512,true,false);require(pair.worker.commit_local(start).accepted&&pair.host.receive(start).accepted,"opposite assembly starts");
    p::ChannelClosePayload close;close.channel_id=1;
    p::Frame closed;closed.version=4;closed.type=p::kChannelClose;p::encode_channel_close(close,&closed.payload);
    require(pair.host.transmit(closed,accepted_writer,nullptr,&error)&&pair.host.receive_reserved()>0,"ordinary local Close preserves receiving assembly");
    auto end=data(100,false,true);require(pair.worker.commit_local(end).accepted&&pair.host.receive(end).accepted,"Close-crossed continuation is delivered");
    auto message=pair.host.take_message();require(message&&message->size()==612&&message->origin().authority.binding_id==42,"immutable authority and payload survive handover");
    Pair pressure(1024,1024,1024);
    auto fragment=data(400,true,false);
    require(pressure.worker.commit_local(fragment).accepted,"wire valid quota setup");
    auto result=pressure.host.receive(fragment,true);
    require(result.accepted&&result.resource_exhausted&&result.discard&&!pressure.host.receive_reserved(),"quota refusal commits validation and quarantines this producer");
    fragment=data(10,false,true);require(pressure.worker.commit_local(fragment).accepted,"queued continuation exists");
    result=pressure.host.receive(fragment,true);
    require(result.accepted&&result.discard&&!result.resource_exhausted&&!pressure.host.take_message(),"quarantine drains queued tail without allocation or revival");
}
void final_close_releases_queues_after_writer_commit() {
    Pair pair(16 * 1024 * 1024, 1024 * 1024, 4 * 1024 * 1024, 63);
    auto fragment = data(100, true, false);
    require(pair.host.commit_local(fragment).accepted && pair.worker.receive(fragment).accepted,
            "final Close setup owns opposite-direction partial message");
    const uint64_t received = pair.worker.receive_reserved();
    require(received > 0, "partial message has an actual receive charge");
    auto end = data(1, false, true);
    require(pair.host.commit_local(end).accepted, "peer queued its continuation before observing final Close");
    p::ChannelClosePayload payload; payload.channel_id = 1; payload.close_kind = 3;
    payload.flags = p::kCloseFlagCodePresent; payload.code = 1000; payload.reason = "done";
    p::Frame final; final.version = 4; final.type = p::kChannelClose;
    require(p::encode_channel_close(payload, &final.payload), "final Close encodes");
    std::string error;
    require(!pair.worker.transmit(final, blocked_writer, nullptr, &error) &&
            pair.worker.receive_reserved() == received, "blocked final marker preserves assembly and registry");
    capsid::ChannelEntry entry;
    require(pair.worker.registry().find(1, &entry) && entry.phase == capsid::ChannelPhase::Open,
            "failed final writer leaves channel open");
    require(pair.worker.transmit(final, accepted_writer, nullptr, &error) &&
            !pair.worker.receive_reserved() && pair.host.receive(final).accepted,
            "committed final marker releases stopped producers and receive assembly");
    require(pair.worker.receive(end).accepted && !pair.worker.receive_reserved(),
            "crossed continuation drains without allocating after final marker");
    p::ChannelAckPayload ack; ack.channel_id = 1; ack.kind = p::kAckTransportClose;
    p::Frame response; response.version = 4; response.type = p::kChannelAck;
    require(p::encode_channel_ack(ack, &response.payload) && pair.host.commit_local(response).accepted &&
            pair.worker.receive(response).accepted, "final transport ACK reclaims both endpoints");
    require(!pair.host.registry().live_count() && !pair.worker.registry().live_count() &&
            !pair.host.receive_reserved() && !pair.worker.receive_reserved() &&
            !pair.host.send_reserved() && !pair.worker.send_reserved(), "final marker leaves no channel or quota charge");
}
} // namespace
int main() {
#ifdef _MSC_VER
    _set_error_mode(_OUT_TO_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    owned_large_message_and_lease();
    native_forwarding_preserves_leases_and_retries();
    quota_refusal_is_atomic_and_empty_messages_are_bounded();
    cancelled_data_drains_without_allocation();
    per_channel_queue_and_fairness();
#if !CAPSID_CHANNEL_IO_NO_FAULT_SWEEP
    allocation_failures_preserve_state();
#endif
    half_close_releases_only_the_local_producer();
    writer_transaction_quarantine_and_close_receive();
    final_close_releases_queues_after_writer_commit();
    std::puts(CAPSID_CHANNEL_IO_NO_FAULT_SWEEP
        ? "channel IO Debug iterators: owned queues, leases, quota, credit and teardown PASS"
        : "channel IO: owned queues, leases, quota rollback and credit PASS");
}
