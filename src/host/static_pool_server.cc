// M2 fixed-pool Host data plane. See static_pool_server.h.

// Socket release detaches IOCP ownership (Windows 8.1+). The documented
// Windows runtime baseline is Windows 10; expose that API before Asio is read.
#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0A00
#endif

#include "host/static_pool_server.h"

#include "host/config.h"
#include "host/local_capsid_policy.h"
#include "host/listener_cors.h"
#include "host/static_pool.h"
#include "host/structured_log.h"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <signal.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace capsid::host {

namespace {

// A failed fixed pool still owns its public endpoint. These small, bounded
// exchanges run only on the accept reactor, independently of exited shards.
// Read a complete HTTP head before replying and never retain a request body.
class UnavailableSession : public std::enable_shared_from_this<UnavailableSession> {
  public:
    UnavailableSession(boost::asio::ip::tcp::socket peer, const SingleWorkerServerOptions &options,
                       std::function<void(UnavailableSession *)> finished)
        : stream_(std::move(peer)), cors_config_(options.cors),
          read_timeout_(std::chrono::milliseconds(bounded_timeout(options.request_timeout_ms))),
          write_timeout_(std::chrono::milliseconds(bounded_timeout(options.write_timeout_ms))),
          finished_(std::move(finished)) {
        parser_.header_limit(8192);
        parser_.body_limit(boost::none);
    }

    void run() {
        stream_.expires_after(read_timeout_);
        boost::beast::http::async_read_header(
            stream_, buffer_, parser_,
            [self = shared_from_this()](boost::system::error_code ec, std::size_t) {
                if (ec || self->closed_)
                    self->close();
                else
                    self->reply();
            });
    }

    void close() {
        if (closed_)
            return;
        closed_ = true;
        boost::system::error_code ignored;
        stream_.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
        stream_.socket().close(ignored);
        auto finished = std::move(finished_);
        if (finished)
            finished(this);
    }

  private:
    static std::uint64_t bounded_timeout(std::uint64_t configured) {
        return configured == 0 || configured > 5000 ? 5000 : configured;
    }

    void reply() {
        namespace http = boost::beast::http;
        const auto &request = parser_.get();
        response_.version(request.version());
        response_.result(http::status::service_unavailable);
        response_.set(http::field::content_type, "text/plain");
        response_.body() = "worker unavailable";
        if (cors_config_.configured) {
            ListenerCors cors(cors_config_);
            const auto decision = cors.prepare(request);
            if (decision == CorsDecision::kBadRequest) {
                response_.result(http::status::bad_request);
                response_.body() = "duplicate Origin header";
            } else if (decision == CorsDecision::kPreflightAllowed ||
                       decision == CorsDecision::kPreflightRejected) {
                cors.build_preflight(response_);
            } else {
                cors.stamp(response_);
            }
        }
        response_.keep_alive(false);
        response_.prepare_payload();
        stream_.expires_after(write_timeout_);
        const auto done = [self = shared_from_this()](boost::system::error_code, std::size_t) {
            self->close();
        };
        if (request.method() == http::verb::head) {
            serializer_.emplace(response_);
            http::async_write_header(stream_, *serializer_, done);
        } else {
            http::async_write(stream_, response_, done);
        }
    }

