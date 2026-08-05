// M2 fixed-pool Host data plane. See static_pool_server.h.

#include "host/static_pool_server.h"

#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace capsid::host {

class StaticPoolServerImpl {
public:
    explicit StaticPoolServerImpl(StaticPoolServerOptions options)
        : options_(std::move(options)) {}

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
        if (options_.workers == 0) {
            if (error != nullptr) {
                *error = "static pool requires at least one worker";
            }
            return false;
        }
        shards_.reserve(options_.workers);
        // The first shard binds the port (kernel-assigned when 0); every
        // later shard reuses the same address:port through SO_REUSEPORT.
        std::uint16_t shared_port = 0;
        for (std::uint32_t index = 0; index < options_.workers; ++index) {
            SingleWorkerServerOptions shard_options = options_.worker_options;
            shard_options.ready_fd = -1;           // pool owns the READY record
            shard_options.write_ready_record = false;  // pool-level READY
            shard_options.install_process_signals = false;  // pool owns signals
            shard_options.so_reuseport = true;     // one shared port
            shard_options.defer_accept = true;     // pool-wide activation barrier
            if (index > 0) {
                shard_options.listen_port = shared_port;
            }
            std::unique_ptr<SingleWorkerServer> shard(
                new SingleWorkerServer(std::move(shard_options)));
            std::string shard_error;
            if (!shard->start(bundle, &shard_error)) {
                rollback(shards_.size(), error);
                return false;
            }
            if (index == 0) {
                shared_port = shard->bound_port();
            }
            shards_.push_back(std::move(shard));
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
        for (const std::unique_ptr<SingleWorkerServer>& shard : shards_) {
            shard->request_stop();
        }
    }

    bool wait(std::string* error) {
        for (const std::unique_ptr<SingleWorkerServer>& shard : shards_) {
            std::string shard_error;
            shard->wait(&shard_error);
        }
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
        for (const std::unique_ptr<SingleWorkerServer>& shard : shards_) {
            if (shard->worker_available()) {
                ++count;
            }
        }
        return count;
    }

private:
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
        const ssize_t written = ::write(
            options_.worker_options.ready_fd, line.data(), line.size());
        if (written != static_cast<ssize_t>(line.size())) {
            return false;
        }
        return true;
    }

    StaticPoolServerOptions options_;
    std::atomic<bool> start_gate_ = false;
    std::atomic<bool> stop_requested_ = false;
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

bool StaticPoolServer::wait(std::string* error) { return impl_->wait(error); }

std::size_t StaticPoolServer::active_workers() const {
    return impl_->active_workers();
}

int StaticPoolServer::run(const std::vector<std::uint8_t>& bundle) {
    std::string error;
    if (!impl_->start(bundle, &error)) {
        std::fprintf(stderr, "capsid-host: %s\n", error.c_str());
        return 1;
    }
    // Benchmark-only signal handling: SIGTERM/SIGINT are blocked and
    // waited for with sigwait on the calling thread, so no C++ object is
    // ever touched from a signal handler (shards install no signal wiring
    // of their own). The bounded stop/wait runs after the signal.
    sigset_t term_set;
    sigemptyset(&term_set);
    sigaddset(&term_set, SIGTERM);
    sigaddset(&term_set, SIGINT);
    if (sigprocmask(SIG_BLOCK, &term_set, nullptr) != 0) {
        std::fprintf(stderr, "capsid-host: cannot block signals\n");
        impl_->request_stop();
        impl_->wait(&error);
        return 1;
    }
    int signal_number = 0;
    if (sigwait(&term_set, &signal_number) != 0) {
        std::fprintf(stderr, "capsid-host: cannot wait for signals\n");
        impl_->request_stop();
        impl_->wait(&error);
        return 1;
    }
    impl_->request_stop();
    impl_->wait(&error);
    return 0;
}

}  // namespace capsid::host
