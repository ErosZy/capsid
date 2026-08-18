// ManagedListener implementation — see managed_listener.h. One io thread
// runs a Boost.Beast acceptor and every client session; worker responses
// arrive through the §9.2 event sink (pool pump thread → WorkerEventMailbox
// → io thread). The request/response state machine mirrors
// single_worker_server.cc (the M1A data-plane reference), keyed per
// (executor, request_id) because different executors have independent
// request-id spaces.

#include "host/managed_listener.h"

#include "host/listener_cors.h"
#include "host/request_normalization.h"
#include "host/response_body_batch.h"
#include "host/structured_log.h"
#include "host/worker_executor.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace capsid::host {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

// Defined at capsid::host scope after the anonymous namespace (the M1A
// server pattern); the event-bridge types below only hold the pointer.
class ListenerSession;

namespace {

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

// RFC 7230 field-value byte: HTAB / SP / VCHAR / obs-text.
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

// The cross-thread bridge for worker events (§9.2). The pool's pump thread
// (the sole drainer of every executor's event queue) posts events here; the
// listener io thread drains them and dispatches into the session layer.
//
// The mailbox OWNS the io_context and lives as long as BOTH the listener
// (its ManagedListenerImpl holds a shared_ptr) and the pool (the event sink and the
// client-bytes loader hold shared_ptrs). The io_context is therefore
// destroyed only when the pump thread is joined and the io thread is
// joined — a post can never race its destruction.
//
// Lock-order rule: post() takes mutex_ and posts onto the io_context while
// still holding it. The pool invokes the sink under the pool mutex, so the
// io thread must NEVER take a lock the pool can hold while holding mutex_
// (it drains with a swap under mutex_ and dispatches AFTER releasing, so
// pick_worker/submit inside a dispatch never nest with mutex_).
class WorkerEventMailbox : public std::enable_shared_from_this<WorkerEventMailbox> {
public:
    asio::io_context ioc;
    // One-time wiring: the io-side dispatch target. Must be called before
    // the io thread starts — the drain reads it without the mutex, and
    // post() reads it under the mutex, so it never changes afterwards.
    void set_dispatcher(
        std::function<void(const WorkerExecutor*, WorkerEvent)> dispatcher) {
        std::lock_guard<std::mutex> lock(mutex_);
        dispatcher_ = std::move(dispatcher);
    }

    void post(const WorkerExecutor* executor, WorkerEvent event) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || !dispatcher_) {
            return;  // fail closed: no listener side to serve the event
        }
        events_.emplace_back(executor, std::move(event));
        // Post under mutex_: serialized against close()/destruction, so a
        // post can never race the io_context.
        asio::post(ioc, [weak = weak_from_this()] {
            if (std::shared_ptr<WorkerEventMailbox> mailbox = weak.lock()) {
                mailbox->drain();
            }
        });
    }

    // §8.2 "bytes awaiting client consumption" per executor, for the pool's
    // pick_worker loader. Written by the io thread (write submissions and
    // completions), read by the pump thread under the pool mutex; guarded
    // by mutex_.
    void add_client_bytes(const WorkerExecutor* executor, std::size_t bytes) {
        if (bytes == 0) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        client_bytes_[executor] += bytes;
    }

    void sub_client_bytes(const WorkerExecutor* executor, std::size_t bytes) {
        if (bytes == 0) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = client_bytes_.find(executor);
        if (it == client_bytes_.end()) {
            return;
        }
        if (it->second <= bytes) {
            client_bytes_.erase(it);
        } else {
            it->second -= bytes;
        }
    }

    std::uint64_t load_of(const WorkerExecutor* executor) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = client_bytes_.find(executor);
        return it == client_bytes_.end() ? 0 : it->second;
    }

    // Drops future posts (the listener is stopping). Existing queued events
    // are drained by the io thread and dispatched to an expiring listener —
    // their sessions are being closed anyway.
    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }

private:
    // Set once via set_dispatcher() before the io thread starts; read by
    // the drain and by post() (under mutex_).
    std::function<void(const WorkerExecutor*, WorkerEvent)> dispatcher_;

    void drain() {
        std::deque<std::pair<const WorkerExecutor*, WorkerEvent>> local;
        {
            // Swap + dispatch AFTER release (lock-order rule in the class
            // comment): the dispatcher can route through pick_worker, which
            // takes the pool mutex.
            std::lock_guard<std::mutex> lock(mutex_);
            local.swap(events_);
        }
        for (auto& [executor, event] : local) {
            if (dispatcher_) {
                dispatcher_(executor, std::move(event));
            }
        }
    }

    mutable std::mutex mutex_;
    bool closed_ = false;
    std::deque<std::pair<const WorkerExecutor*, WorkerEvent>> events_;
    std::map<const WorkerExecutor*, std::uint64_t> client_bytes_;
};

// Early-credit window (host, spec §8.1): response credit is returned at
// receive time while the per-request HTTP write queue is below this;
// beyond it, credit falls back to write-completion, so a slow client
// cannot make the host buffer unboundedly. Mirrored from
// single_worker_server.cc.
static constexpr std::size_t kEarlyCreditWindow = 64u * 1024u;

// Request state owned by the io thread only. Mirrors the M1A
// single_worker_server PendingRequest minus the SSE/E-1 members (deferred
// follow-up PRs).
struct PendingRequest {
    const WorkerExecutor* executor = nullptr;
    std::shared_ptr<ListenerSession> session;
    std::string method;
    bool keep_alive = false;
    unsigned version = 11;

    // Request direction.
    std::string request_body;
    std::size_t request_body_offset = 0;
    std::uint64_t request_credit = 0;
    bool request_ended = false;

    // Response direction. Shared ownership keeps the serializer alive
    // through in-flight async writes after the request is erased.
    std::shared_ptr<http::response<http::buffer_body>> response;
    std::shared_ptr<http::response_serializer<http::buffer_body>> serializer;
    // Complete responses advertised by the bounded fixed-body protocol are
    // accumulated to their exact <=4 KiB size and emitted in one HTTP write.
    std::shared_ptr<http::response<
        http::vector_body<std::uint8_t>>> fixed_response;
    std::shared_ptr<http::response_serializer<
        http::vector_body<std::uint8_t>>> fixed_serializer;
    std::size_t fixed_body_expected = 0;
    std::size_t fixed_body_received = 0;
    bool fixed_write_started = false;
    std::vector<std::uint8_t> outgoing;
    // The response-credit protocol (spec §8.1, mirrored from
    // single_worker_server): each queued block remembers whether its
    // credit was returned at receive time (early window) or must be
    // returned on the write completion.
    struct QueuedBody {
        std::vector<std::uint8_t> bytes;
        bool credit_returned_early = false;
    };
    std::deque<QueuedBody> body_queue;
    std::size_t body_queue_bytes = 0;  // client-bytes accounting
    std::uint32_t pending_response_credit = 0;
    bool outgoing_credit_returned_early = false;
    bool head_sent = false;
    bool head_only = false;
    bool writing = false;
    bool end_seen = false;
    bool cl_known = false;
    std::size_t cl_remaining = 0;
    // stream_idle_timeout_ms deadline around one socket write (E-3 §9.2).
    std::optional<asio::steady_timer> write_timer;
};

}  // namespace