    boost::beast::tcp_stream stream_;
    boost::beast::flat_buffer buffer_{8192};
    boost::beast::http::request_parser<boost::beast::http::string_body> parser_;
    boost::beast::http::response<boost::beast::http::string_body> response_;
    std::optional<boost::beast::http::response_serializer<boost::beast::http::string_body>>
        serializer_;
    ListenerCorsConfig cors_config_;
    std::chrono::milliseconds read_timeout_;
    std::chrono::milliseconds write_timeout_;
    std::function<void(UnavailableSession *)> finished_;
    bool closed_ = false;
};

// Moving a socket preserves its executor. Transfer native ownership before
// starting IO whenever a pending accept's owner differs from its final owner.
bool rehome_peer(boost::asio::ip::tcp::socket &peer, boost::asio::any_io_executor executor) {
    if (peer.get_executor() == executor)
        return true;
    boost::system::error_code ec;
    const auto endpoint = peer.local_endpoint(ec);
    if (ec)
        return false;
    boost::asio::ip::tcp::socket rebound(std::move(executor));
    const auto native = peer.release(ec);
    if (ec)
        return false; // peer retains ownership on release failure
    rebound.assign(endpoint.protocol(), native, ec);
    if (ec) {
#if defined(_WIN32)
        ::closesocket(native);
#else
        ::close(native);
#endif
        return false;
    }
    peer = std::move(rebound);
    return true;
}

#if defined(_WIN32)
// Console control events (CTRL_C / CTRL_BREAK / console close) are the
// Windows stand-in for the SIGTERM/SIGINT gate in run(): the
// handler only sets the stop event, never touches C++ objects.
HANDLE g_static_pool_term_event = nullptr;

BOOL WINAPI static_pool_console_handler(DWORD) {
    if (g_static_pool_term_event != nullptr) {
        SetEvent(g_static_pool_term_event);
    }
    return TRUE;  // handled: the process stops on its own schedule
}
#endif

// M2 item 7 (design §12.2): single write path for pool control events.
// Null log (fixtures without the process-wide instance) is a no-op.
void emit_log(StructuredLog* log, LogFields fields) {
    if (log != nullptr) {
        log->log(LogLane::kControl, std::move(fields));
    }
}

}  // namespace

class StaticPoolServerImpl : public std::enable_shared_from_this<StaticPoolServerImpl> {
public:
    explicit StaticPoolServerImpl(StaticPoolServerOptions options)
        : options_(std::move(options)),
          state_(options_.workers) {
        auto &worker=options_.worker_options;
        if(!worker.channel_service_budget) worker.channel_service_budget=std::make_shared<ServiceChannelBudget>(worker.channel_service_limits);
        if(!worker.channel_listener_budget) worker.channel_listener_budget=std::make_shared<ServiceChannelBudget>(worker.channel_listener_limits,worker.channel_service_budget);
    }

    StructuredLog* structured_log() const { return options_.log; }

    ~StaticPoolServerImpl() {
        // A running pool is torn down with the same bounded stop the
        // facade performs: request_stop is idempotent and wait joins every
        // shard, so destruction never terminates or leaks.
        request_stop();
        std::string error;
        wait(&error);
    }

