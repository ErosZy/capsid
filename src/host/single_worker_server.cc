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
#include <poll.h>
#include <set>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "capsid/runtime.h"
#include "client_ipc_metrics.h"
#include "host/request_normalization.h"
#include "host/worker_event_source.h"

namespace capsid::host {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using SteadyClock = std::chrono::steady_clock;

constexpr std::uint64_t kMaxInflightRequests = 128;
constexpr std::size_t kMaxRequestBodyBytes = 16u * 1024u * 1024u;
constexpr auto kReadyTimeout = std::chrono::seconds(10);
constexpr auto kShutdownGrace = std::chrono::seconds(2);

// Early-credit window (host): frames are reimbursed on receive while
// the per-request HTTP write queue is below this; beyond it credit
// falls back to write-completion so slow clients stay bounded.
static const size_t kEarlyCreditWindow = 64u * 1024u;

void write_stderr(std::string_view message) {
    std::fwrite(message.data(), 1, message.size(), stderr);
    std::fputc('\n', stderr);
}

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

enum class CommandType {
    kBeginRequest,
    kWriteRequest,
    kEndRequest,
    kCancel,
    kGrantResponseCredit,
    kShutdown,
};

// A command from the io thread to the worker thread. Strings are copied so
// the worker thread never reads io-thread-owned buffers.
struct Command {
    CommandType type = CommandType::kBeginRequest;
    std::uint64_t request_id = 0;
    std::string method;
    std::string url;
    std::vector<NormalizedPublicRequestHeader> headers;
    std::vector<std::uint8_t> body;
    std::uint32_t credit = 0;
    // kBeginRequest only: the request has no body, so the kRequestEnd frame
    // is sent back-to-back with the head in the same flush. A separate end
    // command could land in a different IPC read than its begin, letting the
    // Runtime complete a synchronous handler response (and erase the request)
    // before the end frame arrives — which it rejects as an invalid frame
    // and kills the worker.
    bool end_request = false;
};

struct WorkerEvent {
    enum class Type {
        kRequestCredit,
        kResponseHead,
        kResponseBody,
        kResponseEnd,
        kLog,
        kError,
        kExit,
        kRequestTimeout,
        kRequestFailure,
    };
    Type type = Type::kLog;
    std::uint64_t request_id = 0;
    std::uint32_t credit = 0;
    std::uint16_t status = 0;
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<std::uint8_t> body;
    std::string text;
};

class Session;

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
    std::vector<std::uint8_t> outgoing;
    std::deque<std::vector<std::uint8_t>> body_queue;
    // Total bytes currently queued for the HTTP write, so the early
    // credit window (performance loop v1) can stay bounded: a slow
    // client must not make the host buffer unboundedly.
    size_t body_queue_bytes;
    bool head_sent = false;
    bool head_only = false;
    bool writing = false;
    bool end_seen = false;
    bool cl_known = false;
    std::size_t cl_remaining = 0;
    // Credit aggregation (diagnostic, zero bytes overhead when disabled).
    std::uint32_t pending_response_credit = 0;
};

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
        // exact value "0" disables it.
        const char* nodelay = std::getenv("CAPSID_TCP_NODELAY");
        if (nodelay != nullptr && std::strcmp(nodelay, "0") == 0) {
            std::fprintf(stderr,
                         "capsid-host: TCP_NODELAY disabled\n");
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
    std::optional<std::uint64_t> current_id_;
};

class Impl : public std::enable_shared_from_this<Impl> {
public:
    explicit Impl(SingleWorkerServerOptions options)
        : options_(std::move(options)), signals_(ioc_) {}

    ~Impl();

    bool start(const std::vector<std::uint8_t>& bundle, std::string* error);
    void request_stop();
    bool wait(std::string* error);
    std::uint16_t bound_port() const { return bound_port_; }
    bool activate_accept(std::string* error);
    bool worker_available() const {
        return worker_available_.load(std::memory_order_relaxed);
    }

    // Called from Session (io thread).
    void begin_request(std::uint64_t request_id,
                       const std::shared_ptr<Session>& session,
                       const http::request<http::string_body>& request,
                       const NormalizedPublicRequest& normalized);
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

    // Commands from io thread to worker thread.
    void submit_command(Command command);

    // Worker thread entry.
    void worker_thread_main();

private:
    friend class Session;

    void do_accept();
    void arm_accept_on_io_thread();
    void on_signal(int signal_number);
    void pump_events();
    void handle_worker_event(WorkerEvent event);
    void advance_request_body(std::uint64_t request_id);
    void write_body_block(std::uint64_t request_id, std::vector<std::uint8_t> bytes);
    void write_end_block(std::uint64_t request_id);
    void io_post(std::function<void()> function);
    bool wait_for_ready();
    bool bind_listener();
    void close_listener();
    bool write_ready_line();
    bool execute_command(Command command);
    bool batch_flush();
    bool handle_worker_protocol_event(const capsid_event& event);
    void queue_worker_event(WorkerEvent event);
    void queue_worker_exit_event();
    bool report_runtime_failure(std::uint64_t request_id,
                                capsid_result result,
                                const char* operation);
    bool sanitize_response_headers(
        std::vector<std::pair<std::string, std::string>>* headers);
    void reject_response_head(std::uint64_t request_id,
                              const std::string& reason);
    void shutdown_worker_and_join();

