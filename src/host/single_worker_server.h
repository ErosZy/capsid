#ifndef CAPSID_HOST_SINGLE_WORKER_SERVER_H
#define CAPSID_HOST_SINGLE_WORKER_SERVER_H

#include <boost/asio/ip/tcp.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace capsid::host {

class StructuredLog;
class MetricsRegistry;

// Defined in single_worker_server.cc; the server keeps it behind a shared
// pointer so the header exposes no Boost or worker-Runtime types.
class Impl;

// M1A benchmark fixture configuration. Every field is validated by main.cc
// before the server is constructed.
struct SingleWorkerServerOptions {
    std::string worker_path;
    std::string source_bundle_path;
    std::string source_name;
    std::string application;
    std::string listen_address;
    std::uint16_t listen_port = 0;
    std::string public_scheme;
    std::string public_authority;
    std::uint64_t request_timeout_ms = 0;
    std::uint32_t initial_stream_window = 0;
    bool strict_sandbox = false;
    int ready_fd = -1;
    // M2 static-pool composition. write_ready_record=false skips the READY
    // publication (the pool writes one canonical record only after every
    // shard is READY); install_process_signals=false skips the process-level
    // SIGTERM/SIGINT wiring (the pool owns signal delivery); so_reuseport
    // shares one address:port across several listeners and is rejected
    // statically when the platform cannot honor it — never degraded to a
    // fake multi-process.
    bool write_ready_record = true;
    bool install_process_signals = true;
    bool so_reuseport = false;
    // External-accept mode (Windows multi-shard static-pool): the shard
    // spawns its worker and runs its session reactor, but does NOT bind or
    // accept a listener. The pool owns the single shared acceptor and hands
    // accepted sockets in through adopt_socket(). Mutually exclusive with
    // defer_accept and so_reuseport.
    bool external_accept = false;
    // Pool barrier mode: the listener is bound and the worker is READY,
    // but the server does NOT accept connections until activate_accept()
    // is called. The pool starts every shard "prepared but not accepting"
    // and activates them only when the whole pool is READY.
    bool defer_accept = false;
    // M2 E-1 admission control (design §10.3). The shard's inflight ceiling
    // (0 = unlimited) is forwarded to the worker as its max-inflight hard
    // boundary and bounds the Host-side request table; the bounded queue
    // (queueRequests slots, queueHeaderBytes cap, queueTimeout deadline)
    // parks requests while every slot is busy. queue_requests == 0 disables
    // queueing (an inflight-full shard rejects directly with 429);
    // queue_header_bytes == 0 imposes no byte cap; queue_timeout_ms == 0
    // waits indefinitely.
    std::uint64_t max_inflight_per_worker = 128;
    std::uint64_t queue_requests = 0;
    std::uint64_t queue_header_bytes = 0;
    std::uint64_t queue_timeout_ms = 0;
    // M2 E-2 SSE streaming permit (design §9.3). A text/event-stream
    // response must acquire its permit before the head reaches the client;
    // a permit-exhausted request is cancelled and answered with a
    // synthesized 503. The permit is held only by the Content-Type, never
    // by a missing Content-Length. Must stay below max_inflight_per_worker
    // except for the single documented 1/1 boundary. 0 = unlimited.
    std::uint64_t max_streaming_inflight_per_worker = 2;
    // Silence past this deadline cancels the stream and closes the
    // connection (heartbeat keep-alive; 0 = no idle timeout).
    std::uint64_t stream_idle_timeout_ms = 60000;
    // M2 E-3 slow-client write deadline (§9.2). A socket write that has
    // not completed within this deadline — the client stopped reading —
    // cancels the request and releases its resources. Host-side socket
    // view only: the worker-side request timeout is a separate timer
    // (§8.3, independent accounting). 0 = disabled.
    std::uint64_t write_timeout_ms = 60000;
    // M2 item 7: the process-wide structured log and metrics registry
    // (design §12). Null disables event logging/metrics on this path.
    StructuredLog* log = nullptr;
    MetricsRegistry* metrics = nullptr;
    // v0.1.3 local capsid.json permissions (--capsid-json). Defaults to
    // ./capsid.json; a missing default file is a no-op (the deny-all
    // defaults stay), while capsid_json_required=true (an explicit
    // --capsid-json) fails startup when the file is missing.
    std::string capsid_json_path = "capsid.json";
    bool capsid_json_required = false;
};