    bool start(const std::vector<std::uint8_t>& bundle, std::string* error) {
        if (start_gate_.exchange(true)) {
            if (error != nullptr) {
                *error = "static pool already started";
            }
            return false;
        }
        struct StartCompletion {
            StaticPoolServerImpl* impl;
            ~StartCompletion() { impl->finish_start(); }
        } completion{this};
        if (stop_requested_.load(std::memory_order_acquire)) {
            if (error != nullptr) {
                *error = "static pool stop was requested before start";
            }
            return false;
        }
        if (options_.workers == 0) {
            if (error != nullptr) {
                *error = "static pool requires at least one worker";
            }
            return false;
        }
        if (options_.workers > kMaxStaticPoolWorkers) {
            if (error != nullptr) {
                *error = "static pool worker limit exceeded (maximum " +
                         std::to_string(kMaxStaticPoolWorkers) + ")";
            }
            return false;
        }
        // Compile the local App policy and its Binding set once for the
        // whole fixed pool. Every shard gets the same immutable snapshot;
        // neither capsid.json nor bindingsRoot can drift between workers.
        std::shared_ptr<const LocalCapsidPolicy> prepared_policy =
            options_.worker_options.prepared_local_policy;
        if (!prepared_policy) {
            auto loaded_policy = std::make_shared<LocalCapsidPolicy>();
            std::string policy_error;
            if (!load_local_capsid_policy(
                    options_.worker_options.capsid_json_path,
                    options_.worker_options.capsid_json_required,
                    options_.worker_options.binding_registry.get(),
                    options_.worker_options.secrets_root,
                    loaded_policy.get(), &policy_error)) {
                if (error != nullptr) {
                    *error = policy_error;
                }
                return false;
            }
            prepared_policy = std::move(loaded_policy);
        }
        // TSan gate (PR-06): reserve writes the vector's begin/end pair,
        // which request_stop() reads under shards_mutex_ — a concurrent
        // stop during start would race the lockless reallocation. Every
        // shards_ mutation holds the same mutex as every read.
        try {
            std::lock_guard<std::mutex> lock(shards_mutex_);
            shards_.reserve(options_.workers);
        } catch (const std::bad_alloc&) {
            if (error != nullptr) {
                *error = "static pool worker allocation failed";
            }
            return false;
        } catch (const std::length_error&) {
            if (error != nullptr) {
                *error = "static pool worker allocation exceeds container limit";
            }
            return false;
        }
        // StaticPoolState wiring (E-4): register each worker under its
        // immutable owner shard BEFORE it is spawned, mark READY the
        // instant start() returns (barrier-mode start() completes
        // spawn/load/READY/listen synchronously), and activate the state
        // machine only when every target worker is READY. Every state
        // machine write happens on this control thread while the pool is
        // still starting; after activation the pool is fixed
        // (minReady == maxWorkers) and the state is read-only — queueing
        // and load selection (later M2 batches) must define their own
        // cross-thread access before reading it from reactor threads.
        // The pool owns the public endpoint on every platform, including
        // while no Worker is READY. Shards own only sessions and Workers.
        if (!bind_pool_acceptor(error)) {
            return false;
        }
        for (std::uint32_t index = 0; index < options_.workers; ++index) {
            if (!state_.register_starting(index, index)) {
                rollback(shards_.size(), error);
                return false;
            }
            SingleWorkerServerOptions shard_options = options_.worker_options;
            shard_options.prepared_local_policy = prepared_policy;
            shard_options.ready_fd = -1;           // pool owns the READY record
            shard_options.write_ready_record = false;  // pool-level READY
            shard_options.install_process_signals = false;  // pool owns signals
            // One pool-owned acceptor, no per-shard listener: every shard
            // receives connections through adopt_socket().
            shard_options.external_accept = true;
            shard_options.defer_accept = false;
            shard_options.so_reuseport = false;
            // E-1 admission (§10.3): pool-level options are forwarded into
            // every shard (a non-zero pool field overrides the shard
            // template; 0 = not set). In the v1 single-App pool the
            // Host-global gate is exactly the per-shard gate: the Host
            // budget is the shard budget times the shard count, so no
            // pool-level counter is needed (a cross-shard budget would
            // require shared atomics and is deferred until the
            // queue/load-selection batch defines its cross-thread access).
            if (options_.max_inflight_per_worker != 0) {
                shard_options.max_inflight_per_worker =
                    options_.max_inflight_per_worker;
            }
            if (options_.queue_requests != 0) {
                shard_options.queue_requests = options_.queue_requests;
            }
            if (options_.queue_header_bytes != 0) {
                shard_options.queue_header_bytes =
                    options_.queue_header_bytes;
            }
            if (options_.queue_timeout_ms != 0) {
                shard_options.queue_timeout_ms = options_.queue_timeout_ms;
            }
            // E-2 SSE permit (§9.3): a non-zero pool-level field overrides
            // the shard template the same way; 0 = not set keeps the shard
            // defaults (2 slots, 60s idle).
            if (options_.max_streaming_inflight_per_worker != 0) {
                shard_options.max_streaming_inflight_per_worker =
                    options_.max_streaming_inflight_per_worker;
            }
            if (options_.stream_idle_timeout_ms != 0) {
                shard_options.stream_idle_timeout_ms =
                    options_.stream_idle_timeout_ms;
            }
            // E-3 slow-client write deadline (§9.2): forwarded the same way;
            // 0 = not set keeps the shard template value.
            if (options_.write_timeout_ms != 0) {
                shard_options.write_timeout_ms = options_.write_timeout_ms;
            }
            std::unique_ptr<SingleWorkerServer> shard(
                new SingleWorkerServer(std::move(shard_options)));
            {
                std::lock_guard<std::mutex> lock(shards_mutex_);
                starting_shard_ = shard.get();
            }
            std::string shard_error;
            bool shard_started = false;
            try {
                shard_started = shard->start(bundle, &shard_error);
            } catch (...) {
                std::lock_guard<std::mutex> lock(shards_mutex_);
                if (starting_shard_ == shard.get()) {
                    starting_shard_ = nullptr;
                }
                throw;
            }
            {
                std::lock_guard<std::mutex> lock(shards_mutex_);
                if (starting_shard_ == shard.get()) {
                    starting_shard_ = nullptr;
                }
            }
            if (!shard_started) {
                // Preserve the shard's own diagnostic; rollback only fills
                // in a generic message when nothing was reported.
                if (error != nullptr) {
                    *error = shard_error.empty()
                        ? "static pool shard failed to start"
                        : shard_error;
                }
                rollback(shards_.size(), error);
                return false;
            }
            if (!state_.mark_ready(index)) {
                rollback(shards_.size(), error);
                return false;
            }
            {
                std::lock_guard<std::mutex> lock(shards_mutex_);
                shards_.push_back(std::move(shard));
            }
            if (stop_requested_.load(std::memory_order_acquire)) {
                rollback(shards_.size(), error);
                return false;
            }
        }
        // Activation gate: every target worker registered and READY.
        // Nothing is armed before this holds.
        if (!state_.can_activate() || !state_.activate()) {
            rollback(shards_.size(), error);
            return false;
        }
        // Arm the sole public accept loop only after every shard is READY.
        if (!start_pool_accept_loop()) {
            rollback(shards_.size(), error);
            return false;
        }
        // M2 item 6: a pool-level healthCheck gates the pool READY record.
        // The probe runs through the now-accepting pool endpoint (shared
        // listener or pool acceptor), using the same routing-aware target an
        // external client would build.
        const SingleWorkerServerOptions& worker_options =
            options_.worker_options;
        if (worker_options.health_check.configured) {
            std::string probe_target = worker_options.health_check.path;
            std::string probe_host = worker_options.public_authority;
            std::string probe_capsid_app;
            switch (worker_options.routing_mode) {
            case RequestRoutingMode::kPath:
                probe_target = "/@capsid/" + worker_options.application +
                               worker_options.health_check.path;
                break;
            case RequestRoutingMode::kSubdomain:
                probe_host = worker_options.application +
                             worker_options.routing_suffix;
                break;
            case RequestRoutingMode::kHeader:
                probe_capsid_app = worker_options.application;
                break;
            }
            std::string probe_error;
            const bool probe_ok = probe_local_health(
                worker_options.listen_address,
                pool_port_,
                probe_target, probe_host, probe_capsid_app,
                worker_options.health_check.timeout_ms, &probe_error);
            if (!probe_ok) {
                if (error != nullptr) {
                    *error = "health check failed: " + probe_error;
                }
                rollback(shards_.size(), error);
                return false;
            }
        }
        if (stop_requested_.load(std::memory_order_acquire)) {
            rollback(shards_.size(), error);
            return false;
        }
        // Pool-level READY: exactly one canonical record, only after every
        // shard is READY AND accepting. A failed publication rolls the
        // whole pool back.
        if (!write_pool_ready_line()) {
            rollback(shards_.size(), error);
            return false;
        }
        return true;
    }