    // Diagnostic IPC metrics (CAPSID_HOST_IPC_METRICS=1 only; zero overhead
    // in headline runs). Counters are reset after each write so the runner
    // can sample per-profile-run deltas.
    struct Metrics {
        // Command direction (io → worker).
        std::atomic<uint64_t> commands_submitted = 0;
        std::atomic<uint64_t> command_batches = 0;   // wake-pipe writes
        std::atomic<uint64_t> commands_executed = 0;
        std::atomic<uint64_t> flush_calls = 0;
        std::atomic<uint64_t> flush_eagain = 0;
        // Event direction (worker → io).
        std::atomic<uint64_t> events_queued = 0;
        std::atomic<uint64_t> asio_posts = 0;        // io_context::post calls
        std::atomic<uint64_t> response_heads = 0;
        std::atomic<uint64_t> response_body_frames = 0;
        std::atomic<uint64_t> response_ends = 0;
        // Credit.
        std::atomic<uint64_t> grant_commands = 0;
        std::atomic<uint64_t> credit_bytes_granted = 0;
        std::atomic<uint64_t> credit_stall_count = 0;
        // Worker-thread queue depths.
        std::atomic<size_t> command_queue_high_water = 0;
        std::atomic<size_t> event_queue_high_water = 0;
    };
    Metrics metrics_;
    bool metrics_enabled_ = false;
    void write_metrics_line();

    // Credit aggregation (CAPSID_CREDIT_GRANT_THRESHOLD env var; 0 = off).
    // When non-zero, response credit is accumulated per-request and only
    // submitted in batches of at least this many bytes, reducing WINDOW_UPDATE
    // frames on the command channel.
    std::uint32_t credit_grant_threshold_ = 0;
    void flush_pending_credit(std::uint64_t request_id,
                              PendingRequest& pending,
                              bool force);

    // Bodyless request fusion (CAPSID_BODYLESS env var; on by default, only
    // the exact value "0" disables). When on, requests with an empty body
    // are sent as a single RequestHead carrying kFlagRequestEnd so the worker
    // skips request-direction credit and observes EOF immediately. The toggle
    // lets the same binary run the off/on A/B for M1C acceptance evidence.
    bool bodyless_enabled_ = true;

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
    // shutdown_sent_ guarantees the Runtime shutdown frame is queued at
    // most once (repeated stops can never trigger a double shutdown);
    // io_running_ tells request_stop whether the event-loop thread exists,
    // so the shutdown path never posts into a dead io_context.
    std::atomic<bool> start_gate_ = false;
    std::atomic<bool> stop_requested_ = false;
    std::atomic<bool> shutdown_sent_ = false;
    std::atomic<bool> io_running_ = false;
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

    // Bridge between the io thread and the worker thread.
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Command> commands_;
    std::deque<WorkerEvent> events_;
    std::set<std::uint64_t> canceled_;
    bool ready_ = false;
    bool ready_match_ = false;
    bool exited_ = false;
    bool exit_event_queued_ = false;
    capsid_worker* worker_ = nullptr;
    WorkerEventSource source_;
    std::optional<SteadyClock::time_point> shutdown_deadline_;
    std::thread worker_thread_;
};

void Session::start() { read_request(); }

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
    if (impl_->worker_dead()) {
        send_simple(http::status::bad_gateway, "worker unavailable", false,
                    request.version());
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

    const std::uint64_t request_id = impl_->allocate_request_id();
    if (request_id == 0) {
        send_simple(http::status::service_unavailable,
                    "no worker request slots available", request.keep_alive(),
                    request.version());
        return;
    }
    current_id_ = request_id;
    impl_->begin_request(request_id, shared_from_this(), request,
                         normalized.request);
    start_disconnect_probe();
}

void Session::close() {
    if (closed_) {
        return;
    }
    closed_ = true;
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
    // worker thread's bounded terminate backstop forces the blocking
    // destroy to finish promptly. Never std::terminate, never a leak.
    request_stop();
    if (io_thread_.joinable()) {
        io_thread_.join();
    }
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    if (worker_) {
        capsid_worker_destroy(worker_);
        worker_ = nullptr;
    }
}

