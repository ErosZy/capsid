#ifndef CAPSID_HOST_IPC_METRICS_H
#define CAPSID_HOST_IPC_METRICS_H

#include "client_ipc_metrics.h"

#include <cstdint>
#include <cstdio>

namespace capsid::host {

// One read-and-zero interval of the opt-in Host profiler. These counters
// describe transport work; they are not part of the public Runtime ABI.
struct HostIpcMetrics {
    std::uint64_t commands_submitted = 0;
    std::uint64_t command_batches = 0;
    std::uint64_t commands_executed = 0;
    std::uint64_t flush_calls = 0;
    std::uint64_t flush_eagain = 0;
    std::uint64_t events_queued = 0;
    std::uint64_t asio_posts = 0;
    std::uint64_t response_heads = 0;
    std::uint64_t response_body_frames = 0;
    std::uint64_t response_ends = 0;
    std::uint64_t grant_commands = 0;
    std::uint64_t credit_bytes_granted = 0;
    std::uint64_t credit_stall_count = 0;
    std::uint64_t command_queue_high_water = 0;
    std::uint64_t event_queue_high_water = 0;
};

void write_ipc_metrics_line(std::FILE* output,
                            const HostIpcMetrics& host,
                            const capsid::ClientIpcMetrics& client_metrics);

}  // namespace capsid::host

#endif