    void request_stop() {
        if (stop_requested_.exchange(true)) {
            return;  // idempotent
        }
        close_pool_acceptor();
        // Stop every shard in one pass, then wait() joins them: the
        // bounded per-shard shutdown windows run in parallel instead of
        // serially.
        std::lock_guard<std::mutex> lock(shards_mutex_);
        if (starting_shard_ != nullptr) {
            starting_shard_->request_stop();
        }
        for (const std::unique_ptr<SingleWorkerServer>& shard : shards_) {
            shard->request_stop();
        }
    }

    bool stop_requested() const {
        return stop_requested_.load(std::memory_order_acquire);
    }

    bool wait(std::string* error) {
        // Concurrent-wait guard (TSan gate): wait() joins every shard's
        // threads, and two threads calling it at once would double-join
        // (UB). call_once makes the FIRST caller run the joins while every
        // concurrent and later caller BLOCKS until they complete — a
        // concurrent caller never returns before the pool is actually
        // stopped, and a repeated caller observes the already-waited pool.
        {
            std::unique_lock<std::mutex> lock(lifecycle_mutex_);
            if (!start_gate_.load(std::memory_order_acquire)) {
                if (error != nullptr) {
                    *error = "static pool was not started";
                }
                return false;
            }
            lifecycle_cv_.wait(lock, [this] { return start_finished_; });
        }
        std::call_once(wait_once_, [this] {
            stop_pool_acceptor();
            for (const std::unique_ptr<SingleWorkerServer>& shard : shards_) {
                std::string shard_error;
                shard->wait(&shard_error);
            }
        });
        if (error != nullptr) {
            *error = "static pool stopped";
        }
        return true;
    }