// One client connection. Owns the socket, the parser and the read
// deadline; the request state lives in ManagedListenerImpl::requests_.
class ListenerSession : public std::enable_shared_from_this<ListenerSession> {
    friend class ManagedListenerImpl;

public:
    ListenerSession(std::shared_ptr<ManagedListenerImpl> impl, tcp::socket socket);

    void start();
    void close();
    bool closed() const { return closed_; }

    beast::tcp_stream& stream() { return stream_; }

    // Sends a small synchronous response and returns to reading (or closes
    // when keep_alive is false).
    void send_simple(http::status status,
                     std::string_view body,
                     bool keep_alive,
                     unsigned version);

private:
    void read_head();
    void read_body();
    void handle_request(http::request<http::string_body> request);
    // Listener-level CORS: answers a browser preflight (returns false) or
    // records the matched origin for response stamping (returns true).
    bool prepare_cors(const http::request<http::string_body>& request);
    void send_cors_preflight(const http::request<http::string_body>& request);
    void arm_read_timer(bool head_phase);
    void on_read_timeout();
    void start_disconnect_probe();

    std::shared_ptr<ManagedListenerImpl> impl_;
    beast::tcp_stream stream_;
    beast::flat_buffer buffer_;
    std::shared_ptr<http::request_parser<http::string_body>> parser_;
    // The current in-flight request (for the disconnect probe and close).
    std::optional<const WorkerExecutor*> current_executor_;
    std::optional<std::uint64_t> current_request_id_;
    asio::steady_timer read_timer_;
    // The shared listener-CORS decision engine, engaged only while the
    // listener owns CORS (config.configured). One request is in flight per
    // session at a time, so a single per-session instance is exact.
    std::optional<ListenerCors> cors_;
    bool closed_ = false;
    bool probe_active_ = false;
};

// The request/response state machine and the io thread.
class ManagedListenerImpl : public std::enable_shared_from_this<ManagedListenerImpl> {
    friend class ListenerSession;
    // The facade needs mailbox_ (wire_pool) and stop_requested_ (running).
    friend class ManagedListener;

public:
    explicit ManagedListenerImpl(ManagedListenerOptions options)
        : options_(std::move(options)),
          mailbox_(std::make_shared<WorkerEventMailbox>()),
          max_request_body_bytes_(options_.max_request_body_bytes) {}

    ~ManagedListenerImpl() {
        request_stop();
        if (io_thread_.joinable()) {
            io_thread_.join();
        }
    }

    bool start(std::string* error);
    void request_stop();
    bool wait(std::string* error);
    std::uint16_t bound_port() const { return bound_port_; }

private:
    // ---- io-thread helpers ----------------------------------------------

    void io_loop();
    void do_accept();
    void stop_from_io();
    void drop_session(const std::shared_ptr<ListenerSession>& session);
    void begin_request(const WorkerExecutor* executor, std::uint64_t request_id,
                       const std::shared_ptr<ListenerSession>& session,
                       const http::request<http::string_body>& request,
                       const NormalizedPublicRequest& normalized);
    void handle_worker_event(const WorkerExecutor* executor,
                             WorkerEvent event);
    void advance_request_body(const WorkerExecutor* executor,
                              std::uint64_t request_id);
    void write_body_block(const WorkerExecutor* executor,
                          std::uint64_t request_id,
                          std::vector<std::uint8_t> bytes,
                          bool credit_returned_early);
    void flush_pending_credit(const WorkerExecutor* executor,
                              std::uint64_t request_id,
                              PendingRequest& pending);
    void write_fixed_response(const WorkerExecutor* executor,
                              std::uint64_t request_id);
    void write_end_block(const WorkerExecutor* executor,
                         std::uint64_t request_id);
    void finalize_request(const WorkerExecutor* executor,
                          std::uint64_t request_id,
                          std::shared_ptr<ListenerSession> session);
    void cancel_request(const WorkerExecutor* executor,
                        std::uint64_t request_id);
    void fail_request(const WorkerExecutor* executor, std::uint64_t request_id,
                      std::shared_ptr<ListenerSession> session);
    void reject_response_head(const WorkerExecutor* executor,
                              std::uint64_t request_id,
                              const std::string& reason);
    bool sanitize_response_headers(
        std::vector<std::pair<std::string, std::string>>* headers);
    void arm_write_timer(std::uint64_t request_id, PendingRequest& pending);
    void disarm_write_timer(PendingRequest& pending);

    ManagedListenerOptions options_;
    RequestRoutingPolicy policy_;
    std::shared_ptr<WorkerEventMailbox> mailbox_;
    const std::uint64_t max_request_body_bytes_;
    std::optional<tcp::acceptor> acceptor_;
    std::map<std::uint64_t, std::shared_ptr<ListenerSession>> sessions_;
    std::map<std::pair<const WorkerExecutor*, std::uint64_t>, PendingRequest>
        requests_;
    std::map<const WorkerExecutor*, std::uint64_t> next_request_id_;
    std::uint64_t connections_ = 0;
    std::uint16_t bound_port_ = 0;
    std::atomic<bool> stop_requested_ = false;
    std::atomic<bool> started_ = false;
    std::thread io_thread_;
    std::once_flag wait_once_;
};

ListenerSession::ListenerSession(std::shared_ptr<ManagedListenerImpl> impl, tcp::socket socket)
    : impl_(std::move(impl)),
      stream_(std::move(socket)),
      read_timer_(impl_->mailbox_->ioc) {
    // TCP_NODELAY on by default (single-connection latency; benchmark
    // parity with the M1A server).
    const char* nodelay = std::getenv("CAPSID_TCP_NODELAY");
    if (nodelay != nullptr && std::strcmp(nodelay, "0") == 0) {
        write_stderr("capsid-host: TCP_NODELAY disabled");
    } else {
        beast::error_code ec;
        stream_.socket().set_option(asio::ip::tcp::no_delay(true), ec);
    }
}

