#include "worker_writer_gate.h"

#include <cstdio>
#include <cstdlib>

namespace {
namespace p = capsid::protocol;
using Gate = capsid_test::WorkerWriterGate;
using Event = CapsidWorkerIo::Event;

void require(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

p::Frame ack(uint64_t dial = 1) {
    p::ChannelAckPayload payload;
    payload.channel_id = 1;
    payload.dial_id = dial;
    p::Frame frame;
    frame.type = p::kChannelAck;
    require(p::encode_channel_ack(payload, &frame.payload), "encode ACK");
    return frame;
}

p::Frame cancel() {
    p::ChannelCancelPayload payload;
    payload.channel_id = 1;
    p::Frame frame;
    frame.type = p::kChannelCancel;
    require(p::encode_channel_cancel(payload, &frame.payload), "encode CANCEL");
    return frame;
}

Gate delivered() {
    Gate gate;
    gate.first_body_seen = true;
    gate.first_body_sent = true;
    return gate;
}

ssize_t real_write(const uint8_t*, size_t size, void*) {
    return static_cast<ssize_t>(size);
}

void stall(Gate& gate) {
    const std::vector<uint8_t> payload(4096 - p::kHeaderSize, 'f');
    std::vector<uint8_t> bytes;
    require(p::append_encoded(p::kResponseBody, 0, 100, payload.data(), payload.size(), &bytes),
            "encode full-budget body");
    require(bytes.size() == 4096, "body fills actual wire budget");
    require(Gate::write(bytes.data(), bytes.size(), real_write, nullptr, &gate) == 0,
            "real write callback is stalled");
    require(gate.stalled && gate.stalled_writes == 1, "record actual stalled write");
}

void blocked(Gate& gate, size_t queued = 4096, bool owner = true, bool committed = false) {
    Gate::observe(Event::kWriteBlocked, ack(), queued, 4096, owner, committed, &gate);
}

void queued_cancel(Gate& gate) {
    Gate::observe(Event::kControlQueued, cancel(), 4096, 4096, true, false, &gate);
}

void accepted_ack(Gate& gate) {
    Gate::observe(Event::kWriteAccepted, ack(), 40, 4096, true, false, &gate);
}

void accepted_cancel(Gate& gate) {
    Gate::observe(Event::kWriteAccepted, cancel(), 48, 4096, true, false, &gate);
}

void valid_order(bool ack_before_write) {
    Gate gate = delivered();
    if (ack_before_write) {
        blocked(gate);
        require(!gate.invalid && !gate.stalled, "full queue can block ACK before the first write attempt");
        stall(gate);
    } else {
        stall(gate);
        blocked(gate);
        require(!gate.invalid, "write can stall before the first blocked ACK");
    }
    queued_cancel(gate);
    accepted_ack(gate);
    accepted_cancel(gate);
    require(gate.complete(), "both scheduler orders retain the complete pressure proof");
}
}  // namespace

int main() {
    valid_order(false);
    valid_order(true);
    {
        Gate gate;
        blocked(gate);
        require(gate.invalid, "reject ACK before first-body delivery");
    }
    {
        Gate gate = delivered();
        blocked(gate, 0);
        require(gate.invalid, "reject fabricated queue pressure");
    }
    {
        Gate gate = delivered();
        blocked(gate, 4097);
        require(gate.invalid, "reject queue overflow");
    }
    for (unsigned mode = 0; mode != 2; ++mode) {
        Gate gate = delivered();
        blocked(gate, 4096, mode != 0, mode != 0);
        require(gate.invalid, "reject missing or committed owner");
    }
    {
        Gate gate = delivered();
        blocked(gate);
        queued_cancel(gate);
        require(gate.invalid && !gate.complete(), "CANCEL still requires a real prior write stall");
    }
    {
        Gate gate = delivered();
        stall(gate);
        queued_cancel(gate);
        require(gate.invalid, "reject CANCEL before blocked ACK");
    }
    {
        Gate gate = delivered();
        stall(gate);
        blocked(gate);
        accepted_ack(gate);
        require(gate.invalid, "reject accepted ACK before queued CANCEL releases writer");
    }
    {
        Gate gate = delivered();
        stall(gate);
        blocked(gate);
        queued_cancel(gate);
        accepted_cancel(gate);
        require(gate.invalid, "reject CANCEL transmission before ACK");
    }
    {
        Gate gate = delivered();
        blocked(gate);
        Gate::observe(Event::kWriteBlocked, ack(2), 4096, 4096, true, false, &gate);
        require(gate.invalid, "reject changed dial identity");
    }
    {
        Gate gate = delivered();
        stall(gate);
        blocked(gate);
        queued_cancel(gate);
        accepted_ack(gate);
        accepted_cancel(gate);
        accepted_ack(gate);
        require(!gate.complete(), "reject duplicate ACK");
    }
    std::puts("writer gate: both scheduler orders and eleven invalid-pressure controls PASS");
    return 0;
}