    // Live capacity: the number of shards whose worker is currently
    // available (READY and not exited/stopped). Never a cached startup
    // count — a worker fault drops this to N−1 immediately once its exit
    // event reaches the reactor.
    std::size_t active_workers() const {
        std::size_t count = 0;
        std::lock_guard<std::mutex> lock(shards_mutex_);
        for (const std::unique_ptr<SingleWorkerServer>& shard : shards_) {
            if (shard->worker_available()) {
                ++count;
            }
        }
        return count;
    }

    std::vector<capsid::WorkerExitStatus> worker_exit_statuses() const {
        std::lock_guard<std::mutex> lock(shards_mutex_);
        std::vector<capsid::WorkerExitStatus> statuses;
        statuses.reserve(shards_.size());
        for (const auto& shard : shards_) {
            statuses.push_back(shard->worker_exit_status());
        }
        return statuses;
    }

    // M2 §7.5: the old generation's pool begins draining — every shard
    // stops accepting and drains in parallel, each shutting down when its
    // own inflight clears or its deadline forces cancellation.
    void begin_drain(std::uint64_t deadline_ms) {
        close_pool_acceptor();
        for (const std::unique_ptr<SingleWorkerServer>& shard : shards_) {
            shard->begin_drain(deadline_ms);
        }
    }

    // §7.5 row 7: the pool-wide report. The drain spans from the first
    // shard's begin to the LAST shard's completion (max total); forced
    // cancellations sum across shards.
    SingleWorkerServer::DrainMetrics drain_metrics() const {
        SingleWorkerServer::DrainMetrics metrics;
        metrics.draining = true;
        metrics.finished = true;
        for (const std::unique_ptr<SingleWorkerServer>& shard : shards_) {
            const SingleWorkerServer::DrainMetrics shard_metrics =
                shard->drain_metrics();
            metrics.draining =
                metrics.draining && shard_metrics.draining;
            metrics.finished =
                metrics.finished && shard_metrics.finished;
            metrics.forced_cancellations +=
                shard_metrics.forced_cancellations;
            if (shard_metrics.total_ms > metrics.total_ms) {
                metrics.total_ms = shard_metrics.total_ms;
            }
        }
        if (shards_.empty()) {
            metrics.draining = false;
            metrics.finished = false;
        }
        return metrics;
    }

private:
    // Every platform keeps one public acceptor independent of Worker exit.
    bool bind_pool_acceptor(std::string* error);
    bool start_pool_accept_loop();
    void stop_pool_acceptor();
    void close_pool_acceptor();
    void do_pool_accept();
    void dispatch_pool_peer(SingleWorkerServer* target, boost::asio::ip::tcp::socket peer);
    void reject_pool_peer(boost::asio::ip::tcp::socket peer);
    void close_accept_resources();

    void finish_start() {
        {
            std::lock_guard<std::mutex> lock(lifecycle_mutex_);
            start_finished_ = true;
        }
        lifecycle_cv_.notify_all();
    }

    // Stops and waits exactly the first `started` shards (the ones that
    // successfully started) and clears the pool state: atomic rollback of
    // a partial pool startup. The failed shard itself already unwound its
    // own listener and worker inside its start().
    void rollback(std::size_t started, std::string* error) {
        // The pool-owned acceptor must not survive a rolled-back start:
        // the port has to be released before start() returns.
        stop_pool_acceptor();
        for (std::size_t index = 0; index < started; ++index) {
            shards_[index]->request_stop();
        }
        for (std::size_t index = 0; index < started; ++index) {
            std::string shard_error;
            shards_[index]->wait(&shard_error);
        }
        if (error != nullptr && error->empty()) {
            *error = "static pool startup failed";
        }
    }