bool Impl::start(const std::vector<std::uint8_t>& bundle,
                 std::string* error) {
    if (start_gate_.exchange(true)) {
        if (error != nullptr) {
            *error = "server already started";
        }
        return false;
    }
    // ---- 1. spawn / load / flush (the same two-phase prepare as the
    // legacy run path). Every failure below recycles what it created
    // before returning false.
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = options_.worker_path.c_str();
    config.request_timeout_ms = options_.request_timeout_ms;
    config.initial_stream_window = options_.initial_stream_window;
    config.strict_sandbox = options_.strict_sandbox ? 1U : 0U;
    config.max_inflight_requests =
        static_cast<std::uint32_t>(kMaxInflightRequests);
    // egress_policy and capability_policy stay NULL: every outbound Fetch
    // is denied by the Runtime default.

    capsid_worker* worker = nullptr;
    const capsid_result spawn_result =
        capsid_worker_spawn(&config, &worker);
    if (spawn_result != CAPSID_OK) {
        if (error != nullptr) {
            *error = "worker spawn failed: " +
                     std::string(capsid_result_string(spawn_result));
        }
        return false;
    }
    worker_ = worker;
    source_.set_worker(worker);

    const capsid_result load_result = capsid_worker_load_bundle_named(
        worker_, bundle.data(), bundle.size(), options_.source_name.c_str());
    if (load_result != CAPSID_OK) {
        if (error != nullptr) {
            *error = "bundle load failed: " +
                     std::string(capsid_result_string(load_result));
        }
        capsid_worker_destroy(worker_);
        worker_ = nullptr;
        return false;
    }
    // load_bundle_named only queues the frame; the worker must receive it
    // before it loads the application and emits READY.
    const capsid_result flush_result = capsid_worker_flush(worker_);
    if (flush_result != CAPSID_OK) {
        if (error != nullptr) {
            *error = "bundle flush failed: " +
                     std::string(capsid_result_string(flush_result));
        }
        capsid_worker_destroy(worker_);
        worker_ = nullptr;
        return false;
    }

    // Diagnostic IPC metrics: CAPSID_HOST_IPC_METRICS=1 enables counters
    // with zero overhead in headline runs (branch is off by default).
    const char* metrics_env = std::getenv("CAPSID_HOST_IPC_METRICS");
    if (metrics_env != nullptr && std::strcmp(metrics_env, "1") == 0) {
        metrics_enabled_ = true;
    }
    // Enable the client-side IPC metrics when the host-side ones are on,
    // so both sides produce matching counter sets (the client metrics are
    // already instrumented in client.cc behind the same env variable).
    if (metrics_enabled_) {
        client_ipc_metrics_enable(worker_, true);
    }
    // Credit aggregation threshold: grant only when pending ≥ threshold.
    const char* credit_env = std::getenv("CAPSID_CREDIT_GRANT_THRESHOLD");
    if (credit_env != nullptr) {
        std::uint64_t val = 0;
        for (const char* p = credit_env; *p != '\0'; ++p) {
            if (*p >= '0' && *p <= '9') {
                val = val * 10 + static_cast<std::uint64_t>(*p - '0');
            } else { val = 0; break; }
        }
        if (val > 0 && val <= std::numeric_limits<std::uint32_t>::max()) {
            credit_grant_threshold_ = static_cast<std::uint32_t>(val);
        }
    }
    // Bodyless fusion toggle: only the exact value "0" disables it (same
    // convention as CAPSID_TCP_NODELAY).
    const char* bodyless_env = std::getenv("CAPSID_BODYLESS");
    if (bodyless_env != nullptr && std::strcmp(bodyless_env, "0") == 0) {
        bodyless_enabled_ = false;
        std::fprintf(stderr,
                     "capsid-host: bodyless request fusion disabled\n");
    }

    // The worker thread takes exclusive ownership of every further Runtime
    // API call (READY arrives through next_event).
    worker_thread_ = std::thread([this] { worker_thread_main(); });

    // Every failure after the worker thread has started must tear it down
    // before returning: the shutdown command wakes the blocking wait, the
    // bounded terminate backstop in the worker loop forces destroy() to
    // finish promptly, and only then is the thread joined.
    if (!wait_for_ready()) {
        shutdown_worker_and_join();
        if (error != nullptr) {
            *error = "worker did not become READY";
        }
        return false;
    }
    worker_available_ = true;
    if (!bind_listener()) {
        // bind_listener may have partially succeeded (open, set_option,
        // bind, listen or local_endpoint): the acceptor is closed and
        // reset here so a failed start never leaves the port occupied.
        worker_available_ = false;
        close_listener();
        shutdown_worker_and_join();
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
        shutdown_worker_and_join();
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
    // many stop sources fire (request_stop, SIGTERM, destructor).
    {
        Command shutdown;
        shutdown.type = CommandType::kShutdown;
        if (!shutdown_sent_.exchange(true)) {
            submit_command(std::move(shutdown));
        }
    }
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
    std::call_once(wait_once_, [this] {
        if (io_thread_.joinable()) {
            io_thread_.join();
        }
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
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
    // trigger a Runtime double shutdown.
    Command shutdown;
    shutdown.type = CommandType::kShutdown;
    if (!shutdown_sent_.exchange(true)) {
        submit_command(std::move(shutdown));
    }
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

void Impl::pump_events() {
    std::deque<WorkerEvent> local;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        local.swap(events_);
    }
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
        pending.response =
            std::make_shared<http::response<http::buffer_body>>();
        pending.response->result(static_cast<http::status>(event.status));
        pending.response->version(pending.version);
        pending.response->keep_alive(pending.keep_alive);
        for (const auto& [name, value] : event.headers) {
            pending.response->base().insert(name, value);
        }
        if (!pending.response->has_content_length() &&
            !pending.response->chunked()) {
            pending.response->chunked(true);
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
                value = value * 10 + static_cast<std::uint64_t>(c - '0');
            }
            if (valid) {
                pending.cl_known = true;
                pending.cl_remaining = static_cast<std::size_t>(value);
            }
        }
        pending.serializer = std::make_shared<
            http::response_serializer<http::buffer_body>>(
            *pending.response);
        pending.head_only = pending.method == "HEAD";
        pending.writing = true;
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
                if (ec) {
                    self->fail_request(request_id, pending.session);
                    return;
                }
                pending.head_sent = true;
                if (!pending.body_queue.empty()) {
                    // Body events that arrived while the head was being
                    // written are queued here and flushed in order.
                    std::vector<std::uint8_t> queued =
                        std::move(pending.body_queue.front());
                    pending.body_queue.pop_front();
                    self->write_body_block(request_id, std::move(queued));
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
        // Performance loop v1: return credit as soon as the frame is
        // received, not after the client write completes, while the
        // per-request write queue stays shallow. This removes one host
        // round-trip per frame (write-completion -> credit -> worker
        // wakeup). Beyond the window the credit falls back to
        // write-completion, so a slow client cannot make the host
        // buffer unboundedly.
        if (pending.body_queue_bytes < kEarlyCreditWindow) {
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
            pending.body_queue.push_back(std::move(event.body));
            return;
        }
        write_body_block(event.request_id, std::move(event.body));
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
        // so the grant frame is queued only to satisfy the tombstone check.
        flush_pending_credit(event.request_id, pending, true);
        // The Runtime erased this request when it sent RESPONSE_END, so any
        // request-direction frame still queued for it (body writes, the
        // request-end) would be rejected as an invalid IPC frame and kill
        // the worker. Tombstone the id; the kCancel marker below is a no-op
        // at the Runtime (the id is already gone) and removes the tombstone
        // once the stale commands have drained, keeping the set bounded.
        {
            std::unique_lock<std::mutex> lock(mutex_);
            canceled_.insert(event.request_id);
        }
        Command cancel;
        cancel.type = CommandType::kCancel;
        cancel.request_id = event.request_id;
        submit_command(std::move(cancel));
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
        write_stderr(event.text);
        return;
    case WorkerEvent::Type::kError:
        write_stderr(std::string("capsid-host: worker error: ") +
                     event.text + " (request " +
                     std::to_string(event.request_id) + ")");
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
            session->send_simple(http::status::bad_gateway,
                                 "worker request failed", keep_alive, version);
        }
        return;
    }
    case WorkerEvent::Type::kExit: {
        worker_dead_ = true;
        worker_available_ = false;
        // Fault isolation: the shard's worker process is gone, so this
        // shard must stop accepting NEW connections immediately — with
        // SO_REUSEPORT the kernel routes subsequent connections to the
        // healthy shards only once this listener is closed. In-flight
        // requests still receive their 502/close handling below. A
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
                session->send_simple(http::status::bad_gateway,
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
        submit_command(std::move(command));
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
            submit_command(std::move(command));
        }
    }
}

void Impl::write_body_block(std::uint64_t request_id,
                            std::vector<std::uint8_t> bytes) {
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
    pending.writing = true;
    // The serializer and the response share one object; the response body
    // view is updated directly for each block.
    pending.response->body().data = pending.outgoing.data();
    pending.response->body().size = pending.outgoing.size();
    pending.response->body().more = true;
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
            if (!pending.end_seen) {
                pending.pending_response_credit +=
                    static_cast<std::uint32_t>(pending.outgoing.size());
                self->flush_pending_credit(request_id, pending, false);
            }
            pending.outgoing.clear();
            if (!pending.body_queue.empty()) {
                std::vector<std::uint8_t> queued =
                    std::move(pending.body_queue.front());
                pending.body_queue.pop_front();
                pending.body_queue_bytes -= queued.size();
                self->write_body_block(request_id, std::move(queued));
            } else if (pending.end_seen) {
                self->write_end_block(request_id);
            }
        });
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
            if (ec) {
                self->fail_request(request_id, pending.session);
                return;
            }
            const bool keep_alive = pending.keep_alive;
            Session* session = pending.session.get();
            self->requests_.erase(it);
            if (session && !session->closed()) {
                if (keep_alive) {
                    session->read_request();
                } else {
                    session->close();
                }
            }
        });
}