void ListenerSession::start() { read_head(); }

void ListenerSession::read_head() {
    current_executor_.reset();
    current_request_id_.reset();
    parser_ = std::make_shared<http::request_parser<http::string_body>>();
    parser_->body_limit(impl_->max_request_body_bytes_);
    const std::uint64_t header_bytes = impl_->options_.config.limits.header_bytes;
    if (header_bytes != 0) {
        parser_->header_limit(header_bytes);
    }
    arm_read_timer(true);
    http::async_read_header(
        stream_, buffer_, *parser_,
        [self = shared_from_this(),
         parser = parser_](beast::error_code ec, std::size_t) {
            self->read_timer_.cancel();
            if (ec) {
                if (ec == http::error::header_limit) {
                    self->send_simple(http::status::request_header_fields_too_large,
                                      "request header limit exceeded", false,
                                      11);
                } else if (ec != asio::error::operation_aborted) {
                    self->close();
                }
                return;
            }
            self->read_body();
        });
}

void ListenerSession::read_body() {
    arm_read_timer(false);
    http::async_read(
        stream_, buffer_, *parser_,
        [self = shared_from_this(),
         parser = parser_](beast::error_code ec, std::size_t) {
            self->read_timer_.cancel();
            if (ec) {
                if (ec == http::error::body_limit) {
                    self->send_simple(http::status::payload_too_large,
                                      "request body limit exceeded", false,
                                      11);
                } else if (ec != asio::error::operation_aborted) {
                    self->close();
                }
                return;
            }
            self->handle_request(parser->release());
        });
}

void ListenerSession::handle_request(
    http::request<http::string_body> request) {
    // Listener-level CORS runs before routing: a browser preflight can
    // never carry header-routing's control header (the custom header is
    // exactly what the preflight asks about), so the listener answers it
    // itself; every other request records whether its Origin is allowed
    // so both response paths stamp the matching ACAO.
    if (impl_->options_.config.cors.configured &&
        !prepare_cors(request)) {
        return;  // preflight answered (allowed or rejected)
    }
    RequestRoutingPolicy policy = impl_->policy_;
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
    // One atomic snapshot load; the pool found in it is pinned for the
    // whole request (§9.2). A retired App keeps its route as a tombstone
    // (routed name, no pool): it is permanently gone, so the router
    // answers 404 (§9.6-6), distinct from the 503 reserved for Apps that
    // were never routed.
    const std::shared_ptr<const RoutingSnapshot> snapshot =
        impl_->options_.routing->load();
    const bool retired = snapshot != nullptr &&
                         snapshot->retired(normalized.request.application);
    std::shared_ptr<GenerationPool> pool =
        snapshot ? snapshot->find(normalized.request.application) : nullptr;
    if (retired) {
        send_simple(http::status::not_found, "app retired",
                    request.keep_alive(), request.version());
        return;
    }
    if (!pool) {
        send_simple(http::status::service_unavailable, "app not found",
                    request.keep_alive(), request.version());
        return;
    }
    WorkerExecutor* executor = pool->pick_worker();
    if (!executor) {
        send_simple(http::status::service_unavailable, "worker unavailable",
                    request.keep_alive(), request.version());
        return;
    }
    // The generation's per-worker inflight ceiling (0 = no cap): the pool
    // schedules least-loaded, but admission is this listener's.
    const std::uint64_t max_inflight = pool->max_inflight_per_worker();
    if (max_inflight != 0 && executor->inflight() >= max_inflight) {
        send_simple(http::status::service_unavailable, "worker at capacity",
                    request.keep_alive(), request.version());
        return;
    }
    std::uint64_t& next = impl_->next_request_id_[executor];
    const std::uint64_t request_id = ++next;
    current_executor_ = executor;
    current_request_id_ = request_id;
    impl_->begin_request(executor, request_id, shared_from_this(), request,
                         normalized.request);
    start_disconnect_probe();
}

bool ListenerSession::prepare_cors(
    const http::request<http::string_body>& request) {
    cors_.emplace(impl_->options_.config.cors);
    const CorsDecision decision = cors_->prepare(request);
    if (decision == CorsDecision::kBadRequest) {
        send_simple(http::status::bad_request, "duplicate Origin header",
                    request.keep_alive(), request.version());
        return false;
    }
    if (decision == CorsDecision::kPreflightAllowed ||
        decision == CorsDecision::kPreflightRejected) {
        send_cors_preflight(request);
        return false;
    }
    return true;
}

void ListenerSession::send_cors_preflight(
    const http::request<http::string_body>& request) {
    auto response = std::make_shared<http::response<http::string_body>>();
    response->version(request.version());
    response->keep_alive(request.keep_alive());
    cors_->build_preflight(*response);
    response->prepare_payload();
    http::async_write(
        stream_, *response,
        [self = shared_from_this(), response, request](
            beast::error_code ec, std::size_t) {
            (void)response;
            (void)request;
            if (!ec && request.keep_alive() && !self->closed_) {
                self->read_head();
            } else {
                self->close();
            }
        });
}

void ListenerSession::arm_read_timer(bool head_phase) {
    const std::uint64_t ms = head_phase
                                 ? impl_->options_.config.limits.header_timeout_ms
                                 : impl_->options_.config.limits
                                       .body_idle_timeout_ms;
    if (ms == 0) {
        return;
    }
    read_timer_.expires_after(std::chrono::milliseconds(ms));
    read_timer_.async_wait(
        [self = shared_from_this()](const beast::error_code ec) {
            if (ec) {
                return;  // cancelled — the read finished first
            }
            self->on_read_timeout();
        });
}

// v1 read deadline: close the connection (408 synthesis is deferred to the
// follow-up listener PR). close() cancels the pending read; its completion
// handler sees operation_aborted and returns.
void ListenerSession::on_read_timeout() { close(); }

// While a request is in flight the Host keeps an async_wait on the socket
// so an abortive peer disconnect (RST) cancels the worker request
// immediately (§9.3) instead of waiting for the worker deadline. Mirrors
// the M1A probe.
void ListenerSession::start_disconnect_probe() {
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
            // request finished and its bytes belong to the next parse):
            // leave the data in place and re-arm from the next
            // handle_request.
            if (!self->current_request_id_.has_value()) {
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
            if (peeked <= 0) {
                self->close();
            } else {
                // Pipelined data while a request is in flight: v1 forbids
                // pipelining (§8.3) — close.
                self->close();
            }
        });
}