    bool write_pool_ready_line() {
        if (options_.worker_options.ready_fd < 0) {
            return true;  // no caller to notify (zero-worker fixture path)
        }
        const std::string line =
            "{\"schema\":\"capsid-host-ready-v1\",\"app\":\"" +
            options_.worker_options.application + "\",\"address\":\"" +
            options_.worker_options.listen_address + "\",\"port\":" +
            std::to_string(pool_port_) +
            "}\n";
        std::size_t offset = 0;
        while (offset < line.size()) {
#if defined(_WIN32)
            // The READY channel may be a loopback socketpair (in-process
            // harnesses) or a stdio pipe (process harnesses); write_any_fd
            // covers both, unlike raw CRT write() on a socket fd.
            const ssize_t written = capsid::win32::write_any_fd(
                options_.worker_options.ready_fd, line.data() + offset,
                line.size() - offset);
#else
            const ssize_t written = ::write(
                options_.worker_options.ready_fd, line.data() + offset,
                line.size() - offset);
#endif
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                return false;
            }
            offset += static_cast<std::size_t>(written);
        }
        return true;
    }

    StaticPoolServerOptions options_;
    // Fixed-pool activation state machine: written only by start() on the
    // control thread, read-only afterwards (see the wiring comment in
    // start()). Owns the register/READY/activate contract that gates the
    // pool-level READY record.
    StaticPoolState state_;
    std::atomic<bool> start_gate_ = false;
    std::atomic<bool> stop_requested_ = false;
    std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_cv_;
    bool start_finished_ = false;
    mutable std::mutex shards_mutex_;
    SingleWorkerServer* starting_shard_ = nullptr;
    std::once_flag wait_once_;
    std::vector<std::unique_ptr<SingleWorkerServer>> shards_;
    boost::asio::io_context accept_ioc_;
    std::optional<boost::asio::ip::tcp::acceptor> pool_acceptor_;
    std::thread accept_thread_;
    std::mutex accept_lifecycle_mutex_;
    bool accept_started_ = false; // control-thread operations hold the mutex
    std::atomic<bool> accept_closed_ = false;
    std::uint16_t pool_port_ = 0;
    std::size_t next_shard_ = 0;
    std::unordered_map<UnavailableSession*, std::shared_ptr<UnavailableSession>> unavailable_;
};

bool StaticPoolServerImpl::bind_pool_acceptor(std::string *error) {
    std::lock_guard<std::mutex> lock(accept_lifecycle_mutex_);
    if (stop_requested_.load(std::memory_order_acquire) ||
        accept_closed_.load(std::memory_order_acquire)) {
        if (error != nullptr)
            *error = "static pool stopped before listener bind";
        return false;
    }
    namespace asio = boost::asio;
    using tcp = asio::ip::tcp;
    boost::system::error_code ec;
    tcp::endpoint endpoint;
    endpoint.address(asio::ip::make_address(options_.worker_options.listen_address, ec));
    if (ec) {
        if (error != nullptr) {
            *error = "invalid static-pool listen address";
        }
        return false;
    }
    endpoint.port(options_.worker_options.listen_port);
    pool_acceptor_.emplace(accept_ioc_);
    pool_acceptor_->open(endpoint.protocol(), ec);
    if (!ec) {
        pool_acceptor_->set_option(asio::socket_base::reuse_address(true), ec);
    }
    if (!ec) {
        pool_acceptor_->bind(endpoint, ec);
    }
    if (!ec) {
        pool_acceptor_->listen(asio::socket_base::max_listen_connections, ec);
    }
    if (!ec) {
        const tcp::endpoint local = pool_acceptor_->local_endpoint(ec);
        if (!ec) {
            pool_port_ = local.port();
        }
    }
    if (ec) {
        pool_acceptor_.reset();
        if (error != nullptr) {
            *error = "static-pool shared listener failed: " + ec.message();
        }
        return false;
    }
    return true;
}

bool StaticPoolServerImpl::start_pool_accept_loop() {
    std::lock_guard<std::mutex> lock(accept_lifecycle_mutex_);
    if (accept_closed_.load(std::memory_order_acquire) ||
        stop_requested_.load(std::memory_order_acquire))
        return false;
    do_pool_accept();
    accept_thread_ = std::thread([this] { accept_ioc_.run(); });
    accept_started_ = true;
    return true;
}

void StaticPoolServerImpl::close_pool_acceptor() {
    std::lock_guard<std::mutex> lock(accept_lifecycle_mutex_);
    if (accept_closed_.exchange(true, std::memory_order_acq_rel))
        return;
    if (!accept_started_)
        close_accept_resources();
    else
        boost::asio::post(accept_ioc_, [this] { close_accept_resources(); });
}

void StaticPoolServerImpl::close_accept_resources() {
    if (pool_acceptor_) {
        boost::system::error_code ignored;
        pool_acceptor_->close(ignored);
    }
    while (!unavailable_.empty()) {
        const auto it = unavailable_.begin();
        const auto session = it->second;
        unavailable_.erase(it);
        session->close();
    }
}

