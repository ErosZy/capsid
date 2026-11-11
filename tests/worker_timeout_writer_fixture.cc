#include "worker_runtime.h"
#include "worker_runtime_io.h"

#include <chrono>
#include <cstdio>

namespace {
namespace p = capsid::protocol;
using Clock = std::chrono::steady_clock;

struct Gate {
    bool armed = false;
    bool stalled = false;
    bool released = false;
    bool invalid = false;
    Clock::time_point armed_at{}, first_block{}, accepted_at{};
    uint64_t stalls = 0;
    uint64_t blocked = 0;
    unsigned accepted = 0;
    size_t peak = 0;

    static uint64_t request_id(const uint8_t* data) {
        uint64_t id = 0;
        for (unsigned i = 0; i < 8; ++i)
            id |= static_cast<uint64_t>(data[12 + i]) << (8 * i);
        return id;
    }

    static ssize_t write(const uint8_t* data, size_t size,
                         capsid::OutboundBuffer::Writer next,
                         void* next_state, void* opaque) {
        auto& gate = *static_cast<Gate*>(opaque);
        if (gate.armed && !gate.released) {
            if (Clock::now() - gate.armed_at < std::chrono::milliseconds(1500)) {
                gate.stalled = true;
                ++gate.stalls;
                return 0;
            }
            gate.released = true;
        }
        // Hold the first actual producer output, including its request
        // credit/head. Nothing is consumed from the pressure queue before
        // the complete input batch can fill it through the real runtime.
        if (!gate.armed && size >= p::kHeaderSize &&
                p::load_u32_at(data) == p::kMagic &&
                request_id(data) >= 93 && request_id(data) < 109) {
            gate.armed = true;
            gate.armed_at = Clock::now();
            gate.stalled = true;
            ++gate.stalls;
            return 0;
        }
        return next(data, size, next_state);
    }

    static void terminal(const p::Frame& frame, size_t queued, size_t limit,
                         bool accepted, void* opaque) {
        auto& gate = *static_cast<Gate*>(opaque);
        if (queued > gate.peak) gate.peak = queued;
        if (queued > limit || limit != 1024u * 1024u) gate.invalid = true;
        if (frame.request_id != 92) return;
        if (frame.type != p::kError || frame.flags != p::kErrorFlagTimeout)
            gate.invalid = true;
        const size_t wire = p::kHeaderSize + frame.payload.size();
        if (!accepted) {
            const auto now = Clock::now();
            if (!gate.blocked) gate.first_block = now;
            ++gate.blocked;
            if (!gate.armed || !gate.stalled || gate.released ||
                    queued != limit || wire <= limit - queued)
                gate.invalid = true;
        } else {
            gate.accepted_at = Clock::now();
            ++gate.accepted;
            if (!gate.blocked || !gate.released) gate.invalid = true;
        }
    }

    bool complete() const {
        return !invalid && armed && stalled && released && stalls && blocked != 0 &&
            accepted == 1 && peak == 1024u * 1024u &&
            accepted_at - first_block >= std::chrono::milliseconds(100);
    }
};
}

int main(int argc, char** argv) {
    Gate gate;
    CapsidWorkerIo io;
    io.write = Gate::write;
    io.observe_http_terminal = Gate::terminal;
    io.opaque = &gate;
    const int result = capsid_worker_main(argc, argv, &io);
    std::fprintf(stderr,
        "TIMEOUT_WRITER_GATE {\"complete\":%s,\"runtimeExit\":%d,\"queuePeak\":%zu,"
        "\"stalls\":%llu,\"blockedTerminals\":%llu,\"acceptedTerminals\":%u,"
        "\"terminalHeldMs\":%lld,\"invalid\":%s}\n",
        gate.complete() ? "true" : "false", result, gate.peak,
        static_cast<unsigned long long>(gate.stalls),
        static_cast<unsigned long long>(gate.blocked), gate.accepted,
        gate.blocked && gate.accepted ? static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                gate.accepted_at - gate.first_block).count()) : 0LL,
        gate.invalid ? "true" : "false");
    return result ? result : gate.complete() ? 0 : 73;
}
