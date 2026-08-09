// WP-05 §9.2: ManagedListener — one public HTTP listener for the Managed
// data plane. Owns the socket + the connection/header gates + the immutable
// routing policy; routes each request through ONE atomic RoutingTable
// snapshot (pins the shared_ptr<GenerationPool> it found) and submits to a
// pool-picked WorkerExecutor.
//
// Thread model:
//   - the io thread (owned by this object) runs a Boost.Beast acceptor and
//     every client session, exactly like SingleWorkerServer;
//   - the worker-response path is the §9.2 event sink: the pool's pump
//     thread (the sole drainer of every executor's event queue) forwards
//     events through a WorkerEventMailbox, which posts them onto this io
//     thread. The mailbox OWNS the io_context and lives as long as the
//     pool's sink holds it, so a post can never race the io_context's
//     destruction (the pool outlives the listener).
//   - start() validates the routing policy BEFORE binding (a header-mode
//     listener that is not trusted fails closed, §3.7/§8.1).
//
// v1 scope: connection ceiling, header/body limits, header/body-read
// deadlines, stream write deadline, path/subdomain/header routing, HEAD,
// request-upload credit, 400/404/503/504, keep-alive. Deferred (follow-up
// PRs): SSE permits, the E-1 admission queue, metrics, 408 synthesis.

#ifndef CAPSID_HOST_MANAGED_LISTENER_H
#define CAPSID_HOST_MANAGED_LISTENER_H

#include "host/host_config_model.h"
#include "host/routing_snapshot.h"

#include <cstdint>
#include <memory>
#include <string>

namespace capsid::host {

// Forward declaration: the ManagedListener implementation (the M1A server
// pattern — a namespace-scope class, defined in managed_listener.cc).
class Impl;

struct ManagedListenerOptions {
    // The configured listener. config.tcp is "host" or "host:port"; the
    // routing policy is compiled from config at start() and validated there
    // (fail closed before any bind).
    ListenerConfig config;
    // The published App → pool map. One snapshot load per request; the
    // pool found in that snapshot is pinned for the whole request.
    std::shared_ptr<RoutingTable> routing;
    // Beast request body limit for the buffered upload (413 beyond it).
    std::uint64_t max_request_body_bytes = 16U * 1024U * 1024U;
};

class ManagedListener {
public:
    explicit ManagedListener(ManagedListenerOptions options);
    ~ManagedListener();  // request_stop() + wait() (bounded)

    ManagedListener(const ManagedListener&) = delete;
    ManagedListener& operator=(const ManagedListener&) = delete;

    // Binds the socket, arms the accept loop and starts the io thread.
    // Fails (false, *error) on an invalid policy, an unparseable tcp
    // address, a bind error, or a missing routing table — before any
    // traffic is served.
    bool start(std::string* error);
    // §9.2 bridge wiring for pools that start serving AFTER this listener
    // is already running (the Managed coordinator's Admin-API deploys):
    // installs the listener's event sink on the pool so response events
    // reach the sessions. start() wires every pool in the current
    // RoutingTable snapshot automatically; use this for later pools.
    // Precondition: the pool has no in-flight requests yet — call before
    // publishing it into the RoutingTable.
    void wire_pool(const std::shared_ptr<GenerationPool>& pool);
    // Stops accepting, closes every session and joins the io thread. The
    // pool's event sink keeps working (posts drain into the mailbox queue
    // and are dropped with it) — the pool outlives the listener.
    void request_stop();
    bool wait(std::string* error);
    // The bound port (useful with "host:0" — the OS-assigned port).
    std::uint16_t bound_port() const;
    bool running() const;

private:
    std::shared_ptr<Impl> impl_;
};

}  // namespace capsid::host

#endif