void StaticPoolServerImpl::stop_pool_acceptor() {
    close_pool_acceptor();
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    std::lock_guard<std::mutex> lock(accept_lifecycle_mutex_);
    // Also covers a reactor that finished after a fatal accept error.
    close_accept_resources();
    if (pool_acceptor_) {
        boost::system::error_code ignored;
        pool_acceptor_->close(ignored);
        pool_acceptor_.reset();
    }
    accept_started_ = false;
}

void StaticPoolServerImpl::do_pool_accept() {
    if (!pool_acceptor_ || !pool_acceptor_->is_open() ||
        accept_closed_.load(std::memory_order_acquire) ||
        stop_requested_.load(std::memory_order_acquire)) {
        return;
    }
    // The accepted peer must be BORN on the target shard's reactor: a
    // moved socket keeps its accepting executor, so a peer created on
    // the pool's accept io_context would run its network callbacks on
    // the accept thread while the shard's Worker events mutate the same
    // session state from the shard thread (the Windows heap-corruption
    // regression). The shard is therefore selected BEFORE the accept is
    // armed and the socket is created directly on its executor.
    // Completion rechecks the selected shard. If its worker exited while
    // accept was pending, dispatch_pool_peer can rebind the unused socket
    // to a healthy shard; a plain move would retain the dead reactor.
    // The facade keeps Impl alive through stop/wait, and shards are only
    // destroyed after the accept thread joins. The handler's weak capture
    // avoids a lifetime cycle; the raw shard outlives its armed operation.
    SingleWorkerServer *target = nullptr;
    {
        std::lock_guard<std::mutex> lock(shards_mutex_);
        const std::size_t count = shards_.size();
        for (std::size_t probe = 0; probe < count; ++probe) {
            const std::size_t index = next_shard_++ % count;
            SingleWorkerServer *candidate = shards_[index].get();
            if (candidate->worker_available()) {
                target = candidate;
                break;
            }
        }
    }
    const boost::asio::any_io_executor executor =
        target ? target->accept_executor() : accept_ioc_.get_executor();
    pool_acceptor_->async_accept(
        executor, [weak = weak_from_this(), target](boost::system::error_code ec,
                                                    boost::asio::ip::tcp::socket peer) {
            const std::shared_ptr<StaticPoolServerImpl> self = weak.lock();
            if (!self || !self->pool_acceptor_) {
                return;
            }
            if (!ec && !self->accept_closed_.load(std::memory_order_acquire) &&
                !self->stop_requested_.load(std::memory_order_acquire)) {
                self->dispatch_pool_peer(target, std::move(peer));
            }
            if (ec && ec != boost::asio::error::operation_aborted) {
                // A transient accept error would stop the only listener;
                // treat it like a bind failure and stop the pool.
                self->close_pool_acceptor();
                return;
            }
            if (peer.is_open()) {
                boost::system::error_code ignored;
                peer.close(ignored);
            }
            self->do_pool_accept();
        });
}

void StaticPoolServerImpl::dispatch_pool_peer(SingleWorkerServer *target,
                                              boost::asio::ip::tcp::socket peer) {
    // Availability can change after selection, including while the shard
    // prepares its adoption handler. Only a successful admission consumes
    // the peer. Each failed attempt remains before any request IO and may
    // choose the next shard; a fixed pool cannot try more than N owners.
    for (std::uint32_t attempt = 0; attempt < options_.workers; ++attempt) {
        if (accept_closed_.load(std::memory_order_acquire) ||
            stop_requested_.load(std::memory_order_acquire))
            return;
        if (target == nullptr || !target->worker_available()) {
            target = nullptr;
            std::lock_guard<std::mutex> lock(shards_mutex_);
            const std::size_t count = shards_.size();
            for (std::size_t probe = 0; probe < count; ++probe) {
                SingleWorkerServer *candidate = shards_[next_shard_++ % count].get();
                if (candidate->worker_available()) {
                    target = candidate;
                    break;
                }
            }
        }
        if (target == nullptr)
            break;
        // No request IO has started. Detach the native handle from the
        // old IOCP and register it on the replacement reactor before any
        // Session can use it. Moving an Asio socket alone cannot do this.
        if (!rehome_peer(peer, target->accept_executor()))
            return;
        if (target->try_adopt_socket(peer))
            return;
        target = nullptr;
    }
    if (!accept_closed_.load(std::memory_order_acquire) &&
        !stop_requested_.load(std::memory_order_acquire))
        reject_pool_peer(std::move(peer));
}

