// M1A single-worker Host data plane.
//
// Thread model (matches the frozen ownership rules):
//   - one io_context owner thread runs the acceptor, every client session and
//     every timer; it is the only thread touching Beast objects;
//   - one worker thread exclusively owns the capsid_worker (spawned/loaded by
//     the main thread before the worker thread starts) and executes all
//     Runtime API calls;
//   - the two threads communicate through two mutex-protected queues; the
//     worker thread copies every event payload before posting it to the io
//     thread, so payload/header views never outlive the next next_event call.
//
// HTTP framing is Beast's and only Beast's: request parsing, keep-alive
// handling, response serialization, chunked encoding and content-length are
// delegated to http::serializer/parser. Route/app identity uses M0
// normalize_public_request(). Body credits follow the frozen contract:
// request bytes are written only when the worker grants request credit, and
// response credit is returned only after the client write succeeds. HEAD
// consumes the worker body without exposing it. Client disconnect cancels
// the worker request, worker timeouts map to 504 and route misses to 404.

#include "host/single_worker_server.h"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "capsid/runtime.h"
#include "client_ipc_metrics.h"
#include "host/credit_limits.h"
#include "host/request_normalization.h"
#include "host/response_body_batch.h"
#include "host/structured_log.h"
#include "host/worker_event_source.h"
#include "host/worker_executor.h"

namespace capsid::host {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

namespace {

// M2 item 7 (design §12.2): single write path for data-plane control
// events. Null log (unit fixtures without the process-wide instance) is a
// no-op. App-lane (kLog forwarding) goes through LogLane::kApp; every
// Host-side failure below is a control-plane event.
void emit_log(StructuredLog* log, LogLane lane, LogFields fields) {
    if (log != nullptr) {
        log->log(lane, std::move(fields));
    }
}

}  // namespace
using tcp = asio::ip::tcp;
using SteadyClock = std::chrono::steady_clock;

constexpr std::size_t kMaxRequestBodyBytes = 16u * 1024u * 1024u;

// Early-credit window (host): frames are reimbursed on receive while
// the per-request HTTP write queue is below this; beyond it credit
// falls back to write-completion so slow clients stay bounded.
static const size_t kEarlyCreditWindow = 64u * 1024u;

// RFC 7230 tchar: the only legal header-name characters.
bool is_token_char(unsigned char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9')) {
        return true;
    }
    switch (c) {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
        return true;
    default:
        return false;
    }
}

// RFC 7230 field-value byte: HTAB / SP / VCHAR / obs-text. Anything else
// (CR, LF, NUL, other controls) would corrupt the serialized response.
bool valid_field_value_byte(unsigned char c) {
    return c == '\t' || (c >= 0x20 && c <= 0x7e) || c >= 0x80;
}

std::string ascii_lower(std::string_view text) {
    std::string lower(text);
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return lower;
}

bool is_event_stream_content_type(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() &&
           (value[begin] == ' ' || value[begin] == '\t')) {
        ++begin;
    }
    static constexpr std::string_view kEventStream = "text/event-stream";
    if (value.size() - begin < kEventStream.size() ||
        !std::equal(kEventStream.begin(), kEventStream.end(),
                    value.begin() + static_cast<std::ptrdiff_t>(begin),
                    [](unsigned char a, unsigned char b) {
                        if (a >= 'A' && a <= 'Z') {
                            a = static_cast<unsigned char>(a - 'A' + 'a');
                        }
                        if (b >= 'A' && b <= 'Z') {
                            b = static_cast<unsigned char>(b - 'A' + 'a');
                        }
                        return a == b;
                    })) {
        return false;
    }
    std::size_t end = begin + kEventStream.size();
    while (end < value.size() &&
           (value[end] == ' ' || value[end] == '\t')) {
        ++end;
    }
    return end == value.size() || value[end] == ';';
}

