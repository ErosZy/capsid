#ifndef CAPSID_TESTS_WORKER_WRITER_GATE_H
#define CAPSID_TESTS_WORKER_WRITER_GATE_H

#include "worker_runtime_io.h"

namespace capsid_test {
namespace p = capsid::protocol;

struct WorkerWriterGate {
    bool first_body_seen = false;
    bool first_body_sent = false;
    size_t first_remaining = 0;
    bool stalled = false;
    bool released = false;
    bool ack_blocked = false;
    bool cancel_queued = false;
    bool invalid = false;
    uint64_t dial = 0;
    size_t peak_queue = 0;
    uint64_t stalled_writes = 0;
    uint64_t blocked_acks = 0;
    unsigned accepted_acks = 0;
    unsigned accepted_cancels = 0;

    static uint64_t request_id(const uint8_t* data) {
        uint64_t result = 0;
        for (unsigned index = 0; index < 8; ++index)
            result |= static_cast<uint64_t>(data[12+index]) << (index*8);
        return result;
    }
    static bool filler_body(const uint8_t* data, size_t size) {
        return size >= p::kHeaderSize && p::load_u32_at(data) == p::kMagic &&
            data[6] == p::kResponseBody && data[7] == 0 && request_id(data) == 100;
    }
    static ssize_t write(const uint8_t* data, size_t size,
                         capsid::OutboundBuffer::Writer next, void* next_state, void* opaque) {
        auto& gate = *static_cast<WorkerWriterGate*>(opaque);
        // Let the real head and first body reach the host. The next pump
        // fills an entire wire-budget-sized body frame without a head
        // occupying part of that budget. Stall that second frame exactly
        // as an EAGAIN writer does, while input and JS timers keep running.
        if (!gate.released && (gate.stalled || (gate.first_body_sent && filler_body(data,size)))) {
            gate.stalled = true;
            ++gate.stalled_writes;
            return 0;
        }
        if (!gate.first_body_seen && filler_body(data,size)) {
            gate.first_body_seen = true;
            gate.first_remaining = p::kHeaderSize + p::load_u32_at(data+20);
        }
        const ssize_t written = next(data,size,next_state);
        if (written > 0 && gate.first_remaining != 0) {
            const auto count = static_cast<size_t>(written);
            if (count > gate.first_remaining) gate.invalid = true;
            else gate.first_remaining -= count;
            if (!gate.first_remaining) gate.first_body_sent = true;
        }
        return written;
    }

    static void observe(CapsidWorkerIo::Event event, const p::Frame& frame,
                         size_t queued, size_t limit, bool owner_present,
                         bool owner_committed, void* opaque) {
        auto& gate = *static_cast<WorkerWriterGate*>(opaque);
        if (queued > gate.peak_queue) gate.peak_queue = queued;
        if (queued > limit || limit != 4096) gate.invalid = true;
        std::string error;
        if (frame.type == p::kChannelAck) {
            p::ChannelAckPayload ack;
            if (!p::decode_channel_ack(frame.payload.data(),frame.payload.size(),&ack,&error) ||
                ack.kind != p::kAckDialResult || ack.channel_id != 1 || !ack.dial_id) {
                gate.invalid = true;
                return;
            }
            if (gate.dial && ack.dial_id != gate.dial) gate.invalid = true;
            gate.dial = ack.dial_id;
            if (event == CapsidWorkerIo::Event::kWriteBlocked) {
                ++gate.blocked_acks;
                const size_t wire = p::kHeaderSize + frame.payload.size();
                // A full queue can reject ACK before its next flush calls
                // the writer, especially when Host and Worker share a CPU.
                // The first body and insufficient wire capacity prove this
                // prefix. CANCEL and complete() still require a real stall.
                if (!gate.first_body_sent || (gate.released && !gate.cancel_queued) || !owner_present || owner_committed ||
                    queued > limit || wire <= limit-queued) gate.invalid = true;
                gate.ack_blocked = true;
            } else if (event == CapsidWorkerIo::Event::kWriteAccepted) {
                ++gate.accepted_acks;
                if (!gate.cancel_queued || !gate.released || !gate.ack_blocked) {
                    gate.invalid = true;
                    gate.released = true;  // Cleanup never changes a failed verdict.
                }
            }
        } else if (frame.type == p::kChannelCancel) {
            p::ChannelCancelPayload cancel;
            if (!p::decode_channel_cancel(frame.payload.data(),frame.payload.size(),&cancel,&error) ||
                cancel.channel_id != 1 || cancel.dial_id != 0) {
                gate.invalid = true;
                gate.released = true;
                return;
            }
            if (event == CapsidWorkerIo::Event::kControlQueued) {
                if (!gate.ack_blocked || !gate.stalled || gate.released || !owner_present || owner_committed)
                    gate.invalid = true;
                gate.cancel_queued = true;
                gate.released = true;
            } else if (event == CapsidWorkerIo::Event::kWriteAccepted) {
                ++gate.accepted_cancels;
                if (gate.accepted_acks != 1 || !gate.cancel_queued) gate.invalid = true;
            }
        } else if (event == CapsidWorkerIo::Event::kWriteAccepted &&
                   (frame.type == p::kChannelData || frame.type == p::kChannelClose)) {
            gate.invalid = true;
        }
    }

    bool complete() const {
        return !invalid && first_body_sent && stalled && stalled_writes != 0 &&
            ack_blocked && blocked_acks != 0 && cancel_queued && released &&
            accepted_acks == 1 && accepted_cancels == 1 && peak_queue == 4096;
    }
};

}  // namespace capsid_test
#endif
