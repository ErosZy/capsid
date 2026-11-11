#ifndef CAPSID_CLIENT_IPC_METRICS_H
#define CAPSID_CLIENT_IPC_METRICS_H

#include <atomic>
#include <stddef.h>
#include <stdint.h>

struct capsid_worker;

namespace capsid {

/*
 * Internal, opt-in host-side transport counters used by the repository's
 * profiler. They are deliberately not part of the public runtime ABI.
 *
 * Every field is atomic: the worker thread increments the counters while
 * the host's metrics thread snapshots them, and snapshot() exchanges each
 * field (read-and-zero in one atomic step), so a snapshot is a delta — the
 * same per-line semantics as the host-side metrics — and a concurrent
 * increment is never torn or lost.
 */
struct ClientIpcMetrics {
    // This bank can also retain the final deltas after its worker handle
    // is released. An empty bank has deterministic zero values.
    std::atomic<uint64_t> queued_frames{0};
    std::atomic<uint64_t> queued_wire_bytes{0};
    std::atomic<uint64_t> queue_would_block{0};
    std::atomic<uint64_t> flush_calls{0};
    std::atomic<uint64_t> socket_write_calls{0};
    std::atomic<uint64_t> socket_write_bytes{0};
    std::atomic<uint64_t> socket_write_eagain{0};
    std::atomic<uint64_t> next_event_calls{0};
    std::atomic<uint64_t> parsed_frames{0};
    std::atomic<uint64_t> parser_payload_copied_bytes{0};
    std::atomic<uint64_t> socket_read_calls{0};
    std::atomic<uint64_t> socket_read_bytes{0};
    std::atomic<uint64_t> socket_read_eagain{0};
    std::atomic<size_t> queued_bytes_high_water{0};

    ClientIpcMetrics();
};

void client_ipc_metrics_enable(capsid_worker *worker, bool enabled);
void client_ipc_metrics_reset(capsid_worker *worker);
bool client_ipc_metrics_snapshot(capsid_worker *worker,
                                 ClientIpcMetrics *metrics);
// Transfers each interval counter exactly once, without requiring a live
// worker handle. Source and destination must be distinct banks.
bool client_ipc_metrics_drain(ClientIpcMetrics *source,
                              ClientIpcMetrics *metrics);

}  // namespace capsid

#endif
