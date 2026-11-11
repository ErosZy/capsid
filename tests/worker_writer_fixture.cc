#include "worker_runtime.h"
#include "worker_writer_gate.h"

#include <cstdio>

using Gate = capsid_test::WorkerWriterGate;

int main(int argc, char** argv) {
    Gate gate;
    CapsidWorkerIo io;
    io.write = Gate::write;
    io.observe = Gate::observe;
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
