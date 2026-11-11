#include "host/ipc_metrics.h"

#include <cinttypes>

namespace capsid::host {

void write_ipc_metrics_line(std::FILE* output,
                            const HostIpcMetrics& host,
                            const capsid::ClientIpcMetrics& client_metrics) {
    // Write one compact JSON line to stderr; the runner captures it.
    std::fprintf(output,
        "{"
        "\"host\":{"
        "\"commands_submitted\":%" PRIu64 ",\"command_batches\":%" PRIu64 ","
        "\"commands_executed\":%" PRIu64 ",\"flush_calls\":%" PRIu64 ",\"flush_eagain\":%" PRIu64 ","
        "\"events_queued\":%" PRIu64 ",\"asio_posts\":%" PRIu64 ","
        "\"response_heads\":%" PRIu64 ",\"response_body_frames\":%" PRIu64 ","
        "\"response_ends\":%" PRIu64 ",\"grant_commands\":%" PRIu64 ","
        "\"credit_bytes_granted\":%" PRIu64 ",\"credit_stall_count\":%" PRIu64 ","
        "\"command_queue_hw\":%zu,\"event_queue_hw\":%zu},"
        "\"client\":{"
        "\"queued_frames\":%" PRIu64 ",\"queued_wire_bytes\":%" PRIu64 ",\"queue_would_block\":%" PRIu64 ","
        "\"flush_calls\":%" PRIu64 ",\"socket_write_calls\":%" PRIu64 ",\"socket_write_bytes\":%" PRIu64 ","
        "\"socket_write_eagain\":%" PRIu64 ",\"next_event_calls\":%" PRIu64 ",\"parsed_frames\":%" PRIu64 ","
        "\"parser_payload_copied_bytes\":%" PRIu64 ","
        "\"socket_read_calls\":%" PRIu64 ",\"socket_read_bytes\":%" PRIu64 ","
        "\"socket_read_eagain\":%" PRIu64 ",\"queued_bytes_hw\":%zu"
        "}}\n",
        // host
        host.commands_submitted,
        host.command_batches,
        host.commands_executed,
        host.flush_calls,
        host.flush_eagain,
        host.events_queued,
        host.asio_posts,
        host.response_heads,
        host.response_body_frames,
        host.response_ends,
        host.grant_commands,
        host.credit_bytes_granted,
        host.credit_stall_count,
        (size_t)host.command_queue_high_water,
        (size_t)host.event_queue_high_water,
        // client
        client_metrics.queued_frames.load(std::memory_order_relaxed),
        client_metrics.queued_wire_bytes.load(std::memory_order_relaxed),
        client_metrics.queue_would_block.load(std::memory_order_relaxed),
        client_metrics.flush_calls.load(std::memory_order_relaxed),
        client_metrics.socket_write_calls.load(std::memory_order_relaxed),
        client_metrics.socket_write_bytes.load(std::memory_order_relaxed),
        client_metrics.socket_write_eagain.load(std::memory_order_relaxed),
        client_metrics.next_event_calls.load(std::memory_order_relaxed),
        client_metrics.parsed_frames.load(std::memory_order_relaxed),
        client_metrics.parser_payload_copied_bytes.load(std::memory_order_relaxed),
        client_metrics.socket_read_calls.load(std::memory_order_relaxed),
        client_metrics.socket_read_bytes.load(std::memory_order_relaxed),
        client_metrics.socket_read_eagain.load(std::memory_order_relaxed),
        (size_t)client_metrics.queued_bytes_high_water);
}

}  // namespace capsid::host