void StaticPoolServerImpl::reject_pool_peer(boost::asio::ip::tcp::socket peer) {
    // Bound memory even if many peers stop midway through an HTTP head.
    // Excess connections are closed; admitted exchanges finish or time out.
    if (unavailable_.size() >= 128 || !rehome_peer(peer, accept_ioc_.get_executor()))
        return;
    auto session = std::make_shared<UnavailableSession>(
        std::move(peer), options_.worker_options,
        [weak = weak_from_this()](UnavailableSession *finished) {
            if (const auto self = weak.lock())
                self->unavailable_.erase(finished);
        });
    unavailable_.emplace(session.get(), session);
    session->run();
}

StaticPoolServer::StaticPoolServer(StaticPoolServerOptions options)
    : impl_(std::make_shared<StaticPoolServerImpl>(std::move(options))) {}

StaticPoolServer::~StaticPoolServer() {
    // The facade owns the Impl and stops it while still holding the
    // reference: the same bounded teardown as the single-worker facade.
    impl_->request_stop();
    std::string error;
    impl_->wait(&error);
    impl_.reset();
}

bool StaticPoolServer::start(const std::vector<std::uint8_t>& bundle,
                             std::string* error) {
    return impl_->start(bundle, error);
}

void StaticPoolServer::request_stop() { impl_->request_stop(); }

void StaticPoolServer::begin_drain(std::uint64_t deadline_ms) {
    impl_->begin_drain(deadline_ms);
}

SingleWorkerServer::DrainMetrics StaticPoolServer::drain_metrics() const {
    return impl_->drain_metrics();
}

std::vector<capsid::WorkerExitStatus> StaticPoolServer::worker_exit_statuses() const {
    return impl_->worker_exit_statuses();
}

bool StaticPoolServer::wait(std::string* error) { return impl_->wait(error); }

std::size_t StaticPoolServer::active_workers() const {
    return impl_->active_workers();
}

int StaticPoolServer::run(const std::vector<std::uint8_t>& bundle) {
    std::string error;
#if !defined(_WIN32)
    // Install the process signal handler before starting any shards. Asio
    // dispatches it on this thread; parent control uses the same stop gate.
    boost::asio::io_context signal_io;
    boost::asio::signal_set signals(signal_io, SIGTERM, SIGINT);
    signals.async_wait([this](const boost::system::error_code& ec, int) {
        if (!ec) impl_->request_stop();
    });
#endif
    if (!impl_->start(bundle, &error)) {
        // M2 item 7 (§12.2): a failed pool start is a structured startup
        // line; the error text is static and sanitized.
        emit_log(impl_->structured_log(),
                 {.event = log_events::kStartup,
                  .result = "fail",
                  .message = error});
        return 1;
    }
    // Both parent control and process signals can request a bounded stop.
    // Windows console callbacks only set an event, never call C++ objects.
#if defined(_WIN32)
    g_static_pool_term_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_static_pool_term_event == nullptr ||
        SetConsoleCtrlHandler(
            &static_pool_console_handler, TRUE) == 0) {
        emit_log(impl_->structured_log(),
                 {.event = log_events::kStartup,
                  .result = "fail",
                  .message = "cannot install stop handler"});
        impl_->request_stop();
        impl_->wait(&error);
        return 1;
    }
    bool wait_failed = false;
    while (!impl_->stop_requested()) {
        const DWORD result = WaitForSingleObject(g_static_pool_term_event, 50);
        if (result == WAIT_OBJECT_0) break;
        if (result != WAIT_TIMEOUT) { wait_failed = true; break; }
    }
#else
    while (!impl_->stop_requested())
        signal_io.run_for(std::chrono::milliseconds(50));
    signals.cancel();
#endif
    impl_->request_stop();
    impl_->wait(&error);
#if defined(_WIN32)
    if (wait_failed) {
        emit_log(impl_->structured_log(),
                 {.event = log_events::kShutdown, .result = "fail",
                  .message = "cannot wait for stop event"});
        return 1;
    }
#endif
    return 0;
}

}  // namespace capsid::host