// Parses the comma-separated token list of a Connection header and collects
// the (lowercased) nominated names. Returns false on an empty or non-token
// nomination, which fails the response closed (RFC 7230 §6.1).
bool collect_connection_nominations(std::string_view value,
                                    std::set<std::string>* nominated) {
    std::size_t begin = 0;
    while (begin <= value.size()) {
        while (begin < value.size() &&
               (value[begin] == ' ' || value[begin] == '\t')) {
            ++begin;
        }
        const std::size_t end = value.find(',', begin);
        const std::size_t token_end =
            end == std::string_view::npos ? value.size() : end;
        std::size_t trimmed = token_end;
        while (trimmed > begin &&
               (value[trimmed - 1] == ' ' || value[trimmed - 1] == '\t')) {
            --trimmed;
        }
        if (trimmed == begin) {
            return false;
        }
        for (std::size_t index = begin; index < trimmed; ++index) {
            if (!is_token_char(static_cast<unsigned char>(value[index]))) {
                return false;
            }
        }
        nominated->insert(ascii_lower(value.substr(begin, trimmed - begin)));
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return true;
}

// CommandType, Command and WorkerEvent live in worker_executor.h: the
// executor owns the command queue and the event queue, so their element
// types belong to its public contract (spec §8.1).

class Session;

struct QueuedResponseBody {
    std::vector<std::uint8_t> bytes;
    bool credit_returned_early = false;
};

// Request state owned by the io thread only.
struct PendingRequest {
    // Shared ownership: an in-flight async write on the session stream must
    // keep the Session alive until the operation completes, otherwise the
    // completion handler reads a freed stream when the client disconnects
    // (or a timeout closes the connection) mid-write.
    std::shared_ptr<Session> session;
    std::string method;
    bool keep_alive = false;
    unsigned version = 11;

    // Request direction.
    std::string request_body;
    std::size_t request_body_offset = 0;
    std::uint64_t request_credit = 0;
    bool request_ended = false;

    // Response direction. Shared ownership: an async_write in flight keeps
    // the serializer (and the response it references) alive even if this
    // request is cancelled or fails before the write completes.
    std::shared_ptr<http::response<http::buffer_body>> response;
    std::shared_ptr<http::response_serializer<http::buffer_body>> serializer;
    // Complete non-streamed responses up to the protocol's 4 KiB fixed-body
    // bound are held until RESPONSE_END and emitted as one fixed-length Beast
    // write. The Runtime supplies the exact size before any extra credit is
    // granted, so this allocation cannot grow with a slow client.
    std::shared_ptr<http::response<
        http::vector_body<std::uint8_t>>> fixed_response;
    std::shared_ptr<http::response_serializer<
        http::vector_body<std::uint8_t>>> fixed_serializer;
    std::size_t fixed_body_expected = 0;
    std::size_t fixed_body_received = 0;
    std::vector<std::uint8_t> outgoing;
    std::deque<QueuedResponseBody> body_queue;
    // Total bytes currently queued for the HTTP write, so the early
    // credit window (performance loop v1) can stay bounded: a slow
    // client must not make the host buffer unboundedly.
    size_t body_queue_bytes = 0;
    bool outgoing_credit_returned_early = false;
    bool head_sent = false;
    bool head_only = false;
    bool writing = false;
    bool end_seen = false;
    bool cl_known = false;
    std::size_t cl_remaining = 0;
    // Credit aggregation (diagnostic, zero bytes overhead when disabled).
    std::uint32_t pending_response_credit = 0;
    // M2 E-2 SSE permit (§9.3): true while this response holds one of the
    // worker's streaming slots. Released exactly once on every completion
    // path (response end, cancel, worker exit) before requests_.erase.
    bool holds_streaming_permit = false;
    // Stream idle watchdog (E-2 §9.3): armed after the head reaches the
    // wire, restarted by every body frame, cancelled by the permit release;
    // firing cancels the request and closes the connection. Owned by the io
    // thread, like every other PendingRequest member.
    std::optional<asio::steady_timer> idle_timer;
    // M2 E-3 slow-client write deadline (§9.2): armed when a socket write
    // is submitted, disarmed when it completes; a write that does not
    // complete within write_timeout_ms_ (the client stopped reading)
    // cancels the request and closes the connection. A separate timer from
    // the stream idle watchdog and from the worker-side request timeout
    // (§8.3 — the timers are independent, neither replaces the other).
    std::optional<asio::steady_timer> write_timer;
};

// E-1 admission (§10.3): a request parked in the shard's bounded queue.
// Owns the full request (head + body) so the connection can keep being
// drained while the worker is busy; the io thread pops FIFO entries once
// an inflight slot frees up. deadline is engaged only when
// queue_timeout_ms > 0.
struct QueuedRequest {
    std::shared_ptr<Session> session;
    http::request<http::string_body> request;
    NormalizedPublicRequest normalized;
    std::size_t bytes = 0;  // estimated head+body size for the byte cap
    std::optional<SteadyClock::time_point> deadline;
};

// Where the E-1 gate chain let the request through. kAccepted hands the
// request to the worker; kQueued parks it in the bounded queue (the queue
// owns the request state from then on); kQueueFull maps to 429.
enum class AdmissionResult { kAccepted, kQueued, kQueueFull };

// Session lives in the anonymous namespace; its Impl member is the
// capsid::host::Impl forward-declared in the header, so name lookup finds it
// through the enclosing capsid::host scope.
class Session : public std::enable_shared_from_this<Session> {
    friend class Impl;

public:
    Session(std::shared_ptr<Impl> impl, tcp::socket socket)
        : impl_(std::move(impl)), stream_(std::move(socket)) {
        // TCP_NODELAY is on by default (single-connection latency drops
        // 43ms→1.3ms with no throughput regression; A/B fairness requires
        // it, as the Go baseline's net/http also disables Nagle). Only the
        // exact value "0" disables it. The M2 item 7 warn line is emitted
        // in start() (Impl is incomplete here); the option state is
        // captured instead.
        const char* nodelay = std::getenv("CAPSID_TCP_NODELAY");
        if (nodelay != nullptr && std::strcmp(nodelay, "0") == 0) {
            nodelay_disabled_ = true;
        } else {
            beast::error_code ec;
            stream_.socket().set_option(asio::ip::tcp::no_delay(true), ec);
        }
    }

    void start();
    void start_disconnect_probe();
    void close();
    bool closed() const { return closed_; }

    // Sends a small synchronous response and returns to reading (or closes
    // the connection when keep_alive is false).
    void send_simple(http::status status,
                     std::string_view body,
                     bool keep_alive,
                     unsigned version);

    beast::tcp_stream& stream() { return stream_; }
    std::optional<std::uint64_t> current_request_id() const {
        return current_id_;
    }
    void clear_current_request() { current_id_.reset(); }

private:
    void read_request();
    void handle_request(http::request<http::string_body> request);

    std::shared_ptr<Impl> impl_;
    beast::tcp_stream stream_;
    beast::flat_buffer buffer_;
    bool closed_ = false;
    bool probe_active_ = false;
    bool nodelay_disabled_ = false;
    std::optional<std::uint64_t> current_id_;
};

class Impl : public std::enable_shared_from_this<Impl> {
public:
    explicit Impl(SingleWorkerServerOptions options)
        : max_inflight_(options.max_inflight_per_worker),
          max_queue_requests_(options.queue_requests),
          max_queue_header_bytes_(options.queue_header_bytes),
          queue_timeout_ms_(options.queue_timeout_ms),
          max_streaming_inflight_(options.max_streaming_inflight_per_worker),
          stream_idle_timeout_ms_(options.stream_idle_timeout_ms),
          write_timeout_ms_(options.write_timeout_ms),
          options_(std::move(options)),
          signals_(ioc_),
          executor_(std::make_unique<WorkerExecutor>()) {
        // Events queued by the executor's worker thread wake this io thread
        // through a weak post: the pending handler must never keep the Impl
        // alive on its own — once the facade stopped and released it, a
        // stale post is dropped with the dead weak.
        executor_->set_event_notifier([this] {
            io_post([weak = weak_from_this()] {
                if (const std::shared_ptr<Impl> alive = weak.lock()) {
                    alive->pump_events();
                }
            });
        });
    }

    ~Impl();

    bool start(const std::vector<std::uint8_t>& bundle, std::string* error);
    void request_stop();
    bool wait(std::string* error);
    void begin_drain(std::uint64_t deadline_ms);
    SingleWorkerServer::DrainMetrics drain_metrics() const {
        SingleWorkerServer::DrainMetrics metrics;
        metrics.draining = draining_.load(std::memory_order_relaxed);
        metrics.finished = drain_finished_.load(std::memory_order_relaxed);
        metrics.total_ms = drain_total_ms_.load(std::memory_order_relaxed);
        metrics.forced_cancellations =
            forced_cancellations_.load(std::memory_order_relaxed);
        return metrics;
    }
    std::uint16_t bound_port() const { return bound_port_; }
    // M2 item 7: the process-wide structured log (null in unit fixtures).
    StructuredLog* log() const { return options_.log; }
    bool activate_accept(std::string* error);
    bool worker_available() const {
        return worker_available_.load(std::memory_order_relaxed);
    }

    // Called from Session (io thread).
    void begin_request(std::uint64_t request_id,
                       const std::shared_ptr<Session>& session,
                       const http::request<http::string_body>& request,
                       const NormalizedPublicRequest& normalized);
    // E-1 admission (§10.3). Runs on the io thread like every request
    // state transition. kAccepted requires no further action; kQueued
    // hands the request to the bounded queue; kQueueFull maps to 429.
    AdmissionResult admit_request(
        const std::shared_ptr<Session>& session,
        http::request<http::string_body>& request,
        const NormalizedPublicRequest& normalized);
    // Pops queued requests into free inflight slots (FIFO). Called after
    // every inflight release and after queue-timer drains.
    void pump_queue();
    void arm_queue_timer();
    void on_queue_timer(const beast::error_code ec);
    // Removes a session's queued request (its connection closed while the
    // request was parked; the session never reads the next request).
    void cancel_queued(const std::shared_ptr<Session>& session);
    std::uint64_t allocate_request_id();
    void cancel_request(std::uint64_t request_id);
    // The session is taken by value: fail_request/finalize_request erase the
    // request (and with it the stored shared_ptr) before touching the
    // session, so the parameter must keep its own reference.
    void fail_request(std::uint64_t request_id,
                      std::shared_ptr<Session> session);
    void finalize_request(std::uint64_t request_id,
                          std::shared_ptr<Session> session);
    void drop_session(const std::shared_ptr<Session>& session);
    bool worker_dead() const { return worker_dead_; }

private:
    friend class Session;

    void do_accept();
    void arm_accept_on_io_thread();
    void on_signal(int signal_number);
    // M2 §7.5 bounded drain (see the facade contract). The drain flag and
    // the report counters are atomics so begin_drain (any thread) and the
    // io-thread completion paths never race; the deadline timer lives on
    // the io thread only.
    void start_drain_on_io(std::uint64_t deadline_ms);
    void on_drain_deadline(const beast::error_code ec);
    void maybe_complete_drain();
    void complete_drain();
    static std::uint64_t steady_ms();
    void pump_events();
    void handle_worker_event(WorkerEvent event);
    void advance_request_body(std::uint64_t request_id);
    void write_body_block(std::uint64_t request_id,
                          std::vector<std::uint8_t> bytes,
                          bool credit_returned_early = false);
    void write_fixed_response(std::uint64_t request_id);
    void write_end_block(std::uint64_t request_id);
    void io_post(std::function<void()> function);
    bool bind_listener();
    void close_listener();
    bool write_ready_line();
    bool sanitize_response_headers(
        std::vector<std::pair<std::string, std::string>>* headers);
    void reject_response_head(std::uint64_t request_id,
                              const std::string& reason);
    // M2 E-2 SSE permit (§9.3): acquires one streaming slot for the
    // response (0 = unlimited), arming the idle watchdog; returns false
    // when the worker has no slot left. The caller then cancels the request
    // and answers with a synthesized 503 BEFORE the head reaches the wire.
    bool acquire_streaming_permit(std::uint64_t request_id,
                                  PendingRequest& pending);
    // Returns the permit exactly once (idempotent), cancelling the idle
    // watchdog; called on every requests_.erase completion path.
    void release_streaming_permit(PendingRequest& pending);
    void arm_stream_idle_timer(std::uint64_t request_id,
                               PendingRequest& pending);
    void on_stream_idle_timer(std::uint64_t request_id,
                              const beast::error_code ec);
    // M2 E-3 §9.2: arms the write deadline around one socket write
    // submission; disarm_write_timer cancels it once that write completes.
    void arm_write_timer(std::uint64_t request_id, PendingRequest& pending);
    void disarm_write_timer(PendingRequest& pending);
    void on_write_timer(std::uint64_t request_id,
                        const beast::error_code ec);
    void finish_start() {
        {
            std::lock_guard<std::mutex> lock(lifecycle_mutex_);
            start_finished_ = true;
        }
        lifecycle_cv_.notify_all();
    }

    // Diagnostic IPC metrics (CAPSID_HOST_IPC_METRICS=1 only; zero overhead
    // in headline runs). Counters are reset after each write so the runner
    // can sample per-profile-run deltas. The worker-direction counters
    // (commands/events/grants/queue depths) live on the WorkerExecutor's
    // Metrics; write_metrics_line merges both sides.
    struct Metrics {
        std::atomic<uint64_t> asio_posts = 0;        // io_context::post calls
        std::atomic<uint64_t> response_heads = 0;
        std::atomic<uint64_t> response_body_frames = 0;
        std::atomic<uint64_t> response_ends = 0;
        std::atomic<uint64_t> credit_stall_count = 0;
    };
    Metrics metrics_;
    bool metrics_enabled_ = false;
    void write_metrics_line();

    // Credit aggregation (CAPSID_CREDIT_GRANT_THRESHOLD env var; 0 = off).
    // When non-zero, response credit is accumulated per-request and only
    // submitted in batches of at least this many bytes, reducing WINDOW_UPDATE
    // frames on the command channel. Default 16384: the E11 stream-64k A/B
    // (4 interleaved rounds, 0 errors) measured +3.0% QPS with neutral p99;
    // the value is clamped to window/4 at startup so aggregation can never
    // starve a long-lived stream (see host/credit_limits.h).
    std::uint32_t credit_grant_threshold_ = 16384;
    void flush_pending_credit(std::uint64_t request_id,
                              PendingRequest& pending,
                              bool force);

    // Bodyless request fusion (CAPSID_BODYLESS env var; on by default, only
    // the exact value "0" disables). When on, requests with an empty body
    // are sent as a single RequestHead carrying kFlagRequestEnd so the worker
    // skips request-direction credit and observes EOF immediately. The toggle
    // lets the same binary run the off/on A/B for M1C acceptance evidence.
    bool bodyless_enabled_ = true;

    // E-1 admission (§10.3). All of this state lives on the io thread
    // (written by handle_request/admit_request/pump_queue/queue-timer and
    // read by the same thread), so no cross-thread access is introduced —
    // the E-4 contract that StaticPoolState stays read-only after start()
    // is untouched: the pool-level load selection reads it from reactor
    // threads only in later M2 batches, and this shard admission does not
    // read it at all.
    std::uint64_t max_inflight_ = 128;
    std::uint64_t max_queue_requests_ = 0;
    std::uint64_t max_queue_header_bytes_ = 0;
    std::uint64_t queue_timeout_ms_ = 0;
    std::deque<QueuedRequest> queue_;
    std::size_t queue_bytes_ = 0;
    std::optional<asio::steady_timer> queue_timer_;

    // M2 E-2 SSE permit (§9.3): max_streaming_inflight_ bounds the number of
    // responses that currently hold a streaming slot (0 = unlimited);
    // stream_idle_timeout_ms_ (0 = none) is the silence deadline per held
    // permit. All three live on the io thread like the admission state.
    std::uint64_t max_streaming_inflight_ = 2;
    std::uint64_t stream_idle_timeout_ms_ = 60000;
    std::uint64_t streaming_inflight_ = 0;

    // M2 E-3 slow-client write deadline (§9.2): a socket write outstanding
    // longer than write_timeout_ms_ (0 = disabled) cancels the request and
    // closes the connection. Host-side socket view; the worker-side request
    // timeout stays a separate timer (§8.3).
    std::uint64_t write_timeout_ms_ = 60000;

    SingleWorkerServerOptions options_;
    asio::io_context ioc_;
    std::optional<tcp::acceptor> acceptor_;
    asio::signal_set signals_;
    std::map<std::uint64_t, std::shared_ptr<Session>> sessions_;
    std::map<std::uint64_t, PendingRequest> requests_;
    std::uint64_t next_request_id_ = 1;
    std::uint64_t next_session_id_ = 1;
    bool shutting_down_ = false;
    bool worker_dead_ = false;
    std::string bound_address_;
    std::uint16_t bound_port_ = 0;

    // Controllable lifecycle (M2). start_gate_ rejects a second start
    // (success or failure); stop_requested_ makes request_stop idempotent;
    // the Runtime shutdown frame itself is queued at most once by the
    // WorkerExecutor (request_shutdown), so repeated stops can never
    // trigger a double shutdown; io_running_ tells request_stop whether the
    // event-loop thread exists, so the shutdown path never posts into a
    // dead io_context.
    std::atomic<bool> start_gate_ = false;
    std::atomic<bool> stop_requested_ = false;
    std::atomic<bool> io_running_ = false;
    std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_cv_;
    bool start_finished_ = false;
    std::thread io_thread_;
    // Pool barrier: while defer_accept is pending, the work guard pins the
    // event loop alive (there is no accept work yet); activate_accept()
    // queues the accept handler BEFORE releasing it, so the loop cannot
    // return between the release and the accept arming. The guard is only
    // ever touched on the owner io thread (activation, stop, kExit) so no
    // control thread races the reactor.
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>>
        accept_guard_;
    std::atomic<bool> accept_activated_ = false;
    // Activation acknowledgement: the io thread sets accept_armed_ after
    // do_accept ran (or was rejected); activate_accept() waits for it so
    // the pool publishes READY only after every accept is confirmed.
    std::mutex activation_mutex_;
    std::condition_variable activation_cv_;
    bool accept_armed_ = false;        // handler completed (io thread)
    bool accept_armed_success_ = false;  // accept loop actually armed
    // Thread-safe worker availability for pool capacity reporting.
    std::atomic<bool> worker_available_ = false;
    // Concurrent-wait guard: the first wait() call owns the joins.
    std::once_flag wait_once_;

    // The WorkerExecutor owns everything that talks to the worker process:
    // the capsid_worker handle, the WorkerEventSource, the command queue,
    // the event queue and the worker thread (spec §8.1). Declared after
    // ioc_ so it is destroyed BEFORE the io_context: the executor's
    // notifier posts into ioc_, and its destructor joins the worker thread,
    // so no post can outlive ioc_ as long as the executor is destroyed
    // first.
    std::unique_ptr<WorkerExecutor> executor_;
    // M2 §7.5 bounded drain state and report. The io thread owns the
    // deadline timer and the completion transitions; the atomics let any
    // thread begin a drain and snapshot the report.
    std::atomic<bool> draining_ = false;
    std::atomic<bool> drain_finished_ = false;
    std::atomic<std::uint64_t> drain_started_ms_ = 0;
    std::atomic<std::uint64_t> drain_total_ms_ = 0;
    std::atomic<std::uint64_t> forced_cancellations_ = 0;
    std::optional<asio::steady_timer> drain_timer_;
    std::optional<SteadyClock::time_point> drain_deadline_;
};

void Session::start() {
    // M2 item 7 (§12.2): the delayed TCP_NODELAY warning lands here —
    // Impl is complete at this point.
    if (nodelay_disabled_) {
        emit_log(impl_->log(), LogLane::kControl,
                 {.level = "warn",
                  .event = log_events::kStartup,
                  .message = "TCP_NODELAY disabled"});
    }
    read_request();
}

// While a request is in flight the Host keeps an async_wait on the socket so
// an abortive peer disconnect (RST) cancels the worker request immediately
// (design §9.3) instead of waiting for the worker deadline. The wait consumes
// nothing and the completion inspects the socket with MSG_PEEK, so the probe
// can stay armed across the response without corrupting the next request's
// parse:
//   - recv error: the peer reset the connection — cancel now;
//   - EOF (FIN): the client half-closed its send side (connection: close)
//     but still reads the response — benign, the request continues; a client
//     that fully closed is detected by the next in-flight write, which gets
//     an RST and fails the request;
//   - data: a pipelined second request, which v1 forbids (§8.3) — close.
void Session::start_disconnect_probe() {
    if (probe_active_ || closed_) {
        return;
    }
    probe_active_ = true;
    stream_.socket().async_wait(
        tcp::socket::wait_read,
        [self = shared_from_this()](beast::error_code ec) {
            self->probe_active_ = false;
            if (self->closed_) {
                return;
            }
            // The socket fired while no request was in flight (the previous
            // request already finished and its bytes belong to the next
            // request being parsed): leave the data in place and re-arm from
            // the next handle_request.
            if (!self->current_id_.has_value()) {
                return;
            }
            if (ec) {
                self->close();
                return;
            }
            char scratch = 0;
            const ssize_t peeked = ::recv(
                self->stream_.socket().native_handle(), &scratch, 1,
                MSG_PEEK);
            (void)scratch;
            if (peeked < 0) {
                self->close();
            } else if (peeked > 0) {
                self->close();
            }
        });
}

void Session::read_request() {
    clear_current_request();
    auto parser = std::make_shared<http::request_parser<http::string_body>>();
    parser->body_limit(kMaxRequestBodyBytes);
    http::async_read(
        stream_,
        buffer_,
        *parser,
        [self = shared_from_this(), parser](beast::error_code ec, std::size_t) {
            if (ec) {
                self->close();
                return;
            }
            self->handle_request(parser->release());
        });
}

void Session::handle_request(http::request<http::string_body> request) {
    // E-1 admission gate chain (§10.3), v1 fixed-pool form:
    //   ① listener/header gate — the request head has been parsed; the v1
    //     listener accepts into the shard and admits here.
    //   ④ shard pool capacity — a dead worker means no READY worker on
    //     this shard: 503, before any admission bookkeeping.
    if (impl_->worker_dead()) {
        send_simple(http::status::service_unavailable, "worker unavailable",
                    false, request.version());
        return;
    }

    RequestRoutingPolicy policy;
    policy.mode = RequestRoutingMode::kPath;
    policy.public_scheme = impl_->options_.public_scheme;
    policy.public_authority = impl_->options_.public_authority;
    policy.trusted_header_routing = false;

    std::vector<PublicRequestHeaderView> views;
    views.reserve(16);
    for (const auto& field : request.base()) {
        views.push_back({field.name_string(), field.value()});
    }
    const RequestNormalizationResult normalized = normalize_public_request(
        policy, request.target(), views);
    if (!normalized.ok) {
        const http::status status =
            normalized.error.code == RequestNormalizationErrorCode::kRouteNotFound
                ? http::status::not_found
                : http::status::bad_request;
        send_simple(status, normalized.error.message, request.keep_alive(),
                    request.version());
        return;
    }
    if (normalized.request.application != impl_->options_.application) {
        send_simple(http::status::not_found, "app not found",
                    request.keep_alive(), request.version());
        return;
    }

    // ② Host-global inflight/queue gate is merged with ③ (the App
    // queue gate) in the v1 single-App pool: admission is enforced per
    // shard, and the pool's Host budget is the shard budget times the
    // shard count (recorded in static_pool_server.cc).
    // ③ App queue gate — the bounded queue, or 429 when full.
    // ⑤ worker max-inflight hard boundary — enforced by the worker
    // itself; the shard admission bounds what reaches it.
    switch (impl_->admit_request(shared_from_this(), request,
                                 normalized.request)) {
    case AdmissionResult::kAccepted: {
        const std::uint64_t request_id = impl_->allocate_request_id();
        if (request_id == 0) {
            send_simple(http::status::service_unavailable,
                        "no worker request slots available",
                        request.keep_alive(), request.version());
            return;
        }
        current_id_ = request_id;
        impl_->begin_request(request_id, shared_from_this(), request,
                             normalized.request);
        start_disconnect_probe();
        return;
    }
    case AdmissionResult::kQueued:
        // The bounded queue owns the request until an inflight slot frees
        // (pump_queue) or its deadline expires (504). The session must not
        // read the next request while one is parked: read_request is only
        // resumed by finalize/close on this session.
        return;
    case AdmissionResult::kQueueFull:
        send_simple(http::status::too_many_requests, "app queue full",
                    request.keep_alive(), request.version());
        return;
    }
}

void Session::close() {
    if (closed_) {
        return;
    }
    closed_ = true;
    // A request parked in the admission queue has no request_id yet; it is
    // owned by the queue and must be dropped here (its connection is
    // closing, so the parked request can never be served).
    impl_->cancel_queued(shared_from_this());
    if (current_id_) {
        impl_->cancel_request(*current_id_);
        current_id_.reset();
    }
    beast::error_code ec;
    stream_.socket().shutdown(tcp::socket::shutdown_both, ec);
    stream_.close();
    impl_->drop_session(shared_from_this());
}

void Session::send_simple(http::status status,
                          std::string_view body,
                          bool keep_alive,
                          unsigned version) {
    auto response = std::make_shared<http::response<http::string_body>>();
    response->result(status);
    response->version(version);
    response->keep_alive(keep_alive);
    response->set(http::field::content_type, "text/plain");
    response->body() = std::string(body);
    response->prepare_payload();
    http::async_write(
        stream_,
        *response,
        [self = shared_from_this(), response, keep_alive](
            beast::error_code ec, std::size_t) {
            // response is captured so the write operation never touches a
            // freed message.
            (void)response;
            if (!ec && keep_alive && !self->closed_) {
                self->read_request();
            } else {
                self->close();
            }
        });
}

Impl::~Impl() {
    // A running object is torn down with a bounded stop: request_stop is
    // idempotent, the event-loop thread drains through on_signal, and the
    // executor's bounded terminate backstop forces the blocking destroy to
    // finish promptly. capsid_worker_destroy stays exclusively on the
    // executor's reaper thread (spec §8.1). Never std::terminate, never a
    // leak.
    request_stop();
    if (io_thread_.joinable()) {
        io_thread_.join();
    }
    executor_->stop_and_join();
}

bool Impl::start(const std::vector<std::uint8_t>& bundle,
                 std::string* error) {
    if (start_gate_.exchange(true)) {
        if (error != nullptr) {
            *error = "server already started";
        }
        return false;
    }
    struct StartCompletion {
        Impl* impl;
        ~StartCompletion() { impl->finish_start(); }
    } completion{this};
    if (stop_requested_.load(std::memory_order_acquire)) {
        if (error != nullptr) {
            *error = "server stop was requested before start";
        }
        return false;
    }
    if (max_inflight_ > std::numeric_limits<std::uint32_t>::max()) {
        if (error != nullptr) {
            *error = "max_inflight_per_worker exceeds the worker limit";
        }
        return false;
    }
    // M2 E-2 §9.3: the streaming permit must stay below the inflight
    // ceiling — except the single documented 1/1 boundary, where both are
    // 1 and the worker explicitly forgoes concurrency for streaming. Both
    // 0 values mean unlimited and are exempt.
    if (max_streaming_inflight_ != 0 && max_inflight_ != 0 &&
        max_streaming_inflight_ >= max_inflight_ &&
        !(max_inflight_ == 1 && max_streaming_inflight_ == 1)) {
        if (error != nullptr) {
            *error = "max_streaming_inflight_per_worker must be below "
                     "max_inflight_per_worker (except the 1/1 boundary)";
        }
        return false;
    }
    // ---- 1. spawn / load / flush (the same two-phase prepare as the
    // legacy run path), now inside the executor's factory. The factory
    // recycles what it created on failure; from the executor's start()
    // onward, capsid_worker_destroy runs exclusively on the executor's
    // reaper thread.
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = options_.worker_path.c_str();
    config.request_timeout_ms = options_.request_timeout_ms;
    config.initial_stream_window = options_.initial_stream_window;
    config.strict_sandbox = options_.strict_sandbox ? 1U : 0U;
    // E-1 §10.3 gate ⑤: the worker's own max-inflight hard boundary.
    // 0 = unlimited (worker default); otherwise the shard and the worker
    // share the same ceiling.
    config.max_inflight_requests =
        static_cast<std::uint32_t>(max_inflight_);
    // egress_policy and capability_policy stay NULL: every outbound Fetch
    // is denied by the Runtime default.

    // Diagnostic IPC metrics: CAPSID_HOST_IPC_METRICS=1 enables counters
    // with zero overhead in headline runs (branch is off by default).
    const char* metrics_env = std::getenv("CAPSID_HOST_IPC_METRICS");
    if (metrics_env != nullptr && std::strcmp(metrics_env, "1") == 0) {
        metrics_enabled_ = true;
    }
    executor_->set_metrics_enabled(metrics_enabled_);

    // Credit aggregation threshold: grant only when pending ≥ threshold.
    const char* credit_env = std::getenv("CAPSID_CREDIT_GRANT_THRESHOLD");
    if (credit_env != nullptr) {
        std::uint64_t val = 0;
        for (const char* p = credit_env; *p != '\0'; ++p) {
            if (*p >= '0' && *p <= '9') {
                const std::uint64_t digit =
                    static_cast<std::uint64_t>(*p - '0');
                if (val >
                    (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
                    val = 0;
                    break;
                }
                val = val * 10 + digit;
            } else { val = 0; break; }
        }
        if (val > 0 && val <= std::numeric_limits<std::uint32_t>::max()) {
            credit_grant_threshold_ = static_cast<std::uint32_t>(val);
        }
    }
    // Clamp the (possibly defaulted) threshold to the response window so
    // aggregation can never exceed what a stream could actually regain.
    credit_grant_threshold_ = clamp_credit_grant_threshold(
        credit_grant_threshold_, config.initial_stream_window);
    // Bodyless fusion toggle: only the exact value "0" disables it (same
    // convention as CAPSID_TCP_NODELAY).
    const char* bodyless_env = std::getenv("CAPSID_BODYLESS");
    if (bodyless_env != nullptr && std::strcmp(bodyless_env, "0") == 0) {
        bodyless_enabled_ = false;
        emit_log(log(), LogLane::kControl,
                 {.level = "warn",
                  .event = log_events::kStartup,
                  .message = "bodyless request fusion disabled"});
    }

    WorkerExecutor::WorkerFactory factory =
        [this, &config, &bundle](capsid_worker** out,
                                 std::string* factory_error) -> bool {
        capsid_worker* worker = nullptr;
        const capsid_result spawn_result =
            capsid_worker_spawn(&config, &worker);
        if (spawn_result != CAPSID_OK) {
            if (factory_error != nullptr) {
                *factory_error = "worker spawn failed: " +
                                 std::string(capsid_result_string(spawn_result));
            }
            return false;
        }
        const capsid_result load_result = capsid_worker_load_bundle_named(
            worker, bundle.data(), bundle.size(),
            options_.source_name.c_str());
        if (load_result != CAPSID_OK) {
            if (factory_error != nullptr) {
                *factory_error = "bundle load failed: " +
                                 std::string(capsid_result_string(load_result));
            }
            capsid_worker_destroy(worker);
            return false;
        }
        // load_bundle_named only queues the frame; the worker must receive
        // it before it loads the application and emits READY.
        const capsid_result flush_result = capsid_worker_flush(worker);
        if (flush_result != CAPSID_OK) {
            if (factory_error != nullptr) {
                *factory_error = "bundle flush failed: " +
                                 std::string(capsid_result_string(flush_result));
            }
            capsid_worker_destroy(worker);
            return false;
        }
        // Enable the client-side IPC metrics when the host-side ones are on,
        // so both sides produce matching counter sets (the client metrics
        // are already instrumented in client.cc behind the same env var).
        if (metrics_enabled_) {
            client_ipc_metrics_enable(worker, true);
        }
        *out = worker;
        return true;
    };

    // The executor's worker thread takes exclusive ownership of every
    // further Runtime API call (READY arrives through next_event). On
    // failure start() leaves the executor stopped: no thread, no worker.
    if (!executor_->start(factory, error)) {
        return false;
    }
    worker_available_ = true;
    if (!bind_listener()) {
        // bind_listener may have partially succeeded (open, set_option,
        // bind, listen or local_endpoint): the acceptor is closed and
        // reset here so a failed start never leaves the port occupied.
        worker_available_ = false;
        close_listener();
        executor_->stop_and_join();
        if (error != nullptr) {
            *error = "listener bind failed";
        }
        return false;
    }
    if (!write_ready_line()) {
        // The listener was fully bound when the READY record failed: the
        // port must be released BEFORE start() returns, so a caller that
        // keeps the object alive (StaticPoolServer retry) can rebind.
        worker_available_ = false;
        close_listener();
        executor_->stop_and_join();
        if (error != nullptr) {
            *error = "failed to write the READY record";
        }
        return false;
    }

    // ---- 2. signals + accept loop, then the event-loop thread. The
    // accept is armed BEFORE the thread starts so start() returns with a
    // live, immediately serviceable listener. Pool shards skip the
    // process-level signal wiring: the pool owns signal delivery, and a
    // shard must never race the pool's stop path.
    if (options_.install_process_signals) {
        signals_.add(SIGTERM);
        signals_.add(SIGINT);
        // Weak capture: the pending signal wait must never keep the Impl
        // alive on its own; the facade owns the lifetime while running.
        signals_.async_wait(
            [weak = weak_from_this()](beast::error_code, int signal_number) {
                if (const std::shared_ptr<Impl> alive = weak.lock()) {
                    alive->on_signal(signal_number);
                }
            });
    }

    if (options_.defer_accept) {
        // Pool barrier: the shard is fully prepared (worker READY,
        // listener bound) but must not accept a single connection until
        // the pool activates it. The work guard keeps the event loop
        // alive through the activation wait.
        accept_guard_.emplace(asio::make_work_guard(ioc_));
    } else {
        do_accept();
    }
    io_running_ = true;
    io_thread_ = std::thread([this] {
        ioc_.run();
        io_running_ = false;
    });
    if (stop_requested_.load(std::memory_order_acquire)) {
        io_post([weak = weak_from_this()] {
            if (const std::shared_ptr<Impl> alive = weak.lock()) {
                alive->on_signal(0);
            }
        });
        executor_->stop_and_join();
        if (io_thread_.joinable()) {
            io_thread_.join();
        }
        if (error != nullptr) {
            *error = "server stop was requested during start";
        }
        return false;
    }
    return true;
}

bool Impl::activate_accept(std::string* error) {
    if (accept_activated_.exchange(true)) {
        if (error != nullptr) {
            *error = "accept already activated";
        }
        return false;
    }
    // The control thread must never read the guard (the io thread owns
    // it); the OPTION decides whether activation is pending.
    if (!options_.defer_accept) {
        if (error != nullptr) {
            *error = "accept activation is not pending";
        }
        return false;
    }
    // A dead worker or an already-stopping server rejects the activation
    // synchronously: no post, no wait, no hang.
    if (!worker_available_.load(std::memory_order_relaxed) ||
        stop_requested_.load(std::memory_order_relaxed)) {
        if (error != nullptr) {
            *error = "accept activation rejected";
        }
        return false;
    }
    // The actual arming (guard release + do_accept) runs on the owner io
    // thread; this call blocks until that thread reports the outcome. The
    // confirmation distinguishes COMPLETED (the handler ran, possibly
    // finding the shard dead) from SUCCEEDED (the accept loop is actually
    // armed): a shard whose worker died or whose stop began in the window
    // must confirm FAILURE, never a fake arm — so the pool only publishes
    // READY when every shard truly accepts.
    io_post([weak = weak_from_this()] {
        if (const std::shared_ptr<Impl> alive = weak.lock()) {
            alive->arm_accept_on_io_thread();
        }
    });
    std::unique_lock<std::mutex> lock(activation_mutex_);
    const bool completed = activation_cv_.wait_for(
        lock, std::chrono::seconds(2),
        [this] { return accept_armed_; });
    if (!completed || !accept_armed_success_) {
        if (error != nullptr) {
            *error = "accept activation failed";
        }
        return false;
    }
    return true;
}

void Impl::arm_accept_on_io_thread() {
    // Owner-io-thread arm: release the barrier guard and start accepting.
    // do_accept() itself re-checks the shutdown state and the acceptor, so
    // a worker that died between the gate check and this handler does not
    // arm — and the outcome is reported as failed, never as a fake arm.
    bool succeeded = false;
    if (accept_guard_) {
        accept_guard_->reset();
    }
    if (!shutting_down_ && acceptor_ && acceptor_->is_open()) {
        do_accept();
        succeeded = true;
    }
    {
        std::lock_guard<std::mutex> lock(activation_mutex_);
        accept_armed_ = true;
        accept_armed_success_ = succeeded;
    }
    activation_cv_.notify_all();
}

void Impl::request_stop() {
    if (stop_requested_.exchange(true)) {
        return;  // idempotent: nothing is touched again
    }
    // The Runtime shutdown frame is queued at most once regardless of how
    // many stop sources fire (request_stop, SIGTERM, destructor): the
    // executor's request_shutdown owns the exactly-once gate.
    executor_->request_shutdown();
    // When the event-loop thread exists, the acceptor/session teardown
    // runs on it (the only thread allowed to touch Beast objects). The
    // weak capture is safe even from the destructor: if the loop already
    // ended, the posted handler is dropped with the dead context.
    if (io_running_.load()) {
        io_post([self = weak_from_this()] {
            if (const std::shared_ptr<Impl> alive = self.lock()) {
                alive->on_signal(0);
            }
        });
    }
}

bool Impl::wait(std::string* error) {
    // The event-loop thread exits when the acceptor and every session are
    // gone; the worker thread exits when its bounded shutdown completes.
    // Both joins make wait() block until the server is fully stopped —
    // threads are never detached. Concurrent-wait guard: two threads
    // calling wait() at once would double-join (UB); call_once makes the
    // first caller own the joins while concurrent/later callers BLOCK
    // until they complete, so no caller returns before the server is
    // actually stopped.
    {
        std::unique_lock<std::mutex> lock(lifecycle_mutex_);
        if (!start_gate_.load(std::memory_order_acquire)) {
            if (error != nullptr) {
                *error = "server was not started";
            }
            return false;
        }
        lifecycle_cv_.wait(lock, [this] { return start_finished_; });
    }
    std::call_once(wait_once_, [this] {
        if (io_thread_.joinable()) {
            io_thread_.join();
        }
        executor_->stop_and_join();
    });
    if (error != nullptr) {
        *error = "server stopped";
    }
    return true;
}


void Impl::do_accept() {
    if (shutting_down_ || !acceptor_->is_open()) {
        return;
    }
    // Weak capture: an idle accept must not keep the Impl alive after the
    // facade released it. While the server is running the facade holds
    // the only strong reference; a dead weak simply stops accepting.
    acceptor_->async_accept(
        ioc_,
        [weak = weak_from_this()](beast::error_code ec, tcp::socket socket) {
            const std::shared_ptr<Impl> self = weak.lock();
            if (!self) {
                return;
            }
            if (self->shutting_down_) {
                return;
            }
            if (ec) {
                self->do_accept();
                return;
            }
            auto session =
                std::make_shared<Session>(self, std::move(socket));
            self->sessions_[self->next_session_id_++] = session;
            session->start();
            self->do_accept();
        });
}

void Impl::on_signal(int) {
    if (shutting_down_) {
        return;
    }
    shutting_down_ = true;
    worker_available_ = false;
    if (acceptor_) {
        acceptor_->close();
    }
    // Cancel the pending signal wait so the event loop drains instead of
    // staying alive behind it; the wait handler completes with
    // operation_aborted on the io thread and releases itself.
    beast::error_code ignored;
    signals_.cancel(ignored);
    // Close every client session; each close cancels its in-flight request.
    const std::vector<std::uint64_t> session_ids = [this] {
        std::vector<std::uint64_t> ids;
        ids.reserve(sessions_.size());
        for (const auto& entry : sessions_) {
            ids.push_back(entry.first);
        }
        return ids;
    }();
    for (const std::uint64_t id : session_ids) {
        auto it = sessions_.find(id);
        if (it != sessions_.end()) {
            it->second->close();
        }
    }
    // The shutdown frame is queued at most once even when several stop
    // sources fire (SIGTERM, request_stop, destructor): a duplicate would
    // trigger a Runtime double shutdown; the executor owns the gate.
    executor_->request_shutdown();
    // No ioc_.stop() here: every cancelled handler (signal wait, accept,
    // sessions) completes on the io thread and releases its captures, so
    // the loop drains naturally and run() returns with an empty queue. A
    // hard stop would strand self-retaining handlers forever. A pending
    // pool barrier (defer_accept) must be released here too, or the work
    // guard would pin the loop forever.
    if (accept_guard_) {
        accept_guard_->reset();
    }
}

void Impl::begin_drain(std::uint64_t deadline_ms) {
    if (!draining_.exchange(true)) {
        drain_started_ms_.store(steady_ms(), std::memory_order_relaxed);
    }
    if (io_running_.load()) {
        io_post([self = weak_from_this(), deadline_ms] {
            if (const std::shared_ptr<Impl> alive = self.lock()) {
                alive->start_drain_on_io(deadline_ms);
            }
        });
    }
}

void Impl::start_drain_on_io(std::uint64_t deadline_ms) {
    if (shutting_down_) {
        return;
    }
    // §7.5 row 1: the listener stops accepting new connections — the
    // routing/registry has already stopped delivering to this pool.
    // Existing sessions are untouched (row 2 keeps them serving).
    if (acceptor_) {
        acceptor_->close();
    }
    if (accept_guard_) {
        accept_guard_->reset();
    }
    if (deadline_ms != 0) {
        drain_deadline_ = SteadyClock::now() +
                          std::chrono::milliseconds(deadline_ms);
        if (!drain_timer_) {
            drain_timer_.emplace(ioc_);
        }
        drain_timer_->expires_at(*drain_deadline_);
        drain_timer_->async_wait(
            [weak = weak_from_this()](const beast::error_code ec) {
                if (const std::shared_ptr<Impl> self = weak.lock()) {
                    self->on_drain_deadline(ec);
                }
            });
    }
    // An already-idle pool completes the drain immediately.
    maybe_complete_drain();
}

void Impl::on_drain_deadline(const beast::error_code ec) {
    if (ec == asio::error::operation_aborted || shutting_down_) {
        return;
    }
    // §7.5 row 4: the deadline expired — cancel every remaining request.
    // Each in-flight session counts as one forced cancellation; parked
    // requests are counted per entry and dropped with their sessions.
    for (const auto& entry : sessions_) {
        if (entry.second->current_id_.has_value()) {
            forced_cancellations_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    forced_cancellations_.fetch_add(queue_.size(), std::memory_order_relaxed);
    const std::vector<std::uint64_t> session_ids = [this] {
        std::vector<std::uint64_t> ids;
        ids.reserve(sessions_.size());
        for (const auto& entry : sessions_) {
            ids.push_back(entry.first);
        }
        return ids;
    }();
    for (const std::uint64_t id : session_ids) {
        auto it = sessions_.find(id);
        if (it != sessions_.end()) {
            it->second->close();
        }
    }
    // Row 5: the brief grace runs inside the kShutdown execution, which
    // arms the worker-thread terminate backstop (kShutdownGrace).
    complete_drain();
}

void Impl::maybe_complete_drain() {
    if (!draining_.load() || shutting_down_) {
        return;
    }
    if (!requests_.empty() || !queue_.empty()) {
        return;  // §7.5 row 2: keep serving the in-flight requests
    }
    complete_drain();
}

void Impl::complete_drain() {
    if (shutting_down_) {
        return;
    }
    shutting_down_ = true;
    worker_available_ = false;
    drain_finished_.store(true, std::memory_order_relaxed);
    const std::uint64_t started =
        drain_started_ms_.load(std::memory_order_relaxed);
    if (started != 0) {
        drain_total_ms_.store(steady_ms() - started,
                              std::memory_order_relaxed);
    }
    if (accept_guard_) {
        accept_guard_->reset();
    }
    // The deadline timer must not pin the io loop after the drain finished:
    // destroy it here (safe from within its own handler) or run() would
    // keep waiting for a deadline that no longer matters.
    drain_timer_.reset();
    // Idle keep-alive connections also close: the drained pool serves no
    // further requests, and an idle session parked in read_request would
    // otherwise pin the io loop forever. Sessions still holding a request
    // (only possible on the forced path) are cancelled by Session::close.
    const std::vector<std::uint64_t> session_ids = [this] {
        std::vector<std::uint64_t> ids;
        ids.reserve(sessions_.size());
        for (const auto& entry : sessions_) {
            ids.push_back(entry.first);
        }
        return ids;
    }();
    for (const std::uint64_t id : session_ids) {
        auto it = sessions_.find(id);
        if (it != sessions_.end()) {
            it->second->close();
        }
    }
    // §7.5 row 3: inflight cleared → shutdown; the worker flushes and
    // reads to EXIT (never a forced terminate on this path). The
    // executor's request_shutdown owns the exactly-once gate.
    executor_->request_shutdown();
}

std::uint64_t Impl::steady_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            SteadyClock::now().time_since_epoch())
            .count());
}

void Impl::pump_events() {
    std::deque<WorkerEvent> local = executor_->drain_events();
    for (WorkerEvent& event : local) {
        handle_worker_event(std::move(event));
    }
    // Diagnostic: emit one metrics line per pump so the runner sees a
    // continuous counter series and can extract per-profile-run deltas.
    write_metrics_line();
}

void Impl::handle_worker_event(WorkerEvent event) {
    switch (event.type) {
    case WorkerEvent::Type::kRequestCredit: {
        auto it = requests_.find(event.request_id);
        if (it == requests_.end()) {
            return;
        }
        it->second.request_credit += event.credit;
        advance_request_body(event.request_id);
        return;
    }
    case WorkerEvent::Type::kResponseHead: {
        if (metrics_enabled_) {
            metrics_.response_heads.fetch_add(1, std::memory_order_relaxed);
        }
        auto it = requests_.find(event.request_id);
        if (it == requests_.end()) {
            return;
        }
        PendingRequest& pending = it->second;
        // Response semantics gate (design §8.3): the Runtime decoder only
        // validates the FetchRPC binary structure, so the Host filters
        // hop-by-hop fields, validates names/values, and rejects duplicate
        // or invalid Content-Length itself. Any violation fails the response
        // closed before a single response byte has left the server.
        if (event.status < 200 || event.status > 599) {
            reject_response_head(event.request_id,
                                 "invalid worker response status");
            return;
        }
        if (!sanitize_response_headers(&event.headers)) {
            reject_response_head(event.request_id,
                                 "invalid worker response headers");
            return;
        }
        // M2 E-2 §9.3: the streaming permit is decided by the Content-Type
        // alone — a text/event-stream response must hold a slot before its
        // head reaches the client; a plain chunked response (no SSE type)
        // stays on the ordinary inflight/credit path even without a
        // Content-Length. A permit-exhausted stream is cancelled here and
        // answered with a synthesized 503: never a 200 that is torn down.
        bool is_sse = false;
        for (const auto& [name, value] : event.headers) {
            if (name.size() != 12 ||
                !std::equal(name.begin(), name.end(), "content-type",
                            [](unsigned char a, unsigned char b) {
                                return std::tolower(a) == std::tolower(b);
                            })) {
                continue;
            }
            if (is_event_stream_content_type(value)) {
                is_sse = true;
            }
            break;
        }
        if (is_sse &&
            !acquire_streaming_permit(event.request_id, pending)) {
            reject_response_head(event.request_id,
                                 "streaming capacity exhausted");
            return;
        }
        pending.head_only = pending.method == "HEAD";
        if (event.fixed_body) {
            pending.fixed_response = std::make_shared<http::response<
                http::vector_body<std::uint8_t>>>();
            pending.fixed_response->result(
                static_cast<http::status>(event.status));
            pending.fixed_response->version(pending.version);
            pending.fixed_response->keep_alive(pending.keep_alive);
            for (const auto& [name, value] : event.headers) {
                pending.fixed_response->base().insert(name, value);
            }
            const auto content_length_field =
                pending.fixed_response->base().find(
                    http::field::content_length);
            if (content_length_field !=
                pending.fixed_response->base().end()) {
                std::uint64_t value = 0;
                bool valid = true;
                const std::string_view text(
                    content_length_field->value());
                if (text.empty()) {
                    valid = false;
                }
                for (const char c : text) {
                    if (c < '0' || c > '9') {
                        valid = false;
                        break;
                    }
                    const std::uint64_t digit =
                        static_cast<std::uint64_t>(c - '0');
                    if (value >
                        (std::numeric_limits<std::uint64_t>::max() -
                         digit) / 10) {
                        valid = false;
                        break;
                    }
                    value = value * 10 + digit;
                }
                if (!valid || value != event.fixed_body_size) {
                    reject_response_head(
                        event.request_id,
                        "fixed response Content-Length mismatch");
                    return;
                }
            }
            pending.fixed_body_expected = event.fixed_body_size;
            pending.fixed_body_received = 0;
            if (!pending.head_only) {
                pending.fixed_response->body().reserve(
                    pending.fixed_body_expected);
            }
            return;
        }
        pending.response =
            std::make_shared<http::response<http::buffer_body>>();
        pending.response->result(static_cast<http::status>(event.status));
        pending.response->version(pending.version);
        pending.response->keep_alive(pending.keep_alive);
        for (const auto& [name, value] : event.headers) {
            pending.response->base().insert(name, value);
        }
        const auto content_length_field =
            pending.response->base().find(http::field::content_length);
        if (content_length_field != pending.response->base().end()) {
            std::uint64_t value = 0;
            bool valid = true;
            const std::string_view text(content_length_field->value());
            if (text.empty()) {
                valid = false;
            }
            for (const char c : text) {
                if (c < '0' || c > '9') {
                    valid = false;
                    break;
                }
                const std::uint64_t digit =
                    static_cast<std::uint64_t>(c - '0');
                if (value >
                    (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
                    valid = false;
                    break;
                }
                value = value * 10 + digit;
            }
            if (!valid ||
                value > static_cast<std::uint64_t>(
                             std::numeric_limits<std::size_t>::max())) {
                reject_response_head(event.request_id,
                                     "invalid worker Content-Length");
                return;
            }
            pending.cl_known = true;
            pending.cl_remaining = static_cast<std::size_t>(value);
        }
        if (!pending.response->has_content_length() &&
            !pending.response->chunked()) {
            pending.response->chunked(true);
        }
        pending.serializer = std::make_shared<
            http::response_serializer<http::buffer_body>>(
            *pending.response);
        pending.writing = true;
        // E-3 §9.2: the head write has a deadline; a client that stops
        // reading while the head is in flight is torn down.
        arm_write_timer(event.request_id, pending);
        http::async_write_header(
            pending.session->stream(),
            *pending.serializer,
            [self = shared_from_this(),
             response = pending.response,
             serializer = pending.serializer,
             session = pending.session,
             request_id = event.request_id](beast::error_code ec,
                                            std::size_t) {
                (void)response;
                (void)serializer;
                (void)session;
                auto it = self->requests_.find(request_id);
                if (it == self->requests_.end()) {
                    return;
                }
                PendingRequest& pending = it->second;
                pending.writing = false;
                // E-3 §9.2: the head reached the client (or failed); the
                // deadline for this write is spent.
                self->disarm_write_timer(pending);
                if (ec) {
                    self->fail_request(request_id, pending.session);
                    return;
                }
                pending.head_sent = true;
                if (pending.holds_streaming_permit) {
                    // The stream is now on the wire: its silence deadline
                    // starts here and is restarted by every body frame.
                    self->arm_stream_idle_timer(request_id, pending);
                }
                if (!pending.body_queue.empty()) {
                    // Body events that arrived while the head was being
                    // written are queued here and flushed in order.
                    QueuedResponseBody queued =
                        take_coalesced_response_body(
                            &pending.body_queue,
                            &pending.body_queue_bytes,
                            kResponseBodyWriteBatchLimit);
                    self->write_body_block(
                        request_id, std::move(queued.bytes),
                        queued.credit_returned_early);
                } else if (pending.end_seen) {
                    if (pending.head_only) {
                        self->finalize_request(request_id, pending.session);
                    } else {
                        self->write_end_block(request_id);
                    }
                }
            });
        return;
    }
    case WorkerEvent::Type::kResponseBody: {
        if (metrics_enabled_) {
            metrics_.response_body_frames.fetch_add(1, std::memory_order_relaxed);
        }
        auto it = requests_.find(event.request_id);
        if (it == requests_.end()) {
            return;
        }
        PendingRequest& pending = it->second;
        if (pending.holds_streaming_permit) {
            // Heartbeat (§9.3): any body frame keeps the stream alive.
            arm_stream_idle_timer(event.request_id, pending);
        }
        if (pending.fixed_response) {
            if (pending.fixed_body_received >
                    pending.fixed_body_expected ||
                event.body.size() >
                pending.fixed_body_expected -
                    pending.fixed_body_received) {
                fail_request(event.request_id, pending.session);
                return;
            }
            pending.fixed_body_received += event.body.size();
            if (!pending.head_only) {
                pending.fixed_response->body().insert(
                    pending.fixed_response->body().end(),
                    event.body.begin(), event.body.end());
            }
            return;
        }
        // Performance loop v1: return credit as soon as the frame is
        // received, not after the client write completes, while the
        // per-request write queue stays shallow. This removes one host
        // round-trip per frame (write-completion -> credit -> worker
        // wakeup). Beyond the window the credit falls back to
        // write-completion, so a slow client cannot make the host
        // buffer unboundedly.
        const bool credit_returned_early =
            pending.body_queue_bytes < kEarlyCreditWindow;
        if (credit_returned_early) {
            pending.pending_response_credit +=
                static_cast<std::uint32_t>(event.body.size());
            flush_pending_credit(event.request_id, pending, false);
        }
        if (pending.head_only) {
            // HEAD consumes the worker body without exposing it.
            return;
        }
        if (pending.writing) {
            pending.body_queue_bytes += event.body.size();
            pending.body_queue.push_back(
                QueuedResponseBody{std::move(event.body),
                                   credit_returned_early});
            return;
        }
        write_body_block(event.request_id, std::move(event.body),
                         credit_returned_early);
        return;
    }
    case WorkerEvent::Type::kResponseEnd: {
        if (metrics_enabled_) {
            metrics_.response_ends.fetch_add(1, std::memory_order_relaxed);
        }
        auto it = requests_.find(event.request_id);
        if (it == requests_.end()) {
            return;
        }
        PendingRequest& pending = it->second;
        pending.end_seen = true;
        // Drain any remaining pending credit; the Runtime erased the request,
        // so terminal retirement drops the stale grant.
        flush_pending_credit(event.request_id, pending, true);
        // The Runtime erased this request when it sent RESPONSE_END, so any
        // request-direction frame still queued for it (body writes, the
        // request-end) would be rejected as an invalid IPC frame and kill
        // the worker. Acknowledge the worker-thread tombstone without a
        // redundant Runtime cancel. Retirement purges shared queued frames
        // and keeps the tombstone until an already-swapped batch has drained.
        executor_->retire_terminal_request(event.request_id);
        if (pending.fixed_response) {
            if (pending.fixed_body_received !=
                pending.fixed_body_expected) {
                fail_request(event.request_id, pending.session);
                return;
            }
            write_fixed_response(event.request_id);
            return;
        }
        if (pending.head_only) {
            if (!pending.writing) {
                finalize_request(event.request_id, pending.session);
            }
            return;
        }
        if (!pending.writing) {
            write_end_block(event.request_id);
        }
        return;
    }
    case WorkerEvent::Type::kLog:
        // M2 item 7 (§12.2): worker LOG frames are application log
        // forwarding — bounded app lane, droppable and counted; the text
        // is the application's own emitted log line (the worker already
        // sanitizes secrets out of LOG frames).
        emit_log(log(), LogLane::kApp,
                 {.event = log_events::kAppLog,
                  .message = event.text});
        return;
    case WorkerEvent::Type::kError:
        // A worker error is a process-lifecycle control-plane event.
        emit_log(log(), LogLane::kControl,
                 {.event = log_events::kWorkerCrash,
                  .result = "error",
                  .message = event.text});
        return;
    case WorkerEvent::Type::kRequestTimeout: {
        auto it = requests_.find(event.request_id);
        if (it == requests_.end()) {
            return;
        }
        PendingRequest& pending = it->second;
        const std::shared_ptr<Session> session = pending.session;
        const bool head_sent = pending.head_sent;
        const bool writing = pending.writing;
        const bool keep_alive = pending.keep_alive;
        const unsigned version = pending.version;
        cancel_request(event.request_id);
        // If the response head has already reached the wire (sent, or the
        // write is still in flight), a fresh response would be parsed as
        // body bytes and corrupt the stream; the connection is closed
        // instead. A clean 504 is only possible before any response byte
        // left the server. head_sent/writing are snapshotted before the
        // erase: cancel_request removes the request state.
        if (head_sent || writing) {
            if (session && !session->closed()) {
                session->close();
            }
        } else if (session && !session->closed()) {
            session->send_simple(http::status::gateway_timeout,
                                 "worker request timeout", keep_alive, version);
        }
        return;
    }
    case WorkerEvent::Type::kRequestFailure: {
        auto it = requests_.find(event.request_id);
        if (it == requests_.end()) {
            return;
        }
        PendingRequest& pending = it->second;
        const std::shared_ptr<Session> session = pending.session;
        const bool head_sent = pending.head_sent;
        const bool writing = pending.writing;
        const bool keep_alive = pending.keep_alive;
        const unsigned version = pending.version;
        cancel_request(event.request_id);
        // Same rule as the timeout path: once any response byte has reached
        // the wire, only a connection close is legal.
        if (head_sent || writing) {
            if (session && !session->closed()) {
                session->close();
            }
        } else if (session && !session->closed()) {
            session->send_simple(http::status::service_unavailable,
                                 "worker request failed", keep_alive, version);
        }
        return;
    }
    case WorkerEvent::Type::kExit: {
        worker_dead_ = true;
        worker_available_ = false;
        // E-1 admission: every parked request dies with the worker — no
        // READY worker remains to serve it (§10.3 → 503). The queue and
        // its timer are drained before the in-flight requests below.
        while (!queue_.empty()) {
            QueuedRequest abandoned = std::move(queue_.front());
            queue_.pop_front();
            queue_bytes_ -= abandoned.bytes;
            if (abandoned.session && !abandoned.session->closed()) {
                abandoned.session->send_simple(
                    http::status::service_unavailable, "worker exited",
                    abandoned.request.keep_alive(),
                    abandoned.request.version());
            }
        }
        queue_timer_.reset();
        // Fault isolation: the shard's worker process is gone, so this
        // shard must stop accepting NEW connections immediately — with
        // SO_REUSEPORT the kernel routes subsequent connections to the
        // healthy shards only once this listener is closed. In-flight
        // requests still receive their 503/close handling below. A
        // pending pool barrier is released as well (the loop must be able
        // to drain even if the worker died before activation).
        if (acceptor_) {
            acceptor_->close();
        }
        if (accept_guard_) {
            accept_guard_->reset();
        }
        const std::vector<std::uint64_t> request_ids = [this] {
            std::vector<std::uint64_t> ids;
            ids.reserve(requests_.size());
            for (const auto& entry : requests_) {
                ids.push_back(entry.first);
            }
            return ids;
        }();
        for (const std::uint64_t request_id : request_ids) {
            auto it = requests_.find(request_id);
            if (it == requests_.end()) {
                continue;
            }
            PendingRequest& pending = it->second;
            const std::shared_ptr<Session> session = pending.session;
            const bool head_sent = pending.head_sent;
            const bool writing = pending.writing;
            const bool keep_alive = pending.keep_alive;
            const unsigned version = pending.version;
            cancel_request(request_id);
            // Same reasoning as the timeout path: once any response byte
            // has reached the wire, only a connection close is legal.
            if (head_sent || writing) {
                if (session && !session->closed()) {
                    session->close();
                }
            } else if (session && !session->closed()) {
                session->send_simple(http::status::service_unavailable,
                                     "worker exited", keep_alive, version);
            }
        }
        return;
    }
    }
}

void Impl::advance_request_body(std::uint64_t request_id) {
    auto it = requests_.find(request_id);
    if (it == requests_.end()) {
        return;
    }
    PendingRequest& pending = it->second;
    // One write per credit event: the worker replenishes request credit only
    // after consuming the previous chunk, so submitting several chunks at
    // once would race the worker's window and be rejected as an invalid
    // frame.
    if (pending.request_credit > 0 &&
        pending.request_body_offset < pending.request_body.size()) {
        const std::size_t remaining =
            pending.request_body.size() - pending.request_body_offset;
        const std::size_t chunk = static_cast<std::size_t>(
            std::min<std::uint64_t>(pending.request_credit, remaining));
        Command command;
        command.type = CommandType::kWriteRequest;
        command.request_id = request_id;
        command.body.assign(
            pending.request_body.data() + pending.request_body_offset,
            pending.request_body.data() + pending.request_body_offset + chunk);
        executor_->submit(std::move(command));
        pending.request_credit -= chunk;
        pending.request_body_offset += chunk;
    }
    if (pending.request_body_offset == pending.request_body.size() &&
        !pending.request_ended) {
        pending.request_ended = true;
        // If the response already ended, the Runtime erased this request and
        // a request-end frame would be rejected as an invalid IPC frame.
        // (The response-end tombstone would drop it anyway; skip submitting
        // it in the first place.)
        if (!pending.end_seen) {
            Command command;
            command.type = CommandType::kEndRequest;
            command.request_id = request_id;
            executor_->submit(std::move(command));
        }
    }
}

void Impl::write_body_block(std::uint64_t request_id,
                            std::vector<std::uint8_t> bytes,
                            bool credit_returned_early) {
    auto it = requests_.find(request_id);
    if (it == requests_.end()) {
        return;
    }
    PendingRequest& pending = it->second;
    if (pending.cl_known) {
        if (bytes.size() > pending.cl_remaining) {
            // The worker produced more body bytes than its declared
            // Content-Length. Truncating would hide the framing violation
            // (design §8.3): fail the connection closed instead.
            fail_request(request_id, pending.session);
            return;
        }
        pending.cl_remaining -= bytes.size();
    }
    pending.outgoing = std::move(bytes);
    pending.outgoing_credit_returned_early = credit_returned_early;
    pending.writing = true;
    // The serializer and the response share one object; the response body
    // view is updated directly for each block.
    pending.response->body().data = pending.outgoing.data();
    pending.response->body().size = pending.outgoing.size();
    pending.response->body().more = true;
    // E-3 §9.2: each body write carries its own deadline — a client that
    // stopped reading stalls here, not on a shared timer.
    arm_write_timer(request_id, pending);
    http::async_write(
        pending.session->stream(),
        *pending.serializer,
        [self = shared_from_this(),
         response = pending.response,
         serializer = pending.serializer,
         session = pending.session,
         request_id](beast::error_code ec, std::size_t) {
            (void)response;
            (void)serializer;
            (void)session;
            auto it = self->requests_.find(request_id);
            if (it == self->requests_.end()) {
                return;
            }
            PendingRequest& pending = it->second;
            pending.writing = false;
            // E-3 §9.2: the write completed (or failed); its deadline is
            // spent. The next write arms its own.
            self->disarm_write_timer(pending);
            if (ec && ec != http::error::need_buffer) {
                self->fail_request(request_id, pending.session);
                return;
            }
            // need_buffer is the buffer_body serializer's normal signal that
            // the current block was fully written and more data is expected;
            // it is not an error. Credit is returned only after the client
            // write succeeded, for exactly the bytes that were written. If
            // the response already ended, the Runtime erased the request and
            // the credit is moot; submitting the frame would only be
            // rejected, so it is skipped.
            if (!pending.end_seen &&
                !pending.outgoing_credit_returned_early) {
                pending.pending_response_credit +=
                    static_cast<std::uint32_t>(pending.outgoing.size());
                self->flush_pending_credit(request_id, pending, false);
            }
            pending.outgoing_credit_returned_early = false;
            pending.outgoing.clear();
            if (!pending.body_queue.empty()) {
                QueuedResponseBody queued =
                    take_coalesced_response_body(
                        &pending.body_queue,
                        &pending.body_queue_bytes,
                        kResponseBodyWriteBatchLimit);
                self->write_body_block(
                    request_id, std::move(queued.bytes),
                    queued.credit_returned_early);
            } else if (pending.end_seen) {
                self->write_end_block(request_id);
            }
        });
}

void Impl::write_fixed_response(std::uint64_t request_id) {
    auto it = requests_.find(request_id);
    if (it == requests_.end()) {
        return;
    }
    PendingRequest& pending = it->second;
    pending.fixed_response->content_length(
        pending.fixed_body_expected);
    pending.fixed_serializer = std::make_shared<http::response_serializer<
        http::vector_body<std::uint8_t>>>(*pending.fixed_response);
    pending.writing = true;
    arm_write_timer(request_id, pending);
    const auto completion =
        [self = shared_from_this(),
         response = pending.fixed_response,
         serializer = pending.fixed_serializer,
         session = pending.session,
         request_id](beast::error_code ec, std::size_t) {
            (void)response;
            (void)serializer;
            (void)session;
            auto it = self->requests_.find(request_id);
            if (it == self->requests_.end()) {
                return;
            }
            PendingRequest& pending = it->second;
            pending.writing = false;
            self->disarm_write_timer(pending);
            if (ec) {
                self->fail_request(request_id, pending.session);
                return;
            }
            pending.head_sent = true;
            self->finalize_request(request_id, pending.session);
        };
    if (pending.head_only) {
        http::async_write_header(
            pending.session->stream(),
            *pending.fixed_serializer,
            completion);
    } else {
        http::async_write(
            pending.session->stream(),
            *pending.fixed_serializer,
            completion);
    }
}

void Impl::write_end_block(std::uint64_t request_id) {
    auto it = requests_.find(request_id);
    if (it == requests_.end()) {
        return;
    }
    PendingRequest& pending = it->second;
    if (pending.cl_known && pending.cl_remaining > 0) {
        // The worker ended the response short of its declared
        // content-length; the client would wait forever, so the connection
        // is failed instead.
        fail_request(request_id, pending.session);
        return;
    }
    pending.writing = true;
    pending.response->body().data = nullptr;
    pending.response->body().size = 0;
    pending.response->body().more = false;
    // E-3 §9.2: the chunked terminator is a write like any other; a client
    // stalled on it is torn down too.
    arm_write_timer(request_id, pending);
    http::async_write(
        pending.session->stream(),
        *pending.serializer,
        [self = shared_from_this(),
         response = pending.response,
         serializer = pending.serializer,
         session_ref = pending.session,
         request_id](beast::error_code ec, std::size_t) {
            (void)response;
            (void)serializer;
            (void)session_ref;
            auto it = self->requests_.find(request_id);
            if (it == self->requests_.end()) {
                return;
            }
            PendingRequest& pending = it->second;
            pending.writing = false;
            // E-3 §9.2: the terminator reached the client; the deadline for
            // this write is spent. The erase below destroys the timer.
            self->disarm_write_timer(pending);
            if (ec) {
                self->fail_request(request_id, pending.session);
                return;
            }
            const bool keep_alive = pending.keep_alive;
            Session* session = pending.session.get();
            // E-2 §9.3: the chunked end-block completed the response;
            // return the streaming permit exactly once.
            self->release_streaming_permit(pending);
            self->requests_.erase(it);
            // An inflight slot just freed: admit the next parked request
            // (E-1). The chunked end-block is a request-completion path
            // like finalize_request; without this, a parked request behind
            // a chunked response would sit until its queue deadline.
            self->pump_queue();
            if (session && !session->closed()) {
                if (keep_alive) {
                    session->read_request();
                } else {
                    session->close();
                }
            }
        });
}

namespace {
// Estimated wire bytes of one parked request (method + target + header
// names/values + body): the shard's queue_header_bytes budget. Exactness
// is not required — the cap exists to bound memory, so a per-field
// constant is fine.
std::size_t estimate_request_bytes(
    const http::request<http::string_body>& request) {
    std::size_t bytes = request.method_string().size() +
                        request.target().size() + request.body().size() + 8;
    for (const auto& field : request.base()) {
        bytes += field.name_string().size() + field.value().size() + 4;
    }
    return bytes;
}
}  // namespace

AdmissionResult Impl::admit_request(
    const std::shared_ptr<Session>& session,
    http::request<http::string_body>& request,
    const NormalizedPublicRequest& normalized) {
    // Fast path: a free inflight slot and nothing parked ahead of this
    // request (the queue is FIFO; a request must not jump a parked one).
    if (queue_.empty() &&
        (max_inflight_ == 0 || requests_.size() < max_inflight_)) {
        return AdmissionResult::kAccepted;
    }
    // Queueing disabled: the App quota is exhausted → 429 directly.
    if (max_queue_requests_ == 0) {
        return AdmissionResult::kQueueFull;
    }
    const std::size_t bytes = estimate_request_bytes(request);
    if (queue_.size() >= max_queue_requests_ ||
        (max_queue_header_bytes_ != 0 &&
         queue_bytes_ + bytes > max_queue_header_bytes_)) {
        return AdmissionResult::kQueueFull;
    }
    QueuedRequest entry;
    entry.session = session;
    entry.request = std::move(request);
    entry.normalized = normalized;  // copy: the caller still owns its result
    entry.bytes = bytes;
    if (queue_timeout_ms_ != 0) {
        entry.deadline =
            SteadyClock::now() + std::chrono::milliseconds(queue_timeout_ms_);
    }
    queue_.push_back(std::move(entry));
    queue_bytes_ += bytes;
    arm_queue_timer();
    return AdmissionResult::kQueued;
}

void Impl::pump_queue() {
    while (!queue_.empty() &&
           (max_inflight_ == 0 || requests_.size() < max_inflight_)) {
        QueuedRequest entry = std::move(queue_.front());
        queue_.pop_front();
        queue_bytes_ -= entry.bytes;
        // Deadline expiry while parked maps to 504 (§10.3: queue or Host
        // deadline 到期 → 504). The connection stays eligible for the next
        // request: this was a scheduling rejection, not a worker fault.
        if (entry.deadline &&
            SteadyClock::now() >= *entry.deadline) {
            if (entry.session && !entry.session->closed()) {
                entry.session->send_simple(
                    http::status::gateway_timeout, "queued request timeout",
                    entry.request.keep_alive(), entry.request.version());
            }
            continue;
        }
        const std::uint64_t request_id = allocate_request_id();
        if (request_id == 0) {
            // Unreachable: pump runs only when an inflight slot is free,
            // and allocate_request_id fails only at the inflight cap.
            if (entry.session && !entry.session->closed()) {
                entry.session->send_simple(
                    http::status::service_unavailable,
                    "no worker request slots available",
                    entry.request.keep_alive(), entry.request.version());
            }
            continue;
        }
        entry.session->current_id_ = request_id;
        begin_request(request_id, entry.session, entry.request,
                      entry.normalized);
        entry.session->start_disconnect_probe();
    }
    arm_queue_timer();
    // §7.5 row 3: the queue may have fully drained into inflight (or the
    // last parked request expired); the drain completes only when BOTH the
    // queue and the inflight table are empty, so re-check here.
    maybe_complete_drain();
}

void Impl::arm_queue_timer() {
    if (queue_timeout_ms_ == 0 || queue_.empty()) {
        queue_timer_.reset();
        return;
    }
    if (!queue_timer_) {
        queue_timer_.emplace(ioc_);
    }
    queue_timer_->expires_at(*queue_.front().deadline);
    queue_timer_->async_wait(
        [weak = weak_from_this()](const beast::error_code ec) {
            if (const std::shared_ptr<Impl> self = weak.lock()) {
                self->on_queue_timer(ec);
            }
        });
}

void Impl::on_queue_timer(const beast::error_code ec) {
    if (ec == asio::error::operation_aborted || shutting_down_) {
        return;
    }
    // Drain every parked request whose deadline has passed (the queue is
    // FIFO, so once the head is fresh the rest are too). A drained entry
    // never consumed an inflight slot, so no pump is needed here.
    while (!queue_.empty() && queue_.front().deadline &&
           SteadyClock::now() >= *queue_.front().deadline) {
        QueuedRequest expired = std::move(queue_.front());
        queue_.pop_front();
        queue_bytes_ -= expired.bytes;
        if (expired.session && !expired.session->closed()) {
            expired.session->send_simple(
                http::status::gateway_timeout, "queued request timeout",
                expired.request.keep_alive(), expired.request.version());
        }
    }
    arm_queue_timer();
}

void Impl::cancel_queued(const std::shared_ptr<Session>& session) {
    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
        if (it->session == session) {
            queue_bytes_ -= it->bytes;
            queue_.erase(it);
            return;
        }
    }
}

std::uint64_t Impl::allocate_request_id() {
    // Monotonic 64-bit non-zero request ids (design §9.4): the id space is
    // not a resource, so ids are never reused and late frames for a
    // cancelled request can never collide with a fresh one. The inflight
    // cap bounds the count, not the id space. The cap is the E-1
    // max_inflight_per_worker ceiling (0 = unlimited).
    if (max_inflight_ != 0 && requests_.size() >= max_inflight_) {
        return 0;
    }
    std::uint64_t candidate;
    do {
        candidate = next_request_id_++;
        if (candidate == 0) {
            candidate = next_request_id_++;
        }
        // A wrap past an id that is still active skips it; with a 64-bit
        // space this is unreachable in practice.
    } while (requests_.count(candidate) != 0);
    return candidate;
}

void Impl::begin_request(
    std::uint64_t request_id,
    const std::shared_ptr<Session>& session,
    const http::request<http::string_body>& request,
    const NormalizedPublicRequest& normalized) {
    PendingRequest pending;
    pending.session = session;
    pending.method = std::string(request.method_string());
    pending.keep_alive = request.keep_alive();
    pending.version = request.version();
    pending.request_body = request.body();
    const bool has_body = !pending.request_body.empty();

    // The command is assembled before the pending state is moved into the
    // map: reading from a moved-from string would yield an empty method.
    Command begin;
    begin.type = CommandType::kBeginRequest;
    begin.request_id = request_id;
    begin.method = pending.method;
    begin.url = normalized.url;
    begin.headers = normalized.headers;
    begin.end_request = bodyless_enabled_ && !has_body;
    requests_[request_id] = std::move(pending);
    executor_->submit(std::move(begin));

    // The fused path marks the request ended locally because the worker
    // already observed EOF via the RequestEnd flag; the unfused path leaves
    // it to the worker's request-credit event, which drives advance_request_body
    // to submit an explicit kEndRequest once the (empty) body is consumed.
    if (!has_body && bodyless_enabled_) {
        auto it = requests_.find(request_id);
        if (it != requests_.end()) {
            it->second.request_ended = true;
        }
    }
}

void Impl::cancel_request(std::uint64_t request_id) {
    auto it = requests_.find(request_id);
    if (it == requests_.end()) {
        return;
    }
    // E-2 §9.3: every cancellation path (client disconnect, timeouts,
    // worker exit) returns the streaming permit exactly once.
    release_streaming_permit(it->second);
    requests_.erase(it);
    // The tombstone drops late commands for this request; it is removed
    // again when the matching cancel reaches the worker thread, so the
    // set stays bounded by the cancels still in flight.
    executor_->mark_canceled(request_id);
    Command cancel;
    cancel.type = CommandType::kCancel;
    cancel.request_id = request_id;
    executor_->submit(std::move(cancel));
    // An inflight slot just freed: admit the next parked request (E-1).
    pump_queue();
    // §7.5 row 3: a cleared request may complete a drain.
    maybe_complete_drain();
}

void Impl::fail_request(std::uint64_t request_id,
                        std::shared_ptr<Session> session) {
    cancel_request(request_id);
    if (session && !session->closed()) {
        session->close();
    }
}

void Impl::finalize_request(std::uint64_t request_id,
                            std::shared_ptr<Session> session) {
    auto it = requests_.find(request_id);
    if (it == requests_.end()) {
        return;
    }
    // E-2 §9.3: the response completed; return the streaming permit.
    release_streaming_permit(it->second);
    const bool keep_alive = it->second.keep_alive;
    requests_.erase(it);
    // An inflight slot just freed: admit the next parked request (E-1)
    // before resuming the session read.
    pump_queue();
    if (session && !session->closed()) {
        if (keep_alive) {
            session->read_request();
        } else {
            session->close();
        }
    }
    // §7.5 row 3: a cleared request may complete a drain.
    maybe_complete_drain();
}

void Impl::drop_session(const std::shared_ptr<Session>& session) {
    for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
        if (it->second == session) {
            sessions_.erase(it);
            return;
        }
    }
}

