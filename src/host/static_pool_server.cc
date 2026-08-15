// M2 fixed-pool Host data plane. See static_pool_server.h.

#include "host/static_pool_server.h"

#include "host/static_pool.h"
#include "host/structured_log.h"

#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <signal.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace capsid::host {

namespace {

#if defined(_WIN32)
// Console control events (CTRL_C / CTRL_BREAK / console close) are the
// Windows stand-in for the SIGTERM/SIGINT sigwait gate in run(): the
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

class StaticPoolServerImpl {
public:
    explicit StaticPoolServerImpl(StaticPoolServerOptions options)
        : options_(std::move(options)),
          state_(options_.workers) {}

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
        // TSan gate (PR-06): reserve writes the vector's begin/end pair,
        // which request_stop() reads under shards_mutex_ — a concurrent
        // stop during start would race the lockless reallocation. Every
        // shards_ mutation holds the same mutex as every read.
        {
            std::lock_guard<std::mutex> lock(shards_mutex_);
            shards_.reserve(options_.workers);
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
        // The first shard binds the port (kernel-assigned when 0); every
        // later shard reuses the same address:port through SO_REUSEPORT.
        std::uint16_t shared_port = 0;
        for (std::uint32_t index = 0; index < options_.workers; ++index) {
            if (!state_.register_starting(index, index)) {
                rollback(shards_.size(), error);
                return false;
            }
            SingleWorkerServerOptions shard_options = options_.worker_options;
            shard_options.ready_fd = -1;           // pool owns the READY record
            shard_options.write_ready_record = false;  // pool-level READY
            shard_options.install_process_signals = false;  // pool owns signals
            shard_options.so_reuseport = true;     // one shared port
            shard_options.defer_accept = true;     // pool-wide activation barrier
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
            if (index > 0) {
                shard_options.listen_port = shared_port;
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
                rollback(shards_.size(), error);
                return false;
            }
            if (!state_.mark_ready(index)) {
                rollback(shards_.size(), error);
                return false;
            }
            if (index == 0) {
                shared_port = shard->bound_port();
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
        // Pool-wide activation barrier: every shard is prepared (worker
        // READY + listener bound) but NOT accepting yet. Only now are the
        // accept loops armed one by one — a client must never observe a
        // partially READY pool serving HTTP.
        for (const std::unique_ptr<SingleWorkerServer>& shard : shards_) {
            std::string activation_error;
            if (!shard->activate_accept(&activation_error)) {
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

    // M2 §7.5: the old generation's pool begins draining — every shard
    // stops accepting and drains in parallel, each shutting down when its
    // own inflight clears or its deadline forces cancellation.
    void begin_drain(std::uint64_t deadline_ms) {
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
        for (std::size_t index = 0; index < started; ++index) {
            shards_[index]->request_stop();
        }
        for (std::size_t index = 0; index < started; ++index) {
            std::string shard_error;
            shards_[index]->wait(&shard_error);
        }
        if (error != nullptr) {
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
            std::to_string(shards_.empty() ? 0 : shards_[0]->bound_port()) +
            "}\n";
        std::size_t offset = 0;
        while (offset < line.size()) {
#if defined(_WIN32)
            // MSVC write() takes an unsigned int count; the READY record
            // is a single small JSON line, well below UINT_MAX.
            const ssize_t written = ::write(
                options_.worker_options.ready_fd, line.data() + offset,
                static_cast<unsigned int>(line.size() - offset));
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
};

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

bool StaticPoolServer::wait(std::string* error) { return impl_->wait(error); }

std::size_t StaticPoolServer::active_workers() const {
    return impl_->active_workers();
}

int StaticPoolServer::run(const std::vector<std::uint8_t>& bundle) {
    std::string error;
    if (!impl_->start(bundle, &error)) {
        // M2 item 7 (§12.2): a failed pool start is a structured startup
        // line; the error text is static and sanitized.
        emit_log(impl_->structured_log(),
                 {.event = log_events::kStartup,
                  .result = "fail",
                  .message = error});
        return 1;
    }
    // Benchmark-only signal handling: SIGTERM/SIGINT are blocked and
    // waited for with sigwait on the calling thread, so no C++ object is
    // ever touched from a signal handler (shards install no signal wiring
    // of their own). The bounded stop/wait runs after the signal. Windows
    // has no sigwait: a console control handler (CTRL_C/CTRL_BREAK/close)
    // signals the same stop gate instead.
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
    const DWORD wait_result =
        WaitForSingleObject(g_static_pool_term_event, INFINITE);
    (void)wait_result;
#else
    sigset_t term_set;
    sigemptyset(&term_set);
    sigaddset(&term_set, SIGTERM);
    sigaddset(&term_set, SIGINT);
    if (sigprocmask(SIG_BLOCK, &term_set, nullptr) != 0) {
        emit_log(impl_->structured_log(),
                 {.event = log_events::kStartup,
                  .result = "fail",
                  .message = "cannot block signals"});
        impl_->request_stop();
        impl_->wait(&error);
        return 1;
    }
    int signal_number = 0;
    if (sigwait(&term_set, &signal_number) != 0) {
        emit_log(impl_->structured_log(),
                 {.event = log_events::kShutdown,
                  .result = "fail",
                  .message = "cannot wait for signals"});
        impl_->request_stop();
        impl_->wait(&error);
        return 1;
    }
#endif
    impl_->request_stop();
    impl_->wait(&error);
    return 0;
}

}  // namespace capsid::host