std::uint64_t Impl::allocate_request_id() {
    // Monotonic 64-bit non-zero request ids (design §9.4): the id space is
    // not a resource, so ids are never reused and late frames for a
    // cancelled request can never collide with a fresh one. The inflight
    // cap bounds the count, not the id space.
    if (requests_.size() >= kMaxInflightRequests) {
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
    submit_command(std::move(begin));

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
    requests_.erase(it);
    {
        // The tombstone drops late commands for this request; it is removed
        // again when the matching cancel reaches the worker thread, so the
        // set stays bounded by the cancels still in flight.
        std::unique_lock<std::mutex> lock(mutex_);
        canceled_.insert(request_id);
    }
    Command cancel;
    cancel.type = CommandType::kCancel;
    cancel.request_id = request_id;
    submit_command(std::move(cancel));
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
    const bool keep_alive = it->second.keep_alive;
    requests_.erase(it);
    if (session && !session->closed()) {
        if (keep_alive) {
            session->read_request();
        } else {
            session->close();
        }
    }
}

void Impl::drop_session(const std::shared_ptr<Session>& session) {
    for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
        if (it->second == session) {
            sessions_.erase(it);
            return;
        }
    }
}

void Impl::submit_command(Command command) {
    if (metrics_enabled_) {
        metrics_.commands_submitted.fetch_add(1, std::memory_order_relaxed);
    }
    std::unique_lock<std::mutex> lock(mutex_);
    const bool was_empty = commands_.empty();
    commands_.push_back(std::move(command));
    if (was_empty) {
        if (metrics_enabled_) {
            metrics_.command_batches.fetch_add(1, std::memory_order_relaxed);
        }
        // Wake the worker thread out of its blocking wait. The byte is
        // written on the empty→non-empty transition only: a push into a
        // non-empty queue is already covered by an outstanding byte (or the
        // worker is draining), so the wake pipe cannot fill.
        source_.wake();
    }
    if (metrics_enabled_) {
        {
            const size_t size = commands_.size();
            size_t hw = metrics_.command_queue_high_water.load(
                std::memory_order_relaxed);
            while (hw < size &&
                   !metrics_.command_queue_high_water.compare_exchange_weak(
                       hw, size, std::memory_order_relaxed)) {
            }
        }
    }
    cv_.notify_one();
}