void Impl::io_post(std::function<void()> function) {
    if (metrics_enabled_) {
        metrics_.asio_posts.fetch_add(1, std::memory_order_relaxed);
    }
    asio::post(ioc_, std::move(function));
}

bool Impl::bind_listener() {
    beast::error_code ec;
    const tcp::endpoint endpoint(
        asio::ip::make_address(options_.listen_address, ec),
        options_.listen_port);
    if (ec) {
        emit_log(log(), LogLane::kControl,
                 {.event = log_events::kStartup,
                  .result = "fail",
                  .message = "invalid listen address " +
                             options_.listen_address});
        return false;
    }
    acceptor_.emplace(ioc_);
    acceptor_->open(endpoint.protocol(), ec);
    if (ec) {
        emit_log(log(), LogLane::kControl,
                 {.event = log_events::kStartup,
                  .result = "fail",
                  .message = "listener open failed: " + ec.message()});
        return false;
    }
    acceptor_->set_option(asio::socket_base::reuse_address(true), ec);
    if (ec) {
        emit_log(log(), LogLane::kControl,
                 {.event = log_events::kStartup,
                  .result = "fail",
                  .message = "listener option failed: " + ec.message()});
        return false;
    }
    // M2 shared-port pools: every shard must be able to bind the SAME
    // address:port. SO_REUSEPORT is set before bind and is mandatory — an
    // unsupported platform is rejected with a static, redacted message
    // instead of silently degrading to a fake multi-process pool. The
    // default (single-worker) path never asks for it.
    if (options_.so_reuseport) {
        const int enable = 1;
        if (::setsockopt(acceptor_->native_handle(), SOL_SOCKET,
                         SO_REUSEPORT, &enable, sizeof(enable)) != 0) {
            emit_log(log(), LogLane::kControl,
                     {.event = log_events::kStartup,
                      .result = "fail",
                      .message =
                          "listener reuse_port option is unavailable"});
            return false;
        }
    }
    acceptor_->bind(endpoint, ec);
    if (ec) {
        emit_log(log(), LogLane::kControl,
                 {.event = log_events::kStartup,
                  .result = "fail",
                  .message = "listener bind failed: " + ec.message()});
        return false;
    }
    acceptor_->listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        emit_log(log(), LogLane::kControl,
                 {.event = log_events::kStartup,
                  .result = "fail",
                  .message = "listener listen failed: " + ec.message()});
        return false;
    }
    const tcp::endpoint local = acceptor_->local_endpoint(ec);
    if (ec) {
        emit_log(log(), LogLane::kControl,
                 {.event = log_events::kStartup,
                  .result = "fail",
                  .message = "listener endpoint failed: " + ec.message()});
        return false;
    }
    bound_address_ = local.address().to_string();
    bound_port_ = local.port();
    return true;
}