void ListenerSession::close() {
    if (closed_) {
        return;
    }
    closed_ = true;
    read_timer_.cancel();
    if (current_request_id_) {
        impl_->cancel_request(*current_executor_, *current_request_id_);
        current_request_id_.reset();
    }
    beast::error_code ec;
    stream_.socket().shutdown(tcp::socket::shutdown_both, ec);
    stream_.close();
    impl_->drop_session(shared_from_this());
}

void ListenerSession::send_simple(http::status status,
                                  std::string_view body,
                                  bool keep_alive,
                                  unsigned version) {
    auto response = std::make_shared<http::response<http::string_body>>();
    response->result(status);
    response->version(version);
    response->keep_alive(keep_alive);
    response->set(http::field::content_type, "text/plain");
    if (cors_) {
        cors_->stamp(*response);
    }
    response->body() = std::string(body);
    response->prepare_payload();
    http::async_write(
        stream_, *response,
        [self = shared_from_this(), response, keep_alive](
            beast::error_code ec, std::size_t) {
            (void)response;
            if (!ec && keep_alive && !self->closed_) {
                self->read_head();
            } else {
                self->close();
            }
        });
}

// --------------------------------------------------------------------------
// ManagedListenerImpl
// --------------------------------------------------------------------------

bool ManagedListenerImpl::start(std::string* error) {
    if (started_.exchange(true)) {
        if (error != nullptr) {
            *error = "listener already started";
        }
        return false;
    }
    if (stop_requested_.load(std::memory_order_acquire)) {
        if (error != nullptr) {
            *error = "listener stop was requested before start";
        }
        return false;
    }
    if (!options_.routing) {
        if (error != nullptr) {
            *error = "listener requires a routing table";
        }
        return false;
    }

    // Compile + validate the routing policy BEFORE binding: a header-mode
    // listener that is not trusted fails closed at startup (§3.7/§8.1), and
    // a malformed authority never serves a single request.
    const std::string& mode = options_.config.routing.mode;
    if (mode == "path") {
        policy_.mode = RequestRoutingMode::kPath;
    } else if (mode == "subdomain") {
        policy_.mode = RequestRoutingMode::kSubdomain;
    } else if (mode == "header") {
        policy_.mode = RequestRoutingMode::kHeader;
    } else {
        if (error != nullptr) {
            *error = "listener routing mode must be path, subdomain or header";
        }
        return false;
    }
    policy_.public_scheme = options_.config.public_scheme;
    policy_.subdomain_suffix = options_.config.routing.suffix;
    policy_.public_authority = options_.config.public_authority;
    policy_.trusted_header_routing = options_.config.trusted;
    RequestNormalizationError policy_error;
    if (!is_valid_routing_policy(policy_, &policy_error)) {
        if (error != nullptr) {
            *error = "listener " + options_.config.name +
                     ": invalid routing policy: " + policy_error.message;
        }
        return false;
    }

    // config.tcp is "host" or "host:port" (bracketed IPv6 is deferred and
    // fails closed here — the last-':' split cannot address it).
    const std::string& tcp = options_.config.tcp;
    const std::string::size_type colon = tcp.rfind(':');
    const std::string host =
        colon == std::string::npos ? tcp : tcp.substr(0, colon);
    const std::string port_text =
        colon == std::string::npos ? "" : tcp.substr(colon + 1);
    std::uint16_t port = 80;  // "host" alone means the default HTTP port
    if (!port_text.empty()) {
        bool valid = !port_text.empty();
        std::uint32_t value = 0;
        for (const char c : port_text) {
            if (c < '0' || c > '9') {
                valid = false;
                break;
            }
            value = value * 10 + static_cast<std::uint32_t>(c - '0');
            if (value > 65535) {
                valid = false;
                break;
            }
        }
        if (!valid) {
            if (error != nullptr) {
                *error = "listener " + options_.config.name +
                         ": invalid tcp port '" + port_text + "'";
            }
            return false;
        }
        port = static_cast<std::uint16_t>(value);
    }
    if (host.empty()) {
        if (error != nullptr) {
            *error = "listener " + options_.config.name + ": empty tcp host";
        }
        return false;
    }

    beast::error_code ec;
    const tcp::endpoint endpoint(asio::ip::make_address(host, ec), port);
    if (ec) {
        if (error != nullptr) {
            *error = "listener " + options_.config.name +
                     ": invalid tcp address '" + host + "'";
        }
        return false;
    }
    acceptor_.emplace(mailbox_->ioc);
    acceptor_->open(endpoint.protocol(), ec);
    if (ec) {
        if (error != nullptr) {
            *error = "listener " + options_.config.name +
                     ": open failed: " + ec.message();
        }
        return false;
    }
    acceptor_->set_option(asio::socket_base::reuse_address(true), ec);
    if (ec) {
        if (error != nullptr) {
            *error = "listener " + options_.config.name +
                     ": option failed: " + ec.message();
        }
        return false;
    }
    acceptor_->bind(endpoint, ec);
    if (ec) {
        if (error != nullptr) {
            *error = "listener " + options_.config.name +
                     ": bind failed: " + ec.message();
        }
        return false;
    }
    acceptor_->listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        if (error != nullptr) {
            *error = "listener " + options_.config.name +
                     ": listen failed: " + ec.message();
        }
        return false;
    }
    bound_port_ = acceptor_->local_endpoint(ec).port();
    if (ec) {
        if (error != nullptr) {
            *error = "listener " + options_.config.name +
                     ": local endpoint failed: " + ec.message();
        }
        return false;
    }

    // Wire the event bridge (§9.2): every pool in the current snapshot
    // forwards its events into our mailbox, whose dispatcher routes them
    // into this io thread. Both wirings precede the io thread (and thus
    // the first accept), so a request can never pin a worker before its
    // response path exists. The sink swap is pool-mutex-guarded and the
    // dispatcher is set under the mailbox mutex, so neither wiring can be
    // observed half-made.
    if (const std::shared_ptr<const RoutingSnapshot> snapshot =
            options_.routing->load()) {
        for (const auto& [application, pool] : snapshot->routes()) {
            (void)application;
            pool->set_event_sink(
                [mailbox = mailbox_](const WorkerExecutor* executor,
                                     WorkerEvent event) {
                    mailbox->post(executor, std::move(event));
                });
        }
    }
    mailbox_->set_dispatcher(
        [weak = weak_from_this()](const WorkerExecutor* executor,
                                  WorkerEvent event) {
            if (std::shared_ptr<ManagedListenerImpl> self = weak.lock()) {
                self->handle_worker_event(executor, std::move(event));
            }
        });

    io_thread_ = std::thread([self = shared_from_this()] { self->io_loop(); });
    return true;
}