// M1A single-worker Host data plane: one Boost.Asio io_context owner, one
// capsid_worker owned by a dedicated worker thread, Beast HTTP/1 as the sole
// framing authority, and M0 request normalization at the route boundary.
// This mode is an explicit benchmark fixture, not a deployment API: it never
// reads or writes active.json and must not be described as a production path.
class SingleWorkerServer {
public:
    explicit SingleWorkerServer(SingleWorkerServerOptions options);
    ~SingleWorkerServer();

    SingleWorkerServer(const SingleWorkerServer&) = delete;
    SingleWorkerServer& operator=(const SingleWorkerServer&) = delete;

    // Controllable lifecycle (M2). start() runs the spawn/load/READY/
    // listen/ready-fd sequence and then starts the HTTP event loop on a
    // background thread; it returns true only once the server can serve
    // requests immediately. A second start() on the same object returns
    // false without touching the first start's service. Any failure inside
    // start() recycles every created worker/thread/listener before it
    // returns false.
    bool start(const std::vector<std::uint8_t>& bundle, std::string* error);

    // Thread-safe, idempotent stop request: repeated calls never close
    // twice, corrupt a descriptor or trigger a Runtime double shutdown.
    void request_stop();

    // Waits for the event-loop thread and the worker owner thread to exit
    // completely (never detaches); returns true on a normal stop. Errors
    // use static, redacted text only.
    bool wait(std::string* error);

    // Compatibility wrapper preserving the CLI behavior: runs start() then
    // waits for SIGTERM/SIGINT; returns the process exit code.
    int run(const std::vector<std::uint8_t>& bundle);

    // The port the listener actually bound after a successful start(); 0
    // when the server is not listening (start failed or not yet started).
    // Lets a pool query the first shard's kernel-assigned port and hand it
    // to the remaining shards.
    std::uint16_t bound_port() const;

    // Pool barrier entry: arms the accept loop for a server started with
    // defer_accept=true. Fails without effect when the server was not
    // started in defer mode or the accept was already activated. The
    // activation happens on the server's own io thread and this call
    // returns only after that thread confirmed the accept is armed (or
    // rejected the activation); it never hangs on a stopped/dead shard.
    bool activate_accept(std::string* error);

    // Thread-safe worker availability: true from the successful READY
    // handshake until the worker exits (kExit), stop completes or a
    // startup failure; false otherwise. Lets a pool report its live
    // capacity (N → N−1 on a worker fault) without caching.
    bool worker_available() const;

    // External-accept mode only: hands an already-accepted, owned Asio
    // socket to this shard's io thread. Returns false when the shard is not
    // running or cannot take the connection; on false the socket is closed
    // by the caller's move (or by its destructor).
    bool adopt_socket(boost::asio::ip::tcp::socket socket);

    // M2 §7.5 bounded drain. Once the new pool has been published, the old
    // pool begins draining: the listener stops accepting new connections
    // (the routing/registry no longer delivers to it) while every in-flight
    // request keeps running. When the inflight table and the admission
    // queue both empty, the server shuts down on its own (worker flushes
    // and reads to EXIT). When deadline_ms elapses first (0 = wait
    // indefinitely), every remaining request is cancelled (counted) and,
    // after the brief shutdown grace, the worker is terminated. The server
    // is fully reaped by wait(); thread-safe and idempotent, safe to call
    // before or after request_stop().
    void begin_drain(std::uint64_t deadline_ms);

    // §7.5 row 7: the drain report — total drain time (begin → inflight
    // cleared or forced cancellation) and the number of requests cancelled
    // by the deadline. Thread-safe snapshot; meaningful after wait().
    struct DrainMetrics {
        bool draining = false;
        bool finished = false;
        std::uint64_t total_ms = 0;
        std::uint64_t forced_cancellations = 0;
    };
    DrainMetrics drain_metrics() const;

private:
    std::shared_ptr<Impl> impl_;
};

}  // namespace capsid::host

#endif
