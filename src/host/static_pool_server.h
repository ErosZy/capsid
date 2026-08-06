#ifndef CAPSID_HOST_STATIC_POOL_SERVER_H
#define CAPSID_HOST_STATIC_POOL_SERVER_H

#include "host/single_worker_server.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace capsid::host {

// Defined in static_pool_server.cc; kept behind a shared pointer so the
// header exposes no implementation details.
class StaticPoolServerImpl;

// M2 fixed-pool composition options. worker_options is the template for
// every shard; workers is the fixed pool size (0 rejects startup). The
// admission fields (E-1, design §10.3) are pool-level: they are forwarded
// into every shard at start(), and a shard's own values (when set) win.
struct StaticPoolServerOptions {
    SingleWorkerServerOptions worker_options;
    std::uint32_t workers = 0;
    std::uint64_t max_inflight_per_worker = 0;
    std::uint64_t queue_requests = 0;
    std::uint64_t queue_header_bytes = 0;
    std::uint64_t queue_timeout_ms = 0;
};

// M2 fixed-pool Host data plane: N independently owned shards, each with
// its own SingleWorkerServer, its own worker process and its own Asio
// reactor thread, all listening on the same address:port through
// SO_REUSEPORT. Workers never migrate or share across shards. The pool
// publishes exactly one canonical READY record — only after every shard is
// READY — and startup is atomic: any shard failure (or a failed pool READY
// publication) rolls back every started shard before start() returns.
// The StaticPoolState machine drives the activation contract
// (register -> READY -> activate; see static_pool.h). Queueing, load
// selection and SSE admission belong to later M2 batches.
class StaticPoolServer {
public:
    explicit StaticPoolServer(StaticPoolServerOptions options);
    ~StaticPoolServer();

    StaticPoolServer(const StaticPoolServer&) = delete;
    StaticPoolServer& operator=(const StaticPoolServer&) = delete;

    // Starts every shard (spawn/load/READY/listen for each), then writes
    // the single pool READY record. Returns true only when the whole pool
    // is live and can serve requests immediately. A second start() returns
    // false without touching the running pool; any failure rolls the
    // whole pool back before returning false.
    bool start(const std::vector<std::uint8_t>& bundle, std::string* error);

    // Thread-safe, idempotent stop: requests a stop on every shard (all
    // in one pass, then wait() joins them in parallel) so the bounded
    // shutdown window is not paid N times serially.
    void request_stop();

    // Waits for every shard's event-loop and worker owner threads to exit
    // completely (never detaches); returns true on a normal stop.
    bool wait(std::string* error);

    // The number of LIVE worker shards, reported from each shard's
    // thread-safe availability: the fixed pool size while healthy, N−1
    // after a worker fault, 0 after stop/wait or a rolled-back start.
    // Never a cached startup count.
    std::size_t active_workers() const;

    // Benchmark-only blocking entry (NOT a managed production path): starts
    // the pool, then waits for SIGTERM/SIGINT on the calling thread
    // (sigwait, no handler), performs the bounded stop/wait and returns
    // the process exit code. Shards never install their own signal wiring.
    int run(const std::vector<std::uint8_t>& bundle);

private:
    std::shared_ptr<StaticPoolServerImpl> impl_;
};

}  // namespace capsid::host

#endif