void Impl::close_listener() {
    // Releases the listener completely: the bound socket is closed (the
    // port becomes bindable immediately) and the optional state plus the
    // bound endpoint record are reset so a failed start leaves no trace of
    // a listener behind.
    if (acceptor_) {
        acceptor_->close();
        acceptor_.reset();
    }
    bound_address_.clear();
    bound_port_ = 0;
}

bool Impl::write_ready_line() {
    if (!options_.write_ready_record) {
        // Pool shard: the shard never publishes its own READY; the pool
        // writes one canonical record only when EVERY shard is ready. A
        // successful start() without a record still means the shard is
        // fully warm and listening.
        return true;
    }
    const std::string line =
        "{\"schema\":\"capsid-host-ready-v1\",\"app\":\"" +
        options_.application + "\",\"address\":\"" + bound_address_ +
        "\",\"port\":" + std::to_string(bound_port_) + "}\n";
    std::size_t offset = 0;
    while (offset < line.size()) {
        const ssize_t written = ::write(options_.ready_fd,
                                        line.data() + offset,
                                        line.size() - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            emit_log(log(), LogLane::kControl,
                     {.event = log_events::kStartup,
                      .result = "fail",
                      .message = "failed to write the READY record"});
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    if (offset != line.size()) {
        emit_log(log(), LogLane::kControl,
                 {.event = log_events::kStartup,
                  .result = "fail",
                  .message = "failed to write the READY record"});

        return false;
    }
    return true;
}

// The worker machinery that used to live between write_ready_line() and
// flush_pending_credit() moved to worker_executor.cc (spec §8.1): the
// executor owns the worker thread, the command/event queues and the
// protocol decoder; the io thread only submits commands and drains events.

void Impl::flush_pending_credit(std::uint64_t request_id,
                              PendingRequest& pending,
                              bool force) {
    if (pending.pending_response_credit == 0) {
        return;
    }
    // threshold 0 = immediate grant (backward compat)
    if (credit_grant_threshold_ > 0 && !force &&
        pending.pending_response_credit < credit_grant_threshold_) {
        return;
    }
    Command grant;
    grant.type = CommandType::kGrantResponseCredit;
    grant.request_id = request_id;
    grant.credit = pending.pending_response_credit;
    pending.pending_response_credit = 0;
    executor_->submit(std::move(grant));
}

// Fails a response whose worker head was rejected before any byte reached
// the wire: the request is cancelled and a clean 502 is sent instead.
void Impl::reject_response_head(std::uint64_t request_id,
                                const std::string& reason) {
    auto it = requests_.find(request_id);
    if (it == requests_.end()) {
        return;
    }
    const std::shared_ptr<Session> session = it->second.session;
    const bool keep_alive = it->second.keep_alive;
    const unsigned version = it->second.version;
    cancel_request(request_id);
    if (session && !session->closed()) {
        // §10.3: worker/IPC failure before the response head → 503.
        session->send_simple(http::status::service_unavailable, reason,
                             keep_alive, version);
    }
}

// M2 E-2 §9.3: acquires one streaming slot for a text/event-stream
// response. 0 = unlimited; otherwise the request is refused when the
// worker's slots are all held. Called from kResponseHead between the
// header sanitize and the response construction — the client has not seen
// a single response byte, so a refused permit can still be answered with a
// synthesized 503.
bool Impl::acquire_streaming_permit(std::uint64_t request_id,
                                    PendingRequest& pending) {
    (void)request_id;
    if (max_streaming_inflight_ == 0) {
        return true;
    }
    if (streaming_inflight_ >= max_streaming_inflight_) {
        return false;
    }
    ++streaming_inflight_;
    pending.holds_streaming_permit = true;
    return true;
}

// Returns the permit exactly once (idempotent), cancelling the idle
// watchdog. Every requests_.erase completion path calls this first:
// response end (write_end_block/finalize_request), cancel_request (which
// covers client disconnect, timeouts, and worker exit).
void Impl::release_streaming_permit(PendingRequest& pending) {
    if (!pending.holds_streaming_permit) {
        return;
    }
    pending.holds_streaming_permit = false;
    if (pending.idle_timer) {
        pending.idle_timer->cancel();
        pending.idle_timer.reset();
    }
    --streaming_inflight_;
}

// Arms (or restarts) the stream idle watchdog: any silence past
// stream_idle_timeout_ms_ on a held permit cancels the request and closes
// the connection. The head has reached the wire by the time this is first
// called, so a fired watchdog can only tear the connection down.
void Impl::arm_stream_idle_timer(std::uint64_t request_id,
                                 PendingRequest& pending) {
    if (stream_idle_timeout_ms_ == 0) {
        return;
    }
    if (!pending.idle_timer) {
        pending.idle_timer.emplace(ioc_);
    }
    pending.idle_timer->expires_after(
        std::chrono::milliseconds(stream_idle_timeout_ms_));
    pending.idle_timer->async_wait(
        [self = shared_from_this(), request_id](const beast::error_code ec) {
            self->on_stream_idle_timer(request_id, ec);
        });
}

void Impl::on_stream_idle_timer(std::uint64_t request_id,
                                const beast::error_code ec) {
    if (ec) {
        // Cancelled by the permit release — the response completed (or was
        // cancelled) through a faster path.
        return;
    }
    auto it = requests_.find(request_id);
    if (it == requests_.end()) {
        return;
    }
    PendingRequest& pending = it->second;
    const std::shared_ptr<Session> session = pending.session;
    // The head reached the wire before the watchdog was armed, so after
    // the cancel only a connection close is legal (§9.3).
    cancel_request(request_id);
    if (session && !session->closed()) {
        session->close();
    }
}

// M2 E-3 §9.2: arms (or restarts) the write deadline around one socket
// write. Armed at every async_write submission (head, body block, chunked
// terminator), disarmed by that write's completion handler; a deadline that
// fires means the write never completed — the client stopped reading. The
// pending write keeps the serializer/session alive via the captured shared
// pointers, so the fire path can cancel and close like any other
// cancellation.
void Impl::arm_write_timer(std::uint64_t request_id,
                           PendingRequest& pending) {
    if (write_timeout_ms_ == 0) {
        return;
    }
    if (!pending.write_timer) {
        pending.write_timer.emplace(ioc_);
    }
    pending.write_timer->expires_after(
        std::chrono::milliseconds(write_timeout_ms_));
    pending.write_timer->async_wait(
        [self = shared_from_this(), request_id](const beast::error_code ec) {
            self->on_write_timer(request_id, ec);
        });
}

void Impl::disarm_write_timer(PendingRequest& pending) {
    if (!pending.write_timer) {
        return;
    }
    pending.write_timer->cancel();
    pending.write_timer.reset();
}

void Impl::on_write_timer(std::uint64_t request_id,
                          const beast::error_code ec) {
    if (ec) {
        // Cancelled by the write's own completion — the write finished in
        // time (or the request was already torn down).
        return;
    }
    auto it = requests_.find(request_id);
    if (it == requests_.end()) {
        return;
    }
    PendingRequest& pending = it->second;
    const std::shared_ptr<Session> session = pending.session;
    // §9.2: a write that has not completed within the deadline cancels the
    // request and releases its resources. The head may already be on the
    // wire (body/terminator writes), so a connection close is the only
    // legal follow-up; the worker-side request timeout is a separate timer
    // and is not charged for this (§8.3).
    cancel_request(request_id);
    if (session && !session->closed()) {
        session->close();
    }
}

// Response headers from the worker pass through this gate before they reach
// the serializer (design §8.3): the Runtime decoder only validates the
// FetchRPC binary structure, so the Host cannot assume HTTP semantics were
// filtered. The filter runs in two phases so it does not depend on field
// order: every field is validated and all Connection nominations are
// collected first, then the fixed hop-by-hop set and the nominated names are
// removed. Names must be RFC 7230 tokens, values must be HTAB/SP/VCHAR/
// obs-text, and Content-Length must appear at most once and parse as a
// decimal. Any violation fails the response closed.
bool Impl::sanitize_response_headers(
    std::vector<std::pair<std::string, std::string>>* headers) {
    std::set<std::string> nominated;
    unsigned content_length_count = 0;
    for (const auto& [name, value] : *headers) {
        if (name.empty()) {
            return false;
        }
        for (const unsigned char c : name) {
            if (!is_token_char(c)) {
                return false;
            }
        }
        for (const unsigned char c : value) {
            if (!valid_field_value_byte(c)) {
                return false;
            }
        }
        const std::string lower = ascii_lower(name);
        if (lower == "connection") {
            // The Connection header itself is hop-by-hop; the names it
            // nominates are hop-by-hop too (RFC 7230 §6.1). Empty or
            // non-token nominations are rejected.
            if (!collect_connection_nominations(value, &nominated)) {
                return false;
            }
        } else if (lower == "content-length") {
            ++content_length_count;
            if (content_length_count > 1) {
                return false;  // duplicate Content-Length is rejected
            }
            if (value.empty()) {
                return false;
            }
            std::uint64_t content_length = 0;
            for (const unsigned char c : value) {
                if (c < '0' || c > '9') {
                    return false;
                }
                const std::uint64_t digit = c - '0';
                if (content_length >
                    (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
                    return false;
                }
                content_length = content_length * 10 + digit;
            }
        }
    }

    std::vector<std::pair<std::string, std::string>> sanitized;
    sanitized.reserve(headers->size());
    for (const auto& [name, value] : *headers) {
        const std::string lower = ascii_lower(name);
        if (lower == "connection" || lower == "keep-alive" ||
            lower == "proxy-connection" || lower == "transfer-encoding" ||
            lower == "te" || lower == "trailer" || lower == "upgrade" ||
            lower == "proxy-authenticate" ||
            lower == "proxy-authorization" ||
            nominated.count(lower) != 0) {
            continue;  // hop-by-hop: never reaches the client
        }
        sanitized.push_back({name, value});
    }
    *headers = std::move(sanitized);
    return true;
}

void Impl::write_metrics_line() {
    if (!metrics_enabled_) {
        return;
    }
    // Delta snapshot: exchange() reads and zeroes each counter in one
    // atomic step, so a line is the delta since the previous line and a
    // concurrent increment (worker thread) is never torn or lost. The
    // worker-direction counters live in the executor (spec §8.1); the
    // io-direction counters live here. The client-side snapshot is a
    // delta for the same reason.
    WorkerExecutor::Metrics& em = executor_->metrics();
    const auto commands_submitted =
        em.commands_submitted.exchange(0, std::memory_order_relaxed);
    const auto command_batches =
        em.command_batches.exchange(0, std::memory_order_relaxed);
    const auto commands_executed =
        em.commands_executed.exchange(0, std::memory_order_relaxed);
    const auto flush_calls =
        em.flush_calls.exchange(0, std::memory_order_relaxed);
    const auto flush_eagain =
        em.flush_eagain.exchange(0, std::memory_order_relaxed);
    const auto events_queued =
        em.events_queued.exchange(0, std::memory_order_relaxed);
    const auto grant_commands =
        em.grant_commands.exchange(0, std::memory_order_relaxed);
    const auto credit_bytes_granted =
        em.credit_bytes_granted.exchange(0, std::memory_order_relaxed);
    const auto command_queue_high_water =
        em.command_queue_high_water.exchange(0, std::memory_order_relaxed);
    const auto event_queue_high_water =
        em.event_queue_high_water.exchange(0, std::memory_order_relaxed);
#define CAPSID_METRIC_EXCHANGE(field) \
    const auto field = \
        metrics_.field.exchange(0, std::memory_order_relaxed)
    CAPSID_METRIC_EXCHANGE(asio_posts);
    CAPSID_METRIC_EXCHANGE(response_heads);
    CAPSID_METRIC_EXCHANGE(response_body_frames);
    CAPSID_METRIC_EXCHANGE(response_ends);
    CAPSID_METRIC_EXCHANGE(credit_stall_count);
#undef CAPSID_METRIC_EXCHANGE

    capsid::ClientIpcMetrics client_metrics;
    // The worker may already be destroyed (exited) when the final metrics
    // line is written; snapshot nothing then — the client block prints
    // zeroed fields. Ownership of the handle always stays with the executor.
    if (capsid_worker* live = executor_->worker()) {
        client_ipc_metrics_snapshot(live, &client_metrics);
    }
    // Write one compact JSON line to stderr; the runner captures it.
    std::fprintf(stderr,
        "{"
        "\"host\":{"
        "\"commands_submitted\":%lu,\"command_batches\":%lu,"
        "\"commands_executed\":%lu,\"flush_calls\":%lu,\"flush_eagain\":%lu,"
        "\"events_queued\":%lu,\"asio_posts\":%lu,"
        "\"response_heads\":%lu,\"response_body_frames\":%lu,"
        "\"response_ends\":%lu,\"grant_commands\":%lu,"
        "\"credit_bytes_granted\":%lu,\"credit_stall_count\":%lu,"
        "\"command_queue_hw\":%zu,\"event_queue_hw\":%zu},"
        "\"client\":{"
        "\"queued_frames\":%lu,\"queued_wire_bytes\":%lu,\"queue_would_block\":%lu,"
        "\"flush_calls\":%lu,\"socket_write_calls\":%lu,\"socket_write_bytes\":%lu,"
        "\"socket_write_eagain\":%lu,\"next_event_calls\":%lu,\"parsed_frames\":%lu,"
        "\"parser_payload_copied_bytes\":%lu,"
        "\"socket_read_calls\":%lu,\"socket_read_bytes\":%lu,"
        "\"socket_read_eagain\":%lu,\"queued_bytes_hw\":%zu"
        "}}\n",
        // host
        (unsigned long)commands_submitted,
        (unsigned long)command_batches,
        (unsigned long)commands_executed,
        (unsigned long)flush_calls,
        (unsigned long)flush_eagain,
        (unsigned long)events_queued,
        (unsigned long)asio_posts,
        (unsigned long)response_heads,
        (unsigned long)response_body_frames,
        (unsigned long)response_ends,
        (unsigned long)grant_commands,
        (unsigned long)credit_bytes_granted,
        (unsigned long)credit_stall_count,
        (size_t)command_queue_high_water,
        (size_t)event_queue_high_water,
        // client
        (unsigned long)client_metrics.queued_frames,
        (unsigned long)client_metrics.queued_wire_bytes,
        (unsigned long)client_metrics.queue_would_block,
        (unsigned long)client_metrics.flush_calls,
        (unsigned long)client_metrics.socket_write_calls,
        (unsigned long)client_metrics.socket_write_bytes,
        (unsigned long)client_metrics.socket_write_eagain,
        (unsigned long)client_metrics.next_event_calls,
        (unsigned long)client_metrics.parsed_frames,
        (unsigned long)client_metrics.parser_payload_copied_bytes,
        (unsigned long)client_metrics.socket_read_calls,
        (unsigned long)client_metrics.socket_read_bytes,
        (unsigned long)client_metrics.socket_read_eagain,
        (unsigned long)client_metrics.queued_bytes_high_water);
}

SingleWorkerServer::SingleWorkerServer(SingleWorkerServerOptions options)
    : impl_(std::make_shared<Impl>(std::move(options))) {}

SingleWorkerServer::~SingleWorkerServer() {
    // The facade owns the Impl and must stop it while it still holds the
    // reference: long-lived handlers never own the Impl (weak captures),
    // so destroying a running server performs the bounded stop here —
    // listener closed, sessions closed, both threads joined, worker
    // reaped — within the shutdown window. Impl::~Impl alone can never be
    // relied on to initiate the stop.
    impl_->request_stop();
    std::string error;
    impl_->wait(&error);
    impl_.reset();
}

bool SingleWorkerServer::start(const std::vector<std::uint8_t>& bundle,
                               std::string* error) {
    return impl_->start(bundle, error);
}

void SingleWorkerServer::request_stop() { impl_->request_stop(); }

void SingleWorkerServer::begin_drain(std::uint64_t deadline_ms) {
    impl_->begin_drain(deadline_ms);
}

SingleWorkerServer::DrainMetrics SingleWorkerServer::drain_metrics() const {
    return impl_->drain_metrics();
}

bool SingleWorkerServer::wait(std::string* error) {
    return impl_->wait(error);
}

std::uint16_t SingleWorkerServer::bound_port() const {
    return impl_->bound_port();
}

bool SingleWorkerServer::activate_accept(std::string* error) {
    return impl_->activate_accept(error);
}

bool SingleWorkerServer::worker_available() const {
    return impl_->worker_available();
}

int SingleWorkerServer::run(const std::vector<std::uint8_t>& bundle) {
    std::string error;
    if (!impl_->start(bundle, &error)) {
        emit_log(impl_->log(), LogLane::kControl,
                 {.event = log_events::kStartup,
                  .result = "fail",
                  .message = error});
        return 1;
    }
    if (!impl_->wait(&error)) {
        emit_log(impl_->log(), LogLane::kControl,
                 {.event = log_events::kShutdown,
                  .result = "fail",
                  .message = error});
        return 1;
    }
    return 0;
}

}  // namespace capsid::host