void Impl::io_post(std::function<void()> function) {
    if (metrics_enabled_) {
        metrics_.asio_posts.fetch_add(1, std::memory_order_relaxed);
    }
    asio::post(ioc_, std::move(function));
}

bool Impl::wait_for_ready() {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool notified = cv_.wait_for(lock, kReadyTimeout, [this] {
        return ready_ || exited_;
    });
    if (!notified) {
        write_stderr("capsid-host: worker did not become READY in time");
        return false;
    }
    if (exited_) {
        write_stderr("capsid-host: worker exited before READY");
        return false;
    }
    if (!ready_match_) {
        write_stderr("capsid-host: worker compatibility ID mismatch");
        return false;
    }
    return true;
}

bool Impl::bind_listener() {
    beast::error_code ec;
    const tcp::endpoint endpoint(
        asio::ip::make_address(options_.listen_address, ec),
        options_.listen_port);
    if (ec) {
        write_stderr("capsid-host: invalid listen address " +
                     options_.listen_address);
        return false;
    }
    acceptor_.emplace(ioc_);
    acceptor_->open(endpoint.protocol(), ec);
    if (ec) {
        write_stderr("capsid-host: listener open failed: " + ec.message());
        return false;
    }
    acceptor_->set_option(asio::socket_base::reuse_address(true), ec);
    if (ec) {
        write_stderr("capsid-host: listener option failed: " + ec.message());
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
            write_stderr(
                "capsid-host: listener reuse_port option is unavailable");
            return false;
        }
    }
    acceptor_->bind(endpoint, ec);
    if (ec) {
        write_stderr("capsid-host: listener bind failed: " + ec.message());
        return false;
    }
    acceptor_->listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        write_stderr("capsid-host: listener listen failed: " + ec.message());
        return false;
    }
    const tcp::endpoint local = acceptor_->local_endpoint(ec);
    if (ec) {
        write_stderr("capsid-host: listener endpoint failed: " + ec.message());
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
    const ssize_t written =
        ::write(options_.ready_fd, line.data(), line.size());
    if (written != static_cast<ssize_t>(line.size())) {
        write_stderr("capsid-host: failed to write the READY record");
        return false;
    }
    return true;
}

void Impl::worker_thread_main() {
    for (;;) {
        // Drain command wake bytes before swapping: a command pushed before
        // this drain is either already in the queue (the swap picks it up)
        // or was pushed after the swap (its wake byte is still in the pipe
        // and wakes the poll below). Draining first keeps both orderings
        // safe.
        source_.drain_wake_bytes();
        {
            std::unique_lock<std::mutex> lock(mutex_);
            std::deque<Command> local;
            local.swap(commands_);
            if (shutdown_deadline_.has_value() &&
                SteadyClock::now() >= *shutdown_deadline_) {
                if (worker_) {
                    capsid_worker_terminate(worker_);
                }
            }
            lock.unlock();
            for (Command& command : local) {
                if (!execute_command(std::move(command))) {
                    // A closed or wedged worker: fail every pending request
                    // on the io thread and reap below.
                    queue_worker_exit_event();
                    goto worker_exit;
                }
            }
            // Batch flush: every queued frame reaches the worker in one
            // write syscall instead of one per command.
            if (!batch_flush()) {
                queue_worker_exit_event();
                goto worker_exit;
            }
        }

        // Drain every pending event; next_event returns WOULD_BLOCK when the
        // IPC channel is idle.
        bool worker_exited = false;
        for (;;) {
            // struct_size is the size-negotiation contract of the Runtime
            // API: a value-uninitialized capsid_event reads as garbage and
            // next_event rejects it, so zero the struct and state its size.
            capsid_event event = {};
            event.struct_size = sizeof(event);
            const capsid_result result =
                capsid_worker_next_event(worker_, &event);
            if (result == CAPSID_OK) {
                if (!handle_worker_protocol_event(event)) {
                    worker_exited = true;
                    break;
                }
                continue;
            }
            if (result == CAPSID_WOULD_BLOCK) {
                break;
            }
            // The IPC channel is closed or corrupt: the worker is gone.
            queue_worker_exit_event();
            worker_exited = true;
            break;
        }
        if (worker_exited) {
            goto worker_exit;
        }

        // Block until the worker IPC descriptor is readable, a command wake
        // arrives, or the shutdown deadline passes. The io thread's wake
        // byte keeps command pickup latency at poll latency instead of a
        // polling period; the worker thread never busy-polls.
        std::optional<SteadyClock::time_point> until;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (shutdown_deadline_.has_value()) {
                until = shutdown_deadline_;
            }
        }
        if (!source_.wait(until)) {
            queue_worker_exit_event();
            goto worker_exit;
        }
    }

