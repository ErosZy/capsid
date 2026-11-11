#ifndef CAPSID_WORKER_RUNTIME_IO_H
#define CAPSID_WORKER_RUNTIME_IO_H

#include "outbound_buffer.h"

// Internal I/O strategy for the embedded Worker loop. The ordinary Worker
// supplies nullptr. Test embeddings can exercise real queue saturation by
// stalling the socket writer, while observing the same channel state
// transitions without replacing protocol frames or JavaScript callbacks.
struct CapsidWorkerIo {
    enum class Event { kControlQueued, kWriteAccepted, kWriteBlocked };
    using Write = ssize_t (*)(const uint8_t*, size_t,
                             capsid::OutboundBuffer::Writer, void*, void*);
    using Observe = void (*)(Event, const capsid::protocol::Frame&,
                            size_t, size_t, bool, bool, void*);
    // Callbacks are synchronous, must not throw or reenter the runtime,
    // and must not retain frame/payload views after returning. A writer
    // delegates to the supplied real writer whenever it allows progress.
    // Test observer of actual HTTP terminal enqueue attempts and wire occupancy.
    using ObserveHttpTerminal = void (*)(const capsid::protocol::Frame&,
                                         size_t, size_t, bool, void*);
    ObserveHttpTerminal observe_http_terminal = nullptr;
    Write write = nullptr;
    Observe observe = nullptr;
    void* opaque = nullptr;
};

#endif
