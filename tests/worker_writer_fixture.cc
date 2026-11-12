#include "worker_writer_receipt.h"
#include "worker_runtime.h"
#include "worker_writer_gate.h"

#include <cstdio>

using Gate = capsid_test::WorkerWriterGate;

int main(int argc, char** argv) {
    capsid_test::WriterReceipt receipt;
    if (!receipt.valid()) {
        std::fprintf(stderr, "WRITER_GATE could not create the exclusive owned receipt\n");
        return 74;
    }
    Gate gate;
    gate.pressure_ready = [](const Gate& value, void* state) {
        const bool published = static_cast<capsid_test::WriterReceipt*>(state)->publish(
            value.dial, value.peak_queue, value.stalled_writes, value.blocked_acks);
        std::fprintf(stderr,"WRITER_RECEIPT published=%d errno=%d\n",int(published),errno);
        return published;
    };
    gate.pressure_state = &receipt;
    CapsidWorkerIo io;
    io.write = [](const uint8_t* data, size_t size, capsid::OutboundBuffer::Writer next, void* next_state, void* opaque) -> ssize_t {
        auto& value = *static_cast<Gate*>(opaque);
        const bool stalled = value.stalled;
        const ssize_t result = Gate::write(data,size,next,next_state,opaque);
        if (!stalled && value.stalled)
            std::fprintf(stderr,"WRITER_STALL body=%d queue=%zu ackBlocked=%d invalid=%d\n",int(value.first_body_sent),value.peak_queue,int(value.ack_blocked),int(value.invalid));
        return result;
    };
    io.observe = [](CapsidWorkerIo::Event event,const capsid::protocol::Frame& frame,size_t queued,size_t limit,bool owner_present,bool owner_committed,void* opaque) {
        auto& value = *static_cast<Gate*>(opaque);
        const uint64_t previous_blocked = value.blocked_acks;
        Gate::observe(event,frame,queued,limit,owner_present,owner_committed,opaque);
        if (frame.type == capsid::protocol::kChannelAck && previous_blocked == 0)
            std::fprintf(stderr,"WRITER_ACK event=%d queue=%zu limit=%zu body=%d stall=%d owner=%d committed=%d blocked=%llu invalid=%d notified=%d\n",
                int(event),queued,limit,int(value.first_body_sent),int(value.stalled),int(owner_present),int(owner_committed),
                static_cast<unsigned long long>(value.blocked_acks),int(value.invalid),int(value.pressure_notified));
    };
    io.opaque = &gate;
    const int result = capsid_worker_main(argc,argv,&io);
    std::fprintf(stderr,
        "WRITER_GATE {\"complete\":%s,\"runtimeExit\":%d,\"queuePeak\":%zu,"
        "\"stalls\":%llu,\"blockedAcks\":%llu,\"ack\":%u,\"cancel\":%u,\"invalid\":%s}\n",
        gate.complete() ? "true" : "false", result, gate.peak_queue,
        static_cast<unsigned long long>(gate.stalled_writes),
        static_cast<unsigned long long>(gate.blocked_acks), gate.accepted_acks,
        gate.accepted_cancels, gate.invalid ? "true" : "false");
    return result != 0 ? result : gate.complete() ? 0 : 73;
}