worker_exit:
    // The worker thread remains the sole owner up to and including the
    // blocking destroy.
    capsid_worker_destroy(worker_);
    worker_ = nullptr;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        exited_ = true;
    }
    cv_.notify_all();
}

// A Runtime call failed for a reason other than a full queue: the Host and
// the Runtime disagree about the request's state (WOULD_BLOCK on admission,
// INVALID_ARGUMENT for a frame the Runtime no longer accepts). The request
// fails closed on the client side instead of leaving the Host and the
// Runtime silently out of sync. Returns false when the worker itself is
// gone, which takes the exit path and fails every pending request.
bool Impl::report_runtime_failure(std::uint64_t request_id,
                                  capsid_result result,
                                  const char* operation) {
    if (result == CAPSID_CLOSED) {
        return false;
    }
    write_stderr(std::string("capsid-host: worker ") + operation +
                 " failed: " + capsid_result_string(result) + " (request " +
                 std::to_string(request_id) + ")");
    WorkerEvent failure;
    failure.type = WorkerEvent::Type::kRequestFailure;
    failure.request_id = request_id;
    queue_worker_event(std::move(failure));
    return true;
}

bool Impl::execute_command(Command command) {
    if (metrics_enabled_) {
        metrics_.commands_executed.fetch_add(1, std::memory_order_relaxed);
    }
    // Commands for a cancelled request are dropped before execution: the
    // worker may have already expired the request (and erased its state), so
    // a late write/end/grant frame would be rejected as an invalid IPC frame
    // and kill the worker. The tombstone entry is removed again when the
    // matching cancel reaches the worker thread, so the set stays bounded by
    // the cancels still in flight.
    if (command.type != CommandType::kBeginRequest &&
        command.type != CommandType::kShutdown &&
        command.type != CommandType::kCancel) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (canceled_.count(command.request_id) != 0) {
            return true;
        }
    }
    switch (command.type) {
    case CommandType::kBeginRequest: {
        std::vector<capsid_header> headers;
        headers.reserve(command.headers.size());
        for (const auto& header : command.headers) {
            headers.push_back(capsid_header{
                {reinterpret_cast<const std::uint8_t*>(header.name.data()),
                 header.name.size()},
                {reinterpret_cast<const std::uint8_t*>(header.value.data()),
                 header.value.size()},
            });
        }
        const capsid_header* header_ptr =
            headers.empty() ? nullptr : headers.data();
        capsid_result result;
        if (command.end_request) {
            result = capsid_worker_begin_bodyless_request(
                worker_, command.request_id, command.method.c_str(),
                command.url.c_str(), header_ptr, headers.size());
        } else {
            result = capsid_worker_begin_request(
                worker_, command.request_id, command.method.c_str(),
                command.url.c_str(), header_ptr, headers.size());
        }
        if (result != CAPSID_OK) {
            return report_runtime_failure(command.request_id, result,
                                          "begin_request");
        }
        break;
        if (result != CAPSID_OK) {
            return report_runtime_failure(command.request_id, result,
                                          "begin_request");
        }
        break;
    }
    case CommandType::kWriteRequest: {
        const capsid_result result = capsid_worker_write_request(
            worker_, command.request_id, command.body.data(),
            command.body.size());
        if (result == CAPSID_WOULD_BLOCK) {
            // The Runtime did not consume the chunk (credit or write queue
            // full); retry the same chunk before any later request-body
            // chunk. Nothing was queued, so no flush is needed.
            std::unique_lock<std::mutex> lock(mutex_);
            commands_.push_front(std::move(command));
            return true;
        }
        if (result != CAPSID_OK) {
            return report_runtime_failure(command.request_id, result,
                                          "write_request");
        }
        break;
    }
    case CommandType::kEndRequest: {
        const capsid_result result =
            capsid_worker_end_request(worker_, command.request_id);
        if (result != CAPSID_OK) {
            return report_runtime_failure(command.request_id, result,
                                          "end_request");
        }
        break;
    }
    case CommandType::kCancel: {
        const capsid_result result =
            capsid_worker_cancel(worker_, command.request_id);
        if (result == CAPSID_CLOSED) {
            return false;
        }
        if (result != CAPSID_OK) {
            // Cancel is best-effort: the Runtime already forgot the request
            // or the channel is wedged. Log and continue; the request is
            // gone from the Host either way.
            write_stderr(std::string("capsid-host: worker cancel failed: ") +
                         capsid_result_string(result) + " (request " +
                         std::to_string(command.request_id) + ")");
        } else {
            // The cancel reached the worker: the Runtime has forgotten this
            // request, so no further frame for it can be valid. The tombstone
            // entry is no longer needed; dropping it here bounds the set to
            // the cancels still in flight.
            std::unique_lock<std::mutex> lock(mutex_);
            canceled_.erase(command.request_id);
        }
        break;
    }
    case CommandType::kGrantResponseCredit: {
        if (metrics_enabled_) {
            metrics_.grant_commands.fetch_add(1, std::memory_order_relaxed);
            metrics_.credit_bytes_granted.fetch_add(
                command.credit, std::memory_order_relaxed);
        }
        const capsid_result result = capsid_worker_grant_response_credit(
            worker_, command.request_id, command.credit);
        if (result == CAPSID_WOULD_BLOCK) {
            std::unique_lock<std::mutex> lock(mutex_);
            commands_.push_front(std::move(command));
            return true;
        }
        if (result != CAPSID_OK) {
            // Grants are only submitted for requests whose response has not
            // ended (see the write completion guard) and dropped by the
            // tombstone once a request ends or is cancelled, so any other
            // rejection is a genuine Host/Runtime state mismatch.
            return report_runtime_failure(command.request_id, result,
                                          "grant_response_credit");
        }
        break;
    }
    case CommandType::kShutdown:
        capsid_worker_shutdown(worker_);
        {
            std::unique_lock<std::mutex> lock(mutex_);
            shutdown_deadline_ = SteadyClock::now() + kShutdownGrace;
        }
        break;
    }
    // Frames were queued; the caller (worker thread main loop) batches the
    // flush across all commands of this batch.
    return true;
}