void ManagedListenerImpl::io_loop() {
    do_accept();
    mailbox_->ioc.run();
}

void ManagedListenerImpl::do_accept() {
    if (!acceptor_) {
        return;
    }
    acceptor_->async_accept(
        [self = shared_from_this()](beast::error_code ec, tcp::socket socket) {
            if (ec) {
                if (ec != asio::error::operation_aborted) {
                    self->do_accept();
                }
                return;
            }
            // §9.2 connection ceiling: an excess connection is closed
            // without service.
            const std::uint64_t ceiling =
                self->options_.config.limits.connections;
            if (ceiling != 0 && self->connections_ >= ceiling) {
                beast::error_code ignored;
                socket.close(ignored);
                self->do_accept();
                return;
            }
            ++self->connections_;
            const std::uint64_t session_id = self->sessions_.size() + 1;
            std::shared_ptr<ListenerSession> session =
                std::make_shared<ListenerSession>(self, std::move(socket));
            self->sessions_[session_id] = session;
            session->start();
            self->do_accept();
        });
}

void ManagedListenerImpl::request_stop() {
    if (stop_requested_.exchange(true)) {
        return;
    }
    // Fail closed at the mailbox: late events drop instead of accumulating.
    mailbox_->close();
    // Wake the io thread; it closes the acceptor + every session, and run()
    // drains naturally. The post goes through the mailbox, which outlives
    // this ManagedListenerImpl (the pool's sink holds it), so the wake is always safe.
    asio::post(mailbox_->ioc, [weak = weak_from_this()] {
        if (std::shared_ptr<ManagedListenerImpl> self = weak.lock()) {
            self->stop_from_io();
        }
    });
}

void ManagedListenerImpl::stop_from_io() {
    acceptor_.reset();  // cancels the pending async_accept
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
}

bool ManagedListenerImpl::wait(std::string* error) {
    request_stop();  // wait() implies the stop request (idempotent)
    std::call_once(wait_once_, [this] {
        if (io_thread_.joinable()) {
            io_thread_.join();
        }
    });
    if (error != nullptr) {
        *error = "listener stopped";
    }
    return true;
}

void ManagedListenerImpl::drop_session(
    const std::shared_ptr<ListenerSession>& session) {
    for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
        if (it->second == session) {
            sessions_.erase(it);
            if (connections_ > 0) {
                --connections_;
            }
            return;
        }
    }
}

void ManagedListenerImpl::begin_request(
    const WorkerExecutor* executor, std::uint64_t request_id,
    const std::shared_ptr<ListenerSession>& session,
    const http::request<http::string_body>& request,
    const NormalizedPublicRequest& normalized) {
    PendingRequest pending;
    pending.executor = executor;
    pending.session = session;
    pending.method = std::string(request.method_string());
    pending.keep_alive = request.keep_alive();
    pending.version = request.version();
    pending.request_body = request.body();
    const bool has_body = !pending.request_body.empty();

    Command begin;
    begin.type = CommandType::kBeginRequest;
    begin.request_id = request_id;
    begin.method = pending.method;
    begin.url = normalized.url;
    begin.headers = normalized.headers;
    begin.end_request = !has_body;
    requests_[{executor, request_id}] = std::move(pending);
    // const: the §9.2 sink signature; submitting commands is the listener's.
    const_cast<WorkerExecutor*>(executor)->submit(std::move(begin));

    if (!has_body) {
        auto it = requests_.find({executor, request_id});
        if (it != requests_.end()) {
            it->second.request_ended = true;
        }
    }
}

