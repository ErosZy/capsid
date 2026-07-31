#ifndef CAPSID_CLIENT_IPC_METRICS_H
#define CAPSID_CLIENT_IPC_METRICS_H

#include <stddef.h>
#include <stdint.h>

struct capsid_worker;

namespace capsid {

/*
 * Internal, opt-in host-side transport counters used by the repository's
 * profiler. They are deliberately not part of the public runtime ABI.
 */
struct ClientIpcMetrics {
    uint64_t queued_frames;
    uint64_t queued_wire_bytes;
    uint64_t queue_would_block;
    uint64_t flush_calls;
    uint64_t socket_write_calls;
    uint64_t socket_write_bytes;
    uint64_t socket_write_eagain;
    uint64_t next_event_calls;
    uint64_t parsed_frames;
    uint64_t parser_payload_copied_bytes;
    uint64_t socket_read_calls;
    uint64_t socket_read_bytes;
    uint64_t socket_read_eagain;
    size_t queued_bytes_high_water;

    ClientIpcMetrics();
};

void client_ipc_metrics_enable(capsid_worker *worker, bool enabled);
void client_ipc_metrics_reset(capsid_worker *worker);
bool client_ipc_metrics_snapshot(const capsid_worker *worker,
                                 ClientIpcMetrics *metrics);

}  // namespace capsid

#endif