// batch_flush sends every queued frame that the current command batch
// produced. It is called once per command drain, not per command, so a
// batch of N GET requests produces one write syscall instead of N.
bool Impl::batch_flush() {
    const capsid_result result = capsid_worker_flush(worker_);
    if (result == CAPSID_OK || result == CAPSID_WOULD_BLOCK) {
        // WOULD_BLOCK is fine — the frames are in the worker's input buffer;
        // the next batch's poll/wait will drain and flush more.
        return true;
    }
    write_stderr(std::string("capsid-host: worker batch flush failed: ") +
                 capsid_result_string(result));
    return false;
}

bool Impl::handle_worker_protocol_event(const capsid_event& event) {
    switch (event.type) {
    case CAPSID_EVENT_READY: {
        std::string payload(
            reinterpret_cast<const char*>(event.payload.data),
            event.payload.size);
        capsid_build_info info;
        capsid_build_info_init(&info);
        const capsid_result result = capsid_runtime_build_info(&info);
        bool match = false;
        if (result == CAPSID_OK && info.compatibility_id != nullptr) {
            match = payload == info.compatibility_id;
        }
        std::unique_lock<std::mutex> lock(mutex_);
        ready_ = true;
        ready_match_ = match;
        cv_.notify_all();
        return true;
    }
    case CAPSID_EVENT_REQUEST_CREDIT: {
        WorkerEvent worker_event;
        worker_event.type = WorkerEvent::Type::kRequestCredit;
        worker_event.request_id = event.request_id;
        worker_event.credit = event.credit;
        queue_worker_event(std::move(worker_event));
        return true;
    }
    case CAPSID_EVENT_RESPONSE_HEAD: {
        WorkerEvent worker_event;
        worker_event.type = WorkerEvent::Type::kResponseHead;
        worker_event.request_id = event.request_id;
        worker_event.status = static_cast<std::uint16_t>(event.status);
        std::size_t count = 0;
        if (capsid_response_header_count(&event, &count) == CAPSID_OK) {
            worker_event.headers.reserve(count);
            for (std::size_t index = 0; index < count; ++index) {
                capsid_header header;
                if (capsid_response_header_at(&event, index, &header) !=
                    CAPSID_OK) {
                    continue;
                }
                worker_event.headers.emplace_back(
                    std::string(reinterpret_cast<const char*>(
                                    header.name.data),
                                header.name.size),
                    std::string(reinterpret_cast<const char*>(
                                    header.value.data),
                                header.value.size));
            }
        }
        queue_worker_event(std::move(worker_event));
        return true;
    }
    case CAPSID_EVENT_RESPONSE_BODY: {
        WorkerEvent worker_event;
        worker_event.type = WorkerEvent::Type::kResponseBody;
        worker_event.request_id = event.request_id;
        worker_event.body.assign(event.payload.data,
                                 event.payload.data + event.payload.size);
        queue_worker_event(std::move(worker_event));
        return true;
    }
    case CAPSID_EVENT_RESPONSE_END: {
        // The Runtime erased this request when it sent RESPONSE_END, so any
        // command still queued for it (a response-credit grant most likely)
        // would be rejected by the Runtime ABI as an invalid frame and logged
        // as an internal state error. Mark the request terminal here, on the
        // worker thread, before the event is handed to the io thread: the
        // drop check in execute_command then covers the window until the io
        // thread's kResponseEnd handler inserts the same tombstone and the
        // kCancel marker removes it again.
        {
            std::unique_lock<std::mutex> lock(mutex_);
            canceled_.insert(event.request_id);
        }
        WorkerEvent worker_event;
        worker_event.type = WorkerEvent::Type::kResponseEnd;
        worker_event.request_id = event.request_id;
        queue_worker_event(std::move(worker_event));
        return true;
    }
    case CAPSID_EVENT_LOG: {
        WorkerEvent worker_event;
        worker_event.type = WorkerEvent::Type::kLog;
        worker_event.text.assign(
            reinterpret_cast<const char*>(event.payload.data),
            event.payload.size);
        queue_worker_event(std::move(worker_event));
        return true;
    }
    case CAPSID_EVENT_ERROR: {
        WorkerEvent worker_event;
        worker_event.type = WorkerEvent::Type::kError;
        worker_event.request_id = event.request_id;
        worker_event.text.assign(
            reinterpret_cast<const char*>(event.payload.data),
            event.payload.size);
        queue_worker_event(std::move(worker_event));
        return true;
    }
    case CAPSID_EVENT_REQUEST_TIMEOUT: {
        // Mark the request cancelled on the worker thread before the timeout
        // event is delivered: the worker has already erased the request, so
        // any late command for it must be dropped before it reaches the IPC
        // channel or the worker rejects it as an invalid frame.
        {
            std::unique_lock<std::mutex> lock(mutex_);
            canceled_.insert(event.request_id);
        }
        WorkerEvent worker_event;
        worker_event.type = WorkerEvent::Type::kRequestTimeout;
        worker_event.request_id = event.request_id;
        queue_worker_event(std::move(worker_event));
        return true;
    }
    case CAPSID_EVENT_EXIT:
        queue_worker_exit_event();
        return false;
    default:
        // AUDIT and MEMORY_METRICS are not part of the M1A data plane.
        return true;
    }
}