void ManagedListenerImpl::handle_worker_event(const WorkerExecutor* executor,
                                                WorkerEvent event) {
    switch (event.type) {
    case WorkerEvent::Type::kRequestCredit: {
        auto it = requests_.find({executor, event.request_id});
        if (it == requests_.end()) {
            return;
        }
        it->second.request_credit += event.credit;
        advance_request_body(executor, event.request_id);
        return;
    }
    case WorkerEvent::Type::kResponseHead: {
        auto it = requests_.find({executor, event.request_id});
        if (it == requests_.end()) {
            return;
        }
        PendingRequest& pending = it->second;
        if (event.status < 200 || event.status > 599) {
            reject_response_head(executor, event.request_id,
                                 "invalid worker response status");
            return;
        }
        // Listener-level CORS is authoritative when configured: the App
        // cannot write its own Access-Control-Allow-Origin (that would
        // bypass the Host allow-list), and credentials survive only for an
        // exact allowed origin. A wildcard or a disallowed/absent Origin
        // strips App-owned credentials so wildcard never becomes any-origin
        // credentialed CORS. Vary is merged token-wise so `Vary:
        // Accept-Encoding` cannot suppress the required `Vary: Origin`.
        if (pending.session && pending.session->cors_) {
            pending.session->cors_->filter_headers(&event.headers);
        }
        if (!sanitize_response_headers(&event.headers)) {
            reject_response_head(executor, event.request_id,
                                 "invalid worker response headers");
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
                        executor, event.request_id,
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
                reject_response_head(executor, event.request_id,
                                     "invalid worker Content-Length");
                return;
            }
            pending.cl_known = true;
            pending.cl_remaining = static_cast<std::size_t>(value);
        }
        pending.serializer = std::make_shared<
            http::response_serializer<http::buffer_body>>(*pending.response);
        pending.writing = true;
        arm_write_timer(event.request_id, pending);
        http::async_write_header(
            pending.session->stream(), *pending.serializer,
            [self = shared_from_this(), response = pending.response,
             serializer = pending.serializer, session = pending.session,
             executor, request_id = event.request_id](beast::error_code ec,
                                                      std::size_t) {
                (void)response;
                (void)serializer;
                (void)session;
                auto it = self->requests_.find({executor, request_id});
                if (it == self->requests_.end()) {
                    return;
                }
                PendingRequest& pending = it->second;
                pending.writing = false;
                self->disarm_write_timer(pending);
                if (ec) {
                    self->fail_request(executor, request_id, pending.session);
                    return;
                }
                pending.head_sent = true;
                if (!pending.body_queue.empty()) {
                    PendingRequest::QueuedBody queued =
                        take_coalesced_response_body(
                            &pending.body_queue,
                            &pending.body_queue_bytes,
                            kResponseBodyWriteBatchLimit);
                    self->write_body_block(executor, request_id,
                                           std::move(queued.bytes),
                                           queued.credit_returned_early);
                } else if (pending.end_seen) {
                    if (pending.head_only) {
                        self->finalize_request(executor, request_id,
                                               pending.session);
                    } else {
                        self->write_end_block(executor, request_id);
                    }
                }
            });
        return;
    }
    case WorkerEvent::Type::kResponseBody: {
        auto it = requests_.find({executor, event.request_id});
        if (it == requests_.end()) {
            return;
        }
        PendingRequest& pending = it->second;
        if (pending.fixed_response) {
            if (pending.fixed_body_received >
                    pending.fixed_body_expected ||
                event.body.size() >
                    pending.fixed_body_expected -
                        pending.fixed_body_received) {
                fail_request(executor, event.request_id, pending.session);
                return;
            }
            pending.fixed_body_received += event.body.size();
            if (!pending.head_only) {
                mailbox_->add_client_bytes(executor, event.body.size());
                pending.fixed_response->body().insert(
                    pending.fixed_response->body().end(),
                    event.body.begin(), event.body.end());
            }
            return;
        }
        // Response credit (spec §8.1): return credit as soon as the frame
        // is received, not after the client write completes, while the
        // per-request write queue stays shallow — one fewer host
        // round-trip per frame. Beyond the window credit falls back to
        // write-completion, so a slow client cannot make the host buffer
        // unboundedly. Without any return path the worker exhausts its
        // initial response window (256 KiB) and the transfer stalls
        // forever, which the WP-09 §13.6 soak's /big endpoint exposed.
        const bool credit_returned_early =
            pending.body_queue_bytes < kEarlyCreditWindow;
        if (credit_returned_early) {
            pending.pending_response_credit +=
                static_cast<std::uint32_t>(event.body.size());
            flush_pending_credit(executor, event.request_id, pending);
        }
        if (pending.head_only) {
            // HEAD consumes the worker body without exposing it.
            return;
        }
        // Client-bytes accounting: counted once at arrival (queued or
        // written directly), returned on the write completion.
        mailbox_->add_client_bytes(executor, event.body.size());
        if (pending.writing) {
            pending.body_queue_bytes += event.body.size();
            pending.body_queue.push_back(
                PendingRequest::QueuedBody{std::move(event.body),
                                           credit_returned_early});
            return;
        }
        write_body_block(executor, event.request_id, std::move(event.body),
                         credit_returned_early);
        return;
    }
    case WorkerEvent::Type::kResponseEnd: {
        auto it = requests_.find({executor, event.request_id});
        if (it == requests_.end()) {
            return;
        }
        PendingRequest& pending = it->second;
        pending.end_seen = true;
        // Drain any remaining pending credit; the Runtime erased the
        // request, so the grant frame is queued only to satisfy the
        // terminal retirement (single_worker_server does the same).
        flush_pending_credit(executor, event.request_id, pending);
        // The Runtime erased this request with RESPONSE_END. Purge queued
        // stale frames and retire the tombstone after the worker's current
        // command batch, without a redundant Runtime cancel.
        const_cast<WorkerExecutor*>(executor)->retire_terminal_request(
            event.request_id);
        if (pending.fixed_response) {
            if (pending.fixed_body_received !=
                pending.fixed_body_expected) {
                fail_request(executor, event.request_id, pending.session);
                return;
            }
            write_fixed_response(executor, event.request_id);
            return;
        }
        if (pending.head_only) {
            if (!pending.writing) {
                finalize_request(executor, event.request_id, pending.session);
            }
            return;
        }
        if (!pending.writing) {
            write_end_block(executor, event.request_id);
        }
        return;
    }
    case WorkerEvent::Type::kLog:
        if (options_.log != nullptr) {
            LogFields fields;
            fields.level = event.binding_log ? event.log_level : "info";
            fields.event = log_events::kAppLog;
            fields.app = event.application_id;
            fields.generation = event.generation_digest;
            fields.binding = event.binding_id;
            if (event.request_id != 0) {
                fields.request_id = std::to_string(event.request_id);
            }
            fields.fields = event.log_fields_json;
            fields.message = event.text;
            options_.log->log(LogLane::kApp, std::move(fields));
        } else {
            write_stderr(event.text);
        }
        return;
    case WorkerEvent::Type::kError:
        write_stderr(std::string("capsid-host: worker error: ") + event.text +
                     " (request " + std::to_string(event.request_id) + ")");
        return;
    case WorkerEvent::Type::kRequestTimeout: {
        auto it = requests_.find({executor, event.request_id});
        if (it == requests_.end()) {
            return;
        }
        PendingRequest& pending = it->second;
        const std::shared_ptr<ListenerSession> session = pending.session;
        const bool head_sent = pending.head_sent;
        const bool writing = pending.writing;
        const bool keep_alive = pending.keep_alive;
        const unsigned version = pending.version;
        cancel_request(executor, event.request_id);
        // Once any response byte reached the wire, only a connection close
        // is legal (a fresh response would corrupt the stream).
        if (head_sent || writing) {
            if (session && !session->closed()) {
                session->close();
            }
        } else if (session && !session->closed()) {
            session->send_simple(http::status::gateway_timeout,
                                 "worker request timeout", keep_alive,
                                 version);
        }
        return;
    }
    case WorkerEvent::Type::kRequestFailure: {
        auto it = requests_.find({executor, event.request_id});
        if (it == requests_.end()) {
            return;
        }
        PendingRequest& pending = it->second;
        const std::shared_ptr<ListenerSession> session = pending.session;
        const bool head_sent = pending.head_sent;
        const bool writing = pending.writing;
        const bool keep_alive = pending.keep_alive;
        const unsigned version = pending.version;
        cancel_request(executor, event.request_id);
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
        // A worker died: every request pinned to this executor is gone with
        // it (the executor emits no per-request failure for an exit). The
        // pool handles the replacement — the listener only fails the
        // requests, and keeps accepting: a replacement worker serves the
        // next request.
        const std::vector<std::pair<const WorkerExecutor*, std::uint64_t>>
            pinned = [this, executor] {
                std::vector<std::pair<const WorkerExecutor*, std::uint64_t>>
                    ids;
                for (const auto& entry : requests_) {
                    if (entry.first.first == executor) {
                        ids.push_back(entry.first);
                    }
                }
                return ids;
            }();
        for (const auto& key : pinned) {
            auto it = requests_.find(key);
            if (it == requests_.end()) {
                continue;
            }
            PendingRequest& pending = it->second;
            const std::shared_ptr<ListenerSession> session = pending.session;
            const bool head_sent = pending.head_sent;
            const bool writing = pending.writing;
            const bool keep_alive = pending.keep_alive;
            const unsigned version = pending.version;
            cancel_request(executor, key.second);
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

void ManagedListenerImpl::advance_request_body(const WorkerExecutor* executor,
                                                 std::uint64_t request_id) {
    auto it = requests_.find({executor, request_id});
    if (it == requests_.end()) {
        return;
    }
    PendingRequest& pending = it->second;
    // One write per credit event: the worker replenishes request credit
    // only after consuming the previous chunk, so submitting several chunks
    // at once would race the worker's window and be rejected as an invalid
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
        const_cast<WorkerExecutor*>(executor)->submit(std::move(command));
        pending.request_credit -= chunk;
        pending.request_body_offset += chunk;
    }
    if (pending.request_body_offset == pending.request_body.size() &&
        !pending.request_ended) {
        pending.request_ended = true;
        if (!pending.end_seen) {
            Command command;
            command.type = CommandType::kEndRequest;
            command.request_id = request_id;
            const_cast<WorkerExecutor*>(executor)->submit(std::move(command));
        }
    }
}

void ManagedListenerImpl::write_body_block(
    const WorkerExecutor* executor, std::uint64_t request_id,
    std::vector<std::uint8_t> bytes, bool credit_returned_early) {
    auto it = requests_.find({executor, request_id});
    if (it == requests_.end()) {
        return;
    }
    PendingRequest& pending = it->second;
    if (pending.cl_known) {
        if (bytes.size() > pending.cl_remaining) {
            // The worker produced more body than its declared
            // Content-Length: fail the connection closed.
            fail_request(executor, request_id, pending.session);
            return;
        }
        pending.cl_remaining -= bytes.size();
    }
    pending.outgoing = std::move(bytes);
    pending.outgoing_credit_returned_early = credit_returned_early;
    pending.writing = true;
    pending.response->body().data = pending.outgoing.data();
    pending.response->body().size = pending.outgoing.size();
    pending.response->body().more = true;
    arm_write_timer(request_id, pending);
    http::async_write(
        pending.session->stream(), *pending.serializer,
        [self = shared_from_this(), response = pending.response,
         serializer = pending.serializer, session = pending.session,
         executor, request_id,
         bytes = pending.outgoing.size()](beast::error_code ec, std::size_t) {
            (void)response;
            (void)serializer;
            (void)session;
            // The client-bytes term is returned on the write completion
            // regardless of the request state (a cancel erases the entry
            // but the write still completes).
            self->mailbox_->sub_client_bytes(executor, bytes);
            auto it = self->requests_.find({executor, request_id});
            if (it == self->requests_.end()) {
                return;
            }
            PendingRequest& pending = it->second;
            pending.writing = false;
            self->disarm_write_timer(pending);
            if (ec && ec != http::error::need_buffer) {
                self->fail_request(executor, request_id, pending.session);
                return;
            }
            // need_buffer is the buffer_body serializer's normal signal that
            // the current block was fully written and more data is expected.
            // Credit is returned only after the client write succeeded, for
            // exactly the bytes that were written (early-credited blocks
            // were reimbursed at receive time and must not be doubled).
            // Once the response ended, the Runtime erased the request and
            // the credit is moot; submitting the frame would only be
            // rejected, so it is skipped.
            if (!pending.end_seen && !pending.outgoing_credit_returned_early) {
                pending.pending_response_credit +=
                    static_cast<std::uint32_t>(bytes);
                self->flush_pending_credit(executor, request_id, pending);
            }
            pending.outgoing_credit_returned_early = false;
            pending.outgoing.clear();
            if (!pending.body_queue.empty()) {
                PendingRequest::QueuedBody queued =
                    take_coalesced_response_body(
                        &pending.body_queue,
                        &pending.body_queue_bytes,
                        kResponseBodyWriteBatchLimit);
                self->write_body_block(executor, request_id,
                                       std::move(queued.bytes),
                                       queued.credit_returned_early);
            } else if (pending.end_seen) {
                self->write_end_block(executor, request_id);
            }
        });
}

// Spec §8.1: submit any accumulated response credit as one grant command.
// Immediate grant (no threshold), matching the reference implementation's
// default. The executor drops grants for tombstoned ids, so a stale grant
// raced with RESPONSE_END is harmless.
void ManagedListenerImpl::flush_pending_credit(
    const WorkerExecutor* executor, std::uint64_t request_id,
    PendingRequest& pending) {
    if (pending.pending_response_credit == 0) {
        return;
    }
    Command grant;
    grant.type = CommandType::kGrantResponseCredit;
    grant.request_id = request_id;
    grant.credit = pending.pending_response_credit;
    pending.pending_response_credit = 0;
    const_cast<WorkerExecutor*>(executor)->submit(std::move(grant));
}

void ManagedListenerImpl::write_fixed_response(
    const WorkerExecutor* executor, std::uint64_t request_id) {
    auto it = requests_.find({executor, request_id});
    if (it == requests_.end()) {
        return;
    }
    PendingRequest& pending = it->second;
    pending.fixed_response->content_length(
        pending.fixed_body_expected);
    pending.fixed_serializer = std::make_shared<http::response_serializer<
        http::vector_body<std::uint8_t>>>(*pending.fixed_response);
    pending.fixed_write_started = true;
    pending.writing = true;
    arm_write_timer(request_id, pending);
    const std::size_t bytes =
        pending.head_only ? 0 : pending.fixed_body_received;
    const auto completion =
        [self = shared_from_this(), response = pending.fixed_response,
         serializer = pending.fixed_serializer, session = pending.session,
         executor, request_id, bytes](beast::error_code ec, std::size_t) {
            (void)response;
            (void)serializer;
            (void)session;
            self->mailbox_->sub_client_bytes(executor, bytes);
            auto it = self->requests_.find({executor, request_id});
            if (it == self->requests_.end()) {
                return;
            }
            PendingRequest& pending = it->second;
            pending.writing = false;
            self->disarm_write_timer(pending);
            if (ec) {
                self->fail_request(executor, request_id, pending.session);
                return;
            }
            pending.head_sent = true;
            self->finalize_request(
                executor, request_id, pending.session);
        };
    if (pending.head_only) {
        http::async_write_header(
            pending.session->stream(), *pending.fixed_serializer,
            completion);
    } else {
        http::async_write(
            pending.session->stream(), *pending.fixed_serializer,
            completion);
    }
}

void ManagedListenerImpl::write_end_block(const WorkerExecutor* executor,
                                            std::uint64_t request_id) {
    auto it = requests_.find({executor, request_id});
    if (it == requests_.end()) {
        return;
    }
    PendingRequest& pending = it->second;
    if (pending.cl_known && pending.cl_remaining > 0) {
        // The worker ended the response short of its declared
        // content-length; the client would wait forever.
        fail_request(executor, request_id, pending.session);
        return;
    }
    pending.writing = true;
    pending.response->body().data = nullptr;
    pending.response->body().size = 0;
    pending.response->body().more = false;
    arm_write_timer(request_id, pending);
    http::async_write(
        pending.session->stream(), *pending.serializer,
        [self = shared_from_this(), response = pending.response,
         serializer = pending.serializer, session_ref = pending.session,
         executor, request_id](beast::error_code ec, std::size_t) {
            (void)response;
            (void)serializer;
            (void)session_ref;
            auto it = self->requests_.find({executor, request_id});
            if (it == self->requests_.end()) {
                return;
            }
            PendingRequest& pending = it->second;
            pending.writing = false;
            self->disarm_write_timer(pending);
            if (ec) {
                self->fail_request(executor, request_id, pending.session);
                return;
            }
            const bool keep_alive = pending.keep_alive;
            std::shared_ptr<ListenerSession> session = pending.session;
            self->requests_.erase(it);
            if (session && !session->closed()) {
                if (keep_alive) {
                    session->read_head();
                } else {
                    session->close();
                }
            }
        });
}

void ManagedListenerImpl::finalize_request(
    const WorkerExecutor* executor, std::uint64_t request_id,
    std::shared_ptr<ListenerSession> session) {
    auto it = requests_.find({executor, request_id});
    if (it == requests_.end()) {
        return;
    }
    const bool keep_alive = it->second.keep_alive;
    requests_.erase(it);
    if (session && !session->closed()) {
        if (keep_alive) {
            session->read_head();
        } else {
            session->close();
        }
    }
}

void ManagedListenerImpl::cancel_request(const WorkerExecutor* executor,
                                           std::uint64_t request_id) {
    auto it = requests_.find({executor, request_id});
    if (it == requests_.end()) {
        return;
    }
    // Queued response bytes never reach the wire; return their client-bytes
    // accounting (the in-flight write returns its own on completion).
    mailbox_->sub_client_bytes(executor, it->second.body_queue_bytes);
    if (it->second.fixed_response &&
        !it->second.fixed_write_started &&
        !it->second.head_only) {
        mailbox_->sub_client_bytes(
            executor, it->second.fixed_body_received);
    }
    requests_.erase(it);
    // const: the §9.2 sink signature; tombstones and cancels are the
    // listener's.
    const_cast<WorkerExecutor*>(executor)->mark_canceled(request_id);
    Command cancel;
    cancel.type = CommandType::kCancel;
    cancel.request_id = request_id;
    const_cast<WorkerExecutor*>(executor)->submit(std::move(cancel));
}

void ManagedListenerImpl::fail_request(const WorkerExecutor* executor,
                                         std::uint64_t request_id,
                                         std::shared_ptr<ListenerSession> session) {
    cancel_request(executor, request_id);
    if (session && !session->closed()) {
        session->close();
    }
}

void ManagedListenerImpl::reject_response_head(
    const WorkerExecutor* executor, std::uint64_t request_id,
    const std::string& reason) {
    auto it = requests_.find({executor, request_id});
    if (it == requests_.end()) {
        return;
    }
    const std::shared_ptr<ListenerSession> session = it->second.session;
    const bool keep_alive = it->second.keep_alive;
    const unsigned version = it->second.version;
    cancel_request(executor, request_id);
    if (session && !session->closed()) {
        // Worker/IPC failure before the response head → 503.
        session->send_simple(http::status::service_unavailable, reason,
                             keep_alive, version);
    }
}

bool ManagedListenerImpl::sanitize_response_headers(
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
            if (!collect_connection_nominations(value, &nominated)) {
                return false;
            }
        } else if (lower == "content-length") {
            ++content_length_count;
            if (content_length_count > 1 || value.empty()) {
                return false;
            }
            for (const unsigned char c : value) {
                if (c < '0' || c > '9') {
                    return false;
                }
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

void ManagedListenerImpl::arm_write_timer(std::uint64_t request_id,
                                            PendingRequest& pending) {
    const std::uint64_t ms =
        options_.config.limits.stream_idle_timeout_ms;
    if (ms == 0) {
        return;
    }
    if (!pending.write_timer) {
        pending.write_timer.emplace(mailbox_->ioc);
    }
    pending.write_timer->expires_after(std::chrono::milliseconds(ms));
    pending.write_timer->async_wait(
        [self = shared_from_this(),
         executor = pending.executor,
         request_id](const beast::error_code ec) {
            if (ec) {
                return;  // cancelled by the write completion
            }
            auto it = self->requests_.find({executor, request_id});
            if (it == self->requests_.end()) {
                return;
            }
            PendingRequest& pending = it->second;
            const std::shared_ptr<ListenerSession> session = pending.session;
            // The head reached the wire before this timer was armed, so
            // after the cancel only a connection close is legal.
            self->cancel_request(executor, request_id);
            if (session && !session->closed()) {
                session->close();
            }
        });
}

void ManagedListenerImpl::disarm_write_timer(PendingRequest& pending) {
    if (pending.write_timer) {
        pending.write_timer->cancel();
    }
}

// --------------------------------------------------------------------------
// Facade
// --------------------------------------------------------------------------

ManagedListener::ManagedListener(ManagedListenerOptions options)
    : impl_(std::make_shared<ManagedListenerImpl>(std::move(options))) {}

ManagedListener::~ManagedListener() {
    impl_->request_stop();
    std::string error;
    impl_->wait(&error);
}

bool ManagedListener::start(std::string* error) { return impl_->start(error); }

void ManagedListener::wire_pool(const std::shared_ptr<GenerationPool>& pool) {
    if (!pool) {
        return;
    }
    // The mailbox is shared: the pool's sink captures it and keeps it
    // alive as long as the pool lives, so a post can never race the io
    // thread's teardown (the sink drops events once the mailbox is
    // closed). Precondition (caller): the pool has no in-flight requests.
    pool->set_event_sink(
        [mailbox = impl_->mailbox_](const WorkerExecutor* executor,
                                    WorkerEvent event) {
            mailbox->post(executor, std::move(event));
        });
}

void ManagedListener::request_stop() { impl_->request_stop(); }

bool ManagedListener::wait(std::string* error) { return impl_->wait(error); }

std::uint16_t ManagedListener::bound_port() const {
    return impl_->bound_port();
}

bool ManagedListener::running() const {
    return !impl_->stop_requested_.load(std::memory_order_relaxed);
}

}  // namespace capsid::host