void Impl::queue_worker_event(WorkerEvent event) {
    bool need_post = false;
    {
        // The event-queue size must be read under the mutex: this function
        // runs on both the io thread and the worker thread, and the other
        // side may be mutating events_ concurrently.
        std::unique_lock<std::mutex> lock(mutex_);
        if (metrics_enabled_) {
            metrics_.events_queued.fetch_add(1, std::memory_order_relaxed);
            {
                const size_t size = events_.size() + 1;
                size_t hw = metrics_.event_queue_high_water.load(
                    std::memory_order_relaxed);
                while (hw < size &&
                       !metrics_.event_queue_high_water.compare_exchange_weak(
                           hw, size, std::memory_order_relaxed)) {
                }
            }
        }
        need_post = events_.empty();
        events_.push_back(std::move(event));
    }
    // Only post when the queue was empty — pump_events drains everything
    // in one call. New events that arrive while pump_events is running
    // will see the (now-empty) queue and schedule their own post. The
    // weak capture guarantees a late post can never keep the Impl alive:
    // once the facade stopped and released it, a stale event is dropped.
    if (need_post) {
        io_post([weak = weak_from_this()] {
            if (const std::shared_ptr<Impl> alive = weak.lock()) {
                alive->pump_events();
            }
        });
    }
}

void Impl::queue_worker_exit_event() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!exit_event_queued_) {
        WorkerEvent exit_event;
        exit_event.type = WorkerEvent::Type::kExit;
        events_.push_back(std::move(exit_event));
        exit_event_queued_ = true;
        // The exit event is always the last one, so a post is needed even if
        // the queue was non-empty — pump_events may already be running on
        // earlier queued events and won't see this one without a post. The
        // weak capture mirrors queue_worker_event: a post that outlives the
        // facade's stop is dropped, never a self-retaining handler.
        io_post([weak = weak_from_this()] {
            if (const std::shared_ptr<Impl> alive = weak.lock()) {
                alive->pump_events();
            }
        });
    }
}

// Stops the worker thread on the failure paths that run after it started:
// the shutdown command wakes the blocking wait, and the bounded terminate
// backstop in the worker loop forces the blocking destroy to finish
// promptly. Callers return without leaving a joinable thread behind.
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
    submit_command(std::move(grant));
}

void Impl::shutdown_worker_and_join() {
    // Shares the exactly-once Runtime shutdown gate with request_stop,
    // SIGTERM and the destructor: a start-failure path can never race a
    // concurrent stop into a double shutdown frame.
    Command shutdown;
    shutdown.type = CommandType::kShutdown;
    if (!shutdown_sent_.exchange(true)) {
        submit_command(std::move(shutdown));
    }
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
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
        session->send_simple(http::status::bad_gateway, reason, keep_alive,
                             version);
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
    // Delta snapshot: exchange() reads and zeroes each host counter in one
    // atomic step, so a line is the delta since the previous line and a
    // concurrent increment (worker thread) is never torn or lost. The
    // client-side snapshot is a delta for the same reason.
#define CAPSID_METRIC_EXCHANGE(field) \
    const auto field = \
        metrics_.field.exchange(0, std::memory_order_relaxed)
    CAPSID_METRIC_EXCHANGE(commands_submitted);
    CAPSID_METRIC_EXCHANGE(command_batches);
    CAPSID_METRIC_EXCHANGE(commands_executed);
    CAPSID_METRIC_EXCHANGE(flush_calls);
    CAPSID_METRIC_EXCHANGE(flush_eagain);
    CAPSID_METRIC_EXCHANGE(events_queued);
    CAPSID_METRIC_EXCHANGE(asio_posts);
    CAPSID_METRIC_EXCHANGE(response_heads);
    CAPSID_METRIC_EXCHANGE(response_body_frames);
    CAPSID_METRIC_EXCHANGE(response_ends);
    CAPSID_METRIC_EXCHANGE(grant_commands);
    CAPSID_METRIC_EXCHANGE(credit_bytes_granted);
    CAPSID_METRIC_EXCHANGE(credit_stall_count);
    CAPSID_METRIC_EXCHANGE(command_queue_high_water);
    CAPSID_METRIC_EXCHANGE(event_queue_high_water);
#undef CAPSID_METRIC_EXCHANGE

    capsid::ClientIpcMetrics client_metrics;
    client_ipc_metrics_snapshot(worker_, &client_metrics);
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
        command_queue_high_water,
        event_queue_high_water,
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
        write_stderr(std::string("capsid-host: ") + error);
        return 1;
    }
    if (!impl_->wait(&error)) {
        write_stderr(std::string("capsid-host: ") + error);
        return 1;
    }
    return 0;
}

}  // namespace capsid::host
